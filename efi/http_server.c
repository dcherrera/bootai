/*
 * http_server.c — Minimal HTTP/1.0 server for BootAI
 *
 * Listens on a TCP port, accepts one connection at a time,
 * parses HTTP requests, routes to API handlers, and sends responses.
 * Uses pre-allocated static buffers (no per-request allocation).
 *
 * HTTP/1.0 with Connection: close — one request per connection.
 */

#include "efi.h"

/* Printf (provided by uefi_libc.c) */
int uefi_printf(const char *fmt, ...);

/* ---- Pre-allocated buffers ---- */

#define HTTP_REQ_BUF_SIZE   8192
#define HTTP_RESP_BUF_SIZE  65536

static char g_req_buf[HTTP_REQ_BUF_SIZE];
static char g_resp_buf[HTTP_RESP_BUF_SIZE];

/* ---- Freestanding helpers ---- */

static int http_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int http_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static int http_atoi(const char *s) {
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

static void http_memcpy(void *dst, const void *src, int n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

/* ---- Parsed HTTP request ---- */

typedef struct {
    const char *method;     /* "GET", "POST", "OPTIONS" — points into req_buf */
    int method_len;
    const char *path;       /* "/v1/chat/completions" — points into req_buf */
    int path_len;
    const char *body;       /* POST body — points into req_buf */
    int body_len;
    int content_length;     /* Content-Length header value, or -1 */
    const char *auth_header;  /* "Bearer xyz..." or NULL */
    int auth_header_len;
    const char *query;      /* "offset=12345" or NULL */
    int query_len;
} http_request;

/* ---- HTTP response builder ---- */

typedef struct {
    char *buf;
    int len;
    int cap;
} http_response;

void resp_init(http_response *r) {
    r->buf = g_resp_buf;
    r->len = 0;
    r->cap = HTTP_RESP_BUF_SIZE;
}

void resp_append(http_response *r, const char *s, int n) {
    if (n < 0) n = http_strlen(s);
    if (r->len + n > r->cap - 1) n = r->cap - 1 - r->len;
    http_memcpy(r->buf + r->len, s, n);
    r->len += n;
}

void resp_append_str(http_response *r, const char *s) {
    resp_append(r, s, http_strlen(s));
}

void resp_append_int(http_response *r, int val) {
    char tmp[12];
    int neg = 0, ti = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { tmp[ti++] = '0'; }
    else { while (val > 0) { tmp[ti++] = '0' + val % 10; val /= 10; } }
    char out[13];
    int oi = 0;
    if (neg) out[oi++] = '-';
    while (ti > 0) out[oi++] = tmp[--ti];
    out[oi] = '\0';
    resp_append(r, out, oi);
}

/* ---- HTTP request parsing ---- */

static int http_parse_request(const char *buf, int buf_len, http_request *req) {
    /* Defaults */
    req->method = buf;
    req->method_len = 0;
    req->path = "";
    req->path_len = 0;
    req->body = "";
    req->body_len = 0;
    req->content_length = -1;
    req->auth_header = 0;
    req->auth_header_len = 0;
    req->query = 0;
    req->query_len = 0;

    if (buf_len < 4) return 0;

    /* Parse method (up to first space) */
    int pos = 0;
    while (pos < buf_len && buf[pos] != ' ') pos++;
    req->method_len = pos;

    if (pos >= buf_len) return 0;
    pos++; /* skip space */

    /* Parse path (up to next space or '?') */
    req->path = buf + pos;
    int path_start = pos;
    while (pos < buf_len && buf[pos] != ' ' && buf[pos] != '?') pos++;
    req->path_len = pos - path_start;

    /* Capture query string if '?' is present */
    if (pos < buf_len && buf[pos] == '?') {
        pos++; /* skip '?' */
        req->query = buf + pos;
        int query_start = pos;
        while (pos < buf_len && buf[pos] != ' ') pos++;
        req->query_len = pos - query_start;
    }

    /* Skip to end of request line */
    while (pos < buf_len && buf[pos] != '\n') pos++;
    if (pos < buf_len) pos++; /* skip \n */

    /* Parse headers */
    while (pos < buf_len) {
        /* Check for end of headers (\r\n or \n) */
        if (buf[pos] == '\r' && pos + 1 < buf_len && buf[pos + 1] == '\n') {
            pos += 2;
            break;
        }
        if (buf[pos] == '\n') {
            pos++;
            break;
        }

        /* Check for Content-Length header (case-insensitive first char) */
        if ((buf[pos] == 'C' || buf[pos] == 'c') && pos + 16 <= buf_len) {
            if (http_strncmp(buf + pos, "Content-Length: ", 16) == 0 ||
                http_strncmp(buf + pos, "content-length: ", 16) == 0) {
                req->content_length = http_atoi(buf + pos + 16);
            }
        }

        /* Check for Authorization header */
        if ((buf[pos] == 'A' || buf[pos] == 'a') && pos + 15 <= buf_len) {
            if (http_strncmp(buf + pos, "Authorization: ", 15) == 0 ||
                http_strncmp(buf + pos, "authorization: ", 15) == 0) {
                req->auth_header = buf + pos + 15;
                /* Find end of header value */
                int hend = pos + 15;
                while (hend < buf_len && buf[hend] != '\r' && buf[hend] != '\n') hend++;
                req->auth_header_len = hend - (pos + 15);
            }
        }

        /* Skip to next line */
        while (pos < buf_len && buf[pos] != '\n') pos++;
        if (pos < buf_len) pos++;
    }

    /* Body is everything after headers */
    if (pos < buf_len) {
        req->body = buf + pos;
        req->body_len = buf_len - pos;
    }

    return 1;
}

/* ---- Format HTTP response with headers ---- */

void http_format_response(http_response *resp, int status_code,
                                  const char *status_text,
                                  const char *content_type,
                                  const char *body, int body_len) {
    resp_init(resp);

    /* Status line */
    resp_append_str(resp, "HTTP/1.0 ");
    resp_append_int(resp, status_code);
    resp_append_str(resp, " ");
    resp_append_str(resp, status_text);
    resp_append_str(resp, "\r\n");

    /* Headers */
    resp_append_str(resp, "Content-Type: ");
    resp_append_str(resp, content_type);
    resp_append_str(resp, "\r\n");

    resp_append_str(resp, "Content-Length: ");
    resp_append_int(resp, body_len);
    resp_append_str(resp, "\r\n");

    resp_append_str(resp, "Connection: close\r\n");
    resp_append_str(resp, "Access-Control-Allow-Origin: *\r\n");
    resp_append_str(resp, "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
    resp_append_str(resp, "Access-Control-Allow-Headers: Content-Type, Authorization\r\n");
    resp_append_str(resp, "\r\n");

    /* Body */
    resp_append(resp, body, body_len);
}

/* ---- API handler declarations (implemented in api.c) ---- */

void api_health(http_response *resp);
void api_models(http_response *resp);
void api_chat_completions(const char *body, int body_len, http_response *resp);

/* Management API (implemented in mgmt_api.c) */
void mgmt_route(http_request *req, http_response *resp);

/* ---- HTTP router ---- */

static void http_route(http_request *req, http_response *resp) {
    /* OPTIONS preflight for CORS */
    if (req->method_len == 7 && http_strncmp(req->method, "OPTIONS", 7) == 0) {
        http_format_response(resp, 204, "No Content", "text/plain", "", 0);
        return;
    }

    /* Management API: /api/... */
    if (req->path_len >= 4 && http_strncmp(req->path, "/api/", 5) == 0) {
        mgmt_route(req, resp);
        return;
    }

    /* GET /health */
    if (req->method_len == 3 && http_strncmp(req->method, "GET", 3) == 0 &&
        req->path_len == 7 && http_strncmp(req->path, "/health", 7) == 0) {
        api_health(resp);
        return;
    }

    /* GET /v1/models */
    if (req->method_len == 3 && http_strncmp(req->method, "GET", 3) == 0 &&
        req->path_len == 10 && http_strncmp(req->path, "/v1/models", 10) == 0) {
        api_models(resp);
        return;
    }

    /* POST /v1/chat/completions */
    if (req->method_len == 4 && http_strncmp(req->method, "POST", 4) == 0 &&
        req->path_len == 20 && http_strncmp(req->path, "/v1/chat/completions", 20) == 0) {
        api_chat_completions(req->body, req->body_len, resp);
        return;
    }

    /* 404 Not Found */
    const char *body = "{\"error\":\"not found\"}";
    http_format_response(resp, 404, "Not Found", "application/json",
                         body, http_strlen(body));
}

/* ---- Main server loop ---- */

void ba_http_serve(uint16_t port) {
    efi_print_ascii("  Starting HTTP server on port ");
    char port_str[8];
    int pi = 0, pv = port;
    if (pv >= 10000) port_str[pi++] = '0' + pv / 10000;
    if (pv >= 1000) port_str[pi++] = '0' + (pv / 1000) % 10;
    if (pv >= 100) port_str[pi++] = '0' + (pv / 100) % 10;
    if (pv >= 10) port_str[pi++] = '0' + (pv / 10) % 10;
    port_str[pi++] = '0' + pv % 10;
    port_str[pi] = '\0';
    efi_print_ascii(port_str);
    efi_print_ascii("...\r\n");

    /* Create listening socket */
    tcp4_conn_t listener;
    EFI_STATUS status = tcp4_listen(port, &listener);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: failed to start listener\r\n");
        return;
    }

    efi_print_ascii("  HTTP server listening. Press any key to stop.\r\n");

    /* Server loop */
    for (;;) {
        /* Check for keypress to stop server */
        EFI_INPUT_KEY key;
        EFI_STATUS key_status = efi_read_key(&key);
        if (key_status == EFI_SUCCESS) {
            efi_print_ascii("\r\n  Stopping server...\r\n");
            break;
        }

        /* Poll mDNS responder (non-blocking) */
        ba_mdns_poll();

        /* Try to accept a connection (500ms timeout) */
        tcp4_conn_t client;
        status = tcp4_accept(&listener, &client);
        if (status == EFI_TIMEOUT) {
            continue; /* No connection — loop back and check for keypress */
        }
        if (status != EFI_SUCCESS) {
            efi_print_ascii("  Accept error, retrying...\r\n");
            gBS->Stall(500000);
            continue;
        }

        /* Read the full HTTP request */
        int total_read = 0;
        int headers_done = 0;
        int content_length = -1;
        int body_start = 0;
        int streaming_handled = 0;

        while (total_read < HTTP_REQ_BUF_SIZE - 1) {
            INTN got = tcp4_recv(&client, g_req_buf + total_read,
                                  (UINTN)(HTTP_REQ_BUF_SIZE - 1 - total_read));
            if (got <= 0) break;
            total_read += (int)got;
            g_req_buf[total_read] = '\0';

            /* Check if we've received all headers */
            if (!headers_done) {
                for (int i = 0; i + 3 < total_read; i++) {
                    if (g_req_buf[i] == '\r' && g_req_buf[i+1] == '\n' &&
                        g_req_buf[i+2] == '\r' && g_req_buf[i+3] == '\n') {
                        headers_done = 1;
                        body_start = i + 4;

                        /* Extract Content-Length from headers */
                        for (int j = 0; j < i; j++) {
                            if ((g_req_buf[j] == 'C' || g_req_buf[j] == 'c') &&
                                j + 16 <= i &&
                                (http_strncmp(g_req_buf + j, "Content-Length: ", 16) == 0 ||
                                 http_strncmp(g_req_buf + j, "content-length: ", 16) == 0)) {
                                content_length = http_atoi(g_req_buf + j + 16);
                                break;
                            }
                        }
                        break;
                    }
                }
            }

            /* Once headers are done, check for streaming endpoints */
            if (headers_done) {
                http_request hdr_req;
                if (http_parse_request(g_req_buf, total_read, &hdr_req) &&
                    hdr_req.path_len == 11 &&
                    http_strncmp(hdr_req.path, "/api/update", 11) == 0) {
                    /* Streaming endpoint — hand off to handler */
                    efi_print_ascii("  POST /api/update\r\n");
                    int body_bytes = total_read - body_start;
                    mgmt_handle_update(&client, hdr_req.auth_header,
                                       hdr_req.auth_header_len,
                                       g_req_buf + body_start,
                                       body_bytes, content_length);
                    streaming_handled = 1;
                    break;
                }

                /* Normal request: wait for full body */
                if (content_length <= 0) break; /* No body expected */
                int body_received = total_read - body_start;
                if (body_received >= content_length) break; /* Full body received */
            }
        }

        if (streaming_handled) {
            tcp4_close(&client);
            if (g_reboot_pending) efi_reboot();
            continue;
        }

        /* Parse and route */
        http_request req;
        http_response resp;

        if (http_parse_request(g_req_buf, total_read, &req)) {
            /* Log the request */
            efi_print_ascii("  ");
            for (int i = 0; i < req.method_len && i < 8; i++) {
                CHAR16 ch[2] = { (CHAR16)(unsigned char)req.method[i], 0 };
                efi_print(ch);
            }
            efi_print_ascii(" ");
            for (int i = 0; i < req.path_len && i < 64; i++) {
                CHAR16 ch[2] = { (CHAR16)(unsigned char)req.path[i], 0 };
                efi_print(ch);
            }
            efi_print_ascii("\r\n");

            http_route(&req, &resp);
        } else {
            const char *body = "{\"error\":\"bad request\"}";
            http_format_response(&resp, 400, "Bad Request", "application/json",
                                 body, http_strlen(body));
        }

        /* Send response and close */
        tcp4_send(&client, resp.buf, (UINTN)resp.len);
        tcp4_close(&client);

        /* Check for pending reboot (from API "reboot" command) */
        if (g_reboot_pending) efi_reboot();
    }

    /* Clean up listener */
    tcp4_close(&listener);
    efi_print_ascii("  Server stopped.\r\n");
}
