# BootAI SSH + Hot Reload — Spec

> The *why* and every design decision. The *how* is `bootai_ssh_plan.md`.
> Status: **SPEC, not started** (David, 2026-10-28).

## 1. Goal

Remote development on a real BootAI machine without USB round trips ("sneaker net"):

- **SSH into BootAI** from the Mac (David or an agent), with key-only login.
- **Run BootAI commands** over that session and see their output.
- **Push a new `BOOTX64.EFI` and hot-reload it** in place: no reboot, no USB stick.
- **See the effect immediately:** build on the Mac → push → reload → read the log, in one command.

**Non-goals (v1):** a Unix shell, multiple users, port forwarding, SFTP, password login, internet exposure. BootAI is a UEFI app with no processes, so the "shell" is BootAI's own command dispatcher.

## 2. What BootAI already has (reuse; don't rebuild)

| Piece | Where | State |
|---|---|---|
| Own TCP/IP stack | `efi/ns_tcp.c`, `ns_ip.c`, `ns_arp.c`, `ns_dhcp.c`, `ns_udp.c`, `ns_net.h` | Server-side TCP: listen, accept, send, recv, close. **One connection at a time, no window scaling, no out-of-order reassembly, no congestion control** (LAN only). |
| NIC drivers | `e1000e/` (Intel wired), `iwn/` + `efi/wifi.c` (Intel Wi-Fi) | Working on the Dell. |
| mDNS | `efi/mdns.c` | Advertises `bootai.local`. |
| HTTP server + management API | `efi/http_server.c`, `efi/mgmt_api.c` | `POST /api/cmd` (runs a REPL command, returns captured output), `GET /api/log?offset=N`, `GET /api/health`. Bearer token from `wifi.conf`. `DEBUG_REMOTE=1` bypasses auth. **Plain HTTP, no encryption.** |
| Command dispatcher | `efi/main.c` | REPL commands (`netdiag`, `meminfo`, `e1000e poll`, `install`, …). Interactive-only ones are blocked from the API. |
| Image loading | `efi/drivers.c` | Already uses `gBS->LoadImage` / `StartImage` to load driver images from files. Hot reload reuses this. |
| File I/O | `efi/fs.c` | Read and write on the boot volume. |
| Log tee | `efi/console.c` | All output is teed to `bootai.log` and a ring buffer. |

So **SSH = a crypto + protocol layer on top of `ns_tcp`, feeding the existing command dispatcher and log.** Hot reload = an upload plus `LoadImage`/`StartImage` of the new image.

## 3. Why SSH, not TLS or the existing HTTP API

- **TLS** (e.g. for HTTPS) needs certificate chains, a root store, RSA/ECDSA verification and algorithm negotiation with servers we don't control. Large and security-sensitive.
- **SSH between our own two ends** lets us pin **exactly one algorithm per role**. There are no certificates, the host key is trust-on-first-use, and the client key is pinned in `authorized_keys`. The Mac uses stock OpenSSH.
- **The existing HTTP management API** is plain text on the LAN with a bearer token. Fine for diagnostics, but uploading and running code over it would be remote code execution in the clear. It stays as is; it is not the dev channel.

## 4. Protocol subset (SSH-2, RFC 4253/4252/4254)

**One algorithm per role**, advertised alone. A client offering none of them is disconnected.

| Role | Algorithm | Notes |
|---|---|---|
| Key exchange | `curve25519-sha256` (RFC 8731) | X25519 plus a SHA-256 exchange hash |
| Host key | `ssh-ed25519` | Generated on first boot and stored on the boot volume |
| Cipher + MAC | `chacha20-poly1305@openssh.com` | Two ChaCha20 keys (length + payload) plus Poly1305; no separate MAC |
| Compression | `none` | |
| User auth | `publickey` with `ssh-ed25519` only | Keys in `/bootai/authorized_keys`; no password method offered |
| Channel | one `session` | `exec` requests (a command, its output, its exit status); optionally `shell` = a line-based REPL over the channel |

**Re-keying:** honour a client's KEXINIT re-key (OpenSSH re-keys after ~1 GB). v1 may disconnect instead; then the push size limit is documented.

**Limits:** max packet 32 KB, window 2 MB, one session per connection, one connection at a time (matches `ns_tcp`).

## 5. Crypto source

- **Monocypher** (already vendored in Pantheon, `NOTICE` credits it) provides X25519, ChaCha20 (`crypto_chacha20_djb`), Poly1305 and Ed25519 (the optional `monocypher-ed25519.c`, with SHA-512).
- **SHA-256** is needed for the exchange hash and key derivation. Write a small one, or use one from foundry if present, with the FIPS 180-4 test vectors.
- **Randomness:** UEFI `EFI_RNG_PROTOCOL` if the firmware has it; otherwise RDRAND (checked via CPUID) mixed with TSC jitter. **Refuse to generate a host key or start KEX without a working entropy source.** Never fall back silently.
- **Licence:** Monocypher is CC0 / BSD-2, and BootAI is AGPL-3.0 + commercial. **Vendoring it and its NOTICE text is David's call** (licence rule). Flag it at plan Phase 0.
- No other crypto. No OpenSSL, no libssh. Pure C, `ba_` prefix, BootAI conventions.

## 6. Commands over SSH

- `ssh bootai <cmd>` runs `<cmd>` through the same dispatcher as `/api/cmd` and returns its captured output and an exit status (0 ok, non-zero error).
- **Interactive commands** (the ones `/api/cmd` blocks) stay blocked.
- **New dev commands:**
  - `log [offset]`: read the log ring (the same source as `/api/log`).
  - `push <size> <sha256>`: receive a new EFI image on stdin into `/bootai/stage/BOOTX64.EFI.new`, check its size and SHA-256, then reply ok or mismatch.
  - `reload`: hot-reload the staged image (§7).
  - `version`: build commit, build time, the image's SHA-256.

## 7. Hot reload

**Flow:**
1. `push` stages the new image and checks its hash.
2. `reload` calls `LoadImage(buffer)`.
   - **On failure:** report the error and keep running the old image.
   - **On success:** the old image writes a short handoff record (reason, previous image hash, log offset) to `/bootai/stage/handoff`, shuts down its network and NIC cleanly, and calls `StartImage` on the new one.
3. **The new image starts normally** (network, mDNS, SSH), reads the handoff and logs "reloaded from <old hash>".
4. **Return paths:**
   - **A new image that crashes or hangs** drops you back to the firmware (or a reboot). The USB still holds the last *installed* `BOOTX64.EFI`, so a reboot always gets back to a known-good build.
   - **`reload --install`** also copies the staged image over `BOOTX64.EFI`, but only once it has reported healthy over SSH (an explicit second command, never automatic).
5. **Teardown order matters:** the old image must stop the NIC (e1000e / Wi-Fi DMA) before `StartImage`, or the new image starts with a live device scribbling into old buffers. This is a test item.

**The dev loop** (a Mac script, `tools/dev_push.sh`): `make efi` → `ssh bootai push … < build/BOOTX64.EFI` → `ssh bootai reload` → wait for SSH to come back → `ssh bootai log <offset>`. An agent uses the same script.

## 8. Security

- **Off by default.** Enabled by `ssh=1` in `wifi.conf`, and shown in the boot menu with a **kill switch**: a menu item that turns SSH off and persists that.
- **Key-only login:** `authorized_keys` holds David's and the agent's Ed25519 keys. No passwords, no root. A bad key closes the connection; repeated failures are rate-limited per IP.
- **LAN only:** v1 refuses connections from non-RFC1918 source addresses. No exposure outside the LAN.
- **Host key** lives on the boot volume (`/bootai/ssh_host_ed25519_key`, never logged). The Mac pins it in `known_hosts` on first connect.
- `push`/`reload` is remote code execution **by design**, and only an authenticated key can do it.
- `DEBUG_REMOTE=1` doesn't affect SSH auth.

## 9. Constraints

- UEFI boot services: single-threaded and polled. The SSH state machine is driven from the same poll loop as HTTP and mDNS. It must never block the REPL for long; compute heavy steps (X25519, Ed25519) take milliseconds.
- `ns_tcp` handles one connection at a time with no out-of-order reassembly: fine on a LAN, and pushes stay a few MB.
- Memory: fixed buffers, with no unbounded allocation from network input. Every length field is checked before use.

## 10. Testing

- **Crypto:** known-answer tests for SHA-256 (FIPS vectors), X25519 (RFC 7748), Ed25519 (RFC 8032), ChaCha20-Poly1305@openssh (vectors from OpenSSH's own tests), and the exchange-hash derivation.
- **Protocol on the host:** build the SSH core as a host (Mac/Linux) test binary over a socket and drive it with **stock OpenSSH** (`ssh -vvv`, with only our algorithms). Covers the full handshake, `exec`, a bad key, a wrong algorithm and a truncated packet. Red first: a corrupted MAC must be rejected.
- **QEMU:** `make run` with user-mode networking and port forwarding. Covers SSH in, `exec`, `push` + `reload`, the new image coming back with the handoff logged, and SSH reconnecting.
- **Dell (with David):** wired e1000e first, then Wi-Fi. Run the dev loop end to end; do a reload during an active network poll; check the kill switch.
- **Fuzz:** the packet parser fed random and truncated input (host test), with no crashes and no out-of-bounds access.

## 11. Open (need David)

1. **Monocypher licence:** vendoring CC0/BSD-2 code into AGPL + commercial BootAI. His call.
2. **Agent access:** whether an agent's key goes in `authorized_keys` by default, or only while David enables it.
3. **The HTTP management API:** keep it as is, or require SSH for `/api/cmd` once SSH exists.
