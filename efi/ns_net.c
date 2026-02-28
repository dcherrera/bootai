/*
 * ns_net.c — Custom network stack: init, frame dispatch, raw TX
 *
 * Phase 5c: Core of the custom network stack. Uses direct IWN data
 * path functions (iwn_snp_direct_tx/rx) instead of the SNP protocol
 * function pointer table, which hangs on Dell E6510 firmware.
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void ns_memset(void *dst, int val, int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0) *d++ = (uint8_t)val;
}

static void ns_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

/* ---- Global state ---- */

static ns_state g_ns;

ns_state *ns_get_state(void) { return &g_ns; }

/* ---- Init ---- */

void ns_init(EFI_SIMPLE_NETWORK_PROTOCOL *snp) {
    ns_memset(&g_ns, 0, sizeof(g_ns));
    g_ns.snp = snp;

    /* Get MAC directly from IWN device (bypasses SNP function pointers) */
    iwn_snp_get_mac(g_ns.mac);

    efi_print_ascii("  ns: init MAC ");
    /* Print MAC */
    for (int i = 0; i < 6; i++) {
        uint8_t b = g_ns.mac[i];
        char hex[3];
        hex[0] = "0123456789ABCDEF"[b >> 4];
        hex[1] = "0123456789ABCDEF"[b & 0xF];
        hex[2] = 0;
        efi_print_ascii(hex);
        if (i < 5) efi_print_ascii(":");
    }
    efi_print_ascii("\r\n");
}

int ns_is_ready(void) {
    return g_ns.ip_ready;
}

void ns_get_ip(uint8_t *ip) {
    ns_memcpy(ip, g_ns.ip, 4);
}

void ns_get_mac(uint8_t *mac) {
    ns_memcpy(mac, g_ns.mac, 6);
}

/* ---- Raw frame send ---- */

void ns_raw_send(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, int len) {
    if (!g_ns.snp) return;

    /* Send directly via IWN (bypasses SNP Transmit function pointer).
     * iwn_snp_direct_tx() calls iwn_encap_tx() which handles
     * Ethernet→802.11 encapsulation + LLC/SNAP + CCMP. */
    iwn_snp_direct_tx(dst_mac, ethertype, payload, len);
}

/* ---- Frame receive + dispatch ---- */

static int g_ns_rx_count = 0;

int ns_poll_rx_count(void) { return g_ns_rx_count; }

void ns_poll(void) {
    if (!g_ns.snp) return;

    uint8_t frame[1518];
    int frame_len = 0;

    /* Receive directly via IWN (bypasses SNP Receive function pointer).
     * iwn_snp_direct_rx() calls iwn_rx_poll() + iwn_rx_dequeue()
     * which returns decapsulated Ethernet frames. */
    int ret = iwn_snp_direct_rx(frame, (int)sizeof(frame), &frame_len);
    if (ret != 0) return;

    if (frame_len < 14) return;

    g_ns_rx_count++;

    /* Parse EtherType at offset 12-13 */
    uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];

    switch (ethertype) {
    case 0x0806:  /* ARP */
        ns_arp_handle(frame, frame_len);
        break;
    case 0x0800:  /* IPv4 */
        ns_ip_handle(frame, frame_len);
        break;
    default:
        break;
    }
}
