#pragma once
/**
 * lanplay_bridge.hpp — interface between ldn_mitm and the switch-lan-play sysmodule.
 *
 * The sysmodule writes sdmc:/tmp/lanplay.status every ~2 seconds:
 *   active=1
 *   my_ip=10.13.X.Y
 *
 * This header provides helpers to query that state and to forward outbound
 * LDN discovery packets to the sysmodule's private IPC socket.
 *
 * IPC message format (sent by LDUdpSocket::sendto to 127.0.0.1:LDN_IPC_PORT):
 *   [4 bytes: IPv4 destination address, network byte order]
 *   [N bytes: LDN protocol packet (LANPacketHeader + payload)]
 *
 * The destination can be 10.13.255.255 (broadcast, for Scan) or a unicast
 * virtual IP 10.13.x.y (for ScanResp and other directed packets).
 */

#include <switch.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdbool.h>

/** Port that the sysmodule's IPC UDP socket listens on (127.0.0.1 only). */
static const uint16_t LDN_IPC_PORT = 11453;

#ifdef __cplusplus
extern "C" {
#endif

/** Returns true if switch-lan-play is active and has a valid virtual IP. */
bool     lanplay_is_active(void);

/** Returns this console's virtual IP (host byte order, 10.13.x.y), or 0. */
uint32_t lanplay_get_virtual_ip(void);

/** Returns the virtual subnet broadcast (10.13.255.255, host byte order), or 0. */
uint32_t lanplay_get_broadcast(void);

/** Fills *out with 127.0.0.1:LDN_IPC_PORT when active. Returns true on success. */
bool     lanplay_get_ipc_sockaddr(struct sockaddr_in *out);

/**
 * lanplay_send_ipc — forward a serialised LDN packet to the sysmodule IPC socket.
 *
 * Prepends the 4-byte network-byte-order destination IP from *dst_addr, then
 * sends [dst_ip(4)][buf(len)] to 127.0.0.1:LDN_IPC_PORT via a persistent UDP
 * socket.  The socket is created on first call and reused thereafter.
 *
 * Returns true if the datagram was sent successfully.
 */
bool     lanplay_send_ipc(const void *buf, size_t len,
                          const struct sockaddr_in *dst_addr);

#ifdef __cplusplus
}
#endif
