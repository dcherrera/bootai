# BootAI

UEFI application that boots directly into AI inference with tool use. No kernel, no OS — runs entirely in UEFI boot services mode.

## Relationship to Echad

BootAI is the **static core** of the Echad distributed cluster system (`../Echad/`). It is not a standalone product — it is the foundation every Echad node runs on. The cluster features (Raft consensus, distributed storage, state store) are built on top of BootAI.

BootAI's code compiles for multiple targets via a platform HAL:

| Target | HAL | Binary | Boot |
|--------|-----|--------|------|
| Tier 1 (UEFI node) | `efi/` | `all_bootai.efi` | UEFI direct |
| GPU node | `linux/` (to be built) | `echad-daemon` | Minimal Linux kernel (EFISTUB) |
| Tier 2 (Desktop) | `app/` (to be built) | Desktop app | User's OS |

Platform-independent code (`model/`, `tokenizer/`, `tools/`, `foundry/`) compiles for all targets. Platform-specific code (`efi/`, `linux/`, `app/`) implements the same HAL interface with different backends.

See `../Echad/Echad.md` for the full architecture.

## Project Structure

```
bootai/
├── CLAUDE.md
├── README.md
├── Makefile
├── efi/
│   ├── main.c                   # EFI entry point → boot menu → inference REPL
│   ├── efi.h                    # UEFI protocol types and helpers
│   ├── console.c                # Text output via ConOut + log tee to bootai.log
│   ├── input.c                  # Keyboard via Simple Text Input protocol
│   ├── fs.c                     # File ops via Simple File System protocol
│   ├── net.c                    # Networking via TCP4/HTTP protocols
│   └── mdns.c                   # mDNS/Bonjour responder (bootai.local)
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
make efi              # Build all_bootai.efi (full build — boots into menu)
make PROD=1 efi       # Build production EFI (boots straight into chat)
make release          # Build PROD + GPT disk image (flashable with balenaEtcher/dd)
make run              # Run in QEMU with OVMF (interactive — do NOT run from Claude)
make run-prod         # Clean build PROD + run in QEMU (straight to chat — do NOT run from Claude)
make clean            # Clean build artifacts

# Model export
python3 tools/export_weights.py --model Qwen/Qwen2.5-Coder-7B-Instruct --quant q4_k_m --output model.bin
```

### Build Flags

| Flag | What | When |
|------|------|------|
| `PROD=1` | Skip boot menu and REPL, boot straight into chat inference | Production/demo builds |

## Flashing USB

The USB drive is FAT32 with an existing EFI directory structure. To flash the latest build, mount and copy — do NOT use `dd` or `sudo`:

```bash
# 1. Find the USB disk
diskutil list external
# Look for the BOOTAI partition (e.g. /dev/disk11s1)

# 2. Mount, copy EFI binary, eject
diskutil mount /dev/diskNs1
cp all_bootai.efi /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI
diskutil eject /dev/diskN
```

### USB directory layout

The USB must have this structure for the model to load:

```
BOOTAI/
├── EFI/BOOT/BOOTX64.EFI          ← all_bootai.efi
├── rwkvos/
│   ├── model.btw                  ← quantized weights
│   └── vocab.btv                  ← tokenizer vocabulary
├── drivers/                       ← optional UEFI drivers
├── workspace/                     ← user files
└── wifi.conf                      ← saved WiFi credentials
```

### First-time USB setup

If the model/vocab files are missing, copy them:

```bash
# Source files are in ../models/base/SmolLM2-135M-Instruct/
cp ../models/base/SmolLM2-135M-Instruct/model.btw /Volumes/BOOTAI/rwkvos/model.btw
cp ../models/base/SmolLM2-135M-Instruct/model.btv /Volumes/BOOTAI/rwkvos/vocab.btv
```

**Note:** The source vocab file is named `model.btv` but the code expects `vocab.btv`. Always rename when copying.

**Important:** `dd` requires sudo which is not available from Claude's shell. Always use mount+copy instead.

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

## Console Logging (`bootai.log`)

All `efi_print_ascii()` output is tee'd into a 128KB in-memory buffer. The buffer is flushed to `\bootai.log` on the USB drive at the top of each REPL iteration (before the `> ` prompt). This gives you a full transcript of every boot session.

### How it works

1. `console.c` accumulates all `efi_print_ascii()` output in a static `log_buf[128KB]`
2. `efi_log_flush()` calls `efi_write_file("bootai.log", log_buf, log_pos)` — overwrites the file each time with the full buffer
3. The flush runs at the top of the main REPL loop, so every command's output is persisted before the next prompt
4. The log is plain ASCII — no UCS-2 conversion

### Reading the log

After a session on real hardware, mount the USB on Mac and read:

```bash
diskutil mount /dev/diskNs1
cat /Volumes/BOOTAI/bootai.log
```

### Limitations

- **128KB max** — if output exceeds this, later output is silently dropped
- **No flush during long commands** — if you power off mid-command (e.g. during `iwn connect`), that command's output is lost. Wait for the `> ` prompt to ensure the flush completed.
- **Overwrites each flush** — the file always contains the current session from boot to the last completed command. No append across boots.
- **Typed commands not captured** — `efi_read_line()` echoes via `efi_print()` (UCS-2 direct), not `efi_print_ascii()`, so user input text bypasses the log buffer. You see `> ` prompts but not what was typed.

### Key functions

| Function | File | Purpose |
|----------|------|---------|
| `efi_log_flush()` | `console.c` | Write buffer to `\bootai.log` via `efi_write_file` |
| `efi_fs_root()` | `fs.c` | Exposes static `fs_root` for log open check |

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
