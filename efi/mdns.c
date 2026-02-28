/*
 * mdns.c — mDNS/Bonjour responder for BootAI (RFC 6762)
 *
 * Advertises "bootai.local" on the local network via multicast DNS
 * so clients can discover the HTTP server without knowing the IP.
 *
 * Uses UEFI UDP4 protocol to join 224.0.0.251:5353 multicast group,
 * send announcements, and respond to queries.
 */

#include "efi.h"
#include "ns_net.h"

/* ---- Constants ---- */

#define MDNS_PORT       5353
#define MDNS_TTL        120     /* seconds */
#define MDNS_MCAST_ADDR { .Addr = {224, 0, 0, 251} }

/* ---- Freestanding helpers (same as net.c — minimal, no shared lib) ---- */

static void mdns_memset(void *dst, int val, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

static void mdns_memcpy(void *dst, const void *src, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* Case-insensitive byte compare */
static int mdns_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* ---- Module state ---- */

static EFI_SERVICE_BINDING_PROTOCOL *mdns_udp4_sb;
static EFI_UDP4_PROTOCOL            *mdns_udp4;
static EFI_HANDLE                    mdns_child;
static EFI_IPv4_ADDRESS              mdns_our_ip;
static EFI_IPv4_ADDRESS              mdns_mcast = MDNS_MCAST_ADDR;
static int                           mdns_running;

/* Async receive state */
static EFI_UDP4_COMPLETION_TOKEN     mdns_rx_token;
static int                           mdns_rx_pending;

/* EVT_NOTIFY_SIGNAL for completion tokens */
#define EVT_NOTIFY_SIGNAL  0x00000200
#define TPL_CALLBACK       8

static void EFIAPI mdns_empty_notify(EFI_EVENT event, void *ctx) {
    (void)event; (void)ctx;
}

/* ---- DNS name: "\x06bootai\x05local\x00" ---- */

static const uint8_t mdns_name[] = {
    0x06, 'b','o','o','t','a','i',
    0x05, 'l','o','c','a','l',
    0x00
};
#define MDNS_NAME_LEN 14  /* sizeof mdns_name */

/* ---- Build mDNS A-record response ---- */

/*
 * Builds a response packet advertising our IP for bootai.local.
 * Returns packet length.
 *
 * Packet layout:
 *   Header (12 bytes): ID=0, Flags=0x8400, ANCOUNT=1
 *   Answer: bootai.local A record, TTL, 4-byte IP
 */
static int mdns_build_response(uint8_t *buf, int buf_size, uint32_t ttl) {
    if (buf_size < 40) return 0;

    int pos = 0;
    mdns_memset(buf, 0, (UINTN)buf_size);

    /* Header */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* ID = 0 */
    buf[pos++] = 0x84; buf[pos++] = 0x00;  /* Flags: QR=1, AA=1 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* QDCOUNT = 0 */
    buf[pos++] = 0x00; buf[pos++] = 0x01;  /* ANCOUNT = 1 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* NSCOUNT = 0 */
    buf[pos++] = 0x00; buf[pos++] = 0x00;  /* ARCOUNT = 0 */

    /* Answer: NAME */
    mdns_memcpy(buf + pos, mdns_name, MDNS_NAME_LEN);
    pos += MDNS_NAME_LEN;

    /* TYPE = A (1) */
    buf[pos++] = 0x00; buf[pos++] = 0x01;

    /* CLASS = IN (1) | cache-flush (0x8000) */
    buf[pos++] = 0x80; buf[pos++] = 0x01;

    /* TTL (4 bytes, network order) */
    buf[pos++] = (uint8_t)(ttl >> 24);
    buf[pos++] = (uint8_t)(ttl >> 16);
    buf[pos++] = (uint8_t)(ttl >> 8);
    buf[pos++] = (uint8_t)(ttl);

    /* RDLENGTH = 4 */
    buf[pos++] = 0x00; buf[pos++] = 0x04;

    /* RDATA = our IP */
    buf[pos++] = mdns_our_ip.Addr[0];
    buf[pos++] = mdns_our_ip.Addr[1];
    buf[pos++] = mdns_our_ip.Addr[2];
    buf[pos++] = mdns_our_ip.Addr[3];

    return pos;
}

/* ---- Custom stack mDNS receive callback ---- */

static int mdns_ns_got_query;
static uint8_t mdns_ns_rx_buf[512];
static int mdns_ns_rx_len;

static void mdns_ns_callback(const uint8_t *data, int len,
                              const uint8_t *src_ip, uint16_t src_port) {
    (void)src_ip; (void)src_port;
    if (len > (int)sizeof(mdns_ns_rx_buf)) len = (int)sizeof(mdns_ns_rx_buf);
    mdns_memcpy(mdns_ns_rx_buf, data, (UINTN)len);
    mdns_ns_rx_len = len;
    mdns_ns_got_query = 1;
}

/* ---- Send a UDP packet to multicast ---- */

static EFI_STATUS mdns_send(const uint8_t *data, int len) {
    /* Custom stack path (Dell) */
    if (ns_is_ready()) {
        uint8_t mcast_ip[4] = {224, 0, 0, 251};
        ns_udp_send(mcast_ip, MDNS_PORT, MDNS_PORT, data, len);
        return EFI_SUCCESS;
    }
    EFI_UDP4_SESSION_DATA session;
    mdns_memset(&session, 0, sizeof(session));
    mdns_memcpy(&session.DestinationAddress, &mdns_mcast, 4);
    session.DestinationPort = MDNS_PORT;

    EFI_UDP4_TRANSMIT_DATA tx;
    mdns_memset(&tx, 0, sizeof(tx));
    tx.UdpSessionData = &session;
    tx.DataLength = (uint32_t)len;
    tx.FragmentCount = 1;
    tx.FragmentTable[0].FragmentLength = (uint32_t)len;
    tx.FragmentTable[0].FragmentBuffer = (void *)data;

    EFI_UDP4_COMPLETION_TOKEN token;
    mdns_memset(&token, 0, sizeof(token));

    EFI_STATUS status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                          mdns_empty_notify, 0, &token.Event);
    if (status != EFI_SUCCESS) return status;

    token.Status = EFI_NOT_READY;
    token.Packet.TxData = &tx;

    status = mdns_udp4->Transmit(mdns_udp4, &token);
    if (status != EFI_SUCCESS) {
        gBS->CloseEvent(token.Event);
        return status;
    }

    /* Poll for completion (up to 2s) */
    for (int i = 0; i < 20; i++) {
        mdns_udp4->Poll(mdns_udp4);
        if (token.Status != EFI_NOT_READY) break;
        gBS->Stall(100000);
    }
    gBS->CloseEvent(token.Event);

    return token.Status;
}

/* ---- Check if packet is a query for bootai.local A record ---- */

static int mdns_is_our_query(const uint8_t *pkt, int len) {
    if (len < 12) return 0;

    /* QR must be 0 (query) */
    if (pkt[2] & 0x80) return 0;

    /* QDCOUNT >= 1 */
    uint16_t qdcount = ((uint16_t)pkt[4] << 8) | pkt[5];
    if (qdcount == 0) return 0;

    /* Walk question section */
    int pos = 12;

    for (uint16_t q = 0; q < qdcount && pos < len; q++) {
        /* Compare QNAME against bootai.local (case-insensitive) */
        int name_start = pos;

        /* Walk labels to find name length */
        int name_len = 0;
        int tmp = pos;
        while (tmp < len) {
            uint8_t llen = pkt[tmp];
            if (llen == 0) { tmp++; name_len = tmp - name_start; break; }
            if ((llen & 0xC0) == 0xC0) { tmp += 2; name_len = tmp - name_start; break; }
            tmp += 1 + llen;
        }

        /* Check if this QNAME matches bootai.local */
        int match = 0;
        if (name_len == MDNS_NAME_LEN) {
            match = 1;
            for (int i = 0; i < MDNS_NAME_LEN && match; i++) {
                if (mdns_name[i] != (uint8_t)mdns_lower(pkt[name_start + i]))
                    match = 0;
            }
        }

        pos = name_start + name_len;
        if (pos + 4 > len) return 0;

        uint16_t qtype  = ((uint16_t)pkt[pos] << 8) | pkt[pos + 1];
        uint16_t qclass = ((uint16_t)pkt[pos + 2] << 8) | pkt[pos + 3];
        pos += 4;

        /* Match: QTYPE A(1) or ANY(255), QCLASS IN(1) or ANY(255) */
        if (match && (qtype == 1 || qtype == 255) &&
            ((qclass & 0x7FFF) == 1 || (qclass & 0x7FFF) == 255))
            return 1;
    }

    return 0;
}

/* ---- Issue async Receive if not already pending ---- */

static void mdns_issue_receive(void) {
    if (mdns_rx_pending || !mdns_udp4) return;

    mdns_memset(&mdns_rx_token, 0, sizeof(mdns_rx_token));

    EFI_STATUS status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                          mdns_empty_notify, 0,
                                          &mdns_rx_token.Event);
    if (status != EFI_SUCCESS) return;

    mdns_rx_token.Status = EFI_NOT_READY;
    mdns_rx_token.Packet.RxData = 0;

    status = mdns_udp4->Receive(mdns_udp4, &mdns_rx_token);
    if (status != EFI_SUCCESS) {
        gBS->CloseEvent(mdns_rx_token.Event);
        return;
    }

    mdns_rx_pending = 1;
}

/* ---- Public API ---- */

EFI_STATUS ba_mdns_init(void) {
    EFI_STATUS status;

    mdns_running = 0;
    mdns_rx_pending = 0;
    mdns_udp4_sb = 0;
    mdns_udp4 = 0;
    mdns_child = 0;

    /* Get our IP */
    status = efi_get_local_ip(&mdns_our_ip);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  mDNS: no local IP\r\n");
        return status;
    }

    /* Custom stack path (Dell) — use ns_udp for mDNS */
    if (ns_is_ready()) {
        ns_udp_bind(MDNS_PORT, mdns_ns_callback);
        mdns_running = 1;
        mdns_ns_got_query = 0;
        efi_print_ascii("  mDNS: initialized via custom stack (bootai.local)\r\n");
        return EFI_SUCCESS;
    }

    /* Locate UDP4 Service Binding */
    UINTN handle_count = 0;
    EFI_HANDLE *handles = 0;

    status = gBS->LocateHandleBuffer(2 /* SEARCH_BY_PROTOCOL */,
        &gEfiUdp4ServiceBindingGuid, 0, &handle_count, &handles);
    if (status != EFI_SUCCESS || handle_count == 0) {
        efi_print_ascii("  mDNS: UDP4 not found\r\n");
        return EFI_NOT_FOUND;
    }

    gBS->HandleProtocol(handles[0], &gEfiUdp4ServiceBindingGuid,
                        (void **)&mdns_udp4_sb);
    gBS->FreePool(handles);

    if (!mdns_udp4_sb) return EFI_NOT_FOUND;

    /* Create child */
    status = mdns_udp4_sb->CreateChild(mdns_udp4_sb, &mdns_child);
    if (status != EFI_SUCCESS) return status;

    status = gBS->HandleProtocol(mdns_child, &gEfiUdp4ProtocolGuid,
                                 (void **)&mdns_udp4);
    if (status != EFI_SUCCESS) {
        mdns_udp4_sb->DestroyChild(mdns_udp4_sb, mdns_child);
        return status;
    }

    /* Configure UDP4 for mDNS: port 5353, AllowDuplicatePort, TTL=255 */
    EFI_UDP4_CONFIG_DATA cfg;
    mdns_memset(&cfg, 0, sizeof(cfg));
    cfg.AcceptBroadcast = 1;
    cfg.AcceptAnyPort = 0;
    cfg.AllowDuplicatePort = 1;
    cfg.TypeOfService = 0;
    cfg.TimeToLive = 255;
    cfg.DoNotFragment = 0;
    cfg.UseDefaultAddress = 1;
    cfg.StationPort = MDNS_PORT;

    /* Retry on EFI_NO_MAPPING */
    for (int retry = 0; retry < 20; retry++) {
        status = mdns_udp4->Configure(mdns_udp4, &cfg);
        if (status != EFI_NO_MAPPING) break;
        gBS->Stall(250000);
    }

    if (status != EFI_SUCCESS) {
        efi_print_ascii("  mDNS: UDP4 configure failed\r\n");
        mdns_udp4_sb->DestroyChild(mdns_udp4_sb, mdns_child);
        mdns_udp4 = 0;
        return status;
    }

    /* Join multicast group 224.0.0.251 */
    status = mdns_udp4->Groups(mdns_udp4, 1, &mdns_mcast);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  mDNS: multicast join failed\r\n");
        /* Non-fatal — we can still send, just won't receive queries on some firmware */
    }

    mdns_running = 1;
    efi_print_ascii("  mDNS: initialized (bootai.local)\r\n");
    return EFI_SUCCESS;
}

void ba_mdns_announce(void) {
    if (!mdns_running || !mdns_udp4) return;

    uint8_t pkt[64];
    int len = mdns_build_response(pkt, sizeof(pkt), MDNS_TTL);
    if (len <= 0) return;

    efi_print_ascii("  mDNS: announcing bootai.local\r\n");

    /* RFC 6762 section 8.3: send twice, 1 second apart */
    mdns_send(pkt, len);
    gBS->Stall(1000000); /* 1 second */
    mdns_send(pkt, len);

    /* Start listening for queries */
    mdns_issue_receive();
}

void ba_mdns_poll(void) {
    if (!mdns_running) return;

    /* Custom stack path (Dell) */
    if (ns_is_ready()) {
        ns_poll();
        if (mdns_ns_got_query) {
            mdns_ns_got_query = 0;
            if (mdns_is_our_query(mdns_ns_rx_buf, mdns_ns_rx_len)) {
                uint8_t resp[64];
                int rlen = mdns_build_response(resp, sizeof(resp), MDNS_TTL);
                if (rlen > 0) mdns_send(resp, rlen);
            }
        }
        return;
    }

    if (!mdns_udp4) return;

    /* Drive the network stack */
    mdns_udp4->Poll(mdns_udp4);

    if (!mdns_rx_pending) {
        mdns_issue_receive();
        return;
    }

    /* Check if receive completed */
    if (mdns_rx_token.Status == EFI_NOT_READY) return;

    mdns_rx_pending = 0;

    if (mdns_rx_token.Status == EFI_SUCCESS && mdns_rx_token.Packet.RxData) {
        EFI_UDP4_RECEIVE_DATA *rx = mdns_rx_token.Packet.RxData;

        if (rx->FragmentCount > 0 && rx->FragmentTable[0].FragmentBuffer) {
            const uint8_t *data = (const uint8_t *)rx->FragmentTable[0].FragmentBuffer;
            int data_len = (int)rx->FragmentTable[0].FragmentLength;

            if (mdns_is_our_query(data, data_len)) {
                /* Respond with our A record */
                uint8_t resp[64];
                int rlen = mdns_build_response(resp, sizeof(resp), MDNS_TTL);
                if (rlen > 0) mdns_send(resp, rlen);
            }
        }

        /* Recycle the receive buffer */
        if (rx->RecycleSignal) {
            gBS->SignalEvent(rx->RecycleSignal);
        }
    }

    gBS->CloseEvent(mdns_rx_token.Event);

    /* Issue next receive */
    mdns_issue_receive();
}

void ba_mdns_stop(void) {
    if (!mdns_running) return;

    /* Send goodbye (TTL=0) */
    uint8_t pkt[64];
    int len = mdns_build_response(pkt, sizeof(pkt), 0);
    if (len > 0) mdns_send(pkt, len);

    /* Custom stack path (Dell) */
    if (ns_is_ready()) {
        ns_udp_unbind(MDNS_PORT);
        mdns_running = 0;
        efi_print_ascii("  mDNS: stopped\r\n");
        return;
    }

    if (!mdns_udp4) return;

    /* Cancel pending receive */
    if (mdns_rx_pending) {
        gBS->CloseEvent(mdns_rx_token.Event);
        mdns_rx_pending = 0;
    }

    /* Leave multicast group */
    mdns_udp4->Groups(mdns_udp4, 0, &mdns_mcast);

    /* Destroy child */
    mdns_udp4_sb->DestroyChild(mdns_udp4_sb, mdns_child);
    mdns_udp4 = 0;
    mdns_child = 0;
    mdns_running = 0;

    efi_print_ascii("  mDNS: stopped\r\n");
}
