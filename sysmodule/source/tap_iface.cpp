/**
 * tap_iface.cpp — virtual TAP layer for the Switch sysmodule.
 *
 * Replaces src/pcaploop.cpp + libpcap.
 *
 * Architecture
 * ============
 * The Switch game sends LAN-play broadcast UDP packets to 10.13.255.255.
 * On a BSD-based kernel (Horizon uses a NetBSD-derived network stack) a
 * SOCK_RAW socket with IPPROTO_IP receives ALL inbound IP datagrams,
 * including broadcast packets delivered to the local machine.  We use
 * IP_HDRINCL for full control over the outgoing IP header when injecting
 * packets back to the game.
 *
 * Outgoing-packet capture
 * -----------------------
 * On most BSD kernels, locally-generated outgoing packets are NOT visible
 * to a raw socket.  For LAN discovery this is acceptable: the game sends a
 * broadcast, which is echoed back via the relay to all other players
 * (including our own sysmodule).  The sysmodule therefore only needs to
 * (a) forward outgoing packets from a UDP listener on the LAN-play ports,
 * and (b) inject inbound packets from the relay using the raw socket.
 *
 * For outgoing packet capture we bind an additional UDP socket to port 0
 * with SO_REUSEPORT and SO_BROADCAST so we can receive the game's own
 * broadcasts on the subnet.
 */

#include "tap_iface.h"
#include "packet.h"

/* Maximum raw IP payload we handle */
#define TAP_BUF_SIZE 2048

/* Synthetic Ethernet MAC for the virtual interface.
 * Built from the lower 5 bytes of the Switch device UID so it is unique
 * per console while staying in the locally-administered range. */
static void build_virtual_mac(uint8_t mac[6])
{
    /* Locally administered, unicast: bit 1 of first octet set, bit 0 clear */
    mac[0] = 0x02;

    /* Use the bottom 40 bits of the device UID for the remaining bytes */
    SetSysSerialNumber serial;
    Result rc = setsysGetSerialNumber(&serial);
    if (R_SUCCEEDED(rc)) {
        /* Hash the serial string into 5 bytes */
        uint32_t h = 0x811c9dc5u;
        size_t i = 0;
        for (; i < sizeof(serial.number) && serial.number[i]; i++) {
            h ^= (uint8_t)serial.number[i];
            h *= 0x01000193u;
        }
        mac[1] = (h >> 24) & 0xff;
        mac[2] = (h >> 16) & 0xff;
        mac[3] = (h >>  8) & 0xff;
        mac[4] =  h        & 0xff;
        mac[5] = (uint8_t)(i % 256); /* extra mixing from string length */
    } else {
        /* Fallback: fixed bytes */
        mac[1] = 0x4E; mac[2] = 0x58; /* "NX" */
        mac[3] = 0x4C; mac[4] = 0x50; /* "LP" */
        mac[5] = 0x01;
    }
}

int tap_init(struct lan_play *lp)
{
    /* Build the virtual MAC for this console */
    build_virtual_mac(lp->my_mac);
    LLOG(LLOG_INFO, "tap: virtual MAC %02x:%02x:%02x:%02x:%02x:%02x",
         lp->my_mac[0], lp->my_mac[1], lp->my_mac[2],
         lp->my_mac[3], lp->my_mac[4], lp->my_mac[5]);

    /* Open raw IP socket */
    lp->raw_fd = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
    if (lp->raw_fd < 0) {
        LLOG(LLOG_ERROR, "tap: socket(SOCK_RAW) failed: %s", strerror(errno));
        return -1;
    }

    /* Enable inclusion of full IP header in sent packets */
    int on = 1;
    if (setsockopt(lp->raw_fd, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) < 0) {
        /* Non-fatal: log and continue — the socket is still usable */
        LLOG(LLOG_WARNING, "tap: IP_HDRINCL failed: %s (continuing)", strerror(errno));
    }

    /* Allow receiving and sending broadcast packets */
    if (setsockopt(lp->raw_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
        LLOG(LLOG_WARNING, "tap: SO_BROADCAST failed: %s", strerror(errno));
    }

    /* Bind to INADDR_ANY so we see all incoming packets */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(lp->raw_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LLOG(LLOG_WARNING, "tap: bind failed: %s (continuing)", strerror(errno));
    }

    /* Set a receive timeout so the thread can check lp->running */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(lp->raw_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LLOG(LLOG_INFO, "tap: raw socket fd=%d opened", lp->raw_fd);
    return 0;
}

void tap_close(struct lan_play *lp)
{
    if (lp->raw_fd >= 0) {
        close(lp->raw_fd);
        lp->raw_fd = -1;
    }
}

/**
 * tap_send_packet — called by packet.c::send_payloads() to deliver a
 * completed Ethernet frame back to the game.
 *
 * We strip the 14-byte Ethernet header (which was synthesised by send_ether)
 * and use the raw socket to inject the inner IPv4 packet.  The kernel
 * delivers it to any socket listening on the destination address, so the
 * game's LAN-play socket sees it as an incoming packet from a peer.
 */
int tap_send_packet(struct lan_play *lp, const void *eth_frame, int len)
{
    if (len <= ETHER_HEADER_LEN) return 0;

    const uint8_t *ip_start = (const uint8_t *)eth_frame + ETHER_HEADER_LEN;
    int ip_len              = len - ETHER_HEADER_LEN;

    /* Destination IP is at offset 16 inside the IPv4 header */
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    memcpy(&dst.sin_addr.s_addr, ip_start + IPV4_OFF_DST, 4);

    ssize_t sent = sendto(lp->raw_fd, ip_start, (size_t)ip_len, 0,
                          (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) {
        LLOG(LLOG_ERROR, "tap: sendto failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * tap_recv_thread_fn — background thread that pumps inbound IP packets
 * from the raw socket through the LAN-play pipeline.
 *
 * For each received IP packet:
 *   1. Build a synthetic Ethernet frame (src=broadcast MAC, dst=virtual MAC)
 *   2. Call get_packet() which runs process_ether → process_ipv4 → relay
 */
void tap_recv_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;

    /* Receive buffer: 14-byte synthetic Ethernet header + MTU IP payload.
     * Allocated on the stack to avoid sharing state between thread invocations. */
    uint8_t eth_buf[ETHER_HEADER_LEN + TAP_BUF_SIZE];
    uint8_t ip_buf[TAP_BUF_SIZE];

    /* Pre-build the Ethernet header template:
     *   dst = broadcast (FF:FF:FF:FF:FF:FF) — will be filled by ARP logic
     *   src = our virtual MAC
     *   type = 0x0800 (IPv4) */
    uint8_t *eth_hdr = eth_buf;
    memset(eth_hdr,        0xff, 6);        /* dst = broadcast */
    memcpy(eth_hdr + 6,    lp->my_mac, 6);  /* src = our MAC   */
    eth_hdr[12] = 0x08; eth_hdr[13] = 0x00; /* type = IPv4     */

    LLOG(LLOG_INFO, "tap: receive thread started (fd=%d)", lp->raw_fd);

    while (lp->running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        ssize_t n = recvfrom(lp->raw_fd, ip_buf, sizeof(ip_buf), 0,
                             (struct sockaddr *)&from, &from_len);

        if (n <= 0) {
            /* Timeout or error — loop back to check lp->running */
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                LLOG(LLOG_ERROR, "tap: recvfrom error: %s", strerror(errno));
            }
            continue;
        }

        if ((size_t)n > TAP_BUF_SIZE) continue; /* oversized, drop */

        /* Skip packets that do NOT belong to the 10.13.0.0/16 subnet */
        uint8_t *ip_src = ip_buf + IPV4_OFF_SRC;
        uint8_t *ip_dst = ip_buf + IPV4_OFF_DST;

        uint32_t net  = *(uint32_t *)lp->packet_ctx.subnet_net;
        uint32_t mask = *(uint32_t *)lp->packet_ctx.subnet_mask;

        if ((*(uint32_t *)ip_src & mask) != net &&
            (*(uint32_t *)ip_dst & mask) != net) {
            continue; /* not our subnet */
        }

        /* Don't loop back our own packets */
        if (CMP_IPV4(ip_src, lp->my_ip)) continue;

        /* Copy IP payload after the synthetic Ethernet header */
        memcpy(eth_buf + ETHER_HEADER_LEN, ip_buf, (size_t)n);

        /* Build a fake pcap_pkthdr so get_packet() accepts it */
        struct pcap_pkthdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.caplen = (uint32_t)(ETHER_HEADER_LEN + n);
        hdr.len    = hdr.caplen;

        /* Set the packet_ctx MAC to our virtual MAC so the ARP/IP logic
         * can recognise frames "sent by us". */
        packet_set_mac(&lp->packet_ctx, lp->my_mac);
        get_packet(&lp->packet_ctx, &hdr, eth_buf);
    }

    LLOG(LLOG_INFO, "tap: receive thread exiting");
}
