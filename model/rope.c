/*
 * rope.c - Rotary Position Embeddings (RoPE)
 *
 * Applies rotary position embeddings to query and key tensors.
 * Uses iterated sqrtf (hardware sqrtss) instead of powf to avoid
 * the inaccurate freestanding logf polynomial.
 */

#include "model.h"

/* Math declarations (provided by uefi_libc.c on bare metal) */
float sqrtf(float x);
float cosf(float x);
float sinf(float x);

void ba_rope_apply(float *vec, int n_heads, int head_dim, int pos, float theta) {
    int half = head_dim / 2;

    /* Compute theta_step = theta^(1/half) via iterated sqrt.
     * sqrtf uses hardware sqrtss — exact to float precision.
     * This avoids powf → expf(e * logf(theta)) which has ~1% error
     * due to our freestanding logf's truncated Taylor series. */
    float theta_step = theta;
    for (int p = half; p > 1; p >>= 1)
        theta_step = sqrtf(theta_step);
    /* Now theta_step = theta^(1/half).
     * inv_freq[i] = theta^(-2*i/head_dim) = theta^(-i/half) = 1/theta_step^i
     *
     * Uses halved (rotate_half) pairing: pairs (i, i+half) per head.
     * This matches HuggingFace transformers Q/K weight layout. */

    for (int h = 0; h < n_heads; h++) {
        float *head = &vec[h * head_dim];
        float inv_freq = 1.0f;
        for (int i = 0; i < half; i++) {
            float angle = (float)pos * inv_freq;
            float cos_a = cosf(angle);
            float sin_a = sinf(angle);
            float x0 = head[i];
            float x1 = head[i + half];
            head[i]        = x0 * cos_a - x1 * sin_a;
            head[i + half] = x1 * cos_a + x0 * sin_a;
            inv_freq /= theta_step;
        }
    }
}

/* Cached version: table lookups instead of sinf/cosf.
 * cos_cache/sin_cache are [max_seq * half_dim], precomputed at init. */
void ba_rope_apply_cached(float *vec, int n_heads, int head_dim, int pos,
                          const float *cos_cache, const float *sin_cache,
                          int half_dim) {
    const float *cos_row = &cos_cache[pos * half_dim];
    const float *sin_row = &sin_cache[pos * half_dim];

    for (int h = 0; h < n_heads; h++) {
        float *head = &vec[h * head_dim];
        for (int i = 0; i < half_dim; i++) {
            float cos_a = cos_row[i];
            float sin_a = sin_row[i];
            float x0 = head[i];
            float x1 = head[i + half_dim];
            head[i]            = x0 * cos_a - x1 * sin_a;
            head[i + half_dim] = x1 * cos_a + x0 * sin_a;
        }
    }
}
