# Phase 5c: Custom Network Stack — Debug Log

## Hardware

- **Machine**: Dell E6510
- **WiFi card**: Intel Centrino Advanced-N 6200 (PCI `8086:422C`)
- **Firmware**: Dell BIOS A17 (last official release)
- **UEFI IP stack**: Absent — Dell firmware does not include MnpDxe, ArpDxe, Ip4Dxe, TcpDxe, etc.

## Starting Point

Phase 5b complete: IWN WiFi driver connects to WPA2 network (auth, association, EAPOL 4-way handshake), SNP protocol installed. Data frame TX/RX works (`iwn ping` gets ARP replies). But there is no IP stack — the Dell firmware has no UEFI network DXE drivers beyond SNP.

**Goal**: Build ARP, DHCP, IPv4, UDP, and TCP directly on raw Ethernet frames via SNP, enabling `serve 8080` on Dell hardware.

## Debug Timeline

### 1. UEFI IP Stack DXE Drivers Fail to Load

**Symptom**: After `iwn connect` installs SNP, `efi_net_init()` tries to locate `IP4Config2` protocol — not found. Falls back to `LocateProtocol` — also not found. Dell firmware simply doesn't have these drivers.

**Investigation**: Attempted loading EDK2 DXE drivers from USB (`drivers/` directory). `LoadImage` + `StartImage` succeeds but `ConnectController` hangs indefinitely. The drivers have DEPEX (dependency expression) chains that can't be satisfied on Dell firmware.

**Resolution**: Built a complete custom network stack (Phase 5c) — 7 new files (~1,500 lines) implementing ARP, DHCP, IPv4, UDP, and TCP on raw SNP frames. Dual dispatch in `net.c` — UEFI path (QEMU) vs custom path (Dell) selected automatically.

### 2. Custom Stack Init Hangs — No Output After "using custom network stack"

**Symptom**: After WiFi connects successfully (auth, assoc, EAPOL, SNP installed), the screen shows:

```
No UEFI IP stack  using custom network stack
```

Then hangs. No further output. No crash, no error — just frozen.

**Debug approach**: `bootai.log` was empty for this section because the log only flushes to disk at the REPL `> ` prompt. Since the hang occurs during `efi_net_init()` (before reaching the REPL), the log file on USB doesn't capture the debug output. Had to rely on screen photos.

**Investigation**: The code flow after the message is:

```
efi_net_init()
  → prints "No UEFI IP stack — using custom network stack"
  → ns_init(snp)          ← hangs somewhere in here
  → ns_dhcp_run()         ← never reached
```

### 3. SNP Start() Call Identified as Exact Hang Point

**Symptom**: Added step-by-step trace prints inside `ns_init()`:

```c
efi_print_ascii("ns_init: enter\r\n");
// ... memset, memcpy MAC ...
efi_print_ascii("ns_init: checking SNP state\r\n");
// ... state check ...
efi_print_ascii("ns_init: calling Start()\r\n");
snp->Start(snp);   // <-- HANGS HERE
efi_print_ascii("ns_init: Start done\r\n");  // never printed
```

**Screen output** (after 5 retries due to separate association timeout issue):

```
No UEFI IP stack  using custom network stack
ns_init: enter
ns_init: copying MAC
ns_init: checking SNP state
ns_init: calling Start()
```

Last line printed is "calling Start()". The `snp->Start(snp)` indirect call never returns.

**Root cause**: The target function `iwn_snp_start()` in `iwn_snp.c` is trivial — it checks the SNP state, sets `Started`, prints a message, and returns `EFI_SUCCESS`. The function itself cannot hang.

The issue is the **indirect function call through the SNP protocol table**. The SNP protocol struct was installed via `gBS->InstallProtocolInterface()` with function pointers like `snp->Start = iwn_snp_start`. Both the function pointer types (in `efi.h`) and the implementations (in `iwn_snp.c`) have the correct `EFIAPI` (`__attribute__((ms_abi))`) calling convention.

Despite correct calling convention annotations, the indirect call through the protocol function pointer table hangs on Dell E6510 firmware. This is likely a **PIC/relocation issue** — the PE32+ binary's function pointers in the protocol struct may not resolve correctly when called through the firmware's protocol dispatch mechanism. The same SNP protocol table works in QEMU/OVMF.

### 4. Fix: Bypass SNP Function Pointer Table Entirely

**Solution**: Since the IWN driver code is linked into the same binary, we can call the underlying data functions directly instead of going through the SNP protocol interface.

Added three direct data path functions to `iwn_snp.c`:

```c
/* Direct data path — bypasses SNP function pointer table */

void iwn_snp_get_mac(uint8_t *mac) {
    iwn_memcpy(mac, g_iwn.mac_addr, 6);
}

int iwn_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                      const uint8_t *payload, int len) {
    return iwn_encap_tx(&g_iwn, dst_mac, ethertype,
                        payload, (uint16_t)len);
}

int iwn_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len) {
    iwn_rx_poll(&g_iwn);
    uint16_t flen = 0;
    int ret = iwn_rx_dequeue(&g_iwn, buf, &flen);
    if (ret == 0 && frame_len) *frame_len = (int)flen;
    return ret;
}
```

Rewrote `ns_net.c` to use these direct functions:

| Operation | Before (hangs) | After (bypass) |
|-----------|----------------|----------------|
| Get MAC | `snp->Mode->CurrentAddress` | `iwn_snp_get_mac(mac)` |
| Init | `snp->Start(snp)` + `snp->Initialize(snp,0,0)` | Removed — not needed |
| Transmit | `snp->Transmit(snp, 0, len, buf, ...)` | `iwn_snp_direct_tx(dst, etype, payload, len)` |
| Receive | `snp->Receive(snp, NULL, &sz, buf, ...)` | `iwn_snp_direct_rx(buf, sz, &len)` |

**Key design change**: The old `ns_raw_send()` built a complete 14-byte Ethernet frame (dst MAC + src MAC + ethertype + payload) and passed it to `snp->Transmit()` with `HeaderSize=0`. The new version passes dst_mac, ethertype, and payload separately to `iwn_snp_direct_tx()`, which calls `iwn_encap_tx()` — this function handles Ethernet-to-802.11 encapsulation + LLC/SNAP + CCMP encryption internally.

**Result**: **CONFIRMED FIXED.** Dell E6510 now prints `ns: init MAC 00:27:10:56:AD:80` and proceeds to DHCP. The SNP bypass eliminates the hang entirely.

### 5. SNP Bypass Confirmed — DHCP DISCOVER Sent, No OFFER Received

**Symptom**: With the SNP bypass fix, `ns_init()` completes and DHCP starts. But DISCOVER gets no response:

```
ns: init MAC 00:27:10:56:A :80
DHCP: starting...
DHCP: DISCOVER
DHCP: no OFFER received (timeout)
Custom DHCP failed
```

Two new bugs visible in this output: the MAC display is corrupted (`A :80` instead of `AD:80`), and the DHCP DISCOVER was sent but the router never responded. Investigation revealed three root causes (bugs 7, 8, 9 below).

### 6. Association Timeout — Multiple Retries Required

**Symptom**: During testing, the `iwn connect Herrera` command sometimes times out at the association stage:

```
Assoc: no response (timeout)
```

The user needed 5 retries on one test session to get a successful connection.

**Analysis**: This is a separate issue from the SNP hang. The association timeout occurs in the IWN driver layer (Phase 4c), before the custom network stack is even involved. Possible causes:

- Dell E6510 firmware/BIOS interfering with WiFi timing
- Router not responding to association requests quickly enough
- Missing retry logic in `iwn_assoc.c` (currently single-attempt)

**Status**: Not yet addressed. Workaround is to retry `iwn connect` manually.

### 7. Em Dash UTF-8 Rendering

**Symptom**: The string `"No UEFI IP stack — using custom network stack"` renders on Dell screen as:

```
No UEFI IP stack  using custom network stack
```

The `—` (UTF-8 em dash, 3 bytes: `0xE2 0x80 0x94`) is printed as two spaces by `efi_print_ascii()`, which treats each byte individually.

**Impact**: Cosmetic only. Does not affect functionality.

### 8. MAC Address Display — Hex Lookup Out of Bounds

**Symptom**: MAC printed as `00:27:10:56:A :80` instead of `00:27:10:56:AD:80`. The 5th byte (`0xAD`) displays as `A ` — high nibble `0xA` is correct but low nibble `0xD` is garbage.

**Root cause**: The hex lookup string in `ns_net.c:ns_init()` was `"0123456789AB"` — only 12 characters (indices 0-11). Nibble values 0xC through 0xF (12-15) index past the string terminator, reading garbage memory.

```c
// BEFORE (broken — only 12 chars):
hex[0] = "0123456789AB"[b >> 4];
hex[1] = "0123456789AB"[b & 0xF];

// AFTER (correct — all 16 hex chars):
hex[0] = "0123456789ABCDEF"[b >> 4];
hex[1] = "0123456789ABCDEF"[b & 0xF];
```

**Fix**: Changed lookup string to `"0123456789ABCDEF"` (16 characters).

### 9. TX Ring Buffer Overflow — DHCP Frame Too Large for Inline

**Symptom**: DHCP DISCOVER sent but no OFFER received. No firmware error — the frame appeared to transmit but the router never responded.

**Root cause**: The TX ring command buffer (`struct iwn_tx_cmd`) is 140 bytes per entry. After the 4-byte header and 56-byte `iwn_cmd_data`, only **80 bytes** remain for inline frame data. The ARP ping frame (60 bytes) fits, but the DHCP frame is **310 bytes** (24 802.11 header + 8 LLC/SNAP + 278 IP+UDP+DHCP).

`iwn_tx_raw()` was using a single TX descriptor segment, copying the entire frame inline:

```c
/* BEFORE — copies 310 bytes into 80-byte space! */
iwn_memcpy(ring_buf + 4 + sizeof(struct iwn_cmd_data), frame, frame_len);

desc->nsegs = 1;
desc->segs[0].addr = IWN_LOADDR(buf_paddr);
desc->segs[0].len = (uint16_t)(IWN_HIADDR(buf_paddr) | ((uint16_t)totlen << 4));
```

This overflows by 230 bytes into adjacent ring entries. While the DMA memory is contiguous (so the firmware may read valid bytes), the corrupted adjacent entries and the scheduler byte count pointing at a single oversized segment likely cause the firmware to transmit a malformed or truncated frame that the router drops.

**Fix**: 2-segment TX descriptor, following OpenBSD's approach:

```c
if (frame_len <= inline_space) {
    /* Small frame: inline in ring (1 segment) */
    iwn_memcpy(ring_buf + hdr_len, frame, frame_len);
    desc->nsegs = 1;
    desc->segs[0].len = ...(hdr_len + frame_len)...;
} else {
    /* Large frame: 2 segments */
    iwn_memcpy(dev->tx_data.vaddr, frame, frame_len);
    desc->nsegs = 2;
    desc->segs[0].len = ...(hdr_len)...;     /* ring: header+cmd_data */
    desc->segs[1].addr = dev->tx_data.paddr;  /* DMA buf: frame */
    desc->segs[1].len = ...(frame_len)...;
}
```

Added a dedicated 4 KB DMA page (`dev->tx_data`) allocated alongside ring 0 for large frame payloads. Small frames (ARP, EAPOL, management) still use the fast inline path.

**Threshold**: Frames ≤ 80 bytes use inline (1 segment). Frames > 80 bytes use DMA buffer (2 segments).

### 10. IP Destination Filter Drops DHCP OFFER

**Symptom**: Even if the DHCP DISCOVER is transmitted correctly, the OFFER from the router may be silently dropped by our IP layer.

**Root cause**: `ns_ip_handle()` checks whether the incoming packet's destination IP matches our IP or is broadcast. During DHCP, our IP (`ns->ip`) is `{0,0,0,0}`. If the DHCP server sends the OFFER to the **offered IP** (e.g., `192.168.1.100`) instead of broadcast (`255.255.255.255`), both checks fail:

```c
int for_us = ip_eq(dst, ns->ip);     // ip_eq({192.168.1.100}, {0.0.0.0}) = false
int is_bcast = (dst == 255.255.255.255);  // false
// → packet dropped!
```

The RFC says DHCP servers should honor the broadcast flag (`flags = 0x8000`), but some routers/DHCP servers send unicast to the offered IP regardless.

**Fix**: When `ip_ready == 0` (during DHCP), accept ALL incoming IP packets regardless of destination address. Once DHCP completes and `ip_ready` is set, normal destination filtering resumes.

```c
if (ns->ip_ready) {
    /* Normal filtering: must be our IP, broadcast, or subnet broadcast */
    ...
    if (!for_us && !is_bcast && !subnet_bcast) return;
}
/* During DHCP (ip_ready=0): accept everything */
```

### 11. EAPOL Handshake Intermittent Failure — TX Ring Overflow in EAPOL/Assoc Paths

**Symptom**: Second Dell test after applying DHCP fixes (bugs 7-9). The WiFi connection failed before reaching DHCP — the EAPOL 4-way handshake was interrupted by the AP:

```
M2 sent
Waiting for M3...
EAPOL: M1 retransmission, ignoring
EAPOL: M1 retransmission, ignoring
EAPOL: AP sent deauth!
EAPOL: M3 timeout
```

The AP retransmitted M1 twice (indicating it didn't receive or rejected our M2), then deauthenticated.

**Investigation**: The same TX ring buffer overflow from bug 9 also affects `iwn_tx_eapol()` (iwn_eapol.c) and `iwn_tx_frame()` (iwn_assoc.c). Both functions **duplicated** the TX ring manipulation code with `desc->nsegs = 1` (inline only) instead of calling `iwn_tx_raw()`.

The EAPOL M2 frame is ~135 bytes:
- 24 bytes 802.11 data header
- 8 bytes LLC/SNAP
- 4 bytes EAPOL header
- 95+ bytes EAPOL-Key body (SNonce + RSN IE + MIC)

This overflows the 80-byte inline space by ~55 bytes. Unlike the 310-byte DHCP overflow which always failed, the 55-byte EAPOL overflow is smaller and often "works" because the DMA ring memory is contiguous — the firmware reads valid bytes from adjacent ring entries. But occasionally the overflow corrupts adjacent ring state enough to produce a malformed frame that the AP rejects, causing deauthentication.

The association request frame from `iwn_tx_frame()` is ~70 bytes (usually fits within 80), so it was less affected. But the code was still duplicated and vulnerable for larger association frames with longer SSIDs or additional IEs.

**Root cause**: Code duplication. Three separate functions manipulated the TX ring independently:

| Function | File | Frame Size | Overflow? |
|----------|------|-----------|-----------|
| `iwn_tx_raw()` | iwn_data.c | Variable | Fixed (2-segment) |
| `iwn_tx_eapol()` | iwn_eapol.c | ~135 bytes | **YES** (55 bytes over) |
| `iwn_tx_frame()` | iwn_assoc.c | ~30-80 bytes | Borderline |

**Fix**: Refactored both `iwn_tx_eapol()` and `iwn_tx_frame()` to call `iwn_tx_raw()` instead of duplicating the ring mechanics. Added `plcp` and `rflags` parameters to `iwn_tx_raw()` so callers can specify the TX rate (CCK 1 Mbps for management/EAPOL reliability, OFDM 24 Mbps for data frames).

```c
/* iwn_tx_eapol() — BEFORE: 58 lines of duplicated ring logic */
iwn_tx_ring *ring = &dev->txq[0];
int idx = ring->cur;
// ... 55 more lines of ring manipulation ...
desc->nsegs = 1;  /* <-- overflow for frames > 80 bytes */

/* iwn_tx_eapol() — AFTER: single call to canonical TX function */
uint8_t txant = (uint8_t)IWN_LSB(dev->txchainmask);
return iwn_tx_raw(dev, frame, frame_len,
                  IWN_ID_BSS, 0,
                  10, IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant));
```

Same refactoring applied to `iwn_tx_frame()`. Eliminated ~120 lines of duplicated code. All three TX paths (data, EAPOL, management) now flow through the single canonical `iwn_tx_raw()` with its 2-segment DMA handling.

### 12. CCMP IV Not Stripped From Decrypted RX Frames

**Symptom**: After EAPOL handshake completes, DHCP DISCOVER is sent but zero data frames are ever received — not even ARP replies. Added HW diagnostics (`iwn_snp_debug_state()`) showing:

```
IWN: tx_ok=0 hw=21 cur=12 sw=0    ← Before ARP probe
IWN: tx_ok=1 hw=4  cur=4  sw=0    ← After ARP probe (2s wait)
```

`tx_ok` increments (TX works), `hw` index advances (firmware is delivering frames to the HW RX ring), but `sw=0` (zero data frames make it to our software buffer). The firmware is alive and active — but every data frame is silently dropped by our filter.

**Root cause**: When the Intel firmware decrypts a CCMP-encrypted frame, it leaves the 8-byte CCMP header (IV + ExtIV) in the frame between the 802.11 header and the LLC/SNAP payload. The driver must strip it manually (same as OpenBSD `iwn_rx_done()`).

```
What firmware delivers:  [802.11 hdr (24B)] [CCMP IV (8B)] [LLC/SNAP (8B)] [Payload] [MIC (8B)]
What our code expected:  [802.11 hdr (24B)] [LLC/SNAP (8B)] [Payload]
```

Our `iwn_rx_poll()` looked for LLC/SNAP at offset 24, but found CCMP IV bytes instead. The SNAP prefix check (`0xAA 0xAA 0x03 0x00 0x00 0x00`) failed against random IV bytes, so every encrypted data frame was dropped.

**Fix**: In `iwn_rx_poll()`, check the Protected bit (`frame[1] & 0x40`). If set, skip 8 bytes (CCMP header) after the 802.11 header to reach LLC/SNAP, and subtract 8 from payload length for the CCMP MIC at the end:

```c
int ccmp_hdr = 0;
if (frame[1] & 0x40) {
    ccmp_hdr = 8;
    hdr_len += 8;
}
// ... later ...
if (ccmp_hdr && payload_len >= 8)
    payload_len -= 8;  /* exclude CCMP MIC */
```

### 13. AP Deauth After Data TX — GTK Used for TX Instead of PTK

**Symptom**: After fixing bug 12, the ARP probe runs and triggers a deauthentication from the AP during the 2-second wait:

```
DHCP: ARP probe (data path test)...
IWN: tx_ok=0 hw=55 cur=43 sw=0
DATA: AP sent deauth!
DHCP: ARP probe: 0 data frames
```

TX completes (`tx_ok` 0→1), firmware is active, but the AP immediately deauths us.

**Root cause**: `iwn_encap_tx()` used `node_id = IWN5000_ID_BROADCAST (15)` for broadcast/multicast destination frames. Node 15 has the GTK (Group Temporal Key) installed. In WPA2 infrastructure mode, ALL client TX goes to the AP (ToDS=1) and must be encrypted with PTK (Pairwise Transient Key, on node 0). The firmware encrypted our frame with GTK, the AP tried to decrypt with PTK, decryption failed, and the AP deauthenticated us for a security violation.

The GTK (node 15) is only for **RX group frame decryption** — when the AP broadcasts to all stations. For TX, the client always sends to the AP via PTK, and the AP handles re-broadcasting with GTK.

**Fix**: Changed `iwn_encap_tx()` to always use `node_id = IWN_ID_BSS (0)` for all data TX:

```c
/* OLD — used GTK for broadcast TX, causing AP to fail PTK decrypt → deauth */
if (dst_mac[0] & 1)
    node_id = IWN5000_ID_BROADCAST;  /* node 15, GTK */
else
    node_id = IWN_ID_BSS;            /* node 0, PTK */

/* NEW — all TX goes to AP via PTK in infra mode */
uint8_t node_id = IWN_ID_BSS;
```

### 14. Firmware Not Decrypting Frames — Missing IWN_KFLAG_MAP on Pairwise Key

**Symptom**: After fixing bugs 12-13, no more deauth — but still zero data frames in the software buffer. Added per-frame RX diagnostics inside `iwn_rx_poll()` to dump frame type, frame control, and raw bytes at offset 24-39 for every data frame. Results:

```
RX debug: total=173 mpdu=81 data=31 tx=1
RX DATA: fc=0862 len=124 [37 00 00 60 02 00 00 00 e8 57 77 9c 55 76 1e d2]
RX DATA: fc=0842 len=478 [3b 00 00 60 02 00 00 00 07 a8 0c d1 64 40 af 1a]
...
```

**31 data frames arrived** (not zero!). All have:
- `fc[0]=0x08` (type=2 data, subtype=0), `fc[1]=0x42/0x62` (FromDS=1, Protected=1)
- Bytes 24-27: `XX 00 00 60` — valid CCMP header (ExtIV=1, Key ID=1 = GTK)
- Bytes 32+: Random ciphertext (e.g., `e8 57 77 9c`) — NOT decrypted LLC/SNAP (`AA AA 03`)

**The firmware is delivering frames without decrypting them.** The CCMP header is intact, the Protected bit is set, and the payload is still encrypted ciphertext. Our LLC/SNAP check correctly fails against ciphertext.

All 31 frames have Key ID=1 (GTK-encrypted group/broadcast traffic). Zero unicast (PTK, Key ID=0) frames seen — the ARP reply never arrived or was filtered by the firmware.

**Root cause**: Comparing with OpenBSD's `iwn_set_key()`:

```c
/* OpenBSD — pairwise key: */
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP);

/* Our code — pairwise key (BEFORE fix): */
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_KID(0));  /* NO MAP! */
```

`IWN_KFLAG_MAP` (bit 3, 0x0008) tells the firmware to add the key to the hardware key mapping table used for RX decryption. Without it, the key is only used for TX encryption. The firmware likely won't enter "secure association" mode without a properly mapped pairwise key, preventing decryption of ALL frames (including GTK group traffic).

**Fix**: Added `IWN_KFLAG_MAP` to pairwise key installation:

```c
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0));
```

### 15. M4 Not Received by AP — Key Install Races TX Completion

**Symptom**: Possibly contributing to the deauth in bug 13. After sending EAPOL M4, we immediately install keys (two `ADD_NODE` commands, ~400ms). If M4 hasn't actually been transmitted and received by the AP by the time we start sending CCMP-encrypted data frames, the AP doesn't consider the handshake complete and deauths.

**Fix**: Added a 200ms delay after M4 TX before installing keys:

```c
efi_print_ascii("  M4 sent\r\n");
gBS->Stall(200000);  /* 200 ms — let M4 transmit */
// ... now install keys ...
```

### 16. Association Timeout — Auto-Retry Loop

**Symptom**: `iwn connect` frequently requires multiple manual attempts. Auth or assoc times out intermittently due to RF contention, AP not responding in time, or TX frame not going out before the response timer starts.

**Fix**: Added auto-retry loop in `iwn_connect()` — retries the auth→assoc→run sequence up to 3 times. Prints `Retry N/3...` for each retry. Gives up after 3 failures with a clear error message.

```c
int connected = 0;
for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
        efi_print_ascii("  Retry ");
        print_dec((uint32_t)attempt);
        efi_print_ascii("/3...\r\n");
    }
    if (iwn_auth(dev) != 0) continue;
    if (iwn_associate(dev) != 0) continue;
    if (iwn_run(dev) != 0) continue;
    connected = 1;
    break;
}
```

### 16. GTK Key ID Mismatch — AP Broadcasts With kid=1, EAPOL Distributes kid=2

**Symptom**: Round 8 (with `IWN_KFLAG_MAP` fix applied). Auth/assoc succeeds on retry 1/3, EAPOL completes cleanly — GTK extracted with `kid=2`, keys installed. ARP probe shows:

```
total=54 mpdu=24 data=0 tx=1
```

**Zero data frames** — down from 31 in round 7. With `IWN_KFLAG_MAP`, the firmware entered "secure mode" and now **drops** all frames it can't decrypt, instead of passing encrypted frames through.

**Root cause**: All 31 data frames from round 7 had CCMP Key ID=1 in the CCMP header (byte 3 = `0x60`: ExtIV=1, Key ID=1). But the EAPOL M3 message distributed the GTK with `kid=2`. This is normal GTK rotation behavior — the AP has:

- **kid=1** (active, currently encrypting broadcasts)
- **kid=2** (newly distributed via EAPOL, will become active on next rotation)

With `IWN_KFLAG_MAP`, the firmware tries to decrypt Key ID=1 frames but has no key at slot 1 (we installed GTK at slot 2). Decryption fails, firmware drops the frame. Result: `data=0`.

For DHCP, the OFFER is a broadcast frame (when using `flags=0x8000`), encrypted with GTK Key ID=1 → can't decrypt → DHCP fails.

**Resolution**: Instead of fighting GTK decryption (which would require tracking Key ID rotation and installing keys at both slots), bypass the issue entirely — switch DHCP to **unicast mode**. See bug 17.

### 17. Duplicate ADD_NODE(bcast) Hangs Intel 6000 Firmware

**Symptom**: Round 9. First connect attempt: auth ok, assoc ok, RXON(assoc) ok, ADD_NODE(AP) ok, LINKQ(AP) ok — then:

```
ADD_NODE(bcast): timeout (INT=0x00000000)
```

All subsequent retries: `RXON: timeout (INT=0x00000000)`. The firmware stopped generating interrupts entirely. Five more `iwn connect` attempts all failed with the same timeout.

**Root cause**: Attempted to re-add broadcast node (id=15) after RXON(assoc) in `iwn_run()`, following OpenBSD's pattern. But node 15 was already added during `iwn_auth()` and **survives RXON(assoc)** — the RXON command does NOT clear the node table. The ADD_NODE command with `control=0` (fresh add, not update) on an already-existing node hangs the Intel 6000 firmware permanently. The interrupt controller stops responding (`INT=0x00000000`), killing all subsequent commands.

**Fix**: Reverted the broadcast node re-add. Added comment:

```c
/* Note: broadcast node (id=15) was already added during iwn_auth()
 * and survives RXON(assoc). Do NOT re-add it here — a duplicate
 * ADD_NODE without IWN_NODE_UPDATE hangs the firmware. The node
 * persists and GTK can be installed via UPDATE in iwn_install_keys(). */
```

### 18. DHCP Unicast Mode — Bypass GTK Key ID Mismatch

**Change** (not a bug fix — a design change to work around bug 16):

Cleared the DHCP broadcast flag from `0x8000` to `0x0000` in `ns_dhcp.c`. With the unicast flag, the router sends the DHCP OFFER directly to our MAC address as a **unicast frame**. The AP encrypts unicast frames with the **PTK (pairwise key)**, which the firmware CAN decrypt via the MAP table (bug 14 fix). This completely bypasses the GTK Key ID mismatch issue.

Also removed the ARP probe from `ns_dhcp_run()` — routers ignore ARP requests from sender IP `0.0.0.0`, so it wasted 2 seconds with no useful data. RX diagnostics are now enabled during the DHCP exchange itself instead.

## Bugs Found — Summary

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|------------|-----|
| 1 | No UEFI IP stack on Dell | IP4Config2 not found | Dell firmware lacks DXE network drivers | Build custom ARP/DHCP/IP/UDP/TCP stack |
| 2 | DXE drivers hang on load | ConnectController never returns | DEPEX chains unsatisfied on Dell | Abandoned — custom stack instead |
| 3 | **SNP Start() hangs** | `snp->Start(snp)` never returns | PIC/relocation issue with EFIAPI indirect calls through protocol table on Dell firmware | Bypass SNP table — call IWN data functions directly |
| 4 | Association timeout | `iwn connect` needs retries | RF contention, intermittent TX failure | Auto-retry loop (3 attempts) |
| 5 | Log not captured on hang | bootai.log empty for crash section | Log flushes at REPL prompt, hang is before REPL | Screen photos for debug |
| 6 | Em dash UTF-8 rendering | `—` prints as two spaces | `efi_print_ascii()` treats multi-byte UTF-8 as individual bytes | Cosmetic, not fixed |
| 7 | **MAC hex out of bounds** | `0xAD` prints as `A ` | Lookup string `"0123456789AB"` only 12 chars; nibbles C-F OOB | Changed to `"0123456789ABCDEF"` |
| 8 | **TX ring buffer overflow** | DHCP DISCOVER silently fails | 310-byte frame overflows 80-byte inline space in ring entry | 2-segment TX descriptor + separate DMA buffer |
| 9 | **IP filter drops DHCP OFFER** | OFFER to offered IP dropped | `ns_ip_handle()` rejects non-broadcast when `ip={0,0,0,0}` | Accept all IP packets when `ip_ready=0` |
| 10 | **EAPOL/assoc TX overflow** | AP deauths after M2, intermittent | `iwn_tx_eapol()` and `iwn_tx_frame()` duplicate ring logic with `nsegs=1`; M2 (~135 bytes) overflows 80-byte inline | Refactored to call `iwn_tx_raw()` (has 2-segment fix) |
| 11 | **CCMP IV not stripped** | Zero data frames received after EAPOL | Firmware leaves 8-byte CCMP header in decrypted frames; SNAP check fails against IV bytes | Skip CCMP header (8B) + MIC (8B) when Protected bit set |
| 12 | **GTK used for TX** | AP deauths after first data TX | `iwn_encap_tx()` used node 15 (GTK) for broadcast TX; AP fails PTK decrypt | Always use `IWN_ID_BSS` (node 0, PTK) for all TX |
| 13 | **M4 TX race** | AP may not receive M4 before encrypted data arrives | Key install starts immediately after M4 queued, no TX completion wait | 200ms delay after M4 before key install |
| 14 | **Assoc timeout no retry** | User must re-run `iwn connect` manually | Single-attempt auth/assoc with no retry | Auto-retry loop (3 attempts) in `iwn_connect()` |
| 15 | **Firmware not decrypting** | All 31 RX data frames still encrypted (ciphertext at offset 32, not LLC/SNAP) | Pairwise key installed without `IWN_KFLAG_MAP` — firmware doesn't add key to HW key mapping table for RX decrypt | Added `IWN_KFLAG_MAP` to pairwise key kflags |
| 16 | **GTK Key ID mismatch** | data=0 with IWN_KFLAG_MAP (round 8); firmware drops all frames it can't decrypt | AP broadcasts with GTK kid=1, EAPOL distributes kid=2 (normal rotation); firmware can't find key at slot 1 | Bypassed — switch DHCP to unicast mode (PTK) |
| 17 | **Duplicate ADD_NODE hangs firmware** | `ADD_NODE(bcast): timeout (INT=0x00000000)` — firmware dead (round 9) | Node 15 already exists from `iwn_auth()`, survives RXON; fresh ADD_NODE (control=0) on existing node hangs Intel 6000 | Reverted re-add; node persists, use UPDATE for GTK |
| 18 | **DHCP broadcast via GTK fails** | DHCP OFFER is broadcast → encrypted with GTK kid=1 → can't decrypt → timeout | Broadcast flag 0x8000 forces broadcast OFFER, encrypted with mismatched GTK | Cleared broadcast flag to 0x0000 — OFFER sent as unicast via PTK |

## Key Lessons Learned

### 1. UEFI protocol function pointer tables are not reliable on all firmware

The SNP protocol interface pattern (`gBS->InstallProtocolInterface` + `gBS->HandleProtocol` to retrieve function pointers) works perfectly in QEMU/OVMF but hangs when calling through the table on Dell E6510 firmware. The function pointers are correct, the calling convention is correct, the target functions are trivial — but the indirect call never returns. When your driver code is linked into the same binary as the caller, direct function calls are both simpler and more reliable than going through the UEFI protocol dispatch mechanism.

### 2. bootai.log is useless for debugging init-time hangs

The log buffer flushes to disk at the REPL prompt. If the system hangs during initialization (before reaching the REPL), all debug output is in memory but never written to disk. For init-time debugging, screen photos are the only option. Consider adding an earlier flush point or a periodic flush timer.

### 3. Step-by-step trace prints are the most effective debug tool

On bare metal with no debugger, adding a print before and after each operation narrows hangs to exact lines. Three test cycles (initial hang → association retry → successful repro with trace prints) identified the exact call: `snp->Start(snp)`.

### 4. Bypassing abstractions can solve firmware-specific bugs

The SNP protocol is an abstraction layer. When the abstraction itself is the bug (firmware-specific indirect call issue), the fix is to bypass it entirely. Direct function calls to the underlying IWN data path (`iwn_encap_tx`, `iwn_rx_dequeue`) eliminate the protocol dispatch overhead and the firmware-specific hang.

### 5. TX ring entry sizes are smaller than you think

The Intel WiFi TX ring command buffer is 140 bytes per entry (4-byte header + 136-byte data). After the 56-byte `iwn_cmd_data` TX parameters, only 80 bytes remain for frame data. This is fine for control frames and ARP, but any real data frame (DHCP at 310 bytes, HTTP at 1400+ bytes) will overflow. OpenBSD handles this with multi-segment TX descriptors — the 802.11 header goes inline, the payload points to a separate DMA-mapped mbuf. On bare metal without mbufs, a single dedicated DMA page works for sequential single-threaded TX.

### 6. DHCP servers don't always send broadcast responses

Despite setting the broadcast flag (`flags = 0x8000`) in DHCP DISCOVER, some routers send the OFFER as unicast to the offered IP address. During DHCP, we don't have an IP yet (`{0,0,0,0}`), so the IP layer's destination filter must be relaxed — accept everything when `ip_ready == 0`.

### 7. Never duplicate hardware ring manipulation code

The TX ring buffer overflow manifested in THREE places because the ring manipulation logic was copy-pasted into `iwn_tx_raw()`, `iwn_tx_eapol()`, and `iwn_tx_frame()`. When bug 8 was fixed in `iwn_tx_raw()` (2-segment TX), the other two copies were left broken. The EAPOL overflow only caused intermittent failures (because the 55-byte overrun was small enough to "usually work" with contiguous DMA memory), making it hard to reproduce. **One canonical function for hardware operations, called from all paths.** Duplicated code means duplicated bugs — and the copies won't get the fix.

### 8. The firmware doesn't strip CCMP — you must

Intel 6000 firmware decrypts CCMP frames in place but leaves the 8-byte CCMP header (IV + ExtIV) and 8-byte MIC in the delivered frame. The driver must check the Protected bit and skip these manually. This matches OpenBSD's `iwn_rx_done()` which explicitly strips the CCMP header after checking `IWN_RX_DECRYPT_OK`. Without this, every encrypted data frame silently fails the LLC/SNAP verification and gets dropped — producing the symptom of "TX works, firmware receives frames, but zero data frames in software buffer."

### 9. In WPA2 infra mode, ALL TX uses PTK — GTK is RX-only

The GTK (Group Temporal Key) is installed on the broadcast node (node 15) for **receiving** group-addressed frames from the AP. For **transmitting**, even broadcast/multicast frames go to the AP (ToDS=1) and must be encrypted with PTK (node 0). The AP decrypts with PTK, then re-encrypts with GTK for group delivery. Using GTK for TX causes the AP to fail PTK decryption and immediately deauthenticate the client.

### 10. Add HW state diagnostics early — they pay for themselves

The `iwn_snp_debug_state()` function (prints `tx_ok`, HW RX ring index, software buffer count) was added as a diagnostic tool and immediately identified the root cause: TX works, firmware is active, but zero data frames pass our filter. Without it, we would have spent cycles investigating TX, 2-segment descriptors, or radio issues. A few lines of state dump save hours of guessing.

### 11. IWN_KFLAG_MAP is required for RX decryption — compare with OpenBSD

The Intel 6000 firmware requires `IWN_KFLAG_MAP` (bit 3) on the pairwise key `ADD_NODE` command to register the key in the hardware key mapping table for RX decryption. Without it, the key is TX-encryption only. Without a properly mapped pairwise key, the firmware won't decrypt ANY received frames (PTK or GTK). This was discovered by comparing our `iwn_install_keys()` with OpenBSD's `iwn_set_key()` — OpenBSD always sets `IWN_KFLAG_CCMP | IWN_KFLAG_MAP` for pairwise keys. Per-frame RX diagnostics (hex dump of bytes 24-39) confirmed the problem: all 31 data frames showed CCMP ciphertext at offset 32 instead of decrypted LLC/SNAP headers.

### 12. Intel 6000 node table survives RXON — never duplicate ADD_NODE

The RXON command (which reconfigures the radio) does NOT clear the firmware's node table. Nodes added during authentication (e.g., broadcast node 15 in `iwn_auth()`) persist across RXON(assoc) in `iwn_run()`. A duplicate `ADD_NODE` command with `control=0` (fresh add) on an already-existing node ID **hangs the Intel 6000 firmware permanently** — the interrupt controller stops responding, all subsequent commands timeout with `INT=0x00000000`. To update an existing node (e.g., to install a GTK), use `control=IWN_NODE_UPDATE` instead.

### 13. GTK Key ID rotation makes broadcast decryption unreliable on WiFi

WPA2 APs maintain two GTK slots and rotate between them. The EAPOL handshake distributes the **next** GTK (e.g., kid=2), but the AP continues broadcasting with the **current** GTK (kid=1) until rotation completes. With `IWN_KFLAG_MAP` enabled, the Intel firmware enters secure mode and drops any frame it can't decrypt — since we only have kid=2's key material, all kid=1 broadcast traffic is dropped. Rather than tracking GTK rotation and installing keys at both slots, it's simpler to avoid relying on broadcast reception entirely — e.g., use DHCP unicast mode (clear broadcast flag) so the OFFER comes as a PTK-encrypted unicast frame.

### 14. ARP from 0.0.0.0 is ignored by most routers

ARP requests with sender IP `0.0.0.0` (as used in DHCP-time ARP probes) are ignored by many routers — they won't send a unicast ARP reply to a station with no IP. The ARP probe wastes 2 seconds of polling time with no useful result. Better to test the data path with the DHCP exchange itself, which has its own timeout and response tracking.

## Current Status

**2026-02-26 (Dell test round 1)**: Three bugs found and fixed after first Dell hardware test of custom stack:

1. MAC hex display (lookup string too short) — fixed
2. TX ring buffer overflow (DHCP frame 310 bytes > 80 bytes inline) — fixed with 2-segment TX in `iwn_tx_raw()`
3. IP filter drops DHCP OFFER (dest check fails during DHCP) — fixed with `ip_ready` gate

**2026-02-26 (Dell test round 2)**: EAPOL handshake failed (AP deauth after M2). Root cause: `iwn_tx_eapol()` and `iwn_tx_frame()` duplicated the TX ring logic with the same inline-only overflow bug. Fixed by refactoring both to call `iwn_tx_raw()` (which has the 2-segment fix). Added `plcp`/`rflags` parameters to `iwn_tx_raw()` for rate selection. Eliminated ~120 lines of duplicated code.

**2026-02-26 (Dell test round 3)**: EAPOL fix confirmed — handshake completes cleanly. DHCP still fails: zero frames received during 5-second wait. Added HW diagnostics (`iwn_snp_debug_state()`) and ARP probe before DHCP to isolate the issue.

**2026-02-26 (Dell test round 4)**: Diagnostics confirmed TX works (`tx_ok` increments) and firmware is active (`hw` index advances), but zero data frames in software buffer. Root cause: CCMP IV not stripped from decrypted RX frames — SNAP check fails against IV bytes. Fixed.

**2026-02-26 (Dell test round 5)**: CCMP RX fix in place. AP deauths us immediately after first data TX. Root cause: `iwn_encap_tx()` used GTK (node 15) for broadcast TX — AP fails PTK decrypt and deauths. Fixed to always use PTK (node 0). Also added 200ms post-M4 delay and auto-retry loop for auth/assoc.

**2026-02-26 (Dell test round 6)**: No deauth (PTK-only fix works). Still zero data frames in software buffer. ARP probe shows `tx_ok` 0→1 and `hw` advancing but `sw=0`. Added per-frame RX diagnostics (counters + hex dump in `iwn_rx_poll()`).

**2026-02-26 (Dell test round 7)**: RX diagnostics reveal: `total=173 mpdu=81 data=31 tx=1` — 31 data frames arrived but ALL still encrypted. Bytes 24-31 show CCMP headers with Key ID=1 (GTK), bytes 32+ are ciphertext (not LLC/SNAP). Root cause: pairwise key installed without `IWN_KFLAG_MAP` — firmware doesn't add key to HW key mapping table, preventing RX decryption of ALL frames. Fixed by adding `IWN_KFLAG_MAP` to pairwise key kflags.

**2026-02-26 (Dell test round 8)**: `IWN_KFLAG_MAP` fix applied. Auth/assoc succeeds (retry 1/3), EAPOL completes cleanly. But ARP probe: `total=54 mpdu=24 data=0 tx=1` — **zero data frames** (down from 31 in round 7). With MAP, firmware enters "secure mode" and drops all frames it can't decrypt. Analysis of round 7 hex dumps: all 31 frames had CCMP Key ID=1 (byte 3 = `0x60`), but EAPOL gave us GTK kid=2. GTK rotation mismatch — AP broadcasts with old key (kid=1), distributes new key (kid=2). Firmware can't find key at slot 1, drops everything.

**2026-02-26 (Dell test round 9)**: Attempted to re-add broadcast node (id=15) after RXON(assoc), following OpenBSD's pattern. **Firmware hung permanently**: `ADD_NODE(bcast): timeout (INT=0x00000000)`. Node 15 already existed from `iwn_auth()` and survived RXON. Duplicate fresh ADD_NODE on existing node kills Intel 6000 interrupt controller. Five subsequent `iwn connect` attempts all timeout. Reverted the re-add.

**2026-02-27 (Dell test round 10 — build ready)**: Two changes to bypass GTK mismatch:
1. Cleared DHCP broadcast flag from `0x8000` to `0x0000` — router sends OFFER as unicast (PTK-encrypted, firmware CAN decrypt via MAP table)
2. Removed ARP probe (routers ignore ARP from 0.0.0.0, wasted 2 seconds)

Key question for round 10: **Does PTK-based unicast decryption actually work?** Round 8 showed zero data frames, but all observed frames were GTK-encrypted broadcasts. We haven't confirmed any unicast PTK frame was delivered and decrypted. The DHCP unicast OFFER will be the first test.

**Total: 18 bugs found, 16 fixed, 1 accepted, 1 design workaround** (em dash rendering accepted; GTK Key ID mismatch worked around via unicast DHCP).

## Files Modified

| File | Changes |
|------|---------|
| `iwn/iwn_snp.c` | Added `iwn_snp_get_mac()`, `iwn_snp_direct_tx()`, `iwn_snp_direct_rx()` — direct data path bypassing SNP function pointer table |
| `iwn/iwn_data.c` | Modified `iwn_tx_raw()` — 2-segment TX descriptor for frames > 80 bytes, separate DMA buffer for large payloads; added `plcp`/`rflags` params for caller-specified TX rate. Enhanced RX debug: counters for IWN_RX_DONE(195), IWN_RX_PHY(192), mgmt frames, unknown types with type code tracking |
| `iwn/iwn_eapol.c` | Replaced 58-line duplicated TX ring logic in `iwn_tx_eapol()` with single `iwn_tx_raw()` call. Added `IWN_KFLAG_MAP` to pairwise key installation. Added key install diagnostics (prints kflags + first 4 key bytes) |
| `iwn/iwn_assoc.c` | Replaced 60-line duplicated TX ring logic in `iwn_tx_frame()` with single `iwn_tx_raw()` call. Reverted broadcast node re-add in `iwn_run()` (caused firmware hang) |
| `iwn/iwn_internal.h` | Updated `iwn_tx_raw()` declaration — added `plcp`, `rflags` parameters |
| `iwn/iwn_dma.c` | Allocate/free 4 KB `tx_data` DMA page for ring 0 large frame payloads |
| `iwn/if_iwnvar.h` | Added `tx_data` DMA buffer field to `iwn_dev` struct |
| `efi/efi.h` | Declared direct data path functions + RX debug functions (`iwn_rx_set_debug`, `iwn_rx_print_debug`) |
| `efi/ns_net.c` | Rewritten to use direct IWN calls; fixed MAC hex lookup (`"0123456789ABCDEF"`) |
| `efi/ns_ip.c` | Modified IP destination filter — accept all packets when `ip_ready == 0` (during DHCP) |
| `efi/ns_dhcp.c` | Cleared DHCP broadcast flag (0x8000→0x0000) for unicast OFFER. Removed ARP probe. RX diagnostics enabled during DHCP exchange with frame breakdown on success/timeout |
| `efi/ns_arp.c` | NEW — ARP table, resolve, reply (~150 lines) |
| `efi/ns_udp.c` | NEW — UDP send/recv, port binding (~100 lines) |
| `efi/ns_tcp.c` | NEW — TCP state machine, retransmit (~550 lines) |
| `efi/ns_net.h` | NEW — shared types and declarations (~100 lines) |
| `efi/net.c` | Modified — dual dispatch: UEFI path (QEMU) vs custom path (Dell) |
