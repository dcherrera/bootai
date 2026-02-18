/*
 * model.h — Common model interface
 *
 * Both Qwen2.5 and RWKV-X implement this interface.
 * The loader auto-detects architecture from the weight file header.
 */

#ifndef BOOTAI_MODEL_H
#define BOOTAI_MODEL_H

#include <stdint.h>
#include <stddef.h>

/* Model architecture */
typedef enum {
    MODEL_QWEN25,   /* Qwen2.5-Coder-7B-Instruct (Q4) */
    MODEL_RWKVX,    /* RWKV-X 3.6B (Q8) */
} ba_model_arch;

/* Sampling config */
typedef struct {
    float temperature;
    int   top_k;
    float top_p;
    float repeat_penalty;
    int   max_tokens;
} ba_sample_config;

/* Opaque model state */
typedef struct ba_model ba_model;

/* Load model from file — auto-detects architecture */
ba_model* ba_model_load(const char *path);

/* Get model architecture */
ba_model_arch ba_model_arch_of(const ba_model *m);

/* Run forward pass — returns logits for next token */
float* ba_model_forward(ba_model *m, const int *tokens, int n_tokens);

/* Sample next token from logits */
int ba_model_sample(const float *logits, int vocab_size, ba_sample_config *config);

/* Reset model state (clear KV cache / recurrent state) */
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

#endif /* BOOTAI_MODEL_H */
