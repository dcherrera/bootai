/*
 * qwen2.c - Qwen3/Qwen2 transformer forward pass
 *
 * Standard transformer: RMSNorm + GQA + SwiGLU + RoPE.
 * All operations on raw float* arrays — no tm_tensor in hot path.
 * Weights pre-converted from F16 to F32 at load time for fast SSE matvec.
 *
 * Ported from foundry/rwkvx/rfa.c with modifications for bare-metal.
 */

#include "model.h"
#include "../efi/efi.h"

/* Math declarations (provided by uefi_libc.c) */
float sqrtf(float x);
float expf(float x);
float fabsf(float x);

/* Freestanding libc (provided by uefi_libc.c) */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

/* ---- SSE vector types (GCC vector extensions, no immintrin.h needed) ---- */

typedef float    v4sf __attribute__((__vector_size__(16)));
typedef int      v4si __attribute__((__vector_size__(16)));
typedef long long v2di __attribute__((__vector_size__(16)));

/* SSE intrinsic equivalents via GCC builtins */
static inline v4sf sse_load(const float *p) {
    return __builtin_ia32_loadups(p);
}
static inline void sse_store(float *p, v4sf v) {
    __builtin_ia32_storeups(p, v);
}
static inline v4sf sse_set1(float x) {
    return (v4sf){x, x, x, x};
}
static inline v4sf sse_setzero(void) {
    return (v4sf){0.0f, 0.0f, 0.0f, 0.0f};
}
static inline v4sf sse_mul(v4sf a, v4sf b) { return a * b; }
static inline v4sf sse_add(v4sf a, v4sf b) { return a + b; }

/* Horizontal sum of 4 floats */
static inline float sse_hsum(v4sf v) {
    /* v = {a, b, c, d} */
    v4sf shuf = __builtin_ia32_shufps(v, v, 0x4E); /* {c, d, a, b} */
    v4sf sums = v + shuf;                           /* {a+c, b+d, ...} */
    shuf = __builtin_ia32_shufps(sums, sums, 0xB1); /* {b+d, a+c, ...} */
    sums = sums + shuf;                              /* {a+b+c+d, ...} */
    return sums[0];
}

/* f16_to_f32 is now in model.h (shared with loader.c) */

/* ---- Core math ops (ported from rfa.c) ---- */

/* SiLU: x * sigmoid(x) = x / (1 + exp(-x)).
 * Uses the Cody-Waite expf from uefi_libc — accurate to float precision.
 * Cost is ~20 FLOPs vs ~3 for the old Padé, but negligible next to matvecs. */
static inline float fast_silu(float x) {
    if (x > 10.0f) return x;      /* sigmoid ≈ 1 */
    if (x < -10.0f) return 0.0f;  /* sigmoid ≈ 0 */
    return x / (1.0f + expf(-x));
}

static void fast_rms_norm(const float *x, const float *w, float *out,
                          int n, float eps) {
    /* SSE 4-wide sum of squares — dual accumulator to hide FP add latency */
    v4sf acc0 = sse_setzero(), acc1 = sse_setzero();
    int i = 0;
    for (; i + 7 < n; i += 8) {
        v4sf x0 = sse_load(&x[i]);
        v4sf x1 = sse_load(&x[i + 4]);
        acc0 = sse_add(acc0, sse_mul(x0, x0));
        acc1 = sse_add(acc1, sse_mul(x1, x1));
    }
    v4sf acc = sse_add(acc0, acc1);
    float sum_sq = sse_hsum(acc);
    for (; i < n; i++)
        sum_sq += x[i] * x[i];

    v4sf scale_v = sse_set1(1.0f / sqrtf(sum_sq / n + eps));

    /* SSE 4-wide scale + multiply */
    i = 0;
    for (; i + 3 < n; i += 4) {
        v4sf xv = sse_load(&x[i]);
        v4sf wv = sse_load(&w[i]);
        sse_store(&out[i], sse_mul(sse_mul(xv, scale_v), wv));
    }
    float scale = scale_v[0];
    for (; i < n; i++)
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
 * SSE vectorized: int8→int32 sign extension + cvtdq2ps, 8-wide unroll.
 * GCC compiles v4si = {row[j]..row[j+3]} into pmovsxbd at -O2+. */
static void matvec_q8(const float *x, const ba_q8_weight *q, float *out,
                       int rows, int cols, int8_t *q8_buf) {
    (void)q8_buf;  /* not needed for W8A32 */

    for (int i = 0; i < rows; i++) {
        const int8_t *row = &q->data[(size_t)i * cols];
        v4sf acc0 = sse_setzero(), acc1 = sse_setzero();
        v4sf acc2 = sse_setzero(), acc3 = sse_setzero();
        int j = 0;
        /* 16-wide unroll (4 accumulators) + prefetch next cache line */
        for (; j + 15 < cols; j += 16) {
            __builtin_prefetch(&row[j + 64], 0, 0);
            __builtin_prefetch(&x[j + 64], 0, 0);
            v4si w0_i = {row[j],    row[j+1],  row[j+2],  row[j+3]};
            acc0 = sse_add(acc0, sse_mul(__builtin_ia32_cvtdq2ps(w0_i),
                                          sse_load(&x[j])));
            v4si w1_i = {row[j+4],  row[j+5],  row[j+6],  row[j+7]};
            acc1 = sse_add(acc1, sse_mul(__builtin_ia32_cvtdq2ps(w1_i),
                                          sse_load(&x[j+4])));
            v4si w2_i = {row[j+8],  row[j+9],  row[j+10], row[j+11]};
            acc2 = sse_add(acc2, sse_mul(__builtin_ia32_cvtdq2ps(w2_i),
                                          sse_load(&x[j+8])));
            v4si w3_i = {row[j+12], row[j+13], row[j+14], row[j+15]};
            acc3 = sse_add(acc3, sse_mul(__builtin_ia32_cvtdq2ps(w3_i),
                                          sse_load(&x[j+12])));
        }
        /* 8-wide cleanup */
        for (; j + 7 < cols; j += 8) {
            v4si w0_i = {row[j], row[j+1], row[j+2], row[j+3]};
            acc0 = sse_add(acc0, sse_mul(__builtin_ia32_cvtdq2ps(w0_i),
                                          sse_load(&x[j])));
            v4si w1_i = {row[j+4], row[j+5], row[j+6], row[j+7]};
            acc1 = sse_add(acc1, sse_mul(__builtin_ia32_cvtdq2ps(w1_i),
                                          sse_load(&x[j+4])));
        }
        float sum = sse_hsum(sse_add(sse_add(acc0, acc1), sse_add(acc2, acc3)));
        for (; j < cols; j++)
            sum += (float)row[j] * x[j];
        out[i] = sum * q->scales[i];
    }
}

/* F32 matvec: out[rows] = W[rows,cols] @ x[cols]
 * Weights pre-converted to F32 at load — pure SSE, no F16 conversion.
 * Dual accumulators (8-wide unroll) to hide FP add latency. */
static void matvec_f32(const float *x, const float *w, float *out,
                        int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        const float *row = &w[(size_t)i * cols];
        v4sf acc0 = sse_setzero(), acc1 = sse_setzero();
        int j = 0;
        for (; j + 7 < cols; j += 8) {
            acc0 = sse_add(acc0, sse_mul(sse_load(&row[j]),     sse_load(&x[j])));
            acc1 = sse_add(acc1, sse_mul(sse_load(&row[j + 4]), sse_load(&x[j + 4])));
        }
        float sum = sse_hsum(sse_add(acc0, acc1));
        for (; j < cols; j++)
            sum += row[j] * x[j];
        out[i] = sum;
    }
}

/* F16 matvec: out[rows] = W_f16[rows,cols] @ x[cols]
 * Converts F16→F32 per element on-the-fly. Used for lm_head when embed
 * is kept as F16 (streamed Q8 loader saves ~600 MB).
 * SSE dual-accumulator with 8-wide unroll — same structure as matvec_f32
 * but with f16_to_f32 conversion per weight element. */
static void matvec_f16(const float *x, const uint16_t *w, float *out,
                        int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        const uint16_t *row = &w[(size_t)i * cols];
        v4sf acc0 = sse_setzero(), acc1 = sse_setzero();
        int j = 0;
        for (; j + 7 < cols; j += 8) {
            v4sf w0 = {f16_to_f32(row[j]),   f16_to_f32(row[j+1]),
                       f16_to_f32(row[j+2]), f16_to_f32(row[j+3])};
            acc0 = sse_add(acc0, sse_mul(w0, sse_load(&x[j])));
            v4sf w1 = {f16_to_f32(row[j+4]), f16_to_f32(row[j+5]),
                       f16_to_f32(row[j+6]), f16_to_f32(row[j+7])};
            acc1 = sse_add(acc1, sse_mul(w1, sse_load(&x[j+4])));
        }
        float sum = sse_hsum(sse_add(acc0, acc1));
        for (; j < cols; j++)
            sum += f16_to_f32(row[j]) * x[j];
        out[i] = sum;
    }
}

/* Dispatch to Q8 or F32 matvec based on model quantization */
static inline void matvec_dispatch(const float *x, const float *w_f32,
                                    const ba_q8_weight *w_q8, float *out,
                                    int rows, int cols, int quant,
                                    int8_t *q8_buf) {
    if (quant == 2)
        matvec_q8(x, w_q8, out, rows, cols, q8_buf);
    else
        matvec_f32(x, w_f32, out, rows, cols);
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
    int quant   = m->quant;
    int pos     = m->position;

    /* Step 1: Embedding lookup */
    if (m->embed) {
        /* F32 path (monolithic load) */
        const float *emb_row = &m->embed[(size_t)token * c];
        memcpy(ws->x, emb_row, c * sizeof(float));
    } else {
        /* F16 path (streamed load) — convert one row on-the-fly */
        const uint16_t *emb_row = &m->embed_f16[(size_t)token * c];
        for (int i = 0; i < c; i++)
            ws->x[i] = f16_to_f32(emb_row[i]);
    }

    /* Precompute attention scale — hd is constant across all layers */
    float attn_scale = 1.0f / sqrtf((float)hd);

    /* Step 2: Transformer layers */
    for (int layer = 0; layer < cfg->n_layers; layer++) {
        ba_layer_weights *lw = &m->layers[layer];

        /* 2a: RMSNorm (pre-attention) */
        fast_rms_norm(ws->x, lw->ln1_w, ws->ln_buf, c, cfg->rms_eps);

        /* 2b: Q/K/V projections */
        matvec_dispatch(ws->ln_buf, lw->q_proj, &lw->q_proj_q8, ws->q,
                        q_dim, c, quant, ws->q8_buf);
        matvec_dispatch(ws->ln_buf, lw->k_proj, &lw->k_proj_q8, ws->k,
                        kv_dim, c, quant, ws->q8_buf);
        matvec_dispatch(ws->ln_buf, lw->v_proj, &lw->v_proj_q8, ws->v,
                        kv_dim, c, quant, ws->q8_buf);

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

        /* 2d: RoPE on Q and K (cached table lookup if available) */
        if (ws->rope_cos && pos < ws->rope_max_seq) {
            ba_rope_apply_cached(ws->q, n_q, hd, pos,
                                 ws->rope_cos, ws->rope_sin, ws->rope_half_dim);
            ba_rope_apply_cached(ws->k, n_kv, hd, pos,
                                 ws->rope_cos, ws->rope_sin, ws->rope_half_dim);
        } else {
            ba_rope_apply(ws->q, n_q, hd, pos, cfg->rope_theta);
            ba_rope_apply(ws->k, n_kv, hd, pos, cfg->rope_theta);
        }

        /* 2e: Append K,V to cache */
        ba_kv_append(&m->kv, layer, ws->k, ws->v);

        /* 2f: GQA attention */
        ba_kv_layer *kv_l = &m->kv.layers[layer];
        int total = kv_l->cache_len;

        memset(ws->attn_out, 0, q_dim * sizeof(float));

        for (int qh = 0; qh < n_q; qh++) {
            int kvh = qh / group;
            float *q_head = &ws->q[qh * hd];
            float *scores = ws->attn_scores;

            /* Compute attention scores (SSE 4-wide dot products) */
            float max_s = -1e30f;
            for (int t = 0; t < total; t++) {
                int idx = (kv_l->cache_start + t) % kv_l->max_len;
                float *kt = &kv_l->k_cache[idx * kv_dim + kvh * hd];
                v4sf acc = sse_setzero();
                int j = 0;
                for (; j + 3 < hd; j += 4)
                    acc = sse_add(acc, sse_mul(sse_load(&q_head[j]),
                                               sse_load(&kt[j])));
                float dot = sse_hsum(acc);
                for (; j < hd; j++)
                    dot += q_head[j] * kt[j];
                scores[t] = dot * attn_scale;
                if (scores[t] > max_s) max_s = scores[t];
            }

            /* Softmax (accurate expf — fast_expf's ~12-bit errors compound
             * across 24 layers and 100+ autoregressive steps, causing
             * degenerate repetition with stochastic sampling) */
            float sum_e = 0.0f;
            for (int t = 0; t < total; t++) {
                scores[t] = expf(scores[t] - max_s);
                sum_e += scores[t];
            }
            float inv_sum = 1.0f / sum_e;
            for (int t = 0; t < total; t++)
                scores[t] *= inv_sum;

            /* Value aggregation (SSE 4-wide) */
            float *y_head = &ws->attn_out[qh * hd];
            for (int t = 0; t < total; t++) {
                int vidx = (kv_l->cache_start + t) % kv_l->max_len;
                float *vt = &kv_l->v_cache[vidx * kv_dim + kvh * hd];
                v4sf sv = sse_set1(scores[t]);
                int j = 0;
                for (; j + 3 < hd; j += 4)
                    sse_store(&y_head[j],
                        sse_add(sse_load(&y_head[j]),
                                sse_mul(sv, sse_load(&vt[j]))));
                float s = scores[t];
                for (; j < hd; j++)
                    y_head[j] += s * vt[j];
            }
        }

        /* 2g: Output projection + residual */
        matvec_dispatch(ws->attn_out, lw->o_proj, &lw->o_proj_q8, ws->x_out,
                        c, q_dim, quant, ws->q8_buf);
        for (int i = 0; i < c; i++)
            ws->x[i] += ws->x_out[i];

        /* 2h: RMSNorm (pre-FFN) */
        fast_rms_norm(ws->x, lw->ln2_w, ws->ln_buf, c, cfg->rms_eps);

        /* 2i: SwiGLU FFN */
        matvec_dispatch(ws->ln_buf, lw->gate_proj, &lw->gate_proj_q8,
                        ws->ffn_gate, ffn, c, quant, ws->q8_buf);
        matvec_dispatch(ws->ln_buf, lw->up_proj, &lw->up_proj_q8,
                        ws->ffn_up, ffn, c, quant, ws->q8_buf);

        for (int i = 0; i < ffn; i++)
            ws->ffn_gate[i] = fast_silu(ws->ffn_gate[i]) * ws->ffn_up[i];

        matvec_dispatch(ws->ffn_gate, lw->down_proj, &lw->down_proj_q8,
                        ws->ffn_out, c, ffn, quant, ws->q8_buf);

        /* FFN residual */
        for (int i = 0; i < c; i++)
            ws->x[i] += ws->ffn_out[i];
    }

    /* Step 3: Final RMSNorm */
    fast_rms_norm(ws->x, m->ln_out_w, ws->ln_buf, c, cfg->rms_eps);

    /* Step 4: LM head → logits */
    if (m->lm_head) {
        /* F32 path (monolithic loader — SmolLM etc.) */
        matvec_f32(ws->ln_buf, m->lm_head, ws->logits, cfg->vocab_size, c);
    } else if (m->lm_head_q8.data) {
        /* Q8 path (streamed loader — quantized from F16 embed at load) */
        matvec_q8(ws->ln_buf, &m->lm_head_q8, ws->logits, cfg->vocab_size, c, ws->q8_buf);
    } else {
        /* F16 fallback */
        matvec_f16(ws->ln_buf, m->embed_f16, ws->logits, cfg->vocab_size, c);
    }

    /* Debug: print logit stats on first 2 tokens to verify forward pass */
    if (pos < 2) {
        float lmin = ws->logits[0], lmax = ws->logits[0];
        int argmax = 0;
        for (int i = 1; i < cfg->vocab_size; i++) {
            if (ws->logits[i] > lmax) { lmax = ws->logits[i]; argmax = i; }
            if (ws->logits[i] < lmin) lmin = ws->logits[i];
        }
        /* Also print first few hidden state values after all layers */
        efi_print_ascii("  [dbg] pos=");
        /* simple int print */
        char dbg[32];
        int dp = 0;
        int pv = pos;
        if (pv == 0) dbg[dp++] = '0';
        else { char tmp[12]; int ti = 0; while (pv > 0) { tmp[ti++] = '0' + pv % 10; pv /= 10; } while (ti > 0) dbg[dp++] = tmp[--ti]; }
        dbg[dp] = 0;
        efi_print_ascii(dbg);
        efi_print_ascii(" argmax=");
        pv = argmax; dp = 0;
        if (pv == 0) dbg[dp++] = '0';
        else { char tmp[12]; int ti = 0; while (pv > 0) { tmp[ti++] = '0' + pv % 10; pv /= 10; } while (ti > 0) dbg[dp++] = tmp[--ti]; }
        dbg[dp] = 0;
        efi_print_ascii(dbg);
        /* Print x[0..3] to check hidden state sanity */
        efi_print_ascii(" x0=");
        int xi = (int)(ws->x[0] * 100.0f);
        if (xi < 0) { efi_print_ascii("-"); xi = -xi; }
        pv = xi; dp = 0;
        if (pv == 0) dbg[dp++] = '0';
        else { char tmp[12]; int ti = 0; while (pv > 0) { tmp[ti++] = '0' + pv % 10; pv /= 10; } while (ti > 0) dbg[dp++] = tmp[--ti]; }
        dbg[dp] = 0;
        efi_print_ascii(dbg);
        efi_print_ascii("\r\n");
    }

    m->position++;
    return ws->logits;
}
