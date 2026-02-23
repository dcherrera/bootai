/*
 * qwen2.h - Qwen3/Qwen2.5 model configuration
 *
 * Architecture parameters are now loaded from the .btw header at runtime.
 * These constants are kept as reference for documentation.
 */

#ifndef BOOTAI_QWEN2_H
#define BOOTAI_QWEN2_H

/* Qwen3-0.6B (primary target for Phase 4) */
#define QWEN3_06B_LAYERS      28
#define QWEN3_06B_HIDDEN      1024
#define QWEN3_06B_HEADS       16
#define QWEN3_06B_KV_HEADS    8
#define QWEN3_06B_HEAD_DIM    64
#define QWEN3_06B_FFN         2816
#define QWEN3_06B_VOCAB       151936
#define QWEN3_06B_ROPE_THETA  1000000.0f
#define QWEN3_06B_RMS_EPS     1e-6f
#define QWEN3_06B_MAX_SEQ_LEN 40960

/* Qwen2.5-Coder-7B-Instruct (future Phase 4c) */
#define QWEN25_7B_LAYERS      28
#define QWEN25_7B_HIDDEN      3584
#define QWEN25_7B_HEADS       28
#define QWEN25_7B_KV_HEADS    7
#define QWEN25_7B_HEAD_DIM    128
#define QWEN25_7B_FFN         18944
#define QWEN25_7B_VOCAB       152064
#define QWEN25_7B_ROPE_THETA  1000000.0f
#define QWEN25_7B_RMS_EPS     1e-6f
#define QWEN25_7B_MAX_SEQ_LEN 32768

#endif /* BOOTAI_QWEN2_H */
