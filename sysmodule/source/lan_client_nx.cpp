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
        LLOG(LLOG_ERROR, "relay_send_raw: sendto failed: %s", strerror(errno));
        return -1;
    }
    lp->upload_packet++;
    lp->upload_byte += (uint64_t)len;
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
            uint8_t *buf = (uint8_t *)malloc(total);
            if (!buf) return -1;
            buf[0] = frag_type;
            memcpy(buf + 1,                        header, LC_FRAG_HEADER_LEN);
            memcpy(buf + 1 + LC_FRAG_HEADER_LEN,   packet + pos, part_len);
            int ret = relay_send_raw(lp, buf, total);
            free(buf);
            if (ret) return ret;
        }
        return 0;
    }

    /* Non-fragmented: [type(1)] [data(len)] */
    size_t total = 1 + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;
    buf[0] = type;
    if (len > 0) memcpy(buf + 1, packet, len);
    int ret = relay_send_raw(lp, buf, total);
    free(buf);
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

static int arp_for_each_cb(void *p, const struct arp_item *item)
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
    arp_for_each(&lp->packet_ctx, &ctx, arp_for_each_cb);
    return 0;
}

static int lan_client_process(struct lan_play *lp,
                              const uint8_t *packet, uint16_t len)
{
    if (len == 0) return 0;

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
    if (!resp) return;

    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, lp->key, LP_KEY_LEN);
    SHA1Update(&ctx, challenge, challenge_len);
    SHA1Final(resp, &ctx);
    memcpy(resp + 20, lp->username, uname_len);
    lan_client_send(lp, LAN_CLIENT_TYPE_AUTH_ME, resp, resp_len);
    free(resp);
}

/* ---------------------------------------------------------------------- */
/*  Receive thread                                                          */
/* ---------------------------------------------------------------------- */
void lan_client_recv_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: receive thread started (fd=%d)", lp->relay_fd);

    while (lp->running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        ssize_t n = recvfrom(lp->relay_fd,
                             lp->relay_buf, sizeof(lp->relay_buf), 0,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LLOG(LLOG_ERROR, "relay: recvfrom error: %s", strerror(errno));
            continue;
        }

        lp->download_packet++;
        lp->download_byte += (uint64_t)n;

        uint8_t type = lp->relay_buf[0] & 0x7f;

        switch (type) {
        case LAN_CLIENT_TYPE_KEEPALIVE:
            break;
        case LAN_CLIENT_TYPE_IPV4:
            lan_client_process(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_IPV4_FRAG:
            lan_client_process_frag(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_AUTH_ME:
            lan_client_process_auth_me(lp, lp->relay_buf + 1, (uint16_t)(n - 1));
            break;
        case LAN_CLIENT_TYPE_INFO:
            LLOG(LLOG_INFO, "[Server]: %.*s", (int)(n - 1), lp->relay_buf + 1);
            break;
        }
    }

    LLOG(LLOG_INFO, "relay: receive thread exiting");
}

/* ---------------------------------------------------------------------- */
/*  Keepalive thread                                                        */
/* ---------------------------------------------------------------------- */
void lan_client_keepalive_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;
    LLOG(LLOG_INFO, "relay: keepalive thread started");

    while (lp->running) {
        uint8_t ka = LAN_CLIENT_TYPE_KEEPALIVE;
        relay_send_raw(lp, &ka, 1);

        /* Also toggle next_real_broadcast so every 1 s we do a real broadcast */
        lp->next_real_broadcast = true;

        svcSleepThread(10000000000LL); /* 10 seconds in nanoseconds */
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
}
