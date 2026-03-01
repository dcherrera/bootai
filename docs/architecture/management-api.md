# Remote Management API

BootAI includes a remote management API for remote command execution and debugging over Ethernet. Instead of the manual flash-boot-read-log cycle (~5 min/round), you can execute REPL commands and stream logs in real time from your Mac.

**Status:** Verified working on Dell E6510 over e1000e wired Ethernet (2026-03-01).

## Quick Start

### Production (with auth)

```bash
# 1. Build
make clean && make efi

# 2. Create wifi.conf on USB root with your token:
#    api_token=bootai

# 3. Flash USB, boot Dell, enter REPL, start server:
> serve 8080

# 4. From Mac:
curl -H "Authorization: Bearer bootai" http://192.168.0.101:8080/api/health
curl -H "Authorization: Bearer bootai" -d '{"cmd":"meminfo"}' http://192.168.0.101:8080/api/cmd
curl -H "Authorization: Bearer bootai" http://192.168.0.101:8080/api/log
```

### Debug (no auth)

```bash
make clean && make DEBUG_REMOTE=1 efi

# No Authorization header needed:
curl http://<dell-ip>:8080/api/health
```

### QEMU (port-forwarded)

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
| `POST` | `/api/update` | Stream a new EFI binary to disk (OTA firmware update) |

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

All other commands work: `ls`, `cat`, `iwn scan`, `iwn pass`, `iwn connect`, `iwn ping`, `wifi scan`, `wifi save`, `wifi disconnect`, `netinfo`, `netdiag`, `tensor`, `fetch`, `reboot`.

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

### POST /api/update

Stream a new EFI binary to `\EFI\BOOT\BOOTX64.EFI` on the USB drive. This is the OTA firmware update endpoint — it eliminates the physical USB swap cycle.

The binary is streamed directly from TCP to disk in 4KB chunks. The request body can be any size up to 4MB, bypassing the 8KB HTTP request buffer limit.

**Request:**

```bash
curl -H "Authorization: Bearer bootai" \
     --data-binary @all_bootai.efi \
     http://192.168.0.101:8080/api/update
```

**Response (success):**

```json
{"ok": true, "message": "firmware updated"}
```

**Response (errors):**

| Status | Body | Cause |
|--------|------|-------|
| 403 | `{"ok":false,"error":"unauthorized"}` | Missing or invalid Bearer token |
| 400 | `{"ok":false,"error":"invalid content-length"}` | Missing Content-Length or > 4MB |
| 500 | `{"ok":false,"error":"filesystem not ready"}` | `efi_fs_init()` not called or failed |
| 500 | `{"ok":false,"error":"file create fail"}` | Could not create file on disk |
| 500 | `{"ok":false,"error":"incomplete transfer"}` | TCP connection dropped before all bytes received |

**How it works:**

1. HTTP server parses headers, detects `/api/update` path
2. Hands off the TCP connection to `mgmt_handle_update()` before reading the full body
3. Handler authenticates, deletes old `\EFI\BOOT\BOOTX64.EFI`, creates new file
4. Writes any body bytes already in the request buffer (typically ~7KB from the first TCP recv)
5. Loops `tcp4_recv()` in 4KB chunks, writing each directly to the file
6. Flushes and closes the file, sends JSON response

### Reboot Command

The `reboot` command resets the system via UEFI `RuntimeServices->ResetSystem(EfiResetWarm)`. Available from both the REPL and the API.

**REPL:**

```
> reboot
  Rebooting...
```

Calls `efi_reboot()` directly — immediate warm reset.

**API:**

```bash
curl -H "Authorization: Bearer bootai" \
     -d '{"cmd":"reboot"}' \
     http://192.168.0.101:8080/api/cmd
```

```json
{"ok": true, "output": "  Rebooting...\r\n"}
```

Sets `g_reboot_pending = 1` and returns. The HTTP server sends the response, closes the TCP connection, then calls `efi_reboot()`. This ensures the client receives its response before the machine resets.

### OTA Update Workflow

Build, push, and reboot — all from the Mac terminal:

```bash
# 1. Build new firmware
cd bootai && make clean && make efi

# 2. Push to Dell over network
curl -H "Authorization: Bearer bootai" \
     --data-binary @all_bootai.efi \
     http://192.168.0.101:8080/api/update

# 3. Reboot into new firmware
curl -H "Authorization: Bearer bootai" \
     -d '{"cmd":"reboot"}' \
     http://192.168.0.101:8080/api/cmd

# 4. Wait for Dell to boot, start server on Dell keyboard:
#    > serve 8080

# 5. Verify new firmware is running
curl -H "Authorization: Bearer bootai" \
     http://192.168.0.101:8080/api/health
```

This replaces the manual cycle of: eject USB, mount on Mac, copy EFI, eject, plug into Dell, boot.

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

## Dell E6510 Verification (2026-03-01)

Tested over e1000e wired Ethernet (Intel 82577LM) with production auth:

```
$ curl -s -H "Authorization: Bearer bootai" http://192.168.0.101:8080/api/health
{"status":"ok","wifi":"none","network":"ready","ip":"192.168.0.101","log_pos":2124}

$ curl -s -H "Authorization: Bearer bootai" -d '{"cmd":"meminfo"}' http://192.168.0.101:8080/api/cmd
{"ok":true,"output":"\r\n  Installed RAM:  8157 MB\r\n  ..."}
```

### Bugs Fixed During Bring-Up

1. **api_token not loaded on e1000e-only systems** — `parse_wifi_conf()` was only called from `efi_wifi_auto_connect()`, which early-returned if no UEFI WiFi2 adapter was found (`!g_wifi2`). On Dell E6510 (no WiFi2 protocol — IWN is a custom driver), the token was never parsed. Fix: read and parse `wifi.conf` before the WiFi adapter check, so `api_token` is always loaded.

2. **Bearer token comparison failed on non-null-terminated header** — `mgmt_auth_check()` used `mgmt_streq()` (null-terminated compare) on the `auth_header` pointer, which points into the raw HTTP buffer and is NOT null-terminated (bounded by `auth_header_len`). The header value `"bootai\r\n..."` never matched `"bootai\0"`. Fix: switched to length-bounded `mgmt_strncmp()` with exact length check.

## Limitations

- **128KB log buffer** — if the log fills up, older output is lost (not a ring buffer, just drops new writes)
- **32KB response limit** — command output longer than ~32KB will be truncated in the JSON response
- **Single-threaded** — commands execute synchronously; the server is blocked during execution
- **No websocket** — log streaming requires polling, not push
- **500-char command limit** — commands longer than 500 characters are rejected
