/**
 * lanplay_bridge.cpp — reads sdmc:/tmp/lanplay.status and forwards outbound
 * LDN discovery datagrams to the switch-lan-play sysmodule IPC socket.
 *
 * The status file is produced by the sysmodule every ~2 seconds:
 *   active=1
 *   my_ip=10.13.X.Y
 *
 * We cache the parsed result for ~1 second (ARM PMU ticks at 19.2 MHz).
 * A persistent UDP socket is lazily opened on the first IPC send and reused;
 * this avoids the cost of socket()/close() per packet.
 */
#include "lanplay_bridge.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#define STATUS_FILE   "sdmc:/tmp/lanplay.status"
#define CACHE_TICKS   19200000ULL   /* ~1 second at 19.2 MHz ARM PMU clock */
#define IPC_MSG_MAX   4096          /* max LDN packet size including prefix */

/* -------------------------------------------------------------------------- */
/* Status cache                                                                 */
/* -------------------------------------------------------------------------- */
struct BridgeState {
    bool     active;
    uint32_t virtual_ip;   /* host byte order */
    u64      last_tick;
};

static BridgeState g_state = { false, 0, 0 };

static void refresh_if_needed(void)
{
    u64 now = armGetSystemTick();
    if (now - g_state.last_tick < CACHE_TICKS) return;

    /* Reset and re-read */
    g_state.active     = false;
    g_state.virtual_ip = 0;
    g_state.last_tick  = now;

    FILE *f = fopen(STATUS_FILE, "r");
    if (!f) return;

    char line[128];
    bool saw_active = false;
    uint32_t vip    = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "active=1", 8) == 0) {
            saw_active = true;
        } else if (strncmp(line, "my_ip=", 6) == 0) {
            char ip_str[32] = {0};
            if (sscanf(line + 6, "%31s", ip_str) == 1) {
                struct in_addr a;
                if (inet_aton(ip_str, &a) == 1) {
                    vip = ntohl(a.s_addr);   /* convert to host byte order */
                }
            }
        }
    }
    fclose(f);

    if (saw_active && vip != 0) {
        g_state.active     = true;
        g_state.virtual_ip = vip;
    }
}

bool lanplay_is_active(void)
{
    refresh_if_needed();
    return g_state.active;
}

uint32_t lanplay_get_virtual_ip(void)
{
    refresh_if_needed();
    return g_state.active ? g_state.virtual_ip : 0;
}

uint32_t lanplay_get_broadcast(void)
{
    refresh_if_needed();
    if (!g_state.active) return 0;
    /* Virtual subnet is always 10.13.0.0/16; broadcast = 10.13.255.255 */
    return (10u << 24) | (13u << 16) | 0xFFFFu;
}

bool lanplay_get_ipc_sockaddr(struct sockaddr_in *out)
{
    refresh_if_needed();
    if (!g_state.active) return false;
    memset(out, 0, sizeof(*out));
    out->sin_family      = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 127.0.0.1 */
    out->sin_port        = htons(LDN_IPC_PORT);
    return true;
}

/* -------------------------------------------------------------------------- */
/* IPC send                                                                    */
/* -------------------------------------------------------------------------- */

/* Persistent send socket — opened once, reused for all IPC sends.
 * ldn_mitm's worker loop is single-threaded so no mutex needed. */
static int g_ipc_fd = -1;

static int get_ipc_fd(void)
{
    if (g_ipc_fd >= 0) return g_ipc_fd;
    g_ipc_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return g_ipc_fd;
}

bool lanplay_send_ipc(const void *buf, size_t len,
                      const struct sockaddr_in *dst_addr)
{
    struct sockaddr_in ipc;
    if (!lanplay_get_ipc_sockaddr(&ipc)) return false;
    if (!buf || len == 0 || len > IPC_MSG_MAX - 4) return false;
    if (!dst_addr) return false;

    int fd = get_ipc_fd();
    if (fd < 0) return false;

    /* Build [4-byte dst IPv4 (network byte order)][LDN packet] */
    uint8_t msg[4 + IPC_MSG_MAX];
    memcpy(msg,     &dst_addr->sin_addr.s_addr, 4);   /* dst IP as-is (net order) */
    memcpy(msg + 4, buf, len);

    ssize_t r = ::sendto(fd, msg, (ssize_t)(4 + len), 0,
                         (struct sockaddr *)&ipc, sizeof(ipc));
    return (r > 0);
}
