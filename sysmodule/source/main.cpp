/**
 * main.cpp — entry point for the switch-lan-play Atmosphere sysmodule.
 *
 * Boot sequence
 * =============
 * 1. Initialise libnx services (sockets, filesystem, set:sys)
 * 2. Read /config/lan-play/config.ini from the SD card
 * 3. Resolve the relay server address via DNS
 * 4. Initialise the packet_ctx (virtual LAN interface state)
 * 5. Initialise the raw-socket TAP layer
 * 6. Initialise the relay UDP client
 * 7. Launch three background threads:
 *      - tap_recv_thread      (inbound packets from WiFi/LAN → game)
 *      - relay_recv_thread    (inbound packets from relay server → game)
 *      - keepalive_thread     (periodic UDP keepalive → relay server)
 * 8. Sleep forever (sysmodule stays resident until console shuts down)
 *
 * Memory / resource ownership
 * ============================
 * - lp           : heap-allocated, freed in every error path and cleanup.
 * - lp->username : strdup(), freed before freeing lp.
 * - Thread stacks: statically allocated (no dynamic lifetime concern).
 * - Sockets      : closed by tap_close() / lan_client_close().
 */

#include "nx_common.h"
#include "config.h"
#include "tap_iface.h"
#include "lan_client_nx.h"
#include "packet.h"
#include "sha1.h"

/* -------------------------------------------------------------------------
 * Logging implementation
 * ---------------------------------------------------------------------- */
static const char *level_names[] = {
    "", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
};

void nx_log(int level, const char *fmt, ...)
{
    if (level > LLOG_DEBUG) return;
    
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = snprintf(buf, sizeof(buf), "[LanPlay][%s] ", level_names[level]);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);

    /* Push natively to Atmosphere's debug stream (dmnt_cht) */
    svcOutputDebugString(buf, strlen(buf));
    
    FILE *f = fopen("sdmc:/lan-play.log", "a");
    if (!f) return;
    fprintf(f, "%s\n", buf);
    fclose(f);
}

/* -------------------------------------------------------------------------
 * DNS resolution helper
 * ---------------------------------------------------------------------- */
static int resolve_server(const char *addr_str, struct sockaddr_in *out)
{
    char host[256];
    strncpy(host, addr_str, sizeof(host)-1);
    host[sizeof(host)-1] = '\0';

    char *colon = strchr(host, ':');
    int port = 11451;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);

    /* 1. Try parsing as a direct IP address first to avoid DNS issues/timeouts */
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        LLOG(LLOG_INFO, "main: relay parsed as direct IP: %s:%d", host, port);
        return 0;
    }

    /* 2. Fall back to DNS resolution if it's a domain name */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int gai_err = getaddrinfo(host, NULL, &hints, &res);
    if (gai_err != 0 || !res) {
        LLOG(LLOG_ERROR, "main: cannot resolve '%s': %s", host,
             gai_err ? gai_strerror(gai_err) : "empty result");
        if (res) freeaddrinfo(res); /* belt-and-suspenders */
        return -1;
    }

    memcpy(&out->sin_addr, &((struct sockaddr_in*)res->ai_addr)->sin_addr, sizeof(struct in_addr));
    freeaddrinfo(res);  /* always freed, even on success */

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
    LLOG(LLOG_INFO, "main: relay server resolved to %s:%d", ip_str, port);
    return 0;
}

/* -------------------------------------------------------------------------
 * SHA1 of password → stored in lp->key (for auth)
 * ---------------------------------------------------------------------- */
static void hash_password(struct lan_play *lp, const char *password)
{
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char *)password, (uint32_t)strlen(password));
    SHA1Final(lp->key, &ctx);
    /* Scrub the temporary context so the password doesn't linger in memory */
    memset(&ctx, 0, sizeof(ctx));
}

/* -------------------------------------------------------------------------
 * Free all heap resources owned by a lan_play context.
 * Sockets are expected to have been closed before this is called
 * (by tap_close / lan_client_close).
 * ---------------------------------------------------------------------- */
static void lan_play_free(struct lan_play *lp)
{
    if (!lp) return;
    if (lp->username) {
        free(lp->username);
        lp->username = NULL;
    }
    free(lp);
}

/* -------------------------------------------------------------------------
 * Build the send buffer for packet_ctx
 * ---------------------------------------------------------------------- */
static uint8_t g_pkt_buffer[BUFFER_SIZE];

/* -------------------------------------------------------------------------
 * Thread stacks (statically allocated — sysmodule has no heap growth)
 * ---------------------------------------------------------------------- */
#define STACK_SIZE 0x8000 /* 32 KB per thread */
static uint8_t s_tap_stack[STACK_SIZE]          __attribute__((aligned(0x1000)));
static uint8_t s_relay_stack[STACK_SIZE]        __attribute__((aligned(0x1000)));
static uint8_t s_keepalive_stack[STACK_SIZE]    __attribute__((aligned(0x1000)));
static uint8_t s_ldn_bridge_stack[STACK_SIZE]   __attribute__((aligned(0x1000)));

/* =========================================================================
 * CRITICAL: libnx sysmodule boilerplate.
 * Without these, libnx treats us as a normal application and tries to
 * connect to applet services that don't exist → immediate fatal crash.
 * ====================================================================== */
extern "C" {
    /* Tell libnx this is a sysmodule, not a regular app */
    u32 __nx_applet_type = AppletType_None;

    /* Don't try to use the applet exit mechanism */
    u32 __nx_applet_exit_mode = 0;

    /* Custom heap for the sysmodule (2 MB for safety) */
    #define INNER_HEAP_SIZE 0x200000
    static char g_inner_heap[INNER_HEAP_SIZE];

    void __libnx_initheap(void) {
        extern char *fake_heap_start;
        extern char *fake_heap_end;
        fake_heap_start = g_inner_heap;
        fake_heap_end   = g_inner_heap + INNER_HEAP_SIZE;
    }
}

/* Socket configuration — sysmodules can't use the default large buffers */
static const SocketInitConfig g_socket_config = {
    .tcp_tx_buf_size     = 0x1000,
    .tcp_rx_buf_size     = 0x1000,
    .tcp_tx_buf_max_size = 0x4000,
    .tcp_rx_buf_max_size = 0x4000,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 2,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_User, /* MUST BE USER. bsd:s ignores game local broadcasts */
};

/* -------------------------------------------------------------------------
 * Init Error Tracking
 * ---------------------------------------------------------------------- */
static Result g_rc_fs     = 0;
static Result g_rc_setsys = 0;
static Result g_rc_socket = 0;
static Result g_rc_nifm   = 0;

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
extern "C" void __appInit(void)
{
    /* Open the service manager session FIRST! */
    Result sm_rc = smInitialize();
    if (R_FAILED(sm_rc)) {
        return; /* If SM fails, everything else will too */
    }

    g_rc_fs = fsInitialize();
    if (R_SUCCEEDED(g_rc_fs)) {
        fsdevMountSdmc();
    }

    g_rc_setsys = setsysInitialize();
    /* BSD sockets — use constrained buffer sizes for sysmodule context */
    g_rc_socket = socketInitialize(&g_socket_config);
    /* DNS resolver */
    g_rc_nifm = nifmInitialize(NifmServiceType_User);

    /* Close the service manager session now that lookups are done */
    smExit();
}

extern "C" void __appExit(void)
{
    fsdevUnmountAll();
    nifmExit();
    socketExit();
    setsysExit();
    fsExit();
}

static void ensure_tmp_dir(void) {
    if (R_SUCCEEDED(g_rc_fs)) {
        mkdir("sdmc:/tmp", 0777);
    }
}

static void write_status_error(const char* error_msg) {
    if (R_FAILED(g_rc_fs)) return;
    ensure_tmp_dir();
    FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
    if (sf) {
        fprintf(sf, "active=0\n");
        fprintf(sf, "error=%s\n", error_msg);
        fclose(sf);
    }
}

static int run_service(void)
{
    if (R_SUCCEEDED(g_rc_fs)) ensure_tmp_dir();

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule starting ===");

    /* ------------------------------------------------------------------ */
    /* 0. Wait for Network to be fully established by Horizon (Boot time) */
    /* ------------------------------------------------------------------ */
    LLOG(LLOG_INFO, "Waiting for active Internet Connection...");
    while (true) {
        u32 out_ip = 0;
        if (R_SUCCEEDED(g_rc_nifm)) {
            nifmGetCurrentIpAddress(&out_ip);
            if (out_ip != 0) {
                LLOG(LLOG_INFO, "Network is UP and stabilized!");
                break;
            }
        }
        /* If NIFM isn't up, just keep checking every 2 seconds */
        svcSleepThread(2000000000LL);
    }

    /* ------------------------------------------------------------------ */
    /* 1. Read config                                                       */
    /* ------------------------------------------------------------------ */
    nx_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    if (nx_config_load(&cfg) != 0) {
        LLOG(LLOG_WARNING, "Config missing. Waiting for app to configure.");
        
        /* Loop until a reload is triggered */
        while (true) {
            FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
            if (sf) { fprintf(sf, "active=0\n"); fclose(sf); }

            struct stat st;
            if (stat("sdmc:/tmp/lanplay.reload", &st) == 0) {
                unlink("sdmc:/tmp/lanplay.reload");
                return 1; /* Trigger reload loop */
            }
            svcSleepThread(2000000000LL);
        }
    }

    /* Auto-assign a unique IP from the device serial if not set in config */
    nx_config_auto_ip(&cfg);

    /* ------------------------------------------------------------------ */
    /* 2. Allocate & zero the main context                                  */
    /* ------------------------------------------------------------------ */
    struct lan_play *lp = (struct lan_play *)malloc(sizeof(*lp));
    if (!lp) {
        LLOG(LLOG_ERROR, "Out of memory allocating lan_play context");
        write_status_error("Out of memory");
        svcSleepThread(3000000000LL);
        return 1;
    }
    memset(lp, 0, sizeof(*lp));
    lp->raw_fd   = -1;
    lp->relay_fd = -1;
    lp->ldn_fd   = -1;
    lp->running  = true;
    lp->pmtu     = 0; /* no fragmentation by default */

    mutexInit(&lp->mutex);

    /* ------------------------------------------------------------------ */
    /* 3. Resolve relay server address                                      */
    /* ------------------------------------------------------------------ */
    if (resolve_server(cfg.relay_addr, &lp->server_addr) != 0) {
        LLOG(LLOG_ERROR, "Cannot resolve relay server '%s'", cfg.relay_addr);
        write_status_error("Cannot resolve relay server");
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Parse network config                                              */
    /* ------------------------------------------------------------------ */
    struct in_addr ip_addr, net_addr, mask_addr;
    if (!inet_aton(cfg.my_ip,       &ip_addr)  ||
        !inet_aton(cfg.subnet_net,  &net_addr)  ||
        !inet_aton(cfg.subnet_mask, &mask_addr)) {
        LLOG(LLOG_ERROR, "Invalid IP/subnet in config (ip=%s net=%s mask=%s)",
             cfg.my_ip, cfg.subnet_net, cfg.subnet_mask);
        write_status_error("Invalid IP/subnet config");
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        return 1;
    }
    memcpy(lp->my_ip, &ip_addr.s_addr, 4);

    /* ------------------------------------------------------------------ */
    /* 5. Initialise packet_ctx (virtual LAN interface state)               */
    /* ------------------------------------------------------------------ */
    packet_init(&lp->packet_ctx, lp,
                g_pkt_buffer, sizeof(g_pkt_buffer),
                &ip_addr.s_addr,
                &net_addr.s_addr,
                &mask_addr.s_addr,
                300 /* ARP TTL seconds */);

    /* ------------------------------------------------------------------ */
    /* 6. Auth — keep username in lp->username (freed by lan_play_free)    */
    /* ------------------------------------------------------------------ */
    if (cfg.username[0] != '\0') {
        lp->username = strdup(cfg.username);
        if (!lp->username) {
            LLOG(LLOG_ERROR, "Out of memory for username");
            write_status_error("Out of memory for username");
            lan_play_free(lp);
            svcSleepThread(3000000000LL);
            return 1;
        }
        hash_password(lp, cfg.password);
        /* Scrub password string from config struct */
        memset(cfg.password, 0, sizeof(cfg.password));
        LLOG(LLOG_INFO, "Auth enabled for user '%s'", lp->username);
    }

    /* ------------------------------------------------------------------ */
    /* 7. TAP init                                                          */
    /* ------------------------------------------------------------------ */
    if (tap_init(lp) != 0) {
        LLOG(LLOG_ERROR, "TAP init failed — aborting");
        write_status_error("TAP interface init failed");
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        return 1;
    }
    /* Set packet_ctx MAC to our virtual MAC now that TAP is ready */
    packet_set_mac(&lp->packet_ctx, lp->my_mac);

    /* ------------------------------------------------------------------ */
    /* 8. Relay client init                                                 */
    /* ------------------------------------------------------------------ */
    if (lan_client_init(lp) != 0) {
        LLOG(LLOG_ERROR, "Relay client init failed — aborting");
        write_status_error("Relay client init failed");
        tap_close(lp);
        lan_play_free(lp);
        svcSleepThread(3000000000LL);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 9. Start background threads                                          */
    /* ------------------------------------------------------------------ */
    Result rc;
    bool tap_started       = false;
    bool relay_started     = false;
    bool keepalive_started = false;
    bool ldn_bridge_started = false;
    Thread keepalive_thread;
    memset(&keepalive_thread,      0, sizeof(keepalive_thread));
    memset(&lp->ldn_bridge_thread, 0, sizeof(lp->ldn_bridge_thread));

    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    rc = threadCreate(&lp->tap_thread, tap_recv_thread_fn, lp,
                      s_tap_stack, sizeof(s_tap_stack), prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate tap failed: 0x%x", rc);
        write_status_error("Failed to create tap thread");
        goto cleanup;
    }

    rc = threadCreate(&lp->relay_thread, lan_client_recv_thread_fn, lp,
                      s_relay_stack, sizeof(s_relay_stack), prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate relay failed: 0x%x", rc);
        write_status_error("Failed to create relay thread");
        threadClose(&lp->tap_thread);
        goto cleanup;
    }

    rc = threadCreate(&keepalive_thread, lan_client_keepalive_thread_fn, lp,
                      s_keepalive_stack, sizeof(s_keepalive_stack), prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate keepalive failed: 0x%x", rc);
        write_status_error("Failed to create keepalive thread");
        threadClose(&lp->tap_thread);
        threadClose(&lp->relay_thread);
        goto cleanup;
    }

    threadStart(&lp->tap_thread);      tap_started       = true;
    threadStart(&lp->relay_thread);    relay_started     = true;
    threadStart(&keepalive_thread);    keepalive_started = true;

    /* LDN bridge thread — optional, only when ldn_fd is available */
    if (lp->ldn_fd >= 0) {
        rc = threadCreate(&lp->ldn_bridge_thread, ldn_bridge_thread_fn, lp,
                          s_ldn_bridge_stack, sizeof(s_ldn_bridge_stack), prio, -2);
        if (R_FAILED(rc)) {
            LLOG(LLOG_WARNING, "threadCreate ldn_bridge failed: 0x%x "
                 "(ldn_mitm integration disabled)", rc);
        } else {
            threadStart(&lp->ldn_bridge_thread);
            ldn_bridge_started = true;
            LLOG(LLOG_INFO, "ldn_bridge: thread started");
        }
    }

    LLOG(LLOG_INFO, "ALL 3 Threads started. LAN relay is ACTIVE. My IP: %s | Relay: %s", cfg.my_ip, cfg.relay_addr);

    /* ------------------------------------------------------------------ */
    /* 10. Service Loop (Restartable without reboot)                       */
    /* ------------------------------------------------------------------ */
    while (true) {
        /* Write status for the Homebrew App to read */
        FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
        if (sf) {
            fprintf(sf, "active=1\n");
            fprintf(sf, "error=\n");
            fprintf(sf, "my_ip=%s\n",      cfg.my_ip);
            fprintf(sf, "relay=%s\n",      cfg.relay_addr);
            fprintf(sf, "up_pkt=%llu\n",   (unsigned long long)lp->upload_packet);
            fprintf(sf, "up_bytes=%llu\n", (unsigned long long)lp->upload_byte);
            fprintf(sf, "dn_pkt=%llu\n",   (unsigned long long)lp->download_packet);
            fprintf(sf, "dn_bytes=%llu\n", (unsigned long long)lp->download_byte);
            fclose(sf);
        }

        /* Check for reload trigger from the Homebrew App */
        struct stat st;
        if (stat("sdmc:/tmp/lanplay.reload", &st) == 0) {
            LLOG(LLOG_INFO, "Reload trigger detected — restarting service...");
            unlink("sdmc:/tmp/lanplay.reload");
            break; /* Exit inner loop to trigger reload */
        }

        svcSleepThread(2000000000LL); /* 2 s check */
    }

    /* ------------------------------------------------------------------ */
    /* 11. Cleanup (before reload or exit)                                 */
    /* ------------------------------------------------------------------ */
cleanup:
    LLOG(LLOG_INFO, "Shutting down threads for reload...");
    lp->running = false;

    if (ldn_bridge_started) {
        threadWaitForExit(&lp->ldn_bridge_thread);
        threadClose(&lp->ldn_bridge_thread);
    }
    if (keepalive_started) {
        threadWaitForExit(&keepalive_thread);
        threadClose(&keepalive_thread);
    }
    if (relay_started) {
        threadWaitForExit(&lp->relay_thread);
        threadClose(&lp->relay_thread);
    }
    if (tap_started) {
        threadWaitForExit(&lp->tap_thread);
        threadClose(&lp->tap_thread);
    }

    tap_close(lp);
    lan_client_close(lp);
    lan_play_free(lp);

    /* Small delay before loop restart */
    svcSleepThread(500000000LL);
    return 1; /* returning 1 here would exit main, we need the loop outside */
}

/* Updated main with the retry/reload loop */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (R_SUCCEEDED(g_rc_fs)) {
        unlink("sdmc:/lan-play.log"); /* Clean old log */
    }

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule starting ===");
    LLOG(LLOG_INFO, "Init Results:");
    LLOG(LLOG_INFO, "  FS:     0x%08X", g_rc_fs);
    LLOG(LLOG_INFO, "  SetSys: 0x%08X", g_rc_setsys);
    LLOG(LLOG_INFO, "  Socket: 0x%08X", g_rc_socket);
    LLOG(LLOG_INFO, "  NIFM:   0x%08X", g_rc_nifm);

    if (R_FAILED(g_rc_socket) || R_FAILED(g_rc_fs)) {
        LLOG(LLOG_ERROR, "CRITICAL: A required service failed to initialize. Aborting sysmodule.");
        write_status_error("Sysmodule libnx init failed (check log)");
        svcSleepThread(5000000000LL);
        return 0;
    }

    while (true) {
        if (run_service() == 0) break;
        LLOG(LLOG_INFO, "Restarting service loop...");
    }

    return 0;
}
