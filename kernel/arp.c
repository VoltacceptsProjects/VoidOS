#include "arp.h"
#include "netdev.h"
#include "serial.h"

#define ARP_HW_ETHERNET 1
#define ARP_PROTO_IPV4  0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

#define ARP_CACHE_SIZE 16

struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
};

static struct arp_entry g_cache[ARP_CACHE_SIZE];

static int ip_equal(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint16_t be16(uint16_t v) {
    /* This kernel targets x86, which is little-endian; network byte
     * order is big-endian, so every 16-bit protocol field needs an
     * explicit swap on the way in and out. */
    return (uint16_t)((v >> 8) | (v << 8));
}

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) g_cache[i].valid = 0;
}

static void cache_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && ip_equal(g_cache[i].ip, ip)) {
            for (int j = 0; j < 6; j++) g_cache[i].mac[j] = mac[j];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_cache[i].valid) {
            for (int j = 0; j < 4; j++) g_cache[i].ip[j] = ip[j];
            for (int j = 0; j < 6; j++) g_cache[i].mac[j] = mac[j];
            g_cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: evict slot 0. A real LRU isn't worth it for a
     * single-host workload (one AP, one HTTP server). */
    for (int j = 0; j < 4; j++) g_cache[0].ip[j] = ip[j];
    for (int j = 0; j < 6; j++) g_cache[0].mac[j] = mac[j];
    g_cache[0].valid = 1;
}

int arp_lookup(const uint8_t target_ip[4], uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && ip_equal(g_cache[i].ip, target_ip)) {
            for (int j = 0; j < 6; j++) out_mac[j] = g_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

void arp_request(const uint8_t target_ip[4]) {
    struct netdev* dev = netdev_get();
    if (!dev) return;

    struct arp_packet pkt;
    pkt.htype = be16(ARP_HW_ETHERNET);
    pkt.ptype = be16(ARP_PROTO_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = be16(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) pkt.sha[i] = dev->mac[i];
    for (int i = 0; i < 4; i++) pkt.spa[i] = dev->ip[i];
    for (int i = 0; i < 6; i++) pkt.tha[i] = 0; /* unknown - that's the question */
    for (int i = 0; i < 4; i++) pkt.tpa[i] = target_ip[i];

    uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    netdev_tx(broadcast, 0x0806, (const uint8_t*)&pkt, sizeof(pkt));
}

void arp_handle_frame(const uint8_t src_mac[6], const uint8_t* payload, uint32_t len) {
    (void)src_mac;
    if (len < sizeof(struct arp_packet)) return;
    const struct arp_packet* pkt = (const struct arp_packet*)payload;

    if (be16(pkt->htype) != ARP_HW_ETHERNET || be16(pkt->ptype) != ARP_PROTO_IPV4) return;
    if (pkt->hlen != 6 || pkt->plen != 4) return;

    uint16_t oper = be16(pkt->oper);
    struct netdev* dev = netdev_get();
    if (!dev) return;

    if (oper == ARP_OP_REPLY) {
        cache_insert(pkt->spa, pkt->sha);
        serial_writestring("[arp] learned a MAC from a reply\n");
        return;
    }

    if (oper == ARP_OP_REQUEST) {
        cache_insert(pkt->spa, pkt->sha); /* learn the requester while we're here */
        if (!ip_equal(pkt->tpa, dev->ip) || dev->ip[0] == 0) return; /* not for us / no IP yet */

        struct arp_packet reply;
        reply.htype = be16(ARP_HW_ETHERNET);
        reply.ptype = be16(ARP_PROTO_IPV4);
        reply.hlen = 6;
        reply.plen = 4;
        reply.oper = be16(ARP_OP_REPLY);
        for (int i = 0; i < 6; i++) reply.sha[i] = dev->mac[i];
        for (int i = 0; i < 4; i++) reply.spa[i] = dev->ip[i];
        for (int i = 0; i < 6; i++) reply.tha[i] = pkt->sha[i];
        for (int i = 0; i < 4; i++) reply.tpa[i] = pkt->spa[i];
        netdev_tx(pkt->sha, 0x0806, (const uint8_t*)&reply, sizeof(reply));
    }
}
