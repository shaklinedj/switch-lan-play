/* Adapted from src/ipv4/icmp.c — only the include path changed. */
#include "ipv4.h"

int process_icmp(struct packet_ctx *self, const struct ipv4 *ipv4)
{
    uint8_t payload[BUFFER_SIZE];
    struct payload part;

    int icmp_len = ipv4->total_len - ipv4->header_len;
    memcpy(payload, ipv4->payload, (size_t)icmp_len);
    WRITE_NET8(payload,  0, 0);      /* type = echo reply */
    WRITE_NET16(payload, 2, 0x0000); /* clear checksum */
    uint16_t sum = calc_checksum(payload, icmp_len);
    WRITE_NET16(payload, 2, sum);

    part.ptr  = payload;
    part.len  = (uint16_t)icmp_len;
    part.next = NULL;

    int ret = send_ipv4(self, ipv4->src, IPV4_PROTOCOL_ICMP, &part);
    if (ret != 0) LLOG(LLOG_ERROR, "process_icmp: send_ipv4 returned %d", ret);
    return 0;
}
