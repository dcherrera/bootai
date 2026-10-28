# Build Plan: BootAI SSH + Hot Reload

> **START HERE.** The *how*. The *why* is `bootai_ssh_spec.md` ("spec §N"); read §2, §4, §7 and §8 first.
> Rules: pure C, BootAI conventions (`ba_` prefix). No OpenSSL/libssh. Tests and scripts live in the repo.
> Every gate is shown red before it is trusted green. Commits carry no AI attribution. Agents push
> branches; David merges.

---

## Phase 0 — decisions + groundwork

- [ ] **0.1** David: Monocypher licence for BootAI (spec §11.1), and the agent-key policy (§11.2)
- [ ] **0.2** Entropy: probe `EFI_RNG_PROTOCOL` on the Dell and in QEMU; RDRAND fallback via CPUID; refuse without entropy (spec §5)
- [ ] **0.3** Host test harness: build the SSH core for macOS/Linux behind a socket shim with the same interface as `ns_tcp` (`tests/ssh/`)

## Phase 1 — crypto

- [ ] **1.1** Vendor Monocypher (+ `monocypher-ed25519`) under `efi/crypto/` with its NOTICE (after 0.1)
- [ ] **1.2** `ba_sha256.{c,h}` (FIPS 180-4)
- [ ] **1.3** `ba_chachapoly_ssh.{c,h}`: `chacha20-poly1305@openssh.com` (length key + payload key, Poly1305 over the encrypted length and payload)
- [ ] **1.4** Test: known-answer vectors for SHA-256, X25519, Ed25519 and chachapoly-openssh; a corrupted tag must be rejected (red)

## Phase 2 — SSH transport + auth (host-tested against OpenSSH)

- [ ] **2.1** `ba_ssh_transport.{c,h}`: version exchange, binary packet layer, KEXINIT (one algorithm per role, spec §4), `curve25519-sha256` key exchange, NEWKEYS, key derivation
- [ ] **2.2** Host key: generate on first boot (needs 0.2), store at `/bootai/ssh_host_ed25519_key`, sign the exchange hash
- [ ] **2.3** `ba_ssh_auth.{c,h}`: `publickey` with ssh-ed25519 against `/bootai/authorized_keys`; no other method; failure rate limit
- [ ] **2.4** Test: stock `ssh -vvv` against the host binary completes auth; a wrong key, a wrong algorithm and truncated/corrupt packets are rejected; the packet parser is fuzzed

## Phase 3 — channels + commands

- [ ] **3.1** `ba_ssh_channel.{c,h}`: one `session` channel, `exec` (and optionally `shell`), window and packet limits, exit-status, EOF/close
- [ ] **3.2** Route `exec` to the existing command dispatcher (the same path as `/api/cmd`); interactive commands stay blocked
- [ ] **3.3** Dev commands: `log [offset]`, `version`
- [ ] **3.4** Test: `ssh host 'meminfo'` returns the same output as `/api/cmd`; a blocked command is refused with a non-zero exit

## Phase 4 — into BootAI (QEMU)

- [ ] **4.1** Wire the SSH server into the poll loop beside HTTP and mDNS; port 22; off unless `ssh=1` in `wifi.conf`
- [ ] **4.2** Boot-menu kill switch (turns SSH off and persists it); LAN-only source check (spec §8)
- [ ] **4.3** Test: `make run` in QEMU with port forwarding; `ssh` in, run commands, kill switch, non-LAN refusal

## Phase 5 — push + hot reload

- [ ] **5.1** `push <size> <sha256>`: stream the channel data into `/bootai/stage/BOOTX64.EFI.new`, then check the size and hash
- [ ] **5.2** `reload`: `LoadImage` from the staged buffer (reuse `drivers.c`); on success write the handoff record, stop the NIC and network cleanly, `StartImage`; on failure keep running the old image
- [ ] **5.3** The new image reads the handoff and logs "reloaded from <hash>"; `reload --install` copies over `BOOTX64.EFI`, only on an explicit command
- [ ] **5.4** `tools/dev_push.sh`: make efi → push → reload → wait for SSH → `log <offset>`
- [ ] **5.5** Test (QEMU): ten push/reload cycles; a deliberately bad image (fails `LoadImage`) leaves the old one running; a crashing image is recovered by a reboot to the installed build

## Phase 6 — real hardware (with David)

- [ ] **6.1** Dell, wired e1000e: the dev loop end to end; reload during network traffic; NIC teardown verified (no stale DMA)
- [ ] **6.2** Dell, Wi-Fi (iwn): the same
- [ ] **6.3** Measure: handshake time, push throughput, reload-to-SSH-back time

## Phase 7 — docs + cleanup

- [ ] **Document:** README "Remote development (SSH)"; `CLAUDE.md` dev-loop section; spec status → BUILT
- [ ] Delete this plan and spec once built (repo rule)
