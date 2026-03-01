/*
 * model.h — Common model interface and weight structures
 *
 * Both Qwen3 and RWKV-X implement this interface.
 * The loader auto-detects architecture from the .btw file header.
 */

#ifndef BOOTAI_MODEL_H
#define BOOTAI_MODEL_H

#include <stdint.h>
#include <stddef.h>

/* Freestanding allocator (provided by efi/uefi_libc.c on bare metal) */
void* uefi_malloc(size_t n);
void* uefi_calloc(size_t count, size_t size);
void  uefi_free(void *ptr);

/* ---- Model architecture ---- */

typedef enum {
    MODEL_QWEN3,    /* Qwen3-0.6B (Q8) */
    MODEL_QWEN25,   /* Qwen2.5-Coder-7B-Instruct (Q4) — future */
    MODEL_LLAMA,    /* Llama-family: Llama 2/3, SmolLM, Mistral (no bias, no QK-norm) */
    MODEL_RWKVX,    /* RWKV-X 3.6B (Q8) — future */
} ba_model_arch;

/* ---- Runtime config (from .btw header) ---- */

typedef struct {
    int n_layers;
    int n_embd;
    int n_heads;
    int n_kv_heads;
    int head_dim;
    int ffn_dim;
    int vocab_size;
    int max_seq_len;
    float rope_theta;
    float rms_eps;
    int has_qk_norm;
    int has_qkv_bias;
} ba_model_config;

/* ---- Quantized weight: int8 data + per-row float scales ---- */

typedef struct {
    int8_t *data;     /* [rows * cols] row-major int8 */
    float  *scales;   /* [rows] per-row symmetric scale */
} ba_q8_weight;

/* ---- Per-layer weights ---- */

typedef struct {
    /* Attention Q/K/V/O projections (F16) */
    uint16_t *q_proj;   /* [q_dim * n_embd] */
    uint16_t *k_proj;   /* [kv_dim * n_embd] */
    uint16_t *v_proj;   /* [kv_dim * n_embd] */
    uint16_t *o_proj;   /* [n_embd * q_dim] */

    /* QK-Norm weights (f32, NULL if not present) */
    float *q_norm;      /* [head_dim] */
    float *k_norm;      /* [head_dim] */

    /* Q/K/V biases (f32, NULL if not present — Qwen3 has no bias) */
    float *q_bias;      /* [n_heads * head_dim] */
    float *k_bias;      /* [n_kv_heads * head_dim] */
    float *v_bias;      /* [n_kv_heads * head_dim] */

    /* RMSNorm weights (f32) */
    float *ln1_w;       /* [n_embd] input layernorm */
    float *ln2_w;       /* [n_embd] post-attention layernorm */

    /* FFN projections (F16) */
    uint16_t *gate_proj; /* [ffn_dim * n_embd] */
    uint16_t *up_proj;   /* [ffn_dim * n_embd] */
    uint16_t *down_proj; /* [n_embd * ffn_dim] */
} ba_layer_weights;

/* ---- KV cache ---- */

typedef struct {
    float *k_cache;     /* [max_len * n_kv_heads * head_dim] per layer */
    float *v_cache;     /* [max_len * n_kv_heads * head_dim] per layer */
    int cache_len;      /* current number of tokens cached */
    int max_len;        /* capacity */
} ba_kv_layer;

typedef struct {
    ba_kv_layer *layers;  /* [n_layers] */
    int n_layers;
    int kv_dim;           /* n_kv_heads * head_dim */
    int max_len;
} ba_kv_cache;

/* ---- Workspace (pre-allocated scratch buffers) ---- */

typedef struct {
    float *x;             /* [n_embd] current hidden state */
    float *x_out;         /* [n_embd] output after projection */
    float *ln_buf;        /* [n_embd] norm output */
    float *q;             /* [n_heads * head_dim] */
    float *k;             /* [n_kv_heads * head_dim] */
    float *v;             /* [n_kv_heads * head_dim] */
    float *attn_out;      /* [n_heads * head_dim] */
    float *attn_scores;   /* [max_seq_len] attention weights per head */
    float *ffn_gate;      /* [ffn_dim] */
    float *ffn_up;        /* [ffn_dim] */
    float *ffn_out;       /* [n_embd] */
    float *logits;        /* [vocab_size] */
    int8_t *q8_buf;       /* [max(n_embd, ffn_dim)] quantized input buffer */
    /* RoPE precomputed sin/cos cache — avoids sinf/cosf per token */
    float *rope_cos;      /* [max_seq_len * half_dim] */
    float *rope_sin;      /* [max_seq_len * half_dim] */
    int rope_half_dim;    /* head_dim / 2 */
    int rope_max_seq;     /* max_seq_len used for cache */
} ba_workspace;

/* ---- Top-level model struct ---- */

typedef struct ba_model {
    ba_model_config config;
    ba_model_arch arch;
    int position;           /* current sequence position */

    /* Weights */
    uint16_t *embed;        /* [vocab_size * n_embd] f16 embeddings */
    ba_layer_weights *layers;  /* [n_layers] */
    float *ln_out_w;        /* [n_embd] final norm weight */
    uint16_t *lm_head;      /* [vocab_size * n_embd] f16 lm_head */
    int shared_embed_lmhead; /* 1 if lm_head == embed */

    /* State */
    ba_kv_cache kv;
    ba_workspace ws;

    /* Raw loaded data (owns memory) */
    void *raw_data;
    uint64_t raw_size;
} ba_model;

/* ---- Sampling config ---- */

typedef struct {
    float temperature;
    int   top_k;
    float top_p;
    float repeat_penalty;
    int   max_tokens;
} ba_sample_config;

/* ---- Public API ---- */

/* Load model from .btw file */
ba_model* ba_model_load(const char *path);

/* Get model architecture */
ba_model_arch ba_model_arch_of(const ba_model *m);

/* Run forward pass — returns logits for next token */
float* ba_model_forward(ba_model *m, const int *tokens, int n_tokens);

/* Sample next token from logits */
int ba_model_sample(const float *logits, int vocab_size, ba_sample_config *config);

/* Reset model state (clear KV cache) */
void ba_model_reset(ba_model *m);

/* Free model */
void ba_model_free(ba_model *m);

/* Get vocab size */
int ba_model_vocab_size(const ba_model *m);

/* Default sampling config */
static inline ba_sample_config ba_default_config(void) {
    return (ba_sample_config){
        .temperature = 0.7f,
        .top_k = 40,
        .top_p = 0.9f,
        .repeat_penalty = 1.1f,
        .max_tokens = 2048
    };
}

/* ---- KV cache API ---- */

void ba_kv_init(ba_kv_cache *kv, int n_layers, int max_len, int n_kv_heads, int head_dim);
void ba_kv_append(ba_kv_cache *kv, int layer, const float *k, const float *v);
void ba_kv_reset(ba_kv_cache *kv);
void ba_kv_free(ba_kv_cache *kv);

/* ---- Workspace API ---- */

void ba_workspace_init(ba_workspace *ws, ba_model_config *cfg);
void ba_workspace_free(ba_workspace *ws);

/* ---- Forward pass (Qwen3/Qwen2) ---- */

float* ba_qwen_forward(ba_model *m, int token);

/* ---- RoPE ---- */

void ba_rope_apply(float *vec, int n_heads, int head_dim, int pos, float theta);
void ba_rope_apply_cached(float *vec, int n_heads, int head_dim, int pos,
                          const float *cos_cache, const float *sin_cache,
                          int half_dim);

/* ---- Tokenizer ---- */

typedef struct ba_tokenizer ba_tokenizer;

ba_tokenizer* ba_tokenizer_load(const char *path);
int  ba_tokenizer_encode(ba_tokenizer *tok, const char *text, int *tokens_out, int max_tokens);
int  ba_tokenizer_decode(ba_tokenizer *tok, const int *tokens, int n_tokens, char *text_out, int max_len);
int  ba_tokenizer_im_start(ba_tokenizer *tok);
int  ba_tokenizer_im_end(ba_tokenizer *tok);
int  ba_tokenizer_eos(ba_tokenizer *tok);
void ba_tokenizer_free(ba_tokenizer *tok);

/* Sampling PRNG seed */
void ba_sample_seed(uint64_t seed);

#endif /* BOOTAI_MODEL_H */
