# HTTP Server — OpenAI-Compatible API

BootAI includes a headless HTTP server that exposes an OpenAI-compatible API. Any device on the network can send inference requests to a bare-metal UEFI machine running BootAI — no OS required.

## Quick Start

```bash
# Build
make clean && make phase4

# Run in QEMU (interactive — run this yourself)
make run

# At the REPL:
boot> serve          # starts on port 8080
boot> serve 9090     # or specify a port
```

From another terminal:

```bash
# Health check
curl http://localhost:8080/health

# List models
curl http://localhost:8080/v1/models

# Chat completion
curl -X POST http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":64}'

# Python openai library
import openai
client = openai.OpenAI(base_url="http://localhost:8080/v1", api_key="unused")
r = client.chat.completions.create(
    model="bootai-smollm2-135m",
    messages=[{"role":"user","content":"Hello"}]
)
print(r.choices[0].message.content)
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Returns `{"status":"ok"}` |
| `GET` | `/v1/models` | Returns model list (OpenAI format) |
| `POST` | `/v1/chat/completions` | Run inference (non-streaming) |
| `OPTIONS` | `*` | CORS preflight (returns 204) |

### POST /v1/chat/completions

**Request body:**

```json
{
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Hello"}
  ],
  "temperature": 0.7,
  "max_tokens": 256
}
```

**Response:**

```json
{
  "id": "chatcmpl-bootai",
  "object": "chat.completion",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "Hello! How can I help you today?"
    },
    "finish_reason": "stop"
  }],
  "usage": {
    "prompt_tokens": 24,
    "completion_tokens": 8,
    "total_tokens": 32
  }
}
```

**Parameters:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `messages` | array | required | Chat messages (role + content) |
| `temperature` | float | 0.7 | Sampling temperature |
| `max_tokens` | int | 256 | Maximum tokens to generate (capped at 2048) |

## Architecture

### Files

| File | Purpose |
|------|---------|
| `efi/http_server.c` | HTTP parsing, response formatting, router, server loop |
| `efi/api.c` | OpenAI API handlers, JSON parser, inference integration |
| `efi/net.c` | TCP4 listen/accept (server primitives) |
| `efi/efi.h` | TCP4 server types and declarations |
| `efi/main.c` | `serve` REPL command |

### Request Flow

```
Client (curl/openai) ──TCP──► QEMU port forward ──► UEFI TCP4 stack
                                                         │
                                                    tcp4_accept()
                                                         │
                                                    tcp4_recv() ── read full request
                                                         │
                                                    http_parse_request()
                                                         │
                                                    http_route()
                                                         │
                                            ┌────────────┼────────────┐
                                            │            │            │
                                       /health    /v1/models   /v1/chat/completions
                                            │            │            │
                                            │            │      Parse messages
                                            │            │      Build chat template
                                            │            │      Reset model
                                            │            │      Tokenize + prefill
                                            │            │      Generate tokens
                                            │            │      Format JSON response
                                            │            │            │
                                            └────────────┴────────────┘
                                                         │
                                                    tcp4_send() ── HTTP/1.0 response
                                                         │
                                                    tcp4_close()
```

### Design Decisions

**HTTP/1.0 with `Connection: close`** — Simplest protocol. One request per TCP connection. No chunked encoding, no keep-alive, no pipelining.

**Single-threaded** — UEFI has no threading primitives. The server handles one request at a time. During inference, no other requests are accepted.

**Non-streaming** — Full response is accumulated and sent at once. Streaming SSE would require a different TCP management approach (future work).

**Pre-allocated buffers** — 8KB request buffer, 64KB response buffer. No per-request memory allocation. Safe for bare-metal where heap is limited.

**500ms accept timeout** — `tcp4_accept()` polls for 500ms then returns `EFI_TIMEOUT`. The server loop checks for a keypress between accept cycles, allowing the user to stop the server by pressing any key.

**CORS headers** — All responses include `Access-Control-Allow-Origin: *` so browser-based clients work out of the box.

### TCP4 Server Primitives

The existing TCP4 code in `net.c` was client-only (outbound connections). The server adds two functions:

| Function | Description |
|----------|-------------|
| `tcp4_listen(port, listener)` | Creates a TCP4 child in passive mode (`ActiveFlag=0`), bound to the given port |
| `tcp4_accept(listener, client)` | Issues `Accept`, polls 500ms, returns client connection or `EFI_TIMEOUT` |

The existing `tcp4_send`, `tcp4_recv`, `tcp4_close` were made non-static for use by the HTTP server.

### JSON Parser

The JSON parser in `api.c` is minimal and purpose-built — it only extracts known fields:

- `json_get_string(json, len, key, &out_len)` — extract string value by key
- `json_get_int(json, len, key, default)` — extract integer value
- `json_get_float(json, len, key, default)` — extract float value
- `json_parse_messages(json, len, msgs, max)` — iterate the `messages` array

It handles JSON escapes in strings and nested objects, but doesn't build a parse tree.

### Chat Template

Messages are formatted using the ChatML template (same as the `chat` command):

```
<|im_start|>system
You are a helpful assistant.<|im_end|>
<|im_start|>user
Hello<|im_end|>
<|im_start|>assistant
```

The model state is reset for each request (no cross-request conversation state).

## QEMU Port Forwarding

The `run` and `run3` Makefile targets include QEMU user-mode networking with port forwarding:

```
-netdev user,id=net0,hostfwd=tcp::8080-:8080
```

This maps host port 8080 to guest port 8080, so `curl http://localhost:8080/...` reaches the UEFI TCP4 stack.

## Limitations

- **No streaming** — responses are sent all at once after generation completes
- **No authentication** — `api_key` parameter is ignored
- **Single model** — always uses the loaded model (SmolLM2-135M)
- **No conversation persistence** — model state resets per request
- **32 messages max** — `messages` array is limited to 32 entries
- **4KB chat template** — combined messages must fit in 4KB after template expansion
- **8KB request body** — larger POST bodies are truncated
