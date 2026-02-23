/*
 * qwen2.c - Qwen3/Qwen2 transformer forward pass
 *
 * Standard transformer: RMSNorm + GQA + SwiGLU + RoPE.
 * All operations on raw float* arrays — no tm_tensor in hot path.
 * INT8 quantized matmul for weight projections.
 *
 * Ported from foundry/rwkvx/rfa.c with modifications for bare-metal.
 */

#include "model.h"
#include "../efi/efi.h"

/* Math declarations (provided by uefi_libc.c) */
float sqrtf(float x);
float expf(float x);

/* Freestanding libc (provided by uefi_libc.c) */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

/* ---- f16 conversion ---- */

static inline float f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    if (exp == 0) {
        if (mant == 0) {
            union { uint32_t i; float f; } u = { .i = sign };
            return u.f;
        }
        /* Denormalized: convert to normalized */
        while (!(mant & 0x400)) { mant <<= 1; exp--; }
        exp++; mant &= 0x3FF;
    } else if (exp == 31) {
        /* Inf/NaN */
        union { uint32_t i; float f; } u = { .i = sign | 0x7F800000 | (mant << 13) };
        return u.f;
    }

    union { uint32_t i; float f; } u = {
        .i = sign | ((exp + 112) << 23) | (mant << 13)
    };
    return u.f;
}

/* ---- Core math ops (ported from rfa.c) ---- */

static inline float fast_silu(float x) {
    return x / (1.0f + expf(-x));
}

static void fast_rms_norm(const float *x, const float *w, float *out,
                          int n, float eps) {
    float sum_sq = 0.0f;
    for (int i = 0; i < n; i++)
        sum_sq += x[i] * x[i];
    float scale = 1.0f / sqrtf(sum_sq / n + eps);
    for (int i = 0; i < n; i++)
        out[i] = x[i] * scale * w[i];
}

/* Per-head RMSNorm for QK-Norm (Qwen3) */
static void rms_norm_per_head(float *vec, const float *w, int n_heads,
                               int head_dim, float eps) {
    for (int h = 0; h < n_heads; h++) {
        float *head = &vec[h * head_dim];
        float sum_sq = 0.0f;
        for (int i = 0; i < head_dim; i++)
            sum_sq += head[i] * head[i];
        float scale = 1.0f / sqrtf(sum_sq / head_dim + eps);
        for (int i = 0; i < head_dim; i++)
            head[i] = head[i] * scale * w[i];
    }
}

/* ---- INT8 quantized matvec (ported from rfa.c) ---- */

/* Q8 weight × F32 activation matvec: out[rows] = W_q8[rows,cols] @ x_f32[cols]
 * W8A32: only weights are quantized, activations stay float32.
 * W8A8 (both quantized) is too lossy for small models (135M). */
static void matvec_q8(const float *x, const ba_q8_weight *q, float *out,
                       int rows, int cols, int8_t *q8_buf) {
    (void)q8_buf;  /* not needed for W8A32 */

    for (int i = 0; i < rows; i++) {
        const int8_t *row = &q->data[i * cols];
        float w_scale = q->scales[i];
        float dot = 0.0f;
        for (int j = 0; j < cols; j++)
            dot += x[j] * (float)row[j];
        out[i] = dot * w_scale;
    }
}

/* f16 matvec for embedding/lm_head: out[rows] = W_f16[rows,cols] @ x[cols] */
static void matvec_f16(const float *x, const uint16_t *w_f16, float *out,
                        int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        const uint16_t *row = &w_f16[(size_t)i * cols];
        float sum = 0.0f;
        for (int j = 0; j < cols; j++)
            sum += f16_to_f32(row[j]) * x[j];
        out[i] = sum;
    }
}

/* ---- Qwen forward pass ---- */

float* ba_qwen_forward(ba_model *m, int token) {
    ba_model_config *cfg = &m->config;
    ba_workspace *ws = &m->ws;
    int c       = cfg->n_embd;
    int n_q     = cfg->n_heads;
    int n_kv    = cfg->n_kv_heads;
    int hd      = cfg->head_dim;
    int kv_dim  = n_kv * hd;
    int q_dim   = n_q * hd;
    int ffn     = cfg->ffn_dim;
    int group   = n_q / n_kv;
    int pos     = m->position;

    /* Step 1: Embedding lookup (f16 → f32) */
    const uint16_t *emb_row = &m->embed[(size_t)token * c];
    for (int i = 0; i < c; i++)
        ws->x[i] = f16_to_f32(emb_row[i]);

    /* Step 2: Transformer layers */
    for (int layer = 0; layer < cfg->n_layers; layer++) {
        ba_layer_weights *lw = &m->layers[layer];

        /* 2a: RMSNorm (pre-attention) */
        fast_rms_norm(ws->x, lw->ln1_w, ws->ln_buf, c, cfg->rms_eps);

        /* 2b: Q/K/V projections (F16 matvec) */
        matvec_f16(ws->ln_buf, lw->q_proj, ws->q, q_dim, c);
        matvec_f16(ws->ln_buf, lw->k_proj, ws->k, kv_dim, c);
        matvec_f16(ws->ln_buf, lw->v_proj, ws->v, kv_dim, c);

        /* 2b': Add bias if present (Qwen2.5) */
        if (lw->q_bias) {
            for (int i = 0; i < q_dim; i++) ws->q[i] += lw->q_bias[i];
            for (int i = 0; i < kv_dim; i++) ws->k[i] += lw->k_bias[i];
            for (int i = 0; i < kv_dim; i++) ws->v[i] += lw->v_bias[i];
        }

        /* 2c: QK-Norm (Qwen3) */
        if (cfg->has_qk_norm && lw->q_norm) {
            rms_norm_per_head(ws->q, lw->q_norm, n_q, hd, cfg->rms_eps);
            rms_norm_per_head(ws->k, lw->k_norm, n_kv, hd, cfg->rms_eps);
        }

        /* 2d: RoPE on Q and K */
        ba_rope_apply(ws->q, n_q, hd, pos, cfg->rope_theta);
        ba_rope_apply(ws->k, n_kv, hd, pos, cfg->rope_theta);

        /* 2e: Append K,V to cache */
        ba_kv_append(&m->kv, layer, ws->k, ws->v);

        /* 2f: GQA attention */
        ba_kv_layer *kv_l = &m->kv.layers[layer];
        int total = kv_l->cache_len;
        float scale = 1.0f / sqrtf((float)hd);

        memset(ws->attn_out, 0, q_dim * sizeof(float));

        for (int qh = 0; qh < n_q; qh++) {
            int kvh = qh / group;
            float *q_head = &ws->q[qh * hd];
            float *scores = ws->attn_scores;

            /* Compute attention scores */
            float max_s = -1e30f;
            for (int t = 0; t < total; t++) {
                float *kt = &kv_l->k_cache[t * kv_dim + kvh * hd];
                float dot = 0.0f;
                for (int j = 0; j < hd; j++)
                    dot += q_head[j] * kt[j];
                scores[t] = dot * scale;
                if (scores[t] > max_s) max_s = scores[t];
            }

            /* Softmax */
            float sum_e = 0.0f;
            for (int t = 0; t < total; t++) {
                scores[t] = expf(scores[t] - max_s);
                sum_e += scores[t];
            }
            float inv_sum = 1.0f / sum_e;
            for (int t = 0; t < total; t++)
                scores[t] *= inv_sum;

            /* Value aggregation */
            float *y_head = &ws->attn_out[qh * hd];
            for (int t = 0; t < total; t++) {
                float *vt = &kv_l->v_cache[t * kv_dim + kvh * hd];
                float s = scores[t];
                for (int j = 0; j < hd; j++)
                    y_head[j] += s * vt[j];
            }
        }

        /* 2g: Output projection + residual */
        matvec_f16(ws->attn_out, lw->o_proj, ws->x_out, c, q_dim);
        for (int i = 0; i < c; i++)
            ws->x[i] += ws->x_out[i];

        /* 2h: RMSNorm (pre-FFN) */
        fast_rms_norm(ws->x, lw->ln2_w, ws->ln_buf, c, cfg->rms_eps);

        /* 2i: SwiGLU FFN */
        matvec_f16(ws->ln_buf, lw->gate_proj, ws->ffn_gate, ffn, c);
        matvec_f16(ws->ln_buf, lw->up_proj, ws->ffn_up, ffn, c);

        for (int i = 0; i < ffn; i++)
            ws->ffn_gate[i] = fast_silu(ws->ffn_gate[i]) * ws->ffn_up[i];

        matvec_f16(ws->ffn_gate, lw->down_proj, ws->ffn_out, c, ffn);

        /* FFN residual */
        for (int i = 0; i < c; i++)
            ws->x[i] += ws->ffn_out[i];
    }

    /* Step 3: Final RMSNorm */
    fast_rms_norm(ws->x, m->ln_out_w, ws->ln_buf, c, cfg->rms_eps);

    /* Step 4: LM head (f16 matvec → logits) */
    matvec_f16(ws->ln_buf, m->lm_head, ws->logits, cfg->vocab_size, c);

    m->position++;
    return ws->logits;
}
