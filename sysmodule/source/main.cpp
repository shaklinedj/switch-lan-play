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
#include "ldn_bridge.h"
#include "packet.h"
#include "sha1.h"
#include "lp_pool.h"

/* bsd.h needed for bsdInitialize() — same init pattern as ldn_mitm */
extern "C" {
#include <switch/services/bsd.h>
}

/* -------------------------------------------------------------------------
 * Logging implementation
 * ---------------------------------------------------------------------- */
static const char *level_names[] = {
    "", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"
};

struct known_relay_entry {
    const char *host;
    const char *ip;
};

static const known_relay_entry g_known_relays[] = {
    /* tekn0.net removed from builtin to test DNS resolution */
    { "lan.nonny.horse", "65.21.20.230" },
    { "switch.servegame.com", "89.163.151.130" },
    { "switch-lanyplay-de.ddns.net", "37.201.39.187" },
    { "switch.jayseateam.nl", "45.83.241.140" },
    { "switch.r3ps4j.nl", "141.144.207.91" },
    { "muitxobem-lanplay.ddns.net", "129.148.17.98" },
};

static const char *lookup_known_relay_ip(const char *host)
{
    for (size_t i = 0; i < sizeof(g_known_relays) / sizeof(g_known_relays[0]); i++) {
        if (strcmp(g_known_relays[i].host, host) == 0) {
            return g_known_relays[i].ip;
        }
    }
    return NULL;
}

static void cache_host_mapping(const char *host, const char *ip)
{
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/lan-play", 0777);

    char lines[64][256];
    int line_count = 0;
    bool found = false;

    FILE *in = fopen("sdmc:/config/lan-play/hosts.txt", "r");
    if (in) {
        while (line_count < 64 && fgets(lines[line_count], sizeof(lines[line_count]), in)) {
            char existing_ip[64] = {0};
            char existing_host[128] = {0};
            if (sscanf(lines[line_count], "%63s %127s", existing_ip, existing_host) == 2 &&
                strcmp(existing_host, host) == 0) {
                snprintf(lines[line_count], sizeof(lines[line_count]), "%s %s\n", ip, host);
                found = true;
            }
            line_count++;
        }
        fclose(in);
    }

    if (!found && line_count < 64) {
        snprintf(lines[line_count++], sizeof(lines[0]), "%s %s\n", ip, host);
    }

    FILE *out = fopen("sdmc:/config/lan-play/hosts.txt", "w");
    if (!out) return;
    for (int i = 0; i < line_count; i++) {
        fputs(lines[i], out);
    }
    fclose(out);
    fsdevCommitDevice("sdmc");
}

static int resolve_from_hosts_file(const char *host, struct in_addr *out_addr)
{
    FILE *f = fopen("sdmc:/config/lan-play/hosts.txt", "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char ip[64] = {0};
        char name[128] = {0};

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (sscanf(line, "%63s %127s", ip, name) != 2) continue;
        if (strcmp(name, host) != 0) continue;

        if (inet_pton(AF_INET, ip, out_addr) == 1) {
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

static int resolve_from_builtin_fallback(const char *host, struct in_addr *out_addr)
{
    const char *ip = lookup_known_relay_ip(host);
    if (!ip) return -1;
    return inet_pton(AF_INET, ip, out_addr) == 1 ? 0 : -1;
}

void nx_log(int level, const char *fmt, ...)
{
    if (level > LLOG_DEBUG) return;
    
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = snprintf(buf, sizeof(buf), "[LanPlay][%s] ", level_names[level]);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);

    /* Atmosphere Debug Stream */
    svcOutputDebugString(buf, strlen(buf));
    
    /* Persistent File Log - ROOT visibility */
    const char *log_path = "sdmc:/lan-play.log";
    struct stat st;
    if (stat(log_path, &st) == 0 && st.st_size > 102400) {
        /* Rotate log: delete old and rename current */
        remove("sdmc:/lan-play.old.log");
        rename(log_path, "sdmc:/lan-play.old.log");
    }

    FILE *f = fopen(log_path, "a");
    if (f) {
        fprintf(f, "%s\n", buf);
        fclose(f);
        fsdevCommitDevice("sdmc"); /* Force physical write */
    }
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

    /* 2. hosts.txt fallback — instant, no network needed */
    if (resolve_from_hosts_file(host, &out->sin_addr) == 0) {
        char ip_str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
        LLOG(LLOG_WARNING, "main: hosts.txt fallback for '%s' -> %s:%d", host, ip_str, port);
        return 0;
    }

    /* 3. Built-in table fallback — instant, no network needed */
    if (resolve_from_builtin_fallback(host, &out->sin_addr) == 0) {
        char ip_str[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
        LLOG(LLOG_WARNING, "main: builtin fallback for '%s' -> %s:%d", host, ip_str, port);
        return 0;
    }

    /* 4. DNS — last resort, 3 retries only (avoids 10s wait on 90DNS) */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    for (int retry = 0; retry < 3; retry++) {
        int gai_err = getaddrinfo(host, NULL, &hints, &res);
        if (gai_err == 0 && res) break;
        LLOG(LLOG_WARNING, "main: DNS failed for '%s' (try %d/3): %s", host, retry + 1, gai_strerror(gai_err));
        svcSleepThread(1000000000ULL);
    }

    if (!res) {
        LLOG(LLOG_ERROR, "main: cannot resolve '%s' — use IP:port in config", host);
        return -1;
    }

    memcpy(&out->sin_addr, &((struct sockaddr_in*)res->ai_addr)->sin_addr, sizeof(struct in_addr));
    freeaddrinfo(res);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &out->sin_addr, ip_str, sizeof(ip_str));
    cache_host_mapping(host, ip_str);
    LLOG(LLOG_INFO, "main: resolved '%s' -> %s:%d (DNS)", host, ip_str, port);
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
static uint8_t s_tap_stack[STACK_SIZE]       __attribute__((aligned(0x1000)));
static uint8_t s_relay_stack[STACK_SIZE]     __attribute__((aligned(0x1000)));
static uint8_t s_keepalive_stack[STACK_SIZE] __attribute__((aligned(0x1000)));
static uint8_t s_ldn_udp_stack[STACK_SIZE]   __attribute__((aligned(0x1000)));
static uint8_t s_ldn_tcp_stack[STACK_SIZE]   __attribute__((aligned(0x1000)));

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

/* Socket configuration — sysmodules can't use the default large buffers.
 * Pattern matches ldn_mitm: bsdInitialize() first, then socketInitialize(). */
static const SocketInitConfig g_socket_config = {
    .tcp_tx_buf_size     = 0x800,
    .tcp_rx_buf_size     = 0x800,
    .tcp_tx_buf_max_size = 0x2000,
    .tcp_rx_buf_max_size = 0x2000,
    .udp_tx_buf_size     = 0x2000,  /* 8 KB — game LAN packets are small (<2 KB) */
    .udp_rx_buf_size     = 0x2000,  /* 8 KB */
    .sb_efficiency       = 2,
    .num_bsd_sessions    = 8,   /* tap(2) + relay(1) + ldn_bridge(3) + spare(2) */
    .bsd_service_type    = BsdServiceType_System, /* bsd:s needed for SOCK_RAW (EPERM with bsd:u) */
};

/* Transfer Memory for BSD sockets.
 * Per UDP socket: sb_efficiency × (udp_tx + udp_rx) = 2×(8+8) KB = 32 KB.
 * 8 sessions × 32 KB = 256 KB → 0x40000 with comfortable margin. */
#define SOCKET_TMEM_SIZE 0x40000
static uint8_t g_socket_tmem_buffer[SOCKET_TMEM_SIZE] alignas(0x1000);

static const BsdInitConfig g_bsd_config = {
    .version             = 1,
    .tmem_buffer         = g_socket_tmem_buffer,
    .tmem_buffer_size    = sizeof(g_socket_tmem_buffer),
    .tcp_tx_buf_size     = g_socket_config.tcp_tx_buf_size,
    .tcp_rx_buf_size     = g_socket_config.tcp_rx_buf_size,
    .tcp_tx_buf_max_size = g_socket_config.tcp_tx_buf_max_size,
    .tcp_rx_buf_max_size = g_socket_config.tcp_rx_buf_max_size,
    .udp_tx_buf_size     = g_socket_config.udp_tx_buf_size,
    .udp_rx_buf_size     = g_socket_config.udp_rx_buf_size,
    .sb_efficiency       = g_socket_config.sb_efficiency,
};

/* -------------------------------------------------------------------------
 * Init Error Tracking
 * ---------------------------------------------------------------------- */
static Result g_rc_fs     = 0;
static Result g_rc_setsys = 0;
static Result g_rc_bsd    = 0;
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
        Result mount_rc = fsdevMountSdmc();
        if (R_FAILED(mount_rc)) {
            g_rc_fs = mount_rc;
        } else {
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/lan-play", 0777);
            mkdir("sdmc:/tmp", 0777);
        }
        /* ULTRA EARLY LOG for debugging Atmosphere 1.1.0 boots */
        LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 EARLY BOOT ===");
    }

    g_rc_setsys = setsysInitialize();

    /* Follow ldn_mitm init order: NIFM, BSD, then socket wrapper. */
    g_rc_nifm = nifmInitialize(NifmServiceType_Admin);
    if (R_FAILED(g_rc_nifm)) {
        g_rc_nifm = nifmInitialize(NifmServiceType_User);
    }

    /* BSD + socket init (same pattern as ldn_mitm) */
    g_rc_bsd = bsdInitialize(&g_bsd_config, g_socket_config.num_bsd_sessions, g_socket_config.bsd_service_type);
    if (R_SUCCEEDED(g_rc_bsd)) {
        g_rc_socket = socketInitialize(&g_socket_config);
    } else {
        g_rc_socket = g_rc_bsd;
    }

    /* Close the service manager session now that lookups are done */
    smExit();
}

extern "C" void __appExit(void)
{
    fsdevUnmountAll();
    nifmExit();
    socketExit();
    bsdExit();
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

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting ===");

    /* Initialise static memory pool (must happen before any threads) */
    lp_pool_init();

    /* ------------------------------------------------------------------ */
    /* 0. Wait for Network to be fully established by Horizon (Boot time) */
    /* ------------------------------------------------------------------ */
    LLOG(LLOG_INFO, "Waiting for active Internet Connection...");
    u32 g_local_wifi_ip = 0; /* captured here, stored in lp->wifi_ip later */
    int wait_seconds = 0;
    while (true) {
        u32 out_ip = 0;
        if (R_SUCCEEDED(g_rc_nifm)) {
            nifmGetCurrentIpAddress(&out_ip);
            if (out_ip != 0) {
                g_local_wifi_ip = out_ip;
                LLOG(LLOG_INFO, "Network is UP and stabilized!");
                break;
            }
        }

        /* Update status so HBAPP knows we are alive but waiting */
        write_status_error("Esperando WiFi...");

        /* Only log every 30 seconds after the first minute to save SD wear */
        if (wait_seconds < 60 || (wait_seconds % 30 == 0)) {
            LLOG(LLOG_DEBUG, "Still waiting for WiFi (T+%ds)...", wait_seconds);
        }

        svcSleepThread(2000000000LL); /* 2 seconds */
        wait_seconds += 2;
    }

    /* Give Horizon extra time to set up default gateway & routing table.
     * nifmGetCurrentIpAddress can return an IP before the route is ready,
     * causing "Host is unreachable" on the first sendto().  A short
     * stabilization sleep avoids this race condition.                      */
    LLOG(LLOG_INFO, "Waiting 5s for routing table to stabilize...");
    svcSleepThread(5000000000LL); /* 5 seconds */

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
    lp->bpf_fd   = -1;
    lp->relay_fd = -1;
    lp->running  = true;
    lp->pmtu     = 0; /* no fragmentation by default */
    lp->wifi_ip  = g_local_wifi_ip; /* local WiFi IP for self-echo filtering in tap_recv */

    mutexInit(&lp->mutex);

    /* ------------------------------------------------------------------ */
    /* 3. Resolve relay server address                                      */
    /* ------------------------------------------------------------------ */
    while (resolve_server(cfg.relay_addr, &lp->server_addr) != 0) {
        LLOG(LLOG_WARNING, "Relay DNS not ready for '%s' — retrying...", cfg.relay_addr);
        write_status_error("Esperando DNS del relay...");

        struct stat rst;
        if (stat("sdmc:/tmp/lanplay.reload", &rst) == 0) {
            unlink("sdmc:/tmp/lanplay.reload");
            lan_play_free(lp);
            return 1;
        }

        svcSleepThread(2000000000LL);
    }

    /* ------------------------------------------------------------------ */
    /* 3b. Route probe — verify we can actually reach the relay IP          */
    /* ------------------------------------------------------------------ */
    {
        struct sockaddr_in probe_dst = lp->server_addr;
        int probe_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe_fd >= 0) {
            int probe_ok = 0;
            for (int attempt = 0; attempt < 15; attempt++) {
                uint8_t dummy = 0xFF;
                ssize_t r = sendto(probe_fd, &dummy, 1, 0,
                                   (struct sockaddr *)&probe_dst,
                                   sizeof(probe_dst));
                if (r >= 0) {
                    probe_ok = 1;
                    LLOG(LLOG_INFO, "Route probe OK (attempt %d)", attempt + 1);
                    break;
                }
                LLOG(LLOG_DEBUG, "Route probe attempt %d: %s", attempt + 1, strerror(errno));
                svcSleepThread(2000000000LL); /* 2 s */
            }
            close(probe_fd);
            if (!probe_ok) {
                LLOG(LLOG_WARNING, "Route probe never succeeded — proceeding anyway");
            }
        }
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
    /* 8b. LDN Bridge init (captures ldn_mitm traffic for relay)           */
    /* ------------------------------------------------------------------ */
    if (ldn_bridge_init(lp) != 0) {
        LLOG(LLOG_WARNING, "LDN bridge init failed — LDN games won't work via relay");
        /* Non-fatal: native LAN Play games still work via TAP */
    }

    /* ------------------------------------------------------------------ */
    /* 9. Start background threads                                          */
    /* ------------------------------------------------------------------ */
    Result rc;
    bool tap_started       = false;
    bool relay_started     = false;
    bool keepalive_started = false;
    bool ldn_udp_started   = false;
    bool ldn_tcp_started   = false;
    Thread keepalive_thread;
    memset(&keepalive_thread, 0, sizeof(keepalive_thread));

    s32 base_prio = 31; /* Safe static priority for sysmodule */

    rc = threadCreate(&lp->tap_thread, tap_recv_thread_fn, lp,
                      s_tap_stack, sizeof(s_tap_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate tap failed: 0x%x (prio %d)", rc, base_prio);
        write_status_error("Failed to create tap thread");
        goto cleanup;
    }

    rc = threadCreate(&lp->relay_thread, lan_client_recv_thread_fn, lp,
                      s_relay_stack, sizeof(s_relay_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate relay failed: 0x%x (prio %d)", rc, base_prio);
        write_status_error("Failed to create relay thread");
        threadClose(&lp->tap_thread);
        goto cleanup;
    }

    rc = threadCreate(&keepalive_thread, lan_client_keepalive_thread_fn, lp,
                      s_keepalive_stack, sizeof(s_keepalive_stack), base_prio, -2);
    if (R_FAILED(rc)) {
        LLOG(LLOG_ERROR, "threadCreate keepalive failed: 0x%x", rc);
        write_status_error("Failed to create keepalive thread");
        threadClose(&lp->tap_thread);
        threadClose(&lp->relay_thread);
        goto cleanup;
    }

    threadStart(&lp->tap_thread);      tap_started      = true;
    threadStart(&lp->relay_thread);    relay_started    = true;
    threadStart(&keepalive_thread);    keepalive_started = true;

    /* LDN bridge threads (optional — may fail if bridge init failed) */
    rc = threadCreate(&lp->ldn_udp_thread, ldn_bridge_udp_thread_fn, lp,
                      s_ldn_udp_stack, sizeof(s_ldn_udp_stack), base_prio, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&lp->ldn_udp_thread);
        ldn_udp_started = true;
    } else {
        LLOG(LLOG_WARNING, "ldn_bridge UDP thread create failed: 0x%x", rc);
    }

    rc = threadCreate(&lp->ldn_tcp_thread, ldn_bridge_tcp_thread_fn, lp,
                      s_ldn_tcp_stack, sizeof(s_ldn_tcp_stack), base_prio, -2);
    if (R_SUCCEEDED(rc)) {
        threadStart(&lp->ldn_tcp_thread);
        ldn_tcp_started = true;
    } else {
        LLOG(LLOG_WARNING, "ldn_bridge TCP thread create failed: 0x%x", rc);
    }

    /* Write everything in a single LLOG block to prevent FatFS concurrent fopen locking drops */
    LLOG(LLOG_INFO, "ALL threads started. LAN relay is ACTIVE. My IP: %s | Relay: %s", cfg.my_ip, cfg.relay_addr);

    /* ------------------------------------------------------------------ */
    /* 10. Service Loop (Restartable without reboot)                       */
    /* ------------------------------------------------------------------ */
    while (true) {
        /* Write status for the Homebrew App to read */
        FILE *sf = fopen("sdmc:/tmp/lanplay.status", "w");
        if (sf) {
            fprintf(sf, "active=1\n");
            fprintf(sf, "error=\n");
            fprintf(sf, "relay=%s\n", cfg.relay_addr);
            fprintf(sf, "up_pkt=%llu\n", (unsigned long long)lp->upload_packet);
            fprintf(sf, "up_bytes=%llu\n", (unsigned long long)lp->upload_byte);
            fprintf(sf, "dn_pkt=%llu\n", (unsigned long long)lp->download_packet);
            fprintf(sf, "dn_bytes=%llu\n", (unsigned long long)lp->download_byte);
            fclose(sf);
        }

        /* Check for reload trigger from the Homebrew App */
        struct stat st;
        if (stat("sdmc:/tmp/lanplay.reload", &st) == 0) {
            LLOG(LLOG_INFO, "Reload trigger detected — restarting service...");
            unlink("sdmc:/tmp/lanplay.reload");
            
            /* Give FS time to commit the config.ini changes from hbapp */
            svcSleepThread(1000000000LL);
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

    if (ldn_tcp_started) {
        threadWaitForExit(&lp->ldn_tcp_thread);
        threadClose(&lp->ldn_tcp_thread);
    }
    if (ldn_udp_started) {
        threadWaitForExit(&lp->ldn_udp_thread);
        threadClose(&lp->ldn_udp_thread);
    }
    if (keepalive_started) {
        threadWaitForExit(&keepalive_thread);
        threadClose(&keepalive_thread);
    }
    /* Close relay_fd BEFORE waiting for the relay recv thread.
     * This unblocks any recvfrom() immediately, allowing the thread
     * to observe lp->running == false and exit cleanly. */
    lan_client_close(lp);
    if (relay_started) {
        threadWaitForExit(&lp->relay_thread);
        threadClose(&lp->relay_thread);
    }
    /* Close tap fd BEFORE waiting for the tap recv thread for the same reason. */
    tap_close(lp);
    if (tap_started) {
        threadWaitForExit(&lp->tap_thread);
        threadClose(&lp->tap_thread);
    }

    ldn_bridge_close(lp);
    lan_play_free(lp);

    /* Small delay before loop restart */
    svcSleepThread(500000000LL);
    return 1; /* returning 1 here would exit main, we need the loop outside */
}

/* Updated main with the retry/reload loop */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* RECOVERY: Ensure directories exist as early as possible for logging */
    /* TRUNCATE LOG: root visibility */
    if (R_SUCCEEDED(g_rc_fs)) {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/lan-play", 0777);
        FILE *f_init = fopen("sdmc:/lan-play.log", "w");
        if (f_init) {
            fprintf(f_init, "=== switch-lan-play sysmodule v1.14 (CLEAN START) ===\n");
            fclose(f_init);
        }
    }

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting ===");
    LLOG(LLOG_INFO, "Init Results (0x0 = Success):");
    LLOG(LLOG_INFO, "  FS:     0x%08X", g_rc_fs);
    LLOG(LLOG_INFO, "  SetSys: 0x%08X", g_rc_setsys);
    LLOG(LLOG_INFO, "  BSD:    0x%08X", g_rc_bsd);
    LLOG(LLOG_INFO, "  Socket: 0x%08X", g_rc_socket);
    LLOG(LLOG_INFO, "  NIFM:   0x%08X", g_rc_nifm);

    if (R_FAILED(g_rc_socket) || R_FAILED(g_rc_fs) || R_FAILED(g_rc_bsd)) {
        LLOG(LLOG_ERROR, "CRITICAL: A required service failed to initialize. Check your firmware/npdm.");
        write_status_error("Sysmodule libnx init failed!");
        svcSleepThread(5000000000LL);
        return 0;
    }

    int restart_count = 0;
    while (true) {
        if (run_service() == 0) break;
        restart_count++;
        /* Exponential backoff: 3s, 6s, 12s, ... capped at 60s */
        int delay = 3;
        for (int i = 1; i < restart_count && delay < 60; i++) delay *= 2;
        if (delay > 60) delay = 60;
        LLOG(LLOG_INFO, "Restarting service loop (attempt #%d, backoff %ds)...", restart_count, delay);
        svcSleepThread((s64)delay * 1000000000LL);
    }

    return 0;
}
