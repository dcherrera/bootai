# BootAI Inference Performance Audit

## Current State Summary

No explicit benchmarks exist. No tok/s, prefill/decode latency, or memory bandwidth metrics have been documented. This audit covers what we know from documentation and architecture.

---

## Model Architecture

- **SmolLM2-135M** (135M params, 30 layers)
- F16 weights (257 MB), F32 activations (W16A32)
- KV cache: 7 MB for 512 context
- Total memory: ~330 MB + UEFI overhead
- Fits easily in 1GB+ RAM (tested on 4GB Surface Pro 7, 8GB Dell E6510)

## Hardware Target

- **Dell E6510**: Intel Core i7-620M (Westmere, 2010)
- 8 GB RAM
- No GPU acceleration
- Single-threaded (UEFI has no threading primitives)

---

## Optimization Decisions (Current)

### F16 vs INT8
F16 chosen for 135M model — lossless from BF16, INT8 causes quality loss at this size.

### No AVX
AVX disabled on UEFI — VEX-encoded instructions cause #UD crashes. **SSE4.2 only.**

### No F16C
Hardware f16→f32 conversion uses VEX encoding, so software bit manipulation is used instead.

### Compiler Flags
- `-O2` marked as "critical for inference performance on real hardware"
- `-msse4.2` allowed
- `-fPIC` required (affects performance due to GOT access overhead)
- `-ffreestanding` (no libc)

### Math Library
Pure C implementations:
- `sqrtf` via x86 `sqrtss`
- `sinf`/`cosf` via minimax polynomials
- `expf` via Cody-Waite reduction

---

## Inference Pipeline

1. **Model loading:** Auto-detects architecture from weight file header
2. **Tokenization:** BPE tokenizer + special token handling + ChatML template
3. **Prefill:** Process all prompt tokens through forward pass
4. **Decode:** Generate tokens one at a time (autoregressive)
5. **Sampling:** Temperature, top-k, top-p, repeat penalty
6. **Detokenization:** Convert token IDs back to text

### Forward Pass (per layer)
- RMSNorm
- GQA attention (grouped query attention, 9 heads)
- RoPE (halved format, iterated sqrtf for theta^(1/n))
- SwiGLU FFN
- Residual connections

---

## Serving Architecture

- **Single-threaded HTTP server** (non-streaming)
- Full inference runs during request (blocks server)
- 500ms accept timeout between connections
- Pre-allocated buffers (8KB request, 64KB response)
- HTTP/1.0 with `Connection: close`

---

## Known Limitations

| Limitation | Impact |
|-----------|--------|
| Single-threaded | No parallel inference, server blocks during generation |
| No AVX (SSE4.2 only) | ~2-4x slower matmul vs AVX2 |
| No F16C | Software f16→f32 conversion overhead |
| `-fPIC` required | GOT indirection overhead on every function call |
| No BLAS | No optimized GEMM/GEMV routines |
| No profiling data | Can't identify actual bottlenecks without measurement |
| Non-streaming HTTP | Client waits for full generation before seeing any output |
| F16 weights (not INT8/Q4) | Higher memory bandwidth pressure |

---

## Relevant Documentation Files

| File | Content |
|------|---------|
| `docs/architecture/inference-on-bare-metal.md` | Model architecture, F16 format, math library, SIMD constraints |
| `docs/architecture/uefi-boot-architecture.md` | Compiler flags, memory model, `-O2` critical |
| `docs/specs/bootai-roadmap.md` | Phase 4 inference complete, multi-turn chat working |
| `docs/architecture/hd-install.md` | Model loading from raw partition (contiguous, faster than FAT32) |
| `docs/architecture/http-server.md` | Server design, non-streaming, single-threaded |
| `docs/architecture/prod-build.md` | PROD build skips menu, boots straight to inference |
| `docs/architecture/memory-diagnostics.md` | Memory budget: ~17 MB used, 7934+ MB free |
| `docs/architecture/networking.md` | Dual network stack (UEFI vs custom ns_*) |

---

## Source Code Files to Audit

| File | Purpose |
|------|---------|
| `model/qwen2.c` | Transformer forward pass (RoPE, GQA, SwiGLU) — main compute |
| `model/qwen2.h` | Qwen2.5 config and declarations |
| `model/loader.c` | Weight loading, format detection |
| `model/kv_cache.c` | KV cache management |
| `model/rope.c` | Rotary position embeddings |
| `model/sampling.c` | Token sampling |
| `model/model.h` | Common model interface |
| `foundry/tensor/tensor.c` | Tensor ops (matmul, norms) — compute kernel |
| `Makefile` | Compiler flags, optimization level |

---

## Next Steps

1. **Benchmark current performance** — measure tok/s for prefill and decode
2. **Profile the forward pass** — identify which operations dominate
3. **Audit source code** — check for vectorization, memory access patterns, unnecessary work
4. **Create optimization plan** — prioritized list of changes with expected impact
