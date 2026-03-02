/*
 * ns_arp.c — ARP resolve + reply for custom network stack
 *
 * Phase 5c: 16-entry static ARP table. Resolves IP→MAC via broadcast
 * ARP request, responds to ARP requests for our IP.
 *
 * ARP packet format reused from iwn/iwn_data.c:331-358.
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void arp_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

static int arp_ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- ARP table ---- */

static ns_arp_entry arp_table[NS_ARP_TABLE_SIZE];

static ns_arp_entry *arp_lookup(const uint8_t *ip) {
    for (int i = 0; i < NS_ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_ip_eq(arp_table[i].ip, ip))
            return &arp_table[i];
    }
    return (void *)0;
}

static void arp_insert(const uint8_t *ip, const uint8_t *mac) {
    /* Update existing entry if present */
    ns_arp_entry *e = arp_lookup(ip);
    if (e) {
        arp_memcpy(e->mac, mac, 6);
        return;
    }
    /* Find empty slot */
    for (int i = 0; i < NS_ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_memcpy(arp_table[i].ip, ip, 4);
            arp_memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = 1;
            return;
        }
    }
    /* Table full — overwrite slot 0 */
    arp_memcpy(arp_table[0].ip, ip, 4);
    arp_memcpy(arp_table[0].mac, mac, 6);
    arp_table[0].valid = 1;
}

/* ---- Public: learn MAC from non-ARP frames ---- */

void ns_arp_learn(const uint8_t *ip, const uint8_t *mac) {
    /* Don't learn 0.0.0.0 or broadcast */
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) return;
    arp_insert(ip, mac);
}

/* ---- Public: dump ARP table for diagnostics ---- */

void ns_arp_dump(void) {
    efi_print_ascii("  ARP table (");
    /* Count valid entries */
    int count = 0;
    for (int i = 0; i < NS_ARP_TABLE_SIZE; i++)
        if (arp_table[i].valid) count++;

    char num[4];
    int ni = 0;
    if (count >= 10) num[ni++] = '0' + count / 10;
    num[ni++] = '0' + count % 10;
    num[ni] = '\0';
    efi_print_ascii(num);
    efi_print_ascii(" entries):\r\n");

    for (int i = 0; i < NS_ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) continue;
        efi_print_ascii("    ");
        /* Print IP */
        for (int j = 0; j < 4; j++) {
            int v = arp_table[i].ip[j];
            char d[4]; int di = 0;
            if (v >= 100) d[di++] = '0' + v / 100;
            if (v >= 10) d[di++] = '0' + (v / 10) % 10;
            d[di++] = '0' + v % 10;
            d[di] = '\0';
            efi_print_ascii(d);
            if (j < 3) efi_print_ascii(".");
        }
        efi_print_ascii(" -> ");
        /* Print MAC */
        for (int j = 0; j < 6; j++) {
            uint8_t b = arp_table[i].mac[j];
            char hex[3];
            hex[0] = "0123456789ABCDEF"[b >> 4];
            hex[1] = "0123456789ABCDEF"[b & 0xF];
            hex[2] = 0;
            efi_print_ascii(hex);
            if (j < 5) efi_print_ascii(":");
        }
        efi_print_ascii("\r\n");
    }
}

/* ---- Build ARP packet (28 bytes) ---- */

static void arp_build(uint8_t *pkt, uint16_t opcode,
                      const uint8_t *sender_mac, const uint8_t *sender_ip,
                      const uint8_t *target_mac, const uint8_t *target_ip) {
    /* Hardware type: Ethernet (1) */
    pkt[0] = 0x00; pkt[1] = 0x01;
    /* Protocol type: IPv4 (0x0800) */
    pkt[2] = 0x08; pkt[3] = 0x00;
    /* Hardware size: 6 */
    pkt[4] = 0x06;
    /* Protocol size: 4 */
    pkt[5] = 0x04;
    /* Opcode */
    pkt[6] = (uint8_t)(opcode >> 8);
    pkt[7] = (uint8_t)(opcode);
    /* Sender MAC + IP */
    arp_memcpy(pkt + 8, sender_mac, 6);
    arp_memcpy(pkt + 14, sender_ip, 4);
    /* Target MAC + IP */
    arp_memcpy(pkt + 18, target_mac, 6);
    arp_memcpy(pkt + 24, target_ip, 4);
}

/* ---- ARP resolve (blocking, 3s timeout) ---- */

int ns_arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    /* Check table first */
    ns_arp_entry *e = arp_lookup(ip);
    if (e) {
        arp_memcpy(mac_out, e->mac, 6);
        return 0;
    }

    ns_state *ns = ns_get_state();

    /* Send ARP request */
    uint8_t arp_pkt[28];
    uint8_t zero_mac[6] = {0};
    arp_build(arp_pkt, 1 /* request */, ns->mac, ns->ip, zero_mac, ip);

    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ns_raw_send(bcast, 0x0806, arp_pkt, 28);

    /* Poll for reply — 3 second timeout */
    for (int t = 0; t < 3000; t++) {
        ns_poll();

        e = arp_lookup(ip);
        if (e) {
            arp_memcpy(mac_out, e->mac, 6);
            return 0;
        }

        gBS->Stall(1000); /* 1 ms */
    }

    return -1; /* timeout */
}

/* ---- Handle incoming ARP frame ---- */

void ns_arp_handle(const uint8_t *frame, int len) {
    /* frame is a complete Ethernet frame (14-byte header + ARP payload) */
    if (len < 14 + 28) return;

    const uint8_t *arp = frame + 14;

    /* Verify: hardware=Ethernet, protocol=IPv4 */
    if (arp[0] != 0x00 || arp[1] != 0x01) return;
    if (arp[2] != 0x08 || arp[3] != 0x00) return;

    uint16_t opcode = ((uint16_t)arp[6] << 8) | arp[7];
    const uint8_t *sender_mac = arp + 8;
    const uint8_t *sender_ip = arp + 14;
    const uint8_t *target_ip = arp + 24;

    ns_state *ns = ns_get_state();

    /* Always learn sender's MAC→IP mapping */
    arp_insert(sender_ip, sender_mac);

    if (opcode == 1) {
        /* ARP Request — if asking for our IP, send reply */
        if (ns->ip_ready && arp_ip_eq(target_ip, ns->ip)) {
            uint8_t reply[28];
            arp_build(reply, 2 /* reply */, ns->mac, ns->ip,
                      sender_mac, sender_ip);
            ns_raw_send(sender_mac, 0x0806, reply, 28);
        }
    }
    /* ARP Reply (opcode==2) — already learned via arp_insert above */
}
