# BootAI Roadmap

7-phase plan from bare-metal boot to full AI assistant.

## Phase Overview

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Boot + Console + Input | COMPLETE |
| 2 | Networking (DHCP, TCP4, HTTP, DNS) | COMPLETE |
| 3 | Foundry Tensor Library on UEFI | COMPLETE |
| 4 | Model Loading + Inference | COMPLETE |
| 5 | WiFi + Network API | PLANNED |
| 6 | Tool Use | PLANNED |
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

## Phase 5: WiFi + Network API (PLANNED)

Connect to WiFi and expose inference over the network. This is the real value proposition — plug in a box, power on, talk to it from any device on the network. No keyboard or monitor needed.

### 5a. WiFi Connection

Two modes for WiFi connectivity:

**Pre-configured (headless auto-connect):**
- Config file at `\rwkvos\wifi.conf` on the boot drive (FAT32 ESP)
- Format: `ssid=MyNetwork\npassword=MyPassword` (plain text, one key=value per line)
- On boot: read config via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (Phase 1), connect automatically
- Zero-touch headless operation — no keyboard or monitor required

**Interactive scan mode:**
- `EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL` to scan for available SSIDs
- Display network list on console, user selects and enters password
- Optionally save credentials to `wifi.conf` for next boot

**Authentication:**
- WPA2-PSK (most common home/office WiFi)
- WiFi protocols handle authentication at the UEFI driver level — we just provide SSID + PSK

**Fallback:**
- If WiFi unavailable (no wireless protocol in firmware), fall back to wired Ethernet
- Ethernet already works from Phase 2 — DHCP via `EFI_IP4_CONFIG2_PROTOCOL`, TCP4 via service binding
- WiFi support depends on UEFI firmware exposing wireless protocols. Most modern UEFI on laptops/NUCs include wireless drivers. Desktops may need USB WiFi adapters with UEFI driver support

### 5b. HTTP Server

Build a minimal HTTP/1.1 server on top of the existing TCP4 infrastructure from Phase 2.

**What already exists (reuse from Phase 2):**
- `tcp4_sb` service binding (`net.c:55`)
- `tcp4_connect`, `tcp4_send`, `tcp4_recv`, `tcp4_close` (`net.c`)
- DHCP via `IP4Config2` (`net.c`)
- DNS resolver over UDP4 (`net.c`)
- URL parsing, IP printing helpers (`net.c`)

**Key difference — server vs client:**
Current TCP4 code is **client-only** (`ActiveFlag=1` in config = active connect). For a server:
- Set `ActiveFlag=0` (passive/listen mode) in `EFI_TCP4_CONFIG_DATA`
- Call `Accept()` (currently `void *` stub in `efi.h:139`) to accept incoming connections
- Each accepted connection gets its own child handle via `tcp4_sb->CreateChild()`

**Implementation steps:**

1. **Type the Accept function pointer** in `efi.h` (currently `void *Accept`)
   ```c
   EFI_STATUS (EFIAPI *Accept)(struct EFI_TCP4_PROTOCOL *This,
                                EFI_TCP4_LISTEN_TOKEN *ListenToken);
   ```

2. **Create listening socket** — `tcp4_listen(port)`:
   - `tcp4_sb->CreateChild()` to get a TCP4 instance
   - Configure with `ActiveFlag=0`, `StationPort=8080`, `StationAddress=0.0.0.0`
   - This puts the socket in passive/listen mode

3. **Accept loop** — `tcp4_accept(listener, conn)`:
   - Call `tcp4->Accept()` with a listen token containing a completion event
   - On completion, the token contains a `NewChildHandle` — a new TCP4 instance for the accepted connection
   - Get `EFI_TCP4_PROTOCOL` from the new child handle
   - Wrap in `tcp4_conn_t` for reuse with existing `tcp4_send`/`tcp4_recv`/`tcp4_close`

4. **Request parsing** — `http_parse_request(buf, len, request)`:
   - Parse request line: `METHOD /path HTTP/1.1\r\n`
   - Parse headers: extract `Content-Length`, `Content-Type`, `Host`
   - Identify body start (`\r\n\r\n` delimiter)
   - No need for full HTTP parser — only 3 endpoints

5. **Response sending** — `http_send_response(conn, status, content_type, body, body_len)`:
   - Format: `HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: N\r\n\r\n{body}`
   - Reuse `tcp4_send()` for wire transmission

6. **Router** — `http_route(request, conn)`:
   - `POST /v1/chat/completions` → `handle_chat_completions()`
   - `GET /v1/models` → `handle_models()`
   - `GET /health` → `handle_health()`
   - Anything else → `404 Not Found`

7. **Main server loop** — `http_server_start(port)`:
   ```
   tcp4_listen(8080)
   loop:
     tcp4_accept(listener, &conn)
     http_parse_request(recv_buf, &request)
     http_route(&request, &conn)
     tcp4_close(&conn)
   ```
   Single-threaded — one request at a time (inference is sequential anyway)

### 5c. OpenAI-Compatible Chat API

Three endpoints that make BootAI a drop-in replacement for any OpenAI-compatible client.

**`POST /v1/chat/completions`** — Main inference endpoint:
- Parse JSON body: extract `messages` array (role + content pairs), `temperature`, `max_tokens`, `stream`
- Convert messages to prompt string (apply chat template)
- Run inference via `model_forward()` / `rx_forward()` loop
- **Non-streaming mode** (`stream: false` or absent):
  - Accumulate all generated tokens
  - Return single JSON response:
    ```json
    {"id":"chatcmpl-1","object":"chat.completion","model":"engram-3.6b",
     "choices":[{"index":0,"message":{"role":"assistant","content":"..."},
     "finish_reason":"stop"}],
     "usage":{"prompt_tokens":N,"completion_tokens":M,"total_tokens":N+M}}
    ```
- **Streaming mode** (`stream: true`):
  - Send `Transfer-Encoding: chunked` header
  - For each generated token, send SSE chunk:
    ```
    data: {"id":"chatcmpl-1","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"token"},"finish_reason":null}]}\n\n
    ```
  - Final chunk: `data: [DONE]\n\n`
  - Requires chunked encoding: `{hex_length}\r\n{chunk_data}\r\n`

**`GET /v1/models`** — Model listing:
- Return JSON with loaded model info:
  ```json
  {"object":"list","data":[{"id":"engram-3.6b","object":"model","owned_by":"bootai"}]}
  ```

**`GET /health`** — Status check:
- Return `{"status":"ok","model":"engram-3.6b","uptime_seconds":N}`
- Useful for load balancers, monitoring, client auto-discovery

**JSON parsing:**
- Lightweight hand-written parser — no dependency
- Only need to parse: `messages` array, `temperature` float, `max_tokens` int, `stream` bool
- JSON generation: `snprintf` with format strings (responses are fixed-structure)

### 5d. mDNS/Bonjour Discovery

Advertise BootAI on the local network so clients can connect without knowing the IP address.

**How it works:**
- Send multicast DNS response to `224.0.0.251:5353` (mDNS group)
- Advertise hostname `bootai.local` → our DHCP-assigned IP
- Also advertise service `_http._tcp.local` on port 8080
- Clients find BootAI via: `curl http://bootai.local:8080/health`

**Implementation:**
- Reuse existing UDP4 infrastructure from Phase 2 (DNS resolver already uses UDP4)
- On boot (after DHCP + WiFi connect): send mDNS announcement packet
- Respond to mDNS queries for `bootai.local` (listen on UDP 5353)
- Packet format: standard DNS response (RFC 6762) with A record pointing to our IP
- Refresh announcement periodically (mDNS TTL-based)

**Client experience:**
```bash
# macOS/Linux — zero-config discovery
curl http://bootai.local:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'

# Python openai library
import openai
client = openai.OpenAI(base_url="http://bootai.local:8080/v1", api_key="unused")
response = client.chat.completions.create(model="engram-3.6b", messages=[...])
```

## Phase 6: Tool Use (PLANNED)

Parse model tool calls and dispatch to backends.

- `<tool_call>` tag detection in model output
- JSON argument parser (lightweight, no dependency)
- Tool dispatcher: route by tool name to backend
- `read_file` / `write_file` / `list_dir` — filesystem backends
- `web_search` — DuckDuckGo search via HTTP
- `web_fetch` — URL fetching via HTTP
- `system_info` — memory stats, uptime, hardware info
- Tool result injection as `tool` message in conversation

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
