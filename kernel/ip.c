#include "ip.h"
#include "arp.h"
#include "netdev.h"
#include "tcp.h"
#include "udp.h"
#include "serial.h"

struct ip_header {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

#define IP_MTU_PAYLOAD (NETDEV_MTU - (int)sizeof(struct ip_header))

static uint8_t g_my_ip[4];
static uint8_t g_gateway_ip[4];
static uint8_t g_netmask[4];
static uint16_t g_next_id = 1;

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

void ip_init(const uint8_t my_ip[4], const uint8_t gateway_ip[4], const uint8_t netmask[4]) {
    for (int i = 0; i < 4; i++) g_my_ip[i] = my_ip[i];
    for (int i = 0; i < 4; i++) g_gateway_ip[i] = gateway_ip[i];
    for (int i = 0; i < 4; i++) g_netmask[i] = netmask[i];

    struct netdev* dev = netdev_get();
    if (dev) {
        for (int i = 0; i < 4; i++) dev->ip[i] = my_ip[i];
        for (int i = 0; i < 4; i++) dev->gateway_ip[i] = gateway_ip[i];
        for (int i = 0; i < 4; i++) dev->netmask[i] = netmask[i];
    }
}

const uint8_t* ip_my_addr(void) { return g_my_ip; }

uint16_t ip_checksum(const void* data, uint32_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        uint16_t word = (uint16_t)((bytes[0] << 8) | bytes[1]);
        sum += word;
        bytes += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint16_t)(bytes[0] << 8);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t ip_pseudo_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             uint8_t protocol, const uint8_t* l4_data, uint32_t l4_len) {
    /* Build pseudo-header + segment into a scratch buffer and run the
     * same checksum routine over the whole thing. l4_len is bounded
     * by NETDEV_MTU elsewhere, so a fixed scratch buffer is safe. */
    uint8_t scratch[12 + NETDEV_MTU];
    if (l4_len > NETDEV_MTU) return 0xFFFF; /* would-never-validate sentinel */

    for (int i = 0; i < 4; i++) scratch[i] = src_ip[i];
    for (int i = 0; i < 4; i++) scratch[4 + i] = dst_ip[i];
    scratch[8] = 0;
    scratch[9] = protocol;
    scratch[10] = (uint8_t)(l4_len >> 8);
    scratch[11] = (uint8_t)(l4_len & 0xFF);
    for (uint32_t i = 0; i < l4_len; i++) scratch[12 + i] = l4_data[i];

    return ip_checksum(scratch, 12 + l4_len);
}

static int ip_equal(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int same_subnet(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & g_netmask[i]) != (g_my_ip[i] & g_netmask[i])) return 0;
    }
    return 1;
}

static int is_broadcast(const uint8_t ip[4]) {
    return ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
}

int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
            const uint8_t* payload, uint32_t len) {
    if ((int)len > IP_MTU_PAYLOAD) return 0; /* no fragmentation - see ip.h */

    uint8_t next_hop_mac[6];
    if (is_broadcast(dst_ip)) {
        /* Limited broadcast (255.255.255.255) - used before an IP is
         * configured (DHCP DISCOVER, see dhcp.c) so it deliberately
         * skips both routing-table and ARP lookups: there is no
         * meaningful "next hop" to resolve yet. */
        uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        for (int i = 0; i < 6; i++) next_hop_mac[i] = bcast_mac[i];
    } else {
        const uint8_t* next_hop_ip = same_subnet(dst_ip) ? dst_ip : g_gateway_ip;
        if (!arp_lookup(next_hop_ip, next_hop_mac)) {
            arp_request(next_hop_ip);
            serial_writestring("[ip] no ARP entry for next hop yet, sent request "
                                "and dropped this packet (caller/TCP layer is "
                                "expected to retransmit)\n");
            return 0;
        }
    }

    uint8_t packet[sizeof(struct ip_header) + NETDEV_MTU];
    struct ip_header* hdr = (struct ip_header*)packet;
    hdr->ver_ihl = (4 << 4) | 5; /* IPv4, 5 * 4 = 20-byte header, no options */
    hdr->dscp_ecn = 0;
    hdr->total_len = be16((uint16_t)(sizeof(struct ip_header) + len));
    hdr->id = be16(g_next_id++);
    hdr->flags_frag = be16(0x4000); /* DF (Don't Fragment) - honest given we never fragment */
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    for (int i = 0; i < 4; i++) hdr->src[i] = g_my_ip[i];
    for (int i = 0; i < 4; i++) hdr->dst[i] = dst_ip[i];
    hdr->checksum = be16(ip_checksum(hdr, sizeof(struct ip_header)));

    for (uint32_t i = 0; i < len; i++) packet[sizeof(struct ip_header) + i] = payload[i];

    return netdev_tx(next_hop_mac, 0x0800, packet, sizeof(struct ip_header) + len);
}

void ip_handle_frame(const uint8_t* payload, uint32_t len) {
    if (len < sizeof(struct ip_header)) return;
    const struct ip_header* hdr = (const struct ip_header*)payload;

    uint8_t version = hdr->ver_ihl >> 4;
    uint8_t ihl_words = hdr->ver_ihl & 0x0F;
    if (version != 4 || ihl_words < 5) return;
    uint32_t header_len = (uint32_t)ihl_words * 4;
    if (len < header_len) return;

    if (ip_checksum(hdr, header_len) != 0) {
        serial_writestring("[ip] bad header checksum, dropped\n");
        return;
    }

    /* Accept unicast-to-us or broadcast (DHCP OFFER/ACK arrive
     * broadcast before we have an IP to be unicast-addressed to). */
    if (!ip_equal(hdr->dst, g_my_ip) && !is_broadcast(hdr->dst)) return;

    uint16_t flags_frag = be16(hdr->flags_frag);
    if (flags_frag & 0x1FFF) {
        /* Non-zero fragment offset: this is a fragment. We never
         * reassemble - see ip.h - so drop it rather than hand a
         * partial payload up to TCP and pretend it's whole. */
        serial_writestring("[ip] fragmented packet, dropped (no reassembly)\n");
        return;
    }

    uint16_t total_len = be16(hdr->total_len);
    if (total_len > len) return; /* truncated frame */
    uint32_t l4_len = total_len - header_len;
    const uint8_t* l4 = payload + header_len;

    if (hdr->protocol == IP_PROTO_TCP) {
        tcp_handle_packet(hdr->src, l4, l4_len);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        udp_handle_packet(hdr->src, l4, l4_len);
    }
    /* ICMP etc: not needed for an HTTP GET, intentionally unhandled. */
}
