# Remote Management API

BootAI includes a remote management API for debugging over Ethernet. Instead of the manual flash-boot-read-log cycle (~5 min/round), you can execute REPL commands and stream logs in real time from your Mac.

## Quick Start

```bash
# Build with debug mode (no auth required)
make clean && make DEBUG_REMOTE=1 efi

# Flash USB, boot Dell, enter REPL, start server:
> serve 8080

# From Mac:
curl http://<dell-ip>:8080/api/health
curl -X POST http://<dell-ip>:8080/api/cmd -d '{"cmd":"iwn scan"}'
curl http://<dell-ip>:8080/api/log
```

In QEMU (port-forwarded):

```bash
curl http://localhost:8080/api/health
curl -X POST http://localhost:8080/api/cmd -d '{"cmd":"ls"}'
curl "http://localhost:8080/api/log?offset=0"
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/cmd` | Execute a REPL command, return captured output |
| `GET` | `/api/log` | Read the log buffer (supports `?offset=N` for streaming) |
| `GET` | `/api/health` | Extended status: network, WiFi, IP, log position |

### POST /api/cmd

Execute any non-interactive REPL command remotely. The command's console output is captured from the log buffer and returned as JSON.

**Request:**

```json
{"cmd": "iwn scan"}
```

**Response (success):**

```json
{"ok": true, "output": "  Scanning...\r\n  1. Herrera (WPA2), -45 dBm\r\n"}
```

**Response (blocked command):**

```json
{"ok": false, "error": "command blocked (interactive)"}
```

Interactive commands that require keyboard input are blocked from the API to prevent deadlocks:

| Command | Why blocked |
|---------|-------------|
| `chat` / `infer` | Reads user input in a loop |
| `serve` | Already running (the API is served from it) |
| `wifi connect` | Prompts for password + save confirmation |
| `mdns` | Waits for keypress to stop |

The `exit` command is safe to call — it returns success but does NOT actually exit the REPL (only the local keyboard can do that).

All other commands work: `ls`, `cat`, `iwn scan`, `iwn pass`, `iwn connect`, `iwn ping`, `wifi scan`, `wifi save`, `wifi disconnect`, `netinfo`, `netdiag`, `tensor`, `fetch`.

### GET /api/log

Read the console log buffer. Supports offset-based streaming — the client remembers the last `pos` and polls with `?offset=pos` to get only new output.

**Request:**

```
GET /api/log              # Full log buffer
GET /api/log?offset=5000  # Only new output since position 5000
```

**Response:**

```json
{"pos": 12345, "data": "...log content..."}
```

| Field | Type | Description |
|-------|------|-------------|
| `pos` | int | Current write position in the log buffer |
| `data` | string | Log content from offset to pos (JSON-escaped) |

**Streaming pattern:**

```bash
# Initial read
POS=0
while true; do
  RESP=$(curl -s "http://<ip>:8080/api/log?offset=$POS")
  DATA=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['data'])")
  NEW_POS=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['pos'])")
  if [ "$NEW_POS" != "$POS" ]; then
    echo "$DATA"
    POS=$NEW_POS
  fi
  sleep 1
done
```

### GET /api/health

Extended health check with network diagnostics.

**Response:**

```json
{
  "status": "ok",
  "wifi": "available",
  "network": "ready",
  "ip": "192.168.1.100",
  "log_pos": 12345
}
```

| Field | Type | Description |
|-------|------|-------------|
| `status` | string | Always `"ok"` |
| `wifi` | string | `"available"` or `"none"` |
| `network` | string | `"ready"` or `"unavailable"` |
| `ip` | string | Local IP address (omitted if unavailable) |
| `log_pos` | int | Current log buffer position (for streaming) |

## Authentication

### Debug Mode (`DEBUG_REMOTE=1`)

All authentication is bypassed. Any request to `/api/` endpoints is accepted. Use this for local Ethernet debugging only.

```bash
make clean && make DEBUG_REMOTE=1 efi
```

### Production Mode (default)

Requires a Bearer token configured in `wifi.conf`:

```ini
# wifi.conf on USB root
api_token=my-secret-token-here

[network]
ssid=Herrera
password=mypassword
```

Requests must include the `Authorization` header:

```bash
curl -H "Authorization: Bearer my-secret-token-here" \
     http://<ip>:8080/api/health
```

Without a valid token, all `/api/` endpoints return 403:

```json
{"ok": false, "error": "unauthorized"}
```

If no `api_token=` line exists in `wifi.conf`, all `/api/` requests are locked out (403) in production mode.

## CCMP Debugging Workflow

The primary use case: debugging WiFi CCMP MIC verification on the Dell over wired Ethernet.

```bash
# 1. Boot Dell with DEBUG_REMOTE=1 build, start server
#    (Dell gets IP via DHCP on wired Ethernet)

# 2. From Mac, set WiFi password
curl -X POST http://<dell-ip>:8080/api/cmd \
  -d '{"cmd":"iwn pass mypassword"}'

# 3. Connect WiFi
curl -X POST http://<dell-ip>:8080/api/cmd \
  -d '{"cmd":"iwn connect Herrera"}'

# 4. Read full log (includes CCMP probe results, crypto debug output)
curl http://<dell-ip>:8080/api/log

# 5. Run more commands, read incremental log
curl -X POST http://<dell-ip>:8080/api/cmd \
  -d '{"cmd":"iwn ping"}'
curl "http://<dell-ip>:8080/api/log?offset=8000"
```

This replaces the manual cycle of: flash USB, boot Dell, type commands on Dell keyboard, power off, mount USB on Mac, read `bootai.log`.

## Architecture

### Files

| File | Purpose |
|------|---------|
| `efi/mgmt_api.c` | Management API handlers, auth check, JSON building |
| `efi/http_server.c` | HTTP parsing (auth header, query string), `/api/` routing |
| `efi/main.c` | `ba_exec_cmd()` — extracted command dispatch, `g_api_context` flag |
| `efi/console.c` | `efi_log_pos()`, `efi_log_buf()` — log buffer accessors |
| `efi/wifi.c` | `api_token=` parsing from wifi.conf, `efi_get_api_token()` |
| `efi/efi.h` | Declarations for all new public functions |

### How /api/cmd Works

```
POST /api/cmd {"cmd":"iwn scan"}
        │
  mgmt_auth_check() ── verify Bearer token (or skip if DEBUG_REMOTE)
        │
  pos_before = efi_log_pos()
        │
  g_api_context = 1   ── blocks interactive commands
        │
  ba_exec_cmd("iwn scan")  ── runs the command, output goes to log buffer
        │
  g_api_context = 0
        │
  pos_after = efi_log_pos()
        │
  output = efi_log_buf()[pos_before..pos_after]  ── captured output
        │
  JSON response: {"ok":true,"output":"...escaped output..."}
```

### Design Decisions

**Log buffer capture** — Commands print to `efi_print_ascii()`, which tees to the 128KB log buffer. The management API records the buffer position before/after execution to extract just that command's output. No separate output capture mechanism needed.

**Blocked commands** — Interactive commands that read keyboard input (`efi_read_line`) would hang forever when called from the API. The `g_api_context` flag lets `ba_exec_cmd()` return -2 instead of entering the interactive path.

**Shared response buffer** — `mgmt_api.c` uses a single 32KB static buffer (`cmd_output_buf`) for building JSON responses. This is safe because the HTTP server is single-threaded.

**Two-layer auth** — Debug mode (`DEBUG_REMOTE=1`) is a compile-time flag that removes all auth code. This avoids accidentally shipping an open API, while making local debugging frictionless.

## Limitations

- **128KB log buffer** — if the log fills up, older output is lost (not a ring buffer, just drops new writes)
- **32KB response limit** — command output longer than ~32KB will be truncated in the JSON response
- **Single-threaded** — commands execute synchronously; the server is blocked during execution
- **No websocket** — log streaming requires polling, not push
- **500-char command limit** — commands longer than 500 characters are rejected
