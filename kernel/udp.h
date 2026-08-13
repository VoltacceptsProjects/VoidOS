#ifndef UDP_H
#define UDP_H

#include <stdint.h>

/* Just enough UDP (RFC 768) to run a DNS query - see dns.h. No socket
 * abstraction, no demux table: one outstanding request/response at a
 * time is all this kernel ever needs UDP for. */

void udp_init(void);

int udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
             const uint8_t* payload, uint32_t len);

/* Polls for one UDP datagram addressed to local_port from dst_ip,
 * copying its payload into out (up to out_cap). Returns bytes copied,
 * or 0 if nothing has arrived. Must be called from a busy loop, same
 * model as tcp_poll() - see tcp.h. */
uint32_t udp_poll_recv(uint16_t local_port, uint8_t* out, uint32_t out_cap);

/* Called by ip_handle_frame() for IP_PROTO_UDP packets. */
void udp_handle_packet(const uint8_t src_ip[4], const uint8_t* segment, uint32_t len);

#endif
