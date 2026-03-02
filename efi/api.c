/*
 * api.c — OpenAI-compatible API handlers for BootAI HTTP server
 *
 * Implements:
 *   GET  /health              — status check
 *   GET  /v1/models           — model listing
 *   POST /v1/chat/completions — inference (non-streaming)
 *
 * Minimal JSON parsing: extracts known fields only (messages, temperature,
 * max_tokens). Not a general-purpose JSON parser.
 */

#include "efi.h"
#include "model/model.h"

/* Printf (provided by uefi_libc.c) */
int uefi_printf(const char *fmt, ...);

/* Model and tokenizer — owned by main.c */
extern ba_model     *g_model;
extern ba_tokenizer *g_tok;

/* http_response type and helpers — defined in http_server.c */
typedef struct {
    char *buf;
    int len;
    int cap;
} http_response;

/* Forward-declared in http_server.c, implemented there */
extern void resp_init(http_response *r);
extern void resp_append(http_response *r, const char *s, int n);
extern void resp_append_str(http_response *r, const char *s);
extern void resp_append_int(http_response *r, int val);

/* ---- Timing (rdtsc) ---- */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
/* Dell E6510 Core i5-560M: ~2.53 GHz */
#define CYCLES_PER_MS 2530000ULL

/* ---- Freestanding helpers ---- */

static int api_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int api_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

/* ---- Minimal JSON field extraction ---- */

/*
 * Find a JSON string value by key. Returns pointer to first char after
 * opening quote, sets *out_len to string length (not including quotes).
 * Returns NULL if not found.
 */
static const char *json_get_string(const char *json, int json_len,
                                    const char *key, int *out_len) {
    int klen = api_strlen(key);
    *out_len = 0;

    for (int i = 0; i + klen + 3 < json_len; i++) {
        /* Look for "key" followed by : and " */
        if (json[i] == '"' && api_strncmp(json + i + 1, key, klen) == 0 &&
            json[i + 1 + klen] == '"') {
            /* Skip past "key": */
            int j = i + 1 + klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;

            if (j < json_len && json[j] == '"') {
                j++; /* skip opening quote */
                int start = j;
                while (j < json_len && json[j] != '"') {
                    if (json[j] == '\\' && j + 1 < json_len) j++; /* skip escaped char */
                    j++;
                }
                *out_len = j - start;
                return json + start;
            }
        }
    }
    return 0;
}

/*
 * Find a JSON integer value by key. Returns the value, or default_val if
 * not found.
 */
static int json_get_int(const char *json, int json_len,
                         const char *key, int default_val) {
    int klen = api_strlen(key);

    for (int i = 0; i + klen + 3 < json_len; i++) {
        if (json[i] == '"' && api_strncmp(json + i + 1, key, klen) == 0 &&
            json[i + 1 + klen] == '"') {
            int j = i + 1 + klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;

            if (j < json_len && (json[j] >= '0' && json[j] <= '9')) {
                int val = 0;
                while (j < json_len && json[j] >= '0' && json[j] <= '9') {
                    val = val * 10 + (json[j] - '0');
                    j++;
                }
                return val;
            }
        }
    }
    return default_val;
}

/*
 * Find a JSON float value by key. Returns the value, or default_val if
 * not found. Only handles simple decimal format (e.g. 0.7, 1.0).
 */
static float json_get_float(const char *json, int json_len,
                             const char *key, float default_val) {
    int klen = api_strlen(key);

    for (int i = 0; i + klen + 3 < json_len; i++) {
        if (json[i] == '"' && api_strncmp(json + i + 1, key, klen) == 0 &&
            json[i + 1 + klen] == '"') {
            int j = i + 1 + klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;

            if (j < json_len && ((json[j] >= '0' && json[j] <= '9') || json[j] == '.')) {
                float val = 0.0f;
                /* Integer part */
                while (j < json_len && json[j] >= '0' && json[j] <= '9') {
                    val = val * 10.0f + (float)(json[j] - '0');
                    j++;
                }
                /* Decimal part */
                if (j < json_len && json[j] == '.') {
                    j++;
                    float frac = 0.1f;
                    while (j < json_len && json[j] >= '0' && json[j] <= '9') {
                        val += (float)(json[j] - '0') * frac;
                        frac *= 0.1f;
                        j++;
                    }
                }
                return val;
            }
        }
    }
    return default_val;
}

/* ---- Chat message parsing ---- */

typedef struct {
    const char *role;
    int role_len;
    const char *content;
    int content_len;
} chat_message;

#define MAX_MESSAGES 32

/*
 * Parse the "messages" array from the JSON body.
 * Returns number of messages found.
 */
static int json_parse_messages(const char *json, int json_len,
                                chat_message *msgs, int max_msgs) {
    int count = 0;

    /* Find "messages" : [ */
    const char *key = "messages";
    int klen = 8;
    int arr_start = -1;

    for (int i = 0; i + klen + 4 < json_len; i++) {
        if (json[i] == '"' && api_strncmp(json + i + 1, key, klen) == 0 &&
            json[i + 1 + klen] == '"') {
            int j = i + 1 + klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;
            if (j < json_len && json[j] == '[') {
                arr_start = j + 1;
                break;
            }
        }
    }

    if (arr_start < 0) return 0;

    /* Walk through array, finding { ... } objects */
    int pos = arr_start;
    while (pos < json_len && count < max_msgs) {
        /* Skip whitespace and commas */
        while (pos < json_len && (json[pos] == ' ' || json[pos] == ','  ||
               json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t')) pos++;

        if (pos >= json_len || json[pos] == ']') break;

        if (json[pos] != '{') { pos++; continue; }

        /* Find matching } — simple depth tracking */
        int obj_start = pos;
        int depth = 1;
        pos++;
        while (pos < json_len && depth > 0) {
            if (json[pos] == '{') depth++;
            else if (json[pos] == '}') depth--;
            else if (json[pos] == '"') {
                pos++;
                while (pos < json_len && json[pos] != '"') {
                    if (json[pos] == '\\') pos++;
                    pos++;
                }
            }
            pos++;
        }
        int obj_end = pos;
        int obj_len = obj_end - obj_start;

        /* Extract role and content from this message object */
        msgs[count].role = json_get_string(json + obj_start, obj_len, "role",
                                            &msgs[count].role_len);
        msgs[count].content = json_get_string(json + obj_start, obj_len, "content",
                                               &msgs[count].content_len);

        if (msgs[count].role && msgs[count].content) {
            count++;
        }
    }

    return count;
}

/* ---- JSON string escaping ---- */

/*
 * Escape a string for JSON output. Writes to dst, returns number of
 * bytes written. dst must be large enough (worst case: 6x src_len).
 */
static int json_escape(char *dst, int dst_max, const char *src, int src_len) {
    int di = 0;
    for (int si = 0; si < src_len && di < dst_max - 6; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"') { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c == '\n') { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else if (c < 0x20) {
            /* \u00XX */
            dst[di++] = '\\'; dst[di++] = 'u'; dst[di++] = '0'; dst[di++] = '0';
            dst[di++] = "0123456789abcdef"[c >> 4];
            dst[di++] = "0123456789abcdef"[c & 0xF];
        }
        else { dst[di++] = (char)c; }
    }
    return di;
}

/* http_format_response — implemented in http_server.c */
extern void http_format_response(http_response *resp, int status_code,
                                  const char *status_text,
                                  const char *content_type,
                                  const char *body, int body_len);

/* ---- API: GET /health ---- */

void api_health(http_response *resp) {
    const char *body = "{\"status\":\"ok\"}";
    http_format_response(resp, 200, "OK", "application/json",
                        body, api_strlen(body));
}

/* ---- API: GET /v1/models ---- */

void api_models(http_response *resp) {
    /* Build response body */
    static char body[512];
    int bi = 0;
    const char *s;

    s = "{\"object\":\"list\",\"data\":[{\"id\":\"";
    while (*s) body[bi++] = *s++;

    if (g_model) {
        s = "bootai-smollm2-135m";
    } else {
        s = "no-model-loaded";
    }
    while (*s) body[bi++] = *s++;

    s = "\",\"object\":\"model\",\"owned_by\":\"bootai\"}]}";
    while (*s) body[bi++] = *s++;
    body[bi] = '\0';

    http_format_response(resp, 200, "OK", "application/json", body, bi);
}

/* ---- API: POST /v1/chat/completions ---- */

/* Chat template buffer and generated text buffer */
#define CHAT_BUF_SIZE  4096
#define GEN_BUF_SIZE   8192
#define MAX_TOKENS_BUF 2048

static char chat_buf[CHAT_BUF_SIZE];
static char gen_buf[GEN_BUF_SIZE];
static int  token_buf[MAX_TOKENS_BUF];

/* Helper: append string to buffer */
static int buf_add(char *buf, int pos, int max, const char *s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    return pos;
}

static int buf_add_n(char *buf, int pos, int max, const char *s, int n) {
    for (int i = 0; i < n && pos < max - 1; i++) buf[pos++] = s[i];
    return pos;
}

void api_chat_completions(const char *body, int body_len, http_response *resp) {
    /* Check model is loaded */
    if (!g_model || !g_tok) {
        const char *err = "{\"error\":{\"message\":\"model not loaded\",\"type\":\"server_error\"}}";
        http_format_response(resp, 503, "Service Unavailable", "application/json",
                            err, api_strlen(err));
        return;
    }

    /* Parse request */
    chat_message msgs[MAX_MESSAGES];
    int n_msgs = json_parse_messages(body, body_len, msgs, MAX_MESSAGES);

    if (n_msgs == 0) {
        const char *err = "{\"error\":{\"message\":\"no messages provided\",\"type\":\"invalid_request_error\"}}";
        http_format_response(resp, 400, "Bad Request", "application/json",
                            err, api_strlen(err));
        return;
    }

    float temperature = json_get_float(body, body_len, "temperature", 0.7f);
    int max_tokens = json_get_int(body, body_len, "max_tokens", 256);
    if (max_tokens > 2048) max_tokens = 2048;
    if (max_tokens < 1) max_tokens = 256;

    /* Build chat template from messages */
    int ci = 0;

    /* Inject default system prompt if none provided (matches chat REPL) */
    int has_system = 0;
    for (int m = 0; m < n_msgs; m++) {
        if (msgs[m].role_len == 6 && api_strncmp(msgs[m].role, "system", 6) == 0) {
            has_system = 1;
            break;
        }
    }
    if (!has_system) {
        ci = buf_add(chat_buf, ci, CHAT_BUF_SIZE,
            "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n");
    }

    for (int m = 0; m < n_msgs; m++) {
        ci = buf_add(chat_buf, ci, CHAT_BUF_SIZE, "<|im_start|>");
        ci = buf_add_n(chat_buf, ci, CHAT_BUF_SIZE, msgs[m].role, msgs[m].role_len);
        ci = buf_add(chat_buf, ci, CHAT_BUF_SIZE, "\n");
        ci = buf_add_n(chat_buf, ci, CHAT_BUF_SIZE, msgs[m].content, msgs[m].content_len);
        ci = buf_add(chat_buf, ci, CHAT_BUF_SIZE, "<|im_end|>\n");
    }
    ci = buf_add(chat_buf, ci, CHAT_BUF_SIZE, "<|im_start|>assistant\n");
    chat_buf[ci] = '\0';

    /* Reset model state for fresh inference */
    ba_model_reset(g_model);

    /* Tokenize */
    int n_prompt = ba_tokenizer_encode(g_tok, chat_buf, token_buf, MAX_TOKENS_BUF);
    if (n_prompt <= 0) {
        const char *err = "{\"error\":{\"message\":\"tokenization failed\",\"type\":\"server_error\"}}";
        http_format_response(resp, 500, "Internal Server Error", "application/json",
                            err, api_strlen(err));
        return;
    }

    /* Prefill */
    float *logits = 0;
    uint64_t t_prefill_start = rdtsc();
    for (int i = 0; i < n_prompt; i++) {
        logits = ba_model_forward(g_model, &token_buf[i], 1);
    }
    uint64_t t_prefill_end = rdtsc();

    if (!logits) {
        const char *err = "{\"error\":{\"message\":\"forward pass failed\",\"type\":\"server_error\"}}";
        http_format_response(resp, 500, "Internal Server Error", "application/json",
                            err, api_strlen(err));
        return;
    }

    /* Generate */
    ba_sample_config cfg = ba_default_config();
    cfg.temperature = temperature;
    cfg.max_tokens = max_tokens;

    int eos_id = ba_tokenizer_eos(g_tok);
    int im_end_id = ba_tokenizer_im_end(g_tok);
    int vocab = ba_model_vocab_size(g_model);

    int gen_pos = 0;
    int n_gen = 0;
    char decode_buf[64];

    uint64_t t_decode_start = rdtsc();
    for (int i = 0; i < max_tokens; i++) {
        int next = ba_model_sample(logits, vocab, &cfg);
        if (next == eos_id || next == im_end_id) break;

        int dec_len = ba_tokenizer_decode(g_tok, &next, 1, decode_buf, sizeof(decode_buf));
        if (dec_len > 0) {
            for (int j = 0; j < dec_len && gen_pos < GEN_BUF_SIZE - 1; j++) {
                gen_buf[gen_pos++] = decode_buf[j];
            }
        }
        n_gen++;
        logits = ba_model_forward(g_model, &next, 1);
    }
    uint64_t t_decode_end = rdtsc();
    gen_buf[gen_pos] = '\0';

    /* Compute timing metrics */
    uint64_t prefill_ms = (t_prefill_end - t_prefill_start) / CYCLES_PER_MS;
    uint64_t decode_ms  = (t_decode_end - t_decode_start) / CYCLES_PER_MS;
    uint64_t prefill_tps = prefill_ms > 0 ? (uint64_t)n_prompt * 1000 / prefill_ms : 0;
    uint64_t decode_tps  = decode_ms > 0 ? (uint64_t)n_gen * 1000 / decode_ms : 0;
    uint64_t total_ms = prefill_ms + decode_ms;

    /* Build OpenAI-compatible JSON response */
    static char json_body[GEN_BUF_SIZE + 1024];
    int ji = 0;
    const char *s;

    s = "{\"id\":\"chatcmpl-bootai\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"";
    while (*s) json_body[ji++] = *s++;

    /* Escape generated text */
    ji += json_escape(json_body + ji, (int)sizeof(json_body) - ji - 256, gen_buf, gen_pos);

    s = "\"},\"finish_reason\":\"";
    while (*s) json_body[ji++] = *s++;

    s = (n_gen >= max_tokens) ? "length" : "stop";
    while (*s) json_body[ji++] = *s++;

    s = "\"}],\"usage\":{\"prompt_tokens\":";
    while (*s) json_body[ji++] = *s++;

    /* Append prompt token count */
    {
        char num[12]; int ni = 0, v = n_prompt;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = ",\"completion_tokens\":";
    while (*s) json_body[ji++] = *s++;

    {
        char num[12]; int ni = 0, v = n_gen;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = ",\"total_tokens\":";
    while (*s) json_body[ji++] = *s++;

    {
        char num[12]; int ni = 0, v = n_prompt + n_gen;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = ",\"prefill_tok_s\":";
    while (*s) json_body[ji++] = *s++;

    {
        char num[12]; int ni = 0; uint64_t v = prefill_tps;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = ",\"decode_tok_s\":";
    while (*s) json_body[ji++] = *s++;

    {
        char num[12]; int ni = 0; uint64_t v = decode_tps;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = ",\"inference_ms\":";
    while (*s) json_body[ji++] = *s++;

    {
        char num[12]; int ni = 0; uint64_t v = total_ms;
        if (v == 0) { num[ni++] = '0'; }
        else { char tmp[12]; int ti = 0;
            while (v > 0) { tmp[ti++] = '0' + (char)(v % 10); v /= 10; }
            while (ti > 0) num[ni++] = tmp[--ti];
        }
        for (int k = 0; k < ni; k++) json_body[ji++] = num[k];
    }

    s = "}}";
    while (*s) json_body[ji++] = *s++;
    json_body[ji] = '\0';

    http_format_response(resp, 200, "OK", "application/json", json_body, ji);
}
