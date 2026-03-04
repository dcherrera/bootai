# Inference Optimization Plan — SmolLM2-135M on Dell E6510

## Benchmarks

| Metric | Baseline (Phase 0+1) | After Phase 2 | After Phase 6a |
|--------|----------------------|---------------|----------------|
| Prefill tok/s | ~1.1 (21 tok / 19.5s) | ~1.1 (no change) | **12** |
| Decode tok/s | ~1.5 (9 tok / ~6s) | ~1.5 (no change) | **12** |
| Per-token latency (ms) | ~650 | ~650 | **~83** |
| Total time "hello" → stop (30 tok) | 19,476 ms | 19,473 ms | **2,380 ms** |
| Memory used (MB) | ~630 | ~630 | **886** (+256 for F32 weights) |

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

## Phase 6: F32 Weight Pre-Conversion + Matvec Overhaul (Expected: 20-40x)

**Status:** Phase 6a DONE. Deployed 2026-03-02. **Actual: 8x decode, 11x prefill.**

### Phase 6a Results

| Metric | Before (Phase 2) | After (Phase 6a) | Speedup |
|--------|-------------------|-------------------|---------|
| Decode tok/s | ~1.5 | 12 | **8x** |
| Prefill tok/s | ~1.1 | 12 | **11x** |
| Per-token latency | ~650 ms | ~83 ms | **8x** |
| Inference time (30 tok) | 19,473 ms | 2,380 ms | **8.2x** |
| Memory used | ~630 MB | 886 MB | +256 MB |

**Why 8x instead of 20-40x:** The original estimate assumed F16 conversion was ~80% of per-token time. Actual bottleneck is now the SSE matvec itself — Westmere's FP throughput (4 FLOPS/cycle at 2.53 GHz ≈ 10 GFLOPS single-core) limits to ~12 tok/s for 134.5M MACs/token. The F16 conversion was ~8x overhead, not 20-40x. Further gains require multi-core (Phase 5) or quantization.

### The Problem: F16→F32 Conversion Dominates the Hot Path

The current `matvec_f16` (qwen2.c:222) is the single most critical function — called **211 times per token** (7 per layer × 30 layers + LM head). It performs F16→F32 conversion **inside the inner loop**, which is catastrophically slow.

#### SmolLM2-135M Matvec Call Profile (per token)

| Projection | Dimensions | Calls/token | MACs/call | Weight bytes (F16) |
|-----------|-----------|-------------|----------|-------------------|
| Q proj | 576×576 | 30 | 331,776 | 663 KB |
| K proj | 192×576 | 30 | 110,592 | 221 KB |
| V proj | 192×576 | 30 | 110,592 | 221 KB |
| O proj | 576×576 | 30 | 331,776 | 663 KB |
| Gate proj | 1536×576 | 30 | 884,736 | 1,769 KB |
| Up proj | 1536×576 | 30 | 884,736 | 1,769 KB |
| Down proj | 576×1536 | 30 | 884,736 | 1,769 KB |
| LM head | 49152×576 | 1 | 28,311,552 | 54,000 KB |
| **Total** | | **211** | **134.5M** | **256 MB** |

#### Current Performance vs Theoretical

| Metric | Current | Theoretical (compute) | Theoretical (bandwidth) |
|--------|---------|----------------------|------------------------|
| MACs/token | 134.5M | 134.5M | — |
| FLOPs/token | 269M | 269M | — |
| i5-560M single-core peak | — | 21.3 GFLOPS | — |
| DDR3-1333 dual-channel | — | — | ~17 GB/s |
| **Time per token** | **~1000 ms** | **~13 ms** (compute bound) | — |
| **Tok/s** | **~1.5** | **~77** (compute) | **~66** (bandwidth, F16) |

The i5-560M can theoretically compute 134.5M MACs in ~13ms at peak, or ~25-40ms at realistic 30-50% efficiency. We're at ~1000ms — **25-80x slower than possible on this exact hardware.**

#### Why It's So Slow: Scalar F16 Conversion in the Inner Loop

The `f16x4_to_f32_sse` function (qwen2.c:80) converts 4 F16 values to F32, but does it with **scalar integer arithmetic** — 4 separate shift/mask/OR sequences. Each conversion is ~4 integer ops. In the inner loop:

```c
for (; j + 3 < cols; j += 4) {
    v4sf w = f16x4_to_f32_sse(&row[j]);  // ~16 scalar int ops!
    v4sf xv = sse_load(&x[j]);            // 1 SSE load
    acc = sse_add(acc, sse_mul(w, xv));    // 2 SSE FP ops
}
```

The F16 conversion is ~8x more expensive than the actual math. The SSE FP pipeline sits mostly idle waiting for integer work to finish.

### Solution: Pre-Convert Weights to F32 at Load Time

**Memory cost:** 256 MB (F16) → 512 MB (F32). The Dell has 8 GB RAM — plenty of headroom.

**What changes:**
1. `loader.c`: After loading F16 weights, allocate F32 buffers and convert once
2. `qwen2.c`: Replace `matvec_f16` with `matvec_f32` — pure SSE load/mul/add, no conversion
3. Layer weights: Change `uint16_t *q_proj` to `float *q_proj` (etc.)

#### New matvec_f32 (target implementation)

```c
static void matvec_f32(const float *x, const float *w, float *out,
                        int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        const float *row = &w[(size_t)i * cols];
        v4sf acc0 = sse_setzero(), acc1 = sse_setzero();
        int j = 0;
        for (; j + 7 < cols; j += 8) {
            acc0 = sse_add(acc0, sse_mul(sse_load(&row[j]),   sse_load(&x[j])));
            acc1 = sse_add(acc1, sse_mul(sse_load(&row[j+4]), sse_load(&x[j+4])));
        }
        float sum = sse_hsum(sse_add(acc0, acc1));
        for (; j < cols; j++)
            sum += row[j] * x[j];
        out[i] = sum;
    }
}
```

This is a textbook SSE dot product:
- 2 loads + 1 mul + 1 add per 4 elements (4 instructions)
- Dual accumulators hide FP add latency (3 cycle latency, 1 cycle throughput)
- No integer ops, no conversion, full SSE pipeline utilization
- ~4 FLOPS/cycle realistic throughput

#### Expected Performance with F32 Weights

| Metric | Current (F16 convert in loop) | Phase 6 (F32 pre-converted) |
|--------|------------------------------|---------------------------|
| Weight memory | 256 MB | 512 MB |
| Memory read/token | 256 MB (F16) | 512 MB (F32) |
| Bandwidth limit | 66 tok/s | **33 tok/s** |
| Compute overhead | ~16 int ops / 4 elements | 0 |
| Effective FP throughput | ~5-10% of peak | ~30-50% of peak |
| **Estimated tok/s** | **~1.5** | **~25-33** |

The bandwidth limit drops from 66 to 33 tok/s (reading 2x more data), but the compute efficiency jumps ~10-20x. Net result: **~20-25x speedup**, bottleneck shifts from compute to memory bandwidth.

### Phase 6b: Further Optimizations (After F32 Conversion)

Once compute is no longer the bottleneck, these become relevant:

| Optimization | Expected Impact | Effort |
|-------------|----------------|--------|
| **Prefetch hints** (`__builtin_prefetch`) | +5-15% (hide DRAM latency) | 30 min |
| **4x loop unrolling** (4 accumulators for 16-wide) | +10-20% (better ILP) | 1 hour |
| **Multi-core** (Phase 5 SIPI) | +80% (2 cores on row split) | 2-3 days |
| **F16 weights + lookup table** | Halve memory reads, ~50 tok/s limit | 2 hours |
| **INT8 weights + SSSE3 dequant** | Quarter memory reads, ~100 tok/s limit | 1 day |

#### F16 Lookup Table Alternative (Phase 6c)

Instead of pre-converting to F32 (doubling memory), use a 64K-entry lookup table:

```c
static float f16_lut[65536];  /* 256 KB — fits in L2 cache */

/* Initialize once at load time */
for (int i = 0; i < 65536; i++)
    f16_lut[i] = f16_to_f32((uint16_t)i);

/* In matvec: 1 table lookup replaces 4 integer ops */
static inline v4sf f16x4_lut(const uint16_t *p) {
    return (v4sf){f16_lut[p[0]], f16_lut[p[1]], f16_lut[p[2]], f16_lut[p[3]]};
}
```

- 256 KB table fits in L2 (i5-560M has 256 KB L2)
- Keeps F16 weight storage (256 MB instead of 512 MB)
- Bandwidth limit stays at ~66 tok/s
- Lookup is ~1-2 cycles (L2 hit) vs ~4 cycles (scalar math)
- Best of both worlds, but L2 cache pressure may hurt other hot data

### Implementation Plan

| Step | What | Files |
|------|------|-------|
| 6a.1 | Add F32 weight fields to `ba_layer_weights` | `model.h` |
| 6a.2 | Pre-convert F16→F32 in loader after weight load | `loader.c` |
| 6a.3 | Replace `matvec_f16` with `matvec_f32` in forward pass | `qwen2.c` |
| 6a.4 | Free F16 buffers after conversion (optional, saves 256 MB) | `loader.c` |
| 6a.5 | Benchmark: expect ~25-33 tok/s | OTA + test |
| 6b.1 | Add prefetch hints to matvec_f32 inner loop | `qwen2.c` |
| 6b.2 | 4x unroll with 4 independent accumulators | `qwen2.c` |
| 6c | F16 LUT alternative (if memory is tight) | `qwen2.c`, `loader.c` |

### Hardware Notes (Dell E6510 / i5-560M / Westmere)

- **SSE4.2** — full SSE support, no AVX
- **No F16C** — hardware `vcvtph2ps` requires Ivy Bridge (2012). Westmere (2010) must convert in software.
- **No AVX** — 128-bit SIMD only (4 floats per op), not 256-bit
- **2 cores / 4 threads** — HT available but `EFI_MP_SERVICES_PROTOCOL` not exposed by Dell firmware
- **DDR3-1333 dual-channel** — ~17 GB/s practical bandwidth
- **L1d: 32 KB, L2: 256 KB, L3: 3 MB** — x vector (576 floats = 2.3 KB) fits 14x in L1

---

## Summary

| Phase | Optimization | Expected Speedup | Effort |
|-------|-------------|-----------------|--------|
| 0 | Benchmark | Baseline measurement | Done |
| 1a | Batch F16→F32 embedding | 5-8% | Done |
| 1b | Batch F16→F32 matvec | 5-10% | Done |
| 1c | Ring buffer KV cache | 5-10% (long context) | 2 hours |
| 2a | SIMD softmax | 3-5% | Done |
| 2b | Unroll RMSNorm | 2-3% | Done |
| 3a | Fused RMSNorm+matvec | Skip | Negligible (L1 fits) |
| 3b | Precompute attn scale | <1% | Done |
| 3c | HTTP SSE streaming | Perceived latency fix | Done |
| 4 | INT8/INT4 quantization | Skip (135M too small) | — |
| 5 | Multi-threading (2 cores) | 50-80% | No EFI_MP_SERVICES; needs SIPI |
| **6a** | **F32 weight pre-conversion** | **8x** (actual) | Done |
| 6b | Prefetch + 4x unroll | +15-30% on top of 6a | 2 hours |
| 6c | F16 LUT (alternative to 6a) | ~15-30x (less memory) | 2 hours |

**Revised realistic targets:**
- Original baseline: **~1.5 tok/s** decode
- After Phase 6a (F32 pre-convert): **12 tok/s** (actual, compute-limited)
- After Phase 6b (prefetch+unroll): **~15-18 tok/s** (estimated)
- After Phase 5 (2 cores, if SIPI works): **~20-30 tok/s**
- Theoretical max (Q4 + 2 cores + perfect BW): **~50-80 tok/s**

---

## Execution Order

1. ~~**Phase 0** — Benchmark.~~ Done.
2. ~~**Phase 3c** — HTTP SSE streaming.~~ Done.
3. ~~**Phase 1a+1b** — Batch F16 conversions.~~ Done (but still in inner loop).
4. ~~**Phase 2a+2b** — SIMD softmax, unrolled RMSNorm.~~ Done.
5. ~~**Phase 3b** — Precompute attn scale.~~ Done.
6. ~~**Phase 6a** — **F32 weight pre-conversion. THE BIG WIN.**~~ Done. **8x speedup** (1.5 → 12 tok/s).
7. **Phase 6b** — Prefetch + 4x unroll. Polish after 6a.
8. **Phase 5** — Multi-threading via SIPI. Doubles throughput once matvec is fast.
9. **Phase 1c** — Ring buffer KV cache. Matters for multi-turn.
10. Phase 3a, 4 — Skip (negligible or inapplicable for 135M).

---

## Bugs

### BUG: API missing default system prompt (FIXED)
The `/v1/chat/completions` endpoint did not inject a system prompt when none was provided in the request. The chat REPL always prepends `<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n`. Without it, SmolLM2-135M-Instruct produces garbage (`<<<` repeated). Fixed by auto-injecting the default system prompt in `api.c` when no system message is present.

### BUG: `serve` command loops after chat inference
After running `chat` on the Dell, typing `serve` causes it to get stuck in a loop and not stop. Needs investigation — may be related to model state not being fully reset, or the HTTP server poll loop interacting badly with post-chat state.
