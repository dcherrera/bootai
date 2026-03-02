# Connecting to BootAI Over the Network

BootAI exposes an OpenAI-compatible API over Ethernet. Any tool that speaks the OpenAI protocol can connect to it — you're talking to a machine running with no OS.

## Prerequisites

1. BootAI booted on hardware with Ethernet connected
2. Run `serve` at the REPL (or use PROD mode which auto-serves)
3. Note the IP address (run `netinfo` on the Dell)

## API Endpoint

```
Base URL:  http://<IP>:8080/v1
API Key:   not required (use any value)
Model:     bootai-smollm2-135m
```

## OpenCode

[OpenCode](https://github.com/opencode-ai/opencode) is a terminal-based AI coding agent. To connect it to BootAI:

**1. Edit `~/.config/opencode/config.json`:**

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "bootai": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "BootAI (bare metal)",
      "options": {
        "baseURL": "http://<IP>:8080/v1"
      },
      "models": {
        "bootai-smollm2-135m": {
          "name": "BootAI SmolLM2-135M"
        }
      }
    }
  }
}
```

Replace `<IP>` with the Dell's IP address (e.g. `192.168.0.101`).

**2. Launch OpenCode:**

```bash
opencode
```

**3. Select the BootAI provider** using `Ctrl+,` or the model picker.

## curl

```bash
# Health check
curl http://<IP>:8080/health

# List models
curl http://<IP>:8080/v1/models

# Chat
curl http://<IP>:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'
```

## Python (OpenAI SDK)

```bash
pip install openai
```

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://<IP>:8080/v1",
    api_key="unused"
)

response = client.chat.completions.create(
    model="bootai-smollm2-135m",
    messages=[{"role": "user", "content": "Hello"}]
)
print(response.choices[0].message.content)
```

## Open WebUI

[Open WebUI](https://github.com/open-webui/open-webui) provides a browser-based chat interface.

```bash
docker run -d -p 3000:8080 \
  -e OPENAI_API_BASE_URL=http://<IP>:8080/v1 \
  -e OPENAI_API_KEY=unused \
  ghcr.io/open-webui/open-webui:main
```

Then open `http://localhost:3000` in your browser.

## Any OpenAI-Compatible Tool

BootAI works with any tool that supports custom OpenAI API endpoints. Set:

| Setting | Value |
|---------|-------|
| Base URL | `http://<IP>:8080/v1` |
| API Key | any value |
| Model | `bootai-smollm2-135m` |

This includes VS Code extensions (Continue, Cline), desktop apps (Jan, LM Studio client mode), and any OpenAI SDK in any language.

## Limitations

- **No streaming** — responses are sent after generation completes
- **Single connection** — one request at a time (no concurrent requests)
- **No auth** — API key is not checked
- **SmolLM2-135M** — current model is 135M params (basic chat only). Larger models coming soon.

## Files

| File | What |
|------|------|
| `efi/http_server.c` | HTTP server, request parsing, routing |
| `efi/api.c` | `/v1/models` and `/v1/chat/completions` handlers |
| `efi/net.c` | TCP4 listen/accept |
| `docs/architecture/http-server.md` | Server architecture details |
