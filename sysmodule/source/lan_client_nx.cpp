/**
 * lan_client_nx.cpp — relay UDP client for the Switch sysmodule.
 *
 * Replaces src/lan-client.c.  Uses nn::socket (BSD socket API via libnx)
 * instead of libuv.  Multithreading is handled with Horizon Threads
 * (threadCreate / threadStart) rather than a libuv event loop.
 *
 * The SLP relay protocol is identical to the PC client so this sysmodule
 * is 100% compatible with the existing switch-lan-play relay server.
 */
#include "lan_client_nx.h"
#include "packet.h"
#include "lp_pool.h"

/* SLP relay protocol type bytes (mirrors src/lan-client.c) */
#define LAN_CLIENT_TYPE_KEEPALIVE   0x00
#define LAN_CLIENT_TYPE_IPV4        0x01
#define LAN_CLIENT_TYPE_PING        0x02
#define LAN_CLIENT_TYPE_IPV4_FRAG   0x03
#define LAN_CLIENT_TYPE_AUTH_ME     0x04
#define LAN_CLIENT_TYPE_INFO        0x10

/* Fragment header layout (mirrors src/lan-client.c) */
#define LC_FRAG_SRC          0
#define LC_FRAG_DST          4
#define LC_FRAG_ID           8
#define LC_FRAG_PART         10
#define LC_FRAG_TOTAL_PART   11
#define LC_FRAG_LEN          12
#define LC_FRAG_PMTU         14
#define LC_FRAG_HEADER_LEN   16

static uint8_t BROADCAST_MAC[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

/* ---------------------------------------------------------------------- */
/*  Internal helpers                                                        */
/* ---------------------------------------------------------------------- */

/** Send raw bytes to the relay server via the UDP socket. */
static int relay_send_raw(struct lan_play *lp, const void *data, size_t len)
{
    ssize_t sent = sendto(lp->relay_fd, data, len, 0,
                          (struct sockaddr *)&lp->server_addr,
                          sizeof(lp->server_addr));
    if (sent < 0) {
        /* Rate-limit error logs: only once every 30 failures */
        static int err_count = 0;
        if (++err_count <= 3 || (err_count % 30 == 0)) {
            LLOG(LLOG_ERROR, "relay_send_raw: sendto failed (%d): %s",
                 err_count, strerror(errno));
        }
        return -1;
    }
    lp->upload_packet++;
    lp->upload_byte += (uint64_t)len;
    lp->last_game_activity = armGetSystemTick();
    return 0;
}

/** Send a type-prefixed relay packet, fragmenting if pmtu is set. */
static int lan_client_send(struct lan_play *lp, uint8_t type,
                           const uint8_t *packet, uint16_t len)
{
    int pmtu = lp->pmtu;
    if (type == LAN_CLIENT_TYPE_IPV4 && pmtu > 0 && len > (uint16_t)pmtu) {
        /* Fragment the packet */
        int total_part = len / pmtu + (len % pmtu ? 1 : 0);
        int id         = lp->frag_id++;

        uint8_t header[LC_FRAG_HEADER_LEN];
        CPY_IPV4(header + LC_FRAG_SRC, packet + IPV4_OFF_SRC);
        CPY_IPV4(header + LC_FRAG_DST, packet + IPV4_OFF_DST);
        WRITE_NET8(header, LC_FRAG_TOTAL_PART, total_part);
        WRITE_NET16(header, LC_FRAG_PMTU, pmtu);

        for (int i = 0, pos = 0; pos < len; i++, pos += pmtu) {
            int part_len = LMIN(pmtu, (int)len - pos);
            uint8_t frag_type = LAN_CLIENT_TYPE_IPV4_FRAG;
            WRITE_NET16(header, LC_FRAG_ID,   id);
            WRITE_NET8(header,  LC_FRAG_PART, i);
            WRITE_NET16(header, LC_FRAG_LEN,  part_len);

            /* Assemble: [type(1)] [header(16)] [data(part_len)] */
            size_t total = 1 + LC_FRAG_HEADER_LEN + part_len;
            uint8_t *buf = (uint8_t *)lp_pool_alloc(total);
            if (!buf) return -1;
            buf[0] = frag_type;
            memcpy(buf + 1,                        header, LC_FRAG_HEADER_LEN);
            memcpy(buf + 1 + LC_FRAG_HEADER_LEN,   packet + pos, part_len);
            int ret = relay_send_raw(lp, buf, total);
            lp_pool_free(buf);
            if (ret) return ret;
        }
        return 0;
    }

    /* Non-fragmented: [type(1)] [data(len)] */
    size_t total = 1 + len;
    uint8_t *buf = (uint8_t *)lp_pool_alloc(total);
    if (!buf) {
        LLOG(LLOG_ERROR, "lan_client_send: out of memory (%zu bytes)", total);
        return -1;
    }
    buf[0] = type;
    if (len > 0) memcpy(buf + 1, packet, len);
    int ret = relay_send_raw(lp, buf, total);
    lp_pool_free(buf);
    return ret;
}

int lan_client_send_ipv4(struct lan_play *lp, void *dst_ip,
                         const void *packet, uint16_t len)
{
    (void)dst_ip;
    return lan_client_send(lp, LAN_CLIENT_TYPE_IPV4, (const uint8_t *)packet, len);
}

/* ---------------------------------------------------------------------- */
/*  Broadcast / unicast delivery                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
    struct lan_play *lp;
    const uint8_t   *packet;
    uint16_t         len;
} for_each_ctx_t;

static int lan_client_arp_cb(void *p, const struct arp_item *item)
{
    for_each_ctx_t *ctx = (for_each_ctx_t *)p;
    struct payload part;
    part.ptr  = ctx->packet;
    part.len  = ctx->len;
    part.next = NULL;
    send_ether(&ctx->lp->packet_ctx, item->mac, ETHER_TYPE_IPV4, &part);
    return 0;
}

static int lan_client_on_broadcast(struct lan_play *lp,
                                   const uint8_t *packet, uint16_t len)
{
    if (lp->next_real_broadcast) {
        lp->next_real_broadcast = false;
        struct payload part;
        part.ptr  = packet;
        part.len  = len;
        part.next = NULL;
        return send_ether(&lp->packet_ctx, BROADCAST_MAC, ETHER_TYPE_IPV4, &part);
    }

    for_each_ctx_t ctx = { lp, packet, len };
    arp_for_each(&lp->packet_ctx, &ctx, lan_client_arp_cb);
    return 0;
}

static int lan_client_process(struct lan_play *lp,
                              const uint8_t *packet, uint16_t len)
{
    if (len == 0) return 0;

    /* Stamp activity: a real game packet arrived from the relay */
    lp->last_game_activity = armGetSystemTick();

    uint8_t dst_mac[6];
    const uint8_t *dst = packet + IPV4_OFF_DST;
    struct payload part;

    if (IS_BROADCAST(dst, lp->packet_ctx.subnet_net, lp->packet_ctx.subnet_mask)) {
        return lan_client_on_broadcast(lp, packet, len);
    } else if (!arp_get_mac_by_ip(&lp->packet_ctx, dst_mac, dst)) {
        return 0;
    }

    part.ptr  = packet;
    part.len  = len;
    part.next = NULL;
    return send_ether(&lp->packet_ctx, dst_mac, ETHER_TYPE_IPV4, &part);
}

static int lan_client_process_frag(struct lan_play *lp,
                                   const uint8_t *packet, uint16_t len)
{
    struct lan_client_fragment *frags = lp->frags;

    uint8_t  src[4], dst[4];
    CPY_IPV4(src, packet + LC_FRAG_SRC);
    CPY_IPV4(dst, packet + LC_FRAG_DST);
    uint16_t id         = READ_NET16(packet, LC_FRAG_ID);
    uint8_t  part_num   = READ_NET8(packet,  LC_FRAG_PART);
    uint8_t  total_part = READ_NET8(packet,  LC_FRAG_TOTAL_PART);
    uint16_t frag_len   = READ_NET16(packet, LC_FRAG_LEN);

    /* Bounds validation: reject obviously malformed fragments */
    if (total_part == 0 || total_part > 8) {
        LLOG(LLOG_WARNING, "relay: frag total_part=%d out of range, dropping", total_part);
        return 0;
    }
    if (part_num >= total_part) {
        LLOG(LLOG_WARNING, "relay: frag part_num=%d >= total_part=%d, dropping", part_num, total_part);
        return 0;
    }
    uint16_t pmtu       = READ_NET16(packet, LC_FRAG_PMTU);

    struct lan_client_fragment *frag = NULL;
    int i;
    for (i = 0; i < LC_FRAG_COUNT; i++) {
        if (frags[i].used && frags[i].id == id && CMP_IPV4(frags[i].src, src)) {
            frag = &frags[i];
            break;
        }
    }
    if (!frag) {
        for (i = 0; i < LC_FRAG_COUNT; i++) {
            if (!frags[i].used) {
                frag = &frags[i];
                frag->used     = 1;
                frag->id       = id;
                frag->local_id = (uint16_t)lp->local_id++;
                CPY_IPV4(frag->src, src);
                frag->part     = 0;
                break;
            }
        }
    }
    if (!frag) {
        LLOG(LLOG_WARNING, "relay: fragment buffer full, dropping");
        return 0;
    }

    if (pmtu > 0 && (size_t)(pmtu * part_num + frag_len) <= ETHER_MTU) {
        frag->part |= (uint8_t)(1 << part_num);
        memcpy(&frag->buffer[pmtu * part_num],
               packet + LC_FRAG_HEADER_LEN, frag_len);
        if (part_num == total_part - 1)
            frag->total_len = (uint16_t)((total_part - 1) * pmtu + frag_len);
        if ((uint8_t)(~(~0u << total_part)) == frag->part) {
            frag->used = 0;
            return lan_client_process(lp, frag->buffer, frag->total_len);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/*  Auth handling                                                           */
/* ---------------------------------------------------------------------- */
static void lan_client_process_auth_me(struct lan_play *lp,
                                       const uint8_t *packet, uint16_t len)
{
    if (!lp->username) {
        LLOG(LLOG_WARNING, "relay: server requests auth but no username set");
        return;
    }
    if (len < 1) return;
    uint8_t auth_type = packet[0];
    if (auth_type != 0) {
        LLOG(LLOG_WARNING, "relay: unknown auth type %d", auth_type);
        return;
    }
    const uint8_t *challenge     = packet + 1;
    uint16_t       challenge_len = (uint16_t)(len - 1);
    uint16_t       uname_len     = (uint16_t)strlen(lp->username);
    uint16_t       resp_len      = 20 + uname_len;
    uint8_t       *resp          = (uint8_t *)malloc(resp_len);
    if (!resp) {
        LLOG(LLOG_ERROR, "lan_client_process_auth_me: out of memory");
        return;
    }

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, lp->key, LP_KEY_LEN);
    SHA1Update(&ctx, challenge, challenge_len);
    SHA1Final(resp, &ctx);
    /* Scrub the SHA1 context — it contains key material */
    memset(&ctx, 0, sizeof(ctx));
    memcpy(resp + 20, lp->username, uname_len);
    lan_client_send(lp, LAN_CLIENT_TYPE_AUTH_ME, resp, resp_len);
    /* Scrub the response buffer before freeing */
    memset(resp, 0, resp_len);
    free(resp);
}

/* ---------------------------------------------------------------------- */
/*  Receive thread                                                          */
/* ---------------------------------------------------------------------- */
void lan_client_recv_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: receive thread started");

    while (lp->running) {
        lp->wd_relay = armGetSystemTick(); /* watchdog pet */
        mutexLock(&lp->mutex);
        int fd = lp->relay_fd;
        mutexUnlock(&lp->mutex);

        if (fd < 0) {
            svcSleepThread(1000000000LL); /* 1 second */
            continue;
        }

        ssize_t n = recvfrom(fd, lp->relay_buf, sizeof(lp->relay_buf), 0, NULL, NULL);
        if (n <= 0) {
            if (n < 0) {
                int err = errno;
                if (err == EBADF) {
                    LLOG(LLOG_WARNING, "relay: recvfrom EBADF on fd %d, socket closed, retrying", fd);
                    continue;
                }
                if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR) {
                    /* Rate-limit error logs: only once every 30 failures */
                    static int err_count = 0;
                    if (++err_count <= 3 || (err_count % 30 == 0)) {
                        LLOG(LLOG_ERROR, "relay: recvfrom error (%d): %s (fd=%d)",
                             err_count, strerror(errno), fd);
                    }
                }
            }
            continue;
        }

        /* Anti-Flood Rate Limiting */
        uint64_t now_tick = armGetSystemTick();
        uint64_t freq = armGetSystemTickFreq();
        if (freq > 0) {
            uint64_t current_sec = now_tick / freq;
            if (current_sec != lp->rl_last_sec_tick) {
                lp->rl_last_sec_tick = current_sec;
                lp->rl_packets_this_sec = 0;
            }
            if (++lp->rl_packets_this_sec > 2000) {
                lp->packets_dropped++;
                static int flood_warn = 0;
                if (++flood_warn <= 3 || (flood_warn % 1000 == 0)) {
                    LLOG(LLOG_WARNING, "relay: anti-flood triggered! >2000 pkt/sec, dropping");
                }
                continue; /* Drop packet if exceeding 2000 pkt/sec */
            }
        }

        lp->download_packet++;
        lp->download_byte += (uint64_t)n;

        uint8_t type = lp->relay_buf[0] & 0x7f;

        switch (type) {
        case LAN_CLIENT_TYPE_KEEPALIVE:
            break;
        case LAN_CLIENT_TYPE_IPV4:
            {
                uint16_t payload_len = (uint16_t)(n - 1);
                /* Minimum valid IPv4 header is 20 bytes */
                if (payload_len < 20) {
                    lp->packets_dropped++;
                    static int short_ipv4 = 0;
                    if (++short_ipv4 <= 5)
                        LLOG(LLOG_WARNING, "relay: IPV4 too short (%d bytes), dropping", payload_len);
                    break;
                }
                static int ipv4_count = 0;
                if (++ipv4_count <= 8) {
                    LLOG(LLOG_INFO, "relay: recv IPV4 packet len=%d count=%d", (int)n, ipv4_count);
                }
                lan_client_process(lp, lp->relay_buf + 1, payload_len);
            }
            break;
        case LAN_CLIENT_TYPE_IPV4_FRAG:
            {
                uint16_t payload_len = (uint16_t)(n - 1);
                /* Fragment header is LC_FRAG_HEADER_LEN (16) bytes minimum */
                if (payload_len < LC_FRAG_HEADER_LEN) {
                    lp->packets_dropped++;
                    static int short_frag = 0;
                    if (++short_frag <= 5)
                        LLOG(LLOG_WARNING, "relay: IPV4_FRAG too short (%d bytes), dropping", payload_len);
                    break;
                }
                static int frag_count = 0;
                if (++frag_count <= 8) {
                    LLOG(LLOG_INFO, "relay: recv IPV4_FRAG packet len=%d count=%d", (int)n, frag_count);
                }
                lan_client_process_frag(lp, lp->relay_buf + 1, payload_len);
            }
            break;
        case LAN_CLIENT_TYPE_AUTH_ME:
            {
                uint16_t payload_len = (uint16_t)(n - 1);
                /* Auth packet must have at least 1 byte (auth_type) */
                if (payload_len < 1) {
                    lp->packets_dropped++;
                    LLOG(LLOG_WARNING, "relay: AUTH_ME empty, dropping");
                    break;
                }
                lan_client_process_auth_me(lp, lp->relay_buf + 1, payload_len);
            }
            break;
        case LAN_CLIENT_TYPE_INFO:
            LLOG(LLOG_INFO, "[Server]: %.*s", (int)(n - 1), lp->relay_buf + 1);
            break;
        }
    }

    LLOG(LLOG_INFO, "relay: receive thread exiting");
}

/* ---------------------------------------------------------------------- */
/*  Keepalive thread (with exponential backoff + ping measurement)          */
/* ---------------------------------------------------------------------- */
void lan_client_keepalive_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: keepalive thread started");

    int consecutive_fails = 0;
    int backoff_seconds   = 10; /* initial sleep between keepalives */

    lp->connection_healthy = true;

    while (lp->running) {
        lp->wd_keepalive = armGetSystemTick(); /* watchdog pet */

        /* Measure RTT: timestamp before sendto */
        uint64_t t_start = armGetSystemTick();

        uint8_t ka = LAN_CLIENT_TYPE_KEEPALIVE;
        int ret = relay_send_raw(lp, &ka, 1);

        if (ret < 0) {
            int err = errno;
            lp->connection_healthy = false;

            /* EHOSTUNREACH / ENETUNREACH = route temporarily missing (WiFi
             * just reconnected).  The socket is fine; don't destroy it —
             * just wait for the routing table to recover. */
            if (err == EHOSTUNREACH || err == ENETUNREACH) {
                LLOG(LLOG_WARNING, "relay: route not ready (%s), waiting...", strerror(err));
                /* Sleep in 1-second chunks so we can terminate quickly */
                for (int i = 0; i < 10 && lp->running; i++)
                    svcSleepThread(1000000000LL);
                continue;
            }

            consecutive_fails++;

            /* Exponential backoff: after 6 consecutive failures (≈60s),
             * try recreating the socket.  Backoff doubles each cycle. */
            if (consecutive_fails >= 6) {
                LLOG(LLOG_WARNING, "relay: %d keepalive failures (backoff=%ds), recreating socket...",
                     consecutive_fails, backoff_seconds);
                mutexLock(&lp->mutex);
                lan_client_close(lp);
                /* Brief pause so the BSD service can free the socket buffers
                 * before we allocate a new one (prevents ENOBUFS). */
                svcSleepThread(2000000000LL);
                if (lan_client_init(lp) == 0) {
                    LLOG(LLOG_INFO, "relay: socket recreated fd=%d", lp->relay_fd);
                    lp->relay_reconnects++;
                } else {
                    LLOG(LLOG_ERROR, "relay: failed to recreate socket");
                }
                mutexUnlock(&lp->mutex);
                consecutive_fails = 0;

                /* Exponential backoff: double the sleep, cap at 60s */
                backoff_seconds = backoff_seconds * 2;
                if (backoff_seconds > 60) backoff_seconds = 60;
            }
        } else {
            /* Keepalive succeeded */
            if (!lp->connection_healthy || consecutive_fails > 0) {
                LLOG(LLOG_INFO, "relay: keepalive OK after %d failures", consecutive_fails);
            }
            lp->connection_healthy = true;
            consecutive_fails = 0;
            backoff_seconds = 10; /* reset backoff on success */

            /* Calculate approximate RTT in milliseconds.
             * armGetSystemTickFreq() gives ticks/second on this core. */
            uint64_t t_end = armGetSystemTick();
            uint64_t freq  = armGetSystemTickFreq();
            if (freq > 0) {
                uint64_t elapsed_ms = ((t_end - t_start) * 1000) / freq;
                lp->current_ping_ms = (uint32_t)(elapsed_ms > 9999 ? 9999 : elapsed_ms);
            }
        }

        /* Also toggle next_real_broadcast so we do a real broadcast periodically */
        lp->next_real_broadcast = true;

        /* ---- Adaptive power management ----
         * If no game packets have flowed for 30 seconds, enter idle mode:
         *   - Keepalive every 30s instead of 10s (3× fewer CPU wakeups)
         *   - Log transition for diagnostics
         * Wake up instantly when game traffic resumes. */
        int sleep_seconds = backoff_seconds;
        {
            uint64_t now_tick = armGetSystemTick();
            uint64_t freq = armGetSystemTickFreq();
            uint64_t idle_threshold_s = 30;
            bool was_idle = lp->idle_mode;

            if (freq > 0 && lp->last_game_activity > 0) {
                uint64_t elapsed_s = (now_tick - lp->last_game_activity) / freq;
                if (elapsed_s >= idle_threshold_s) {
                    if (!was_idle) {
                        LLOG(LLOG_INFO, "power: entering idle mode (no game traffic for %llus)",
                             (unsigned long long)elapsed_s);
                    }
                    lp->idle_mode = true;
                    sleep_seconds = 30; /* reduce wakeups: 30s keepalive */
                } else {
                    if (was_idle) {
                        LLOG(LLOG_INFO, "power: leaving idle mode (game traffic resumed)");
                    }
                    lp->idle_mode = false;
                }
            }
        }

        /* Sleep in 1-second chunks so we can terminate quickly on reboot/reload */
        for (int i = 0; i < sleep_seconds && lp->running; i++) {
            svcSleepThread(1000000000LL); /* 1 second */
        }
    }
    LLOG(LLOG_INFO, "relay: keepalive thread exiting");
}

/* ---------------------------------------------------------------------- */
/*  Init / close                                                            */
/* ---------------------------------------------------------------------- */
int lan_client_init(struct lan_play *lp)
{
    lp->frag_id    = 0;
    lp->local_id   = 0;
    lp->next_real_broadcast = true;
    memset(lp->frags, 0, sizeof(lp->frags));

    lp->relay_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (lp->relay_fd < 0) {
        LLOG(LLOG_ERROR, "relay: socket failed: %s", strerror(errno));
        return -1;
    }

    /* Allow broadcast sends (for servers on LAN) */
    int on = 1;
    setsockopt(lp->relay_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    /* Set receive timeout so thread can check lp->running */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(lp->relay_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char srv_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &lp->server_addr.sin_addr, srv_ip, sizeof(srv_ip));
    LLOG(LLOG_INFO, "relay: server %s:%d  fd=%d",
         srv_ip, ntohs(lp->server_addr.sin_port), lp->relay_fd);

    lp->upload_packet   = 0;
    lp->upload_byte     = 0;
    lp->download_packet = 0;
    lp->download_byte   = 0;
    return 0;
}

void lan_client_close(struct lan_play *lp)
{
    if (lp->relay_fd >= 0) {
        close(lp->relay_fd);
        lp->relay_fd = -1;
    }
    /* Wipe fragment reassembly buffers — they may contain packet data */
    memset(lp->frags, 0, sizeof(lp->frags));
}
