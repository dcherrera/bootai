/*
 * qwen2.h - Qwen2.5 model configuration constants
 *
 * Architecture parameters for the Qwen2.5-Coder-7B-Instruct model.
 * Grouped Query Attention with SwiGLU FFN and RoPE embeddings.
 */

#ifndef BOOTAI_QWEN2_H
#define BOOTAI_QWEN2_H

#define QWEN2_LAYERS      28
#define QWEN2_HIDDEN       3584
#define QWEN2_HEADS        28
#define QWEN2_KV_HEADS     7
#define QWEN2_HEAD_DIM     128
#define QWEN2_FFN          18944
#define QWEN2_VOCAB        152064

#define QWEN2_ROPE_THETA   1000000.0f
#define QWEN2_RMS_NORM_EPS 1e-6f
#define QWEN2_MAX_SEQ_LEN  32768

#endif /* BOOTAI_QWEN2_H */
