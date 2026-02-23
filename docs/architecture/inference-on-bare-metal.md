# Inference on Bare Metal

How SmolLM2-135M runs on bare metal UEFI with no OS.

## Build Pipeline

### 1. Convert the model

The model converter (`foundry/convert/`) takes a HuggingFace model directory and produces two binary files:

```bash
cd foundry
make USE_BLAS=1 model-convert

./examples/model_convert \
  --model-dir ../models/base/SmolLM2-135M/ \
  --output ../bootai/esp/rwkvos/model.btw
```

This reads `config.json`, `model.safetensors`, and `tokenizer.json` from the model directory and writes:

- `model.btw` — weights in binary format (F16 projections, F32 norms, 128-byte header with model config)
- `vocab.btv` — vocabulary + merge rules in binary format

The converter auto-detects the model family from `config.json`'s `model_type` field. Supported families: Llama 2/3, SmolLM, Mistral, Qwen 2/2.5/3, Phi-3, Gemma 2.

### 2. Build the EFI binary

```bash
cd bootai
make efi          # Builds all_bootai.efi
```

This cross-compiles with `x86_64-elf-gcc` using freestanding flags, links with a custom linker script, and converts to PE32+ format via objcopy.

### 3. Create the disk image

The disk image is a FAT32 filesystem image that QEMU mounts as a virtual disk. It contains the EFI binary, model weights, and vocabulary.

```bash
# Create a 300MB FAT32 image with mtools
dd if=/dev/zero of=bootai.img bs=1M count=300
mformat -i bootai.img -F ::

# Create directory structure
mmd -i bootai.img ::EFI
mmd -i bootai.img ::EFI/BOOT
mmd -i bootai.img ::rwkvos

# Copy files
mcopy -i bootai.img all_bootai.efi ::EFI/BOOT/BOOTX64.EFI
mcopy -i bootai.img esp/rwkvos/model.btw ::rwkvos/model.btw
mcopy -i bootai.img esp/rwkvos/vocab.btv ::rwkvos/vocab.btv
```

The image must be large enough to hold the model weights. For SmolLM2-135M (~257 MB weights + ~1.2 MB vocab), 300 MB is sufficient.

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
  -display none \
  -serial stdio \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci
```

### 6. Interact

At the boot menu, select `1` (Run Live). Then at the REPL prompt:

```
> infer
  Enter prompt (empty line to cancel):
  > hello
  Prompt: 1 tokens [28120]
  Generating...

  , "I'm sorry, I didn't know that."
  ...
```

## Updating the EFI binary

After code changes, you can update just the EFI binary without recreating the entire image:

```bash
make efi
mcopy -o -i bootai.img all_bootai.efi ::EFI/BOOT/BOOTX64.EFI
make run
```

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
