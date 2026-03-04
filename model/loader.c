/*
 * loader.c - Load .btw weight files, allocate model, populate weight pointers
 *
 * Reads the flat binary .btw format from the UEFI filesystem.
 * Weights are contiguous in the file — we just walk through and assign pointers.
 * No copying: weight data stays in the loaded buffer.
 */

#include "model.h"
#include "../efi/efi.h"

/* Freestanding memory functions */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

/* Math declarations for RoPE cache precomputation */
float sqrtf(float x);
float sinf(float x);
float cosf(float x);

/* .btw file header — 128 bytes */
typedef struct __attribute__((packed)) {
    char     magic[4];     /* "BTAI" */
    uint32_t version;      /* 1 */
    uint32_t arch;         /* 0=qwen3, 1=qwen2, 2=llama, 3=rwkvx */
    uint32_t quant;        /* 0=f32, 1=f16, 2=q8, 3=q4 */
    uint32_t n_layers;
    uint32_t n_embd;
    uint32_t n_heads;
    uint32_t n_kv_heads;
    uint32_t head_dim;
    uint32_t ffn_dim;
    uint32_t vocab_size;
    float    rope_theta;
    float    rms_eps;
    uint32_t max_seq_len;
    uint32_t has_qk_norm;
    uint32_t has_qkv_bias;
    uint8_t  reserved[64];
} btw_header;

/* Convert F16 array to newly-allocated F32 array */
static float* f16_array_to_f32(const uint16_t *src, int count) {
    float *dst = (float *)uefi_malloc((size_t)count * sizeof(float));
    if (!dst) return 0;
    for (int i = 0; i < count; i++)
        dst[i] = f16_to_f32(src[i]);
    return dst;
}

/* Quantize F16 matrix [rows × cols] to Q8 (per-row symmetric).
 * Returns 1 on success, 0 on allocation failure. */
static int f16_matrix_to_q8(const uint16_t *src, int rows, int cols,
                             ba_q8_weight *out) {
    size_t data_size = (size_t)rows * cols;
    out->data = (int8_t *)uefi_malloc(data_size);
    out->scales = (float *)uefi_malloc((size_t)rows * sizeof(float));
    if (!out->data || !out->scales) {
        if (out->data)   { uefi_free(out->data);   out->data = 0; }
        if (out->scales) { uefi_free(out->scales); out->scales = 0; }
        return 0;
    }

    for (int r = 0; r < rows; r++) {
        const uint16_t *row = &src[(size_t)r * cols];
        int8_t *dst_row = &out->data[(size_t)r * cols];

        /* Find max absolute value in row */
        float max_abs = 0.0f;
        for (int j = 0; j < cols; j++) {
            float v = f16_to_f32(row[j]);
            float a = v < 0.0f ? -v : v;
            if (a > max_abs) max_abs = a;
        }

        /* Quantize: scale = max_abs / 127, int8 = round(val / scale) */
        float scale = max_abs / 127.0f;
        out->scales[r] = scale;
        if (max_abs == 0.0f) {
            for (int j = 0; j < cols; j++) dst_row[j] = 0;
        } else {
            float inv_scale = 127.0f / max_abs;
            for (int j = 0; j < cols; j++) {
                float v = f16_to_f32(row[j]) * inv_scale;
                int q = (int)(v + (v > 0.0f ? 0.5f : -0.5f));
                if (q > 127)  q = 127;
                if (q < -127) q = -127;
                dst_row[j] = (int8_t)q;
            }
        }
    }
    return 1;
}

/* Walk a contiguous buffer, advancing a cursor pointer */
static void *advance(uint8_t **cursor, size_t bytes) {
    void *ptr = *cursor;
    *cursor += bytes;
    return ptr;
}

/* Workspace pre-allocation */
void ba_workspace_init(ba_workspace *ws, ba_model_config *cfg) {
    int c = cfg->n_embd;
    int q_dim = cfg->n_heads * cfg->head_dim;
    int kv_dim = cfg->n_kv_heads * cfg->head_dim;
    int ffn = cfg->ffn_dim;
    int vocab = cfg->vocab_size;
    int buf_dim = ffn > c ? ffn : c;

    ws->x          = (float *)uefi_calloc(c, sizeof(float));
    ws->x_out      = (float *)uefi_calloc(c, sizeof(float));
    ws->ln_buf     = (float *)uefi_calloc(c, sizeof(float));
    ws->q          = (float *)uefi_calloc(q_dim, sizeof(float));
    ws->k          = (float *)uefi_calloc(kv_dim, sizeof(float));
    ws->v          = (float *)uefi_calloc(kv_dim, sizeof(float));
    ws->attn_out   = (float *)uefi_calloc(q_dim, sizeof(float));
    ws->attn_scores = (float *)uefi_calloc(cfg->max_seq_len, sizeof(float));
    ws->ffn_gate   = (float *)uefi_calloc(ffn, sizeof(float));
    ws->ffn_up     = (float *)uefi_calloc(ffn, sizeof(float));
    ws->ffn_out    = (float *)uefi_calloc(c, sizeof(float));
    ws->logits     = (float *)uefi_calloc(vocab, sizeof(float));
    ws->q8_buf     = (int8_t *)uefi_calloc(buf_dim, sizeof(int8_t));

    /* Precompute RoPE sin/cos lookup table — eliminates sinf/cosf per token */
    int half = cfg->head_dim / 2;
    int max_seq = cfg->max_seq_len;
    ws->rope_half_dim = half;
    ws->rope_max_seq = max_seq;
    ws->rope_cos = (float *)uefi_malloc((size_t)max_seq * half * sizeof(float));
    ws->rope_sin = (float *)uefi_malloc((size_t)max_seq * half * sizeof(float));

    if (ws->rope_cos && ws->rope_sin) {
        /* Compute theta_step = theta^(1/half) via iterated sqrt (same as rope.c) */
        float theta_step = cfg->rope_theta;
        for (int p = half; p > 1; p >>= 1)
            theta_step = sqrtf(theta_step);

        for (int pos = 0; pos < max_seq; pos++) {
            float inv_freq = 1.0f;
            float *cos_row = &ws->rope_cos[pos * half];
            float *sin_row = &ws->rope_sin[pos * half];
            for (int i = 0; i < half; i++) {
                float angle = (float)pos * inv_freq;
                cos_row[i] = cosf(angle);
                sin_row[i] = sinf(angle);
                inv_freq /= theta_step;
            }
        }
    }
}

void ba_workspace_free(ba_workspace *ws) {
    uefi_free(ws->x);
    uefi_free(ws->x_out);
    uefi_free(ws->ln_buf);
    uefi_free(ws->q);
    uefi_free(ws->k);
    uefi_free(ws->v);
    uefi_free(ws->attn_out);
    uefi_free(ws->attn_scores);
    uefi_free(ws->ffn_gate);
    uefi_free(ws->ffn_up);
    uefi_free(ws->ffn_out);
    uefi_free(ws->logits);
    uefi_free(ws->q8_buf);
    uefi_free(ws->rope_cos);
    uefi_free(ws->rope_sin);
}

ba_model* ba_model_load_buffer(void *data, uint64_t file_size) {
    UINTN file_pages = ((UINTN)file_size + 4095) / 4096;

    if (file_size < sizeof(btw_header)) {
        efi_print_ascii("  ERROR: file too small for header\r\n");
        gBS->FreePages((uint64_t)(UINTN)data, file_pages);
        return 0;
    }

    btw_header *hdr = (btw_header *)data;
    if (hdr->magic[0] != 'B' || hdr->magic[1] != 'T' ||
        hdr->magic[2] != 'A' || hdr->magic[3] != 'I') {
        efi_print_ascii("  ERROR: invalid magic (expected BTAI)\r\n");
        gBS->FreePages((uint64_t)(UINTN)data, file_pages);
        return 0;
    }

    if (hdr->version != 1) {
        efi_print_ascii("  ERROR: unsupported version\r\n");
        gBS->FreePages((uint64_t)(UINTN)data, file_pages);
        return 0;
    }

    /* Allocate model */
    ba_model *m = (ba_model *)uefi_calloc(1, sizeof(ba_model));
    if (!m) { gBS->FreePages((uint64_t)(UINTN)data, file_pages); return 0; }

    m->raw_data = data;
    m->raw_size = file_size;

    /* Populate config from header */
    ba_model_config *cfg = &m->config;
    cfg->n_layers    = (int)hdr->n_layers;
    cfg->n_embd      = (int)hdr->n_embd;
    cfg->n_heads     = (int)hdr->n_heads;
    cfg->n_kv_heads  = (int)hdr->n_kv_heads;
    cfg->head_dim    = (int)hdr->head_dim;
    cfg->ffn_dim     = (int)hdr->ffn_dim;
    cfg->vocab_size  = (int)hdr->vocab_size;
    cfg->max_seq_len = (int)hdr->max_seq_len;
    cfg->rope_theta  = hdr->rope_theta;
    cfg->rms_eps     = hdr->rms_eps;
    cfg->has_qk_norm = (int)hdr->has_qk_norm;
    cfg->has_qkv_bias = (int)hdr->has_qkv_bias;

    /* Set architecture (matches converter's arch_to_btw mapping) */
    switch (hdr->arch) {
    case 0: m->arch = MODEL_QWEN3; break;
    case 1: m->arch = MODEL_QWEN25; break;
    case 2: m->arch = MODEL_LLAMA; break;
    case 3: m->arch = MODEL_RWKVX; break;
    default: m->arch = MODEL_LLAMA; break;
    }

    m->quant = (int)hdr->quant;

    int c        = cfg->n_embd;
    int q_dim    = cfg->n_heads * cfg->head_dim;
    int kv_dim   = cfg->n_kv_heads * cfg->head_dim;
    int ffn      = cfg->ffn_dim;
    int vocab    = cfg->vocab_size;
    int hd       = cfg->head_dim;

    /* Walk contiguous weight data after header */
    uint8_t *cursor = (uint8_t *)data + sizeof(btw_header);

    /* Embedding: [vocab * c] f16 — always F16, convert to F32 below */
    uint16_t *raw_embed = (uint16_t *)advance(&cursor, (size_t)vocab * c * sizeof(uint16_t));

    /* Allocate layer weight structs */
    m->layers = (ba_layer_weights *)uefi_calloc(cfg->n_layers, sizeof(ba_layer_weights));

    if (m->quant == 2) {
        /* ---- Q8 path: projections are int8 data + f32 scales in raw buffer ---- */
        for (int i = 0; i < cfg->n_layers; i++) {
            ba_layer_weights *lw = &m->layers[i];

            /* input_layernorm.weight: [c] f32 — stays in raw buffer */
            lw->ln1_w = (float *)advance(&cursor, c * sizeof(float));

            /* Q8 attention projections: int8[rows*cols] + float32[rows] */
            lw->q_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)q_dim * c);
            lw->q_proj_q8.scales = (float *)advance(&cursor, q_dim * sizeof(float));

            lw->k_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)kv_dim * c);
            lw->k_proj_q8.scales = (float *)advance(&cursor, kv_dim * sizeof(float));

            lw->v_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)kv_dim * c);
            lw->v_proj_q8.scales = (float *)advance(&cursor, kv_dim * sizeof(float));

            lw->o_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)c * q_dim);
            lw->o_proj_q8.scales = (float *)advance(&cursor, c * sizeof(float));

            /* QK-Norm (optional, f32 — stays in raw buffer) */
            if (cfg->has_qk_norm) {
                lw->q_norm = (float *)advance(&cursor, hd * sizeof(float));
                lw->k_norm = (float *)advance(&cursor, hd * sizeof(float));
            }

            /* Q/K/V biases (optional, f32 — stays in raw buffer) */
            if (cfg->has_qkv_bias) {
                lw->q_bias = (float *)advance(&cursor, q_dim * sizeof(float));
                lw->k_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
                lw->v_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
            }

            /* post_attention_layernorm.weight: [c] f32 — stays in raw buffer */
            lw->ln2_w = (float *)advance(&cursor, c * sizeof(float));

            /* Q8 FFN projections */
            lw->gate_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)ffn * c);
            lw->gate_proj_q8.scales = (float *)advance(&cursor, ffn * sizeof(float));

            lw->up_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)ffn * c);
            lw->up_proj_q8.scales = (float *)advance(&cursor, ffn * sizeof(float));

            lw->down_proj_q8.data   = (int8_t *)advance(&cursor, (size_t)c * ffn);
            lw->down_proj_q8.scales = (float *)advance(&cursor, c * sizeof(float));
        }
    } else {
        /* ---- F16 path: existing code, convert to F32 after cursor walk ---- */
        typedef struct {
            uint16_t *q, *k, *v, *o, *gate, *up, *down;
        } raw_f16_ptrs;
        raw_f16_ptrs *raw_layers = (raw_f16_ptrs *)uefi_calloc(cfg->n_layers, sizeof(raw_f16_ptrs));

        for (int i = 0; i < cfg->n_layers; i++) {
            ba_layer_weights *lw = &m->layers[i];

            lw->ln1_w = (float *)advance(&cursor, c * sizeof(float));

            raw_layers[i].q    = (uint16_t *)advance(&cursor, (size_t)q_dim * c * sizeof(uint16_t));
            raw_layers[i].k    = (uint16_t *)advance(&cursor, (size_t)kv_dim * c * sizeof(uint16_t));
            raw_layers[i].v    = (uint16_t *)advance(&cursor, (size_t)kv_dim * c * sizeof(uint16_t));
            raw_layers[i].o    = (uint16_t *)advance(&cursor, (size_t)c * q_dim * sizeof(uint16_t));

            if (cfg->has_qk_norm) {
                lw->q_norm = (float *)advance(&cursor, hd * sizeof(float));
                lw->k_norm = (float *)advance(&cursor, hd * sizeof(float));
            }

            if (cfg->has_qkv_bias) {
                lw->q_bias = (float *)advance(&cursor, q_dim * sizeof(float));
                lw->k_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
                lw->v_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
            }

            lw->ln2_w = (float *)advance(&cursor, c * sizeof(float));

            raw_layers[i].gate = (uint16_t *)advance(&cursor, (size_t)ffn * c * sizeof(uint16_t));
            raw_layers[i].up   = (uint16_t *)advance(&cursor, (size_t)ffn * c * sizeof(uint16_t));
            raw_layers[i].down = (uint16_t *)advance(&cursor, (size_t)c * ffn * sizeof(uint16_t));
        }

        /* Convert F16 projections to F32 after cursor walk */
        for (int i = 0; i < cfg->n_layers; i++) {
            ba_layer_weights *lw = &m->layers[i];
            lw->q_proj    = f16_array_to_f32(raw_layers[i].q,    q_dim * c);
            lw->k_proj    = f16_array_to_f32(raw_layers[i].k,    kv_dim * c);
            lw->v_proj    = f16_array_to_f32(raw_layers[i].v,    kv_dim * c);
            lw->o_proj    = f16_array_to_f32(raw_layers[i].o,    c * q_dim);
            lw->gate_proj = f16_array_to_f32(raw_layers[i].gate, ffn * c);
            lw->up_proj   = f16_array_to_f32(raw_layers[i].up,   ffn * c);
            lw->down_proj = f16_array_to_f32(raw_layers[i].down, c * ffn);
        }

        uefi_free(raw_layers);
    }

    /* Final norm: [c] f32 — stays in raw buffer */
    m->ln_out_w = (float *)advance(&cursor, c * sizeof(float));

    /* LM head: [vocab * c] f16 — may or may not be present */
    size_t remaining = (size_t)file_size - (size_t)(cursor - (uint8_t *)data);
    size_t lm_head_size = (size_t)vocab * c * sizeof(uint16_t);
    uint16_t *raw_lm_head = 0;
    if (remaining >= lm_head_size) {
        raw_lm_head = (uint16_t *)advance(&cursor, lm_head_size);
        m->shared_embed_lmhead = 0;
    } else {
        m->shared_embed_lmhead = 1;
    }

    /* ---- Convert embed + lm_head from F16 to F32 (both paths) ---- */

    m->embed = f16_array_to_f32(raw_embed, vocab * c);

    if (m->shared_embed_lmhead) {
        m->lm_head = m->embed;
    } else {
        m->lm_head = f16_array_to_f32(raw_lm_head, vocab * c);
    }

    /* Initialize KV cache — use 2048 max context for Qwen3-0.6B */
    int kv_max = cfg->max_seq_len;
    if (kv_max > 2048) kv_max = 2048;  /* limit for memory */
    ba_kv_init(&m->kv, cfg->n_layers, kv_max, cfg->n_kv_heads, cfg->head_dim);

    /* Initialize workspace */
    ba_model_config ws_cfg = *cfg;
    ws_cfg.max_seq_len = kv_max;
    ba_workspace_init(&m->ws, &ws_cfg);

    m->position = 0;

    return m;
}

/* Simple int-to-string for progress messages */
static void loader_int_to_str(char *buf, int val) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    char tmp[12]; int i = 0;
    do { tmp[i++] = '0' + (val % 10); val /= 10; } while (val > 0);
    while (i > 0) *buf++ = tmp[--i];
    *buf = '\0';
}

/* ---- Streamed Q8 loader: reads file section-by-section, no monolithic buffer ----
 * Each projection/norm/bias gets its own small allocation (max ~9 MB).
 * Avoids the 3+ GB contiguous allocation that fails on fragmented UEFI memory. */

/* UCS-2 conversion for UEFI file paths */
static void loader_ascii_to_ucs2(const char *src, CHAR16 *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

/* Bounce buffer for safe file reads into reclaimed memory.
 * UEFI firmware uses Boot Services regions for filesystem/USB drivers.
 * If uefi_malloc falls back to reclaimed BS memory (ba_mem_alloc),
 * reading directly into it corrupts data because the firmware's own
 * driver code/buffers overlap. Solution: read into a small safe buffer
 * from AllocatePool, then memcpy to the final destination. */
static uint8_t *s_bounce_buf = 0;
static size_t s_bounce_size = 0;
#define BOUNCE_CHUNK (4 * 1024 * 1024)  /* 4 MB bounce buffer */

static void bounce_buf_init(void) {
    if (s_bounce_buf) return;
    EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, BOUNCE_CHUNK,
                                       (void **)&s_bounce_buf);
    if (st != EFI_SUCCESS) {
        s_bounce_buf = 0;
        efi_print_ascii("  WARNING: bounce buffer alloc failed\r\n");
    } else {
        s_bounce_size = BOUNCE_CHUNK;
    }
}

/* Read exactly 'size' bytes from file at current position into a new allocation.
 * Uses bounce buffer to avoid reading into reclaimed Boot Services memory. */
static void *stream_read_alloc(EFI_FILE_PROTOCOL *file, size_t size) {
    void *buf = uefi_malloc(size);
    if (!buf) return 0;

    bounce_buf_init();

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = size;
    while (remaining > 0) {
        if (s_bounce_buf) {
            /* Read into safe bounce buffer, then copy to destination */
            UINTN to_read = remaining < s_bounce_size
                            ? (UINTN)remaining : (UINTN)s_bounce_size;
            EFI_STATUS st = file->Read(file, &to_read, s_bounce_buf);
            if (st != EFI_SUCCESS || to_read == 0) {
                uefi_free(buf);
                return 0;
            }
            memcpy(dst, s_bounce_buf, to_read);
            dst += to_read;
            remaining -= to_read;
        } else {
            /* Fallback: direct read (may corrupt if in reclaimed memory) */
            UINTN to_read = remaining < (16 * 1024 * 1024)
                            ? (UINTN)remaining : (UINTN)(16 * 1024 * 1024);
            EFI_STATUS st = file->Read(file, &to_read, dst);
            if (st != EFI_SUCCESS || to_read == 0) {
                uefi_free(buf);
                return 0;
            }
            dst += to_read;
            remaining -= to_read;
        }
    }
    return buf;
}

/* Read Q8 weight: int8 data + float scales, each in their own allocation */
static int stream_read_q8(EFI_FILE_PROTOCOL *file, ba_q8_weight *w, int rows, int cols) {
    size_t data_size = (size_t)rows * cols;
    size_t scale_size = (size_t)rows * sizeof(float);

    w->data = (int8_t *)stream_read_alloc(file, data_size);
    if (!w->data) return 0;

    w->scales = (float *)stream_read_alloc(file, scale_size);
    if (!w->scales) { uefi_free(w->data); w->data = 0; return 0; }

    return 1;
}

static ba_model* ba_model_load_streamed(const char *path) {
    EFI_FILE_PROTOCOL *root = efi_fs_root();
    if (!root) return 0;

    CHAR16 wpath[256];
    loader_ascii_to_ucs2(path, wpath, 256);

    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = root->Open(root, &file, wpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) return 0;

    /* Read header */
    btw_header hdr;
    UINTN hdr_size = sizeof(btw_header);
    status = file->Read(file, &hdr_size, &hdr);
    if (status != EFI_SUCCESS || hdr_size < sizeof(btw_header)) {
        file->Close(file);
        return 0;
    }

    if (hdr.magic[0] != 'B' || hdr.magic[1] != 'T' ||
        hdr.magic[2] != 'A' || hdr.magic[3] != 'I' || hdr.version != 1) {
        file->Close(file);
        return 0;
    }

    /* Only use streamed loader for Q8 */
    if (hdr.quant != 2) {
        file->Close(file);
        return 0;
    }

    efi_print_ascii("  Streamed Q8 load: ");
    efi_print_ascii(path);
    efi_print_ascii("\r\n");

    /* Allocate model */
    ba_model *m = (ba_model *)uefi_calloc(1, sizeof(ba_model));
    if (!m) { file->Close(file); return 0; }

    m->raw_data = 0;  /* No monolithic buffer */
    m->raw_size = 0;
    m->quant = 2;

    /* Populate config */
    ba_model_config *cfg = &m->config;
    cfg->n_layers    = (int)hdr.n_layers;
    cfg->n_embd      = (int)hdr.n_embd;
    cfg->n_heads     = (int)hdr.n_heads;
    cfg->n_kv_heads  = (int)hdr.n_kv_heads;
    cfg->head_dim    = (int)hdr.head_dim;
    cfg->ffn_dim     = (int)hdr.ffn_dim;
    cfg->vocab_size  = (int)hdr.vocab_size;
    cfg->max_seq_len = (int)hdr.max_seq_len;
    cfg->rope_theta  = hdr.rope_theta;
    cfg->rms_eps     = hdr.rms_eps;
    cfg->has_qk_norm = (int)hdr.has_qk_norm;
    cfg->has_qkv_bias = (int)hdr.has_qkv_bias;

    switch (hdr.arch) {
    case 0: m->arch = MODEL_QWEN3; break;
    case 1: m->arch = MODEL_QWEN25; break;
    case 2: m->arch = MODEL_LLAMA; break;
    case 3: m->arch = MODEL_RWKVX; break;
    default: m->arch = MODEL_LLAMA; break;
    }

    int c        = cfg->n_embd;
    int q_dim    = cfg->n_heads * cfg->head_dim;
    int kv_dim   = cfg->n_kv_heads * cfg->head_dim;
    int ffn      = cfg->ffn_dim;
    int vocab    = cfg->vocab_size;
    int hd       = cfg->head_dim;

    /* Read embedding as F16 — keep as F16 to save ~600 MB.
     * Convert one row on-the-fly during embed lookup and lm_head matmul. */
    size_t embed_f16_size = (size_t)vocab * c * sizeof(uint16_t);
    m->embed_f16 = (uint16_t *)stream_read_alloc(file, embed_f16_size);
    if (!m->embed_f16) {
        efi_print_ascii("  ERROR: failed to read embed\r\n");
        uefi_free(m); file->Close(file); return 0;
    }
    m->embed = 0; /* F32 embed not used in streamed mode */

    char num[24];
    uint64_t fpos = 0;
    file->GetPosition(file, &fpos);
    loader_int_to_str(num, (int)(embed_f16_size / (1024 * 1024)));
    efi_print_ascii("  Embed: "); efi_print_ascii(num); efi_print_ascii(" MB @");
    loader_int_to_str(num, (int)(fpos / (1024 * 1024)));
    efi_print_ascii(num); efi_print_ascii("MB\r\n");

    /* Allocate layer weight structs */
    m->layers = (ba_layer_weights *)uefi_calloc(cfg->n_layers, sizeof(ba_layer_weights));
    if (!m->layers) {
        efi_print_ascii("  ERROR: failed to alloc layers\r\n");
        uefi_free(m->embed); uefi_free(m); file->Close(file); return 0;
    }

    /* Read each layer's weights individually */
    for (int i = 0; i < cfg->n_layers; i++) {
        ba_layer_weights *lw = &m->layers[i];

        /* input_layernorm: [c] f32 */
        lw->ln1_w = (float *)stream_read_alloc(file, c * sizeof(float));
        if (!lw->ln1_w) goto fail;

        /* Q8 attention projections */
        if (!stream_read_q8(file, &lw->q_proj_q8, q_dim, c)) goto fail;
        if (!stream_read_q8(file, &lw->k_proj_q8, kv_dim, c)) goto fail;
        if (!stream_read_q8(file, &lw->v_proj_q8, kv_dim, c)) goto fail;
        if (!stream_read_q8(file, &lw->o_proj_q8, c, q_dim)) goto fail;

        /* QK-Norm (optional) */
        if (cfg->has_qk_norm) {
            lw->q_norm = (float *)stream_read_alloc(file, hd * sizeof(float));
            lw->k_norm = (float *)stream_read_alloc(file, hd * sizeof(float));
            if (!lw->q_norm || !lw->k_norm) goto fail;
        }

        /* Q/K/V biases (optional) */
        if (cfg->has_qkv_bias) {
            lw->q_bias = (float *)stream_read_alloc(file, q_dim * sizeof(float));
            lw->k_bias = (float *)stream_read_alloc(file, kv_dim * sizeof(float));
            lw->v_bias = (float *)stream_read_alloc(file, kv_dim * sizeof(float));
            if (!lw->q_bias || !lw->k_bias || !lw->v_bias) goto fail;
        }

        /* post_attention_layernorm: [c] f32 */
        lw->ln2_w = (float *)stream_read_alloc(file, c * sizeof(float));
        if (!lw->ln2_w) goto fail;

        /* Q8 FFN projections */
        if (!stream_read_q8(file, &lw->gate_proj_q8, ffn, c)) goto fail;
        if (!stream_read_q8(file, &lw->up_proj_q8, ffn, c)) goto fail;
        if (!stream_read_q8(file, &lw->down_proj_q8, c, ffn)) goto fail;

        /* Layer 0 sanity check: print ln1_w[0] to verify data integrity */
        if (i == 0) {
            int ln1_val = (int)(lw->ln1_w[0] * 10000.0f);
            efi_print_ascii("  L0 ln1_w[0]=");
            if (ln1_val < 0) { efi_print_ascii("-"); ln1_val = -ln1_val; }
            loader_int_to_str(num, ln1_val);
            efi_print_ascii(num);
            efi_print_ascii("e-4 q8[0]=");
            loader_int_to_str(num, (int)lw->q_proj_q8.data[0]);
            efi_print_ascii(num);
            efi_print_ascii(" sc=");
            int sc_val = (int)(lw->q_proj_q8.scales[0] * 100000.0f);
            loader_int_to_str(num, sc_val);
            efi_print_ascii(num);
            efi_print_ascii("e-5\r\n");
        }

        if ((i + 1) % 6 == 0 || i == cfg->n_layers - 1) {
            loader_int_to_str(num, i + 1);
            efi_print_ascii("  Layer ");
            efi_print_ascii(num);
            efi_print_ascii("/");
            loader_int_to_str(num, cfg->n_layers);
            efi_print_ascii(num);
            efi_print_ascii("\r\n");
        }
    }

    /* Final norm: [c] f32 */
    m->ln_out_w = (float *)stream_read_alloc(file, c * sizeof(float));
    if (!m->ln_out_w) goto fail;

    /* LM head: quantize F16 embed to Q8 for fast SSE matvec (~297 MB).
     * Much faster than F16 on-the-fly conversion, uses far less memory than F32. */
    m->lm_head = 0;
    m->shared_embed_lmhead = 1;

    /* lm_head uses F16 matvec directly — no Q8 quantization needed.
     * Saves minutes of load time, costs <1% inference speed (1 matvec per token). */
    efi_print_ascii("  LM head: F16 (shared with embed)\r\n");

    file->Close(file);

    /* Initialize KV cache */
    int kv_max = cfg->max_seq_len;
    if (kv_max > 2048) kv_max = 2048;
    ba_kv_init(&m->kv, cfg->n_layers, kv_max, cfg->n_kv_heads, cfg->head_dim);

    /* Initialize workspace */
    ba_model_config ws_cfg = *cfg;
    ws_cfg.max_seq_len = kv_max;
    ba_workspace_init(&m->ws, &ws_cfg);

    m->position = 0;

    /* Debug: dump full config to verify header was read correctly */
    efi_print_ascii("  Config: heads=");
    loader_int_to_str(num, cfg->n_heads); efi_print_ascii(num);
    efi_print_ascii(" kv_heads=");
    loader_int_to_str(num, cfg->n_kv_heads); efi_print_ascii(num);
    efi_print_ascii(" head_dim=");
    loader_int_to_str(num, cfg->head_dim); efi_print_ascii(num);
    efi_print_ascii(" ffn=");
    loader_int_to_str(num, cfg->ffn_dim); efi_print_ascii(num);
    efi_print_ascii("\r\n");
    efi_print_ascii("  Config: qk_norm=");
    loader_int_to_str(num, cfg->has_qk_norm); efi_print_ascii(num);
    efi_print_ascii(" qkv_bias=");
    loader_int_to_str(num, cfg->has_qkv_bias); efi_print_ascii(num);
    efi_print_ascii(" arch=");
    loader_int_to_str(num, (int)m->arch); efi_print_ascii(num);
    efi_print_ascii("\r\n");

    /* Pool usage diagnostic */
    efi_print_ascii("  Pool: ");
    loader_int_to_str(num, (int)(ba_mem_pool_used() / (1024 * 1024)));
    efi_print_ascii(num); efi_print_ascii("/");
    loader_int_to_str(num, (int)(ba_mem_pool_total() / (1024 * 1024)));
    efi_print_ascii(num); efi_print_ascii(" MB used (");
    loader_int_to_str(num, ba_mem_pool_regions());
    efi_print_ascii(num); efi_print_ascii(" regions)\r\n");

    efi_print_ascii("  Streamed load complete.\r\n");
    return m;

fail:
    efi_print_ascii("  ERROR: streamed load failed at layer\r\n");
    file->Close(file);
    /* Free whatever was allocated */
    ba_model_free(m);
    return 0;
}

ba_model* ba_model_load(const char *path) {
    /* Initialize filesystem if needed */
    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: filesystem init failed\r\n");
        return 0;
    }

    /* Try monolithic read first (works for small models) */
    void *data = 0;
    UINTN file_size = 0;
    EFI_STATUS status = efi_read_file(path, &data, &file_size);
    if (status == EFI_SUCCESS && data) {
        return ba_model_load_buffer(data, file_size);
    }

    /* Monolithic allocation failed — try streamed Q8 loader */
    efi_print_ascii("  Monolithic load failed, trying streamed loader...\r\n");
    return ba_model_load_streamed(path);
}

ba_model_arch ba_model_arch_of(const ba_model *m) {
    return m->arch;
}

int ba_model_vocab_size(const ba_model *m) {
    return m->config.vocab_size;
}

void ba_model_reset(ba_model *m) {
    ba_kv_reset(&m->kv);
    m->position = 0;
}

/* Free a Q8 weight (data + scales) if individually allocated */
static void free_q8_weight(ba_q8_weight *w) {
    if (w->data)   uefi_free(w->data);
    if (w->scales) uefi_free(w->scales);
    w->data = 0;
    w->scales = 0;
}

void ba_model_free(ba_model *m) {
    if (!m) return;

    if (m->embed) uefi_free(m->embed);
    if (m->embed_f16) uefi_free(m->embed_f16);
    free_q8_weight(&m->lm_head_q8);

    if (m->quant == 2 && !m->raw_data) {
        /* Streamed Q8: each projection/norm/bias individually allocated */
        for (int i = 0; i < m->config.n_layers; i++) {
            ba_layer_weights *lw = &m->layers[i];
            if (lw->ln1_w) uefi_free(lw->ln1_w);
            free_q8_weight(&lw->q_proj_q8);
            free_q8_weight(&lw->k_proj_q8);
            free_q8_weight(&lw->v_proj_q8);
            free_q8_weight(&lw->o_proj_q8);
            if (lw->q_norm) uefi_free(lw->q_norm);
            if (lw->k_norm) uefi_free(lw->k_norm);
            if (lw->q_bias) uefi_free(lw->q_bias);
            if (lw->k_bias) uefi_free(lw->k_bias);
            if (lw->v_bias) uefi_free(lw->v_bias);
            if (lw->ln2_w) uefi_free(lw->ln2_w);
            free_q8_weight(&lw->gate_proj_q8);
            free_q8_weight(&lw->up_proj_q8);
            free_q8_weight(&lw->down_proj_q8);
        }
        if (m->ln_out_w) uefi_free(m->ln_out_w);
    } else if (m->quant != 2) {
        /* F16 path: F32 projections were separately allocated */
        for (int i = 0; i < m->config.n_layers; i++) {
            ba_layer_weights *lw = &m->layers[i];
            if (lw->q_proj)    uefi_free(lw->q_proj);
            if (lw->k_proj)    uefi_free(lw->k_proj);
            if (lw->v_proj)    uefi_free(lw->v_proj);
            if (lw->o_proj)    uefi_free(lw->o_proj);
            if (lw->gate_proj) uefi_free(lw->gate_proj);
            if (lw->up_proj)   uefi_free(lw->up_proj);
            if (lw->down_proj) uefi_free(lw->down_proj);
        }
    }
    /* Q8 monolithic path (raw_data != NULL): projections point into raw_data, don't free them */

    if (!m->shared_embed_lmhead && m->lm_head)
        uefi_free(m->lm_head);

    ba_kv_free(&m->kv);
    ba_workspace_free(&m->ws);
    uefi_free(m->layers);
    if (m->raw_data) {
        UINTN pages = (m->raw_size + 4095) / 4096;
        gBS->FreePages((uint64_t)(UINTN)m->raw_data, pages);
    }
    uefi_free(m);
}

/* ba_model_forward dispatches to architecture-specific forward */
float* ba_model_forward(ba_model *m, const int *tokens, int n_tokens) {
    float *logits = 0;
    for (int i = 0; i < n_tokens; i++) {
        logits = ba_qwen_forward(m, tokens[i]);
    }
    return logits;
}
