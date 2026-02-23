/*
 * sampling.c - Temperature, top-k, top-p sampling
 *
 * Implements token sampling for autoregressive generation.
 * All math is bare-metal (no stdlib dependency except malloc/free).
 * Uses xorshift64 PRNG instead of rand().
 */

#include "model.h"
#include "../efi/efi.h"

/* Math declarations */
float expf(float x);

/* Freestanding memcpy */
void *memcpy(void *dest, const void *src, size_t n);

/* xorshift64 PRNG — no seed from OS, use fixed initial state */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static float rand_float(void) {
    return (float)(xorshift64() >> 11) / (float)(1ULL << 53);
}

/* Seed the RNG (call once at boot with a timer value or similar) */
void ba_sample_seed(uint64_t seed) {
    rng_state = seed ? seed : 0x123456789ABCDEF0ULL;
}

typedef struct { int idx; float val; } indexed_logit;

/* Simple insertion sort (sufficient for top-k candidates) */
static void sort_desc(indexed_logit *arr, int n) {
    for (int i = 1; i < n; i++) {
        indexed_logit key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j].val < key.val) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

int ba_model_sample(const float *logits, int vocab_size, ba_sample_config *config) {
    /* Greedy: argmax when temperature <= 0 */
    if (config->temperature <= 0.0f) {
        int max_i = 0;
        for (int i = 1; i < vocab_size; i++) {
            if (logits[i] > logits[max_i]) max_i = i;
        }
        return max_i;
    }

    /* Work on a copy with temperature scaling */
    float inv_t = 1.0f / config->temperature;

    /* Find top-k candidates first to avoid sorting entire vocab */
    int k = config->top_k > 0 && config->top_k < vocab_size ? config->top_k : vocab_size;

    /* Allocate on heap — vocab can be 150K+ */
    indexed_logit *candidates = (indexed_logit *)uefi_malloc(k * sizeof(indexed_logit));
    if (!candidates) return 0;

    /* Initialize with first k elements */
    for (int i = 0; i < k; i++) {
        candidates[i].idx = i;
        candidates[i].val = logits[i] * inv_t;
    }
    sort_desc(candidates, k);

    /* Scan remaining, insert if larger than smallest */
    for (int i = k; i < vocab_size; i++) {
        float v = logits[i] * inv_t;
        if (v > candidates[k - 1].val) {
            candidates[k - 1].idx = i;
            candidates[k - 1].val = v;
            /* Bubble up */
            for (int j = k - 1; j > 0 && candidates[j].val > candidates[j - 1].val; j--) {
                indexed_logit tmp = candidates[j];
                candidates[j] = candidates[j - 1];
                candidates[j - 1] = tmp;
            }
        }
    }

    int n_cand = k;

    /* Softmax over candidates */
    float max_l = candidates[0].val;
    float sum = 0.0f;
    for (int i = 0; i < n_cand; i++) {
        candidates[i].val = expf(candidates[i].val - max_l);
        sum += candidates[i].val;
    }
    float inv_sum = 1.0f / sum;
    for (int i = 0; i < n_cand; i++)
        candidates[i].val *= inv_sum;

    /* Top-p nucleus sampling */
    if (config->top_p > 0.0f && config->top_p < 1.0f) {
        float cumsum = 0.0f;
        int cutoff = n_cand;
        for (int i = 0; i < n_cand; i++) {
            cumsum += candidates[i].val;
            if (cumsum > config->top_p) {
                cutoff = i + 1;
                break;
            }
        }
        n_cand = cutoff;

        /* Re-normalize */
        sum = 0.0f;
        for (int i = 0; i < n_cand; i++) sum += candidates[i].val;
        inv_sum = 1.0f / sum;
        for (int i = 0; i < n_cand; i++) candidates[i].val *= inv_sum;
    }

    /* Random sample */
    float r = rand_float();
    float cumsum = 0.0f;
    int result = candidates[n_cand - 1].idx;
    for (int i = 0; i < n_cand; i++) {
        cumsum += candidates[i].val;
        if (r < cumsum) {
            result = candidates[i].idx;
            break;
        }
    }

    uefi_free(candidates);
    return result;
}
