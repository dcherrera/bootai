# BootAI Roadmap

7-phase plan from bare-metal boot to full AI assistant.

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Boot + Console + Input | COMPLETE |
| 2 | Networking (DHCP, TCP4, HTTP, DNS) | COMPLETE |
| 3 | Foundry Tensor Library on UEFI | COMPLETE |
| 4 | Model Loading + Inference | COMPLETE |
| 5 | WiFi + Network API | COMPLETE |
| 6 | Tool Use | SCAFFOLDED |
| 7 | Full AI Assistant | PLANNED |

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

## Phase 4: Model Loading + Inference (COMPLETE)

Load quantized models and run inference on bare metal. Current model: **SmolLM2-135M-Instruct** (30 layers, 576 hidden, 9/3 GQA heads, 49K vocab).

### Completed

- Filesystem: read/write files via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (`efi/fs.c`)
- Weight loader: reads `.btw` binary format, auto-detects architecture from header (`model/loader.c`)
- Model converter: `foundry/convert/` converts HuggingFace safetensors → `.btw` (weights) + `.btv` (vocab). Supports Llama, Qwen2/2.5, Qwen3, Phi-3, Gemma2 families
- Qwen2/Llama forward pass: RMSNorm, GQA attention, RoPE (halved/rotate_half format), SwiGLU FFN, F16 weights with F32 activations (`model/qwen2.c`)
- KV cache: per-layer pre-allocated, oldest-half eviction when full (`model/kv_cache.c`)
- BPE tokenizer: GPT-2 style, loaded from `.btv` binary format (`tokenizer/bpe.c`)
- BPE special token handling: `<|im_start|>`, `<|im_end|>`, `<|endoftext|>` mapped correctly
- Chat template support: system/user/assistant message formatting
- Multi-turn chat with KV cache persistence across turns (model state preserved between exchanges)
- Token sampling: temperature, top-k, top-p, repeat penalty (`model/sampling.c`)
- Inference REPL: `infer` command → prompt → generate → display (`efi/main.c`)
- Full line editing: arrow keys (left/right), home/end, insert at cursor, backspace/delete
- Math library: 29 functions in pure C (sinf, cosf, expf, sqrtf, etc.) for freestanding environment (`efi/uefi_libc.c`)
- `make img` target: creates FAT32 disk image with EFI binary + model + vocab from scratch
- `make usb DISK=/dev/diskN`: writes disk image directly to USB via dd
- Export script: `tools/export_weights.py` converts HuggingFace models to `.btw`/`.btv` format (f16 and q8)
- **First successful inference**: SmolLM2-135M generates coherent text on bare metal, logits match Python F32 reference
- **Instruct model working**: SmolLM2-135M-Instruct with chat template produces instruction-following responses
- **Real hardware tested**: Surface Pro 7 (Intel Ice Lake, 4GB RAM) and QEMU with TCG software emulation

### Remaining

- RWKV-X forward pass (WKV-7 kernel, sparse attention) — code exists but untested on bare metal
- RWKV World tokenizer (trie-based) — code exists but untested
- Larger models: Qwen2.5-Coder-7B (Q4), RWKV-X 3.6B (Q8)

## Phase 5: WiFi + Network API (COMPLETE)

Connect to WiFi and expose inference over the network. Plug in a box, power on, talk to it from any device on the network. No keyboard or monitor needed.

### 5a. WiFi Connection (COMPLETE)

Full WiFi management via `EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL` (`efi/wifi.c`, 594 lines).

- WiFi2 protocol: scan for SSIDs (with RSSI + security type), connect, disconnect
- WPA2-PSK authentication via `EFI_SUPPLICANT_PROTOCOL` (credentials set before connect)
- Config file: `\wifi.conf` with multiple `[network]` blocks (max 16 networks, auto-connect on boot)
- REPL commands: `wifi`, `wifi scan`, `wifi connect <SSID>`, `wifi disconnect`, `wifi save`
- Driver loader: `efi/drivers.c` (205 lines) — scans `\drivers\` for `.efi` files, loads via `LoadImage()`/`StartImage()`, binds to hardware
- Fallback to wired Ethernet if WiFi unavailable (Ethernet works from Phase 2)
- Tested on real hardware: HP EliteBook with Intel AX201/AC-9560

### 5b. HTTP Server (COMPLETE)

HTTP/1.0 server on TCP4 (`efi/http_server.c`, 373 lines).

- `tcp4_listen(port)` — passive mode (`ActiveFlag=0`), listens on configurable port
- `tcp4_accept()` — accepts incoming connections with 500ms timeout
- Request parsing: method, path, headers, body (Content-Length support)
- Response formatting with status codes (200, 204, 400, 404, 503) and CORS headers
- Router: `OPTIONS *` → 204, `GET /health`, `GET /v1/models`, `POST /v1/chat/completions`, fallback → 404
- `ba_http_serve(port)` — single-threaded server loop with keypress polling to stop
- Pre-allocated buffers: 8KB request, 64KB response (no per-request heap allocation)

### 5c. OpenAI-Compatible Chat API (COMPLETE)

Three endpoints in `efi/api.c` that make BootAI a drop-in replacement for any OpenAI-compatible client.

**`POST /v1/chat/completions`** — Main inference endpoint:
- Parses `messages` array (role + content), `temperature`, `max_tokens` from JSON body
- Applies ChatML template (`<|im_start|>` / `<|im_end|>`)
- Runs full inference pipeline: state reset, tokenize, prefill, generate with sampling
- Returns OpenAI-format JSON with choices, finish_reason, and usage stats

**`GET /v1/models`** — Model listing (OpenAI format)

**`GET /health`** — Status check (`{"status":"ok"}`)

**JSON handling:**
- Hand-written parser: `json_get_string()`, `json_get_int()`, `json_get_float()`, `json_parse_messages()`
- JSON output escaping for special characters
- REPL integration: `serve [port]` command (default 8080)

### 5d. mDNS/Bonjour Discovery (PLANNED)

Advertise BootAI on the local network so clients can connect without knowing the IP address.

- Send multicast DNS response to `224.0.0.251:5353` (mDNS group)
- Advertise hostname `bootai.local` → DHCP-assigned IP
- Advertise service `_http._tcp.local` on port 8080
- Reuse existing UDP4 infrastructure from Phase 2
- Respond to mDNS queries (RFC 6762)

### 5e. Native Intel WiFi Driver — IWN (IN PROGRESS)

Bare-metal Intel WiFi driver ported from OpenBSD's `iwn` for machines without UEFI WiFi2 protocol support (e.g. Dell E6510 with Intel Centrino Advanced-N 6200). See [IWN WiFi Driver Architecture](../architecture/iwn-wifi-driver.md) for full details.

The driver has its own internal phases:

| IWN Phase | Description | Status |
|-----------|-------------|--------|
| 0 | PCI probe — enumerate bus, match device IDs | COMPLETE |
| 1 | MMIO + EEPROM — power on, read MAC/domain/RF config | COMPLETE |
| 2 | DMA + Firmware — allocate rings, load firmware, get UC_READY | COMPLETE |
| 3 | Scanning — RXON config, CMD_SCAN, parse beacon results | COMPLETE |
| 4 | Association + WPA2 — EAPOL handshake, CCMP keys | PLANNED |
| 5 | SNP integration — EFI_SIMPLE_NETWORK_PROTOCOL wrapper | PLANNED |

**IWN Phase 0-1** (2026-02-23): PCI bus enumeration finds Intel 6200, enables MMIO + bus master, reads EEPROM (MAC, RF config, calibration data, regulatory domain). Confirmed on Dell E6510 hardware.

**IWN Phase 2** (2026-02-24): DMA memory allocation below 4GB (~2.5MB total), firmware parsing (legacy v2 format), FH DMA upload via service channel 9, TX/RX ring setup, full hardware init sequence with post-alive calibration. 9 bugs found and fixed during real-hardware testing.

**IWN Phase 3** (2026-02-24): 8-command radio config sequence (BT_COEX, TX_ANT, RXON, ADD_NODE, LINK_QUALITY, TXPOWER, CRITICAL_TEMP, POWER) all succeed on Dell E6510. Radio receives beacons. Scan command built with proper 802.11 probe request frame. 10 bugs found and fixed during real-hardware debugging. See [IWN Phase 3 Debug Log](../research/iwn-phase3-debug-log.md).

**Files**: `bootai/iwn/` — 9 source files (~2,300 lines total: iwn.h, iwn_internal.h, iwn_shim.h, if_iwnreg.h, if_iwnvar.h, iwn_pci.c, iwn_hw.c, iwn_dma.c, iwn_fw.c, iwn_scan.c)

**Firmware**: `iwlwifi-6000-4.ucode` (454KB, legacy v2 format) renamed to `iwn-6000`, placed in `\drivers\` on the EFI partition.

**Supported hardware**: Full Intel 4965/5000/6000 family (~38 cards from ~2007-2013 laptops). See architecture doc for complete device table.

### Remaining

- mDNS discovery (5d) — quality-of-life, not a blocker
- Streaming mode (`stream: true`) for chat completions (chunked transfer encoding + SSE)
- IWN Phase 3-5: scanning, association, WPA2, SNP integration

## Phase 6: Tool Use (SCAFFOLDED)

Parse model tool calls and dispatch to backends. Header and file stubs exist (`tools/tool_use.h`, `tools/tool_use.c`, `tools/tools_fs.c`, `tools/tools_net.c`, `tools/tools_system.c`) but implementations are TODO.

- `<tool_call>` tag detection in model output
- JSON argument parser (lightweight, no dependency)
- Tool dispatcher: route by tool name to backend
- `read_file` / `write_file` / `list_dir` — filesystem backends (UEFI FS primitives exist from Phase 4)
- `web_search` — DuckDuckGo search via HTTP (HTTP client exists from Phase 2)
- `web_fetch` — URL fetching via HTTP (HTTP client exists from Phase 2)
- `system_info` — memory stats, uptime, hardware info
- Tool result injection as `tool` message in conversation
- Integration with inference loop in `main.c` (pause generation, dispatch, inject result, resume)

## Phase 7: Full AI Assistant (PLANNED)

Polish, persistence, and hardware support.

- Graphics mode: framebuffer text rendering with scrollback
- USB drive detection and boot
- Persistent conversation history (save/load from disk)
- NVRAM settings (model selection, sampling parameters)
- Multi-turn conversation management
- Install-to-disk option (dual-boot support)
- HTTPS/TLS (stretch goal — requires crypto library)
- IPv6 support (stretch goal)
