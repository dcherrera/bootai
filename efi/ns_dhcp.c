/*
 * ns_dhcp.c — DHCP client for custom network stack
 *
 * Phase 5c: Full DHCP exchange (discover→offer→request→ack).
 * Builds raw Ethernet+IP+UDP since we have no IP during discovery.
 * Blocking — runs in ~2-5 seconds.
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void dhcp_memset(void *dst, int val, int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0) *d++ = (uint8_t)val;
}

static void dhcp_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

/* ---- Print helpers ---- */

static void print_ip4(const uint8_t *ip) {
    for (int i = 0; i < 4; i++) {
        int v = ip[i];
        char buf[4];
        int pos = 0;
        if (v >= 100) buf[pos++] = '0' + v / 100;
        if (v >= 10) buf[pos++] = '0' + (v / 10) % 10;
        buf[pos++] = '0' + v % 10;
        buf[pos] = 0;
        efi_print_ascii(buf);
        if (i < 3) efi_print_ascii(".");
    }
}

/* ---- Internet checksum (RFC 1071) ---- */

static uint16_t dhcp_checksum(const uint8_t *data, int len) {
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

/* ---- DHCP message types ---- */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* DHCP magic cookie */
static const uint8_t dhcp_magic[4] = { 0x63, 0x82, 0x53, 0x63 };

/* Transaction ID */
static uint32_t dhcp_xid = 0x42424242;

/* ---- DHCP receive state (set by callback) ---- */

static int dhcp_got_response;
static uint8_t dhcp_offered_ip[4];
static uint8_t dhcp_server_ip[4];
static uint8_t dhcp_gateway[4];
static uint8_t dhcp_subnet[4];
static uint8_t dhcp_dns[4];
static uint8_t dhcp_msg_type;

/* ---- DHCP UDP callback (port 68) ---- */

static void dhcp_rx_callback(const uint8_t *data, int len,
                              const uint8_t *src_ip, uint16_t src_port) {
    (void)src_ip;
    (void)src_port;

    /* DHCP message minimum: 236 bytes header + 4 bytes magic cookie */
    if (len < 240) return;

    /* Check magic cookie at offset 236 */
    if (data[236] != dhcp_magic[0] || data[237] != dhcp_magic[1] ||
        data[238] != dhcp_magic[2] || data[239] != dhcp_magic[3])
        return;

    /* Check XID matches */
    uint32_t xid = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                   ((uint32_t)data[6] << 8) | data[7];
    if (xid != dhcp_xid) return;

    /* yiaddr = offered IP (offset 16) */
    dhcp_memcpy(dhcp_offered_ip, data + 16, 4);

    /* Parse DHCP options (TLV starting at offset 240) */
    dhcp_memset(dhcp_gateway, 0, 4);
    dhcp_memset(dhcp_subnet, 0, 4);
    dhcp_memset(dhcp_dns, 0, 4);
    dhcp_memset(dhcp_server_ip, 0, 4);
    dhcp_msg_type = 0;

    int pos = 240;
    while (pos < len) {
        uint8_t opt = data[pos++];
        if (opt == 255) break;  /* End */
        if (opt == 0) continue; /* Pad */

        if (pos >= len) break;
        uint8_t olen = data[pos++];
        if (pos + olen > len) break;

        switch (opt) {
        case 1:  /* Subnet mask */
            if (olen >= 4) dhcp_memcpy(dhcp_subnet, data + pos, 4);
            break;
        case 3:  /* Router/gateway */
            if (olen >= 4) dhcp_memcpy(dhcp_gateway, data + pos, 4);
            break;
        case 6:  /* DNS server */
            if (olen >= 4) dhcp_memcpy(dhcp_dns, data + pos, 4);
            break;
        case 53: /* Message type */
            if (olen >= 1) dhcp_msg_type = data[pos];
            break;
        case 54: /* Server identifier */
            if (olen >= 4) dhcp_memcpy(dhcp_server_ip, data + pos, 4);
            break;
        }
        pos += olen;
    }

    dhcp_got_response = 1;
}

/* ---- Build raw DHCP packet (Ethernet+IP+UDP+DHCP) ---- */

/*
 * Since we have no IP during DHCP, we build the complete
 * Ethernet + IP + UDP + DHCP message and send via ns_raw_send().
 *
 * Returns total DHCP payload length.
 */
static int dhcp_build_message(uint8_t *dhcp_buf, int buf_size,
                               uint8_t msg_type,
                               const uint8_t *requested_ip,
                               const uint8_t *server_id) {
    if (buf_size < 300) return 0;
    dhcp_memset(dhcp_buf, 0, buf_size);

    ns_state *ns = ns_get_state();
    int pos = 0;

    /* DHCP header (236 bytes) */
    dhcp_buf[pos++] = 1;    /* op: BOOTREQUEST */
    dhcp_buf[pos++] = 1;    /* htype: Ethernet */
    dhcp_buf[pos++] = 6;    /* hlen: 6 */
    dhcp_buf[pos++] = 0;    /* hops */

    /* XID */
    dhcp_buf[pos++] = (uint8_t)(dhcp_xid >> 24);
    dhcp_buf[pos++] = (uint8_t)(dhcp_xid >> 16);
    dhcp_buf[pos++] = (uint8_t)(dhcp_xid >> 8);
    dhcp_buf[pos++] = (uint8_t)(dhcp_xid);

    /* secs = 0, flags = 0x0000 (unicast — router sends OFFER to our MAC)
     * We clear the broadcast flag because on WiFi with PTK encryption,
     * unicast frames are encrypted with the pairwise key (which the firmware
     * can decrypt via MAP table), while broadcast frames use the GTK
     * (which may have a Key ID mismatch — AP broadcasts with kid=1 but
     * distributes kid=2 via EAPOL, so firmware drops them). */
    dhcp_buf[8] = 0; dhcp_buf[9] = 0;
    dhcp_buf[10] = 0x00; dhcp_buf[11] = 0x00;

    /* ciaddr = 0 (no current IP) */
    /* yiaddr, siaddr, giaddr = 0 */
    pos = 28;

    /* chaddr = our MAC */
    dhcp_memcpy(dhcp_buf + pos, ns->mac, 6);
    pos = 236;

    /* Magic cookie */
    dhcp_memcpy(dhcp_buf + pos, dhcp_magic, 4);
    pos += 4;

    /* Option 53: DHCP message type */
    dhcp_buf[pos++] = 53;
    dhcp_buf[pos++] = 1;
    dhcp_buf[pos++] = msg_type;

    /* Option 50: Requested IP (in REQUEST only) */
    if (requested_ip && msg_type == DHCP_REQUEST) {
        dhcp_buf[pos++] = 50;
        dhcp_buf[pos++] = 4;
        dhcp_memcpy(dhcp_buf + pos, requested_ip, 4);
        pos += 4;
    }

    /* Option 54: Server identifier (in REQUEST only) */
    if (server_id && msg_type == DHCP_REQUEST) {
        dhcp_buf[pos++] = 54;
        dhcp_buf[pos++] = 4;
        dhcp_memcpy(dhcp_buf + pos, server_id, 4);
        pos += 4;
    }

    /* Option 55: Parameter request list */
    dhcp_buf[pos++] = 55;
    dhcp_buf[pos++] = 4;
    dhcp_buf[pos++] = 1;   /* Subnet mask */
    dhcp_buf[pos++] = 3;   /* Router */
    dhcp_buf[pos++] = 6;   /* DNS */
    dhcp_buf[pos++] = 51;  /* Lease time */

    /* Option 255: End */
    dhcp_buf[pos++] = 255;

    return pos;
}

/*
 * Send a raw DHCP packet via broadcast (Ethernet+IP+UDP wrapper).
 * We can't use ns_ip_send() because we have no IP yet.
 */
static void dhcp_send_raw(const uint8_t *dhcp_msg, int dhcp_len) {
    /* Build IP+UDP+DHCP packet */
    int udp_len = 8 + dhcp_len;
    int ip_len = 20 + udp_len;
    uint8_t pkt[1500];

    if (ip_len > (int)sizeof(pkt)) return;
    dhcp_memset(pkt, 0, (int)sizeof(pkt));

    /* IPv4 header (20 bytes) */
    pkt[0] = 0x45;                         /* Version=4, IHL=5 */
    pkt[2] = (uint8_t)(ip_len >> 8);       /* Total length */
    pkt[3] = (uint8_t)(ip_len);
    pkt[6] = 0x40;                         /* DF=1 */
    pkt[8] = 64;                           /* TTL */
    pkt[9] = 17;                           /* Protocol: UDP */
    /* Source: 0.0.0.0 */
    /* Dest: 255.255.255.255 */
    pkt[16] = 255; pkt[17] = 255; pkt[18] = 255; pkt[19] = 255;

    /* IP checksum */
    uint16_t ip_csum = dhcp_checksum(pkt, 20);
    pkt[10] = (uint8_t)(ip_csum >> 8);
    pkt[11] = (uint8_t)(ip_csum);

    /* UDP header (8 bytes) at offset 20 */
    pkt[20] = 0x00; pkt[21] = 68;         /* Source port: 68 */
    pkt[22] = 0x00; pkt[23] = 67;         /* Dest port: 67 */
    pkt[24] = (uint8_t)(udp_len >> 8);    /* UDP length */
    pkt[25] = (uint8_t)(udp_len);
    /* UDP checksum: 0 (optional in IPv4) */

    /* DHCP payload */
    dhcp_memcpy(pkt + 28, dhcp_msg, dhcp_len);

    /* Send as broadcast Ethernet frame */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    ns_raw_send(bcast, 0x0800, pkt, ip_len);
}

/* ---- Wait for DHCP response ---- */

static void dhcp_print_dec(int v) {
    char buf[12];
    int p = 0;
    if (v >= 1000) buf[p++] = '0' + (v / 1000) % 10;
    if (v >= 100) buf[p++] = '0' + (v / 100) % 10;
    if (v >= 10) buf[p++] = '0' + (v / 10) % 10;
    buf[p++] = '0' + v % 10;
    buf[p] = 0;
    efi_print_ascii(buf);
}

static int dhcp_wait_response(uint8_t expected_type, int timeout_ms) {
    dhcp_got_response = 0;
    int rx_count = 0;
    int rx_before = ns_poll_rx_count();

    for (int t = 0; t < timeout_ms; t++) {
        ns_poll();

        if (dhcp_got_response) {
            rx_count++;
            if (dhcp_msg_type == expected_type) {
                return 0;
            }
        }
        dhcp_got_response = 0;

        gBS->Stall(1000); /* 1 ms */
    }

    /* Debug: show frame stats after timeout */
    int total_rx = ns_poll_rx_count() - rx_before;
    efi_print_ascii("  DHCP: timeout (");
    dhcp_print_dec(total_rx);
    efi_print_ascii(" frames rx, ");
    dhcp_print_dec(rx_count);
    efi_print_ascii(" DHCP)\r\n");

    return -1; /* timeout */
}

/* ---- Public API ---- */

int ns_dhcp_run(void) {
    ns_state *ns = ns_get_state();

    efi_print_ascii("  DHCP: starting...\r\n");

    /* Enable RX diagnostics for DHCP exchange */
    iwn_rx_set_debug(1);
    iwn_snp_debug_state();

    /* Bump XID for each run */
    dhcp_xid += 7;

    /* Bind port 68 to receive DHCP responses */
    ns_udp_bind(68, dhcp_rx_callback);

    /* ---- DISCOVER ---- */
    {
        uint8_t msg[512];
        int len = dhcp_build_message(msg, sizeof(msg), DHCP_DISCOVER,
                                     (void *)0, (void *)0);
        efi_print_ascii("  DHCP: DISCOVER (");
        dhcp_print_dec(len);
        efi_print_ascii(" bytes)\r\n");
        iwn_snp_debug_state();
        dhcp_send_raw(msg, len);
        efi_print_ascii("  DHCP: TX done\r\n");
        iwn_snp_debug_state();
    }

    /* Wait for OFFER (5s timeout) */
    if (dhcp_wait_response(DHCP_OFFER, 5000) != 0) {
        iwn_rx_print_debug();
        iwn_snp_debug_state();
        iwn_rx_set_debug(0);
        efi_print_ascii("  DHCP: no OFFER received (timeout)\r\n");
        ns_udp_unbind(68);
        return -1;
    }

    efi_print_ascii("  DHCP: OFFER ");
    print_ip4(dhcp_offered_ip);
    efi_print_ascii(" from ");
    print_ip4(dhcp_server_ip);
    efi_print_ascii("\r\n");

    /* ---- REQUEST ---- */
    {
        uint8_t msg[512];
        int len = dhcp_build_message(msg, sizeof(msg), DHCP_REQUEST,
                                     dhcp_offered_ip, dhcp_server_ip);
        efi_print_ascii("  DHCP: REQUEST\r\n");
        dhcp_send_raw(msg, len);
    }

    /* Wait for ACK (5s timeout) */
    if (dhcp_wait_response(DHCP_ACK, 5000) != 0) {
        iwn_rx_print_debug();
        iwn_rx_set_debug(0);
        efi_print_ascii("  DHCP: no ACK received (timeout)\r\n");
        ns_udp_unbind(68);
        return -1;
    }

    /* Store configuration */
    dhcp_memcpy(ns->ip, dhcp_offered_ip, 4);
    dhcp_memcpy(ns->gateway, dhcp_gateway, 4);
    dhcp_memcpy(ns->subnet, dhcp_subnet, 4);
    dhcp_memcpy(ns->dns, dhcp_dns, 4);
    ns->ip_ready = 1;

    ns_udp_unbind(68);

    efi_print_ascii("  DHCP: ACK — IP ");
    print_ip4(ns->ip);
    efi_print_ascii(" GW ");
    print_ip4(ns->gateway);
    efi_print_ascii(" Mask ");
    print_ip4(ns->subnet);
    efi_print_ascii("\r\n");

    iwn_rx_print_debug();
    iwn_rx_set_debug(0);

    return 0;
}
