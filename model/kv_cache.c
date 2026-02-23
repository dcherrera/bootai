/*
 * kv_cache.c - KV cache management
 *
 * Per-layer key-value caches for GQA attention. Pre-allocated at model load
 * time. Simple eviction: drop oldest half when full.
 */

#include "model.h"
#include "../efi/efi.h"

/* Freestanding memcpy/memset (provided by uefi_libc.c) */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);

void ba_kv_init(ba_kv_cache *kv, int n_layers, int max_len, int n_kv_heads, int head_dim) {
    kv->n_layers = n_layers;
    kv->kv_dim = n_kv_heads * head_dim;
    kv->max_len = max_len;

    kv->layers = (ba_kv_layer *)uefi_malloc(n_layers * sizeof(ba_kv_layer));
    for (int i = 0; i < n_layers; i++) {
        size_t cache_bytes = (size_t)max_len * kv->kv_dim * sizeof(float);
        kv->layers[i].k_cache = (float *)uefi_malloc(cache_bytes);
        kv->layers[i].v_cache = (float *)uefi_malloc(cache_bytes);
        kv->layers[i].cache_len = 0;
        kv->layers[i].max_len = max_len;
        memset(kv->layers[i].k_cache, 0, cache_bytes);
        memset(kv->layers[i].v_cache, 0, cache_bytes);
    }
}

void ba_kv_append(ba_kv_cache *kv, int layer, const float *k, const float *v) {
    ba_kv_layer *l = &kv->layers[layer];
    int kv_dim = kv->kv_dim;

    /* Evict oldest half if full */
    if (l->cache_len >= l->max_len) {
        int keep = l->max_len / 2;
        int drop = l->cache_len - keep;
        memmove(l->k_cache, &l->k_cache[drop * kv_dim], keep * kv_dim * sizeof(float));
        memmove(l->v_cache, &l->v_cache[drop * kv_dim], keep * kv_dim * sizeof(float));
        l->cache_len = keep;
    }

    int pos = l->cache_len;
    memcpy(&l->k_cache[pos * kv_dim], k, kv_dim * sizeof(float));
    memcpy(&l->v_cache[pos * kv_dim], v, kv_dim * sizeof(float));
    l->cache_len = pos + 1;
}

void ba_kv_reset(ba_kv_cache *kv) {
    for (int i = 0; i < kv->n_layers; i++) {
        kv->layers[i].cache_len = 0;
    }
}

void ba_kv_free(ba_kv_cache *kv) {
    if (!kv->layers) return;
    for (int i = 0; i < kv->n_layers; i++) {
        uefi_free(kv->layers[i].k_cache);
        uefi_free(kv->layers[i].v_cache);
    }
    uefi_free(kv->layers);
    kv->layers = 0;
}
