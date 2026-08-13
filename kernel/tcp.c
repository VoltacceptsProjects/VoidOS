#include "tcp.h"
#include "ip.h"
#include "serial.h"

#define TCP_MAX_CONNS 4
#define TCP_RECV_BUF 8192
#define TCP_MAX_SEGMENT 1024   /* well under NETDEV_MTU - sizeof(ip)-sizeof(tcp) */
#define TCP_RETRANSMIT_TICKS 50000  /* short_delay-scale ticks, not real time */
#define TCP_MAX_RETRIES 5

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; /* top 4 bits = header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

struct tcp_conn {
    int in_use;
    enum tcp_state state;
    uint8_t dst_ip[4];
    uint16_t dst_port;
    uint16_t local_port;

    uint32_t send_una;  /* oldest unacked sequence number */
    uint32_t send_next; /* next sequence number to use for new data */
    uint32_t recv_next; /* next expected sequence number from peer */

    /* single in-flight unacked outbound segment, kept for retransmit -
     * see tcp.h: this kernel never has more than one segment in
     * flight at a time */
    uint8_t pending_data[TCP_MAX_SEGMENT];
    uint32_t pending_len;
    uint32_t pending_seq;
    int pending_fin; /* 1 if the in-flight segment carries the FIN */
    uint32_t retries;
    uint32_t retransmit_countdown;

    uint8_t recv_buf[TCP_RECV_BUF];
    uint32_t recv_head, recv_tail, recv_count;

    int peer_fin_received;
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static uint16_t g_next_local_port = 49152;

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}

/* retransmit_countdown is a plain decrementing counter, not real
 * elapsed time - same "no timer driver yet" limitation iwlwifi.c's
 * short_delay() documents; tcp_poll() is expected to be called from a
 * busy loop the same way. */

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) g_conns[i].in_use = 0;
}

static struct tcp_conn* get_conn(tcp_handle_t h) {
    if (h < 0 || h >= TCP_MAX_CONNS) return 0;
    if (!g_conns[h].in_use) return 0;
    return &g_conns[h];
}

enum tcp_state tcp_state(tcp_handle_t h) {
    struct tcp_conn* c = get_conn(h);
    return c ? c->state : TCP_CLOSED;
}

static uint32_t pseudo_random_seq(void) {
    /* No RNG in this kernel; an ISS derived from a free-running "burn
     * cycles" counter is unique-enough per boot for a single-
     * connection-at-a-time client talking to one server, which is all
     * this needs. It is NOT cryptographically unpredictable - fine for
     * RFC 793 correctness, not something to rely on for TCP
     * sequence-prediction resistance. */
    static uint32_t counter = 0;
    counter += 12345;
    return counter ^ 0xA5A5A5A5u;
}

static int build_and_send(struct tcp_conn* c, uint8_t flags,
                           const uint8_t* data, uint32_t data_len) {
    uint8_t segment[sizeof(struct tcp_header) + TCP_MAX_SEGMENT];
    struct tcp_header* hdr = (struct tcp_header*)segment;

    hdr->src_port = be16(c->local_port);
    hdr->dst_port = be16(c->dst_port);
    hdr->seq = be32(c->send_next);
    hdr->ack = (flags & TCP_FLAG_ACK) ? be32(c->recv_next) : 0;
    hdr->data_offset = (uint8_t)((sizeof(struct tcp_header) / 4) << 4);
    hdr->flags = flags;
    hdr->window = be16(TCP_RECV_BUF > 65535 ? 65535 : TCP_RECV_BUF);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    for (uint32_t i = 0; i < data_len; i++) segment[sizeof(struct tcp_header) + i] = data[i];
    uint32_t total_len = sizeof(struct tcp_header) + data_len;

    uint16_t csum = ip_pseudo_checksum(ip_my_addr(), c->dst_ip, IP_PROTO_TCP,
                                        segment, total_len);
    hdr->checksum = be16(csum);

    return ip_send(c->dst_ip, IP_PROTO_TCP, segment, total_len);
}

tcp_handle_t tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port) {
    int slot = -1;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        serial_writestring("[tcp] connect: no free connection slot\n");
        return -1;
    }

    struct tcp_conn* c = &g_conns[slot];
    c->in_use = 1;
    for (int i = 0; i < 4; i++) c->dst_ip[i] = dst_ip[i];
    c->dst_port = dst_port;
    c->local_port = g_next_local_port++;
    if (g_next_local_port == 0) g_next_local_port = 49152;

    c->send_una = pseudo_random_seq();
    c->send_next = c->send_una;
    c->recv_next = 0;
    c->recv_head = c->recv_tail = c->recv_count = 0;
    c->peer_fin_received = 0;
    c->pending_fin = 0;
    c->retries = 0;
    c->retransmit_countdown = TCP_RETRANSMIT_TICKS;

    c->pending_len = 0;
    c->pending_seq = c->send_next;
    c->state = TCP_SYN_SENT;

    serial_writestring("[tcp] connect: sending SYN (local port ");
    serial_write_uint(c->local_port);
    serial_writestring(")\n");

    build_and_send(c, TCP_FLAG_SYN, 0, 0);
    c->send_next++; /* SYN consumes one sequence number */

    return (tcp_handle_t)slot;
}

void tcp_close(tcp_handle_t h) {
    struct tcp_conn* c = get_conn(h);
    if (!c) return;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return;

    uint8_t flags = TCP_FLAG_FIN | TCP_FLAG_ACK;
    build_and_send(c, flags, 0, 0);
    c->pending_seq = c->send_next;
    c->pending_len = 0;
    c->pending_fin = 1;
    c->send_next++; /* FIN consumes one sequence number */
    c->retries = 0;
    c->retransmit_countdown = TCP_RETRANSMIT_TICKS;

    c->state = (c->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
}

uint32_t tcp_send(tcp_handle_t h, const uint8_t* data, uint32_t len) {
    struct tcp_conn* c = get_conn(h);
    if (!c || c->state != TCP_ESTABLISHED) return 0;
    if (c->pending_len > 0 || c->pending_fin) return 0; /* one in-flight segment at a time */

    uint32_t send_len = len > TCP_MAX_SEGMENT ? TCP_MAX_SEGMENT : len;
    for (uint32_t i = 0; i < send_len; i++) c->pending_data[i] = data[i];
    c->pending_len = send_len;
    c->pending_seq = c->send_next;
    c->pending_fin = 0;
    c->retries = 0;
    c->retransmit_countdown = TCP_RETRANSMIT_TICKS;

    build_and_send(c, TCP_FLAG_ACK | TCP_FLAG_PSH, c->pending_data, send_len);
    c->send_next += send_len;
    return send_len;
}

uint32_t tcp_recv(tcp_handle_t h, uint8_t* out, uint32_t out_cap) {
    struct tcp_conn* c = get_conn(h);
    if (!c) return 0;
    uint32_t n = c->recv_count < out_cap ? c->recv_count : out_cap;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = c->recv_buf[c->recv_head];
        c->recv_head = (c->recv_head + 1) % TCP_RECV_BUF;
    }
    c->recv_count -= n;
    return n;
}

void tcp_poll(tcp_handle_t h) {
    struct tcp_conn* c = get_conn(h);
    if (!c) return;
    if (c->state == TCP_CLOSED || c->state == TCP_TIME_WAIT) return;
    if (c->pending_len == 0 && !c->pending_fin) return; /* nothing outstanding to retransmit */

    if (c->retransmit_countdown > 0) {
        c->retransmit_countdown--;
        return;
    }

    c->retries++;
    if (c->retries > TCP_MAX_RETRIES) {
        serial_writestring("[tcp] giving up after repeated retransmit timeouts "
                            "(no ACK ever arrived - see tcp.h, nothing feeds this "
                            "kernel real received frames yet)\n");
        c->state = TCP_CLOSED;
        c->pending_len = 0;
        c->pending_fin = 0;
        return;
    }

    serial_writestring("[tcp] retransmit\n");
    if (c->state == TCP_SYN_SENT) {
        build_and_send(c, TCP_FLAG_SYN, 0, 0);
    } else if (c->pending_fin) {
        uint8_t flags = TCP_FLAG_FIN | TCP_FLAG_ACK;
        build_and_send(c, flags, 0, 0);
    } else {
        build_and_send(c, TCP_FLAG_ACK | TCP_FLAG_PSH, c->pending_data, c->pending_len);
    }
    c->retransmit_countdown = TCP_RETRANSMIT_TICKS;
}

static struct tcp_conn* find_conn(const uint8_t src_ip[4], uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (!c->in_use) continue;
        if (c->local_port != local_port) continue;
        if (c->dst_port != remote_port) continue;
        if (c->dst_ip[0] != src_ip[0] || c->dst_ip[1] != src_ip[1] ||
            c->dst_ip[2] != src_ip[2] || c->dst_ip[3] != src_ip[3]) continue;
        return c;
    }
    return 0;
}

void tcp_handle_packet(const uint8_t src_ip[4], const uint8_t* segment, uint32_t len) {
    if (len < sizeof(struct tcp_header)) return;
    const struct tcp_header* hdr = (const struct tcp_header*)segment;

    uint16_t remote_port = be16(hdr->src_port);
    uint16_t local_port = be16(hdr->dst_port);
    struct tcp_conn* c = find_conn(src_ip, remote_port, local_port);
    if (!c) return; /* no matching connection - not RST'ing unsolicited segments is fine for a client-only stack */

    uint32_t header_len = ((hdr->data_offset >> 4) & 0x0F) * 4;
    if (header_len < sizeof(struct tcp_header) || header_len > len) return;
    const uint8_t* payload = segment + header_len;
    uint32_t payload_len = len - header_len;

    uint32_t seg_seq = be32(hdr->seq);
    uint32_t seg_ack = be32(hdr->ack);
    uint8_t flags = hdr->flags;

    if (flags & TCP_FLAG_RST) {
        serial_writestring("[tcp] connection reset by peer\n");
        c->state = TCP_CLOSED;
        c->pending_len = 0;
        c->pending_fin = 0;
        return;
    }

    switch (c->state) {
        case TCP_SYN_SENT: {
            if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && seg_ack == c->send_next) {
                c->recv_next = seg_seq + 1;
                c->send_una = seg_ack;
                c->pending_len = 0;
                c->pending_fin = 0;
                c->state = TCP_ESTABLISHED;
                serial_writestring("[tcp] connection established\n");
                build_and_send(c, TCP_FLAG_ACK, 0, 0);
            }
            break;
        }
        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_FIN_WAIT_2: {
            if ((flags & TCP_FLAG_ACK) && c->pending_len > 0 && seg_ack == c->send_next) {
                c->send_una = seg_ack;
                c->pending_len = 0; /* our outstanding data segment was ACKed */
            }
            if ((flags & TCP_FLAG_ACK) && c->pending_fin && seg_ack == c->send_next) {
                c->send_una = seg_ack;
                c->pending_fin = 0;
                if (c->state == TCP_FIN_WAIT_1) c->state = TCP_FIN_WAIT_2;
            }

            if (payload_len > 0 && seg_seq == c->recv_next) {
                uint32_t room = TCP_RECV_BUF - c->recv_count;
                uint32_t n = payload_len < room ? payload_len : room;
                for (uint32_t i = 0; i < n; i++) {
                    c->recv_buf[c->recv_tail] = payload[i];
                    c->recv_tail = (c->recv_tail + 1) % TCP_RECV_BUF;
                }
                c->recv_count += n;
                c->recv_next += n;
                build_and_send(c, TCP_FLAG_ACK, 0, 0);
            }

            if (flags & TCP_FLAG_FIN) {
                c->recv_next++;
                c->peer_fin_received = 1;
                build_and_send(c, TCP_FLAG_ACK, 0, 0);
                if (c->state == TCP_ESTABLISHED) c->state = TCP_CLOSE_WAIT;
                else if (c->state == TCP_FIN_WAIT_2) c->state = TCP_TIME_WAIT;
            }
            break;
        }
        case TCP_LAST_ACK: {
            if ((flags & TCP_FLAG_ACK) && seg_ack == c->send_next) {
                c->pending_fin = 0;
                c->state = TCP_CLOSED;
                serial_writestring("[tcp] closed\n");
            }
            break;
        }
        default:
            break;
    }
}
