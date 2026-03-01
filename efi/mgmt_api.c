/*
 * mgmt_api.c — Remote management API for BootAI
 *
 * Three endpoints for remote debugging over Ethernet:
 *   POST /api/cmd    — Execute a REPL command, return captured output
 *   GET  /api/log    — Read the log buffer (supports ?offset=N for streaming)
 *   GET  /api/health — Extended status with WiFi/IP info
 *
 * Authentication:
 *   Production: Bearer token from wifi.conf (api_token=...)
 *   Debug (DEBUG_REMOTE=1): auth bypassed entirely
 */

#include "efi.h"

/* http_response type and helpers — defined in http_server.c */
typedef struct {
    char *buf;
    int len;
    int cap;
} http_response;

extern void resp_init(http_response *r);
extern void resp_append(http_response *r, const char *s, int n);
extern void resp_append_str(http_response *r, const char *s);
extern void resp_append_int(http_response *r, int val);
extern void http_format_response(http_response *resp, int status_code,
                                  const char *status_text,
                                  const char *content_type,
                                  const char *body, int body_len);

/* http_request — defined in http_server.c */
typedef struct {
    const char *method;
    int method_len;
    const char *path;
    int path_len;
    const char *body;
    int body_len;
    int content_length;
    const char *auth_header;
    int auth_header_len;
    const char *query;
    int query_len;
} http_request;

/* Command executor — defined in main.c */
extern int ba_exec_cmd(const char *line);
extern int g_api_context;

/* ---- Freestanding helpers ---- */

static int mgmt_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int mgmt_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static int __attribute__((unused)) mgmt_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int mgmt_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

/* ---- JSON escape (for output strings) ---- */

static int mgmt_json_escape(char *dst, int dst_max, const char *src, int src_len) {
    int di = 0;
    for (int si = 0; si < src_len && di < dst_max - 6; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"') { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else if (c < 0x20) {
            dst[di++] = '\\'; dst[di++] = 'u'; dst[di++] = '0'; dst[di++] = '0';
            dst[di++] = "0123456789abcdef"[c >> 4];
            dst[di++] = "0123456789abcdef"[c & 0xF];
        }
        else { dst[di++] = (char)c; }
    }
    return di;
}

/* ---- Authentication ---- */

static int mgmt_auth_check(const char *auth_header, int auth_len) {
#ifdef DEBUG_REMOTE
    (void)auth_header;
    (void)auth_len;
    return 1;  /* Skip auth in debug mode */
#else
    const char *token = efi_get_api_token();
    if (!token || !token[0]) return 0;  /* No token configured = locked out */
    if (!auth_header || auth_len < 8) return 0;
    if (mgmt_strncmp(auth_header, "Bearer ", 7) != 0) return 0;
    /* auth_header is NOT null-terminated — use length-bounded compare */
    int token_len = mgmt_strlen(token);
    int value_len = auth_len - 7;
    if (value_len != token_len) return 0;
    return mgmt_strncmp(auth_header + 7, token, token_len) == 0;
#endif
}

/* ---- Extract "cmd" value from JSON body ---- */

static const char *mgmt_json_get_cmd(const char *json, int json_len, int *out_len) {
    const char *key = "cmd";
    int klen = 3;
    *out_len = 0;

    for (int i = 0; i + klen + 3 < json_len; i++) {
        if (json[i] == '"' && mgmt_strncmp(json + i + 1, key, klen) == 0 &&
            json[i + 1 + klen] == '"') {
            int j = i + 1 + klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;
            if (j < json_len && json[j] == '"') {
                j++; /* skip opening quote */
                int start = j;
                while (j < json_len && json[j] != '"') {
                    if (json[j] == '\\' && j + 1 < json_len) j++;
                    j++;
                }
                *out_len = j - start;
                return json + start;
            }
        }
    }
    return 0;
}

/* ---- Extract query parameter value ---- */

static int mgmt_query_get_int(const char *query, int query_len,
                               const char *key, int default_val) {
    if (!query || query_len <= 0) return default_val;
    int klen = mgmt_strlen(key);

    for (int i = 0; i + klen + 1 <= query_len; i++) {
        /* Check for key= at position i (must be at start or after &) */
        if ((i == 0 || query[i - 1] == '&') &&
            mgmt_strncmp(query + i, key, klen) == 0 &&
            i + klen < query_len && query[i + klen] == '=') {
            return mgmt_atoi(query + i + klen + 1);
        }
    }
    return default_val;
}

/* ---- POST /api/cmd ---- */

#define CMD_OUTPUT_MAX 32768

static char cmd_output_buf[CMD_OUTPUT_MAX];

static void mgmt_api_cmd(http_request *req, http_response *resp) {
    /* Must be POST */
    if (req->method_len != 4 || mgmt_strncmp(req->method, "POST", 4) != 0) {
        const char *body = "{\"ok\":false,\"error\":\"method not allowed\"}";
        http_format_response(resp, 405, "Method Not Allowed",
                             "application/json", body, mgmt_strlen(body));
        return;
    }

    /* Extract "cmd" from JSON body */
    int cmd_len = 0;
    const char *cmd_raw = mgmt_json_get_cmd(req->body, req->body_len, &cmd_len);
    if (!cmd_raw || cmd_len <= 0 || cmd_len > 500) {
        const char *body = "{\"ok\":false,\"error\":\"missing or invalid cmd field\"}";
        http_format_response(resp, 400, "Bad Request",
                             "application/json", body, mgmt_strlen(body));
        return;
    }

    /* Copy cmd to null-terminated buffer */
    char cmd_buf[512];
    for (int i = 0; i < cmd_len && i < 511; i++) cmd_buf[i] = cmd_raw[i];
    cmd_buf[cmd_len < 511 ? cmd_len : 511] = '\0';

    /* Record log position before execution */
    UINTN pos_before = efi_log_pos();

    /* Execute command in API context */
    g_api_context = 1;
    int result = ba_exec_cmd(cmd_buf);
    g_api_context = 0;

    /* Capture output from log buffer */
    UINTN pos_after = efi_log_pos();
    const char *log = efi_log_buf();
    int output_len = 0;
    if (pos_after > pos_before) {
        output_len = (int)(pos_after - pos_before);
    }

    /* Check for blocked command */
    if (result == -2) {
        const char *body = "{\"ok\":false,\"error\":\"command blocked (interactive)\"}";
        http_format_response(resp, 403, "Forbidden",
                             "application/json", body, mgmt_strlen(body));
        return;
    }

    /* Build JSON response: {"ok":true,"output":"..."} */
    int ji = 0;
    const char *s = "{\"ok\":true,\"output\":\"";
    while (*s && ji < CMD_OUTPUT_MAX - 256) cmd_output_buf[ji++] = *s++;

    /* Escape captured output */
    ji += mgmt_json_escape(cmd_output_buf + ji, CMD_OUTPUT_MAX - ji - 16,
                            log + pos_before, output_len);

    s = "\"}";
    while (*s && ji < CMD_OUTPUT_MAX - 1) cmd_output_buf[ji++] = *s++;
    cmd_output_buf[ji] = '\0';

    http_format_response(resp, 200, "OK", "application/json",
                         cmd_output_buf, ji);
}

/* ---- GET /api/log ---- */

static void mgmt_api_log(http_request *req, http_response *resp) {
    /* Must be GET */
    if (req->method_len != 3 || mgmt_strncmp(req->method, "GET", 3) != 0) {
        const char *body = "{\"ok\":false,\"error\":\"method not allowed\"}";
        http_format_response(resp, 405, "Method Not Allowed",
                             "application/json", body, mgmt_strlen(body));
        return;
    }

    /* Parse ?offset=N from query string */
    int offset = mgmt_query_get_int(req->query, req->query_len, "offset", 0);

    UINTN pos = efi_log_pos();
    const char *log = efi_log_buf();

    /* Clamp offset */
    if (offset < 0) offset = 0;
    if ((UINTN)offset > pos) offset = (int)pos;

    int data_len = (int)(pos - (UINTN)offset);

    /* Build JSON: {"pos":NNN,"data":"..."} */
    int ji = 0;
    const char *s = "{\"pos\":";
    while (*s && ji < CMD_OUTPUT_MAX - 256) cmd_output_buf[ji++] = *s++;

    /* Append pos as integer */
    {
        char num[12]; int ni = 0;
        UINTN v = pos;
        if (v == 0) { num[ni++] = '0'; }
        else {
            char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni && ji < CMD_OUTPUT_MAX - 256; k++)
            cmd_output_buf[ji++] = num[k];
    }

    s = ",\"data\":\"";
    while (*s && ji < CMD_OUTPUT_MAX - 256) cmd_output_buf[ji++] = *s++;

    /* Escape log data */
    ji += mgmt_json_escape(cmd_output_buf + ji, CMD_OUTPUT_MAX - ji - 16,
                            log + offset, data_len);

    s = "\"}";
    while (*s && ji < CMD_OUTPUT_MAX - 1) cmd_output_buf[ji++] = *s++;
    cmd_output_buf[ji] = '\0';

    http_format_response(resp, 200, "OK", "application/json",
                         cmd_output_buf, ji);
}

/* ---- GET /api/health ---- */

static void mgmt_api_health(http_request *req, http_response *resp) {
    (void)req;

    int ji = 0;
    const char *s = "{\"status\":\"ok\"";
    while (*s) cmd_output_buf[ji++] = *s++;

    /* WiFi status */
    s = ",\"wifi\":\"";
    while (*s) cmd_output_buf[ji++] = *s++;

    if (efi_wifi_available()) {
        /* g_wifi_connected and g_wifi_ssid are static in wifi.c,
         * so we use the public efi_net_available() as a proxy */
        s = "available";
    } else {
        s = "none";
    }
    while (*s) cmd_output_buf[ji++] = *s++;
    cmd_output_buf[ji++] = '"';

    /* Network status */
    s = ",\"network\":\"";
    while (*s) cmd_output_buf[ji++] = *s++;
    s = efi_net_available() ? "ready" : "unavailable";
    while (*s) cmd_output_buf[ji++] = *s++;
    cmd_output_buf[ji++] = '"';

    /* IP address */
    EFI_IPv4_ADDRESS ip;
    if (efi_get_local_ip(&ip) == EFI_SUCCESS) {
        s = ",\"ip\":\"";
        while (*s) cmd_output_buf[ji++] = *s++;
        /* Format IP address */
        for (int i = 0; i < 4; i++) {
            if (i > 0) cmd_output_buf[ji++] = '.';
            int v = ip.Addr[i];
            if (v >= 100) cmd_output_buf[ji++] = '0' + v / 100;
            if (v >= 10) cmd_output_buf[ji++] = '0' + (v / 10) % 10;
            cmd_output_buf[ji++] = '0' + v % 10;
        }
        cmd_output_buf[ji++] = '"';
    }

    /* Log buffer usage */
    s = ",\"log_pos\":";
    while (*s) cmd_output_buf[ji++] = *s++;
    {
        char num[12]; int ni = 0;
        UINTN v = efi_log_pos();
        if (v == 0) { num[ni++] = '0'; }
        else {
            char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (int)(v % 10); v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) cmd_output_buf[ji++] = num[k];
    }

    cmd_output_buf[ji++] = '}';
    cmd_output_buf[ji] = '\0';

    http_format_response(resp, 200, "OK", "application/json",
                         cmd_output_buf, ji);
}

/* ---- POST /api/update — streaming firmware update ---- */

#define UPDATE_CHUNK_SIZE 4096

void mgmt_handle_update(tcp4_conn_t *client, const char *auth_header,
                         int auth_header_len, const char *body_start,
                         int body_bytes_read, int content_length) {
    /* 1. Auth check */
    if (!mgmt_auth_check(auth_header, auth_header_len)) {
        const char *r = "HTTP/1.0 403 Forbidden\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 34\r\n\r\n"
                        "{\"ok\":false,\"error\":\"unauthorized\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
        return;
    }

    /* 2. Validate Content-Length */
    if (content_length <= 0 || content_length > 4 * 1024 * 1024) {
        const char *r = "HTTP/1.0 400 Bad Request\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 45\r\n\r\n"
                        "{\"ok\":false,\"error\":\"invalid content-length\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
        return;
    }

    /* 3. Open filesystem root */
    EFI_FILE_PROTOCOL *root = efi_fs_root();
    if (!root) {
        const char *r = "HTTP/1.0 500 Internal Server Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 40\r\n\r\n"
                        "{\"ok\":false,\"error\":\"filesystem not ready\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
        return;
    }

    /* 4. Build UCS-2 path for \EFI\BOOT\BOOTX64.EFI */
    CHAR16 wpath[32];
    {
        const char *apath = "\\EFI\\BOOT\\BOOTX64.EFI";
        int i = 0;
        while (apath[i]) { wpath[i] = (CHAR16)(unsigned char)apath[i]; i++; }
        wpath[i] = 0;
    }

    /* 5. Delete old file if it exists */
    EFI_FILE_PROTOCOL *old = 0;
    if (root->Open(root, &old, wpath,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0) == EFI_SUCCESS) {
        old->Delete(old); /* Delete closes the handle */
    }

    /* 6. Create new file */
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS st = root->Open(root, &file, wpath,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (st != EFI_SUCCESS) {
        const char *r = "HTTP/1.0 500 Internal Server Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 37\r\n\r\n"
                        "{\"ok\":false,\"error\":\"file create fail\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
        return;
    }

    /* 7. Write body bytes already in request buffer */
    int total_written = 0;
    if (body_bytes_read > 0) {
        UINTN ws = (UINTN)body_bytes_read;
        file->Write(file, &ws, (void *)body_start);
        total_written += body_bytes_read;
    }

    /* 8. Stream remaining from TCP in chunks */
    char chunk[UPDATE_CHUNK_SIZE];
    while (total_written < content_length) {
        int remaining = content_length - total_written;
        int to_read = remaining < UPDATE_CHUNK_SIZE ? remaining : UPDATE_CHUNK_SIZE;
        INTN got = tcp4_recv(client, chunk, (UINTN)to_read);
        if (got <= 0) break;
        UINTN ws = (UINTN)got;
        file->Write(file, &ws, chunk);
        total_written += (int)got;
    }

    /* 9. Flush and close file */
    file->Flush(file);
    file->Close(file);

    /* 10. Log result */
    efi_print_ascii("  Firmware updated (");
    {
        char num[12]; int ni = 0; int v = total_written;
        if (v == 0) { num[ni++] = '0'; }
        else { char t[12]; int ti = 0; while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; } while (ti > 0) num[ni++] = t[--ti]; }
        num[ni] = '\0';
        efi_print_ascii(num);
    }
    efi_print_ascii(" bytes)\r\n");

    /* 11. Send response */
    if (total_written == content_length) {
        const char *r = "HTTP/1.0 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 38\r\n\r\n"
                        "{\"ok\":true,\"message\":\"firmware updated\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
    } else {
        const char *r = "HTTP/1.0 500 Internal Server Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: 40\r\n\r\n"
                        "{\"ok\":false,\"error\":\"incomplete transfer\"}";
        tcp4_send(client, r, (UINTN)mgmt_strlen(r));
    }
}

/* ---- Router ---- */

void mgmt_route(http_request *req, http_response *resp) {
    /* Auth check */
    if (!mgmt_auth_check(req->auth_header, req->auth_header_len)) {
        const char *body = "{\"ok\":false,\"error\":\"unauthorized\"}";
        http_format_response(resp, 403, "Forbidden",
                             "application/json", body, mgmt_strlen(body));
        return;
    }

    /* Route by path */
    if (req->path_len == 8 && mgmt_strncmp(req->path, "/api/cmd", 8) == 0) {
        mgmt_api_cmd(req, resp);
        return;
    }

    if (req->path_len == 8 && mgmt_strncmp(req->path, "/api/log", 8) == 0) {
        mgmt_api_log(req, resp);
        return;
    }

    if (req->path_len == 11 && mgmt_strncmp(req->path, "/api/health", 11) == 0) {
        mgmt_api_health(req, resp);
        return;
    }

    /* Unknown /api/ endpoint */
    const char *body = "{\"ok\":false,\"error\":\"not found\"}";
    http_format_response(resp, 404, "Not Found",
                         "application/json", body, mgmt_strlen(body));
}
