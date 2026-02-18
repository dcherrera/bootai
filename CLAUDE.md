# BootAI

UEFI application that boots directly into AI inference with tool use. No kernel, no OS — runs entirely in UEFI boot services mode.

## Project Structure

```
bootai/
├── CLAUDE.md
├── README.md
├── Makefile
├── efi/
│   ├── main.c                   # EFI entry point → boot menu → inference REPL
│   ├── efi.h                    # UEFI protocol types and helpers
│   ├── console.c                # Framebuffer text output via GOP
│   ├── input.c                  # Keyboard via Simple Text Input protocol
│   ├── fs.c                     # File ops via Simple File System protocol
│   └── net.c                    # Networking via TCP4/HTTP protocols
├── model/
│   ├── model.h                  # Common model interface (forward, load, sample)
│   ├── qwen2.h                  # Qwen2.5 config + forward pass
│   ├── qwen2.c                  # Transformer: RoPE, GQA, SwiGLU (Q4 weights)
│   ├── rwkvx.h                  # RWKV-X config + forward pass
│   ├── rwkvx.c                  # Hybrid: RWKV-7 blocks + sparse attention (Q8)
│   ├── wkv7.c                   # WKV-7 kernel
│   ├── rope.c                   # Rotary position embeddings (Qwen2.5)
│   ├── kv_cache.c               # KV cache for Qwen2.5 + sparse attn in RWKV-X
│   ├── loader.c                 # Load quantized weights, auto-detect architecture
│   └── sampling.c               # Temperature, top-k, top-p sampling
├── tools/
│   ├── tool_use.h               # Tool call parser + dispatcher
│   ├── tool_use.c               # Parse <tool_call> JSON, dispatch to backends
│   ├── tools_fs.c               # read_file, write_file, list_dir (UEFI FS)
│   ├── tools_net.c              # web_search, web_fetch (UEFI TCP4/HTTP)
│   ├── tools_system.c           # system_info, memory stats (UEFI)
│   └── export_weights.py        # Convert safetensors → flat quantized binary
├── tokenizer/
│   ├── bpe.c                    # BPE tokenizer (Qwen2.5)
│   ├── rwkv_tokenizer.c         # RWKV World tokenizer (trie-based)
│   └── vocab.bin                # Vocabulary data (per-model)
├── foundry/                     # Submodule: pure C tensor runtime
├── JACLibc/                     # Submodule: header-only libc
└── docs/
```

## Tech Stack

- **Language**: C (C99, `-ffreestanding`)
- **Tensor Runtime**: Foundry (`foundry/tensor/tensor.c`) — pure C, 9300 lines, 141 tests
- **Libc**: JACLibc (header-only, bare-metal compatible)
- **Target**: x86_64 UEFI
- **Models**: Qwen2.5-Coder-7B Q4, RWKV-X 3.6B Q8
- **Cross-compiler**: `x86_64-elf-gcc` (from macOS)
- **Emulator**: QEMU + OVMF firmware

## Build Commands

```bash
make efi              # Build rwkvos.efi
make run              # Run in QEMU with OVMF
make clean            # Clean build artifacts
make usb DISK=/dev/X  # Write bootable USB

# Model export
python3 tools/export_weights.py --model Qwen/Qwen2.5-Coder-7B-Instruct --quant q4_k_m --output model.bin
```

## Key Architecture

### UEFI Boot Services (no kernel)

We stay in boot services mode (never call `ExitBootServices`). This gives us:
- Filesystem via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
- Networking via `EFI_TCP4_PROTOCOL` / `EFI_HTTP_PROTOCOL`
- Display via `EFI_GRAPHICS_OUTPUT_PROTOCOL`
- Keyboard via `EFI_SIMPLE_TEXT_INPUT_PROTOCOL`
- Persistent storage via NVRAM variables
- IP configuration via `EFI_DHCP4_PROTOCOL`

### Model Interface

Both models implement the same interface (`model.h`):
- `model_load(path)` — load quantized weights from file
- `model_forward(tokens, state)` — run one forward pass
- `model_sample(logits, config)` — sample next token
- `model_free()` — cleanup

The loader auto-detects architecture from the weight file header.

### Tool Use

The inference loop detects `<tool_call>` tags in model output:
1. Pause generation
2. Parse JSON tool call (name + arguments)
3. Dispatch to backend (fs, net, system)
4. Inject result as `tool` message
5. Resume generation

Available tools: `read_file`, `write_file`, `list_dir`, `web_search`, `web_fetch`, `system_info`

### Quantization

- Qwen2.5: Q4_K_M (4-bit, ~4GB) — dequantize on the fly during matmul
- RWKV-X: Q8 (8-bit, ~3.6GB) — dequantize on the fly during matmul
- Foundry's quantization module handles both formats

## Naming Conventions

Follow Foundry conventions where using Foundry APIs (`tm_`, `ft_`, etc.).

For BootAI-specific code:
- `ba_` prefix for boot/app functions
- `efi_` prefix for UEFI wrapper functions
- Standard C naming otherwise

## Memory Budget (8GB minimum)

```
Qwen2.5 Q4 weights:     ~4.0 GB
KV cache (2K context):   ~0.5 GB
Activation tensors:      ~0.3 GB
UEFI + framebuffer:      ~0.2 GB
Free:                    ~3.0 GB
```

## Commit Rules

- All commits are from the developer — no AI co-author tags
- Never mention Claude, AI, or generated code in commits
- Write commit messages as if the developer wrote the code directly

## Testing

Test in QEMU with OVMF firmware:
```bash
make run    # Boots into QEMU with UEFI
```

For real hardware: write USB with `make usb`, boot from USB.

## Dependencies

### Submodules
- `foundry/` — Pure C tensor runtime (dcherrera/foundry)
- `JACLibc/` — Header-only libc (dcherrera/JACLibc)

### External (host machine only)
- `x86_64-elf-gcc` — Cross-compiler
- `qemu-system-x86_64` — Emulator
- OVMF firmware — UEFI for QEMU
- Python 3 + safetensors — Weight export script only
