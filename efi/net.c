/*
 * net.c — UEFI networking: DHCP, TCP4, DNS4, HTTP/1.0
 *
 * Uses IP4Config2 for DHCP, TCP4 via service binding for connections,
 * DNS4 for hostname resolution, and raw HTTP/1.0 over TCP4.
 *
 * All async operations use CreateEvent + WaitForEvent completion tokens.
 */

#include "efi.h"

/* ---- Protocol GUIDs ---- */

EFI_GUID gEfiTcp4ServiceBindingGuid = {
    0x00720665, 0x67EB, 0x4a99,
    {0xBA, 0xF7, 0xD3, 0xC3, 0x3A, 0x1C, 0x7C, 0xC9}
};

EFI_GUID gEfiTcp4ProtocolGuid = {
    0x65530BC7, 0xA359, 0x410f,
    {0xB0, 0x10, 0x5A, 0xAD, 0xC7, 0xEC, 0x2B, 0x62}
};

EFI_GUID gEfiIp4Config2ProtocolGuid = {
    0x5B446ED1, 0xE30B, 0x4faa,
    {0x87, 0x1A, 0x36, 0x54, 0xEC, 0xA3, 0x60, 0x80}
};

EFI_GUID gEfiDns4ServiceBindingGuid = {
    0xB625B186, 0xE063, 0x44f7,
    {0x89, 0x05, 0x6A, 0x74, 0xDC, 0x6F, 0x52, 0xB4}
};

EFI_GUID gEfiDns4ProtocolGuid = {
    0xAE3D28CC, 0xE05B, 0x4FA1,
    {0xA0, 0x11, 0x7E, 0xB5, 0x5A, 0x3F, 0x14, 0x01}
};

EFI_GUID gEfiUdp4ServiceBindingGuid = {
    0x83f01464, 0x99bd, 0x45e5,
    {0xb3, 0x83, 0xaf, 0x63, 0x05, 0xd8, 0xe9, 0xe6}
};

EFI_GUID gEfiUdp4ProtocolGuid = {
    0x3ad9df29, 0x4501, 0x478d,
    {0xb1, 0xf8, 0x7f, 0x7f, 0xe7, 0x0e, 0x50, 0xf3}
};

/* QEMU user-mode DNS forwarder */
static EFI_IPv4_ADDRESS dns_server = { .Addr = {10, 0, 2, 3} };

/* ---- Static globals ---- */

static EFI_IP4_CONFIG2_PROTOCOL    *ip4_config2;
static EFI_SERVICE_BINDING_PROTOCOL *tcp4_sb;
static int net_available;

/* ---- Freestanding helper functions ---- */

static void ba_memset(void *dst, int val, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

static void ba_memcpy(void *dst, const void *src, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static UINTN ba_strlen(const char *s) {
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

static int ba_strncmp(const char *a, const char *b, UINTN n) {
    for (UINTN i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static void ba_strncpy(char *dst, const char *src, UINTN n) {
    UINTN i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int ba_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

/* ---- Print helpers for debugging ---- */

static void print_ip(EFI_IPv4_ADDRESS *ip) {
    char buf[20];
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        int v = ip->Addr[i];
        if (v >= 100) buf[pos++] = '0' + v / 100;
        if (v >= 10)  buf[pos++] = '0' + (v / 10) % 10;
        buf[pos++] = '0' + v % 10;
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    efi_print_ascii(buf);
}

static void print_status(const char *label, EFI_STATUS status) {
    efi_print_ascii("  ");
    efi_print_ascii(label);
    efi_print_ascii(": 0x");
    /* Simple hex print */
    char hex[17];
    for (int i = 15; i >= 0; i--) {
        int nibble = (int)(status >> (i * 4)) & 0xF;
        hex[15 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    hex[16] = '\0';
    /* Skip leading zeros */
    char *p = hex;
    while (*p == '0' && *(p + 1) != '\0') p++;
    efi_print_ascii(p);
    efi_print_ascii("\r\n");
}

/* ---- EVT_NOTIFY_SIGNAL for completion tokens ---- */
#define EVT_TIMER                 0x80000000
#define EVT_RUNTIME               0x40000000
#define EVT_NOTIFY_WAIT           0x00000100
#define EVT_NOTIFY_SIGNAL         0x00000200

#define TPL_CALLBACK              8

/* Empty notify function for CreateEvent */
static void EFIAPI empty_notify(EFI_EVENT event, void *ctx) {
    (void)event;
    (void)ctx;
}

/* ---- Connect all controllers (bind drivers to devices) ---- */

#define SEARCH_ALL_HANDLES 0
#define SEARCH_BY_PROTOCOL 2

static void connect_all_controllers(void) {
    UINTN handle_count = 0;
    EFI_HANDLE *handles = 0;

    EFI_STATUS status = gBS->LocateHandleBuffer(
        SEARCH_ALL_HANDLES, 0, 0, &handle_count, &handles);
    if (status != EFI_SUCCESS) {
        print_status("LocateHandleBuffer", status);
        return;
    }

    /* Connect all drivers recursively */
    for (UINTN i = 0; i < handle_count; i++) {
        gBS->ConnectController(handles[i], 0, 0, 1);
    }

    gBS->FreePool(handles);

    /* Second pass: connecting may have created new child handles */
    status = gBS->LocateHandleBuffer(
        SEARCH_ALL_HANDLES, 0, 0, &handle_count, &handles);
    if (status == EFI_SUCCESS) {
        for (UINTN i = 0; i < handle_count; i++) {
            gBS->ConnectController(handles[i], 0, 0, 1);
        }
        gBS->FreePool(handles);
    }
}

/* ---- Network initialization ---- */

static void efi_net_post_dhcp_connect(void);

EFI_STATUS efi_net_init(void) {
    EFI_STATUS status;

    ip4_config2 = 0;
    tcp4_sb = 0;
    net_available = 0;

    if (!gBS) return EFI_NOT_READY;

    /* Connect all controllers so network stack binds to NIC */
    connect_all_controllers();
    connect_all_controllers();

    /* Locate IP4Config2 for DHCP — try LocateHandleBuffer first */
    {
        UINTN handle_count = 0;
        EFI_HANDLE *handles = 0;
        status = gBS->LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiIp4Config2ProtocolGuid,
                                         0, &handle_count, &handles);
        if (status == EFI_SUCCESS && handle_count > 0) {
            gBS->HandleProtocol(handles[0], &gEfiIp4Config2ProtocolGuid, (void **)&ip4_config2);
            gBS->FreePool(handles);
        }
    }

    if (!ip4_config2) {
        /* Fallback to LocateProtocol */
        status = gBS->LocateProtocol(&gEfiIp4Config2ProtocolGuid, 0, (void **)&ip4_config2);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("  IP4Config2 not found\r\n");
            ip4_config2 = 0;
            return status;
        }
    }

    /* Set DHCP policy */
    uint32_t policy = IP4_CONFIG2_POLICY_DHCP;
    status = ip4_config2->SetData(ip4_config2, IP4_CONFIG2_DATA_TYPE_POLICY,
                                  sizeof(policy), &policy);
    if (status != EFI_SUCCESS && status != EFI_UNSUPPORTED) {
        print_status("DHCP SetData", status);
    }

    /* Locate TCP4 Service Binding */
    {
        UINTN handle_count = 0;
        EFI_HANDLE *handles = 0;
        status = gBS->LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiTcp4ServiceBindingGuid,
                                         0, &handle_count, &handles);
        if (status == EFI_SUCCESS && handle_count > 0) {
            gBS->HandleProtocol(handles[0], &gEfiTcp4ServiceBindingGuid, (void **)&tcp4_sb);
            gBS->FreePool(handles);
        }
    }

    if (!tcp4_sb) {
        status = gBS->LocateProtocol(&gEfiTcp4ServiceBindingGuid, 0, (void **)&tcp4_sb);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("  TCP4 ServiceBinding not found\r\n");
            tcp4_sb = 0;
            return status;
        }
    }

    /* Wait for DHCP to complete (poll up to 8 seconds) */
    efi_print_ascii("  Waiting for DHCP");
    for (int i = 0; i < 80; i++) {
        gBS->Stall(100000); /* 100ms */

        /* Try to read interface info to see if IP is assigned */
        UINTN data_size = 0;
        status = ip4_config2->GetData(ip4_config2, IP4_CONFIG2_DATA_TYPE_INTERFACEINFO,
                                      &data_size, 0);
        if (status == EFI_BUFFER_TOO_SMALL && data_size > 0) {
            /* IP is assigned — DHCP succeeded */
            efi_print_ascii(" OK\r\n");
            /* Connect upper-layer protocols now that IP is ready */
            efi_net_post_dhcp_connect();
            net_available = 1;
            return EFI_SUCCESS;
        }

        if (i % 10 == 9) efi_print_ascii(".");
    }

    efi_print_ascii(" timeout\r\n");
    /* Even on timeout, mark as available — TCP4 Configure with UseDefaultAddress
       may still work if DHCP completes later */
    net_available = 1;
    return EFI_SUCCESS;
}

/* Post-DHCP connect pass to bind DNS4 and other upper-layer protocols */
static void efi_net_post_dhcp_connect(void) {
    connect_all_controllers();
}

int efi_net_available(void) {
    return net_available;
}

/* ---- TCP4 connection management ---- */

typedef struct {
    EFI_TCP4_PROTOCOL *tcp4;
    EFI_HANDLE child_handle;
} tcp4_conn_t;

static EFI_STATUS tcp4_connect(EFI_IPv4_ADDRESS *host_ip, uint16_t port, tcp4_conn_t *conn) {
    EFI_STATUS status;

    if (!tcp4_sb || !gBS) return EFI_NOT_READY;

    ba_memset(conn, 0, sizeof(*conn));

    /* Create a TCP4 child instance */
    status = tcp4_sb->CreateChild(tcp4_sb, &conn->child_handle);
    if (status != EFI_SUCCESS) {
        print_status("CreateChild", status);
        return status;
    }

    /* Get the TCP4 protocol from the child handle */
    status = gBS->HandleProtocol(conn->child_handle, &gEfiTcp4ProtocolGuid,
                                 (void **)&conn->tcp4);
    if (status != EFI_SUCCESS) {
        print_status("HandleProtocol TCP4", status);
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return status;
    }

    /* Configure TCP4 */
    EFI_TCP4_CONFIG_DATA config;
    ba_memset(&config, 0, sizeof(config));
    config.TypeOfService = 0;
    config.TimeToLive = 64;
    config.AccessPoint.UseDefaultAddress = 1;
    ba_memset(&config.AccessPoint.StationAddress, 0, sizeof(EFI_IPv4_ADDRESS));
    ba_memset(&config.AccessPoint.SubnetMask, 0, sizeof(EFI_IPv4_ADDRESS));
    config.AccessPoint.StationPort = 0; /* ephemeral */
    ba_memcpy(&config.AccessPoint.RemoteAddress, host_ip, sizeof(EFI_IPv4_ADDRESS));
    config.AccessPoint.RemotePort = port;
    config.AccessPoint.ActiveFlag = 1;
    config.ControlOption = 0;

    /* Retry Configure if EFI_NO_MAPPING (DHCP not ready) */
    for (int retry = 0; retry < 20; retry++) {
        status = conn->tcp4->Configure(conn->tcp4, &config);
        if (status != EFI_NO_MAPPING) break;
        gBS->Stall(250000); /* 250ms */
    }

    if (status != EFI_SUCCESS) {
        print_status("TCP4 Configure", status);
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return status;
    }

    /* Create completion event for connect */
    EFI_TCP4_CONNECTION_TOKEN connect_token;
    ba_memset(&connect_token, 0, sizeof(connect_token));

    status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              empty_notify, 0,
                              &connect_token.CompletionToken.Event);
    if (status != EFI_SUCCESS) {
        print_status("CreateEvent", status);
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return status;
    }

    /* Initiate connection */
    connect_token.CompletionToken.Status = EFI_NOT_READY;
    status = conn->tcp4->Connect(conn->tcp4, &connect_token);
    if (status != EFI_SUCCESS) {
        print_status("TCP4 Connect", status);
        gBS->CloseEvent(connect_token.CompletionToken.Event);
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return status;
    }

    /* Poll for connection to complete (WaitForEvent doesn't work with EVT_NOTIFY_SIGNAL) */
    for (int wait = 0; wait < 200; wait++) { /* up to 20 seconds */
        if (connect_token.CompletionToken.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }
    gBS->CloseEvent(connect_token.CompletionToken.Event);

    if (connect_token.CompletionToken.Status == EFI_NOT_READY) {
        efi_print_ascii("  Connect timed out\r\n");
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return EFI_TIMEOUT;
    }

    if (connect_token.CompletionToken.Status != EFI_SUCCESS) {
        print_status("Connect result", connect_token.CompletionToken.Status);
        tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
        return connect_token.CompletionToken.Status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS tcp4_send(tcp4_conn_t *conn, const void *data, UINTN len) {
    EFI_STATUS status;

    if (!conn->tcp4 || !gBS) return EFI_NOT_READY;

    /* Build transmit data */
    EFI_TCP4_TRANSMIT_DATA tx_data;
    ba_memset(&tx_data, 0, sizeof(tx_data));
    tx_data.Push = 1;
    tx_data.Urgent = 0;
    tx_data.DataLength = (uint32_t)len;
    tx_data.FragmentCount = 1;
    tx_data.FragmentTable[0].FragmentLength = (uint32_t)len;
    tx_data.FragmentTable[0].FragmentBuffer = (void *)data;

    /* IO token */
    EFI_TCP4_IO_TOKEN tx_token;
    ba_memset(&tx_token, 0, sizeof(tx_token));
    tx_token.Packet.TxData = &tx_data;

    status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              empty_notify, 0,
                              &tx_token.CompletionToken.Event);
    if (status != EFI_SUCCESS) return status;

    tx_token.CompletionToken.Status = EFI_NOT_READY;
    status = conn->tcp4->Transmit(conn->tcp4, &tx_token);
    if (status != EFI_SUCCESS) {
        gBS->CloseEvent(tx_token.CompletionToken.Event);
        return status;
    }

    /* Poll for completion */
    for (int wait = 0; wait < 100; wait++) { /* up to 10 seconds */
        if (tx_token.CompletionToken.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }
    gBS->CloseEvent(tx_token.CompletionToken.Event);

    if (tx_token.CompletionToken.Status == EFI_NOT_READY) return EFI_TIMEOUT;
    return tx_token.CompletionToken.Status;
}

static INTN tcp4_recv(tcp4_conn_t *conn, void *buf, UINTN max_len) {
    EFI_STATUS status;

    if (!conn->tcp4 || !gBS) return -1;

    /* Build receive data */
    EFI_TCP4_RECEIVE_DATA rx_data;
    ba_memset(&rx_data, 0, sizeof(rx_data));
    rx_data.DataLength = (uint32_t)max_len;
    rx_data.FragmentCount = 1;
    rx_data.FragmentTable[0].FragmentLength = (uint32_t)max_len;
    rx_data.FragmentTable[0].FragmentBuffer = buf;

    /* IO token */
    EFI_TCP4_IO_TOKEN rx_token;
    ba_memset(&rx_token, 0, sizeof(rx_token));
    rx_token.Packet.RxData = &rx_data;

    status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              empty_notify, 0,
                              &rx_token.CompletionToken.Event);
    if (status != EFI_SUCCESS) return -1;

    rx_token.CompletionToken.Status = EFI_NOT_READY;
    status = conn->tcp4->Receive(conn->tcp4, &rx_token);
    if (status != EFI_SUCCESS) {
        gBS->CloseEvent(rx_token.CompletionToken.Event);
        /* Connection closed or error */
        if (status == EFI_CONNECTION_FIN || status == EFI_CONNECTION_RESET) return 0;
        return -1;
    }

    /* Poll for completion */
    for (int wait = 0; wait < 100; wait++) { /* up to 10 seconds */
        if (rx_token.CompletionToken.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }
    gBS->CloseEvent(rx_token.CompletionToken.Event);

    if (rx_token.CompletionToken.Status == EFI_NOT_READY) return -1;

    if (rx_token.CompletionToken.Status != EFI_SUCCESS) {
        if (rx_token.CompletionToken.Status == EFI_CONNECTION_FIN ||
            rx_token.CompletionToken.Status == EFI_CONNECTION_RESET)
            return 0;
        return -1;
    }

    /* Return actual bytes received from the fragment */
    return (INTN)rx_data.FragmentTable[0].FragmentLength;
}

static void tcp4_close(tcp4_conn_t *conn) {
    if (!conn->tcp4 || !gBS) return;

    EFI_TCP4_CLOSE_TOKEN close_token;
    ba_memset(&close_token, 0, sizeof(close_token));

    EFI_STATUS status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                         empty_notify, 0,
                                         &close_token.CompletionToken.Event);
    if (status == EFI_SUCCESS) {
        close_token.CompletionToken.Status = EFI_NOT_READY;
        status = conn->tcp4->Close(conn->tcp4, &close_token);
        if (status == EFI_SUCCESS) {
            for (int wait = 0; wait < 50; wait++) { /* up to 5 seconds */
                if (close_token.CompletionToken.Status != EFI_NOT_READY) break;
                gBS->Stall(100000);
            }
        }
        gBS->CloseEvent(close_token.CompletionToken.Event);
    }

    /* Destroy the child handle */
    tcp4_sb->DestroyChild(tcp4_sb, conn->child_handle);
    conn->tcp4 = 0;
    conn->child_handle = 0;
}

/* ---- URL parsing ---- */

static void parse_url(const char *url, char *host, int host_max,
                      uint16_t *port, char *path, int path_max) {
    /* Defaults */
    ba_strncpy(host, "", host_max);
    *port = 80;
    ba_strncpy(path, "/", path_max);

    /* Skip "http://" */
    if (ba_strncmp(url, "http://", 7) == 0) {
        url += 7;
    }

    /* Extract host (up to ':', '/', or end) */
    int hi = 0;
    while (*url && *url != ':' && *url != '/' && hi < host_max - 1) {
        host[hi++] = *url++;
    }
    host[hi] = '\0';

    /* Extract port if present */
    if (*url == ':') {
        url++;
        *port = (uint16_t)ba_atoi(url);
        while (*url >= '0' && *url <= '9') url++;
    }

    /* Extract path */
    if (*url == '/') {
        ba_strncpy(path, url, path_max);
    }
}

/* ---- IPv4 address parsing ---- */

static int parse_ipv4(const char *str, EFI_IPv4_ADDRESS *out) {
    ba_memset(out, 0, sizeof(*out));

    for (int i = 0; i < 4; i++) {
        if (*str < '0' || *str > '9') return 0;
        int val = 0;
        while (*str >= '0' && *str <= '9') {
            val = val * 10 + (*str - '0');
            str++;
        }
        if (val > 255) return 0;
        out->Addr[i] = (uint8_t)val;
        if (i < 3) {
            if (*str != '.') return 0;
            str++;
        }
    }
    return 1;
}

/* ---- DNS resolution ---- */

/* DNS4 config data structure (must match EDK2 layout) */
typedef struct {
    UINTN            DnsServerListCount; /* UINTN = 64-bit on x86_64 */
    EFI_IPv4_ADDRESS *DnsServerList;
    BOOLEAN          UseDefaultSetting;
    BOOLEAN          EnableDnsCache;
    uint8_t          Protocol;       /* 17 = UDP */
    EFI_IPv4_ADDRESS StationIp;
    EFI_IPv4_ADDRESS SubnetMask;
    uint16_t         LocalPort;
    uint32_t         RetryCount;
    uint32_t         RetryInterval;
} EFI_DNS4_CONFIG_DATA;

/* ---- DNS wire format (RFC 1035) ---- */

static uint16_t dns_query_id = 1;

/*
 * Build a DNS A-record query for hostname into buf.
 * Returns query length, or 0 on error.
 */
static int dns_build_query(const char *hostname, uint8_t *buf, int buf_size) {
    UINTN hlen = ba_strlen(hostname);
    if (hlen == 0 || hlen > 253) return 0;

    /* Estimate: 12 (header) + hlen + 2 (label overhead) + 1 (null) + 4 (qtype/qclass) */
    int max_needed = 12 + (int)hlen + 2 + 1 + 4;
    if (max_needed > buf_size) return 0;

    ba_memset(buf, 0, (UINTN)buf_size);
    int pos = 0;

    /* Header: 12 bytes */
    uint16_t id = dns_query_id++;
    buf[pos++] = (uint8_t)(id >> 8);
    buf[pos++] = (uint8_t)(id & 0xFF);
    buf[pos++] = 0x01;  /* flags: RD=1 (recursion desired) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;  /* QDCOUNT = 1 */
    buf[pos++] = 0x01;
    buf[pos++] = 0x00;  /* ANCOUNT = 0 */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;  /* NSCOUNT = 0 */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;  /* ARCOUNT = 0 */
    buf[pos++] = 0x00;

    /* Question: label-encoded hostname */
    const char *p = hostname;
    while (*p) {
        /* Find next dot or end */
        const char *dot = p;
        while (*dot && *dot != '.') dot++;

        int label_len = (int)(dot - p);
        if (label_len == 0 || label_len > 63) return 0;

        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) {
            buf[pos++] = (uint8_t)p[i];
        }
        p = dot;
        if (*p == '.') p++;
    }
    buf[pos++] = 0x00;  /* terminating null label */

    /* QTYPE = A (1) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    /* QCLASS = IN (1) */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;

    return pos;
}

/*
 * Parse a DNS response and extract the first A record.
 * Returns 1 on success, 0 on failure.
 */
static int dns_parse_response(const uint8_t *buf, int len, EFI_IPv4_ADDRESS *out_ip) {
    if (len < 12) return 0;

    /* Check QR=1 (response) */
    if (!(buf[2] & 0x80)) return 0;

    /* Check RCODE=0 (no error) */
    if ((buf[3] & 0x0F) != 0) return 0;

    /* ANCOUNT */
    uint16_t ancount = ((uint16_t)buf[6] << 8) | buf[7];
    if (ancount == 0) return 0;

    /* Skip header (12 bytes) */
    int pos = 12;

    /* Skip question section: walk labels, then skip QTYPE + QCLASS (4 bytes) */
    /* QDCOUNT */
    uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
    for (uint16_t q = 0; q < qdcount; q++) {
        /* Walk labels */
        while (pos < len) {
            uint8_t llen = buf[pos];
            if (llen == 0) { pos++; break; }
            if ((llen & 0xC0) == 0xC0) { pos += 2; break; } /* compression pointer */
            pos += 1 + llen;
        }
        pos += 4; /* QTYPE + QCLASS */
    }

    /* Walk answer RRs */
    for (uint16_t a = 0; a < ancount && pos < len; a++) {
        /* Skip NAME (handle compression pointers) */
        while (pos < len) {
            uint8_t llen = buf[pos];
            if (llen == 0) { pos++; break; }
            if ((llen & 0xC0) == 0xC0) { pos += 2; break; }
            pos += 1 + llen;
        }

        if (pos + 10 > len) return 0;

        uint16_t rtype  = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        uint16_t rclass = ((uint16_t)buf[pos + 2] << 8) | buf[pos + 3];
        /* skip TTL (4 bytes) */
        uint16_t rdlength = ((uint16_t)buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;

        if (rtype == 1 && rclass == 1 && rdlength == 4) {
            /* A record — extract IPv4 */
            if (pos + 4 > len) return 0;
            out_ip->Addr[0] = buf[pos];
            out_ip->Addr[1] = buf[pos + 1];
            out_ip->Addr[2] = buf[pos + 2];
            out_ip->Addr[3] = buf[pos + 3];
            return 1;
        }

        pos += rdlength;
    }

    return 0;
}

/* ---- UDP4 DNS resolver ---- */

static EFI_STATUS udp4_dns_resolve(const char *hostname, EFI_IPv4_ADDRESS *out_ip) {
    EFI_STATUS status;

    if (!gBS) return EFI_NOT_READY;

    /* Locate UDP4 Service Binding */
    UINTN handle_count = 0;
    EFI_HANDLE *handles = 0;
    EFI_SERVICE_BINDING_PROTOCOL *udp4_sb = 0;

    status = gBS->LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiUdp4ServiceBindingGuid,
                                     0, &handle_count, &handles);
    if (status != EFI_SUCCESS || handle_count == 0) {
        efi_print_ascii("  UDP4 ServiceBinding not found\r\n");
        return EFI_NOT_FOUND;
    }

    gBS->HandleProtocol(handles[0], &gEfiUdp4ServiceBindingGuid, (void **)&udp4_sb);
    gBS->FreePool(handles);

    if (!udp4_sb) return EFI_NOT_FOUND;

    /* Create child instance */
    EFI_HANDLE child_handle = 0;
    status = udp4_sb->CreateChild(udp4_sb, &child_handle);
    if (status != EFI_SUCCESS) {
        print_status("UDP4 CreateChild", status);
        return status;
    }

    EFI_UDP4_PROTOCOL *udp4 = 0;
    status = gBS->HandleProtocol(child_handle, &gEfiUdp4ProtocolGuid, (void **)&udp4);
    if (status != EFI_SUCCESS) {
        print_status("HandleProtocol UDP4", status);
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    /* Configure UDP4 */
    EFI_UDP4_CONFIG_DATA udp_config;
    ba_memset(&udp_config, 0, sizeof(udp_config));
    udp_config.AcceptBroadcast = 0;
    udp_config.AcceptPromiscuous = 0;
    udp_config.AcceptAnyPort = 0;
    udp_config.AllowDuplicatePort = 0;
    udp_config.TypeOfService = 0;
    udp_config.TimeToLive = 64;
    udp_config.DoNotFragment = 0;
    udp_config.ReceiveTimeout = 0;
    udp_config.TransmitTimeout = 0;
    udp_config.UseDefaultAddress = 1;
    ba_memset(&udp_config.StationAddress, 0, sizeof(EFI_IPv4_ADDRESS));
    ba_memset(&udp_config.SubnetMask, 0, sizeof(EFI_IPv4_ADDRESS));
    udp_config.StationPort = 0; /* ephemeral */
    ba_memset(&udp_config.RemoteAddress, 0, sizeof(EFI_IPv4_ADDRESS));
    udp_config.RemotePort = 0;

    /* Retry on EFI_NO_MAPPING (DHCP not ready) */
    for (int retry = 0; retry < 20; retry++) {
        status = udp4->Configure(udp4, &udp_config);
        if (status != EFI_NO_MAPPING) break;
        gBS->Stall(250000); /* 250ms */
    }

    if (status != EFI_SUCCESS) {
        print_status("UDP4 Configure", status);
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    /* Build DNS query */
    uint8_t query_buf[512];
    int query_len = dns_build_query(hostname, query_buf, sizeof(query_buf));
    if (query_len == 0) {
        efi_print_ascii("  Failed to build DNS query\r\n");
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return EFI_DEVICE_ERROR;
    }

    /* --- Transmit DNS query --- */
    EFI_UDP4_SESSION_DATA tx_session;
    ba_memset(&tx_session, 0, sizeof(tx_session));
    ba_memcpy(&tx_session.DestinationAddress, &dns_server, sizeof(EFI_IPv4_ADDRESS));
    tx_session.DestinationPort = 53;

    EFI_UDP4_TRANSMIT_DATA tx_data;
    ba_memset(&tx_data, 0, sizeof(tx_data));
    tx_data.UdpSessionData = &tx_session;
    tx_data.GatewayAddress = 0;
    tx_data.DataLength = (uint32_t)query_len;
    tx_data.FragmentCount = 1;
    tx_data.FragmentTable[0].FragmentLength = (uint32_t)query_len;
    tx_data.FragmentTable[0].FragmentBuffer = query_buf;

    EFI_UDP4_COMPLETION_TOKEN tx_token;
    ba_memset(&tx_token, 0, sizeof(tx_token));

    status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              empty_notify, 0, &tx_token.Event);
    if (status != EFI_SUCCESS) {
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    tx_token.Status = EFI_NOT_READY;
    tx_token.Packet.TxData = &tx_data;

    status = udp4->Transmit(udp4, &tx_token);
    if (status != EFI_SUCCESS) {
        print_status("UDP4 Transmit", status);
        gBS->CloseEvent(tx_token.Event);
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    /* Poll for transmit completion */
    for (int wait = 0; wait < 50; wait++) {
        udp4->Poll(udp4);
        if (tx_token.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }
    gBS->CloseEvent(tx_token.Event);

    if (tx_token.Status != EFI_SUCCESS) {
        print_status("UDP4 Transmit result", tx_token.Status);
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return tx_token.Status;
    }

    /* --- Receive DNS response --- */
    EFI_UDP4_COMPLETION_TOKEN rx_token;
    ba_memset(&rx_token, 0, sizeof(rx_token));

    status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                              empty_notify, 0, &rx_token.Event);
    if (status != EFI_SUCCESS) {
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    rx_token.Status = EFI_NOT_READY;
    rx_token.Packet.RxData = 0;

    status = udp4->Receive(udp4, &rx_token);
    if (status != EFI_SUCCESS) {
        print_status("UDP4 Receive", status);
        gBS->CloseEvent(rx_token.Event);
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return status;
    }

    /* Poll for receive completion (5-second timeout) */
    for (int wait = 0; wait < 50; wait++) {
        udp4->Poll(udp4);
        if (rx_token.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }
    gBS->CloseEvent(rx_token.Event);

    if (rx_token.Status != EFI_SUCCESS || !rx_token.Packet.RxData) {
        if (rx_token.Status == EFI_NOT_READY) {
            efi_print_ascii("  DNS query timed out\r\n");
        } else {
            print_status("UDP4 Receive result", rx_token.Status);
        }
        udp4_sb->DestroyChild(udp4_sb, child_handle);
        return (rx_token.Status == EFI_NOT_READY) ? EFI_TIMEOUT : rx_token.Status;
    }

    /* Parse the DNS response from the received fragment */
    EFI_UDP4_RECEIVE_DATA *rx_data = rx_token.Packet.RxData;
    int parsed = 0;

    if (rx_data->FragmentCount > 0 && rx_data->FragmentTable[0].FragmentBuffer) {
        parsed = dns_parse_response(
            (const uint8_t *)rx_data->FragmentTable[0].FragmentBuffer,
            (int)rx_data->FragmentTable[0].FragmentLength,
            out_ip);
    }

    /* Signal RecycleSignal to return buffer to firmware */
    if (rx_data->RecycleSignal) {
        gBS->SignalEvent(rx_data->RecycleSignal);
    }

    udp4_sb->DestroyChild(udp4_sb, child_handle);

    if (!parsed) {
        efi_print_ascii("  DNS: no A record in response\r\n");
        return EFI_NOT_FOUND;
    }

    return EFI_SUCCESS;
}

/* ---- DNS resolution ---- */

EFI_STATUS efi_dns_resolve(const char *hostname, EFI_IPv4_ADDRESS *out_ip) {
    /* First try: parse as raw IP address */
    if (parse_ipv4(hostname, out_ip)) {
        return EFI_SUCCESS;
    }

    if (!gBS) return EFI_NOT_READY;

    /* Use our lightweight UDP4 DNS client (OVMF's DNS4 driver crashes with #GP) */
    efi_print_ascii("  Resolving ");
    efi_print_ascii(hostname);
    efi_print_ascii("...\r\n");

    EFI_STATUS status = udp4_dns_resolve(hostname, out_ip);
    if (status == EFI_SUCCESS) {
        efi_print_ascii("  Resolved to ");
        print_ip(out_ip);
        efi_print_ascii("\r\n");
    }
    return status;
}

/* ---- HTTP/1.0 GET ---- */

EFI_STATUS efi_http_get(const char *url, char *response, UINTN max_size, UINTN *out_size) {
    char host[256];
    char path[512];
    uint16_t port;

    *out_size = 0;

    /* Parse URL */
    parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    efi_print_ascii("  Host: ");
    efi_print_ascii(host);
    efi_print_ascii("  Port: ");
    char port_str[8];
    int pi = 0;
    int pv = port;
    if (pv >= 10000) port_str[pi++] = '0' + pv / 10000;
    if (pv >= 1000) port_str[pi++] = '0' + (pv / 1000) % 10;
    if (pv >= 100) port_str[pi++] = '0' + (pv / 100) % 10;
    if (pv >= 10) port_str[pi++] = '0' + (pv / 10) % 10;
    port_str[pi++] = '0' + pv % 10;
    port_str[pi] = '\0';
    efi_print_ascii(port_str);
    efi_print_ascii("  Path: ");
    efi_print_ascii(path);
    efi_print_ascii("\r\n");

    /* Resolve hostname to IP */
    EFI_IPv4_ADDRESS ip;
    EFI_STATUS status = efi_dns_resolve(host, &ip);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  Failed to resolve host\r\n");
        return status;
    }

    /* Connect */
    efi_print_ascii("  Connecting to ");
    print_ip(&ip);
    efi_print_ascii("...\r\n");

    tcp4_conn_t conn;
    status = tcp4_connect(&ip, port, &conn);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  Connection failed\r\n");
        return status;
    }

    efi_print_ascii("  Connected. Sending request...\r\n");

    /* Build HTTP/1.0 request */
    char request[1024];
    int rpos = 0;

    /* "GET /path HTTP/1.0\r\n" */
    const char *prefix = "GET ";
    for (int i = 0; prefix[i]; i++) request[rpos++] = prefix[i];
    for (int i = 0; path[i] && rpos < 900; i++) request[rpos++] = path[i];
    const char *ver = " HTTP/1.0\r\n";
    for (int i = 0; ver[i]; i++) request[rpos++] = ver[i];

    /* "Host: hostname\r\n" */
    const char *hdr = "Host: ";
    for (int i = 0; hdr[i]; i++) request[rpos++] = hdr[i];
    for (int i = 0; host[i] && rpos < 950; i++) request[rpos++] = host[i];
    request[rpos++] = '\r';
    request[rpos++] = '\n';

    /* "Connection: close\r\n\r\n" */
    const char *close_hdr = "Connection: close\r\n\r\n";
    for (int i = 0; close_hdr[i]; i++) request[rpos++] = close_hdr[i];

    /* Send request */
    status = tcp4_send(&conn, request, (UINTN)rpos);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  Send failed\r\n");
        tcp4_close(&conn);
        return status;
    }

    /* Receive response */
    efi_print_ascii("  Receiving...\r\n");
    char recv_buf[4096];
    UINTN total = 0;
    int header_done = 0;
    UINTN body_start = 0;

    while (total < max_size - 1) {
        INTN got = tcp4_recv(&conn, recv_buf, sizeof(recv_buf));
        if (got <= 0) break; /* Connection closed or error */

        /* Copy received data into response buffer */
        UINTN to_copy = (UINTN)got;
        if (total + to_copy >= max_size) to_copy = max_size - 1 - total;

        ba_memcpy(response + total, recv_buf, to_copy);
        total += to_copy;
    }
    response[total] = '\0';

    tcp4_close(&conn);

    /* Find end of HTTP headers (\r\n\r\n) and return only the body */
    if (!header_done) {
        for (UINTN i = 0; i + 3 < total; i++) {
            if (response[i] == '\r' && response[i + 1] == '\n' &&
                response[i + 2] == '\r' && response[i + 3] == '\n') {
                body_start = i + 4;
                header_done = 1;
                break;
            }
        }
    }

    if (header_done && body_start < total) {
        UINTN body_len = total - body_start;
        /* Shift body to start of buffer */
        for (UINTN i = 0; i < body_len; i++) {
            response[i] = response[body_start + i];
        }
        response[body_len] = '\0';
        *out_size = body_len;
    } else {
        /* No header boundary found — return everything */
        *out_size = total;
    }

    return EFI_SUCCESS;
}
