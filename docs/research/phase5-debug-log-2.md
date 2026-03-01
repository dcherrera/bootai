# Phase 5c: Custom Network Stack — Debug Log (Part 2)

Continuation of [phase5-debug-log.md](phase5-debug-log.md) (bugs 1-39, rounds 1-32, lessons 1-30).

## Hardware

- **Machine**: Dell E6510
- **WiFi card**: Intel Centrino Advanced-N 6200 (PCI `8086:422C`)
- **Firmware**: Dell BIOS A17 (last official release)

## Where We Left Off (End of Part 1)

### Test Matrix — All 4 Combinations FAIL

| Round | FCS -4 | QoS AAD | Result |
|-------|--------|---------|--------|
| 24-28 | yes | no | ALL MIC FAIL |
| 29-30 | no | no | ALL MIC FAIL |
| 31 | no | yes | ALL MIC FAIL |
| 32 | yes | yes | ALL MIC FAIL |

**Conclusion**: Neither FCS subtraction nor QoS AAD reconstruction (nor both together) fixes MIC verification. There is a third unknown variable.

### What Works

- IWN WiFi driver: PCI init, firmware load, scan, auth, association
- EAPOL 4-way handshake: PTK + GTK derived, PTK installed in HW for TX
- TX encryption: HW CCMP via firmware (M4 ACK'd, DHCP DISCOVER sent)
- NODECRYPT: firmware delivers raw encrypted frames (32+ frames observed)
- AES-CTR decryption: LLC/SNAP headers visible in decrypted plaintext (key, nonce, CTR all correct)
- Self-test: FIPS 197 AES-128 PASS, RFC 3610 CCM PASS, synthetic 802.11 frame PASS
- Custom network stack: ARP, DHCP, IPv4, UDP, TCP on raw SNP frames
- Firmware restart: `iwn_fw_restart()` recovers from firmware death (INT=0x00000000)

### What's Broken

- **MIC verification**: CBC-MAC computed MIC never matches received MIC on real frames
- All 4 combinations of {FCS, QoS AAD} tested and failed
- Third variable hypotheses: phantom QoS length in `mpdu->len`, QoS TC ACK policy byte, other frame boundary issue

## Key Files

| File | Purpose |
|------|---------|
| `iwn/iwn_ccmp.c` | Software CCMP decrypt (AES-CCM) + self-tests + brute-force probe |
| `iwn/iwn_data.c` | RX/TX data path — calls `iwn_ccmp_decrypt()` on Protected frames |
| `iwn/iwn_eapol.c` | EAPOL 4-way handshake — derives PTK/GTK, installs PTK in HW |
| `iwn/iwn_assoc.c` | Association — RXON with NODECRYPT filter |
| `iwn/iwn_crypto.c` | AES-128 encrypt/decrypt primitives |
| `iwn/iwn_fw.c` | Firmware load + `iwn_fw_restart()` |
| `efi/ns_*.c` | Custom network stack (ARP, DHCP, IPv4, UDP, TCP) |

## Software CCMP Architecture

```
iwn_rx_poll()
  → ccmp_self_test()          # runs once per boot
  → for each IWN_MPDU_RX_DONE:
      frame = mpdu data
      flen = mpdu->len         # raw, no subtraction
      if Protected bit set:
          select key (PTK TK or GTK based on group bit)
          iwn_ccmp_decrypt(frame, flen, key)
            → FIRST FRAME: brute-force probe (8 combinations)
            → SUBSEQUENT: apply discovered adjustment
            → extract PN from CCMP header
            → build nonce: [priority=0] [addr2] [PN]
            → build QoS AAD: [fc0=0x88] [fc1 masked] [addrs] [SC masked] [QoS TC=0]
            → AES-CTR decrypt ciphertext in-place (counters 1,2,3...)
            → CBC-MAC over B0 + AAD + plaintext
            → encrypt counter 0 → S0, compute T XOR S0
            → compare with received MIC (8 bytes)
          if MIC OK: strip CCMP header + MIC, deliver plaintext
          if MIC FAIL: drop frame
```

## Bug Counter

**Total: 40 bugs/findings** (39 from Part 1 + 1 new)

## Debug Timeline (continued from Part 1)

### Round 32 — Both FCS + QoS AAD Combined

**Build**: `flen -= 4` (FCS) + QoS AAD (fc0=0x88, aad_len=24, QoS TC=0) + updated self-test to encrypt with QoS AAD.

**Results**: Self-test ALL PASS (AES-128, CCM CTR, CCM MIC, 802.11 frame). EAPOL succeeded first try (M4 status=0x01, ack_fail=0). **data=7**, ALL MIC FAIL.

**Key data from logs**:
- All frames broadcast (mDNS, IPv6 multicast)
- GTK kid=2, key `e77f573f...`
- CTR produces correct LLC/SNAP (`dec=aaaa030000000800` IPv4, `dec=aaaa0300000086dd` IPv6)
- MIC completely different between computed and received — no partial match
- `aes0=db286cd5` constant for all frames (key fingerprint, not per-frame — it's AES(key, zero_block))
- Example: len=832, ct=788, `comp=1dca7d818ab71fb7 rx=2771478dedb12315 FAIL`

**Analysis**: All 4 combinations of {FCS, QoS AAD} now exhausted, all FAIL. Since CTR is correct, the key and nonce are right. The remaining variables are ctext_len (frame boundary) and AAD content.

### 40. Brute-Force MIC Probe — Test 8 Combinations in One Round

**Approach**: Since blind guessing has failed (4 rounds wasted testing individual hypotheses), added a brute-force probe to `iwn_ccmp_decrypt()` that tests 8 combinations on the first encrypted frame:

- **4 ctext_len offsets**: 0, -2, -4, -6 bytes (covers: no adjustment, phantom QoS, FCS, FCS+phantom QoS)
- **2 AAD types**: QoS (`Q`, fc0=0x88, 24 bytes) and non-QoS (`N`, fc0=0x08, 22 bytes)
- **Total**: 8 combinations tested in a single boot

**Implementation** (round 33 build):
1. Removed `flen -= 4` from `iwn_data.c` — pass raw `mpdu->len` to `iwn_ccmp_decrypt()`
2. In `iwn_ccmp_decrypt()`, on first call:
   - Save MIC at 4 candidate positions (BEFORE CTR corrupts them)
   - Print raw frame tail (last 16 bytes + 4 beyond) for manual analysis
   - CTR decrypt with max ctext_len
   - Run CBC-MAC for all 8 combinations
   - Print `MATCH!` or `fail` for each
   - Store winning adjustment in static `flen_adj`
   - Return -1 (sacrifice probe frame)
3. Subsequent frames: apply `flen_adj` to ctext_len, normal decrypt

**Output format**:
```
PROBE flen=832 tail=<hex bytes>
dec=aaaa030000000800
Q-0 ct=792: <computed> fail
Q-2 ct=790: <computed> fail
Q-4 ct=788: <computed> fail
Q-6 ct=786: <computed> MATCH!     ← if this is the answer
N-0 ct=792: <computed> fail
N-2 ct=790: <computed> fail
N-4 ct=788: <computed> fail
N-6 ct=786: <computed> fail
WINNER: adj=6                      ← or "NO MATCH in 8 combinations"
```

If a MATCH is found, subsequent frames automatically use that adjustment. If NO MATCH, the raw tail hex dump provides data for further analysis.

**Status**: Round 33 build flashed to USB, untested.

## Current Status (Round 33)

**2026-02-28 (Dell test round 32)**: Both FCS+QoS fixes. Self-test ALL PASS. EAPOL success. **data=7, ALL MIC FAIL**. Combined fix disproved — all 4 {FCS, QoS} combinations now exhausted.

**2026-03-01 (Dell test round 33 — build ready)**: Brute-force probe. Tests 8 combinations (4 offsets × 2 AAD types) on first frame. Raw flen passed (no caller subtraction). Prints which combination matches (if any) + raw frame tail for analysis.

## Key Lessons Learned (continued from Part 1)

### 31. Exhaustive testing beats hypothesis-driven debugging for unknown firmware behavior

Four consecutive rounds (29-32) each tested a single hypothesis about firmware frame format. All failed. With 3 variables (FCS, QoS AAD, phantom QoS length), there are 8+ combinations. Testing one per round wastes ~5 minutes each. A brute-force probe that tests all combinations in a single round finds the answer immediately. When debugging a black-box firmware, always prefer exhaustive probing over sequential hypothesis testing.

### 32. Self-test consistency ≠ correctness against external implementations

The synthetic 802.11 self-test passes because both encryption and decryption use the SAME AAD/nonce/frame construction. Any systematic error (wrong AAD masking, wrong QoS TC byte, wrong ctext_len) cancels out — the test proves internal consistency but NOT compatibility with the AP's CCMP implementation. Real-frame testing is the only way to validate correctness against external implementations.

## Files Modified (Part 2)

| File | Changes |
|------|---------|
| `iwn/iwn_data.c` | Removed `flen -= 4` — raw `mpdu->len` passed to `iwn_ccmp_decrypt()`. Probe determines correct adjustment internally. |
| `iwn/iwn_ccmp.c` | Added brute-force MIC probe on first frame: saves MIC at 4 positions before CTR, tries 8 combinations (4 offsets × 2 AAD types), stores winning adjustment in static `flen_adj`. Subsequent frames apply discovered adjustment. |
