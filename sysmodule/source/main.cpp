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
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[LanPlay][%s] ", level_names[level]);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* -------------------------------------------------------------------------
 * DNS resolution helper
 * ---------------------------------------------------------------------- */
static int resolve_server(const char *addr_str, struct sockaddr_in *out)
{
    /* Split host:port */
    char host[256] = {0};
    uint16_t port  = SERVER_PORT;

    const char *colon = strrchr(addr_str, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - addr_str);
        if (hlen >= sizeof(host)) return -1;
        memcpy(host, addr_str, hlen);
        port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, addr_str, sizeof(host) - 1);
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        LLOG(LLOG_ERROR, "main: cannot resolve '%s'", host);
        return -1;
    }

    memcpy(out, res->ai_addr, sizeof(*out));
    out->sin_port = htons(port);
    freeaddrinfo(res);

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
}

/* -------------------------------------------------------------------------
 * Build the send buffer for packet_ctx
 * ---------------------------------------------------------------------- */
static uint8_t g_pkt_buffer[BUFFER_SIZE];

/* -------------------------------------------------------------------------
 * Thread stacks (statically allocated — sysmodule has no heap growth)
 * ---------------------------------------------------------------------- */
#define STACK_SIZE 0x8000 /* 32 KB per thread */
static uint8_t s_tap_stack[STACK_SIZE]      __attribute__((aligned(0x1000)));
static uint8_t s_relay_stack[STACK_SIZE]    __attribute__((aligned(0x1000)));
static uint8_t s_keepalive_stack[STACK_SIZE]__attribute__((aligned(0x1000)));

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
extern "C" void __appInit(void)
{
    /* These service initialisations happen before main() in a typical
     * libnx app.  For a sysmodule we call them explicitly. */
    Result rc;

    /* Filesystem (for SD card config) */
    rc = fsInitialize();
    if (R_FAILED(rc)) fatalThrow(rc);

    /* set:sys (for serial number / device UID) */
    rc = setsysInitialize();
    if (R_FAILED(rc)) setsysInitialize(); /* non-fatal: just retry once */

    /* BSD sockets */
    rc = socketInitializeDefault();
    if (R_FAILED(rc)) fatalThrow(rc);

    /* DNS resolver */
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) LLOG(LLOG_WARNING, "nifmInitialize failed: 0x%x", rc);
}

extern "C" void __appExit(void)
{
    nifmExit();
    socketExit();
    setsysExit();
    fsExit();
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule starting ===");

    /* ------------------------------------------------------------------ */
    /* 1. Read config                                                       */
    /* ------------------------------------------------------------------ */
    nx_config_t cfg;
    if (nx_config_load(&cfg) != 0) {
        LLOG(LLOG_WARNING,
             "Config missing or invalid.  Writing default config and exiting.");
        nx_config_write_default();
        LLOG(LLOG_INFO,
             "Edit sdmc:/config/lan-play/config.ini then reboot.");
        /* Keep the sysmodule alive so log messages are visible */
        svcSleepThread(30000000000LL); /* 30 s */
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Allocate & zero the main context                                  */
    /* ------------------------------------------------------------------ */
    struct lan_play *lp = (struct lan_play *)malloc(sizeof(*lp));
    if (!lp) {
        LLOG(LLOG_ERROR, "Out of memory");
        return 1;
    }
    memset(lp, 0, sizeof(*lp));
    lp->raw_fd    = -1;
    lp->relay_fd  = -1;
    lp->running   = true;
    lp->pmtu      = 0; /* no fragmentation by default */

    mutexInit(&lp->mutex);

    /* ------------------------------------------------------------------ */
    /* 3. Resolve relay server address                                      */
    /* ------------------------------------------------------------------ */
    if (resolve_server(cfg.relay_addr, &lp->server_addr) != 0) {
        LLOG(LLOG_ERROR, "Cannot resolve relay server '%s'", cfg.relay_addr);
        free(lp);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Parse network config                                              */
    /* ------------------------------------------------------------------ */
    /* Convert dotted-decimal strings to binary */
    struct in_addr ip_addr, net_addr, mask_addr;
    if (!inet_aton(cfg.my_ip,       &ip_addr)  ||
        !inet_aton(cfg.subnet_net,  &net_addr)  ||
        !inet_aton(cfg.subnet_mask, &mask_addr)) {
        LLOG(LLOG_ERROR, "Invalid IP/subnet in config");
        free(lp);
        return 1;
    }
    memcpy(lp->my_ip,              &ip_addr.s_addr, 4);

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
    /* 6. Auth                                                              */
    /* ------------------------------------------------------------------ */
    if (cfg.username[0] != '\0') {
        lp->username = strdup(cfg.username);
        hash_password(lp, cfg.password);
        LLOG(LLOG_INFO, "Auth enabled for user '%s'", lp->username);
    }

    /* ------------------------------------------------------------------ */
    /* 7. TAP init                                                          */
    /* ------------------------------------------------------------------ */
    if (tap_init(lp) != 0) {
        LLOG(LLOG_ERROR, "TAP init failed — aborting");
        free(lp);
        return 1;
    }
    /* Set packet_ctx MAC to our virtual MAC now that TAP is ready */
    packet_set_mac(&lp->packet_ctx, lp->my_mac);

    /* ------------------------------------------------------------------ */
    /* 8. Relay client init                                                 */
    /* ------------------------------------------------------------------ */
    if (lan_client_init(lp) != 0) {
        LLOG(LLOG_ERROR, "Relay client init failed — aborting");
        tap_close(lp);
        free(lp);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 9. Start background threads                                          */
    /* ------------------------------------------------------------------ */
    Result rc;

    rc = threadCreate(&lp->tap_thread, tap_recv_thread_fn, lp,
                      s_tap_stack, sizeof(s_tap_stack), 0x2C, 3);
    if (R_FAILED(rc)) { LLOG(LLOG_ERROR, "threadCreate tap failed: 0x%x", rc); goto cleanup; }

    rc = threadCreate(&lp->relay_thread, lan_client_recv_thread_fn, lp,
                      s_relay_stack, sizeof(s_relay_stack), 0x2C, 3);
    if (R_FAILED(rc)) { LLOG(LLOG_ERROR, "threadCreate relay failed: 0x%x", rc); goto cleanup; }

    Thread keepalive_thread;
    rc = threadCreate(&keepalive_thread, lan_client_keepalive_thread_fn, lp,
                      s_keepalive_stack, sizeof(s_keepalive_stack), 0x2C, 3);
    if (R_FAILED(rc)) { LLOG(LLOG_ERROR, "threadCreate keepalive failed: 0x%x", rc); goto cleanup; }

    threadStart(&lp->tap_thread);
    threadStart(&lp->relay_thread);
    threadStart(&keepalive_thread);

    LLOG(LLOG_INFO, "All threads started.  LAN play relay is active.");
    LLOG(LLOG_INFO, "My IP: %s  Relay: %s", cfg.my_ip, cfg.relay_addr);

    /* ------------------------------------------------------------------ */
    /* 10. Sleep forever (sysmodule stays resident)                         */
    /* ------------------------------------------------------------------ */
    while (true) {
        svcSleepThread(60000000000LL); /* 60 s heartbeat */
        LLOG(LLOG_DEBUG,
             "stats: up_pkt=%llu up_b=%llu dn_pkt=%llu dn_b=%llu",
             (unsigned long long)lp->upload_packet,
             (unsigned long long)lp->upload_byte,
             (unsigned long long)lp->download_packet,
             (unsigned long long)lp->download_byte);
    }

cleanup:
    lp->running = false;
    tap_close(lp);
    lan_client_close(lp);
    if (lp->username) free(lp->username);
    free(lp);
    return 1;
}
