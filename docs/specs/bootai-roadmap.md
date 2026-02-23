# BootAI Roadmap

6-phase plan from bare-metal boot to full AI assistant.

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Boot + Console + Input | COMPLETE |
| 2 | Networking (DHCP, TCP4, HTTP, DNS) | COMPLETE |
| 3 | Foundry Tensor Library on UEFI | COMPLETE |
| 4 | Model Loading + Inference | IN PROGRESS |
| 5 | Tool Use | PLANNED |
| 6 | Full AI Assistant | PLANNED |

---

## Phase 1: Boot + Console + Input (COMPLETE)

Minimal UEFI application that boots, prints to serial/screen, and reads keyboard input.

- UEFI entry point (`efi_main`) with Microsoft x64 ABI
- Serial output via `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`
- Framebuffer text rendering via `EFI_GRAPHICS_OUTPUT_PROTOCOL`
- Keyboard input via `EFI_SIMPLE_TEXT_INPUT_PROTOCOL`
- Memory allocation via `EFI_BOOT_SERVICES.AllocatePages`
- Boot menu with option selection
- Custom linker script (`uefi.lds`) for PE32+ conversion
- PE relocation fixup handler (`reloc.c`)

## Phase 2: Networking (COMPLETE)

Full IP networking: DHCP, TCP4, DNS, HTTP/1.0 GET.

- DHCP via `EFI_IP4_CONFIG2_PROTOCOL` (automatic IP configuration)
- TCP4 via `EFI_TCP4_PROTOCOL` with service binding pattern
- HTTP/1.0 GET over raw TCP4 (header stripping, body extraction)
- URL parsing (host, port, path)
- UDP4 DNS resolver (RFC 1035 wire format over `EFI_UDP4_PROTOCOL`)
- Driver binding via `ConnectController` (two-pass recursive)
- REPL commands: `fetch <url>`, `netinfo`, `exit`
- Custom OVMF build with `NETWORK_IP4_ENABLE=TRUE`

## Phase 3: Foundry Tensor Library on UEFI (COMPLETE)

Foundry's tensor.c (9,318 lines) and quantization.c compile and run as part of the EFI binary.

- Shim headers (`shim/math.h`, `shim/string.h`, `shim/stdlib.h`, `shim/stdio.h`, `shim/time.h`) provide declaration-only libc interface
- `efi/uefi_libc.c` implements malloc/free via UEFI `AllocatePool`/`FreePool`, printf via ConOut, 29 math functions in pure C
- `efi/uefi_libc.h` force-included via `-include` redirects `malloc`→`uefi_malloc`, `printf`→`uefi_printf`, etc.
- Tensor self-tests pass: matmul, softmax, rms_norm, silu
- Key lesson: `static inline __builtin_*` wrappers crash in PE/COFF (PLT calls); declarations-only + global symbols work

## Phase 4: Model Loading + Inference (IN PROGRESS)

Load quantized models and run inference on bare metal. Current model: **SmolLM2-135M** (30 layers, 576 hidden, 9/3 GQA heads, 49K vocab).

### Completed

- Filesystem: read/write files via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (`efi/fs.c`)
- Weight loader: reads `.btw` binary format, auto-detects architecture from header (`model/loader.c`)
- Model converter: `foundry/convert/` converts HuggingFace safetensors → `.btw` (weights) + `.btv` (vocab). Supports Llama, Qwen2/2.5, Qwen3, Phi-3, Gemma2 families
- Qwen2/Llama forward pass: RMSNorm, GQA attention, RoPE (halved/rotate_half format), SwiGLU FFN, F16 weights with F32 activations (`model/qwen2.c`)
- KV cache: per-layer pre-allocated, oldest-half eviction when full (`model/kv_cache.c`)
- BPE tokenizer: GPT-2 style, loaded from `.btv` binary format (`tokenizer/bpe.c`)
- Token sampling: temperature, top-k, top-p, repeat penalty (`model/sampling.c`)
- Inference REPL: `infer` command → prompt → generate → display (`efi/main.c`)
- Math library: 29 functions in pure C (sinf, cosf, expf, sqrtf, etc.) for freestanding environment (`efi/uefi_libc.c`)
- **First successful inference**: SmolLM2-135M generates coherent text on bare metal, logits match Python F32 reference

### Remaining

- EOS detection: model doesn't stop generating (needs EOS token mapping fix)
- RWKV-X forward pass (WKV-7 kernel, sparse attention) — code exists but untested on bare metal
- RWKV World tokenizer (trie-based) — code exists but untested
- Larger models: Qwen2.5-Coder-7B (Q4), RWKV-X 3.6B (Q8)
- Performance: currently ~0.5 tok/s on SmolLM2-135M in QEMU (no hardware acceleration)

## Phase 5: Tool Use (PLANNED)

Parse model tool calls and dispatch to backends.

- `<tool_call>` tag detection in model output
- JSON argument parser (lightweight, no dependency)
- Tool dispatcher: route by tool name to backend
- `read_file` / `write_file` / `list_dir` — filesystem backends
- `web_search` — DuckDuckGo search via HTTP
- `web_fetch` — URL fetching via HTTP
- `system_info` — memory stats, uptime, hardware info
- Tool result injection as `tool` message in conversation

## Phase 6: Full AI Assistant (PLANNED)

Polish, persistence, and hardware support.

- Graphics mode: framebuffer text rendering with scrollback
- USB drive detection and boot
- Persistent conversation history (save/load from disk)
- NVRAM settings (model selection, sampling parameters)
- Multi-turn conversation management
- Install-to-disk option (dual-boot support)
- HTTPS/TLS (stretch goal — requires crypto library)
- IPv6 support (stretch goal)
