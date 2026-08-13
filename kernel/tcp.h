#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* Client-side TCP (RFC 793 subset): active open (SYN), ESTABLISHED
 * send/receive, and active close (FIN). No listen/passive open (this
 * kernel is never a server), no congestion control beyond "send one
 * segment, wait for its ACK before sending the next" (fine for a
 * request/response HTTP GET, not fine for anything throughput-
 * sensitive), no SACK, no window scaling. One connection at a time -
 * that's every real use in this kernel (appstore.c's HTTP fetches are
 * sequential).
 *
 * This kernel has no scheduler and no timer interrupt, so there is no
 * way to "block" a caller on a socket the way a hosted OS would.
 * Instead: tcp_connect() sends the SYN and returns a connection
 * handle immediately; the caller drives everything else by calling
 * tcp_poll() in a busy loop (same short_delay-and-repoll idiom
 * iwlwifi.c already uses for hardware bring-up) and checking
 * tcp_state() until it reaches TCP_ESTABLISHED, TCP_CLOSED (refused/
 * reset), or the caller's own timeout is hit.
 *
 * None of this has ever seen a real ACK from a real server, for the
 * reason explained in netdev.h: nothing yet drives netdev_rx() with
 * real received frames. The state machine, sequence-number
 * arithmetic, and checksums below are written to be correct against
 * RFC 793 and are exercised by test-harness-injected frames (see the
 * note in tcp.c), but "correct against the RFC" and "verified against
 * a real remote host" are different claims and this file only makes
 * the first one. */

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

typedef int32_t tcp_handle_t; /* -1 = invalid/no free connection slot */

void tcp_init(void);

/* Sends the initial SYN and returns a handle immediately (see the
 * blocking-model note above). dst_ip is a 4-byte IPv4 address. */
tcp_handle_t tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port);

/* Must be called periodically (a busy loop) to make any progress:
 * processes one iteration of retransmit timers. Actual segment
 * processing on receive happens synchronously inside
 * tcp_handle_packet(), called from ip_handle_frame() - tcp_poll()
 * only needs to drive time-based behavior (retransmits, TIME_WAIT
 * expiry). Returns nothing; check tcp_state() after calling. */
void tcp_poll(tcp_handle_t h);

enum tcp_state tcp_state(tcp_handle_t h);

/* Queues data for send once ESTABLISHED. Returns bytes accepted (may
 * be less than len - this kernel doesn't buffer more than one MTU's
 * worth in flight at a time, since HTTP GET requests are always
 * small). */
uint32_t tcp_send(tcp_handle_t h, const uint8_t* data, uint32_t len);

/* Copies any received-and-ACKed bytes into out (up to out_cap) and
 * removes them from the connection's receive buffer. Returns bytes
 * copied (0 if none available right now - not necessarily EOF; check
 * tcp_state() for that). */
uint32_t tcp_recv(tcp_handle_t h, uint8_t* out, uint32_t out_cap);

/* Begins active close (sends FIN). */
void tcp_close(tcp_handle_t h);

/* Called by ip_handle_frame() for IP_PROTO_TCP packets. src_ip is the
 * IP header's source address. */
void tcp_handle_packet(const uint8_t src_ip[4], const uint8_t* segment, uint32_t len);

#endif
