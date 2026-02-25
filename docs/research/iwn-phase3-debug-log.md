# IWN Phase 3: WiFi Scanning Debug Log

## Hardware

- **Machine**: Dell E6510
- **WiFi card**: Intel Centrino Advanced-N 6200 (PCI `8086:422C`)
- **Chip family**: Intel 6000 series ("Puma Peak")
- **Firmware**: `iwn-6000` (legacy v2, API 4, 454 KB)
- **Chain masks**: `txchainmask = rxchainmask = IWN_ANT_BC` (antennas B+C, ROM override per OpenBSD)

## Starting Point

Phase 2 complete: firmware loads, calibration works (1 result, 508 bytes), runtime firmware alive (UC_READY received). Hardware is initialized and ready to accept commands on TX ring 4.

## Debug Timeline

### 1. Fire-and-Forget Commands Cause SW_ERR

**Symptom**: Sending RXON immediately after BT_COEX caused `IWN_INT_SW_ERR` (bit 25). Firmware acknowledged the first command but crashed processing the second.

**Root cause**: All config commands were fire-and-forget — `iwn_cmd()` sent the command but didn't wait for the firmware to finish processing it before sending the next one. The firmware can't handle overlapping commands on the same TX ring.

**Fix**: Implemented `iwn_wait_cmd()` — polls the RX ring for a response notification matching the command code. Each command now waits for its response before the next command is sent.

```
iwn_cmd(dev, CMD, payload, len);
iwn_wait_cmd(dev, CMD, "CMD_NAME", 200);  // 200ms timeout
```

The wait function drains all RX notifications until it finds one with `desc->type == cmd_code`, clearing interrupt and FH status registers each iteration to keep DMA flowing.

### 2. BT_COEX_PRIOTABLE Crashes Firmware

**Symptom**: After BT_COEX succeeded, `BT_COEX_PRIOTABLE` (cmd 204) caused firmware crash (`INT=0x8a000000` — SW_ERR + FH_TX).

**Root cause**: Advanced BT coexistence commands (`BT_COEX_PRIOTABLE` cmd 204, `BT_COEX_PROT` cmd 205) are for 6005/6050 series only. The plain 6000 series firmware doesn't implement them. OpenBSD gates these behind `IWN_FLAG_ADV_BT_COEX`, which is NOT set for 6000.

**Fix**: Removed PRIOTABLE and PROT commands entirely. Only send basic `BT_COEX` (cmd 155).

### 3. Advanced BT_COEX Struct Crashes Firmware

**Symptom**: After removing PRIOTABLE/PROT, `BT_COEX` itself now succeeded but the NEXT command (TX_ANT) crashed the firmware. Same `INT=0x8a000000`.

**Root cause**: We were using the advanced `iwn6000_btcoex_config` struct (~80 bytes with lookup tables) for cmd 155. The plain 6000 series expects the simple `iwn_bluetooth` struct (12 bytes). Firmware acknowledged receipt of the oversized data but crashed while processing it.

**Fix**: Replaced with the simple `iwn_bluetooth` struct matching OpenBSD's `iwn_send_btcoex()`:

```c
struct iwn_bluetooth bt;
iwn_memset(&bt, 0, sizeof(bt));
bt.flags = IWN_BT_COEX_CHAN_ANN | IWN_BT_COEX_BT_PRIO;
bt.lead_time = IWN_BT_LEAD_TIME_DEF;
bt.max_kill = IWN_BT_MAX_KILL_DEF;
```

**Note**: The Intel 6200 is WiFi-only (no Bluetooth), so BT coex is a formality. But the firmware requires it before RXON. The simple struct with basic flags satisfies this requirement without causing issues.

### 4. All Config Commands Succeed — Beacons Streaming

**Result after fixes 1-3**: All 4 initial config commands (BT_COEX, TX_ANT, RXON, ADD_NODE) succeed. The RX ring shows `rx[192]` and `rx[193]` interleaved during command waits — these are `IWN_RX_PHY` and `IWN_MPDU_RX_DONE`, meaning the radio is receiving beacons from nearby access points. First RF activity!

**Problem**: After all config commands, the SCAN command (cmd 128) produced zero notifications. Scan timeout after 8 seconds.

### 5. FH_INT Not Cleared — DMA Stalls

**Symptom**: During scan polling, beacons appeared during command waits (iwn_wait_cmd was clearing interrupts) but ZERO notifications appeared during the 8-second scan poll.

**Root cause**: The Flow Handler interrupt status register (`IWN_FH_INT`, offset 0x010) was not being cleared. OpenBSD clears BOTH `IWN_INT` and `IWN_FH_INT` in every ISR iteration. Without clearing FH_INT, the Flow Handler stops DMA'ing new notifications to the RX ring.

**Fix**: Added `iwn_write(dev, IWN_FH_INT, 0xffffffff)` to all polling loops: `iwn_wait_cmd()`, `iwn_poll_scan_results()`, and `iwn_drain_rx()`.

### 6. Missing Config Commands — No TX Power

**Symptom**: After FH_INT fix, more beacon notifications appeared during config, but SCAN still produced zero notifications. INT register was clean (0x00000000) — no error, firmware just silently ignored the scan.

**Root cause**: OpenBSD sends 8 commands before scan. We were sending only 4. Missing commands:

| # | Command | Code | Purpose | Impact of Missing |
|---|---------|------|---------|-------------------|
| 5 | LINK_QUALITY | 78 | TX rate table for broadcast node | Firmware doesn't know what rate to use for probe requests |
| 6 | TXPOWER_DBM | 149 | Set TX power level | **Firmware can't transmit at all** |
| 7 | SET_CRITICAL_TEMP | 164 | Temperature safety threshold | Firmware may refuse to enable radio |

**Fix**: Added all three commands in the correct order (after ADD_NODE, before POWER):

```
Command sequence (final):
  1. BT_COEX (155)         — Bluetooth coexistence
  2. TX_ANT_CONFIG (152)   — TX antenna chain mask
  3. RXON (16)             — Radio on: STA mode, 2.4 GHz, ch1
  4. ADD_NODE (24)         — Broadcast node (ID 15, FF:FF:FF:FF:FF:FF)
  5. LINK_QUALITY (78)     — CCK 1Mbps rate table, 16 retries
  6. TXPOWER_DBM (149)     — 16 dBm TX power
  7. SET_CRITICAL_TEMP (164) — 110°C threshold
  8. SET_POWER_MODE (119)  — CAM (continuously awake)
```

All 8 commands now succeed on hardware. But scan still times out.

### 7. Big Command DMA — Three Approaches, All Identical

**Symptom**: Scan command (912 bytes, cmd 128) never produces START_SCAN (130) notification. All 8 config commands work. Beacons stream during config. Clean INT (0x00000000) after scan timeout.

**Investigation**: The scan command is ~912 bytes but `iwn_tx_cmd.data[]` is only 136 bytes. The big command path uses TX descriptors to point firmware at the payload. Three approaches tested:

| Approach | Description | Result |
|----------|-------------|--------|
| 2-segment | Seg0 = 4-byte header in ring, Seg1 = payload in separate DMA page | Scan timeout |
| 1-segment (separate DMA) | Header + payload in one contiguous DMA allocation | Scan timeout |
| Ring buffer | Write payload directly into ring's cmd buffer (known-good DMA memory) | Scan timeout |

**Conclusion**: All three fail identically with the same behavior. **DMA delivery is NOT the issue.** The firmware receives the scan command (no error) but silently ignores it. The problem must be in the scan command content itself.

### 8. Scan Command Content — Multiple Differences from OpenBSD

**Root cause found**: Side-by-side comparison of our `iwn_send_scan()` with OpenBSD's `iwn_scan()` revealed multiple content errors:

#### Critical: Missing 802.11 Probe Request Frame Header

OpenBSD builds a **complete 802.11 management frame** (24-byte MAC header + IEs) as the probe request body. We were sending only the raw Information Elements (Supported Rates + Extended Rates) without any frame header.

The firmware expects a fully-formed probe request frame — it fills in Duration and Sequence Control but needs the Frame Control, DA, SA, and BSSID fields to be present. Without the frame header, the firmware has no valid frame to transmit and silently drops the scan.

```
OpenBSD probe body layout:
  [24 bytes] ieee80211_frame header (FC=probe_req, DA=bcast, SA=MAC, BSSID=bcast)
  [2 bytes]  SSID IE (tag=0, len=0 for broadcast)
  [10 bytes] Supported Rates IE
  [6 bytes]  Extended Supported Rates IE
  Total: 42 bytes → tx->len = 42

Our (broken) probe body layout:
  [10 bytes] Supported Rates IE
  [6 bytes]  Extended Supported Rates IE
  Total: 16 bytes → tx->len = 16
```

#### Critical: Wrong TX Rate for 2.4 GHz

| Field | Our Value (broken) | OpenBSD Value |
|-------|-------------------|---------------|
| `tx->plcp` | `0xd` (OFDM 6Mbps) | `10` (CCK 1Mbps) |
| `tx->rflags` | `IWN_RFLAG_ANT(chainmask)` = 0x80 | `IWN_RFLAG_CCK \| IWN_RFLAG_ANT(LSB(chainmask))` = 0x82 |

For 2.4 GHz scanning, probe requests must use CCK modulation. OFDM 6Mbps is only used for 5 GHz scanning. The `IWN_RFLAG_CCK` flag (bit 1) tells the firmware to use CCK modulation; without it, the firmware doesn't know the rate type.

Note: `IWN_RFLAG_ANT()` takes a single antenna (from `IWN_LSB(chainmask)`), not the full chain mask. For Intel 6200 with `txchainmask = IWN_ANT_BC = 6`, `IWN_LSB(6) = 2` (ANT_B). Both `(6 << 6)` and `(2 << 6)` truncate to `0x80` in uint8_t, so the antenna value happened to be correct by accident.

#### Other Differences Fixed

| Field | Our Value | OpenBSD Value | Impact |
|-------|-----------|---------------|--------|
| `hdr->quiet_time` | 0 | `htole16(10)` | When to move to next channel |
| `hdr->quiet_threshold` | 0 | `htole16(1)` | Min packets before quiet timer |
| `hdr->filter` | `MULTICAST` | `MULTICAST \| BEACON` | May not receive beacons during scan |
| `chan->dsp_gain` | `0x50` | `0x6e` | DSP amplifier gain |
| `chan->flags` | `ACTIVE \| NPBREQS(2)` | `ACTIVE` only | NPBREQS only used when searching for specific SSID |
| `chan->passive` | 110ms | 120ms | Must be > active dwell time |
| `hdr->rxchain` | Had IDLE_COUNT/MIMO_COUNT | No IDLE/MIMO COUNT | Unnecessary for scan |
| Missing SSID IE | Not present | `tag=0, len=0` | Required in probe request frame |

**Fix applied**: Rewrote `iwn_send_scan()` to match OpenBSD exactly. The scan command is now ~930 bytes (26 bytes larger due to 802.11 frame header + SSID IE).

## Bugs Found — Summary

| # | Bug | Symptom | Root Cause | Fix |
|---|-----|---------|------------|-----|
| 1 | Fire-and-forget commands | SW_ERR on second command | Overlapping commands on TX ring | `iwn_wait_cmd()` polls for response |
| 2 | Advanced BT coex commands | Firmware crash (cmd 204) | PRIOTABLE/PROT are 6005/6050 only | Remove them for 6000 series |
| 3 | Advanced BT coex struct | Firmware crash after BT_COEX | ~80 byte struct vs expected 12 bytes | Use simple `iwn_bluetooth` struct |
| 4 | FH_INT not cleared | RX ring stops receiving | Flow Handler DMA stalls without clearing | Clear FH_INT (0x010) in all poll loops |
| 5 | Missing LINK_QUALITY cmd | Can't set TX rate for probes | Command missing from config sequence | Add cmd 78 with CCK 1Mbps rate table |
| 6 | Missing TXPOWER_DBM cmd | **Firmware can't transmit** | No TX power configured | Add cmd 149 with 16 dBm |
| 7 | Missing CRITICAL_TEMP cmd | Radio may be disabled | No temperature threshold set | Add cmd 164 with 110°C |
| 8 | Missing 802.11 frame header | Scan silently dropped | Firmware needs complete probe frame | Add 24-byte MAC header + SSID IE |
| 9 | Wrong TX rate (OFDM) | Can't transmit on 2.4 GHz | Need CCK for 2.4 GHz probes | `plcp=10, rflags=CCK\|ANT(B)` |
| 10 | Missing IWN_FILTER_BEACON | May miss beacons in scan | Filter not set in scan header | Add `IWN_FILTER_BEACON` to filter |
| 11 | RSSI reading from wrong offset | Positive RSSI (112, 38) | Data is `iwn_rx_stat`, not direct `iwn5000_rx_phystat` | Cast through `iwn_rx_stat.phybuf` |
| 12 | RSSI missing IWN_RSSI_TO_DBM | Small positive values (10, 8) | 5000+ also needs the -44 offset | Full formula: `max(rssi) - agc - 44` |

## Key Lessons Learned

### 1. Intel WiFi firmware is unforgiving about data sizes
The 6000 firmware will acknowledge receipt of an oversized struct (BT coex: 80 bytes vs 12 expected) but crash while processing it. No error notification — just SW_ERR on the interrupt register. Always match struct sizes exactly.

### 2. 6000 vs 6005/6050 is a real split
Despite similar naming, the 6000 ("Puma Peak") uses simpler firmware than the 6005/6050. Features gated behind `IWN_FLAG_ADV_BT_COEX` and `IWN_FLAG_NEED_CALIBRATION` are real gates, not suggestions.

### 3. The firmware expects complete 802.11 frames
For probe requests in the scan command, the firmware fills in Duration and Sequence Control but needs the rest of the MAC header. Sending just Information Elements without the frame header causes a silent drop — no error, no SW_ERR, just nothing happens.

### 4. DMA is rarely the problem
When a command produces no response and no error, the instinct is to blame DMA delivery. But three completely different DMA approaches failed identically, proving the delivery was fine. The lesson: if the firmware acknowledges (no SW_ERR) but produces no output, the command content is wrong.

### 5. Flow Handler needs its own interrupt clearing
OpenBSD clears both `IWN_INT` (main interrupt status) AND `IWN_FH_INT` (Flow Handler interrupt status, offset 0x010) in every ISR iteration. Without clearing FH_INT, the FH stops DMA'ing new notifications to the RX ring. This is easy to miss because the main INT register looks clean.

### 6. OpenBSD is the canonical reference
Every deviation from OpenBSD's exact struct sizes, field values, and command ordering caused a real bug. The firmware implementation matches OpenBSD exactly. When in doubt, copy OpenBSD byte-for-byte.

### 9. RSSI Reading Wrong Data and Wrong Formula

**Symptom**: RSSI values were positive (112, 38, 35) instead of negative dBm.

**Root cause (bug 1)**: Cast `IWN_RX_PHY` notification data directly as `iwn5000_rx_phystat`, but the data is actually an `iwn_rx_stat`. The `iwn5000_rx_phystat` is nested inside `iwn_rx_stat.phybuf` at byte offset 20. We were reading timestamp bytes as RSSI.

**Root cause (bug 2)**: After fixing the offset, values were small positive (10, 8, 7) instead of negative. Missing `IWN_RSSI_TO_DBM` (44) in the formula. FreeBSD's `iwn5000_get_rssi()` confirms the full formula: `max(chain_rssi) - agc - IWN_RSSI_TO_DBM`.

**Fix**:
```c
// Before (wrong — reading from wrong offset, wrong formula):
struct iwn5000_rx_phystat *phy = (struct iwn5000_rx_phystat *)data;
r = (int8_t)(letoh16(phy->rssi[c]) & 0xff);
r -= IWN_RSSI_TO_DBM;  // no AGC subtraction

// After (correct — via iwn_rx_stat.phybuf, with AGC):
struct iwn_rx_stat *rxs = (struct iwn_rx_stat *)data;
struct iwn5000_rx_phystat *phy = (struct iwn5000_rx_phystat *)rxs->phybuf;
uint8_t agc = (uint8_t)((letoh32(phy->agc) >> 9) & 0x7f);
cached_rssi = (int8_t)(best - (int)agc - IWN_RSSI_TO_DBM);
```

## Current Status

**2026-02-24**: FULLY WORKING. All 11 channels scanned, 9 networks found on Dell E6510. RSSI values are correct negative dBm (-32 to -86). "Herrera" home network detected on ch1 (-69 dBm) and ch11 (-32 dBm). STARLINK, HP LaserJet printer, and hidden networks all detected with proper signal strength and security type.

## Files Modified

| File | Changes |
|------|---------|
| `iwn/iwn_scan.c` | Major rewrite: iwn_wait_cmd(), 8-command config sequence, simple BT coex, FH_INT clearing, proper scan content (802.11 frame header, CCK rate, filter, quiet, dsp_gain) |
| `iwn/iwn_fw.c` | Big command path (3 approaches tried), WiMAX coex, crystal calibration to runtime FW |
| `iwn/if_iwnreg.h` | Added missing struct definitions (iwn_bluetooth, iwn5000_cmd_txpower, iwn_critical_temp, iwn_pmgt_cmd, iwn_cmd_link_quality) |
| `iwn/if_iwnvar.h` | Added cmd_dma field, scan result storage |
