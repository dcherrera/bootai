# BootAI Roadmap

6-phase plan from bare-metal boot to full AI assistant.

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Boot + Console + Input | COMPLETE |
| 2 | Networking (DHCP, TCP4, HTTP, DNS) | COMPLETE |
| 3 | Filesystem (EFI_SIMPLE_FILE_SYSTEM) | PLANNED |
| 4 | Model Loading + Inference | PLANNED |
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

## Phase 3: Filesystem (PLANNED)

Read/write files on the boot FAT32 partition.

- Initialize `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
- Open, read, write, close files
- Directory listing
- Model weight file loading from disk
- Conversation history persistence
- Settings storage (JSON or key-value)

## Phase 4: Model Loading + Inference (PLANNED)

Load quantized models and run inference on bare metal.

- Foundry tensor library integration (pure C, `tensor.c`)
- JACLibc wiring (`malloc` via `AllocatePages`, `printf` via ConOut)
- Weight loader (auto-detect Qwen2.5 Q4 vs RWKV-X Q8)
- Qwen2.5-Coder-7B forward pass (RoPE, GQA, SwiGLU, Q4 dequant)
- RWKV-X 3.6B forward pass (WKV-7 kernel, sparse attention, Q8 dequant)
- KV cache management (bounded memory budget)
- BPE tokenizer (Qwen2.5) and RWKV World tokenizer (trie)
- Token sampling (temperature, top-k, top-p)
- Inference REPL (prompt -> generate -> display)

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
