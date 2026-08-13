#include "udp.h"
#include "ip.h"
#include "serial.h"

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

#define UDP_RX_SLOTS 2
#define UDP_MAX_PAYLOAD 512 /* plenty for a DNS message over UDP (RFC 1035 sec 4.2.1: 512 without EDNS) */

struct udp_rx_slot {
    int valid;
    uint16_t local_port;
    uint8_t data[UDP_MAX_PAYLOAD];
    uint32_t len;
};

static struct udp_rx_slot g_rx[UDP_RX_SLOTS];

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

void udp_init(void) {
    for (int i = 0; i < UDP_RX_SLOTS; i++) g_rx[i].valid = 0;
}

int udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
             const uint8_t* payload, uint32_t len) {
    uint8_t datagram[sizeof(struct udp_header) + UDP_MAX_PAYLOAD];
    if (len > UDP_MAX_PAYLOAD) return 0;

    struct udp_header* hdr = (struct udp_header*)datagram;
    hdr->src_port = be16(src_port);
    hdr->dst_port = be16(dst_port);
    hdr->length = be16((uint16_t)(sizeof(struct udp_header) + len));
    hdr->checksum = 0;

    for (uint32_t i = 0; i < len; i++) datagram[sizeof(struct udp_header) + i] = payload[i];
    uint32_t total = sizeof(struct udp_header) + len;

    /* UDP checksum is optional over IPv4 (a zero value means "none
     * computed" per RFC 768); we compute it anyway since it's cheap
     * and catches corruption same as TCP's. */
    uint16_t csum = ip_pseudo_checksum(ip_my_addr(), dst_ip, IP_PROTO_UDP, datagram, total);
    if (csum == 0) csum = 0xFFFF; /* 0 is reserved to mean "no checksum" */
    hdr->checksum = be16(csum);

    return ip_send(dst_ip, IP_PROTO_UDP, datagram, total);
}

uint32_t udp_poll_recv(uint16_t local_port, uint8_t* out, uint32_t out_cap) {
    for (int i = 0; i < UDP_RX_SLOTS; i++) {
        if (g_rx[i].valid && g_rx[i].local_port == local_port) {
            uint32_t n = g_rx[i].len < out_cap ? g_rx[i].len : out_cap;
            for (uint32_t b = 0; b < n; b++) out[b] = g_rx[i].data[b];
            g_rx[i].valid = 0;
            return n;
        }
    }
    return 0;
}

void udp_handle_packet(const uint8_t src_ip[4], const uint8_t* segment, uint32_t len) {
    (void)src_ip;
    if (len < sizeof(struct udp_header)) return;
    const struct udp_header* hdr = (const struct udp_header*)segment;

    uint16_t dst_port = be16(hdr->dst_port);
    uint16_t total_len = be16(hdr->length);
    if (total_len < sizeof(struct udp_header) || total_len > len) return;
    uint32_t payload_len = total_len - sizeof(struct udp_header);
    const uint8_t* payload = segment + sizeof(struct udp_header);
    if (payload_len > UDP_MAX_PAYLOAD) return;

    for (int i = 0; i < UDP_RX_SLOTS; i++) {
        if (!g_rx[i].valid) {
            g_rx[i].valid = 1;
            g_rx[i].local_port = dst_port;
            g_rx[i].len = payload_len;
            for (uint32_t b = 0; b < payload_len; b++) g_rx[i].data[b] = payload[b];
            return;
        }
    }
    serial_writestring("[udp] no free rx slot, dropped a datagram\n");
}
