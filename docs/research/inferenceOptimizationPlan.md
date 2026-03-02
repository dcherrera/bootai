# Inference Optimization Plan — SmolLM2-135M on Dell E6510

## Benchmarks

| Metric | Baseline (Phase 0+1) | After Phase 2 | After Phase 3 | After Phase 4 | After Phase 5 |
|--------|----------------------|---------------|---------------|---------------|---------------|
| Prefill tok/s | ~1.1 (21 tok / 19.5s) | — | — | — | — |
| Decode tok/s | ~1.5 (9 tok / ~6s) | — | — | — | — |
| Per-token latency (ms) | ~650 | — | — | — | — |
| Total time "hello" → stop (30 tok) | 19,476 ms | — | — | — | — |
| Memory used (MB) | — | — | — | — | — |

> **Note:** Phase 0 (rdtsc benchmarking) and Phase 1 (batch F16 SSE + ring buffer KV cache) were shipped together in one build. No pre-optimization baseline exists. The integer tok/s fields in the API response truncate at these low rates — use `inference_ms` and token counts for accurate calculation.

---

## Current State

**Hardware:** Dell E6510, Intel Core i7-620M (Westmere, 2010), 8 GB DDR3, 2.53 GHz, 3 MB L3, 2 cores / 4 threads
**Model:** SmolLM2-135M (30 layers, 576 hidden, 9 heads, 49K vocab)
**Weights:** F16 (257 MB), F32 activations (W16A32)
**Compiler:** `-O3 -ftree-vectorize -msse4.2 -fPIC -ffreestanding`
**Benchmarks:** None exist yet. No measured tok/s.

### Architectural Constraints (Cannot Change)

| Constraint | Impact | Reason |
|-----------|--------|--------|
| No AVX | 2-4x slower matmul vs AVX2 (128-bit vs 256-bit SIMD) | UEFI doesn't enable AVX; VEX encoding causes #UD |
| No F16C | ~5-10 cycles per F16→F32 (software bit manipulation) | F16C uses VEX encoding |
| `-fPIC` required | 10-20% GOT indirection overhead | UEFI linker requirement |
| No BLAS | No optimized GEMM/GEMV | Bare-metal, no libraries |

---

## Phase 0: Benchmark (Do This First)

**Goal:** Measure current tok/s for prefill and decode so we have a baseline.

### Plan

Add timing instrumentation to `api_chat_completions()` in `efi/api.c` using `rdtsc`.

Measure:
1. **Prefill tok/s** — time from first `ba_model_forward()` to last in the prefill loop
2. **Decode tok/s** — time per `ba_model_forward()` call in the generation loop
3. **Total inference time** — end-to-end for a standard prompt ("hello" → 256 max tokens)
4. **Time to first token** — prefill complete to first generated token

Report via the existing HTTP response (add `inference_ms`, `prefill_tok_s`, `decode_tok_s` to the JSON usage object).

### How to Measure Time in UEFI

```c
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// i7-620M: 2530 MHz → 1ms = 2,530,000 cycles
#define CYCLES_PER_MS 2530000ULL
```

**Deliverable:** Fill in the Baseline column of the benchmarks table.

---

## Phase 1: Low-Hanging Fruit (Expected: 10-20% Total)

### 1a. Batch F16→F32 Conversion in Embedding Lookup

**File:** `model/qwen2.c`
**Current:** Scalar F16→F32 per element (576 elements per token)
**Fix:** Process 4 F16 values at once using SSE2 integer ops

```c
// Current (scalar):
for (int i = 0; i < c; i++)
    ws->x[i] = f16_to_f32(emb_row[i]);

// Optimized (4-wide):
for (int i = 0; i < c; i += 4) {
    __m128i h4 = _mm_loadl_epi64((__m128i*)(emb_row + i));
    h4 = _mm_unpacklo_epi16(h4, _mm_setzero_si128());
    // ... sign/exp/mantissa extraction via shifts ...
    _mm_storeu_ps(ws->x + i, result);
}
```

**Impact:** ~5-8% on embedding + all F16 matvec paths
**Effort:** ~20 lines

### 1b. Batch F16→F32 in matvec_f16

**File:** `model/qwen2.c`
**Current:** Converts 4 F16 values per iteration but using scalar `f16_to_f32()` calls
**Fix:** Same SSE2 batch conversion as 1a

**Impact:** This is the hottest kernel — called for every weight matrix. Even small per-element savings multiply across all 30 layers x multiple matrices per layer.

### 1c. Ring Buffer KV Cache (Eliminate memmove)

**File:** `model/kv_cache.c`
**Current:** When cache is full, `memmove()` shifts half the entries (~1 MB per layer x 30 layers = 30 MB)
**Fix:** Circular buffer with head/tail indices. No copies ever.

```c
// Instead of:
memmove(l->k_cache, &l->k_cache[drop * kv_dim], keep * kv_dim * sizeof(float));

// Use:
l->cache_start = (l->cache_start + drop) % l->max_len;
// Attention loop wraps around: idx = (cache_start + t) % max_len
```

**Impact:** ~5-10% for conversations that exceed 512 tokens
**Effort:** ~30 lines

---

## Phase 2: Compute Kernel Optimization (Expected: 5-15%)

### 2a. SIMD Softmax with Approximate exp()

**File:** `model/qwen2.c`
**Current:** Scalar `expf()` per attention score (~50-100 cycles each, ~480 calls per decode token)
**Fix:** 4-wide SSE exp approximation

```c
// Schraudolph's method (~12-bit accuracy):
static inline __m128 fast_exp_sse(__m128 x) {
    __m128 a = _mm_set1_ps(12102203.0f);
    __m128 b = _mm_set1_ps(1065353216.0f);
    __m128i y = _mm_cvtps_epi32(_mm_add_ps(_mm_mul_ps(x, a), b));
    return _mm_castsi128_ps(y);
}
```

**Impact:** ~3-5% (attention softmax)
**Effort:** ~15 lines

### 2b. Unroll and Vectorize RMSNorm

**File:** `model/qwen2.c` (`fast_rms_norm`)
**Current:** Already SSE4.2 vectorized (4-wide) but single accumulator
**Fix:** Use 2 or 4 independent accumulators to hide FP latency

**Impact:** ~2-3%
**Effort:** ~10 lines

---

## Phase 3: Structural Optimizations (Expected: 5-10%)

### 3a. Fused RMSNorm + Matvec

Currently RMSNorm writes normalized values to memory, then matvec reads them back. Fusing eliminates one full read+write of the hidden state (576 floats x 4 bytes = 2.3 KB per fusion point, ~6 per layer x 30 layers = ~400 KB saved memory traffic).

**Impact:** ~3-5% (memory bandwidth limited on DDR3)
**Effort:** Medium — requires restructuring forward pass

### 3b. Precompute Attention Scale

**Current:** `1.0f / sqrtf(head_dim)` computed per attention head per layer
**Fix:** Compute once at model load time, store in config

**Impact:** Tiny but free
**Effort:** 2 lines

### 3c. HTTP Streaming Response

Not a compute optimization, but dramatically improves perceived speed. Instead of generating all tokens then sending one response, send each token as it's generated via chunked transfer or SSE.

**Current:** Client waits 10-30s for full response
**Fix:** Client sees first token in <1s, then tokens stream in real-time

**Impact:** User-facing latency goes from "wait 30s" to "see first token in ~1s"
**Effort:** ~50 lines in `http_server.c` + `api.c`

---

## Phase 4: Quantization (Expected: 30-50%)

### 4a. INT8 Weights (Q8_0)

Switch from F16 to INT8 weights. Halves memory bandwidth requirements.

- F16: 257 MB weights, ~21 GB/s DDR3 bandwidth
- INT8: 128 MB weights, same bandwidth → **2x less memory pressure**

**Trade-off:** Slight quality loss at 135M size (documented concern). Need to evaluate.

### 4b. INT4 Weights (Q4_K_M)

Quarter the memory bandwidth. The Qwen2.5-7B already uses Q4_K_M in the codebase.

- Q4: 64 MB weights → fits entirely in RAM with room to spare
- Dequantization overhead vs bandwidth savings: net positive on DDR3

**Trade-off:** Significant quality loss at 135M. May only be viable for larger models.

---

## Phase 5: Multi-Threading (Expected: 1.5-2x on 2-core i7)

**Status:** Research needed. UEFI has no threading primitives — we need to build our own.

### The Challenge

UEFI Boot Services mode has no `CreateThread()`, no pthreads, no scheduler. The i7-620M has 2 physical cores (4 with HT), but UEFI only runs on the BSP (Bootstrap Processor). The APs (Application Processors) are halted at boot.

### What's Required

1. **Wake APs via SIPI (Startup IPI)** — send inter-processor interrupts to bring secondary cores online
2. **Spin-lock synchronization** — `lock cmpxchg` for atomic ops, no OS mutex available
3. **Work distribution** — split matvec rows across cores (e.g., core 0 does rows 0-287, core 1 does rows 288-575)
4. **Memory barriers** — `mfence`/`sfence` for cache coherency between cores
5. **Stack allocation per core** — each AP needs its own stack (pre-allocated)

### UEFI MP Protocol (EFI_MP_SERVICES_PROTOCOL)

Some UEFI firmwares expose `EFI_MP_SERVICES_PROTOCOL` which provides:
- `GetNumberOfProcessors()` — discover core count
- `StartupThisAP()` — run a function on a specific AP
- `StartupAllAPs()` — run a function on all APs

If the Dell's firmware supports this, it's the clean path. If not, we need raw SIPI.

### Parallelizable Operations

| Operation | Parallelism | Expected Speedup (2 cores) |
|-----------|------------|---------------------------|
| matvec_f16 (row split) | Embarrassingly parallel | ~1.8x |
| Attention Q*K^T | Per-head parallel | ~1.5x |
| Attention score*V | Per-head parallel | ~1.5x |
| RMSNorm | Reduce (sum-of-squares) | ~1.3x |
| Softmax | Per-head parallel | ~1.5x |
| Embedding lookup | Not worth parallelizing | — |
| Sampling | Not worth parallelizing | — |

### Estimated Total Impact

With 2 cores on matvec (the dominant op): **~1.5-1.8x total speedup**.
Combined with Phase 1-4 optimizations: could reach **3-4x over baseline**.

### Research TODO

- [ ] Check if Dell E6510 firmware exposes `EFI_MP_SERVICES_PROTOCOL`
- [ ] If not, research SIPI (Startup IPI) sequence for Westmere
- [ ] Prototype: wake one AP, have it increment a shared counter
- [ ] Design work-stealing queue or static row partitioning for matvec
- [ ] Measure synchronization overhead (spin-lock cost per matvec call)

---

## Summary

| Phase | Optimization | Expected Speedup | Effort |
|-------|-------------|-----------------|--------|
| 0 | Benchmark | Baseline measurement | 1 hour |
| 1a | Batch F16→F32 embedding | 5-8% | 1 hour |
| 1b | Batch F16→F32 matvec | 5-10% | 1 hour |
| 1c | Ring buffer KV cache | 5-10% (long context) | 2 hours |
| 2a | SIMD softmax | 3-5% | 1 hour |
| 2b | Unroll RMSNorm | 2-3% | 30 min |
| 3a | Fused RMSNorm+matvec | 3-5% | 3 hours |
| 3b | Precompute attn scale | <1% | 5 min |
| 3c | HTTP streaming | Perceived latency fix | 3 hours |
| 4a | INT8 weights | 30-50% | 1 day |
| 4b | INT4 weights | 50-70% (quality risk) | 1 day |
| 5 | Multi-threading (2 cores) | 50-80% | 2-3 days |

**Realistic cumulative:**
- Phases 1-3: **15-35%** faster decode
- +Phase 4 (INT8): **2x** faster decode
- +Phase 5 (2 cores): **3-4x** faster decode over baseline

---

## Execution Order

1. **Phase 0** — Benchmark. No optimization without measurement.
2. **Phase 3c** — HTTP streaming. Doesn't speed up inference but makes it feel instant.
3. **Phase 1a+1b** — Batch F16 conversions. Biggest bang for least effort.
4. **Phase 2a** — SIMD softmax. Quick win.
5. **Phase 1c** — Ring buffer KV cache. Matters for multi-turn.
6. **Phase 4a** — INT8 quantization. Biggest single compute improvement.
7. **Phase 5** — Multi-threading. Second biggest improvement but hardest work.
8. **Phase 3a** — Fused kernels. Most effort, moderate gain.

---

## Bugs

### BUG: API missing default system prompt (FIXED)
The `/v1/chat/completions` endpoint did not inject a system prompt when none was provided in the request. The chat REPL always prepends `<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n`. Without it, SmolLM2-135M-Instruct produces garbage (`<<<` repeated). Fixed by auto-injecting the default system prompt in `api.c` when no system message is present.

### BUG: `serve` command loops after chat inference
After running `chat` on the Dell, typing `serve` causes it to get stuck in a loop and not stop. Needs investigation — may be related to model state not being fully reset, or the HTTP server poll loop interacting badly with post-chat state.
