/*
 * ns_udp.c — UDP send/receive for custom network stack
 *
 * Phase 5c: Builds and parses UDP datagrams. Port binding table
 * supports DHCP (port 68) and mDNS (port 5353).
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void udp_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

static void udp_memset(void *dst, int val, int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0) *d++ = (uint8_t)val;
}

/* ---- Port binding table ---- */

static struct {
    uint16_t port;
    ns_udp_callback cb;
} udp_bindings[NS_UDP_MAX_BINDINGS];

int ns_udp_bind(uint16_t port, ns_udp_callback cb) {
    for (int i = 0; i < NS_UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].port == 0) {
            udp_bindings[i].port = port;
            udp_bindings[i].cb = cb;
            return 0;
        }
    }
    return -1; /* table full */
}

void ns_udp_unbind(uint16_t port) {
    for (int i = 0; i < NS_UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].port == port) {
            udp_bindings[i].port = 0;
            udp_bindings[i].cb = (void *)0;
            return;
        }
    }
}

/* ---- Send UDP datagram ---- */

void ns_udp_send(const uint8_t *dst_ip, uint16_t src_port,
                 uint16_t dst_port, const uint8_t *data, int len) {
    /* Build 8-byte UDP header + payload */
    uint8_t pkt[1480];
    int total = 8 + len;
    if (total > (int)sizeof(pkt)) return;

    udp_memset(pkt, 0, 8);
    pkt[0] = (uint8_t)(src_port >> 8);
    pkt[1] = (uint8_t)(src_port);
    pkt[2] = (uint8_t)(dst_port >> 8);
    pkt[3] = (uint8_t)(dst_port);
    pkt[4] = (uint8_t)(total >> 8);
    pkt[5] = (uint8_t)(total);
    /* pkt[6-7] = checksum (0 = no checksum, valid for IPv4 UDP) */

    udp_memcpy(pkt + 8, data, len);

    ns_ip_send(dst_ip, 17, pkt, total);
}

/* ---- Handle incoming UDP datagram ---- */

void ns_udp_handle(const uint8_t *ip_payload, int len,
                   const uint8_t *src_ip) {
    if (len < 8) return;

    uint16_t src_port = ((uint16_t)ip_payload[0] << 8) | ip_payload[1];
    uint16_t dst_port = ((uint16_t)ip_payload[2] << 8) | ip_payload[3];
    uint16_t udp_len  = ((uint16_t)ip_payload[4] << 8) | ip_payload[5];

    if (udp_len < 8 || udp_len > len) return;

    const uint8_t *data = ip_payload + 8;
    int data_len = udp_len - 8;

    /* Find matching port binding */
    for (int i = 0; i < NS_UDP_MAX_BINDINGS; i++) {
        if (udp_bindings[i].port == dst_port && udp_bindings[i].cb) {
            udp_bindings[i].cb(data, data_len, src_ip, src_port);
            return;
        }
    }
}
