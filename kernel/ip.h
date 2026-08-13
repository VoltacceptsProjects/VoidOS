#ifndef IP_H
#define IP_H

#include <stdint.h>

#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

/* Minimal IPv4 (RFC 791): no fragmentation (this kernel never sends
 * or reassembles fragments - every packet it emits fits in one MTU,
 * and it drops incoming fragments rather than pretending to
 * reassemble them), no options handling beyond skipping IHL, no
 * routing table beyond "is it on my subnet, else send to the
 * gateway". That's everything an HTTP client over TCP actually needs. */

void ip_init(const uint8_t my_ip[4], const uint8_t gateway_ip[4], const uint8_t netmask[4]);

/* Called by netdev_rx() for ethertype 0x0800. Validates the header
 * checksum, drops anything not addressed to us, and dispatches by
 * protocol field to tcp_handle_packet() (UDP is parsed for
 * completeness but nothing in this kernel consumes it yet). */
void ip_handle_frame(const uint8_t* payload, uint32_t len);

/* Sends one IP packet. dst_ip is the final destination; this function
 * resolves the correct next-hop MAC itself (same-subnet vs gateway,
 * via ARP - see arp.h) so callers never need to know about that.
 * Returns 1 if handed to the link layer, 0 on failure (e.g. ARP not
 * resolved yet - see ip_send_retry note in ip.c). */
int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
            const uint8_t* payload, uint32_t len);

uint16_t ip_checksum(const void* data, uint32_t len);

/* TCP/UDP checksums cover a 12-byte IPv4 pseudo-header (src, dst,
 * zero, protocol, TCP-length) in addition to the segment itself -
 * RFC 793 sec 3.1. Callers in tcp.c build the segment first, then
 * call this over {pseudo-header, segment} to get the value that goes
 * in the segment's own checksum field (computed with that field
 * zeroed first, per the standard one's-complement-checksum dance). */
uint16_t ip_pseudo_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             uint8_t protocol, const uint8_t* l4_data, uint32_t l4_len);

const uint8_t* ip_my_addr(void);

#endif
