#include "netdev.h"
#include "arp.h"
#include "ip.h"
#include "serial.h"

static struct netdev g_dev;
static int g_dev_initialized = 0;

struct netdev* netdev_get(void) {
    return g_dev_initialized ? &g_dev : 0;
}

void netdev_register(const uint8_t mac[6], const struct netdev_ops* ops) {
    for (int i = 0; i < 6; i++) g_dev.mac[i] = mac[i];
    g_dev.ip[0] = g_dev.ip[1] = g_dev.ip[2] = g_dev.ip[3] = 0;
    g_dev.gateway_ip[0] = g_dev.gateway_ip[1] = g_dev.gateway_ip[2] = g_dev.gateway_ip[3] = 0;
    g_dev.netmask[0] = g_dev.netmask[1] = g_dev.netmask[2] = g_dev.netmask[3] = 0;
    g_dev.up = 0;
    g_dev.ops = ops;
    g_dev_initialized = 1;
}

int netdev_tx(const uint8_t dst_mac[6], uint16_t ethertype,
              const uint8_t* payload, uint32_t len) {
    struct netdev* dev = netdev_get();
    if (!dev || !dev->ops || !dev->ops->tx) return 0;
    if (len > NETDEV_MTU) return 0;

    /* Assemble a standard 14-byte Ethernet-II header + payload. This
     * is the frame shape every backing device (the null one today, a
     * real 802.11 data-path translation tomorrow) is expected to
     * accept from here on up - see netdev.h. */
    uint8_t frame[14 + NETDEV_MTU];
    for (int i = 0; i < 6; i++) frame[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) frame[6 + i] = dev->mac[i];
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)(ethertype & 0xFF);
    for (uint32_t i = 0; i < len; i++) frame[14 + i] = payload[i];

    return dev->ops->tx(frame, 14 + len);
}

void netdev_rx(const uint8_t* frame, uint32_t len) {
    if (len < 14) return;
    uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];
    const uint8_t* payload = frame + 14;
    uint32_t payload_len = len - 14;

    if (ethertype == 0x0806) {
        arp_handle_frame(frame + 6 /* src mac */, payload, payload_len);
    } else if (ethertype == 0x0800) {
        ip_handle_frame(payload, payload_len);
    }
    /* Anything else (IPv6, VLAN tags, etc.) is silently ignored - this
     * kernel doesn't need them for an HTTP GET. */
}

/* --- the null backing device --------------------------------------- */

static int null_tx(const uint8_t* frame, uint32_t len) {
    serial_writestring("[netdev] null device tx(): ethertype=");
    serial_write_hex(((uint32_t)frame[12] << 8) | frame[13]);
    serial_writestring(" len=");
    serial_write_uint(len);
    serial_writestring(" bytes - NOT put on any wire, no backing "
                        "hardware TX/RX datapath exists yet (see "
                        "netdev.h / iwlwifi.h)\n");
    return 1; /* "accepted", honestly meaning nothing more than that */
}

static const struct netdev_ops g_null_ops = { .tx = null_tx };

void netdev_null_init(void) {
    uint8_t fake_mac[6] = { 0x02, 0x00, 0x00, 0x56, 0x4f, 0x53 }; /* locally administered, spells "VOS" */
    netdev_register(fake_mac, &g_null_ops);
    serial_writestring("[netdev] registered null device - protocol layers "
                        "(ARP/IP/TCP) are live and correct, nothing behind "
                        "them can reach a real network yet\n");
}
