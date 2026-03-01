/*
 * wifi.c — WiFi management for BootAI
 *
 * Uses EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL (WiFi2) and
 * EFI_SUPPLICANT_PROTOCOL for WPA2-PSK authentication.
 *
 * WiFi2 is standard UEFI (2.7+). If firmware exposes it (e.g. HP business
 * laptops with Intel AX201), we use it. If not, all functions silently
 * report "no WiFi adapter" — never blocks boot.
 *
 * Config: \wifi.conf on USB root. Plain text, multiple [network] blocks.
 */

#include "efi.h"

/* Protocol GUIDs */
EFI_GUID gEfiWiFi2ProtocolGuid = {
    0x1b0fb9bf, 0x699d, 0x4fdd,
    {0xa7, 0xc3, 0x25, 0xd6, 0x5e, 0xa3, 0x71, 0x57}
};

EFI_GUID gEfiSupplicantProtocolGuid = {
    0x54fcc43e, 0xaa89, 0x4333,
    {0x9a, 0x85, 0xcd, 0xea, 0x24, 0x05, 0x1a, 0x3e}
};

/* ---- Freestanding helpers ---- */

static void wifi_memset(void *dst, int val, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

static void wifi_memcpy(void *dst, const void *src, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static int wifi_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int wifi_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static int wifi_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ---- Notify function for completion tokens ---- */
#define EVT_NOTIFY_SIGNAL  0x00000200
#define TPL_CALLBACK       8

static void EFIAPI wifi_empty_notify(EFI_EVENT event, void *ctx) {
    (void)event; (void)ctx;
}

/* ---- Static state ---- */

static EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL *g_wifi2 = 0;
static EFI_SUPPLICANT_PROTOCOL *g_supplicant = 0;
static int g_wifi_inited = 0;
static int g_wifi_connected = 0;
static char g_wifi_ssid[64] = {0};

#define SEARCH_BY_PROTOCOL 2

/* ---- WiFi init (internal) ---- */

static void efi_wifi_init(void) {
    if (g_wifi_inited) return;
    g_wifi_inited = 1;

    if (!gBS) return;

    /* Look for WiFi2 protocol */
    UINTN handle_count = 0;
    EFI_HANDLE *handles = 0;

    EFI_STATUS status = gBS->LocateHandleBuffer(
        SEARCH_BY_PROTOCOL, &gEfiWiFi2ProtocolGuid, 0, &handle_count, &handles);

    if (status != EFI_SUCCESS || handle_count == 0) {
        /* No WiFi hardware — this is normal in QEMU and on most hardware */
        return;
    }

    /* Get WiFi2 protocol */
    status = gBS->HandleProtocol(handles[0], &gEfiWiFi2ProtocolGuid, (void **)&g_wifi2);
    if (status != EFI_SUCCESS) {
        g_wifi2 = 0;
        gBS->FreePool(handles);
        return;
    }

    /* Get Supplicant protocol from same handle (for WPA2 credentials) */
    gBS->HandleProtocol(handles[0], &gEfiSupplicantProtocolGuid, (void **)&g_supplicant);
    /* Supplicant is optional — open networks don't need it */

    gBS->FreePool(handles);

    efi_print_ascii("  WiFi adapter found\r\n");
}

/* ---- Public API ---- */

int efi_wifi_available(void) {
    efi_wifi_init();
    return g_wifi2 != 0;
}

void efi_wifi_scan(void) {
    efi_wifi_init();

    if (!g_wifi2) {
        efi_print_ascii("  No WiFi adapter\r\n");
        return;
    }

    /* Create completion event */
    EFI_80211_GET_NETWORKS_TOKEN token;
    wifi_memset(&token, 0, sizeof(token));

    EFI_STATUS status = gBS->CreateEvent(
        EVT_NOTIFY_SIGNAL, TPL_CALLBACK, wifi_empty_notify, 0, &token.Token.Event);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  WiFi scan: CreateEvent failed\r\n");
        return;
    }

    token.Token.Status = EFI_NOT_READY;
    token.Result = 0;

    /* Initiate scan */
    status = g_wifi2->GetNetworks(g_wifi2, &token);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  WiFi scan failed\r\n");
        gBS->CloseEvent(token.Token.Event);
        return;
    }

    /* Poll for completion (up to 10 seconds) */
    efi_print_ascii("  Scanning");
    for (int i = 0; i < 100; i++) {
        if (token.Token.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
        if (i % 10 == 9) efi_print_ascii(".");
    }
    efi_print_ascii("\r\n");

    gBS->CloseEvent(token.Token.Event);

    if (token.Token.Status != EFI_SUCCESS || !token.Result) {
        efi_print_ascii("  WiFi scan: no results\r\n");
        return;
    }

    /* Display results */
    EFI_80211_NETWORK_DESCRIPTION *nets = token.Result;
    if (nets->NetworkCount == 0) {
        efi_print_ascii("  No networks found\r\n");
    } else {
        for (uint32_t i = 0; i < nets->NetworkCount; i++) {
            EFI_80211_NETWORK *net = &nets->Network[i];

            /* Print index */
            char idx[4];
            int v = (int)(i + 1);
            int p = 0;
            if (v >= 10) idx[p++] = '0' + v / 10;
            idx[p++] = '0' + v % 10;
            idx[p++] = '.';
            idx[p] = '\0';
            efi_print_ascii("  ");
            efi_print_ascii(idx);
            efi_print_ascii(" ");

            /* Print SSID */
            char ssid_buf[EFI_MAX_SSID_LEN + 1];
            int slen = net->SSId.SSIdLen;
            if (slen > EFI_MAX_SSID_LEN) slen = EFI_MAX_SSID_LEN;
            wifi_memcpy(ssid_buf, net->SSId.SSId, (UINTN)slen);
            ssid_buf[slen] = '\0';
            efi_print_ascii(ssid_buf);

            /* Print security type */
            const char *sec = " (Open)";
            switch (net->AKMSuite) {
                case Ieee80211SharedKey: sec = " (WEP)"; break;
                case Ieee80211WPAPSK:    sec = " (WPA)"; break;
                case Ieee80211WPA2PSK:   sec = " (WPA2)"; break;
                default: break;
            }
            efi_print_ascii(sec);

            /* Print RSSI */
            efi_print_ascii(", ");
            int rssi = (int)net->RSSI;
            if (rssi < 0) {
                efi_print_ascii("-");
                rssi = -rssi;
            }
            char rssi_buf[8];
            p = 0;
            if (rssi >= 100) rssi_buf[p++] = '0' + rssi / 100;
            if (rssi >= 10)  rssi_buf[p++] = '0' + (rssi / 10) % 10;
            rssi_buf[p++] = '0' + rssi % 10;
            rssi_buf[p] = '\0';
            efi_print_ascii(rssi_buf);
            efi_print_ascii(" dBm\r\n");
        }
    }

    /* Free result (allocated by firmware) */
    gBS->FreePool(nets);
}

EFI_STATUS efi_wifi_connect(const char *ssid, const char *password) {
    efi_wifi_init();

    if (!g_wifi2) {
        efi_print_ascii("  No WiFi adapter\r\n");
        return EFI_UNSUPPORTED;
    }

    int ssid_len = wifi_strlen(ssid);
    if (ssid_len == 0 || ssid_len > EFI_MAX_SSID_LEN) {
        efi_print_ascii("  Invalid SSID\r\n");
        return EFI_UNSUPPORTED;
    }

    /* Set credentials via Supplicant if we have a password */
    if (password && password[0] && g_supplicant) {
        /* Set SSID */
        EFI_80211_SSID ssid_data;
        wifi_memset(&ssid_data, 0, sizeof(ssid_data));
        ssid_data.SSIdLen = (uint8_t)ssid_len;
        wifi_memcpy(ssid_data.SSId, ssid, (UINTN)ssid_len);

        EFI_STATUS s = g_supplicant->SetData(g_supplicant,
            EFI_SUPPLICANT_DATA_TYPE_SSID, &ssid_data, sizeof(ssid_data));
        if (s != EFI_SUCCESS) {
            efi_print_ascii("  WARN: Supplicant SetData SSID failed\r\n");
        }

        /* Set PSK (pre-shared key / password) */
        int psk_len = wifi_strlen(password);
        s = g_supplicant->SetData(g_supplicant,
            EFI_SUPPLICANT_DATA_TYPE_PSK, (void *)password, (UINTN)psk_len);
        if (s != EFI_SUCCESS) {
            efi_print_ascii("  WARN: Supplicant SetData PSK failed\r\n");
        }
    }

    /* Build target network description */
    /* Allocate space for the struct (variable-length, 1 network) */
    EFI_80211_NETWORK_DESCRIPTION net_desc;
    wifi_memset(&net_desc, 0, sizeof(net_desc));
    net_desc.NetworkCount = 1;
    net_desc.Network[0].SSId.SSIdLen = (uint8_t)ssid_len;
    wifi_memcpy(net_desc.Network[0].SSId.SSId, ssid, (UINTN)ssid_len);
    net_desc.Network[0].BSSType = IeeeInfrastructure;

    /* Create completion event */
    EFI_80211_CONNECT_NETWORK_TOKEN token;
    wifi_memset(&token, 0, sizeof(token));

    EFI_STATUS status = gBS->CreateEvent(
        EVT_NOTIFY_SIGNAL, TPL_CALLBACK, wifi_empty_notify, 0, &token.Token.Event);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  WiFi connect: CreateEvent failed\r\n");
        return status;
    }

    token.Token.Status = EFI_NOT_READY;
    token.Data = &net_desc;

    /* Initiate connection */
    efi_print_ascii("  Connecting to ");
    efi_print_ascii(ssid);
    efi_print_ascii("...\r\n");

    status = g_wifi2->ConnectNetwork(g_wifi2, &token);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  WiFi ConnectNetwork failed\r\n");
        gBS->CloseEvent(token.Token.Event);
        return status;
    }

    /* Poll for completion (up to 15 seconds) */
    for (int i = 0; i < 150; i++) {
        if (token.Token.Status != EFI_NOT_READY) break;
        gBS->Stall(100000); /* 100ms */
    }

    gBS->CloseEvent(token.Token.Event);

    if (token.Token.Status != EFI_SUCCESS) {
        efi_print_ascii("  WiFi connection failed\r\n");
        return token.Token.Status;
    }

    /* Bind IP stack to new WiFi interface */
    connect_all_controllers();

    /* Trigger DHCP via network init */
    efi_net_init();

    /* Save state */
    g_wifi_connected = 1;
    int copy_len = ssid_len < 63 ? ssid_len : 63;
    wifi_memcpy(g_wifi_ssid, ssid, (UINTN)copy_len);
    g_wifi_ssid[copy_len] = '\0';

    efi_print_ascii("  Connected to ");
    efi_print_ascii(g_wifi_ssid);
    efi_print_ascii("\r\n");

    return EFI_SUCCESS;
}

void efi_wifi_disconnect(void) {
    efi_wifi_init();

    if (!g_wifi2) {
        efi_print_ascii("  No WiFi adapter\r\n");
        return;
    }

    if (!g_wifi_connected) {
        efi_print_ascii("  Not connected to WiFi\r\n");
        return;
    }

    EFI_80211_DISCONNECT_NETWORK_TOKEN token;
    wifi_memset(&token, 0, sizeof(token));

    EFI_STATUS status = gBS->CreateEvent(
        EVT_NOTIFY_SIGNAL, TPL_CALLBACK, wifi_empty_notify, 0, &token.Token.Event);
    if (status != EFI_SUCCESS) return;

    token.Token.Status = EFI_NOT_READY;

    status = g_wifi2->DisconnectNetwork(g_wifi2, &token);
    if (status == EFI_SUCCESS) {
        /* Poll for completion */
        for (int i = 0; i < 50; i++) {
            if (token.Token.Status != EFI_NOT_READY) break;
            gBS->Stall(100000);
        }
    }

    gBS->CloseEvent(token.Token.Event);

    g_wifi_connected = 0;
    g_wifi_ssid[0] = '\0';
    efi_print_ascii("  Disconnected from WiFi\r\n");
}

void efi_wifi_status(void) {
    efi_wifi_init();

    if (!g_wifi2) {
        efi_print_ascii("  WiFi: no adapter\r\n");
    } else if (g_wifi_connected) {
        efi_print_ascii("  WiFi: connected to ");
        efi_print_ascii(g_wifi_ssid);
        efi_print_ascii("\r\n");
    } else {
        efi_print_ascii("  WiFi: adapter present, not connected\r\n");
    }
}

/* ---- API token (parsed from wifi.conf) ---- */

static char g_api_token[128] = {0};

const char *efi_get_api_token(void) {
    return g_api_token[0] ? g_api_token : 0;
}

/* ---- Config file parser ---- */

#define MAX_WIFI_NETWORKS 16

typedef struct {
    char ssid[64];
    char password[64];
} wifi_network_entry;

/*
 * Parse wifi.conf content into entries array.
 * Returns number of entries parsed.
 */
static int parse_wifi_conf(const char *data, UINTN size, wifi_network_entry *entries, int max_entries) {
    int count = 0;
    int in_network = 0;
    const char *p = data;
    const char *end = data + size;

    while (p < end && count < max_entries) {
        /* Find start of line */
        const char *line_start = p;

        /* Find end of line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            line_end++;

        (void)line_start;

        /* Skip leading whitespace */
        const char *ls = line_start;
        while (ls < line_end && (*ls == ' ' || *ls == '\t')) ls++;
        int trimmed_len = (int)(line_end - ls);

        /* Skip empty lines and comments */
        if (trimmed_len == 0 || *ls == '#') {
            /* advance past line ending */
            p = line_end;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* Check for api_token= (outside [network] blocks) */
        if (!in_network && trimmed_len > 10 && wifi_strncmp(ls, "api_token=", 10) == 0) {
            const char *val = ls + 10;
            int vlen = (int)(line_end - val);
            if (vlen > 127) vlen = 127;
            wifi_memcpy(g_api_token, val, (UINTN)vlen);
            g_api_token[vlen] = '\0';
            p = line_end;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* Check for [network] */
        if (trimmed_len >= 9 && wifi_strncmp(ls, "[network]", 9) == 0) {
            if (in_network && entries[count].ssid[0]) {
                count++; /* finish previous entry */
            }
            if (count < max_entries) {
                in_network = 1;
                wifi_memset(&entries[count], 0, sizeof(wifi_network_entry));
            }
            p = line_end;
            while (p < end && (*p == '\n' || *p == '\r')) p++;
            continue;
        }

        /* Parse ssid= and password= */
        if (in_network && count < max_entries) {
            if (trimmed_len > 5 && wifi_strncmp(ls, "ssid=", 5) == 0) {
                const char *val = ls + 5;
                int vlen = (int)(line_end - val);
                if (vlen > 63) vlen = 63;
                wifi_memcpy(entries[count].ssid, val, (UINTN)vlen);
                entries[count].ssid[vlen] = '\0';
            } else if (trimmed_len > 9 && wifi_strncmp(ls, "password=", 9) == 0) {
                const char *val = ls + 9;
                int vlen = (int)(line_end - val);
                if (vlen > 63) vlen = 63;
                wifi_memcpy(entries[count].password, val, (UINTN)vlen);
                entries[count].password[vlen] = '\0';
            }
        }

        /* Advance past line ending */
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }

    /* Finish last entry if valid */
    if (in_network && count < max_entries && entries[count].ssid[0]) {
        count++;
    }

    return count;
}

void efi_wifi_auto_connect(void) {
    /* Read wifi.conf first — always parse api_token regardless of WiFi adapter */
    void *buf = 0;
    UINTN size = 0;
    EFI_STATUS status = efi_read_file("\\wifi.conf", &buf, &size);

    wifi_network_entry entries[MAX_WIFI_NETWORKS];
    int n_entries = 0;

    if (status == EFI_SUCCESS && buf && size > 0) {
        wifi_memset(entries, 0, sizeof(entries));
        n_entries = parse_wifi_conf((const char *)buf, size, entries, MAX_WIFI_NETWORKS);
        gBS->FreePool(buf);
    }

    /* Now check for WiFi adapter — api_token is already loaded above */
    efi_wifi_init();
    if (!g_wifi2) return; /* No adapter — silently skip WiFi connect */

    if (n_entries == 0) {
        efi_print_ascii("  WiFi: no networks configured in wifi.conf\r\n");
        return;
    }

    /* Try each network until one connects */
    for (int i = 0; i < n_entries; i++) {
        status = efi_wifi_connect(entries[i].ssid, entries[i].password);
        if (status == EFI_SUCCESS) return; /* Connected */
    }

    efi_print_ascii("  WiFi: no configured networks found\r\n");
}

void efi_wifi_save(const char *ssid, const char *password) {
    if (!ssid || !ssid[0]) {
        efi_print_ascii("  No SSID to save\r\n");
        return;
    }

    /* Read existing wifi.conf (if any) */
    void *existing_buf = 0;
    UINTN existing_size = 0;
    efi_read_file("\\wifi.conf", &existing_buf, &existing_size);

    /* Check if SSID already in config */
    if (existing_buf && existing_size > 0) {
        wifi_network_entry entries[MAX_WIFI_NETWORKS];
        wifi_memset(entries, 0, sizeof(entries));
        int n = parse_wifi_conf((const char *)existing_buf, existing_size,
                               entries, MAX_WIFI_NETWORKS);
        for (int i = 0; i < n; i++) {
            if (wifi_streq(entries[i].ssid, ssid)) {
                efi_print_ascii("  ");
                efi_print_ascii(ssid);
                efi_print_ascii(" already in wifi.conf\r\n");
                gBS->FreePool(existing_buf);
                return;
            }
        }
    }

    /* Build new entry to append */
    char new_block[256];
    int pos = 0;
    const char *s;

    /* Add newline separator if file exists and doesn't end with newline */
    if (existing_buf && existing_size > 0) {
        const char *end_char = (const char *)existing_buf + existing_size - 1;
        if (*end_char != '\n') {
            new_block[pos++] = '\n';
        }
    }

    s = "\n[network]\n";
    while (*s && pos < 240) new_block[pos++] = *s++;

    s = "ssid=";
    while (*s && pos < 240) new_block[pos++] = *s++;
    s = ssid;
    while (*s && pos < 240) new_block[pos++] = *s++;
    new_block[pos++] = '\n';

    if (password && password[0]) {
        s = "password=";
        while (*s && pos < 240) new_block[pos++] = *s++;
        s = password;
        while (*s && pos < 240) new_block[pos++] = *s++;
        new_block[pos++] = '\n';
    }

    /* Build complete file content */
    UINTN total_size = existing_size + (UINTN)pos;
    void *write_buf = 0;
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, total_size + 1, &write_buf);
    if (status != EFI_SUCCESS) {
        if (existing_buf) gBS->FreePool(existing_buf);
        return;
    }

    uint8_t *wp = (uint8_t *)write_buf;
    if (existing_buf && existing_size > 0) {
        wifi_memcpy(wp, existing_buf, existing_size);
        wp += existing_size;
    }
    wifi_memcpy(wp, new_block, (UINTN)pos);

    /* Write back */
    status = efi_write_file("\\wifi.conf", write_buf, total_size);
    gBS->FreePool(write_buf);
    if (existing_buf) gBS->FreePool(existing_buf);

    if (status == EFI_SUCCESS) {
        efi_print_ascii("  Saved ");
        efi_print_ascii(ssid);
        efi_print_ascii(" to wifi.conf\r\n");
    } else {
        efi_print_ascii("  Failed to save wifi.conf\r\n");
    }
}
