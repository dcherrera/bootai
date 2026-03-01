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

    int c        = cfg->n_embd;
    int q_dim    = cfg->n_heads * cfg->head_dim;
    int kv_dim   = cfg->n_kv_heads * cfg->head_dim;
    int ffn      = cfg->ffn_dim;
    int vocab    = cfg->vocab_size;
    int hd       = cfg->head_dim;

    /* Walk contiguous weight data after header */
    uint8_t *cursor = (uint8_t *)data + sizeof(btw_header);

    /* Embedding: [vocab * c] f16 */
    m->embed = (uint16_t *)advance(&cursor, (size_t)vocab * c * sizeof(uint16_t));

    /* Allocate layer weight structs */
    m->layers = (ba_layer_weights *)uefi_calloc(cfg->n_layers, sizeof(ba_layer_weights));

    for (int i = 0; i < cfg->n_layers; i++) {
        ba_layer_weights *lw = &m->layers[i];

        /* input_layernorm.weight: [c] f32 */
        lw->ln1_w = (float *)advance(&cursor, c * sizeof(float));

        /* q_proj: [q_dim, c] F16 */
        lw->q_proj = (uint16_t *)advance(&cursor, (size_t)q_dim * c * sizeof(uint16_t));

        /* k_proj: [kv_dim, c] F16 */
        lw->k_proj = (uint16_t *)advance(&cursor, (size_t)kv_dim * c * sizeof(uint16_t));

        /* v_proj: [kv_dim, c] F16 */
        lw->v_proj = (uint16_t *)advance(&cursor, (size_t)kv_dim * c * sizeof(uint16_t));

        /* o_proj: [c, q_dim] F16 */
        lw->o_proj = (uint16_t *)advance(&cursor, (size_t)c * q_dim * sizeof(uint16_t));

        /* QK-Norm (optional) */
        if (cfg->has_qk_norm) {
            lw->q_norm = (float *)advance(&cursor, hd * sizeof(float));
            lw->k_norm = (float *)advance(&cursor, hd * sizeof(float));
        }

        /* Q/K/V biases (optional, Qwen2.5 has bias, Qwen3 does not) */
        if (cfg->has_qkv_bias) {
            lw->q_bias = (float *)advance(&cursor, q_dim * sizeof(float));
            lw->k_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
            lw->v_bias = (float *)advance(&cursor, kv_dim * sizeof(float));
        }

        /* post_attention_layernorm.weight: [c] f32 */
        lw->ln2_w = (float *)advance(&cursor, c * sizeof(float));

        /* gate_proj: [ffn, c] F16 */
        lw->gate_proj = (uint16_t *)advance(&cursor, (size_t)ffn * c * sizeof(uint16_t));

        /* up_proj: [ffn, c] F16 */
        lw->up_proj = (uint16_t *)advance(&cursor, (size_t)ffn * c * sizeof(uint16_t));

        /* down_proj: [c, ffn] F16 */
        lw->down_proj = (uint16_t *)advance(&cursor, (size_t)c * ffn * sizeof(uint16_t));
    }

    /* Final norm: [c] f32 */
    m->ln_out_w = (float *)advance(&cursor, c * sizeof(float));

    /* LM head: [vocab * c] f16 — may or may not be present */
    size_t remaining = (size_t)file_size - (size_t)(cursor - (uint8_t *)data);
    size_t lm_head_size = (size_t)vocab * c * sizeof(uint16_t);
    if (remaining >= lm_head_size) {
        m->lm_head = (uint16_t *)advance(&cursor, lm_head_size);
        m->shared_embed_lmhead = 0;
    } else {
        /* Shared with embedding (tied weights) */
        m->lm_head = m->embed;
        m->shared_embed_lmhead = 1;
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

ba_model* ba_model_load(const char *path) {
    /* Initialize filesystem if needed */
    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: filesystem init failed\r\n");
        return 0;
    }

    /* Read entire file */
    void *data = 0;
    UINTN file_size = 0;
    EFI_STATUS status = efi_read_file(path, &data, &file_size);
    if (status != EFI_SUCCESS || !data) {
        efi_print_ascii("  ERROR: cannot read weight file\r\n");
        return 0;
    }

    return ba_model_load_buffer(data, file_size);
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

void ba_model_free(ba_model *m) {
    if (!m) return;
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
