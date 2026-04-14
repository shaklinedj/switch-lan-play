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
 * to a raw socket.  For LAN discovery this is acceptable: ldn_mitm (patched)
 * sends its outgoing Scan broadcasts explicitly to the private IPC socket at
 * 127.0.0.1:LDN_IPC_PORT (11453) so the sysmodule can relay them over UDP.
 * Inbound packets from the relay are injected via tap_send_packet() and
 * delivered to ldn_mitm's LDN_PORT (11452) socket by the kernel.
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

    /* ------------------------------------------------------------------ */
    /* LDN IPC socket — private loopback UDP server on 127.0.0.1:LDN_IPC_PORT.
     *
     * When ldn_mitm (patched) has switch-lan-play active it sends its
     * outgoing LAN-discovery datagrams (Scan, ScanResp, Connect, SyncNetwork)
     * directly to 127.0.0.1:LDN_IPC_PORT instead of broadcasting on the LAN.
     *
     * This is cleaner than the REUSEPORT approach because:
     *   - No SO_REUSEPORT / SO_BROADCAST needed.
     *   - All traffic on 127.0.0.1:LDN_IPC_PORT is unambiguously from ldn_mitm
     *     (no loop-prevention filter needed).
     *   - Works even if another process owns port LDN_PORT (11452).
     * ------------------------------------------------------------------ */
    lp->ldn_fd = -1;

    int ldn_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ldn_sock < 0) {
        LLOG(LLOG_WARNING, "tap: LDN IPC socket() failed: %s (ldn_mitm IPC disabled)",
             strerror(errno));
    } else {
        /* Short receive timeout so the thread can check lp->running */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (setsockopt(ldn_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            LLOG(LLOG_WARNING, "tap: LDN SO_RCVTIMEO failed: %s", strerror(errno));

        /* Bind to loopback only — no external process should reach this port */
        struct sockaddr_in ipc_addr;
        memset(&ipc_addr, 0, sizeof(ipc_addr));
        ipc_addr.sin_family      = AF_INET;
        ipc_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* 127.0.0.1 */
        ipc_addr.sin_port        = htons(LDN_IPC_PORT);

        if (bind(ldn_sock, (struct sockaddr *)&ipc_addr, sizeof(ipc_addr)) < 0) {
            LLOG(LLOG_WARNING, "tap: LDN IPC bind(127.0.0.1:%d) failed: %s "
                 "(ldn_mitm IPC disabled)", LDN_IPC_PORT, strerror(errno));
            close(ldn_sock);
        } else {
            lp->ldn_fd = ldn_sock;
            LLOG(LLOG_INFO, "tap: LDN IPC socket fd=%d (127.0.0.1:%d) ready",
                 lp->ldn_fd, LDN_IPC_PORT);
        }
    }

    return 0;
}

void tap_close(struct lan_play *lp)
{
    if (lp->raw_fd >= 0) {
        close(lp->raw_fd);
        lp->raw_fd = -1;
    }
    if (lp->ldn_fd >= 0) {
        close(lp->ldn_fd);
        lp->ldn_fd = -1;
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

/* -------------------------------------------------------------------------
 * LDN bridge thread
 * ---------------------------------------------------------------------- */

/**
 * ldn_bridge_thread_fn — receives ldn_mitm's outgoing LAN-discovery packets
 * via the private IPC channel (127.0.0.1:LDN_IPC_PORT) and forwards them to
 * the relay server.
 *
 * IPC message format (produced by patched LDUdpSocket::sendto):
 *   [4 bytes — IPv4 destination address, network byte order]
 *   [N bytes — LDN protocol packet (LANPacketHeader + payload)]
 *
 * The destination IPv4 can be:
 *   10.13.255.255  — virtual subnet broadcast (Scan)
 *   10.13.x.y      — unicast virtual IP (ScanResp, direct sends)
 *
 * This design handles BOTH broadcast (Scan) and unicast (ScanResp) so the
 * complete discovery exchange works through the relay:
 *   Station sends Scan broadcast → all APs receive via relay
 *   AP sends ScanResp unicast   → Station receives via relay
 *
 * No loop-prevention needed: 127.0.0.1 traffic is exclusively local
 * (ldn_mitm).  Relay-injected packets arrive via tap_send_packet and are
 * delivered to ldn_mitm's LDN_PORT socket — they never reach this IPC socket.
 */
void ldn_bridge_thread_fn(void *arg)
{
    struct lan_play *lp = (struct lan_play *)arg;

    if (lp->ldn_fd < 0) {
        LLOG(LLOG_WARNING, "ldn_bridge: IPC socket not available, thread not started");
        return;
    }

    LLOG(LLOG_INFO, "ldn_bridge: IPC thread started (fd=%d, port %d)",
         lp->ldn_fd, LDN_IPC_PORT);

    /*
     * IPC buffer: 4-byte dst IP prefix + LDN payload
     */
    static const size_t IPC_HDR_LEN = 4;
    uint8_t ipc_buf[IPC_HDR_LEN + TAP_BUF_SIZE];

    /* We build the forwarded frame as: Ethernet(14) + IPv4(20) + UDP(8) + payload */
    uint8_t frame[ETHER_HEADER_LEN + IPV4_HEADER_LEN + UDP_OFF_END + TAP_BUF_SIZE];

    while (lp->running) {
        ssize_t n = recv(lp->ldn_fd, ipc_buf, sizeof(ipc_buf), 0);

        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LLOG(LLOG_ERROR, "ldn_bridge: recv error: %s", strerror(errno));
            continue;
        }

        if (n <= (ssize_t)IPC_HDR_LEN) continue; /* truncated, drop */

        /* Extract 4-byte destination IP from the IPC header */
        uint8_t  dst_ip[4];
        memcpy(dst_ip, ipc_buf, IPC_HDR_LEN);
        const uint8_t *ldn_payload = ipc_buf + IPC_HDR_LEN;
        ssize_t        ldn_len     = n - (ssize_t)IPC_HDR_LEN;

        /* ---------------------------------------------------------------- */
        /* Build the synthetic Ethernet + IPv4 + UDP frame to relay         */
        /* ---------------------------------------------------------------- */

        /* --- Ethernet header --- */
        uint8_t *eth = frame;
        memset(eth,      0xff, 6);            /* dst = broadcast MAC (relay routes) */
        memcpy(eth + 6,  lp->my_mac, 6);      /* src = our virtual MAC */
        eth[12] = 0x08; eth[13] = 0x00;       /* EtherType = IPv4 */

        /* --- IPv4 header (20 bytes, no options) --- */
        uint8_t *ip = frame + ETHER_HEADER_LEN;
        uint16_t ip_total_len = (uint16_t)(IPV4_HEADER_LEN + UDP_OFF_END + ldn_len);

        ip[IPV4_OFF_VER_LEN]   = 0x45;
        ip[IPV4_OFF_DSCP_ECN]  = 0x00;
        WRITE_NET16(ip, IPV4_OFF_TOTAL_LEN,         ip_total_len);
        WRITE_NET16(ip, IPV4_OFF_ID,                (uint16_t)(lp->frag_id++ & 0xFFFF));
        WRITE_NET16(ip, IPV4_OFF_FLAGS_FRAG_OFFSET, 0x0000);
        ip[IPV4_OFF_TTL]       = 64;
        ip[IPV4_OFF_PROTOCOL]  = IPV4_PROTOCOL_UDP;
        WRITE_NET16(ip, IPV4_OFF_CHECKSUM, 0);   /* computed below */
        CPY_IPV4(ip + IPV4_OFF_SRC, lp->my_ip); /* source = our virtual IP */
        CPY_IPV4(ip + IPV4_OFF_DST, dst_ip);     /* dest   = from IPC header */

        /* IPv4 header checksum */
        uint32_t cksum = 0;
        for (int i = 0; i < IPV4_HEADER_LEN; i += 2) {
            cksum += (uint32_t)((ip[i] << 8) | ip[i + 1]);
        }
        while (cksum >> 16) cksum = (cksum & 0xFFFFu) + (cksum >> 16);
        WRITE_NET16(ip, IPV4_OFF_CHECKSUM, (uint16_t)(~cksum & 0xFFFFu));

        /* --- UDP header (8 bytes) --- */
        uint8_t *udp = ip + IPV4_HEADER_LEN;
        WRITE_NET16(udp, UDP_OFF_SRCPORT,  LDN_PORT);
        WRITE_NET16(udp, UDP_OFF_DSTPORT,  LDN_PORT);
        WRITE_NET16(udp, UDP_OFF_LENGTH,   (uint16_t)(UDP_OFF_END + ldn_len));
        WRITE_NET16(udp, UDP_OFF_CHECKSUM, 0); /* optional for UDP/IPv4 */

        /* --- LDN payload --- */
        memcpy(udp + UDP_OFF_END, ldn_payload, (size_t)ldn_len);

        /* lan_client_send_ipv4() expects raw IPv4 (no Ethernet header) */
        lan_client_send_ipv4(lp, (void *)(ip + IPV4_OFF_DST), ip, ip_total_len);
    }

    LLOG(LLOG_INFO, "ldn_bridge: IPC thread exiting");
}
