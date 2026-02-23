# Inference on Bare Metal

How SmolLM2-135M runs on bare metal UEFI with no OS.

## Build Pipeline

### 1. Convert the model

The export script downloads a model from HuggingFace, extracts weights as F16, and writes two binary files:

```bash
cd bootai
python3 tools/export_weights.py \
  --model HuggingFaceTB/SmolLM2-135M-Instruct \
  --quant f16 \
  --output ../models/base/SmolLM2-135M-Instruct/model.btw
```

This produces:

- `model.btw` — weights in binary format (F16 projections, F32 norms, 128-byte header with model config)
- `model.btv` — vocabulary + merge rules in binary format (written to the same directory as `model.btw`)

The script also supports `--quant q8` for INT8 quantization and `--vocab-only` for exporting just the tokenizer.

The converter auto-detects the model family from the HuggingFace config. Supported families: Llama 2/3, SmolLM, Mistral, Qwen 2/2.5/3, Phi-3, Gemma 2.

### 2. Build the EFI binary

```bash
cd bootai
make phase4       # Builds all_bootai.efi
```

This cross-compiles with `x86_64-elf-gcc` using freestanding flags (`-O2 -msse4.2`), links with a custom linker script, and converts to PE32+ format via objcopy.

### 3. Create the disk image

The disk image is a FAT32 filesystem image that QEMU mounts as a virtual disk. It contains the EFI binary, model weights, and vocabulary.

```bash
make img
```

This creates a FAT32 image sized to fit the model, then copies the EFI binary, `model.btw`, and `vocab.btv` into it. Model source paths are configurable via the `MODEL_BTW` and `MODEL_BTV` make variables, which default to `../models/base/SmolLM2-135M-Instruct/`.

**Note:** `bootai.img` is gitignored because it contains large binary model weights. You must create it locally.

### 4. Get OVMF firmware

QEMU needs UEFI firmware (OVMF) to boot EFI applications. The project uses a custom OVMF build with networking enabled (`NETWORK_IP4_ENABLE=TRUE`).

The firmware files (`ovmf-code.fd` and `ovmf-vars.fd`) are also gitignored. To get them:

- **From EDK2 source:** Build OVMF with `NETWORK_IP4_ENABLE=TRUE` flag
- **From Homebrew:** `brew install qemu` provides basic OVMF at `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` (works but no networking)

### 5. Run in QEMU

```bash
make run
```

Or manually:

```bash
cp ovmf-vars.fd esp/ovmf-vars-session.fd
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=ovmf-code.fd \
  -drive if=pflash,format=raw,file=esp/ovmf-vars-session.fd \
  -drive file=bootai.img,format=raw,if=virtio \
  -m 8G \
  -accel tcg,thread=multi,tb-size=2048 \
  -cpu max \
  -smp 2 \
  -display none \
  -serial stdio \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci
```

### 6. Interact

At the boot prompt, use the `chat` command for interactive conversation:

```
BootAI v0.1 — Bare Metal AI Inference
  Loading model from \rwkvos\model.btw...
  SmolLM2-135M: 30 layers, 576 hidden, 9 heads, 49152 vocab
  Loading vocab from \rwkvos\vocab.btv...
  49152 tokens, 48900 merges loaded

boot> chat
> Hello
Prefilling.... (21 tokens)
Hello! How can I help you today?
> What is 2+2?
Prefilling... (13 tokens)
2+2 equals 4.
```

## Updating the EFI binary

After code changes, you can update just the EFI binary without recreating the entire image. The `make run` target already updates the image via mcopy, so you can just do:

```bash
make phase4 && make run
```

## Flashing to USB

Write the disk image to a USB drive for testing on real hardware:

```bash
make usb DISK=/dev/diskN
```

This copies the latest EFI binary into bootai.img, then writes the entire image to the USB with dd. The target device is the raw disk (e.g., `/dev/disk4`), not a partition.

On macOS, find the disk number with `diskutil list external`.

Tested hardware: Surface Pro 7 (Intel Ice Lake, 4GB RAM).

## Model Architecture (SmolLM2-135M)

```
Token → Embedding lookup (F16 → F32)
  ↓
30x Transformer Block:
  ├─ RMSNorm (pre-attention)
  ├─ Q/K/V projections (F16 matvec)
  ├─ RoPE (halved/rotate_half pairing)
  ├─ GQA attention (9 Q heads, 3 KV heads, head_dim=64)
  ├─ Output projection + residual
  ├─ RMSNorm (pre-FFN)
  ├─ SwiGLU FFN (gate * silu(up), then down projection)
  └─ FFN residual
  ↓
Final RMSNorm → LM Head (F16 matvec) → Logits
```

- **Weights:** F16 (10-bit mantissa, lossless from BF16 source)
- **Activations:** F32 throughout (W16A32)
- **KV cache:** F32, pre-allocated per layer
- **Tied embeddings:** `lm_head` shares weights with `embed_tokens`
- **RoPE theta:** 100,000 (rope_theta from config.json)
- **RMS epsilon:** 1e-5

## Memory Layout

```
SmolLM2-135M F16 weights:   ~257 MB
KV cache (512 context):       ~7 MB
Workspace (activations):      ~2 MB
UEFI + framebuffer:          ~64 MB
─────────────────────────────────────
Total:                       ~330 MB
```

Easily fits in QEMU's 8GB allocation. On real hardware, any machine with 1GB+ RAM can run this model.

## Key Technical Decisions

### F16 weights instead of INT8

SmolLM2-135M is small enough (135M params) that F16 weights fit easily in memory. F16 preserves all precision from the original BF16 weights (BF16 has 7 mantissa bits, F16 has 10). INT8 quantization on a model this small causes measurable quality loss.

### RoPE: halved format (rotate_half)

HuggingFace stores Q/K projection weights in the order expected by `rotate_half` — element `i` pairs with element `i + head_dim/2`. This differs from llama.cpp's interleaved format `(2i, 2i+1)`, which requires a weight permutation during GGUF conversion.

Since our converter loads weights directly from safetensors without permutation, we must use the halved format.

### Pure C math library

The freestanding environment has no libm. All math functions (sinf, cosf, expf, sqrtf, logf, etc.) are implemented in `efi/uefi_libc.c`. Key implementations:

- **sqrtf:** x86 `sqrtss` instruction (hardware, exact)
- **sinf/cosf:** Quadrant reduction + degree-7 minimax polynomial (<1 ULP error)
- **expf:** Cody-Waite range reduction + degree-6 polynomial
- **RoPE frequencies:** Iterated `sqrtf` to compute `theta^(1/n)` — avoids `powf` which would need the less-accurate `logf`

### No AVX/F16C on UEFI

UEFI firmware does not enable AVX (CR4.OSXSAVE is not set, XSAVE state not configured). Any VEX-encoded instruction causes #UD (Invalid Opcode) on bare metal.

The F16C instruction `vcvtph2ps` uses VEX encoding, so hardware f16-to-f32 conversion cannot be used. The compiler flag `-mf16c` must NOT be used because it causes GCC to emit VEX-encoded instructions throughout the generated code (not just f16 conversion).

BootAI uses a software f16-to-f32 conversion via bit manipulation instead. SSE (up to SSE4.2) is safe — UEFI guarantees SSE state is configured.
