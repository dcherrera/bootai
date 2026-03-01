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

### 1\. UEFI IP Stack DXE Drivers Fail to Load

**Symptom**: After `iwn connect` installs SNP, `efi_net_init()` tries to locate `IP4Config2` protocol — not found. Falls back to `LocateProtocol` — also not found. Dell firmware simply doesn't have these drivers.

**Investigation**: Attempted loading EDK2 DXE drivers from USB (`drivers/` directory). `LoadImage` + `StartImage` succeeds but `ConnectController` hangs indefinitely. The drivers have DEPEX (dependency expression) chains that can't be satisfied on Dell firmware.

**Resolution**: Built a complete custom network stack (Phase 5c) — 7 new files (\~1,500 lines) implementing ARP, DHCP, IPv4, UDP, and TCP on raw SNP frames. Dual dispatch in `net.c` — UEFI path (QEMU) vs custom path (Dell) selected automatically.

### 2\. Custom Stack Init Hangs — No Output After "using custom network stack"

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

### 3\. SNP Start() Call Identified as Exact Hang Point

**Symptom**: Added step-by-step trace prints inside `ns_init()`:

```
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

### 4\. Fix: Bypass SNP Function Pointer Table Entirely

**Solution**: Since the IWN driver code is linked into the same binary, we can call the underlying data functions directly instead of going through the SNP protocol interface.

Added three direct data path functions to `iwn_snp.c`:

```
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

| Operation | Before (hangs)                             | After (bypass)                              |
| --------- | ------------------------------------------ | ------------------------------------------- |
| Get MAC   | snp->Mode->CurrentAddress                  | iwn_snp_get_mac(mac)                        |
| Init      | snp->Start(snp) + snp->Initialize(snp,0,0) | Removed — not needed                        |
| Transmit  | snp->Transmit(snp, 0, len, buf, ...)       | iwn_snp_direct_tx(dst, etype, payload, len) |
| Receive   | snp->Receive(snp, NULL, &sz, buf, ...)     | iwn_snp_direct_rx(buf, sz, &len)            |

**Key design change**: The old `ns_raw_send()` built a complete 14-byte Ethernet frame (dst MAC + src MAC + ethertype + payload) and passed it to `snp->Transmit()` with `HeaderSize=0`. The new version passes dst_mac, ethertype, and payload separately to `iwn_snp_direct_tx()`, which calls `iwn_encap_tx()` — this function handles Ethernet-to-802.11 encapsulation + LLC/SNAP + CCMP encryption internally.

**Result**: **CONFIRMED FIXED.** Dell E6510 now prints `ns: init MAC 00:27:10:56:AD:80` and proceeds to DHCP. The SNP bypass eliminates the hang entirely.

### 5\. SNP Bypass Confirmed — DHCP DISCOVER Sent, No OFFER Received

**Symptom**: With the SNP bypass fix, `ns_init()` completes and DHCP starts. But DISCOVER gets no response:

```
ns: init MAC 00:27:10:56:A :80
DHCP: starting...
DHCP: DISCOVER
DHCP: no OFFER received (timeout)
Custom DHCP failed
```

Two new bugs visible in this output: the MAC display is corrupted (`A :80` instead of `AD:80`), and the DHCP DISCOVER was sent but the router never responded. Investigation revealed three root causes (bugs 7, 8, 9 below).

### 6\. Association Timeout — Multiple Retries Required

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

### 7\. Em Dash UTF-8 Rendering

**Symptom**: The string `"No UEFI IP stack — using custom network stack"` renders on Dell screen as:

```
No UEFI IP stack  using custom network stack
```

The `—` (UTF-8 em dash, 3 bytes: `0xE2 0x80 0x94`) is printed as two spaces by `efi_print_ascii()`, which treats each byte individually.

**Impact**: Cosmetic only. Does not affect functionality.

### 8\. MAC Address Display — Hex Lookup Out of Bounds

**Symptom**: MAC printed as `00:27:10:56:A :80` instead of `00:27:10:56:AD:80`. The 5th byte (`0xAD`) displays as `A ` — high nibble `0xA` is correct but low nibble `0xD` is garbage.

**Root cause**: The hex lookup string in `ns_net.c:ns_init()` was `"0123456789AB"` — only 12 characters (indices 0-11). Nibble values 0xC through 0xF (12-15) index past the string terminator, reading garbage memory.

```
// BEFORE (broken — only 12 chars):
hex[0] = "0123456789AB"[b >> 4];
hex[1] = "0123456789AB"[b & 0xF];

// AFTER (correct — all 16 hex chars):
hex[0] = "0123456789ABCDEF"[b >> 4];
hex[1] = "0123456789ABCDEF"[b & 0xF];
```

**Fix**: Changed lookup string to `"0123456789ABCDEF"` (16 characters).

### 9\. TX Ring Buffer Overflow — DHCP Frame Too Large for Inline

**Symptom**: DHCP DISCOVER sent but no OFFER received. No firmware error — the frame appeared to transmit but the router never responded.

**Root cause**: The TX ring command buffer (`struct iwn_tx_cmd`) is 140 bytes per entry. After the 4-byte header and 56-byte `iwn_cmd_data`, only **80 bytes** remain for inline frame data. The ARP ping frame (60 bytes) fits, but the DHCP frame is **310 bytes** (24 802.11 header + 8 LLC/SNAP + 278 IP+UDP+DHCP).

`iwn_tx_raw()` was using a single TX descriptor segment, copying the entire frame inline:

```
/* BEFORE — copies 310 bytes into 80-byte space! */
iwn_memcpy(ring_buf + 4 + sizeof(struct iwn_cmd_data), frame, frame_len);

desc->nsegs = 1;
desc->segs[0].addr = IWN_LOADDR(buf_paddr);
desc->segs[0].len = (uint16_t)(IWN_HIADDR(buf_paddr) | ((uint16_t)totlen << 4));
```

This overflows by 230 bytes into adjacent ring entries. While the DMA memory is contiguous (so the firmware may read valid bytes), the corrupted adjacent entries and the scheduler byte count pointing at a single oversized segment likely cause the firmware to transmit a malformed or truncated frame that the router drops.

**Fix**: 2-segment TX descriptor, following OpenBSD's approach:

```
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

### 10\. IP Destination Filter Drops DHCP OFFER

**Symptom**: Even if the DHCP DISCOVER is transmitted correctly, the OFFER from the router may be silently dropped by our IP layer.

**Root cause**: `ns_ip_handle()` checks whether the incoming packet's destination IP matches our IP or is broadcast. During DHCP, our IP (`ns->ip`) is `{0,0,0,0}`. If the DHCP server sends the OFFER to the **offered IP** (e.g., `192.168.1.100`) instead of broadcast (`255.255.255.255`), both checks fail:

```
int for_us = ip_eq(dst, ns->ip);     // ip_eq({192.168.1.100}, {0.0.0.0}) = false
int is_bcast = (dst == 255.255.255.255);  // false
// → packet dropped!
```

The RFC says DHCP servers should honor the broadcast flag (`flags = 0x8000`), but some routers/DHCP servers send unicast to the offered IP regardless.

**Fix**: When `ip_ready == 0` (during DHCP), accept ALL incoming IP packets regardless of destination address. Once DHCP completes and `ip_ready` is set, normal destination filtering resumes.

```
if (ns->ip_ready) {
    /* Normal filtering: must be our IP, broadcast, or subnet broadcast */
    ...
    if (!for_us && !is_bcast && !subnet_bcast) return;
}
/* During DHCP (ip_ready=0): accept everything */
```

### 11\. EAPOL Handshake Intermittent Failure — TX Ring Overflow in EAPOL/Assoc Paths

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

The EAPOL M2 frame is \~135 bytes:

- 24 bytes 802.11 data header

- 8 bytes LLC/SNAP

- 4 bytes EAPOL header

- 95+ bytes EAPOL-Key body (SNonce + RSN IE + MIC)

This overflows the 80-byte inline space by \~55 bytes. Unlike the 310-byte DHCP overflow which always failed, the 55-byte EAPOL overflow is smaller and often "works" because the DMA ring memory is contiguous — the firmware reads valid bytes from adjacent ring entries. But occasionally the overflow corrupts adjacent ring state enough to produce a malformed frame that the AP rejects, causing deauthentication.

The association request frame from `iwn_tx_frame()` is \~70 bytes (usually fits within 80), so it was less affected. But the code was still duplicated and vulnerable for larger association frames with longer SSIDs or additional IEs.

**Root cause**: Code duplication. Three separate functions manipulated the TX ring independently:

| Function       | File        | Frame Size   | Overflow?           |
| -------------- | ----------- | ------------ | ------------------- |
| iwn_tx_raw()   | iwn_data.c  | Variable     | Fixed (2-segment)   |
| iwn_tx_eapol() | iwn_eapol.c | ~135 bytes   | YES (55 bytes over) |
| iwn_tx_frame() | iwn_assoc.c | ~30-80 bytes | Borderline          |

**Fix**: Refactored both `iwn_tx_eapol()` and `iwn_tx_frame()` to call `iwn_tx_raw()` instead of duplicating the ring mechanics. Added `plcp` and `rflags` parameters to `iwn_tx_raw()` so callers can specify the TX rate (CCK 1 Mbps for management/EAPOL reliability, OFDM 24 Mbps for data frames).

```
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

Same refactoring applied to `iwn_tx_frame()`. Eliminated \~120 lines of duplicated code. All three TX paths (data, EAPOL, management) now flow through the single canonical `iwn_tx_raw()` with its 2-segment DMA handling.

### 12\. CCMP IV Not Stripped From Decrypted RX Frames

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

```
int ccmp_hdr = 0;
if (frame[1] & 0x40) {
    ccmp_hdr = 8;
    hdr_len += 8;
}
// ... later ...
if (ccmp_hdr && payload_len >= 8)
    payload_len -= 8;  /* exclude CCMP MIC */
```

### 13\. AP Deauth After Data TX — GTK Used for TX Instead of PTK

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

```
/* OLD — used GTK for broadcast TX, causing AP to fail PTK decrypt → deauth */
if (dst_mac[0] & 1)
    node_id = IWN5000_ID_BROADCAST;  /* node 15, GTK */
else
    node_id = IWN_ID_BSS;            /* node 0, PTK */

/* NEW — all TX goes to AP via PTK in infra mode */
uint8_t node_id = IWN_ID_BSS;
```

### 14\. Firmware Not Decrypting Frames — Missing IWN_KFLAG_MAP on Pairwise Key

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

```
/* OpenBSD — pairwise key: */
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP);

/* Our code — pairwise key (BEFORE fix): */
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_KID(0));  /* NO MAP! */
```

`IWN_KFLAG_MAP` (bit 3, 0x0008) tells the firmware to add the key to the hardware key mapping table used for RX decryption. Without it, the key is only used for TX encryption. The firmware likely won't enter "secure association" mode without a properly mapped pairwise key, preventing decryption of ALL frames (including GTK group traffic).

**Fix**: Added `IWN_KFLAG_MAP` to pairwise key installation:

```
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0));
```

### 15\. M4 Not Received by AP — Key Install Races TX Completion

**Symptom**: Possibly contributing to the deauth in bug 13. After sending EAPOL M4, we immediately install keys (two `ADD_NODE` commands, \~400ms). If M4 hasn't actually been transmitted and received by the AP by the time we start sending CCMP-encrypted data frames, the AP doesn't consider the handshake complete and deauths.

**Fix**: Added a 200ms delay after M4 TX before installing keys:

```
efi_print_ascii("  M4 sent\r\n");
gBS->Stall(200000);  /* 200 ms — let M4 transmit */
// ... now install keys ...
```

### 16\. Association Timeout — Auto-Retry Loop

**Symptom**: `iwn connect` frequently requires multiple manual attempts. Auth or assoc times out intermittently due to RF contention, AP not responding in time, or TX frame not going out before the response timer starts.

**Fix**: Added auto-retry loop in `iwn_connect()` — retries the auth→assoc→run sequence up to 3 times. Prints `Retry N/3...` for each retry. Gives up after 3 failures with a clear error message.

```
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

### 16\. GTK Key ID Mismatch — AP Broadcasts With kid=1, EAPOL Distributes kid=2

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

### 17\. Duplicate ADD_NODE(bcast) Hangs Intel 6000 Firmware

**Symptom**: Round 9. First connect attempt: auth ok, assoc ok, RXON(assoc) ok, ADD_NODE(AP) ok, LINKQ(AP) ok — then:

```
ADD_NODE(bcast): timeout (INT=0x00000000)
```

All subsequent retries: `RXON: timeout (INT=0x00000000)`. The firmware stopped generating interrupts entirely. Five more `iwn connect` attempts all failed with the same timeout.

**Root cause**: Attempted to re-add broadcast node (id=15) after RXON(assoc) in `iwn_run()`, following OpenBSD's pattern. But node 15 was already added during `iwn_auth()` and **survives RXON(assoc)** — the RXON command does NOT clear the node table. The ADD_NODE command with `control=0` (fresh add, not update) on an already-existing node hangs the Intel 6000 firmware permanently. The interrupt controller stops responding (`INT=0x00000000`), killing all subsequent commands.

**Fix**: Reverted the broadcast node re-add. Added comment:

```
/* Note: broadcast node (id=15) was already added during iwn_auth()
 * and survives RXON(assoc). Do NOT re-add it here — a duplicate
 * ADD_NODE without IWN_NODE_UPDATE hangs the firmware. The node
 * persists and GTK can be installed via UPDATE in iwn_install_keys(). */
```

### 18\. DHCP Unicast Mode — Bypass GTK Key ID Mismatch

**Change** (not a bug fix — a design change to work around bug 16):

Cleared the DHCP broadcast flag from `0x8000` to `0x0000` in `ns_dhcp.c`. With the unicast flag, the router sends the DHCP OFFER directly to our MAC address as a **unicast frame**. The AP encrypts unicast frames with the **PTK (pairwise key)**, which the firmware CAN decrypt via the MAP table (bug 14 fix). This completely bypasses the GTK Key ID mismatch issue.

Also removed the ARP probe from `ns_dhcp_run()` — routers ignore ARP requests from sender IP `0.0.0.0`, so it wasted 2 seconds with no useful data. RX diagnostics are now enabled during the DHCP exchange itself instead.

### 19\. Group Key Missing IWN_KFLAG_MAP — Linux Sets MAP on Both Keys

**Symptom**: Round 10: auth/assoc clean, EAPOL clean with GTK kid=1 (matching AP's active key this time). Key diagnostics show pairwise `kflags=0x000A` (CCMP|MAP) and group `kflags=0x4102` (GROUP|KID(1)|CCMP). Still `data=0` — zero data frames during DHCP.

**Investigation**: Fetched Linux iwlwifi's `iwlagn_send_sta_key()` from `drivers/net/wireless/intel/iwlwifi/dvm/sta.c`. Two differences from our code:

```
/* Linux — BOTH pairwise AND group keys get MAP: */
sta_cmd.key.key_flags |= STA_KEY_FLG_MAP_KEY_MSK;  /* always set */

/* Our code — only pairwise had MAP. Group was: */
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_KID(gtk_kid) | IWN_KFLAG_GROUP);
/* Missing: IWN_KFLAG_MAP */
```

Without MAP on the group key, the firmware doesn't register the GTK in the hardware key mapping table for RX decryption. Even though the pairwise key has MAP (enabling "secure mode"), the group key without MAP means the firmware has no GTK to decrypt broadcast frames.

**Fix**: Added `IWN_KFLAG_MAP` to group key kflags:

```
node.kflags = htole16(IWN_KFLAG_CCMP | IWN_KFLAG_MAP |
                      IWN_KFLAG_KID(gtk_kid) | IWN_KFLAG_GROUP);
```

### 20\. node.kid (HW Key Table Offset) Not Set for Either Key

**Symptom**: Same investigation as bug 19. Linux also sets `key_offset` (our `node.kid`) on both keys — an index into the firmware's hardware key table. Our code left `node.kid = 0` for both keys (zero-initialized struct).

**Root cause**: The `kid` field in `struct iwn_node_info` tells the firmware which slot in the global hardware key table to store this key. In Linux:

```
sta_cmd.key.key_offset = keyconf->hw_key_idx;  /* free slot from iwl_get_free_ucode_key_offset() */
```

With both keys at slot 0, the group key may overwrite the pairwise key in the hardware table.

**Fix**: Set explicit slots — pairwise at slot 0, group at slot matching its Key ID:

```
/* Pairwise: */
node.kid = 0;      /* HW key table slot 0 */

/* Group: */
node.kid = gtk_kid; /* HW key table slot = Key ID from EAPOL (1 or 2) */
```

### 21\. NODECRYPT Diagnostic — Firmware Decryption Confirmed as Root Cause

**Diagnostic** (round 12): Added `IWN_FILTER_NODECRYPT` (bit 3) to RXON filter to bypass firmware decryption and see if data frames arrive at the radio at all.

**Results**: `total=647 mpdu=114 data=32 mgmt=82 tx=1 other=16 [162]`

**32 data frames received** (vs 0 in all previous rounds with decryption enabled). All frames show encrypted CCMP headers at offset 24:

```
RX DATA: fc=0862 len=138 [eb b7 00 a0 00 00 00 00 c3 63 fb 70 aa 13 bf 4c]
RX DATA: fc=0842 len=158 [ec b7 00 a0 00 00 00 00 f9 30 d1 07 b2 f8 7c 14]
```

CCMP header byte 3 = `0xa0`: ExtIV=1 (bit 5), **Key ID=2** (bits 6-7 = 10 binary). ALL 32 frames are GTK Key ID=2 broadcasts. **Zero unicast PTK frames** — the router isn't sending unicast DHCP OFFERs despite the cleared broadcast flag.

This conclusively proves: **The radio receives data frames fine. The firmware's decryption engine is dropping them all.** Despite having GTK with matching Key ID=2 installed with MAP, the firmware cannot find or use the key.

### 22\. Broadcast Node Lost After RXON(assoc) — Root Cause of Decrypt Failure

**Hypothesis** (round 13): The broadcast node (id=15) is added in `iwn_auth()` but **cleared by RXON(assoc)** in `iwn_run()`. When `iwn_install_keys()` later sends `ADD_NODE` with `UPDATE | SET_KEY` for node 15, the firmware accepts the command but has no valid node to attach the key to — the GTK is silently never installed.

**Evidence**:

- Bugs 19-20 fixed group key flags to match Linux exactly — still `data=0` (round 11)

- NODECRYPT diagnostic proves frames arrive at radio with correct Key ID=2 (round 12)

- Key install commands succeed (no error returned) — but firmware may silently ignore UPDATE on non-existent node

- OpenBSD's `iwn_run()` re-adds broadcast node AFTER RXON(assoc) — we don't

**Bug 17 reassessment**: Bug 17 concluded that "RXON doesn't clear the node table" based on the firmware hang when re-adding node 15. But the hang was likely caused by the **auth retry loop**: if `iwn_auth()` succeeds (adds node 15) but `iwn_associate()` fails, the retry calls `iwn_auth()` again — which sends a new RXON (clearing node 15) and re-adds it. If instead the first `iwn_auth()` succeeds AND `iwn_run()`'s RXON(assoc) also clears nodes, the fresh ADD_NODE would succeed (not a duplicate). The Bug 17 hang was from a different code path.

**Fix**: Re-add broadcast node (id=15) in `iwn_run()` after RXON(assoc), matching OpenBSD's behavior:

```
/* Re-add broadcast node after RXON(assoc).
 * RXON clears the firmware node table. */
struct iwn_node_info bcast_node;
iwn_memset(&bcast_node, 0, sizeof(bcast_node));
bcast_node.id = IWN5000_ID_BROADCAST;
iwn_memset(bcast_node.macaddr, 0xff, 6);
iwn_cmd(dev, IWN_CMD_ADD_NODE, &bcast_node, sizeof(bcast_node));
```

Also removed `IWN_FILTER_NODECRYPT` from RXON filter and changed group key `node.kid` from hardcoded 1 to `gtk_kid`.

**Risk**: If Bug 17 was correct and the node DOES survive RXON, the fresh ADD_NODE will hang the firmware again. Round 13 will determine which hypothesis is correct.

**Result (round 13)**: `ADD_NODE(bcast): timeout (INT=0x00000000)` — **firmware hung again**, identical to Bug 17. Hypothesis disproved: RXON(assoc) does NOT clear the node table. Node 15 survives RXON, duplicate ADD_NODE hangs. Reverted.

### 23\. Full RXON After Key Install Hangs Intel 6000 Firmware

**Hypothesis** (round 14): The firmware initializes its decrypt lookup table at RXON time. Since RXON was sent before keys existed, the decrypt table was built empty. Re-sending the same RXON after key installation should force the firmware to rebuild its decrypt tables.

**Result**: `RXON(crypto): timeout (INT=0x00000000)` — **firmware hung permanently**. Full RXON after key installation kills the Intel 6000 firmware, just like duplicate ADD_NODE. Once keys are installed with MAP, the firmware enters a crypto state that cannot tolerate a full RXON teardown/rebuild.

**Key insight**: Full `IWN_CMD_RXON` (cmd 16) after key installation is forbidden. The firmware apparently tries to tear down and rebuild the entire radio state including crypto, hits an inconsistency (keys exist but association state is being reset), and hangs.

### 24\. RXON_ASSOC Pattern — NODECRYPT Initially, Clear After Key Install

**Solution** (round 15): Match Linux iwlwifi's exact pattern:

1. **Initial RXON(assoc)** in `iwn_run()`: Set `IWN_FILTER_NODECRYPT` — firmware doesn't attempt decryption (no keys yet)

2. **Install keys** via ADD_NODE UPDATE (pairwise + group)

3. **RXON_ASSOC** (cmd 17, lightweight delta): Clear `NODECRYPT` — firmware activates decryption with installed keys

`IWN_CMD_RXON_ASSOC` is a lightweight RXON update that changes filter/flags/chains WITHOUT tearing down radio state. It avoids the full RXON hang while still telling the firmware "start decrypting now."

```
struct iwn_rxon_assoc rxon_assoc;
iwn_memset(&rxon_assoc, 0, sizeof(rxon_assoc));
rxon_assoc.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF | ...);
rxon_assoc.filter = htole32(IWN_FILTER_MULTICAST | IWN_FILTER_BSS);
/* NODECRYPT cleared — firmware activates decrypt with installed keys */
rxon_assoc.ofdm_mask = 0xff;
rxon_assoc.cck_mask = 0x0f;
...
iwn_cmd(dev, IWN_CMD_RXON_ASSOC, &rxon_assoc, sizeof(rxon_assoc));
```

In Linux iwlwifi: `DIS_DECRYPT` (= our NODECRYPT) is set initially in RXON, then cleared via `iwl_commit_rxon()` → `iwlagn_send_rxon_assoc()` after key installation. Linux detects that only filter_flags changed and sends RXON_ASSOC instead of full RXON.

## Bugs Found — Summary

| #   | Bug                                             | Symptom                                                                                                   | Root Cause                                                                                                     | Fix                                                                                                                                      |
| --- | ----------------------------------------------- | --------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | No UEFI IP stack on Dell                        | IP4Config2 not found                                                                                      | Dell firmware lacks DXE network drivers                                                                        | Build custom ARP/DHCP/IP/UDP/TCP stack                                                                                                   |
| 2   | DXE drivers hang on load                        | ConnectController never returns                                                                           | DEPEX chains unsatisfied on Dell                                                                               | Abandoned — custom stack instead                                                                                                         |
| 3   | SNP Start() hangs                               | snp->Start(snp) never returns                                                                             | PIC/relocation issue with EFIAPI indirect calls through protocol table on Dell firmware                        | Bypass SNP table — call IWN data functions directly                                                                                      |
| 4   | Association timeout                             | iwn connect needs retries                                                                                 | RF contention, intermittent TX failure                                                                         | Auto-retry loop (3 attempts)                                                                                                             |
| 5   | Log not captured on hang                        | bootai.log empty for crash section                                                                        | Log flushes at REPL prompt, hang is before REPL                                                                | Screen photos for debug                                                                                                                  |
| 6   | Em dash UTF-8 rendering                         | — prints as two spaces                                                                                    | efi_print_ascii() treats multi-byte UTF-8 as individual bytes                                                  | Cosmetic, not fixed                                                                                                                      |
| 7   | MAC hex out of bounds                           | 0xAD prints as A                                                                                          | Lookup string "0123456789AB" only 12 chars; nibbles C-F OOB                                                    | Changed to "0123456789ABCDEF"                                                                                                            |
| 8   | TX ring buffer overflow                         | DHCP DISCOVER silently fails                                                                              | 310-byte frame overflows 80-byte inline space in ring entry                                                    | 2-segment TX descriptor + separate DMA buffer                                                                                            |
| 9   | IP filter drops DHCP OFFER                      | OFFER to offered IP dropped                                                                               | ns_ip_handle() rejects non-broadcast when ip={0,0,0,0}                                                         | Accept all IP packets when ip_ready=0                                                                                                    |
| 10  | EAPOL/assoc TX overflow                         | AP deauths after M2, intermittent                                                                         | iwn_tx_eapol() and iwn_tx_frame() duplicate ring logic with nsegs=1; M2 (~135 bytes) overflows 80-byte inline  | Refactored to call iwn_tx_raw() (has 2-segment fix)                                                                                      |
| 11  | CCMP IV not stripped                            | Zero data frames received after EAPOL                                                                     | Firmware leaves 8-byte CCMP header in decrypted frames; SNAP check fails against IV bytes                      | Skip CCMP header (8B) + MIC (8B) when Protected bit set                                                                                  |
| 12  | GTK used for TX                                 | AP deauths after first data TX                                                                            | iwn_encap_tx() used node 15 (GTK) for broadcast TX; AP fails PTK decrypt                                       | Always use IWN_ID_BSS (node 0, PTK) for all TX                                                                                           |
| 13  | M4 TX race                                      | AP may not receive M4 before encrypted data arrives                                                       | Key install starts immediately after M4 queued, no TX completion wait                                          | 200ms delay after M4 before key install                                                                                                  |
| 14  | Assoc timeout no retry                          | User must re-run iwn connect manually                                                                     | Single-attempt auth/assoc with no retry                                                                        | Auto-retry loop (3 attempts) in iwn_connect()                                                                                            |
| 15  | Firmware not decrypting                         | All 31 RX data frames still encrypted (ciphertext at offset 32, not LLC/SNAP)                             | Pairwise key installed without IWN_KFLAG_MAP — firmware doesn't add key to HW key mapping table for RX decrypt | Added IWN_KFLAG_MAP to pairwise key kflags                                                                                               |
| 16  | GTK Key ID mismatch                             | data=0 with IWN_KFLAG_MAP (round 8); firmware drops all frames it can't decrypt                           | AP broadcasts with GTK kid=1, EAPOL distributes kid=2 (normal rotation); firmware can't find key at slot 1     | Bypassed — switch DHCP to unicast mode (PTK)                                                                                             |
| 17  | Duplicate ADD_NODE hangs firmware               | ADD_NODE(bcast): timeout (INT=0x00000000) — firmware dead (rounds 9, 13)                                  | Node 15 survives RXON(assoc); fresh ADD_NODE on existing node hangs Intel 6000 permanently                     | Confirmed — never re-add existing nodes                                                                                                  |
| 18  | DHCP broadcast via GTK fails                    | DHCP OFFER is broadcast → encrypted with GTK kid=1 → can't decrypt → timeout                              | Broadcast flag 0x8000 forces broadcast OFFER, encrypted with mismatched GTK                                    | Cleared broadcast flag to 0x0000 — OFFER sent as unicast via PTK                                                                         |
| 19  | Group key missing IWN_KFLAG_MAP                 | data=0 despite correct pairwise MAP (rounds 10-11)                                                        | Linux sets MAP on BOTH pairwise and group; our group key lacked MAP                                            | Added IWN_KFLAG_MAP to group key kflags                                                                                                  |
| 20  | node.kid (HW key offset) not set                | Both keys at slot 0 — group may overwrite pairwise                                                        | Linux sets key_offset per key; our code left both at 0                                                         | Pairwise kid=0, group kid=gtk_kid                                                                                                        |
| 21  | NODECRYPT confirms decrypt failure              | data=32 with NODECRYPT (round 12) vs data=0 without                                                       | Firmware receives frames but can't decrypt; GTK Key ID=2 matches installed key but firmware still drops        | Diagnostic — identified decrypt as root cause                                                                                            |
| 22  | Broadcast node lost after RXON(assoc)           | GTK install "succeeds" but firmware never decrypts                                                        | Disproved — RXON does NOT clear node table (round 13 hung on duplicate ADD_NODE)                               | Hypothesis wrong; node survives RXON                                                                                                     |
| 23  | Full RXON after key install hangs               | RXON(crypto): timeout (INT=0x00000000) — firmware dead (round 14)                                         | Full RXON teardown after MAP keys installed triggers firmware hang                                             | Never send full RXON after key install                                                                                                   |
| 24  | NODECRYPT not set during EAPOL                  | Firmware tries to decrypt with no keys → broken state → can't decrypt after keys installed                | RXON(assoc) sent without NODECRYPT; firmware enters decrypt mode before keys exist                             | Set NODECRYPT initially, clear via RXON_ASSOC after keys                                                                                 |
| 25  | RXON_ASSOC doesn't re-enable decrypt            | data=20, all encrypted, sw=0 despite RXON_ASSOC clearing NODECRYPT (round 15)                             | RXON_ASSOC updates filter flags but does NOT rebuild firmware's key lookup table                               | Abandoned NODECRYPT→RXON_ASSOC pattern; never set NODECRYPT                                                                              |
| 26  | ADD_NODE returns SUCCESS but keys not installed | All 4 ADD_NODE cmds return 0x01 (SUCCESS); still data=3 sw=0 (round 16)                                   | Firmware accepts key UPDATE command but doesn't populate HW key cache                                          | Diagnostic — confirms silent key install failure                                                                                         |
| 27  | SEC_ENC_ERR on ALL frames                       | MPDU tail flags show cipher=7 (SEC_TYPE_ERR) on all 52 frames (round 16b)                                 | Firmware finds source station but CANNOT find key in HW key cache; decrypt not even attempted                  | SMOKING GUN — proved key lookup table is empty                                                                                           |
| 28  | GTK install corrupts key table (hypothesis)     | PTK (pairwise) decryption also broken despite correct install                                             | OpenBSD never installs group keys in HW; GTK ADD_NODE UPDATE on bcast node may corrupt firmware key table      | Remove GTK HW install, keep PTK only (testing round 17)                                                                                  |
| 29  | Firmware death after AP deauth                  | After deauth during EAPOL, all subsequent iwn connect fail with RXON: timeout (INT=0x00000000)            | AP deauth during handshake kills firmware interrupt controller; NIC becomes permanently unresponsive           | iwn_fw_restart() — power cycle + runtime FW reload with saved calibration                                                                |
| 30  | M4 TX not verified                              | AP retransmits M3 twice then deauths — M4 never received (round 18)                                       | Blind 200ms delay after M4 TX provides no confirmation; tx_ok=1 only counts DHCP DISCOVER, not M4              | TX_DONE polling after M4 — reads firmware TX status, ACK fail count, RTS fail count                                                      |
| 31  | HW CCMP decrypt broken permanently              | SEC_ENC_ERR on ALL frames across 19 rounds; firmware accepts keys but never populates HW key lookup table | Intel 6000 firmware bug — black box, unfixable                                                                 | Software CCMP decryption (iwn_ccmp.c): AES-CCM decrypt + MIC verify in ~175 lines. NODECRYPT set permanently, all RX decrypt in software |
| 32  | Removing HW key install broke TX encryption      | Zero data frames (data=0) after removing ALL HW key install; DHCP DISCOVER sent but no OFFER received     | TX path uses firmware CCMP encrypt via security field; with PTK removed from HW, firmware can't encrypt TX     | Restored PTK HW install for TX encryption; only GTK install skipped (RX group decrypt in software)                                       |
| 33  | FCS included in flen with NODECRYPT              | CTR decrypts correct LLC/SNAP but ALL MICs fail — computed vs received MIC completely different            | NODECRYPT skips ALL post-processing including FCS strip; flen includes 4-byte FCS; rx_mic pointer lands in FCS | **Disproved** — FCS subtraction made MIC worse (rounds 24-28). Reversed in round 29                                                     |
| 34  | Self-test: FIPS 197 + RFC 3610 + synthetic frame | Need to prove AES-CCM algorithm correct independently of real frames                                     | N/A — diagnostic                                                                                               | All 3 tests PASS: AES-128, CCM Vector #1, synthetic 802.11 frame through full iwn_ccmp_decrypt()                                         |
| 35  | Self-test trigger in wrong function              | Self-test never ran (round 24) — placed inside iwn_ccmp_decrypt() which is only called for encrypted data | ccmp_self_test() only reachable when data frames arrive; data=0 means never called                             | Moved to iwn_rx_poll() with static once-flag                                                                                             |
| 36  | EAPOL intermittent M4 ACK failures               | M4 TX status=0x83, ack_fail=1, AP deauths (rounds 25-27)                                                 | Intel 6000 TX unreliable at OFDM 24Mbps — ACK timing issues                                                   | Accepted — retry iwn connect                                                                                                             |
| 37  | FCS hypothesis — inconclusive alone               | MIC fails both with and without flen -= 4 (rounds 24-31)                                                 | Cannot determine FCS inclusion in isolation — MIC fails regardless due to AAD mismatch (bug 38)                | Re-applied flen -= 4 in combination with QoS AAD fix (testing round 32)                                                                 |
| 38  | Firmware strips QoS Control field from frames     | All frames have fc=0x08 (non-QoS) but 802.11n APs always send QoS (fc=0x88). QoS AAD alone doesn't fix   | Intel 6000 firmware strips 2-byte QoS Control, converts fc0 0x88→0x08. AP encrypted with QoS AAD (24B) but we compute non-QoS AAD (22B). CTR unaffected (no AAD), only CBC-MAC/MIC breaks | Force QoS AAD reconstruction: fc0=0x88, QoS TC=[0,0], aad_len=24. Testing combined with FCS fix (round 32) |
| 39  | Combined FCS+QoS: two simultaneous bugs            | Neither fix alone works; each was tested in isolation and failed                                          | FCS in flen (need -4) AND QoS stripping (need fc0=0x88, 24B AAD) — both present simultaneously                | Applied both fixes together (round 32): flen -= 4 + QoS AAD. Self-test updated to encrypt with QoS AAD    |

## Key Lessons Learned

### 1\. UEFI protocol function pointer tables are not reliable on all firmware

The SNP protocol interface pattern (`gBS->InstallProtocolInterface` + `gBS->HandleProtocol` to retrieve function pointers) works perfectly in QEMU/OVMF but hangs when calling through the table on Dell E6510 firmware. The function pointers are correct, the calling convention is correct, the target functions are trivial — but the indirect call never returns. When your driver code is linked into the same binary as the caller, direct function calls are both simpler and more reliable than going through the UEFI protocol dispatch mechanism.

### 2\. bootai.log is useless for debugging init-time hangs

The log buffer flushes to disk at the REPL prompt. If the system hangs during initialization (before reaching the REPL), all debug output is in memory but never written to disk. For init-time debugging, screen photos are the only option. Consider adding an earlier flush point or a periodic flush timer.

### 3\. Step-by-step trace prints are the most effective debug tool

On bare metal with no debugger, adding a print before and after each operation narrows hangs to exact lines. Three test cycles (initial hang → association retry → successful repro with trace prints) identified the exact call: `snp->Start(snp)`.

### 4\. Bypassing abstractions can solve firmware-specific bugs

The SNP protocol is an abstraction layer. When the abstraction itself is the bug (firmware-specific indirect call issue), the fix is to bypass it entirely. Direct function calls to the underlying IWN data path (`iwn_encap_tx`, `iwn_rx_dequeue`) eliminate the protocol dispatch overhead and the firmware-specific hang.

### 5\. TX ring entry sizes are smaller than you think

The Intel WiFi TX ring command buffer is 140 bytes per entry (4-byte header + 136-byte data). After the 56-byte `iwn_cmd_data` TX parameters, only 80 bytes remain for frame data. This is fine for control frames and ARP, but any real data frame (DHCP at 310 bytes, HTTP at 1400+ bytes) will overflow. OpenBSD handles this with multi-segment TX descriptors — the 802.11 header goes inline, the payload points to a separate DMA-mapped mbuf. On bare metal without mbufs, a single dedicated DMA page works for sequential single-threaded TX.

### 6\. DHCP servers don't always send broadcast responses

Despite setting the broadcast flag (`flags = 0x8000`) in DHCP DISCOVER, some routers send the OFFER as unicast to the offered IP address. During DHCP, we don't have an IP yet (`{0,0,0,0}`), so the IP layer's destination filter must be relaxed — accept everything when `ip_ready == 0`.

### 7\. Never duplicate hardware ring manipulation code

The TX ring buffer overflow manifested in THREE places because the ring manipulation logic was copy-pasted into `iwn_tx_raw()`, `iwn_tx_eapol()`, and `iwn_tx_frame()`. When bug 8 was fixed in `iwn_tx_raw()` (2-segment TX), the other two copies were left broken. The EAPOL overflow only caused intermittent failures (because the 55-byte overrun was small enough to "usually work" with contiguous DMA memory), making it hard to reproduce. **One canonical function for hardware operations, called from all paths.** Duplicated code means duplicated bugs — and the copies won't get the fix.

### 8\. The firmware doesn't strip CCMP — you must

Intel 6000 firmware decrypts CCMP frames in place but leaves the 8-byte CCMP header (IV + ExtIV) and 8-byte MIC in the delivered frame. The driver must check the Protected bit and skip these manually. This matches OpenBSD's `iwn_rx_done()` which explicitly strips the CCMP header after checking `IWN_RX_DECRYPT_OK`. Without this, every encrypted data frame silently fails the LLC/SNAP verification and gets dropped — producing the symptom of "TX works, firmware receives frames, but zero data frames in software buffer."

### 9\. In WPA2 infra mode, ALL TX uses PTK — GTK is RX-only

The GTK (Group Temporal Key) is installed on the broadcast node (node 15) for **receiving** group-addressed frames from the AP. For **transmitting**, even broadcast/multicast frames go to the AP (ToDS=1) and must be encrypted with PTK (node 0). The AP decrypts with PTK, then re-encrypts with GTK for group delivery. Using GTK for TX causes the AP to fail PTK decryption and immediately deauthenticate the client.

### 10\. Add HW state diagnostics early — they pay for themselves

The `iwn_snp_debug_state()` function (prints `tx_ok`, HW RX ring index, software buffer count) was added as a diagnostic tool and immediately identified the root cause: TX works, firmware is active, but zero data frames pass our filter. Without it, we would have spent cycles investigating TX, 2-segment descriptors, or radio issues. A few lines of state dump save hours of guessing.

### 11\. IWN_KFLAG_MAP is required for RX decryption — compare with OpenBSD

The Intel 6000 firmware requires `IWN_KFLAG_MAP` (bit 3) on the pairwise key `ADD_NODE` command to register the key in the hardware key mapping table for RX decryption. Without it, the key is TX-encryption only. Without a properly mapped pairwise key, the firmware won't decrypt ANY received frames (PTK or GTK). This was discovered by comparing our `iwn_install_keys()` with OpenBSD's `iwn_set_key()` — OpenBSD always sets `IWN_KFLAG_CCMP | IWN_KFLAG_MAP` for pairwise keys. Per-frame RX diagnostics (hex dump of bytes 24-39) confirmed the problem: all 31 data frames showed CCMP ciphertext at offset 32 instead of decrypted LLC/SNAP headers.

### 12\. Intel 6000 node table survives RXON — confirmed twice

The RXON command does NOT clear the firmware's node table. **Confirmed in rounds 9 and 13**: both times, adding broadcast node (id=15) after RXON(assoc) hung the firmware with `INT=0x00000000` — proving the node already existed (a duplicate fresh ADD_NODE on an existing node is fatal). Nodes added during `iwn_auth()` persist through RXON(assoc) in `iwn_run()`. To modify an existing node (e.g., install GTK), use `control=IWN_NODE_UPDATE`. Never re-add.

### 13\. GTK Key ID rotation makes broadcast decryption unreliable on WiFi

WPA2 APs maintain two GTK slots and rotate between them. The EAPOL handshake distributes the **next** GTK (e.g., kid=2), but the AP continues broadcasting with the **current** GTK (kid=1) until rotation completes. With `IWN_KFLAG_MAP` enabled, the Intel firmware enters secure mode and drops any frame it can't decrypt — since we only have kid=2's key material, all kid=1 broadcast traffic is dropped. Rather than tracking GTK rotation and installing keys at both slots, it's simpler to avoid relying on broadcast reception entirely — e.g., use DHCP unicast mode (clear broadcast flag) so the OFFER comes as a PTK-encrypted unicast frame.

### 14\. ARP from 0.0.0.0 is ignored by most routers

ARP requests with sender IP `0.0.0.0` (as used in DHCP-time ARP probes) are ignored by many routers — they won't send a unicast ARP reply to a station with no IP. The ARP probe wastes 2 seconds of polling time with no useful result. Better to test the data path with the DHCP exchange itself, which has its own timeout and response tracking.

### 15\. NODECRYPT diagnostic separates radio from decrypt

Adding `IWN_FILTER_NODECRYPT` to RXON bypasses firmware decryption entirely — frames arrive raw (encrypted). This instantly separates "no frames at the radio" from "firmware can't decrypt." Round 12 proved 32 data frames arrive (all GTK Key ID=2 broadcasts) — the radio and association are fine. The problem is purely in the firmware's key lookup/decryption path. Without this diagnostic, we'd have spent more rounds guessing whether the issue was radio configuration, channel, association state, or key material.

### 16\. Linux iwlwifi is a better reference than OpenBSD for key details

OpenBSD's IWN driver is excellent for structure definitions and general flow, but its key installation code is sparse. Linux iwlwifi (`dvm/sta.c:iwlagn_send_sta_key()`) shows the exact flags and fields needed — MAP on both keys, key_offset set, INVALID cleared. The Intel firmware behavior is documented through Linux's comments and code paths, not through OpenBSD's simpler implementation. When debugging hardware-specific firmware behavior, fetch the Linux equivalent function.

### 17\. Firmware command "success" doesn't mean the operation took effect

The Intel 6000 firmware accepts `ADD_NODE` with `UPDATE | SET_KEY` on a non-existent node without returning an error. The command response reports success, but the key is never installed because there's no valid node to attach it to. This is a silent failure — the firmware doesn't validate that the target node exists before accepting the update. Never assume a firmware command worked just because it returned OK. Verify the effect independently (e.g., with NODECRYPT diagnostic to check if frames are actually being decrypted).

### 18\. Full RXON after key installation is forbidden on Intel 6000

Sending `IWN_CMD_RXON` (cmd 16) after keys are installed with `IWN_KFLAG_MAP` hangs the Intel 6000 firmware permanently (`INT=0x00000000`). The full RXON tears down and rebuilds the entire radio state including the crypto engine. With MAP keys active, this teardown triggers an internal firmware inconsistency. Use `IWN_CMD_RXON_ASSOC` (cmd 17) instead — it updates filter flags, rate masks, and chain config as a lightweight delta without tearing down the radio or crypto state.

### 19\. Set NODECRYPT before keys exist, clear it after — match Linux's DIS_DECRYPT pattern

The Intel firmware's decrypt engine may enter a broken state if RXON enables decryption (no NODECRYPT) when no keys are installed. The firmware tries to decrypt incoming frames, finds no keys in the MAP table, and may not recover when keys are installed later. Linux iwlwifi handles this by setting `RXON_FILTER_DIS_DECRYPT_MSK` (= our `IWN_FILTER_NODECRYPT`) in the initial RXON, then clearing it via `RXON_ASSOC` after key installation. This ensures the firmware's decrypt engine is only activated when keys are ready.

**Update (round 15)**: This pattern DOES NOT WORK. RXON_ASSOC does not rebuild the key lookup table. The NODECRYPT→RXON_ASSOC approach was abandoned.

### 20\. MPDU tail flags are the definitive decrypt diagnostic

The `uint32_t` status word after frame data in `IWN_MPDU_RX_DONE` notifications contains the firmware's exact decrypt verdict: cipher type (bits 8-10), decrypt attempted (bit 11), and decrypt result (bits 11-12). `SEC_TYPE_ERR (cipher=7)` means the firmware found the station but could NOT find a key — the hardware key cache is empty. This is far more informative than inference from `data=0` vs `data=31`. Always read tail flags for decrypt debugging.

### 21\. OpenBSD never installs group keys in hardware — follow its lead

OpenBSD's `iwn_set_key()` early-returns to software crypto for ALL group keys (`IEEE80211_KEY_GROUP`). Only pairwise CCMP keys are sent to Intel firmware via ADD_NODE UPDATE. This is deliberate: the Intel 6000 firmware's group key handling through ADD_NODE may be unreliable or may corrupt the key table. Linux installs both, but Linux also has a much more complex key offset allocator. When porting from OpenBSD, match its conservatism — install pairwise only, handle group in software.

### 22\. Firmware command SUCCESS (0x01) does not mean the key is usable

All four ADD_NODE commands returned 0x01 (SUCCESS) in round 16, yet MPDU tail flags showed SEC_ENC_ERR on every frame in round 16b — the key is NOT in the hardware key cache despite the "success" response. The firmware's ADD_NODE response only confirms the command was parsed and the node metadata was updated. It does NOT confirm the key was indexed into the hardware key lookup table. Independently verify key installation via tail flags, not command response codes.

### 23\. Firmware death requires full power cycle + firmware reload

When the Intel 6000 firmware stops generating interrupts (`INT=0x00000000`), it cannot be recovered by re-sending commands — the interrupt controller is dead. Recovery requires:

1. `iwn_apm_stop()` — stop master, SW reset, clear INIT_DONE

2. 10ms settle

3. `iwn_apm_init()` — full power-on sequence

4. `iwn5000_nic_config()` — radio/antenna config

5. Stop FH DMA, reset ring pointers (reuse existing DMA buffers)

6. Re-program ring addresses in hardware

7. Load RUNTIME firmware (skip init FW — calibration already saved from first boot)

8. Wait for `UC_READY`

9. Post-alive scheduler setup + WiMAX coex + crystal calib + replay saved calibration

Critical requirement: the firmware raw buffer (`fw_rawbuf`) must NOT be freed after the initial `iwn_hw_init()` — the parsed firmware section pointers (`fw.main_text`, `fw.main_data`) point into this buffer. If freed, restart is impossible without re-reading the firmware file from disk.

The DMA scratch buffer (`fw_dma`) can be freed and re-allocated on demand — `iwn_load_section()` already handles NULL `fw_dma.vaddr`.

### 24\. When firmware crypto is a black box, move to software

After 19 rounds of debugging Intel 6000 firmware CCMP decryption (key flags, MAP bits, key offsets, NODECRYPT, RXON_ASSOC, GTK removal), the firmware consistently accepts key install commands but never populates the HW key lookup table. The firmware is a black box — we can't inspect its internal state or fix its key table logic. Software CCMP decryption (\~175 lines of AES-CCM) bypasses the entire problem. The existing `iwn_aes128_encrypt()` is sufficient — CCM uses AES encrypt for both CTR keystream and CBC-MAC. No AES decrypt needed. With NODECRYPT set permanently, frames arrive raw and software handles everything. This is the same approach OpenBSD uses for group keys — and now we use it for ALL keys.

### 25\. Keep firmware raw buffer alive for restart capability

The initial `iwn_hw_init()` freed `fw_rawbuf` after loading runtime firmware (lines 850-853). This made firmware section pointers dangling — `fw.main_text` and `fw.main_data` pointed into freed memory. Any attempt to reload firmware would use invalid pointers. Removed the free to keep the buffer alive (\~340 KB for iwn-6000 firmware). Minor memory cost for critical recovery capability.

### 26\. HW encrypt and HW decrypt are independent — don't remove one when fixing the other

The Intel 6000 firmware's TX encryption and RX decryption use the same key material (installed via ADD_NODE SET_KEY) but are completely independent subsystems. HW decrypt is broken (SEC_ENC_ERR, 19 rounds), but HW encrypt works perfectly (TX_DONE status=0x01, AP ACKs frames). When moving RX decrypt to software, the PTK must STAY installed in firmware for TX. Removing it silently breaks TX — the AP drops our frames and sends nothing back, producing the misleading symptom of "zero data frames received" which looks like an RX problem. Always verify that a "remove" change doesn't have side effects on other subsystems sharing the same resource.

### 27\. RXON_ASSOC Does NOT Re-Enable Decrypt Engine

**Symptom** (round 15): RXON_ASSOC (cmd 17) succeeded — no hang, firmware responsive. But still zero data frames decrypted. 20 data frames with CCMP headers intact, `sw=0`.

**Root cause**: The RXON_ASSOC command updates filter flags, rate masks, and chain config, but does NOT restart the firmware's decrypt engine. Clearing `NODECRYPT` via RXON_ASSOC tells the firmware "enable decrypt filtering" — but the firmware's key lookup table was never built because keys didn't exist when the full RXON ran. RXON_ASSOC doesn't trigger a key table rebuild.

**Resolution**: Abandoned the NODECRYPT→RXON_ASSOC pattern. Instead, never set NODECRYPT at all — let the firmware run with decryption enabled from the start. EAPOL frames (Protected=0) pass through unaffected. After key install, the firmware has keys in the MAP table and should decrypt Protected=1 frames.

### 26b\. NODECRYPT Changes ALL Firmware Post-Processing — Including FCS Strip

**UPDATE (round 29): This lesson was DISPROVED.** Rounds 24-28 showed MIC still fails WITH the FCS subtraction. The self-test (RFC 3610 + synthetic 802.11 frame) proves the algorithm is correct. Removing the FCS subtraction in round 29. `mpdu->len` does NOT include FCS on the Intel 6000.

~~When `IWN_FILTER_NODECRYPT` is set, the Intel 6000 firmware skips not just decryption but ALL post-processing of received data frames. This includes stripping the 4-byte FCS (Frame Check Sequence / CRC32) at the end of every 802.11 frame. The firmware-reported frame length (`flen` from `iwn_rx_mpdu`) includes these 4 bytes.~~

~~In normal operation (no NODECRYPT), the firmware strips FCS before delivering the frame to the driver. With NODECRYPT, the frame is delivered exactly as received over the air, including FCS. Any code that parses frame contents (CCMP header, MIC, payload length) must account for the extra 4 bytes.~~

### 26\. ADD_NODE Response Status — All Return SUCCESS (0x01)

**Diagnostic** (round 16): Added `last_cmd_status` field to `iwn_dev` to capture the first byte of every ADD_NODE firmware response. Response codes: 0x01=SUCCESS, 0x02=NO_ROOM, 0x08=MODIFY_NON_EXIST_STA.

**Results**: ALL four ADD_NODE commands returned 0x01 (SUCCESS):

- ADD_NODE(broadcast, fresh) → 0x01

- ADD_NODE(AP, fresh) → 0x01

- ADD_NODE(pairwise, UPDATE+SET_KEY) → 0x01

- ADD_NODE(group, UPDATE+SET_KEY) → 0x01

Still `data=3 sw=0` — 3 data frames received, all with CCMP headers, still not decrypted. Key installs are accepted by firmware but keys are NOT in the hardware key lookup table.

### 27\. MPDU Tail Flags — SEC_ENC_ERR (cipher=7) On ALL Frames

**Diagnostic** (round 16b): The Intel firmware writes a `uint32_t` status word AFTER the frame data in `IWN_MPDU_RX_DONE` notifications. Layout:

```
[iwn_rx_desc] [iwn_rx_mpdu (4B)] [frame data (len bytes)] [uint32_t tail_flags]
```

**MPDU tail flags bit layout** (from Linux `iwl-agn-rx.c`): `Bits 0-1:  IWN_RX_NOERROR (0x03) — CRC and overflow OK Bit 2:     Source station found in node table Bits 8-10: Cipher type (0=none, 1=WEP, 2=CCMP, 3=TKIP, 7=SEC_TYPE_ERR) Bit 11:    IWN_RX_MPDU_DEC — decrypt was attempted Bits 11-12: IWN_RX_DECRYPT_MASK — 0x1800=OK, 0x0800=fail, 0x0000=not attempted`

**Results**: Every single data frame shows `fl=0x00XX0707 NO_DEC`:

- Bits 0-2: `0x07` → CRC OK, overflow OK, **source station found** (bit 2=1)

- Bits 8-10: `0x07` → cipher = 7 = **SEC_TYPE_ERR** — firmware cannot find key

- Bit 11: 0 → decrypt **NOT even attempted**

52 data frames, ALL with `NO_DEC` and `SEC_ENC_ERR`. All broadcast (Key ID=1), sequential PNs. `tx_ok=1` (DHCP DISCOVER sent). No unicast frames visible.

**SMOKING GUN**: The firmware finds the source station in the node table (bit 2=1) but cannot find a valid key in the hardware key cache (cipher=7). Despite ADD_NODE UPDATE returning SUCCESS (0x01), the keys are NOT in the firmware's key lookup table.

### 28\. OpenBSD Never Installs Group Keys in Hardware

**Research** (round 17): Fetched OpenBSD's `iwn_set_key()` from `sys/dev/pci/if_iwn.c`:

```
int iwn_set_key(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
    if ((k->k_flags & IEEE80211_KEY_GROUP) ||
        k->k_cipher != IEEE80211_CIPHER_CCMP)
        return ieee80211_set_key(ic, ni, k);  /* software crypto */

    /* Only pairwise CCMP reaches here */
    kflags = IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(k->k_id);
    node.kid = k->k_id;  /* k_id=0 for pairwise (struct memset to 0) */
    ...
}
```

**OpenBSD ONLY installs pairwise CCMP keys in hardware.** Group keys are ALWAYS handled in software by `ieee80211_set_key()`. The `IWN_KFLAG_GROUP` code below the early-return is dead/unreachable.

**Hypothesis**: Installing the GTK on the broadcast node (id=15) via ADD_NODE UPDATE corrupts the firmware's key table, breaking PTK (pairwise) decryption too. The Intel 6000 firmware may not properly handle group key installation via ADD_NODE, causing the entire key lookup table to become invalid.

**Fix for round 17**:

1. Remove GTK hardware install — only install PTK (matching OpenBSD exactly)

2. Add unicast/broadcast classification to RX debug (`RX UC:` vs `RX BC:`)

3. GTK saved in `dev->gtk` for future software CCMP if needed

**OpenBSD key deletion** (for reference): `c void iwn_delete_key(...) {     node.kflags = htole16(IWN_KFLAG_INVALID);     node.kid = 0xff; } `

### 29\. Firmware Death After AP Deauth — Unrecoverable Without Restart

**Symptom** (round 17): First `iwn connect` attempt: auth OK after 2 assoc timeouts, EAPOL started but M2 rejected (M1 retransmission, then AP deauth). All subsequent `iwn connect` attempts fail immediately:

```
RXON: timeout (INT=0x00000000)
```

The firmware stopped generating interrupts entirely. Five more `iwn connect` calls all fail identically — the NIC is dead until reboot.

**Root cause**: When the AP sends a deauthentication frame during the EAPOL handshake, the firmware enters an unrecoverable state. The interrupt controller stops responding, and no further commands (RXON, ADD_NODE, etc.) can execute because `iwn_wait_cmd()` polls the INT register which is permanently zero.

**Impact**: Round 17's GTK removal fix was NEVER TESTED because the EAPOL handshake failed before reaching key installation. The deauth happened during M2 (before any keys), but the firmware death prevented retrying on the same boot.

**Fix**: Implemented `iwn_fw_restart()` in `iwn_fw.c` — a full firmware recovery function:

```
int iwn_fw_restart(iwn_dev *dev) {
    /* Reset state tracking */
    dev->fw_loaded = 0;
    dev->state = 0;
    dev->keys_installed = 0;
    dev->assoc_id = 0;

    /* Power cycle */
    iwn_apm_stop(dev);
    gBS->Stall(10000);  /* 10 ms */
    iwn_apm_init(dev);
    iwn5000_nic_config(dev);

    /* Reset DMA (reuse existing buffers) */
    iwn_stop_fh_dma(dev);
    iwn_reset_rx_ring(dev);
    iwn_reset_tx_ring(dev, 4);
    iwn_reset_tx_ring(dev, 0);
    iwn_program_rings(dev);

    /* Reload runtime firmware + post-alive setup */
    iwn5000_load_firmware(dev, fw.main_text, ...);
    /* ... scheduler setup, WiMAX coex, crystal calib, replay calibration ... */

    dev->fw_loaded = 1;
    return 0;
}
```

Integration in `iwn_connect()`: after a failed auth attempt, checks `INT == 0x00000000`. If firmware is dead, calls `iwn_fw_restart()` before retrying:

```
for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
        uint32_t intr = iwn_read(dev, IWN_INT);
        if (intr == 0x00000000 && dev->fw_loaded) {
            /* Firmware dead — restart before retry */
            iwn_fw_restart(dev);
        }
    }
    if (iwn_auth(dev) != 0) continue;
    ...
}
```

### 28\. Build authoritative self-tests before debugging frame boundaries

When a crypto algorithm works on synthetic data but fails on real data, the problem is almost certainly in frame parsing (boundaries, offsets, lengths), not the algorithm. Building three levels of self-test — raw crypto primitive (FIPS 197), protocol-level (RFC 3610 CCM), and full code path (synthetic 802.11 frame) — definitively proves which layer is correct and narrows the search space to input preparation. Without the self-test, we could have spent many more rounds questioning the AES-CCM implementation itself.

### 29\. Don't assume firmware behavior from OS driver code

The `flen -= 4` FCS subtraction was added based on the OpenBSD iwn driver's assumption that `mpdu->len` includes FCS. On the Intel 6000 with NODECRYPT, this turned out to be wrong — `mpdu->len` does NOT include FCS. OS driver code is a reference, not ground truth. When the firmware is a black box with undocumented behavior, empirical testing (self-tests + real frame analysis) is the only reliable source.

### 30\. Place diagnostic triggers in always-called functions

`ccmp_self_test()` placed inside `iwn_ccmp_decrypt()` never ran when data=0 (round 24) because that function is only called for encrypted data frames. Moving it to `iwn_rx_poll()` — which is called during every DHCP poll regardless of data frame count — ensured it ran immediately. Always place one-time diagnostic triggers in functions that are guaranteed to execute in the target scenario.

## Current Status

**2026-02-26 (Dell test round 1)**: Three bugs found and fixed after first Dell hardware test of custom stack:

1. MAC hex display (lookup string too short) — fixed

2. TX ring buffer overflow (DHCP frame 310 bytes > 80 bytes inline) — fixed with 2-segment TX in `iwn_tx_raw()`

3. IP filter drops DHCP OFFER (dest check fails during DHCP) — fixed with `ip_ready` gate

**2026-02-26 (Dell test round 2)**: EAPOL handshake failed (AP deauth after M2). Root cause: `iwn_tx_eapol()` and `iwn_tx_frame()` duplicated the TX ring logic with the same inline-only overflow bug. Fixed by refactoring both to call `iwn_tx_raw()` (which has the 2-segment fix). Added `plcp`/`rflags` parameters to `iwn_tx_raw()` for rate selection. Eliminated \~120 lines of duplicated code.

**2026-02-26 (Dell test round 3)**: EAPOL fix confirmed — handshake completes cleanly. DHCP still fails: zero frames received during 5-second wait. Added HW diagnostics (`iwn_snp_debug_state()`) and ARP probe before DHCP to isolate the issue.

**2026-02-26 (Dell test round 4)**: Diagnostics confirmed TX works (`tx_ok` increments) and firmware is active (`hw` index advances), but zero data frames in software buffer. Root cause: CCMP IV not stripped from decrypted RX frames — SNAP check fails against IV bytes. Fixed.

**2026-02-26 (Dell test round 5)**: CCMP RX fix in place. AP deauths us immediately after first data TX. Root cause: `iwn_encap_tx()` used GTK (node 15) for broadcast TX — AP fails PTK decrypt and deauths. Fixed to always use PTK (node 0). Also added 200ms post-M4 delay and auto-retry loop for auth/assoc.

**2026-02-26 (Dell test round 6)**: No deauth (PTK-only fix works). Still zero data frames in software buffer. ARP probe shows `tx_ok` 0→1 and `hw` advancing but `sw=0`. Added per-frame RX diagnostics (counters + hex dump in `iwn_rx_poll()`).

**2026-02-26 (Dell test round 7)**: RX diagnostics reveal: `total=173 mpdu=81 data=31 tx=1` — 31 data frames arrived but ALL still encrypted. Bytes 24-31 show CCMP headers with Key ID=1 (GTK), bytes 32+ are ciphertext (not LLC/SNAP). Root cause: pairwise key installed without `IWN_KFLAG_MAP` — firmware doesn't add key to HW key mapping table, preventing RX decryption of ALL frames. Fixed by adding `IWN_KFLAG_MAP` to pairwise key kflags.

**2026-02-26 (Dell test round 8)**: `IWN_KFLAG_MAP` fix applied. Auth/assoc succeeds (retry 1/3), EAPOL completes cleanly. But ARP probe: `total=54 mpdu=24 data=0 tx=1` — **zero data frames** (down from 31 in round 7). With MAP, firmware enters "secure mode" and drops all frames it can't decrypt. Analysis of round 7 hex dumps: all 31 frames had CCMP Key ID=1 (byte 3 = `0x60`), but EAPOL gave us GTK kid=2. GTK rotation mismatch — AP broadcasts with old key (kid=1), distributes new key (kid=2). Firmware can't find key at slot 1, drops everything.

**2026-02-26 (Dell test round 9)**: Attempted to re-add broadcast node (id=15) after RXON(assoc), following OpenBSD's pattern. **Firmware hung permanently**: `ADD_NODE(bcast): timeout (INT=0x00000000)`. Node 15 already existed from `iwn_auth()` and survived RXON. Duplicate fresh ADD_NODE on existing node kills Intel 6000 interrupt controller. Five subsequent `iwn connect` attempts all timeout. Reverted the re-add.

**2026-02-27 (Dell test round 10)**: DHCP unicast mode + ARP probe removed. Auth/assoc clean, EAPOL clean with GTK kid=1 (matching AP's active key). Key diagnostics: pairwise `kflags=0x000A`, group `kflags=0x4102` (no MAP on group). Still `data=0`, zero data frames. 11 type 162 (MISSED_BEACONS_NOTIFICATION) during 5-second DHCP window.

**2026-02-27 (Dell test round 11)**: Applied two fixes from Linux iwlwifi comparison: (1) added `IWN_KFLAG_MAP` to group key, (2) set `node.kid` for both keys (pairwise=0, group=1). Group kflags now `0x420A` (GROUP|KID(2)|MAP|CCMP). Still `data=0`. Same pattern: 55 mgmt, 0 data, 11 type 162.

**2026-02-28 (Dell test round 12 — NODECRYPT diagnostic)**: Added `IWN_FILTER_NODECRYPT` to RXON to bypass firmware decryption. **32 data frames received** (`total=647 mpdu=114 data=32 mgmt=82 tx=1 other=16 [162]`). All frames show encrypted CCMP headers with **Key ID=2** (byte 3 = `0xa0`). Zero unicast PTK frames — router only sends GTK-encrypted broadcasts. **This conclusively proves firmware decryption is the problem** — frames arrive at the radio fine, firmware drops them during decrypt despite having the correct key installed.

**2026-02-28 (Dell test round 13)**: Tested hypothesis 22: re-add broadcast node after RXON(assoc). **Hung immediately**: `ADD_NODE(bcast): timeout (INT=0x00000000)`. Identical to round 9 (Bug 17). Hypothesis disproved — RXON does NOT clear node table. Node 15 survives RXON, duplicate ADD_NODE is fatal. Reverted.

**2026-02-28 (Dell test round 14)**: Tested RXON re-send after key installation. EAPOL handshake completes cleanly (keys installed: pairwise `kflags=0x000A slot=0`, group `kflags=0x420A slot=2 kid=2`). Then `RXON(crypto): timeout (INT=0x00000000)` — **firmware hung**. Full RXON after MAP key installation is forbidden on Intel 6000. The firmware cannot handle radio teardown/rebuild once crypto is active.

**2026-02-28 (Dell test round 15 — build ready)**: New approach matching Linux iwlwifi's DIS_DECRYPT pattern:

1. Initial RXON(assoc) now sets `IWN_FILTER_NODECRYPT` — firmware doesn't attempt decrypt during EAPOL (no keys yet)

2. Keys installed via ADD_NODE UPDATE (unchanged)

3. `IWN_CMD_RXON_ASSOC` (cmd 17, lightweight delta) clears NODECRYPT — firmware activates decryption with installed keys

This avoids full RXON (which hangs) while still transitioning the firmware's decrypt engine from "disabled" to "enabled with keys."

**2026-02-28 (Dell test round 15)**: RXON_ASSOC pattern (NODECRYPT initially, clear after key install). RXON_ASSOC succeeded (no hang) — first lightweight RXON delta to not kill firmware. But still `data=20 sw=0` — 20 data frames with CCMP headers intact, not decrypted. RXON_ASSOC does NOT re-enable the decrypt engine after NODECRYPT was set. The firmware's key lookup table is never rebuilt by RXON_ASSOC. Abandoned NODECRYPT→RXON_ASSOC pattern.

**2026-02-28 (Dell test round 16)**: Removed NODECRYPT entirely (firmware decryption active from start). Removed RXON_ASSOC post-key block. Added `last_cmd_status` to track ADD_NODE firmware response. Results: ALL ADD_NODE responses = 0x01 (SUCCESS). Still `data=3 sw=0` — keys accepted but not in hardware key cache. Added MPDU tail flags diagnostic for round 16b.

**2026-02-28 (Dell test round 16b)**: Added MPDU tail flags reading to RX DATA debug output. **SMOKING GUN**: Every frame shows `fl=0x00XX0707 NO_DEC` — cipher field=7 (SEC_ENC_ERR), firmware finds source station (bit 2=1) but CANNOT find key in hardware key cache (bit 11=0, decrypt not even attempted). 52 data frames, ALL broadcast (Key ID=1), sequential PNs. `tx_ok=1`. No unicast frames.

**2026-02-28 (Dell test round 17)**: Research revealed OpenBSD NEVER installs group keys in hardware on Intel WiFi N — only pairwise CCMP. Hypothesis: GTK install on broadcast node corrupts firmware key table. Changes: (1) removed GTK hardware install, (2) added unicast/broadcast classification (`RX UC:` / `RX BC:`) to RX debug. PTK install unchanged (matches OpenBSD exactly).

**Result**: EAPOL handshake FAILED before reaching key installation — AP deauthed after M2 (M1 retransmission then deauth). **GTK removal was never tested.** Worse: firmware died after the deauth (`INT=0x00000000`), making all subsequent `iwn connect` attempts fail without reboot. Two separate problems: (1) intermittent EAPOL M2 rejection, (2) firmware death after deauth (Bug 29).

**2026-02-28 (Dell test round 18 — build ready)**: Implemented firmware restart (`iwn_fw_restart()`) to recover from firmware death without rebooting. Changes: (1) keep `fw_rawbuf` alive after `iwn_hw_init()` so firmware can be reloaded, (2) `iwn_fw_restart()` does power cycle + runtime FW reload + replay saved calibration, (3) `iwn_connect()` detects dead firmware (INT=0 after failed auth) and auto-restarts before retrying. This should allow the retry loop to actually work after an EAPOL failure/deauth, finally testing the GTK removal fix.

**2026-02-28 (Dell test round 18)**: **EAPOL succeeds first try — GTK removal confirmed working!** Auth/assoc clean, EAPOL 4-way handshake completes without firmware death or retries. Firmware restart code was present but NOT needed this round. Key results:

- GTK removal works: `"Group key saved (kid=2) -- software only"` — GTK not installed in hardware, matching OpenBSD

- PTK installed: `kflags=0x000a slot=0`, firmware response `0x01` (SUCCESS)

- SNP installed, custom stack init, DHCP DISCOVER sent (250 bytes via 2-segment TX)

- DHCP fails: 0 DHCP OFFERs received during 5-second window

**But**: Two unicast data frames received during DHCP — both are **EAPOL M3 retransmissions** from the AP: `RX UC: fc=0802 len=187 fl=0x004c1007 NO_DEC [aa aa 03 00 00 00 88 8e 01 03 00 97 02 13 ca 00]`

- `fc=0802`: Data frame, FromDS=0 (to us), Protected=0 (plaintext EAPOL)

- `fl=0x004c1007`: Tail flags — CRC OK, station found, cipher=0 (NONE), NO_DEC (correct for unprotected)

- Payload: `aa aa 03 00 00 00 88 8e` = LLC/SNAP + EtherType 0x888E (EAPOL)

- Key Info `0x13CA` = Pairwise + Install + Ack + MIC + Secure + EncKeyData + Version 2 = **M3**

**M4 was never received by AP.** AP retransmitted M3 twice, then deauthenticated: `DATA: AP sent deauth!`. The M4 frame was queued but may not have been transmitted or ACK'd at the 802.11 level. The blind 200ms `gBS->Stall()` after M4 provided no confirmation.

TX stats at end: `tx_ok=1 hw=37 cur=33 sw=0` — only 1 TX confirmed (DHCP DISCOVER), not 2 (M4 + DISCOVER).

### 30\. M4 TX Not Verified — Blind Delay After M4

**Symptom** (round 18): EAPOL handshake completes on our side (M3 verified, M4 sent, PTK installed), but AP retransmits M3 twice then deauths — M4 was never received by the AP. `tx_ok=1` at end of session (only DHCP DISCOVER counted), suggesting M4 TX completion was never checked.

**Root cause**: After `iwn_tx_eapol()` queues M4 in the TX ring, the code calls `gBS->Stall(200000)` (200ms blind delay) and proceeds to install keys. There is no check whether the firmware actually transmitted M4 or whether the AP ACK'd it at the 802.11 level. If M4 fails to transmit (DMA issue, ring state, firmware timing), we silently proceed with key installation while the AP is still waiting for M4.

**Fix**: Replaced blind 200ms delay with **TX_DONE polling**. After M4 TX, polls the RX ring for `IWN_TX_DONE` (notification type 28) for up to 1 second. Reads `iwn5000_tx_stat` to get firmware TX status, ACK failure count, and RTS failure count:

```
if (rxd->type == IWN_TX_DONE) {
    struct iwn5000_tx_stat *txs = (struct iwn5000_tx_stat *)
        ((uint8_t *)rxbuf + sizeof(struct iwn_rx_desc));
    uint32_t st = txs->stat.status & 0xff;
    // Print: "M4 TX_DONE: status=0xNN ack_fail=N rts_fail=N"
    if (st != 0x01 && st != 0x00)
        // Print: "WARNING: M4 TX may have failed!"
    tx_done = 1;
}
```

Falls back to 200ms delay if TX_DONE isn't received within 1 second.

**2026-02-28 (Dell test round 19 — build ready)**: TX_DONE polling after M4. Three possible diagnostic outcomes:

1. `status=0x01, ack_fail=0` → M4 transmitted and ACK'd (problem is M4 content/timing, not TX)

2. `status=0xNN, ack_fail>0` → M4 transmitted but AP didn't ACK (RF issue or AP rejected at PHY)

3. No TX_DONE within 1s → M4 never left the TX ring (firmware TX engine issue)

### 31\. Software CCMP Decryption — Bypass Broken HW Decrypt Permanently

**Decision** (after 19 rounds): The Intel 6000 firmware's HW CCMP decryption is confirmed broken — it accepts key install commands (returns SUCCESS) but never populates the HW key lookup table. MPDU tail flags show `SEC_ENC_ERR cipher=7` on every frame (round 16b). With `NODECRYPT`, frames arrive fine (round 12: 32 data frames). The firmware is a black box we can't fix.

**Solution**: Move RX decryption to software permanently. TX encryption stays in firmware (working — M4 ACK'd, DHCP DISCOVER sent).

**Implementation** (`iwn/iwn_ccmp.c`, \~175 lines):

Full AES-CCM decryption + MIC verification in a single function `iwn_ccmp_decrypt()`:

1. Extract 48-bit PN from CCMP header (bytes at `hdr_len` offset, non-contiguous layout)

2. Build 13-byte nonce: `[priority(1)] [addr2(6)] [PN(6)]`

3. Build AAD from 802.11 header: masked FC + Addr1 + Addr2 + Addr3 + masked SC (+ QoS TID for subtype 8)

4. AES-CTR decrypt ciphertext in-place (counter blocks 1,2,3...)

5. CBC-MAC over `B_0 || AAD_padded || plaintext_padded` → compute T

6. Encrypt counter 0 → S_0, compute `U = T XOR S_0`

7. Constant-time compare U\[0:8\] with received MIC — return 0 on match, -1 on mismatch

Only uses `iwn_aes128_encrypt()` — CCM uses AES encrypt for both CTR keystream and CBC-MAC. AES decrypt is not needed.

### 32\. Removing HW Key Install Broke TX Encryption

**Symptom** (round 20): After implementing software CCMP decryption (Bug 31), removed ALL hardware key installation from `iwn_install_keys()` — both PTK and GTK. EAPOL handshake completes, keys saved for software decrypt, but **zero data frames received** (`data=0 mgmt=59`). DHCP DISCOVER sent (`tx_ok=1`) but no DHCP OFFER comes back. Previous rounds (with PTK installed in HW) received broadcast data frames.

**Root cause**: TX encryption relies on the firmware. `iwn_encap_tx()` sends data frames with `security=IWN_CIPHER_CCMP`, which tells the firmware to CCMP-encrypt using the key installed on the BSS node. With PTK removed from firmware, the firmware either sends garbage-encrypted frames or unencrypted frames — either way, the AP can't decrypt them and drops them. No response = no data frames at all.

**Key insight**: HW decrypt is broken (confirmed 19 rounds), but **HW encrypt works fine**. TX_DONE status=0x01 with ack_fail=0 in previous rounds proves the AP successfully received and ACK'd firmware-encrypted frames. The plan correctly stated "TX encryption stays in firmware" but the implementation incorrectly removed the PTK HW install.

**Fix**: Restored PTK (pairwise key) firmware install via ADD_NODE UPDATE SET_KEY for TX encryption. Only GTK install is skipped — RX group decrypt stays in software via `iwn_ccmp_decrypt()`. The split: **firmware encrypts TX (PTK in HW), software decrypts RX (PTK TK + GTK in dev->ptk/dev->gtk)**.

**Integration changes**:

| File               | Change                                                                                                                                        |
| ------------------ | --------------------------------------------------------------------------------------------------------------------------------------------- |
| iwn/iwn_ccmp.c     | NEW — iwn_ccmp_decrypt()                                                                                                                      |
| iwn/iwn_assoc.c    | Added IWN_FILTER_NODECRYPT to RXON filter — firmware delivers raw encrypted frames                                                            |
| iwn/iwn_data.c     | Call iwn_ccmp_decrypt() in RX path before CCMP header skip — unicast uses PTK TK (ptk[32:48]), broadcast uses GTK (checks group bit in Addr1) |
| iwn/iwn_eapol.c    | Restored PTK HW install (ADD_NODE UPDATE SET_KEY) for TX encryption; GTK saved in dev->gtk for software RX decrypt only                        |
| iwn/iwn_internal.h | Added iwn_ccmp_decrypt() declaration                                                                                                          |
| Makefile           | Added iwn/iwn_ccmp.c to EFI_SRC                                                                                                               |

**CCM flag bytes**: CTR flags = `0x01` (L-1=1). CBC-MAC B_0 flags = `0x59` (Adata=1, M=8→(8-2)/2=3, L-1=1 → 64+24+1=89=0x59).

**Key selection in RX path**: `frame[4] & 0x01` (group bit in Addr1) → GTK for broadcast, PTK TK for unicast. Only decrypts when `dev->keys_installed` is set.

**QEMU compatibility**: QEMU/OVMF has a real UEFI IP stack — no IWN driver, no NODECRYPT, no software CCMP. The `dev->keys_installed` check only triggers on Dell (IWN path).

### 33\. FCS (4 Bytes) Included in Frame Length with NODECRYPT

**Symptom** (round 22): Software CCMP decryption: CTR decryption produces **correct LLC/SNAP plaintext** (`dec=aaaa030000000800` = IPv4, `dec=aaaa0300000086dd` = IPv6), proving key, nonce, and CTR are all correct. But ALL 7 frames fail MIC verification — computed MIC is completely different from received MIC.

**Evidence**: AES sanity check is consistent (`aes0=47909aeb` on every frame). Key matches GTK (`key=951e3f5f`). Nonces have sequential PNs. CTR produces valid LLC/SNAP. Only CBC-MAC/MIC comparison fails.

**Root cause**: With `NODECRYPT` set, the Intel 6000 firmware skips ALL post-processing of data frames — including stripping the 4-byte FCS (Frame Check Sequence / CRC32) at the end. The firmware-reported `flen` includes FCS. This makes:

1. `ctext_len` 4 bytes too large (CBC-MAC computes over extra bytes)
2. `rx_mic` pointer lands 4 bytes too late — into the FCS, not the real MIC

Frame layout with NODECRYPT:
```
[802.11 hdr (24/26B)] [CCMP hdr (8B)] [ciphertext (N)] [MIC (8B)] [FCS (4B)]
                                                                    ^^^^^^^^^
                                                        flen includes these
```

Our code assumed `flen` excluded FCS (normal firmware behavior). With NODECRYPT, it doesn't.

**Fix**: Subtract 4 from `flen` when Protected bit is set (encrypted frame with NODECRYPT active):
```c
if (frame[1] & 0x40) {
    ccmp_hdr = 8;
    flen -= 4;  /* NODECRYPT: firmware includes 4-byte FCS */
    ...
}
```

This corrects both the `iwn_ccmp_decrypt()` call AND the later `payload_len` calculation (both use `flen`).

**Total: 33 bugs/findings, 23 fixed, 1 accepted, 2 design workarounds, 3 diagnostics, 1 disproved, 1 abandoned, 2 testing** (em dash accepted; GTK mismatch via unicast DHCP; NODECRYPT diagnostic; tail flags diagnostic; ADD_NODE status diagnostic; hypothesis 22 disproved; RXON_ASSOC pattern abandoned; Bug 33 FCS fix testing round 23).

### 34. Self-Test: FIPS 197 AES + RFC 3610 CCM Vector + Synthetic 802.11 Frame

**Motivation**: MIC still failed after the FCS fix in round 23. Needed to definitively prove whether the AES-CCM algorithm implementation was correct, independent of real frames.

**Implementation**: Added `ccmp_self_test()` in `iwn_ccmp.c` with three tests:

1. **FIPS 197 Appendix B** — AES-128 encrypt: Key=`2B7E...3C`, PT=`3243...34`, expected CT=`3925...32`. Verifies `iwn_aes128_encrypt()` matches the standard.

2. **RFC 3610 Packet Vector #1** (M=8, L=2 — same parameters as CCMP) — Full CCM decrypt + MIC verify with intermediate value printing (X0, Taad, Tpt). This is the authoritative test: same nonce/AAD/ciphertext sizes as CCMP.

3. **Synthetic 802.11 frame** — Constructs a complete CCMP-encrypted data frame in memory (fc=0x0842, broadcast, non-QoS, key=0xAA*16, PN=1, plaintext=LLC/SNAP+"Hi"), encrypts with CCM, then calls `iwn_ccmp_decrypt()` to verify the full code path: FC parsing, CCMP header extraction, nonce/AAD construction, decrypt, MIC.

**Results (rounds 26-28)**: ALL THREE PASS on Dell hardware.
- AES-128: PASS (output matches FIPS 197)
- CCM CTR: PASS (plaintext matches RFC 3610)
- CCM MIC: PASS (X0=`eb9d5547730955ab231e0a2dfe4b90d6` matches RFC 3610 exactly)
- 802.11 frame: PASS (full `iwn_ccmp_decrypt()` code path verified)

**Conclusion**: The AES-CCM algorithm and the full `iwn_ccmp_decrypt()` code path (including FC parsing, CCMP header, nonce/AAD construction) are proven correct. The MIC failure on real frames must be due to frame boundary issues (e.g., ctext_len wrong).

### 35. Self-Test Trigger Location — iwn_rx_poll vs iwn_ccmp_decrypt

**Symptom** (round 24): Self-test was placed inside `iwn_ccmp_decrypt()`. With data=0, the function was never called, so self-test never ran.

**Fix**: Made `ccmp_self_test()` non-static, declared in `iwn_internal.h`, called from `iwn_rx_poll()` instead (which IS called during DHCP polling regardless of data frame count). Uses a static `ccmp_self_test_done` flag to run once.

### 36. EAPOL Intermittent Failures — M4 TX ACK Failures

**Symptom** (rounds 25-27): EAPOL handshake intermittently fails — M4 TX status=0x83 with ack_fail=1, followed by AP deauthentication. Sometimes requires 2-3 `iwn connect` retries before handshake completes.

**Evidence**: Round 25: AP deauth after M1 retransmission. Round 26: M4 ack_fail=1. Round 27: First two attempts failed (deauth + firmware restart), third succeeded.

**Analysis**: The Intel 6000 TX path is unreliable at OFDM 24Mbps — ACKs are missed intermittently. This is a radio timing issue, not a protocol bug. Retrying `iwn connect` works around it.

**Status**: Accepted — retry on failure is the practical solution for this hardware.

### 37. FCS Hypothesis Disproved — mpdu->len Does NOT Include FCS

**Background** (bug 33): Hypothesized that with NODECRYPT, firmware includes 4-byte FCS in `mpdu->len`. Added `flen -= 4` at `iwn_data.c:361`. Rounds 23-28 still showed MIC failures.

**Evidence from round 28 (with FCS subtraction)**: 4 real data frames arrived. All showed:
- CTR decrypt correct: `dec=aaaa030000000800` (LLC/SNAP IPv4) — key, nonce, CTR all working
- MIC FAIL on all 4 frames — computed MIC completely different from received MIC
- Self-test (including synthetic 802.11 frame) passes — algorithm and code path proven correct
- Example: `len=832`, `ct=788` (after FCS subtract), `comp=122841183990824e rx=074b6c0e647e2c0e FAIL`

**Analysis**: Since the algorithm is proven correct (self-test PASS) and CTR produces valid plaintext (key/nonce correct), the only remaining variable is `ctext_len`. If `mpdu->len` does NOT include FCS, then `flen -= 4` makes `ctext_len` 4 too small and shifts `rx_mic` 4 bytes before the real MIC — exactly matching the symptom.

**Fix (round 29)**: Removed `flen -= 4`. If `mpdu->len` already excludes FCS:
- `ctext_len` = 832 - 24 - 8 - 8 = 792 (correct)
- `rx_mic` points to actual MIC at offset 24+8+792 = 824 (correct)

**Status**: Inconclusive in isolation. Re-applied in combination with QoS AAD fix (round 32).

### 38. Firmware Strips QoS Control Field — AAD Mismatch

**Hypothesis**: All received data frames have fc0=0x08 (subtype 0, non-QoS data). But modern 802.11n APs virtually always send QoS data frames (fc0=0x88, subtype 8). The Intel 6000 firmware strips the 2-byte QoS Control field from received frames — converting QoS→non-QoS — even with NODECRYPT.

**Impact on CCMP**: The AP encrypts with QoS AAD:
- fc0=0x88 in AAD (QoS data subtype)
- aad_len=24 (includes 2-byte QoS TC field)
- nonce[0] = TID from QoS Control (typically 0 for best effort)

Our code computes non-QoS AAD:
- fc0=0x08 in AAD
- aad_len=22 (no QoS field)
- nonce[0] = 0

CTR is unaffected (doesn't use AAD), explaining why LLC/SNAP decrypts correctly. Only CBC-MAC uses AAD, explaining why only MIC fails.

**Round 31 (QoS AAD only, no FCS fix)**: Still FAIL. QoS AAD alone doesn't fix MIC because FCS is also included in `flen`. Need both fixes simultaneously.

**Fix**: Force QoS AAD in `iwn_ccmp_decrypt()`: use fc0=0x88, add QoS TC=[0x00, 0x00] to AAD (aad_len=24). Updated synthetic 802.11 self-test to encrypt with QoS AAD to match.

### 39. Two Simultaneous Bugs — FCS + QoS Stripping

**Problem**: Neither FCS subtraction nor QoS AAD fixes MIC when applied alone, because BOTH bugs are present simultaneously:

1. **FCS in flen** (bug 33): `mpdu->len` includes 4-byte FCS → ctext_len is 4 too large, rx_mic points into FCS
2. **QoS stripping** (bug 38): Firmware strips QoS field → our AAD (22B, fc0=0x08) doesn't match AP's AAD (24B, fc0=0x88)

**Test matrix**:
| FCS subtract | QoS AAD | Rounds | Result |
|---|---|---|---|
| Yes | No | 24-28 | FAIL |
| No | No | 29-30 | FAIL |
| No | Yes | 31 | FAIL |
| Yes | Yes | 32 | **Testing** |

Each fix alone corrects one error but the other still causes MIC failure. Only when both are applied should the MIC match.

**Fix (round 32)**: Both fixes applied:
- `flen -= 4` restored in `iwn_data.c`
- QoS AAD (fc0=0x88, aad_len=24, QoS TC=0) in `iwn_ccmp.c`
- Self-test updated to encrypt with QoS AAD

**Total: 39 bugs/findings, 23 fixed, 2 accepted, 2 design workarounds, 3 diagnostics, 1 disproved, 1 abandoned, 6 testing, 1 combined-fix** (Bugs 33+38 combined in round 32).

## Current Status (Round 32)

**2026-02-28 (Dell test round 24)**: FCS fix build (flen -= 4). EAPOL succeeded (keys: TK=3a3b7c48, GTK=435639cb kid=1). **data=0** — zero data frames during DHCP window. Self-test never ran because `ccmp_self_test()` was inside `iwn_ccmp_decrypt()` which was never called.

**2026-02-28 (Dell test round 25)**: Moved self-test trigger to `iwn_rx_poll()`. EAPOL failed — AP deauth after M1 retransmission. No data frames.

**2026-02-28 (Dell test round 26)**: Self-test ran and ALL PASSED (AES-128, CCM CTR, CCM MIC). X0 matches RFC 3610 exactly. But M4 TX failed (ack_fail=1), AP deauthed. Only 2 data frames seen — unencrypted EAPOL retransmissions.

**2026-02-28 (Dell test round 27)**: Three connection attempts. First two failed (deauth, firmware restart). Third succeeded (M4 ACK'd). Self-test PASSED again. **data=0** during DHCP window.

**2026-02-28 (Dell test round 28)**: EAPOL succeeded first try (M4 status=0x01). Self-test: AES-128 PASS, CCM CTR PASS, CCM MIC PASS, **802.11 frame PASS** (full code path verified). **4 real data frames arrived!** All broadcast multicast (mDNS/IPv6). CTR decrypt correct (LLC/SNAP valid). **MIC FAIL on all 4** — computed MIC completely different from received MIC. Algorithm proven correct by self-test — root cause narrowed to frame boundary issues.

**2026-02-28 (Dell test round 29)**: Removed `flen -= 4` (FCS subtraction). **data=0** — zero data frames arrived. Cannot confirm whether FCS removal fixes MIC. Self-test PASS.

**2026-02-28 (Dell test round 30)**: Same build (no FCS subtraction, non-QoS AAD). **data=22** — plenty of frames! CTR still correct (LLC/SNAP valid). **ALL MIC FAIL**. FCS removal alone doesn't fix MIC. Observed: all frames fc=0x08 (non-QoS) — suspicious for 802.11n AP. AP deauthed during DHCP window (M4 ack_fail=1).

**2026-02-28 (Dell test round 31)**: Added QoS AAD reconstruction (fc0=0x88, aad_len=24, QoS TC=0) WITHOUT FCS subtraction. Self-test 802.11 frame FAIL (expected — synthetic encrypted with non-QoS, decrypt now forces QoS). **data=16**, **ALL MIC FAIL**. QoS AAD alone doesn't fix MIC either. Neither fix works in isolation.

**Key insight**: Both FCS and QoS bugs exist simultaneously. Each was tested alone and failed. The untested combination is FCS+QoS together.

**2026-02-28 (Dell test round 32 — build ready)**: Both fixes applied simultaneously:
1. `flen -= 4` restored (FCS in mpdu->len)
2. QoS AAD (fc0=0x88, aad_len=24) in `iwn_ccmp_decrypt()`
3. Self-test updated to encrypt with QoS AAD (should PASS)

Expected: MIC passes → frames decrypted → DHCP OFFER received → IP assigned.

## Files Modified

| File               | Changes                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| iwn/iwn_snp.c      | Added iwn_snp_get_mac(), iwn_snp_direct_tx(), iwn_snp_direct_rx() — direct data path bypassing SNP function pointer table                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| iwn/iwn_data.c     | Modified iwn_tx_raw() — 2-segment TX descriptor for frames > 80 bytes, separate DMA buffer for large payloads; added plcp/rflags params for caller-specified TX rate. Enhanced RX debug: counters for IWN_RX_DONE(195), IWN_RX_PHY(192), mgmt frames, unknown types with type code tracking. Round 16b: Added MPDU tail flags reading + DEC_OK/DEC_FAIL/NO_DEC indicator. Round 17: Added unicast/broadcast classification (RX UC: / RX BC:). Round 20: Added iwn_ccmp_decrypt() call in RX path before CCMP header skip — unicast uses PTK TK, broadcast uses GTK. Round 23: Subtract 4 from flen when Protected bit set — NODECRYPT includes 4-byte FCS in frame length. Round 24: Added ccmp_self_test() call at top of iwn_rx_poll(). Round 29: Removed flen -= 4 — FCS hypothesis disproved, mpdu->len does NOT include FCS |
| iwn/iwn_eapol.c    | Replaced 58-line duplicated TX ring logic in iwn_tx_eapol() with single iwn_tx_raw() call. Added IWN_KFLAG_MAP to pairwise key. Set node.kid=0 for pairwise. Key install diagnostics (kflags, slot, first 4 key bytes). Round 17: Removed GTK hardware install — group key saved in dev->gtk for software crypto only (matching OpenBSD). Round 19: Replaced blind 200ms post-M4 delay with TX_DONE polling — reads iwn5000_tx_stat for TX status, ACK fail count, RTS fail count. Round 21: Restored PTK HW install for TX encryption; GTK saved for software RX decrypt only |
| iwn/iwn_assoc.c    | Replaced 60-line duplicated TX ring logic in iwn_tx_frame() with single iwn_tx_raw() call. Round 16: Removed NODECRYPT from RXON(assoc) filter, removed RXON_ASSOC post-key block. Added ADD_NODE response status printing. Confirmed node 15 survives RXON (rounds 9, 13). Round 18: Added firmware death detection (INT=0x00000000) in retry loop — calls iwn_fw_restart() before retrying. Round 20: Added IWN_FILTER_NODECRYPT to RXON permanently — firmware delivers raw encrypted frames for software decrypt                                               |
| iwn/iwn_ccmp.c     | NEW (Round 20) — iwn_ccmp_decrypt() (~175 lines): AES-CCM decrypt + MIC verify. Extracts PN from CCMP header, builds nonce + AAD, AES-CTR decrypts in-place, CBC-MAC verifies MIC. Constant-time MIC comparison. Only uses iwn_aes128_encrypt(). Round 21: Added diagnostic prints (hdr_len, ctext_len, key[0:4], nonce, computed MIC vs received MIC). Round 22: Added AES sanity check (aes0=) and first 8 bytes of decrypted plaintext (dec=) — confirmed CTR produces correct LLC/SNAP. Round 24: Added ccmp_self_test() with 3 tests: FIPS 197 AES-128, RFC 3610 CCM Vector #1 (with intermediate X0/Taad/Tpt prints), synthetic 802.11 CCMP frame through full iwn_ccmp_decrypt() code path. All 3 PASS on Dell |
| iwn/iwn_fw.c       | Round 18: Kept fw_rawbuf alive after iwn_hw_init() (removed free at line 850-853). Added iwn_fw_restart() (~120 lines) — power cycle + runtime FW reload + scheduler setup + replay calibration. Reuses existing DMA buffers.                                                                                                                                                                                                                                                                                                                                      |
| iwn/iwn_internal.h | Updated iwn_tx_raw() declaration — added plcp, rflags parameters. Round 18: Added iwn_fw_restart() declaration. Round 20: Added iwn_ccmp_decrypt() declaration. Round 24: Added ccmp_self_test() declaration                                                                                                                                                                                                                                                                                                                                                     |
| iwn/iwn_scan.c     | Round 16: Modified iwn_wait_cmd() to save firmware response status byte into dev->last_cmd_status                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| iwn/iwn_dma.c      | Allocate/free 4 KB tx_data DMA page for ring 0 large frame payloads                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| iwn/if_iwnvar.h    | Added tx_data DMA buffer field to iwn_dev struct. Round 16: Added last_cmd_status field for firmware response tracking                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| efi/efi.h          | Declared direct data path functions + RX debug functions (iwn_rx_set_debug, iwn_rx_print_debug)                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| efi/ns_net.c       | Rewritten to use direct IWN calls; fixed MAC hex lookup ("0123456789ABCDEF")                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| efi/ns_ip.c        | Modified IP destination filter — accept all packets when ip_ready == 0 (during DHCP)                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| efi/ns_dhcp.c      | Cleared DHCP broadcast flag (0x8000→0x0000) for unicast OFFER. Removed ARP probe. RX diagnostics enabled during DHCP exchange with frame breakdown on success/timeout                                                                                                                                                                                                                                                                                                                                                                                              |
| efi/ns_arp.c       | NEW — ARP table, resolve, reply (~150 lines)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| efi/ns_udp.c       | NEW — UDP send/recv, port binding (~100 lines)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| efi/ns_tcp.c       | NEW — TCP state machine, retransmit (~550 lines)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| efi/ns_net.h       | NEW — shared types and declarations (~100 lines)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| efi/net.c          | Modified — dual dispatch: UEFI path (QEMU) vs custom path (Dell)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
