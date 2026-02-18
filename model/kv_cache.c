/*
 * kv_cache.c - KV cache management
 *
 * Manages key-value caches for Qwen2.5 (standard GQA cache) and
 * RWKV-X (SnapKV-compressed sparse attention cache). Supports
 * pre-allocation, appending, and memory-bounded eviction.
 */

#include "model.h"

/* TODO: Implement KV cache
 *
 * - kv_cache_init(n_layers, n_kv_heads, head_dim, max_seq_len)
 * - kv_cache_append(cache, layer, k, v, pos)
 * - kv_cache_get(cache, layer) -> (keys, values, len)
 * - kv_cache_clear(cache)
 * - kv_cache_free(cache)
 * - SnapKV compression for RWKV-X sparse attention layers
 */
