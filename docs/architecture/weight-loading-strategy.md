# Weight Loading Strategy

How BootAI loads model weights and why the strategy differs by model size.

## Current Implementation (Phase 6a)

All projection weights (Q/K/V/O, gate/up/down, embed, lm_head) are stored on disk as F16 but **pre-converted to F32 at load time**. This trades 2x weight memory for an 8x inference speedup by eliminating F16-to-F32 conversion from the matvec inner loop.

### What Happens at Load Time

1. **Read `.btw` file** — F16 projections + F32 norms/biases, contiguous in a flat binary
2. **Cursor walk** — Parse the header, walk through weights, save raw F16 pointers
3. **Bulk convert** — `f16_array_to_f32()` allocates new F32 buffers and converts each weight matrix
4. **Assign to struct** — `ba_layer_weights` fields (`float *q_proj`, etc.) point to the new F32 buffers
5. **Keep raw buffer** — The original F16 data stays in memory because F32 norm weights and biases still point into it

### Data Flow

```
Disk (.btw file)
  │
  ├─ F16 projections ──→ f16_array_to_f32() ──→ float *q_proj  (new allocation)
  ├─ F16 projections ──→ f16_array_to_f32() ──→ float *k_proj  (new allocation)
  ├─ ...                                        ...
  ├─ F32 norms ─────────────────────────────────→ float *ln1_w  (points into raw buffer)
  ├─ F32 biases ────────────────────────────────→ float *q_bias (points into raw buffer)
  └─ F16 embed ─────→ f16_array_to_f32() ──→ float *embed   (new allocation)
```

### Memory Ownership

| Data | Owner | Freed by |
|------|-------|----------|
| F32 projection weights | Separately allocated per field | `ba_model_free()` — individual `uefi_free()` per field |
| F32 norms, biases | Point into raw buffer | `ba_model_free()` — `FreePages(raw_data)` frees the whole buffer |
| Raw F16 buffer | `m->raw_data` | `ba_model_free()` — `FreePages(raw_data)` |

### Why Not Free the Raw Buffer?

The F32 norm weights (`ln1_w`, `ln2_w`, `ln_out_w`) and optional biases (`q_bias`, `k_bias`, `v_bias`) point directly into the raw buffer. Freeing it would invalidate those pointers. The overhead is the original file size (~257 MB for SmolLM2-135M) which stays resident alongside the new F32 projections.

---

## Model Size Constraints

This F32 pre-conversion strategy doubles weight memory. Whether that fits depends on the model:

| Model | F16 on disk | F32 in memory | Raw buffer | Total weights | Fits 8 GB? |
|-------|-------------|---------------|------------|---------------|------------|
| SmolLM2-135M | 257 MB | 512 MB | 257 MB | ~770 MB | Yes |
| Qwen3-0.6B | ~1.2 GB | ~2.4 GB | ~1.2 GB | ~3.6 GB | Yes (tight) |
| RWKV-X 3.6B | ~6.8 GB | ~13.6 GB | ~6.8 GB | ~20.4 GB | No |
| Qwen2.5-7B | ~14 GB | ~28 GB | ~14 GB | ~42 GB | No |

For models that don't fit, the loader must use a different strategy.

---

## Strategy Per Model Size

### Small models (< 1B): F32 pre-conversion (current)

- Load F16 from disk, convert to F32 at load time
- `ba_layer_weights` fields are `float *` pointing to separately allocated F32 buffers
- Matvec: pure SSE F32 load/mul/add — no conversion overhead
- Best for: SmolLM2-135M, any model where 2x weight memory fits in RAM

### Medium models (1B-3B): F16 with LUT or keep F16 matvec

Two options:
1. **F16 lookup table** — 64K-entry `float[65536]` table (256 KB, fits in L2). Replace per-element bit manipulation with a single table lookup. Keeps F16 weight storage.
2. **F16 matvec with batch conversion** — The pre-Phase 6a approach, but optimized with the SSE batch converter (`f16x4_to_f32_sse`). Slower than F32 but uses half the memory.

### Large models (3B+): INT8 or INT4 quantization

- Weights stored as quantized integers with per-row/per-group scales
- Matvec does dequant in the inner loop (SSSE3 `pmaddubsw` for INT8, or scalar dequant for INT4)
- `ba_layer_weights` fields stay `float *` but point to dequantized buffers OR use separate `ba_q8_weight` struct
- This is a fundamentally different codepath — the existing `matvec_q8()` in qwen2.c is a starting point

---

## How to Add a New Model Size

The loader (`ba_model_load_buffer()` in `loader.c`) should select the strategy based on model dimensions from the `.btw` header:

```c
size_t f32_weight_bytes = (size_t)vocab * c * 2  /* embed + lm_head */
    + (size_t)cfg->n_layers * (q_dim*c + kv_dim*c*2 + c*q_dim + ffn*c*2 + c*ffn) * sizeof(float);

if (f32_weight_bytes < MAX_F32_BUDGET) {
    /* Small model: pre-convert to F32 */
    convert_weights_to_f32(...);
} else {
    /* Large model: keep F16 or use quantized weights */
    keep_f16_weights(...);
}
```

The `ba_layer_weights` struct fields are `float *` regardless — this is correct for all strategies. For F32 pre-conversion, they point to converted buffers. For F16, they'd point to F32 buffers populated by a different matvec (one that converts on the fly). For INT8, a separate `ba_q8_weight` struct is used alongside.

---

## Files

| File | Role |
|------|------|
| `model/model.h` | `ba_layer_weights` and `ba_model` structs — `float *` fields, `f16_to_f32()` helper |
| `model/loader.c` | `ba_model_load_buffer()` — cursor walk, F32 conversion, memory management |
| `model/qwen2.c` | `matvec_f32()` — pure SSE dot product, `ba_qwen_forward()` — forward pass |

## Performance

| Strategy | Decode tok/s (Dell E6510) | Memory (SmolLM2-135M) |
|----------|--------------------------|----------------------|
| F16 in-loop conversion (pre-Phase 6a) | ~1.5 | ~630 MB |
| **F32 pre-conversion (Phase 6a)** | **12** | **886 MB** |
