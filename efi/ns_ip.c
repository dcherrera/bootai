/*
 * ns_ip.c — IPv4 send/receive for custom network stack
 *
 * Phase 5c: Builds and parses IPv4 packets. Routes via ARP for
 * on-subnet addresses, through gateway for off-subnet.
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void ip_memset(void *dst, int val, int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0) *d++ = (uint8_t)val;
}

static void ip_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

static int ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- Internet checksum (RFC 1071) ---- */

static uint16_t ip_checksum(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len - 1; i += 2) {
        sum += ((uint16_t)data[i] << 8) | data[i + 1];
    }
    if (len & 1) {
        sum += (uint16_t)data[len - 1] << 8;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---- IP identification counter ---- */

static uint16_t ip_id_counter = 1;

/* ---- Send IPv4 packet ---- */

void ns_ip_send(const uint8_t *dst_ip, uint8_t protocol,
                const uint8_t *payload, int len) {
    ns_state *ns = ns_get_state();
    if (!ns->ip_ready) return;

    /* Determine next-hop: on-subnet → dst directly, off-subnet → gateway */
    int on_subnet = 1;
    for (int i = 0; i < 4; i++) {
        if ((dst_ip[i] & ns->subnet[i]) != (ns->ip[i] & ns->subnet[i])) {
            on_subnet = 0;
            break;
        }
    }

    /* Broadcast addresses go direct */
    int is_broadcast = (dst_ip[0] == 255 && dst_ip[1] == 255 &&
                        dst_ip[2] == 255 && dst_ip[3] == 255);

    const uint8_t *next_hop = on_subnet || is_broadcast ? dst_ip : ns->gateway;

    /* Resolve next-hop MAC via ARP */
    uint8_t dst_mac[6];
    if (is_broadcast) {
        ip_memset(dst_mac, 0xFF, 6);
    } else {
        /* Check if destination is multicast (224.0.0.0/4) */
        if ((dst_ip[0] & 0xF0) == 0xE0) {
            /* IPv4 multicast MAC: 01:00:5E + lower 23 bits of IP */
            dst_mac[0] = 0x01;
            dst_mac[1] = 0x00;
            dst_mac[2] = 0x5E;
            dst_mac[3] = dst_ip[1] & 0x7F;
            dst_mac[4] = dst_ip[2];
            dst_mac[5] = dst_ip[3];
        } else {
            if (ns_arp_resolve(next_hop, dst_mac) != 0) {
                return; /* ARP failed */
            }
        }
    }

    /* Build IPv4 header (20 bytes, no options) */
    uint8_t pkt[1518];
    int total = 20 + len;
    if (total > 1500) return; /* Too large */

    ip_memset(pkt, 0, 20);
    pkt[0] = 0x45;                          /* Version=4, IHL=5 */
    pkt[1] = 0x00;                          /* DSCP=0 */
    pkt[2] = (uint8_t)(total >> 8);         /* Total length */
    pkt[3] = (uint8_t)(total);
    pkt[4] = (uint8_t)(ip_id_counter >> 8); /* Identification */
    pkt[5] = (uint8_t)(ip_id_counter);
    ip_id_counter++;
    pkt[6] = 0x40;                          /* Flags: DF=1 */
    pkt[7] = 0x00;                          /* Fragment offset */
    pkt[8] = 64;                            /* TTL */
    pkt[9] = protocol;                      /* Protocol */
    /* pkt[10-11] = checksum (filled below) */
    ip_memcpy(pkt + 12, ns->ip, 4);        /* Source IP */
    ip_memcpy(pkt + 16, dst_ip, 4);        /* Destination IP */

    /* Compute header checksum */
    uint16_t csum = ip_checksum(pkt, 20);
    pkt[10] = (uint8_t)(csum >> 8);
    pkt[11] = (uint8_t)(csum);

    /* Append payload */
    ip_memcpy(pkt + 20, payload, len);

    /* Send as Ethernet frame with EtherType 0x0800 */
    ns_raw_send(dst_mac, 0x0800, pkt, total);
}

/* ---- Handle incoming IPv4 frame ---- */

void ns_ip_handle(const uint8_t *frame, int len) {
    /* frame is complete Ethernet frame (14-byte header + IP packet) */
    if (len < 14 + 20) return;

    const uint8_t *ip = frame + 14;

    /* Verify IPv4 */
    if ((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20) return;

    /* Total length */
    int total_len = ((int)ip[2] << 8) | ip[3];
    if (14 + total_len > len) return;

    /* Verify header checksum */
    uint16_t csum = ip_checksum(ip, ihl);
    if (csum != 0) return;

    /* Check destination: must be our IP or broadcast.
     * During DHCP (ip_ready=0), accept all packets — the server may
     * send to the offered IP rather than broadcast. */
    ns_state *ns = ns_get_state();
    if (ns->ip_ready) {
        const uint8_t *dst = ip + 16;
        int for_us = ip_eq(dst, ns->ip);
        int is_bcast = (dst[0] == 255 && dst[1] == 255 &&
                        dst[2] == 255 && dst[3] == 255);

        /* Also accept subnet broadcast (e.g. 192.168.1.255 on /24) */
        if (!for_us && !is_bcast) {
            int subnet_bcast = 1;
            for (int i = 0; i < 4; i++) {
                uint8_t expected = (ns->ip[i] & ns->subnet[i]) |
                                   (~ns->subnet[i] & 0xFF);
                if (dst[i] != expected) { subnet_bcast = 0; break; }
            }
            if (!subnet_bcast) return;
        }
    }

    /* Dispatch by protocol */
    uint8_t protocol = ip[9];
    const uint8_t *payload = ip + ihl;
    int payload_len = total_len - ihl;
    const uint8_t *src_ip = ip + 12;

    switch (protocol) {
    case 17:  /* UDP */
        ns_udp_handle(payload, payload_len, src_ip);
        break;
    case 6:   /* TCP */
        ns_tcp_handle(payload, payload_len, src_ip);
        break;
    default:
        break;
    }
}
