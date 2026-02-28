# IWN WiFi Driver Architecture

> Bare-metal Intel WiFi driver for BootAI, ported from OpenBSD's `iwn` driver to UEFI.

---

## Overview

The IWN driver enables WiFi networking on BootAI by driving the Intel Centrino Advanced-N 6200 directly from UEFI boot services. It is a phased port of OpenBSD's `sys/dev/pci/if_iwn.c`, adapted to run without a kernel by shimming OpenBSD's bus/DMA APIs onto UEFI protocols.

The driver exposes `EFI_SIMPLE_NETWORK_PROTOCOL` (SNP), which plugs into UEFI's existing IP4/TCP4/UDP4 stack. This means all existing BootAI networking code (HTTP server, mDNS, DNS, HTTP GET) works unchanged over WiFi.

## Target Hardware

Primary development target:

| Field | Value |
|-------|-------|
| Card | Intel Centrino Advanced-N 6200 (Dell E6510) |
| PCI IDs | `8086:422C`, `8086:4239` |
| Chip family | Intel 6000 series ("Puma Peak") |
| MAC type | SoftMAC (host handles 802.11 MAC) |
| Firmware | `iwn-6000` (OpenBSD-renamed `iwlwifi-6000-4.ucode`) |

### All Supported Cards

The OpenBSD `iwn` driver supports the full Intel 4965/5000/6000 family. Once the driver is complete, all of these cards will work:

| Card | PCI IDs | Firmware | Common In |
|------|---------|----------|-----------|
| Wireless WiFi Link 4965 | `4229`, `4230` | `iwn-4965` | 2007-2009 laptops |
| WiFi Link 5100 | `4232`, `4237` | `iwn-5000` | ThinkPad T400/T500, Dell E6400 |
| WiFi Link 5300 | `4235`, `4236` | `iwn-5000` | ThinkPad W500, Dell Precision |
| WiFi Link 5350 | `423a`, `423b` | `iwn-5000` | MacBook Pro 2008-2009 |
| WiFi Link 5150 | `423c`, `423d` | `iwn-5150` | ThinkPad X200/X301 |
| WiFi Link 1000 | `0083`, `0084` | `iwn-1000` | Budget 2009-2010 laptops |
| Centrino Ultimate-N 6300 | `422b`, `4238` | `iwn-6000` | ThinkPad T410/W510, Dell E6410 |
| **Centrino Advanced-N 6200** | **`422c`**, **`4239`** | **`iwn-6000`** | **Dell E6510**, ThinkPad T410 |
| Centrino Advanced-N 6250 | `0087`, `0089` | `iwn-6050` | ThinkPad X201, Dell E6410 |
| Centrino Advanced-N 6205 | `0082`, `0085` | `iwn-6005` | ThinkPad T420/T520, Dell E6420 |
| Centrino Advanced-N 6030 | `0090`, `0091` | `iwn-6030` | HP EliteBook 8460p |
| Centrino Advanced-N 6235 | `088e`, `088f` | `iwn-6235` | ThinkPad T430/X230, Dell E6430 |
| WiFi Link 1030 | `008a`, `008b` | `iwn-1000` | Budget 2010-2011 laptops |
| Centrino Wireless-N 2230 | `0887`, `0888` | `iwn-2030` | ThinkPad Edge, Dell Inspiron |
| Centrino Wireless-N 2200 | `0890`, `0891` | `iwn-2000` | Budget 2012-2013 laptops |
| Centrino Wireless-N 135 | `0892`, `0893` | `iwn-135` | Budget ultrabooks |
| Centrino Wireless-N 105 | `0894`, `0895` | `iwn-105` | Budget 2012 laptops |
| Centrino Wireless-N 130 | `0896`, `0897` | `iwn-130` | Budget 2011 laptops |
| Centrino Wireless-N 100 | `08ae`, `08af` | `iwn-100` | Netbooks, budget laptops |

All PCI IDs are vendor `8086` (Intel). These cards were shipped in millions of laptops from ~2007-2013. Any laptop from that era with Intel WiFi likely has one of these.

## File Layout

```
bootai/iwn/
├── iwn.h              # Public API (iwn_probe, iwn_init, iwn_attach, iwn_scan_cmd, iwn_connect_cmd, iwn_ping_cmd)
├── iwn_internal.h     # Shared internal header (MMIO, prph, lock, DMA, FW, scan, assoc, EAPOL, data)
├── iwn_shim.h         # UEFI shim: OpenBSD types → stdint, byte-order no-ops, bus_space stubs
├── if_iwnreg.h        # Hardware registers (from OpenBSD, with additions)
├── if_iwnvar.h        # iwn_dev struct, DMA ring structs, scan results, assoc/EAPOL/data state, device IDs
├── iwn_pci.c          # PCI enumeration, device matching, iwn_init() entry point
├── iwn_hw.c           # MMIO primitives, NIC lock, APM init/stop, EEPROM read, iwn_attach()
├── iwn_dma.c          # DMA memory allocation, TX/RX ring setup, scheduler table
├── iwn_fw.c           # Firmware parse/upload, hw_init sequence, calibration, host commands (big + small)
├── iwn_scan.c         # Radio config, scan command, beacon parsing, result display
├── iwn_assoc.c        # 802.11 Open System auth + association + post-assoc config (~700 lines)
├── iwn_eapol.c        # WPA2-PSK EAPOL 4-way handshake (~420 lines)
├── iwn_data.c         # Data frame TX/RX: Ethernet↔802.11, software RX buffer (~280 lines)
├── iwn_crypto.h       # Crypto API: SHA-1, HMAC-SHA1, PBKDF2, AES-128, AES Unwrap, PRF-384
├── iwn_crypto.c       # Self-contained crypto implementation (~490 lines, no libc)
└── test_crypto.c      # Host-testable: 13 standard test vectors (FIPS, RFC, IEEE)
```

```
├── iwn_snp.c          # EFI_SIMPLE_NETWORK_PROTOCOL wrapper (~280 lines)
```

## UEFI Integration

### Network Stack Layering

```
  Existing BootAI Code (unchanged)
  ────────────────────────────────
  HTTP Server / mDNS / DNS / HTTP GET
          ↓
  UEFI TCP4 / UDP4 / IP4 Protocols
          ↓
  UEFI IP4 / ARP / MNP stack
          ↓
  EFI_SIMPLE_NETWORK_PROTOCOL (SNP)    ← IWN driver implements this
          ↓
  iwn driver (ported from OpenBSD)
          ↓
  Intel 6200 hardware + firmware
```

### PCI Access (Phase 0)

The driver uses `EFI_PCI_IO_PROTOCOL` to discover and communicate with the Intel 6200:

1. **Discovery**: `LocateHandleBuffer(ByProtocol, PCI_IO_GUID)` finds all PCI devices
2. **Identification**: `Pci.Read()` reads vendor/device IDs from config space
3. **Location**: `GetLocation()` returns bus:device.function for diagnostics
4. **MMIO** (Phase 1+): `Mem.Read()`/`Mem.Write()` access BAR0 registers

### EFI_PCI_IO_PROTOCOL Definition

Added to `efi/efi.h` after the IP4_CONFIG2 protocol. Includes:

- `EFI_PCI_IO_PROTOCOL_WIDTH` enum (Uint8/16/32/64)
- `EFI_PCI_IO_PROTOCOL_ACCESS` struct (Mem.Read/Write, Io.Read/Write)
- `EFI_PCI_IO_PROTOCOL_CONFIG_ACCESS` struct (Pci.Read/Write for config space)
- `GetLocation` function pointer (returns segment/bus/device/function)
- `Attributes` function pointer (get/set/enable/disable device attributes)
- Standard UEFI PCI IO GUID: `{4cf5b200-68b8-4ca5-9eec-b23e3f500029a}`

## OpenBSD Shim Layer

The `iwn_shim.h` header maps OpenBSD kernel types to UEFI-compatible equivalents:

| OpenBSD Type | UEFI Equivalent | Notes |
|-------------|-----------------|-------|
| `u_int8_t` | `uint8_t` | BSD unsigned int types |
| `u_int16_t` | `uint16_t` | |
| `u_int32_t` | `uint32_t` | |
| `u_int64_t` | `uint64_t` | |
| `bus_space_tag_t` | `void *` | Will point to PCI IO protocol |
| `bus_space_handle_t` | `uint64_t` | BAR0 mapped address |
| `__packed` | `__attribute__((packed))` | GCC attribute |

IEEE 802.11 constants normally from `<net80211/ieee80211.h>`:

| Constant | Value | Use |
|----------|-------|-----|
| `IEEE80211_ADDR_LEN` | 6 | MAC address length |
| `IEEE80211_TKIP_MICLEN` | 8 | TKIP MIC length |
| `IEEE80211_NWID_LEN` | 32 | Max SSID length |
| `EDCA_NUM_AC` | 4 | QoS access categories |

## Phase Plan

### Phase 0: PCI Probe (done)

- Enumerate PCI bus via `EFI_PCI_IO_PROTOCOL`
- Match against full device ID table (~38 cards)
- Print matched card name with BDF location
- REPL command: `iwn`

### Phase 1: MMIO + EEPROM (done)

- Enable PCI MEMORY + BUS_MASTER attributes before BAR0 access
- MMIO via `EFI_PCI_IO_PROTOCOL.Mem.Read/Write` on BAR0
- Peripheral register access (prph_read/write) for NIC-internal registers at 0x3000+
- NIC lock/unlock (MAC_ACCESS_REQ handshake)
- Hardware prepare (take NIC from Intel AMT)
- APM init: GIO chicken workarounds, HPET threshold, PCIe ASPM L1 check, DMA clock enable, L1A disable
- OTP init for 6000+ series (APMG reset, auto clock gate, clear ECC)
- EEPROM word read with READ_VALID polling and OTP ECC checking
- Read MAC address, SKU capabilities, RF config, regulatory domain, calibration version, crystal calibration

#### Hardware Test Results (Dell E6510)

```
> iwn
  Centrino Advanced-N 6200 found at 2:0.0
  HW type: 7 (6000 series)
  OTP: yes
  MAC: 00:27:10:56:ad:80
  Domain: Mo
  RF config: type=1 step=3 dash=1 tx=0x03 rx=0x03
  Calibration ver: 6
  802.11n: yes
  Intel WiFi ready.
```

#### Key Lesson: PCI Attribute Enable

UEFI does not enable MMIO by default. Without calling `Attributes(Enable, MEMORY | BUS_MASTER)`, all BAR0 reads return `0xffffffff`. This manifested as HW type 31 (all 1s) and NIC lock timeouts on first attempt.

```c
#define EFI_PCI_IO_ATTRIBUTE_MEMORY      0x0200
#define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER  0x0400
pci_io->Attributes(pci_io, 2 /* Enable */, 0x0200 | 0x0400, NULL);
```

### Phase 2: DMA + Firmware Load (done)

Full hardware initialization: DMA ring allocation, firmware upload via FH DMA, UC_READY polling, post-alive calibration, power-cycle, runtime firmware boot.

#### DMA Memory Model

UEFI identity-maps all memory (virt == phys), so no IOMMU mapping is needed. All DMA buffers are allocated below 4 GB using `AllocatePages(AllocateMaxAddress)` with max address `0xFFFFFFFF`.

```
DMA Memory Layout (~2.5 MB total):
├── Keep-Warm Page           4 KB     (1 page)
├── TX Scheduler Table      12 KB     (3 pages) — 20 queues × 320 entries × 2 bytes
├── TX Ring 4 (cmd queue)
│   ├── Descriptors          32 KB    (8 pages) — 256 × 128 bytes
│   └── Commands             35 KB    (9 pages) — 256 × 140 bytes
├── TX Ring 0 (data queue)   [same layout as ring 4]
├── RX Ring
│   ├── Descriptor array     4 KB     (1 page)  — 64 × uint32_t (phys addrs >> 8)
│   ├── Status area          4 KB     (1 page)  — closed_count + flags
│   └── RX Buffers           256 KB   (64 pages) — 64 × 4 KB
└── Firmware DMA scratch     128 KB   (32 pages) — reused per chunk
```

#### Firmware Format

The Intel 6200 uses legacy v2 firmware (`iwlwifi-6000-4.ucode`, renamed to `iwn-6000`).

**Legacy v2 header** (28 bytes):
```
Offset  Type      Field
0x00    uint32    ver             — packed: major.minor.API.serial
0x04    uint32    build           — build number
0x08    uint32    inst_size       — main text (runtime code) size
0x0C    uint32    data_size       — main data (runtime data) size
0x10    uint32    init_size       — init text size
0x14    uint32    init_data_size  — init data size
0x18    uint32    boot_size       — boot text (unused on 5000+)
```

Sections follow concatenated after the header: inst, data, init, init_data, boot.

**Format detection**: First uint32 == 0 means TLV format (has a zero padding word before the `IWL\n` signature). Non-zero first word means legacy v2 (it's the version number). The parser supports both.

#### Firmware Upload: FH DMA Service Channel

The 5000+ series uses **FH (Flow Handler) DMA service channel 9** (`IWN_SRVC_DMACHNL`) to load firmware into NIC SRAM. NOT channel 0 (which is for TX data).

Upload sequence per section (handles >128KB via chunking):
```
1. Copy chunk to DMA buffer (below 4GB)
2. iwn_nic_lock()                                    — hold for entire FH sequence
3. Write IWN_FH_TX_CONFIG(9) = 0                     — pause service channel
4. Write IWN_FH_SRAM_ADDR(9) = dst_sram + offset     — NIC SRAM destination
5. Write IWN_FH_TFBD_CTRL0(9) = dma_paddr            — host DMA source address
6. Write IWN_FH_TFBD_CTRL1(9) = chunk_size            — byte count
7. Write IWN_FH_TXBUF_STATUS(9) = TBNUM(1)|TBIDX(1)|TFBD_VALID — mark buffer valid
8. Write IWN_FH_TX_CONFIG(9) = DMA_ENA|CIRQ_HOST_ENDTFD        — start DMA
9. iwn_nic_unlock()
10. Poll IWN_FH_TX_STATUS for IDLE(9) bit             — DMA complete
```

After all sections uploaded:
```
11. Write IWN_RESET = 0                               — release CPU, start firmware
12. Poll RX ring for IWN_UC_READY (type 1)            — firmware alive
```

#### Hardware Init Sequence (`iwn_hw_init`)

```
 1. APM init (power on, DMA clock, disable L1A)
 2. NIC config (set RADIO_SI, MAC_SI, EARLY_PWROFF_DIS)
 3. Allocate DMA: keep-warm, scheduler, TX×2, RX (64 buffers)
 4. Stop all FH DMA channels (0-7)
 5. Program rings: KW, TX descriptor bases, RX ring+config+status
 6. Enable DMA channels 0-7 (DMA_ENA | DMA_CREDIT_ENA)
 7. Enable shadow registers (6000+: SHADOW_REG_CTRL |= 0x800fffff)
 8. Set VMAIN power source (clear APMG_PS PWR_SRC_MASK)
 9. Clear interrupts (we poll, not interrupt-driven)
10. Parse firmware file
11. Upload INIT firmware (text → 0x00000000, data → 0x00800000)
12. Release CPU → poll for UC_READY (init ucode alive)
13. Clear RFKILL (3× IWN_UCODE_GP1_CLR)
14. Activate TX scheduler (SCHED_DRAM_ADDR, queue 4 active, TXFACT)
15. Send WiMAX coex (cmd 90, all zeros — required before calibration)
16. Send crystal calibration (PHY_CALIB, cmd 176) via TX ring 4
17. Send CALIB_CONFIG (cmd 101) — triggers firmware calibration
18. Poll for CALIBRATION_DONE (103), save CALIBRATION_RESULT (102) entries
19. Power cycle (APM stop → APM init → NIC config)
20. Reset rings, re-program hardware
21. Upload RUNTIME firmware, release CPU → poll for UC_READY
22. Clear RFKILL, activate scheduler
23. Send WiMAX coex + crystal calibration + replay saved calibration to runtime FW
24. Intel WiFi ready!
```

#### Calibration Flow

After init firmware is alive, the driver sends calibration commands and collects results. The command sequence is critical — **WiMAX coexistence must be sent first**, otherwise the 6000 series firmware acknowledges the calibration commands but never actually runs calibration.

**Init firmware calibration sequence:**

| # | Command | Code | Purpose |
|---|---------|------|---------|
| 1 | WiMAX coex | 90 | Disable WiMAX coexistence (all zeros). **Required** — without this, firmware stalls after acking calibration commands |
| 2 | PHY_CALIB (crystal) | 176 | Crystal frequency calibration from EEPROM cap_pin values |
| 3 | CALIB_CONFIG | 101 | Trigger full calibration sequence (enable=start=send=apply=flags=0xFFFFFFFF) |

The firmware responds with `CALIBRATION_RESULT` (type 102) notifications containing calibration data, followed by `CALIBRATION_DONE` (type 103). The driver saves all calibration results and replays them to the runtime firmware after the power cycle.

**Observed on Dell E6510 (6000 series):** 1 calibration result (508 bytes). The 6000 series produces LO + TX_IQ + BASE_BAND calibration data (may arrive as 1 combined or 3 separate results depending on firmware version).

**Runtime firmware calibration:** After runtime firmware boots, the driver replays saved calibration results AND must also send WiMAX coex + crystal calibration to the runtime firmware before RXON.

#### Calibration Bugs Found and Fixed

| Bug | Impact | Fix |
|-----|--------|-----|
| Missing WiMAX coex command | Init firmware acked calibration cmds but never ran calibration (5s timeout) | Send `IWN5000_CMD_WIMAX_COEX` (all zeros) before crystal calib |
| `iwn_poll_rx` early return | Returned on `expected_type` match without advancing `rxq.cur` or updating `IWN_FH_RX_WPTR` — leaked RX buffer slots | Advance cur + update WPTR before returning 0 |
| Calibration data length | `- sizeof(struct iwn_rx_desc)` (8) truncated data by 4 bytes | Changed to `- 4` per OpenBSD reference |
| Unchecked cmd return values | Crystal calib + CALIB_CONFIG failures were silent | Check `iwn_cmd()` return, print error, abort early |

#### Key Lessons Learned

**1. DMA channel matters.** The firmware upload uses service channel 9, not channel 0. Channel 0 is for normal TX data. Using the wrong channel means the firmware never loads.

**2. TXBUF_STATUS is required.** Without writing `IWN_FH_TXBUF_STATUS` with `TBNUM(1)|TBIDX(1)|TFBD_VALID`, the FH DMA engine doesn't recognize the transfer buffer descriptor as valid. The DMA appears to succeed (IDLE bit sets) but no data is actually transferred.

**3. NIC lock during FH writes.** All FH register programming must be done with the NIC lock held (`iwn_nic_lock`). Without it, the MAC can go back to sleep between register writes, causing partial programming.

**4. DMA channels 0-7 must be enabled.** Before firmware load, channels 0-7 need `DMA_ENA | DMA_CREDIT_ENA`. This enables the DMA credit system that the FH needs.

**5. Shadow registers for 6000+.** The 6000 series requires `IWN_SHADOW_REG_CTRL |= 0x800fffff` before firmware can use register shadows for power management.

**6. VMAIN power source.** Must clear `IWN_APMG_PS_PWR_SRC_MASK` to select VMAIN as power source. Without this, the NIC may use VAUX (auxiliary power) which doesn't support full operation.

**7. NIC config sets bits, doesn't clear.** `iwn5000_nic_config` must SET `RADIO_SI` and `MAC_SI` bits (not clear them). These enable the radio and MAC silicon interfaces. Clearing them disables the radio entirely.

**8. Scheduler activation after ALIVE.** The TX scheduler (SCHED_DRAM_ADDR, SCHED_TXFACT, QUEUE_STATUS) must be programmed AFTER the firmware sends ALIVE. Programming it before firmware load is ineffective because the firmware resets scheduler state on boot.

**9. Legacy v2 firmware format.** The `iwlwifi-6000-4.ucode` file uses legacy v2 format (not TLV). Detection: first uint32 is non-zero (it's the version number). The TLV format has a zero padding word first.

### Phase 3: Scanning (done)

First RF activity on real hardware. Configures the radio, sends a scan command across all 11 2.4 GHz channels, receives beacon/probe response frames, parses SSIDs, and displays results.

REPL commands: `iwn` (init) → `iwn scan`

See [IWN Phase 3 Debug Log](../research/iwn-phase3-debug-log.md) for the full debugging narrative with 10 bugs found and fixed during hardware testing.

#### Synchronous Command Waits

UEFI has no interrupts — all RX ring processing is polling-based. The firmware can't handle overlapping commands, so each config command must wait for its response before sending the next:

```c
iwn_cmd(dev, CMD_CODE, payload, len);     // enqueue on TX ring 4
iwn_wait_cmd(dev, CMD_CODE, "NAME", 200); // poll RX ring for matching response
```

`iwn_wait_cmd()` drains all notifications (logging unexpected types) until it finds `desc->type == cmd_code`. On each iteration it clears both `IWN_INT` and `IWN_FH_INT` registers to keep DMA flowing — the Flow Handler stalls if `IWN_FH_INT` (offset 0x010) is not cleared.

#### Big Command Support

The scan command is ~930 bytes, but `iwn_tx_cmd.data[]` is only 136 bytes. For big commands, the payload is written directly into the TX ring's cmd buffer (known-good DMA memory below 4 GB) and referenced via a single-segment TX descriptor.

#### Pre-Scan Radio Configuration

Eight commands must be sent before the scan, all synchronous. Order matches OpenBSD's `iwn_config()` + `iwn_add_broadcast_node()`:

| # | Command | Code | Size | Purpose |
|---|---------|------|------|---------|
| 1 | BT_COEX | 155 | 12 B | Simple Bluetooth coexistence (required before RXON) |
| 2 | TX_ANT_CONFIG | 152 | 4 B | TX antenna chain mask |
| 3 | RXON | 16 | 50 B | Radio on: STA mode, channel 1, 2.4 GHz, HT masks |
| 4 | ADD_NODE | 24 | 92 B | Broadcast node (ID 15, MAC FF:FF:FF:FF:FF:FF) |
| 5 | LINK_QUALITY | 78 | 136 B | TX rate table for broadcast node (CCK 1Mbps × 16 retries) |
| 6 | TXPOWER_DBM | 149 | 12 B | TX power: 16 dBm (required for firmware to transmit) |
| 7 | SET_CRITICAL_TEMP | 164 | 8 B | Safety threshold: 110°C |
| 8 | SET_POWER_MODE | 119 | 16 B | CAM (continuously awake mode) |

**BT_COEX**: Plain 6000 series uses the simple `iwn_bluetooth` struct (12 bytes). The advanced `iwn6000_btcoex_config` (~80 bytes) and `BT_COEX_PRIOTABLE` (cmd 204) / `BT_COEX_PROT` (cmd 205) are for 6005/6050 only — the 6000 firmware crashes on these.

**RXON config**:
- Flags: `IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF | IWN_RXON_AUTO | IWN_RXON_24GHZ`
- Filter: `IWN_FILTER_MULTICAST`
- RX chain: `VALID(chains) | IDLE_COUNT(2) | MIMO_COUNT(2)` (firmware manages chain selection)
- HT masks: `ht_single_mask = ht_dual_mask = ht_triple_mask = 0xff`
- MAC address and WLAP set from EEPROM, BSSID all-zeros (unassociated)

**LINK_QUALITY**: All 16 retry entries set to CCK 1Mbps (`plcp=10, rflags=CCK|ANT(B)`). Without this, firmware doesn't know what rate to use for probe requests.

**TXPOWER_DBM**: `global_limit = 32` (16 dBm × 2), `flags = NO_CLOSED`, `srv_limit = AUTO`. Without this, firmware cannot transmit.

#### Scan Command Layout

The scan command (code 128, `IWN_CMD_SCAN`) is built in `iwn_send_scan()`:

```
Offset   Size        Content
0        28 B        iwn_scan_hdr — nchan=11, quiet_time=10, quiet_threshold=1,
                     crc_threshold=0xffff, rxchain=DRIVER_FORCE|VALID|FORCE_SEL|FORCE_MIMO,
                     flags=24GHZ|AUTO, filter=MULTICAST|BEACON
28       56 B        iwn_cmd_data — TX params: CCK 1Mbps, auto-seq, broadcast ID 15,
                     tx->len = 42 (full probe frame size)
84       680 B       20 × iwn_scan_essid — all zeroed (broadcast scan)
764      42 B        Probe request frame body:
                       [24 B] 802.11 MAC header (FC=probe_req, DA=bcast, SA=MAC, BSSID=bcast)
                       [2 B]  SSID IE (tag=0, len=0)
                       [10 B] Supported Rates IE (1/2/5.5/11/6/9/12/18 Mbps)
                       [6 B]  Extended Rates IE (24/36/48/54 Mbps)
806      132 B       11 × iwn_scan_chan — channels 1-11, active scan, 30ms/120ms dwell
                     ─────────
Total    ~938 B      Sent via big-command path (async, no response expected)
```

The scan is asynchronous — results come as RX notifications (`START_SCAN` 130, `RX_PHY` 192, `MPDU_RX_DONE` 193, `STOP_SCAN` 132).

Channel parameters: `IWN_CHAN_ACTIVE`, `rf_gain=0x28`, `dsp_gain=0x6e`. The `crc_threshold` is set to `IWN_GOOD_CRC_TH_NEVER` (0xffff) to work around a firmware sysassert bug on zero threshold.

#### RX Polling and Beacon Parsing

After sending the scan command, `iwn_poll_scan_results()` polls the RX ring for up to 8 seconds, handling these notification types:

| Type | Code | Action |
|------|------|--------|
| `IWN_START_SCAN` | 130 | Print "Scanning ch N" for each channel |
| `IWN_RX_PHY` | 192 | Cache RSSI: max of 3 antenna chains, subtract 44 dBm offset |
| `IWN_MPDU_RX_DONE` | 193 | Parse beacon frame from `iwn_rx_mpdu` → `iwn_parse_beacon()` |
| `IWN_STOP_SCAN` | 132 | Scan complete — print summary, return |
| `REPLY_ERROR` | 2 | Firmware rejected command — print error code |
| Other | — | Log notification type |

Both `IWN_INT` and `IWN_FH_INT` are cleared every iteration to keep DMA flowing.

**RSSI calculation**: Each `IWN_RX_PHY` notification contains per-chain RSSI values in `iwn5000_rx_phystat.rssi[3]`. The driver takes the maximum across chains and subtracts `IWN_RSSI_TO_DBM` (44) to convert to dBm. If no PHY notification precedes an MPDU, RSSI defaults to -100.

**Beacon parsing** (`iwn_parse_beacon`):
1. Verify frame control: must be Beacon (subtype 8) or Probe Response (subtype 5)
2. Extract BSSID from bytes 16-21 of the MAC header
3. Deduplicate by BSSID (update RSSI if stronger signal seen)
4. Walk Information Elements (TLV at byte 36+):
   - Tag 0 (SSID): copy name, length 0 = hidden
   - Tag 3 (DS Parameter Set): channel number
   - Tag 48 (RSN): mark WPA2 security
   - Tag 221 (Vendor, OUI `00:50:F2:01`): mark WPA security

Results are stored in `dev->scan_results[]` (max 32 entries), sorted by RSSI descending for display.

#### Hardware Test Results (Dell E6510)

```
> iwn scan
  Configuring radio...
  BT_COEX ok
  TX_ANT ok
  RXON ok
  ADD_NODE ok
  rx[192] rx[193] rx[192] rx[193] rx[192] rx[193]  LINKQ ok
  rx[192] TXPOW ok
  rx[192] rx[193] rx[192] rx[193]  CTEMP ok
  rx[192] rx[193]  POWER ok
  SCAN cmd: len=938
  rx[128]  Scanning ch 1  rx[131] 2  rx[131] 3 ... 10  rx[131] 11  rx[131]
  Scan complete: 11 channels, 9 networks found

  #  SSID                              CH  RSSI  Security
  1  Herrera                            11  -32   WPA2
  2  Herrera                             1  -69   WPA2
  3  (hidden)                            1  -69   WPA2
  4  (hidden)                            1  -69   WPA2
  5  (hidden)                           11  -84   WPA2
  6  (hidden)                            6  -84   WPA2
  7  STARLINK                            6  -85   Open
  8  (hidden)                            6  -85   WPA2
  9  DIRECT-e6-HP M283 LaserJet          1  -86   WPA2
```

- All 11 channels scanned, 9 networks found
- Beacons (`rx[192]`/`rx[193]`) stream after RXON — radio is active
- `rx[128]` = scan command ack, `rx[131]` = per-channel scan complete
- RSSI: proper negative dBm (-32 strong to -86 weak)
- RSSI formula: `max(chain_rssi) - agc - IWN_RSSI_TO_DBM` via `iwn_rx_stat.phybuf`

#### Debug Guide

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| SW_ERR after any command | Wrong struct size or unsupported command | Compare struct size with OpenBSD exactly |
| No notifications during poll | FH_INT not cleared | Ensure `iwn_write(dev, IWN_FH_INT, 0xffffffff)` in every loop |
| Config ok but scan silent | Missing commands (TXPOWER, LINK_QUALITY) | Send all 8 commands per OpenBSD sequence |
| Scan silent, no error | Scan command content wrong | Compare probe body byte-for-byte with OpenBSD |
| `rx[192]/rx[193]` streaming | Radio is working — this is good | Beacons from nearby APs after RXON |

### Phase 4a: Crypto Primitives (done)

Self-contained WPA2-PSK cryptographic building blocks. No UEFI dependencies — compiles and tests on the host, cross-compiles into the EFI binary unchanged.

**Files**: `iwn_crypto.h` (API), `iwn_crypto.c` (implementation), `test_crypto.c` (verification)

#### Primitives Implemented

| Primitive | Standard | Purpose in WPA2 |
|-----------|----------|-----------------|
| SHA-1 | FIPS 180-4 | Foundation for HMAC and PBKDF2 |
| HMAC-SHA1 | RFC 2104 | PRF building block, EAPOL MIC |
| PBKDF2-SHA1 | RFC 2898 | Derive PMK from passphrase + SSID (4096 iterations) |
| AES-128 | FIPS 197 | Block cipher for key unwrap and CCMP |
| AES Key Unwrap | RFC 3394 | Unwrap GTK from EAPOL message 3 |
| PRF-384 | IEEE 802.11i | Derive PTK (KCK + KEK + TK) from PMK + nonces + addresses |

#### Design: Self-Contained Module

The crypto module has zero external dependencies beyond `<stdint.h>` and `<stddef.h>`:

- **Own memory ops**: Static `crypto_memcpy`, `crypto_memset`, `crypto_memcmp` — byte loops, no libc
- **Own endian helpers**: `be32_load`, `be32_store`, `be64_store` — SHA-1 and AES use big-endian
- **No UEFI headers**: Compiles identically with `cc` (host) and `x86_64-elf-gcc` (cross)
- **No RNG**: Random number generation needs UEFI (`EFI_RNG_PROTOCOL`) or x86 (`RDTSC`), deferred to `iwn_wpa.c` in Phase 4c

#### SHA-1 (FIPS 180-4)

Streaming interface (`init`/`update`/`final`) required by HMAC, which feeds SHA-1 in two phases: ipad block then data, opad block then inner hash.

- `sha1_transform(state, block)`: 80 rounds with W[16..79] expansion via ROTL1
- Initial hash: `H0=0x67452301, H1=0xEFCDAB89, H2=0x98BADCFE, H3=0x10325476, H4=0xC3D2E1F0`
- Round constants: `K[0..19]=0x5A827999, K[20..39]=0x6ED9EBA1, K[40..59]=0x8F1BBCDC, K[60..79]=0xCA62C1D6`
- Padding: append `0x80`, zero-pad to 56 mod 64, append bit count as big-endian 64-bit

#### HMAC-SHA1 (RFC 2104)

```
HMAC(K, text) = SHA1((K ^ opad) || SHA1((K ^ ipad) || text))
```

- ipad = 0x36, opad = 0x5C
- Keys longer than 64 bytes are hashed to 20 bytes first

#### PBKDF2-SHA1 (RFC 2898)

Derives the 32-byte PMK from a WiFi passphrase and SSID:

```
PMK = PBKDF2(passphrase, SSID, 4096, 32)
```

- Block counter is 1-based, appended as big-endian uint32 to salt
- T is initialized from U1 (not zeros), then XORed with U2..U_c
- Two PBKDF2 blocks needed for 32-byte output (20 + 12 bytes)

#### AES-128 (FIPS 197)

Full encrypt and decrypt with precomputed key schedule:

- **Key schedule**: 4 initial words expanded to 44 via `sub_word(rot_word(w)) ^ rcon`
- **Context struct**: `iwn_aes_ctx` holds the 44-word expanded key, computed once in `iwn_aes128_init`
- **State layout**: Column-major `state[r + 4*c]` per FIPS 197
- **Encrypt**: AddRoundKey → 9× (SubBytes, ShiftRows, MixColumns, AddRoundKey) → SubBytes, ShiftRows, AddRoundKey
- **Decrypt**: AddRoundKey(rk10) → 9× (InvShiftRows, InvSubBytes, AddRoundKey, InvMixColumns) → InvShiftRows, InvSubBytes, AddRoundKey(rk0)
- **MixColumns**: Implemented via `xtime(a) = (a << 1) ^ ((a >> 7) * 0x1b)`

Static S-box (256 bytes), inverse S-box (256 bytes), and Rcon[10] are compile-time constants.

#### AES Key Unwrap (RFC 3394 Section 2.2.2)

Used to unwrap the Group Temporal Key (GTK) from EAPOL message 3. The AP encrypts the GTK with the KEK (from PTK), and the station unwraps it.

- Default IV: `0xA6 × 8` (integrity check value)
- 6 rounds over n 64-bit data blocks, decrementing counter XORed into A
- Returns -1 if integrity check fails (wrong KEK or corrupted data)

#### PRF-384 (IEEE 802.11i Section 8.5.1.1)

Derives the 48-byte Pairwise Transient Key (PTK) from the PMK:

```
PTK = PRF-384(PMK, "Pairwise key expansion" || 0x00 || min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(ANonce,SNonce))
```

The 48-byte PTK splits into:
- KCK (bytes 0-15): Key Confirmation Key — HMAC-SHA1 MIC on EAPOL frames
- KEK (bytes 16-31): Key Encryption Key — AES unwrap of GTK
- TK (bytes 32-47): Temporal Key — AES-CCMP data encryption

Implementation: 3 iterations of HMAC-SHA1 (3 × 20 = 60 bytes), truncated to 48.

#### Test Vectors

13 tests from published standards, all passing:

| # | Test | Standard | What It Verifies |
|---|------|----------|-----------------|
| 1 | `sha1("abc")` | FIPS 180-4 | Basic single-block hash |
| 2 | `sha1("")` | FIPS 180-4 | Empty input edge case |
| 3 | `sha1(56 bytes)` | FIPS 180-4 | Two-block padding (56 bytes forces extra block) |
| 4 | `hmac_sha1(0x0b×20, "Hi There")` | RFC 2202 Case 1 | Short key, short data |
| 5 | `hmac_sha1("Jefe", "what do ya want...")` | RFC 2202 Case 2 | ASCII key and data |
| 6 | `hmac_sha1(0xAA×20, 0xDD×50)` | RFC 2202 Case 3 | Repeated-byte key and long data |
| 7 | `pbkdf2("password", "IEEE", 4096)` | IEEE 802.11i | Standard WiFi PMK derivation |
| 8 | `pbkdf2("ThisIsAPassword", "ThisIsASSID", 4096)` | IEEE 802.11i | Longer passphrase and SSID |
| 9 | AES-128 encrypt | FIPS 197 App B | Known key/plaintext/ciphertext triplet |
| 10 | AES-128 roundtrip | FIPS 197 | Encrypt then decrypt returns original |
| 11 | AES-128 encrypt | NIST SP 800-38A F.1.1 | ECB mode test vector |
| 12 | AES Key Unwrap | RFC 3394 Section 4.1 | 128-bit KEK, 128-bit key data |
| 13 | PRF-384 | Computed | Consistency check against manual HMAC-SHA1 |

Build and run: `make test-iwn-crypto` (uses host `cc`, not cross-compiler)

#### Common Implementation Gotchas

| Bug | Prevention |
|-----|-----------|
| SHA-1 length in bytes not bits | Store `total_len * 8` as big-endian 64-bit in padding |
| SHA-1 padding when buf_len == 56 | Need extra block — 0x80 fills last byte before length field |
| HMAC key > 64 bytes not hashed | Check `key_len > 64` before zero-padding |
| PBKDF2 T initialized to zeros | Copy U1 into T first, then XOR U2..U_c |
| PBKDF2 off-by-one iterations | Loop `j = 1` to `iterations - 1` (U1 already done) |
| AES state column-major | `state[r + 4*c]`, NOT `state[r*4 + c]` |
| AES final round includes MixColumns | Round 10 skips MixColumns (encrypt) / InvMixColumns (decrypt) |
| AES decrypt round key order | Start with rk[40..43], end with rk[0..3] |
| PRF label includes null | `0x00` is a separator between label and data, not a string terminator |
| AES unwrap direction | Must use `iwn_aes128_decrypt`, not encrypt |

### Phase 4b: Authentication + Association (code complete, untested on hardware)

Open System authentication followed by 802.11 association. After this, the firmware is configured for the target AP and the AP starts the EAPOL handshake (for WPA2 networks).

**File**: `iwn_assoc.c` (~700 lines)

REPL command: `iwn pass <password>` → `iwn connect <SSID>`

#### Connection State Machine

```
iwn_connect(dev, ssid)
  ├── Find SSID in scan results
  ├── Check password for WPA2 (fail early if not set)
  │
  ├── iwn_auth(dev)                          [INIT → AUTH]
  │   ├── RXON — station mode, target BSSID + channel
  │   ├── TIMING — beacon interval from scan
  │   ├── ADD_NODE — broadcast node (id=15)
  │   ├── LINK_QUALITY — broadcast CCK 1Mbps
  │   ├── TX Auth Request (Open System, seq=1)
  │   └── Poll for Auth Response (seq=2, status=0)
  │
  ├── iwn_associate(dev)                     [AUTH → ASSOC]
  │   ├── TX Association Request
  │   │   ├── Capability Info (ESS, short preamble/slot)
  │   │   ├── SSID IE, Supported Rates IE, Extended Rates IE
  │   │   └── RSN IE (WPA2 only: CCMP + PSK)
  │   └── Poll for Association Response (AID)
  │
  ├── iwn_run(dev)                           [ASSOC → RUN]
  │   ├── RXON — associated (with AID + BSS filter)
  │   ├── ADD_NODE — AP node (id=0, AP MAC)
  │   └── LINK_QUALITY — AP full rate table (OFDM 54→6, CCK 11→1)
  │
  └── iwn_eapol_handshake(dev)               [WPA2 only]
```

#### TX Frame Mechanism

Management frames are sent on TX ring 0 via `iwn_tx_frame()`:

- Builds `iwn_cmd_data` TX params: `IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ`
- Uses `IWN5000_ID_BROADCAST` (id=15) as target node
- CCK 1 Mbps for reliability on management frames
- Frame copied into ring cmd buffer, referenced via single-segment TX descriptor
- Scheduler byte count table updated, hardware kicked via `IWN_HBUS_TARG_WRPTR`

#### RX Polling for Management Frames

`iwn_poll_mgmt()` polls the RX ring for management frames matching a specific subtype:

- Checks `IWN_MPDU_RX_DONE` notifications, filters by FC type=0 (management)
- Silently consumes `IWN_TX_DONE` completions
- Logs unexpected notification types for debugging
- Clears `IWN_INT` and `IWN_FH_INT` every iteration to keep DMA flowing

#### Association Request RSN IE

For WPA2 networks, the association request includes an RSN Information Element:

```
RSN IE (tag=48, 22 bytes):
├── Version: 1
├── Group Cipher: 00:0F:AC:04 (CCMP)
├── Pairwise Cipher Count: 1
├── Pairwise Cipher: 00:0F:AC:04 (CCMP)
├── AKM Count: 1
├── AKM: 00:0F:AC:02 (PSK)
└── RSN Capabilities: 0x0000
```

The RSN IE is saved in `dev->rsn_ie[]` for replay in the EAPOL Message 2.

#### Post-Association Configuration

After the AP sends an Association Response with AID, `iwn_run()` re-configures the firmware:

1. **RXON (associated)**: Same as auth RXON but adds `IWN_FILTER_BSS`, `IWN_RXON_SHPREAMBLE`, `IWN_RXON_SHSLOT`, and sets `associd = AID | 0xC000`
2. **ADD_NODE (AP, id=0)**: Creates the AP BSS node with the AP's MAC address — used for all data frame TX
3. **LINK_QUALITY (AP)**: Full descending rate table — OFDM 54/48/36/24/18/12/9/6 Mbps then CCK 11/5.5/2/1 Mbps. Firmware handles rate adaptation within this table

### Phase 4c: EAPOL 4-Way Handshake (code complete, untested on hardware)

WPA2-PSK EAPOL 4-way handshake. After association, the AP sends EAPOL Message 1. We respond with Messages 2 and 4, derive the PTK, decrypt the GTK from Message 3, and install both keys into the firmware for CCMP-encrypted data frames.

**File**: `iwn_eapol.c` (~420 lines)

#### Handshake Flow

```
iwn_eapol_handshake(dev)
  ├── Step 1: PMK = PBKDF2-SHA1(password, SSID, 4096, 32)
  │           (~0.5s on x86, status message printed)
  │
  ├── Step 2: Poll RX for EAPOL M1 (5s timeout)
  │   ├── Data frame (type=2) with LLC/SNAP ethertype 0x888E
  │   ├── Validate: descriptor=0x02, ACK set, MIC not set
  │   └── Extract ANonce (32 bytes) + replay counter (8 bytes)
  │
  ├── Step 3: Generate SNonce + derive PTK
  │   ├── SNonce = SHA-1(RDTSC || MAC || counter)  (32 bytes)
  │   ├── PTK = PRF-384(PMK, "Pairwise key expansion",
  │   │                  min(AA,SPA) || max(AA,SPA) ||
  │   │                  min(ANonce,SNonce) || max(ANonce,SNonce))
  │   └── Split: KCK=PTK[0:16], KEK=PTK[16:32], TK=PTK[32:48]
  │
  ├── Step 4: TX EAPOL M2
  │   ├── Key Info: 0x010A (VERSION=2, PAIRWISE, MIC)
  │   ├── Replay counter echoed from M1
  │   ├── Key data: RSN IE from association
  │   └── MIC = HMAC-SHA1(KCK, eapol_frame)[0:16]
  │
  ├── Step 5: Poll RX for EAPOL M3 (5s timeout)
  │   ├── Verify MIC: HMAC-SHA1(KCK, frame_with_zeroed_mic)
  │   ├── Decrypt key data: AES-unwrap(KEK, encrypted_data)
  │   ├── Parse KDE TLVs to find GTK KDE (OUI 00:0F:AC type 0x01)
  │   └── Extract GTK (16 bytes) + key index (bits 0-1)
  │
  ├── Step 6: TX EAPOL M4
  │   ├── Key Info: 0x030A (VERSION=2, PAIRWISE, MIC, SECURE)
  │   ├── Replay counter echoed from M3
  │   └── No key data, no nonce
  │
  └── Step 7: Install keys into firmware
      ├── ADD_NODE(id=0, update, TK, CCMP, key_id=0)    → pairwise
      └── ADD_NODE(id=15, update, GTK, CCMP, GROUP)      → group
```

#### EAPOL Frame Encapsulation

EAPOL travels inside 802.11 **data** frames (type=2), not management frames:

```
802.11 Data Header (24 bytes)
├── FC: 0x0801 (type=2 data, subtype=0, ToDS=1)
├── Duration: 0x0000
├── Addr1 (RA/BSSID): AP MAC
├── Addr2 (TA/SA): our MAC
├── Addr3 (DA): AP MAC
├── SeqCtrl: 0x0000 (firmware fills)
│
LLC/SNAP Header (8 bytes)
├── DSAP: 0xAA, SSAP: 0xAA, Control: 0x03
├── OUI: 00:00:00
├── EtherType: 0x888E (802.1X Authentication)
│
EAPOL Header (4 bytes)
├── Version: 0x01 (802.1X-2001)
├── Type: 0x03 (EAPOL-Key)
├── Body Length: (big-endian)
│
EAPOL-Key Body (95+ bytes)
├── [0]     Descriptor Type: 0x02 (RSN/WPA2)
├── [1-2]   Key Info (big-endian, see table below)
├── [3-4]   Key Length: 0x0010 (16 for CCMP)
├── [5-12]  Replay Counter (big-endian, 8 bytes)
├── [13-44] Key Nonce (32 bytes)
├── [45-60] Key IV (16 bytes, zeroed for AES)
├── [61-68] Key RSC (8 bytes)
├── [69-76] Reserved (8 bytes)
├── [77-92] Key MIC (16 bytes)
├── [93-94] Key Data Length (big-endian)
└── [95+]   Key Data (variable)
```

#### Key Info Bit Field

| Bit(s) | Name | M1 | M2 | M3 | M4 |
|--------|------|-----|-----|-----|-----|
| 0-2 | Version | 2 | 2 | 2 | 2 |
| 3 | Pairwise | 1 | 1 | 1 | 1 |
| 6 | Install | 0 | 0 | 1 | 0 |
| 7 | ACK | 1 | 0 | 1 | 0 |
| 8 | MIC | 0 | 1 | 1 | 1 |
| 9 | Secure | 0 | 0 | 1 | 1 |
| 12 | Encrypted | 0 | 0 | 1 | 0 |

Typical values: M1=0x008A, M2=0x010A, M3=0x13CA, M4=0x030A

#### Data Frame TX vs Management TX

EAPOL data frames use a different TX path than management frames:

| | Management (iwn_tx_frame) | EAPOL Data (iwn_tx_eapol) |
|---|---|---|
| Frame type | Type=0 (management) | Type=2 (data) |
| FC flags | None | ToDS=1 |
| Node ID | `IWN5000_ID_BROADCAST` (15) | `IWN_ID_BSS` (0, AP node) |
| Encapsulation | Raw 802.11 | 802.11 + LLC/SNAP + EAPOL hdr |
| Security | None | None (EAPOL is pre-key) |
| Rate | CCK 1 Mbps | CCK 1 Mbps |

Both use TX ring 0 with `IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ`.

#### SNonce Generation

The Dell E6510 (Nehalem) lacks `RDRAND`. The SNonce is generated using:

1. Read TSC via `__builtin_ia32_rdtsc()` (available in freestanding mode)
2. Concatenate: TSC bytes (8) + MAC address (6) + counter (4) + TSC again (4)
3. Hash with `iwn_sha1()` → first 20 bytes of SNonce
4. Second round with different counter + TSC → remaining 12 bytes

The SNonce does not need to be cryptographically random — the PMK and ANonce provide actual security. The SNonce only needs to be unique per handshake.

#### Key Installation

After Message 4 is sent, the PTK Temporal Key (TK) and GTK are installed into the firmware via `IWN_CMD_ADD_NODE` with update flags:

**Pairwise key (TK)** — installed on AP BSS node (id=0):
```c
node.id = IWN_ID_BSS;           // 0
node.control = IWN_NODE_UPDATE;
node.flags = IWN_FLAG_SET_KEY;
node.kflags = IWN_KFLAG_CCMP | IWN_KFLAG_KID(0);
node.key = TK;                  // PTK[32:48]
node.tsc = 0;
```

**Group key (GTK)** — installed on broadcast node (id=15):
```c
node.id = IWN5000_ID_BROADCAST; // 15
node.control = IWN_NODE_UPDATE;
node.flags = IWN_FLAG_SET_KEY;
node.kflags = IWN_KFLAG_CCMP | IWN_KFLAG_KID(gtk_kid) | IWN_KFLAG_GROUP;
node.key = GTK;
node.tsc = RSC_from_M3;         // replay counter for group frames
```

After key installation, the firmware handles CCMP encryption/decryption of all data frames automatically.

#### EAPOL RX Polling

`iwn_poll_eapol()` polls the RX ring for 802.11 data frames containing EAPOL:

1. Check `IWN_MPDU_RX_DONE` notification
2. Parse FC: must be type=2 (data), subtype=0 or 8 (QoS data)
3. Check LLC/SNAP at byte 24: must match `AA AA 03 00 00 00 88 8E`
4. Extract EAPOL frame (header + body) after LLC/SNAP
5. Also detects deauth frames (type=0, subtype=12) — AP deauthing means failure

#### Device State Fields

Phase 4b-c add these fields to `iwn_dev`:

```c
/* Phase 4: Association state */
uint8_t  state;              /* IWN_STATE_* (INIT→SCAN→AUTH→ASSOC→RUN) */
uint8_t  target_bssid[6];
char     target_ssid[33];
uint8_t  target_ssid_len;
uint8_t  target_channel;
uint16_t target_beacon_interval;
uint8_t  target_security;    /* from scan result */
uint16_t assoc_id;           /* AID from assoc response */
char     password[64];       /* WPA2 passphrase */
uint8_t  rsn_ie[32];         /* saved RSN IE for EAPOL replay */
uint8_t  rsn_ie_len;

/* Phase 4c: EAPOL state */
uint8_t  ptk[48];            /* Pairwise Transient Key (KCK+KEK+TK) */
uint8_t  gtk[16];            /* Group Temporal Key */
uint8_t  gtk_kid;            /* GTK key index (0-3) */
uint8_t  keys_installed;     /* 1 after PTK+GTK installed */
```

#### M3 Key Data Parsing

The AP's Message 3 key data is AES-wrapped and may contain multiple KDE (Key Data Encapsulation) entries:

```
Encrypted Key Data (AES-unwrap removes 8-byte IV prefix):
├── KDE: RSN IE (tag=0x30)           — AP's RSN capabilities
├── KDE: GTK (tag=0xDD)             — Group Temporal Key
│   ├── OUI: 00:0F:AC
│   ├── Data Type: 0x01
│   ├── Key ID + TX flag (1 byte)
│   ├── Reserved (1 byte)
│   └── GTK (16 bytes)
└── Optional: padding
```

The parser walks TLV entries looking for tag=0xDD with OUI 00:0F:AC and data type 0x01 to find the GTK.

#### Common Gotchas

| Bug | Prevention |
|-----|-----------|
| Data frame FC byte order | FC for data+ToDS: byte[0]=0x08, byte[1]=0x01 |
| LLC/SNAP missing | Data frame body MUST start with AA AA 03 00 00 00 88 8E |
| Key Info big-endian | All EAPOL-Key multi-byte fields are big-endian |
| MIC computed over wrong range | MIC covers EAPOL header + body (not 802.11 header) |
| MIC field not zeroed before compute | Must zero 16 MIC bytes, compute HMAC, then write MIC |
| PRF data ordering | Must use min/max of MAC addresses AND nonces (lexicographic) |
| Wrong node ID for data TX | Data frames use id=0 (AP BSS), not id=15 (broadcast) |
| AES-unwrap output size | Wrapped data includes 8-byte IV; plain is 8 bytes shorter |
| M3 key data isn't just GTK | Must walk TLV to find GTK KDE (may also contain RSN IE) |
| Replay counter not echoed | M2 echoes M1's counter; M4 echoes M3's counter |
| WPA2 without password | Must check `password[0]` before starting connect — fail early |

### Phase 4d: Data Frame TX/RX (done)

Ethernet-to-802.11 encapsulation (TX) and 802.11-to-Ethernet decapsulation (RX), plus a software receive buffer that Phase 5 (SNP) will read from. The firmware handles CCMP encrypt/decrypt — we just set the security field in the TX descriptor.

**File**: `iwn_data.c` (~280 lines)

REPL command: `iwn ping` (ARP smoke test)

#### API

| Function | Signature | Purpose |
|----------|-----------|---------|
| `iwn_tx_raw` | `(dev, frame, frame_len, node_id, security)` → `int` | Low-level: submit pre-built 802.11 frame on ring 0 |
| `iwn_encap_tx` | `(dev, dst_mac, ethertype, payload, payload_len)` → `int` | High-level: Ethernet-style TX with 802.11 + LLC/SNAP encapsulation |
| `iwn_rx_poll` | `(dev)` → `int` | Non-blocking: drain HW RX ring into software buffer, return count |
| `iwn_rx_pending` | `(dev)` → `int` | Return count of queued frames in software buffer |
| `iwn_rx_dequeue` | `(dev, buf, buf_len)` → `int` | Dequeue one Ethernet frame (calls `iwn_rx_poll` internally) |
| `iwn_ping_cmd` | `(void)` | REPL: send ARP request, wait 3s for reply |

#### TX Path: `iwn_tx_raw()`

Canonical ring 0 submit — the clean version of what `iwn_tx_frame()` and `iwn_tx_eapol()` each duplicated. Takes a pre-built 802.11 frame and submits it:

- Same ring 0 mechanics as existing code: descriptor at `ring->desc + idx * 128`, command at `ring->cmd + idx * 140`
- `cmd.code = IWN_CMD_TX_DATA` (28), `cmd.qid = 0`
- `tx->flags = IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ`
- `tx->id = node_id`, `tx->security = security`
- Rate: OFDM 24 Mbps (`plcp = 0x09`, `rflags = ANT(txant)`) — firmware uses LINK_QUALITY retry chain for rate fallback
- `tx->lifetime = IWN_LIFETIME_INFINITE`
- Scheduler byte count table updated for queue 0
- HW kick: `IWN_HBUS_TARG_WRPTR = ring->cur | (0 << 8)`

**2-segment TX for large frames**: Each ring entry is 140 bytes. After the 4-byte header and 56-byte `iwn_cmd_data`, only 80 bytes remain for inline frame data. Small frames (ARP at 60 bytes, management frames) fit inline using a single TX descriptor segment. Frames larger than 80 bytes (DHCP at 310 bytes, HTTP data) use 2 segments: segment 0 points to the ring buffer (header + cmd_data, 60 bytes), segment 1 points to a dedicated 4 KB DMA page (`dev->tx_data`). This follows OpenBSD's multi-segment approach, adapted for bare metal without mbufs.

#### TX Path: `iwn_encap_tx()`

High-level TX that takes Ethernet-style parameters and builds a complete 802.11 data frame:

```
802.11 Data Header (24 bytes)
├── FC[0] = 0x08 (type=2 data, subtype=0)
├── FC[1] = 0x41 if keys_installed (ToDS=1, Protected=1)
│         = 0x01 otherwise (ToDS=1 only)
├── Duration: 0x0000
├── Addr1 (RA) = target_bssid (AP receives)
├── Addr2 (TA) = mac_addr (we transmit)
├── Addr3 (DA) = dst_mac (final destination)
├── SeqCtrl = 0x0000 (firmware fills via AUTO_SEQ)
│
LLC/SNAP Header (8 bytes)
├── DSAP: 0xAA, SSAP: 0xAA, Control: 0x03
├── OUI: 00:00:00
├── EtherType: big-endian ethertype parameter
│
Payload (variable)
└── Raw payload bytes
```

**Node ID and security selection:**

| Destination | `dst[0] & 1` | Node ID | Key Used |
|-------------|-------------|---------|----------|
| Unicast | 0 | `IWN_ID_BSS` (0) | PTK (pairwise) |
| Broadcast/multicast | 1 | `IWN5000_ID_BROADCAST` (15) | GTK (group) |

If `keys_installed`, security = `IWN_CIPHER_CCMP` (2). Otherwise security = 0.

#### RX Path: `iwn_rx_poll()`

Non-blocking drain of the HW RX ring into a 16-slot software frame buffer. For each `IWN_MPDU_RX_DONE` notification:

1. Parse FC: check frame type and subtype
2. **Deauth detection** (type=0, subtype=12): set `state = IWN_STATE_INIT`, clear `keys_installed`
3. **Data frames** (type=2, subtype=0 or 8):
   - Handle QoS data (subtype=8): header is 26 bytes instead of 24
   - Skip EAPOL frames (ethertype 0x888E)
   - Verify LLC/SNAP prefix (`AA AA 03 00 00 00`)
   - Decapsulate FromDS → Ethernet frame:

```
FromDS Address Mapping (AP → us):
├── Addr1 (RA) = our MAC (destination)
├── Addr2 (TA) = AP BSSID (transmitter)
└── Addr3 (SA) = original source MAC

Decapsulated Ethernet Frame:
├── DST = Addr1 (our MAC, 6 bytes)
├── SRC = Addr3 (original source, 6 bytes)
├── EtherType = LLC/SNAP bytes 6-7
└── Payload = frame data after LLC/SNAP (8 bytes)
```

4. Queue into `dev->rx_buf[]` ring buffer (drop if full)
5. **TX completions** (`IWN_TX_DONE`): increment `tx_ok` counter

After draining, update `IWN_FH_RX_WPTR` and clear `IWN_INT`/`IWN_FH_INT`.

#### RX Path: `iwn_rx_dequeue()`

Consumer interface for Phase 5 (SNP Receive):

1. Call `iwn_rx_poll()` to drain any new HW frames
2. If `rx_head == rx_tail`, return -1 (no frames)
3. Copy frame from `dev->rx_buf[rx_head]` to caller's buffer
4. Advance `rx_head`

This matches UEFI's polling model — the SNP `Receive()` function just calls `iwn_rx_dequeue()`.

#### Software RX Buffer

```c
#define IWN_RX_BUF_COUNT  16
#define IWN_RX_BUF_SIZE   1518      /* max Ethernet frame */

uint8_t  rx_buf[16][1518];          /* ~24 KB ring buffer */
uint16_t rx_buf_len[16];            /* stored frame lengths */
int      rx_head;                    /* consumer reads here */
int      rx_tail;                    /* producer writes here */
uint32_t tx_ok;                      /* TX success counter */
uint32_t tx_fail;                    /* TX failure counter */
```

16 slots is sufficient for DHCP handshake and low-throughput inference API traffic. Drop-on-full policy — no backpressure to the hardware.

#### ARP Ping Smoke Test

The `iwn ping` REPL command verifies the data path end-to-end:

1. Build ARP request for 192.168.1.1 (28-byte ARP payload)
2. Send as broadcast via `iwn_encap_tx(dev, ff:ff:ff:ff:ff:ff, 0x0806, arp, 28)`
3. Poll `iwn_rx_dequeue()` for 3 seconds looking for ARP reply (opcode=2)
4. On reply: print sender IP and MAC address
5. Also prints ethertype/length of any non-ARP frames received

**Note:** Sender IP is 0.0.0.0 (gratuitous ARP style) because we don't have an IP yet — that comes from DHCP in Phase 5.

#### Firmware CCMP Handling

After Phase 4c installs CCMP keys via `IWN_CMD_ADD_NODE`, the firmware handles all per-frame encryption and decryption:

- **TX**: Setting `tx->security = IWN_CIPHER_CCMP` (2) tells the firmware to encrypt using the key installed for `tx->id` (node ID 0 = PTK, node ID 15 = GTK)
- **RX**: The firmware decrypts incoming CCMP frames and strips the CCMP header before delivering to us. We receive plaintext
- **No software crypto needed** for data frames — the Intel firmware does it all

#### Common Gotchas

| Bug | Prevention |
|-----|-----------|
| Protected bit not set | `FC[1] = 0x41` when `keys_installed`, `0x01` otherwise |
| Wrong security value | `IWN_CIPHER_CCMP = 2`, not 1 (that's WEP40) |
| Broadcast uses wrong node | `dst[0] & 1` → node_id=15 (group key), not node_id=0 |
| FromDS addresses swapped | RX: Addr1=us, Addr2=AP, Addr3=source (not Addr2=source) |
| QoS data header length | Subtype 8 has 2 extra bytes (QoS Control) — LLC/SNAP at offset 26, not 24 |
| Scheduler byte count | Must update BOTH layers: `sched[idx]` and `sched[idx + RING_COUNT]` |
| RX WPTR alignment | Must mask `& ~7` (8-entry aligned) |
| RX buffer full | New frames silently dropped — acceptable for low-throughput use case |
| ARP payload size | ARP is 28 bytes (not including Ethernet header) |
| EAPOL after handshake | Skip ethertype 0x888E in `iwn_rx_poll` to avoid double-processing |

### Phase 5: SNP Integration (done)

Wraps the IWN driver as `EFI_SIMPLE_NETWORK_PROTOCOL` so UEFI's built-in IP4/TCP4/UDP4 stack binds automatically. After `iwn connect`, DHCP runs and all existing BootAI networking (HTTP server, mDNS, DNS, fetch) works over WiFi with zero changes.

**File**: `iwn_snp.c` (~280 lines)

#### Architecture

```
  BootAI HTTP Server / mDNS / DNS / HTTP GET
          ↓
  UEFI TCP4 / UDP4 / IP4 Protocols
          ↓
  MnpDxe / ArpDxe / Ip4Dxe (UEFI firmware DXE drivers)
          ↓
  EFI_SIMPLE_NETWORK_PROTOCOL ← iwn_snp.c implements this
          ↓
  iwn_encap_tx() / iwn_rx_dequeue() (Phase 4d)
          ↓
  802.11 + CCMP (firmware handles encryption)
          ↓
  Intel 6200 hardware
```

The SNP layer is thin — it translates between UEFI's Ethernet-style Transmit/Receive interface and the Phase 4d data path that handles 802.11 encapsulation, LLC/SNAP headers, and CCMP encryption.

#### Automatic Connect Flow

After Phase 5, the full connect-to-networking sequence is automatic:

```
> iwn connect Herrera
  Connecting to 'Herrera' ch 11 ...
  Auth ok
  Assoc ok (AID=1)
  EAPOL msg 1-4: handshake complete
  PTK/GTK installed
  SNP installed                    ← Phase 5: SNP + device path installed
  Waiting for DHCP... OK           ← efi_net_init() runs automatically
  Network: ready
```

This is triggered by two lines added at the end of `iwn_connect()` (in `iwn_assoc.c`):

```c
iwn_snp_install();
efi_net_init();
```

#### SNP Function Table

| Function | Implementation | Notes |
|----------|---------------|-------|
| `Start` | Set State = STARTED | No-op — HW already running |
| `Stop` | Set State = STOPPED | No-op |
| `Initialize` | Set State = INITIALIZED | No-op — HW init done by firmware load |
| `Reset` | Return SUCCESS | No-op |
| `Shutdown` | Set State = STOPPED | No-op |
| `ReceiveFilters` | Store in Mode | Accept unicast + broadcast |
| `StationAddress` | Copy MAC | Reset copies PermanentAddress |
| `Statistics` | Return UNSUPPORTED | Not implemented |
| `MCastIpToMac` | Map IPv4 multicast | `01:00:5E:(IP[1]&0x7F):IP[2]:IP[3]` |
| `NvData` | Return UNSUPPORTED | Not implemented |
| `GetStatus` | Poll HW + recycle TX | Calls `iwn_rx_poll()`, returns recycled TX buffer |
| `Transmit` | Encapsulate + TX | Calls `iwn_encap_tx()` — handles both header modes |
| `Receive` | Dequeue + parse | Calls `iwn_rx_dequeue()` — fills out Ethernet header fields |

#### SNP Mode Initialization

The Mode struct is initialized to report an Ethernet-like device:

```c
snp_mode.State              = EFI_SIMPLE_NETWORK_STOPPED;
snp_mode.HwAddressSize      = 6;
snp_mode.MediaHeaderSize    = 14;
snp_mode.MaxPacketSize      = 1500;
snp_mode.IfType             = 6;   /* IANA ifType for Ethernet */
snp_mode.MediaPresent       = 1;
snp_mode.ReceiveFilterMask  = UNICAST | BROADCAST;
snp_mode.CurrentAddress     = dev->mac_addr;
snp_mode.PermanentAddress   = dev->mac_addr;
snp_mode.BroadcastAddress   = FF:FF:FF:FF:FF:FF;
```

State starts as STOPPED so DXE drivers (if present) can manage the full lifecycle: `Start() → Initialize() → Transmit/Receive`. The custom network stack (Phase 5c) calls `Start()` and `Initialize()` directly before using SNP.

#### TX Buffer Recycling

UEFI's IP stack passes a buffer to `Transmit()` and expects `GetStatus()` to return the same pointer when TX is done. Since IWN's TX is synchronous (copy into HW ring, kick firmware), the buffer is free immediately:

```c
#define TX_RECYCLE_COUNT 8
static void *tx_recycle[TX_RECYCLE_COUNT];
static int tx_recycle_head, tx_recycle_tail;
```

`Transmit()` records the buffer pointer in the ring. `GetStatus()` returns one pointer per call. 8 slots is more than enough — the UEFI IP stack typically has 1-2 TX buffers in flight.

#### Transmit HeaderSize Semantics

UEFI's `Transmit()` has two modes controlled by `HeaderSize`:

| HeaderSize | Meaning | Buffer Contains |
|-----------|---------|-----------------|
| 0 | Complete frame | Full Ethernet frame (14-byte header + payload) |
| != 0 | Header components separate | Payload only; DestAddr, SrcAddr, Protocol provided as parameters |

Both modes extract the destination MAC and ethertype, then call `iwn_encap_tx()` which builds the 802.11 frame with LLC/SNAP.

#### WaitForPacket Event

MnpDxe (and other DXE drivers) access the `WaitForPacket` event on the SNP protocol. This must be a valid `EVT_NOTIFY_WAIT` event — if NULL, MnpDxe crashes or hangs during driver binding.

```c
gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_CALLBACK, snp_wait_check, NULL, &snp_wait_event);
snp_protocol.WaitForPacket = snp_wait_event;
```

The `snp_wait_check` callback calls `iwn_rx_poll()` to check for pending frames.

#### Handle Installation

SNP is installed on a new UEFI handle with two protocols:

1. **SNP** (`gEfiSimpleNetworkProtocolGuid`): The protocol function table
2. **Device Path** (`gEfiDevicePathProtocolGuid`): MAC Address Device Path for driver binding

```
Device Path:
├── MAC Address Node (Type=3/Messaging, SubType=37)
│   ├── Length: 37 bytes (4 header + 32 MAC + 1 IfType)
│   ├── MAC: 00:27:10:56:AD:80 (+ 26 zero-padding to 32 bytes)
│   └── IfType: 6 (Ethernet)
└── End Entire Device Path (Type=0x7F, SubType=0xFF, Length=4)
```

**No `ConnectController` is called.** Earlier versions called `ConnectController(handle, NULL, NULL, TRUE)` to trigger MnpDxe binding, but this causes a hard hang on Dell E6510 (see Phase 5c). The custom network stack (Phase 5c) works directly on the SNP protocol without needing DXE driver binding.

#### UEFI Type Additions (efi.h)

Phase 5 adds ~80 lines to `efi.h`:

| Type | Purpose |
|------|---------|
| `EFI_MAC_ADDRESS` | 32-byte MAC address (UEFI spec, first 6 used) |
| `EFI_IP_ADDRESS` | IP address union (IPv4/IPv6) |
| `EFI_SIMPLE_NETWORK_MODE` | Read-only state (HwAddressSize, MAC, MediaPresent, etc.) |
| `EFI_SIMPLE_NETWORK_PROTOCOL` | 14 function pointers + WaitForPacket + Mode |
| SNP state constants | STOPPED=0, STARTED=1, INITIALIZED=2 |
| Receive filter bits | UNICAST, MULTICAST, BROADCAST, PROMISCUOUS |
| Interrupt status bits | RECEIVE_INTERRUPT, TRANSMIT_INTERRUPT |
| `InstallProtocolInterface` | Typed (was `void *`) — needed to install SNP |

#### GUID Definitions (net.c)

Two new GUIDs added to `efi/net.c`:

```c
EFI_GUID gEfiSimpleNetworkProtocolGuid = {
    0xA19832B9, 0xAC25, 0x11D3,
    {0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}
};

EFI_GUID gEfiDevicePathProtocolGuid = {
    0x09576E91, 0x6D3F, 0x11D2,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
```

#### Guard Against Double Install

`iwn_snp_install()` is guarded by a static `snp_installed` flag. This prevents issues if `iwn connect` is called multiple times (e.g., reconnect after deauth).

#### Known Risks

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Dell firmware lacks IP stack DXE drivers | CONFIRMED — IP stack won't bind to SNP | Custom network stack (Phase 5c) works directly on SNP |
| OVMF DXE drivers incompatible with Dell | CONFIRMED — ConnectController hangs, UDP4/TCP fail to load | Do not call ConnectController; use custom stack instead |
| WaitForPacket NULL | MnpDxe crashes during driver binding | Create proper `EVT_NOTIFY_WAIT` event (fixed) |
| SNP state must start STOPPED | DXE drivers expect to manage Start()/Initialize() lifecycle | State set to STOPPED; custom stack calls Start()/Initialize() itself |
| IfType mismatch | Some UEFI stacks check IfType strictly | Set to 6 (IANA Ethernet) |

#### Common Gotchas

| Bug | Prevention |
|-----|-----------|
| UEFI IP stack doesn't bind | Call `ConnectController(handle, NULL, NULL, TRUE)` after install |
| Transmit HeaderSize == 0 semantics | Buffer has complete Ethernet frame — parse DST/ethertype from bytes 0-13 |
| Transmit HeaderSize != 0 semantics | DestAddr and Protocol provided separately — Buffer is payload only |
| GetStatus TxBuf NULL crash | Check `TxBuf != NULL` before dereferencing |
| Mode State not checked | Each SNP function checks State and returns appropriate error |
| IfType wrong value | IANA ifType for Ethernet is 6, not 1 |
| MCastIpToMac mapping | IPv4 `224.x.y.z` → MAC `01:00:5E:(x & 0x7F):y:z` — mask bit 0 of second octet |
| SNP installed before keys ready | Only install after `iwn_connect()` succeeds — not during `iwn_probe()` |

### Phase 5b: Network Stack Diagnostics (done)

After SNP install, the UEFI IP stack (MnpDxe → ArpDxe → Ip4Dxe → Tcp4Dxe) must bind to the SNP handle for DHCP and TCP/UDP to work. On QEMU/OVMF this happens automatically because the IP stack DXE drivers are built into the firmware. On Dell E6510, these drivers may be absent — the firmware was designed for wired Ethernet NICs with their own driver stack.

**Problem**: `efi_net_init()` finds no `IP4Config2` protocol after IWN SNP install. No DHCP, no TCP, no UDP — networking is "unavailable" despite WiFi working at the radio level.

**Secondary issue**: `efi_net_init()` called `connect_all_controllers()` twice on ALL handles unconditionally, potentially re-triggering driver enumeration on the PCI WiFi device after IWN had already installed SNP and called `ConnectController` on the SNP handle.

**Files**: `efi/net_diag.c` (new, ~100 lines), changes to `efi/net.c`, `iwn/iwn_snp.c`, `efi/efi.h`, `efi/main.c`

#### `netdiag` Command

The `netdiag` REPL command enumerates 8 UEFI network protocol handles and prints counts:

```
> netdiag
  Network protocol handles:
  SNP            : 1
  MNP SB         : none
  ARP SB         : none
  IP4 SB         : none
  IP4Config2     : none
  DHCP4 SB       : none
  TCP4 SB        : none
  UDP4 SB        : none
  IWN SNP installed: yes
  Local IP: not assigned
```

Uses `LocateHandleBuffer(SEARCH_BY_PROTOCOL, &guid)` for each protocol. The expected workflow:

1. Boot Dell → `netdiag` → baseline (SNP=0, everything=0)
2. `iwn pass <pw>` → `iwn connect <SSID>` → WiFi connects, SNP installed
3. `netdiag` → see what appeared after SNP install
4. Read `bootai.log` from USB for full output

#### `connect_all_controllers` Guard

In `efi_net_init()`, the two `connect_all_controllers()` calls are now guarded:

```c
if (!iwn_snp_is_installed()) {
    connect_all_controllers();
    connect_all_controllers();
}
```

When IWN SNP is already installed, `iwn_snp_install()` has already called `ConnectController(snp_handle)` on the specific SNP handle. Broad controller enumeration is unnecessary and could interfere with the IWN PCI device binding.

The `iwn_snp_is_installed()` getter exposes the existing `static int snp_installed` flag from `iwn_snp.c`.

#### New Protocol GUIDs

Four new ServiceBinding GUIDs defined in `efi/net_diag.c`:

| Protocol | GUID | Purpose |
|----------|------|---------|
| MNP SB | `{F36FF770-A7E1-42CF-9ED2-56F0F271F44E}` | Managed Network Protocol — bridges SNP to ARP/IP |
| ARP SB | `{F44C00EE-1F2C-4A00-AA09-1C9F7E44E038}` | Address Resolution Protocol |
| IP4 SB | `{C51711E7-B4BF-404A-BFB8-0A048EF1FFE4}` | IPv4 stack |
| DHCP4 SB | `{9D9A39D8-BD42-4A73-A4D5-8EE94BE11380}` | DHCP client |

#### Three-Phase Strategy — Results

| Phase | What | Status | Result |
|-------|------|--------|--------|
| **1. Diagnostics + Fix** | `netdiag` command + `connect_all_controllers` guard | **DONE** | Dell lacks ALL IP stack protocols (MNP, ARP, IP4, TCP4, UDP4 = 0 handles) |
| **2. Bundle DXE Drivers** | Extract 6 EDK2 `.efi` files onto USB `\drivers\` | **TESTED — FAILED** | 4/6 loaded, 2 failed (Udp4, Tcp). ConnectController hangs on Dell firmware |
| **3. Custom Network Stack** | ARP/DHCP/IP/UDP/TCP on raw SNP | **NEXT** | Required — DXE drivers incompatible with Dell E6510 |

##### Phase 1 Results (netdiag on Dell E6510)

Before `iwn connect`: all 8 protocols = 0 handles.
After `iwn connect`: SNP = 1, everything else = 0.

Confirmed: Dell E6510 firmware has **zero** IP stack DXE drivers.

##### Phase 2 Results (EDK2 DXE drivers on Dell)

Six EDK2 DXE drivers extracted from OVMF placed on USB `\drivers\`:
```
01-MnpDxe.efi     loaded OK
02-ArpDxe.efi     loaded OK
03-Dhcp4Dxe.efi   loaded OK
04-Ip4Dxe.efi     loaded OK
05-Udp4Dxe.efi    StartImage failed
06-TcpDxe.efi     StartImage failed
```

Three boots tested with different SNP configurations:

1. **WaitForPacket=NULL, State=INITIALIZED**: ConnectController returned but MnpDxe never bound. MNP SB: 0 handles.
2. **WaitForPacket=valid event, State=STOPPED**: System hung at `ConnectController(snp_handle)`. Hang is inside MnpDxe — `SNP: Start()` never called.
3. **ConnectController removed**: No hang, SNP installs cleanly. But no IP stack (DXE drivers never bound to SNP).

**Root cause**: OVMF DXE drivers expect a full EDK2 runtime environment that Dell's firmware doesn't provide.

**Conclusion**: DXE driver approach is not viable on Dell E6510. Phase 3 (custom network stack) is required.

##### Phase 3 Result (Custom Network Stack — DONE)

ARP/DHCP/IP/UDP/TCP implemented directly on raw SNP via `iwn_snp_get_protocol()`. 7 new files in `efi/ns_*.c` (~1,155 lines total). Dual dispatch in `net.c` routes to custom stack when UEFI protocols are absent. Builds with zero warnings. See [Networking Architecture — Custom Network Stack](networking.md#custom-network-stack-phase-5c) for full details.

### Phase 5c: Custom Network Stack (done)

Complete ARP/DHCP/IPv4/UDP/TCP stack built on raw SNP frames, used when UEFI firmware lacks IP stack DXE drivers.

**Files**: `efi/ns_net.h`, `efi/ns_net.c`, `efi/ns_arp.c`, `efi/ns_ip.c`, `efi/ns_udp.c`, `efi/ns_dhcp.c`, `efi/ns_tcp.c` (~1,155 lines total). Plus modifications to `efi/net.c`, `efi/efi.h`, `efi/mdns.c`, `Makefile`.

#### Layer Architecture

| Layer | File | Lines | What It Does |
|-------|------|-------|-------------|
| Frame dispatch | `ns_net.c` | ~105 | `ns_init()`, `ns_poll()` EtherType dispatch, `ns_raw_send()` |
| ARP | `ns_arp.c` | ~130 | 16-entry table, blocking resolve (3s), auto-reply |
| IPv4 | `ns_ip.c` | ~145 | Subnet routing, multicast MAC, checksum, protocol dispatch |
| UDP | `ns_udp.c` | ~95 | Port binding table (4 entries), used by DHCP + mDNS |
| DHCP | `ns_dhcp.c` | ~280 | Full DHCP exchange (raw broadcast, no IP yet) |
| TCP | `ns_tcp.c` | ~400 | State machine: LISTEN→SYN_RCVD→ESTABLISHED→FIN_WAIT, MSS 1460 |

#### Dual Dispatch

`tcp4_conn_t` extended with `ns_tcp_conn *ns_conn` field. Each `tcp4_*()` function checks `conn->ns_conn` (or `ns_is_ready()` for listen) to route to the custom stack. UEFI path unchanged.

#### TCP Design

Minimal TCP for HTTP/1.0 server (single connection, Connection: close):
- Window size 8192 (matches rx_buf)
- No OOO reassembly, no window scaling, no Nagle, no congestion control
- ISN increments by 64000 per connection
- SYN+ACK includes MSS option (1460)

#### mDNS Updates

When custom stack is active, mDNS uses `ns_udp_bind(5353, callback)` and `ns_udp_send()` for multicast instead of UEFI UDP4 ServiceBinding.

## Key Design Decisions

### Why OpenBSD over Linux?

OpenBSD's `iwn` driver is ~7,000 lines in 3 files. Linux's `iwlwifi` is ~60,000 lines across 8+ subdirectories. OpenBSD's driver has been successfully ported to macOS (`itlwm` project), validating that it can work outside a Unix kernel.

### Why SNP instead of custom networking?

By implementing UEFI's standard network protocol, we get the entire TCP/IP stack for free — on firmware that has it. UEFI firmware already includes IP4, ARP, DHCP, TCP4, UDP4, and DNS4. All existing BootAI code (HTTP server, mDNS, API) works without modification. When the firmware lacks these drivers (Dell E6510), the custom `ns_*` stack provides the same functionality through a dual-dispatch layer in `net.c`.

### Why polling instead of interrupts?

UEFI boot services mode doesn't provide a clean interrupt handler mechanism. Polling `CSR_INT` at the REPL loop frequency (~100ms) is sufficient for our use case where inference is the bottleneck, not network latency.

---

## Changelog

| Date | Change |
|------|--------|
| 2026-02-23 | Phase 0 implemented: PCI probe, register headers, REPL command |
| 2026-02-23 | Phase 1 implemented: MMIO, APM, OTP, EEPROM read — MAC/domain/RF confirmed on Dell E6510 |
| 2026-02-24 | Phase 2 implemented: DMA rings, firmware parse (legacy v2), FH DMA upload, hw_init sequence |
| 2026-02-24 | Phase 2 bug fixes: 9 bugs found and fixed (wrong DMA channel, missing TXBUF_STATUS, NIC lock, DMA enable, shadow regs, VMAIN, nic_config, RFKILL, scheduler timing) |
| 2026-02-24 | Phase 2 calibration fix: add WiMAX coex command before calibration (required by 6000 series), fix RX ring buffer leak in `iwn_poll_rx`, fix calibration data length (`-4` not `-8`). Calibration now completes on Dell E6510 (1 result, 508 bytes). |
| 2026-02-24 | Phase 3 initial: big-command path, radio config (4 commands), scan command, beacon parsing, RSSI calculation, security detection, result display |
| 2026-02-24 | Phase 3 debugging: 10 bugs found on real hardware. Synchronous command waits (iwn_wait_cmd), simple BT coex for 6000 series, FH_INT clearing, 8-command config sequence (added LINK_QUALITY + TXPOWER_DBM + CRITICAL_TEMP), 802.11 probe frame header, CCK 1Mbps TX rate, IWN_FILTER_BEACON. See [debug log](../research/iwn-phase3-debug-log.md) |
| 2026-02-25 | Phase 4a implemented: WPA2-PSK crypto primitives — SHA-1, HMAC-SHA1, PBKDF2-SHA1, AES-128, AES Key Unwrap (RFC 3394), PRF-384. Self-contained module (~490 lines), 13/13 test vectors passing (FIPS, RFC, IEEE). No UEFI dependencies. |
| 2026-02-25 | Phase 4b implemented: 802.11 Open System authentication + association. RXON state machine (INIT→AUTH→ASSOC→RUN), TX/RX management frames on ring 0, RSN IE for WPA2, post-assoc AP node (id=0) + full rate table. Password check before connect. `iwn_assoc.c` (~700 lines). |
| 2026-02-25 | Phase 4c implemented: WPA2-PSK EAPOL 4-way handshake. PMK derivation (PBKDF2, 4096 rounds), EAPOL data frame TX/RX (LLC/SNAP 0x888E, node id=0), SNonce generation (RDTSC+SHA-1), PTK derivation (PRF-384), M2/M4 construction with HMAC-SHA1 MIC, M3 verification + AES-unwrap GTK extraction, pairwise+group key installation via ADD_NODE with CCMP flags. `iwn_eapol.c` (~420 lines). |
| 2026-02-25 | Phase 4d implemented: Data frame TX/RX. `iwn_tx_raw()` canonical ring 0 submit (OFDM 24 Mbps, CCMP security field). `iwn_encap_tx()` Ethernet-to-802.11 encapsulation with LLC/SNAP, Protected bit, broadcast/unicast node selection. `iwn_rx_poll()` non-blocking HW drain with 802.11-to-Ethernet decapsulation, QoS data handling (26-byte header), deauth detection. 16-slot software RX buffer (24 KB). `iwn ping` ARP smoke test command. `iwn_data.c` (~280 lines). |
| 2026-02-26 | Phase 5 implemented: SNP integration. `iwn_snp.c` (~280 lines) wraps IWN as `EFI_SIMPLE_NETWORK_PROTOCOL`. 14 SNP functions backed by `iwn_encap_tx()`/`iwn_rx_dequeue()`. TX buffer recycling ring (8 slots). MAC Address Device Path for driver binding. Auto DHCP via `efi_net_init()` after connect. Added SNP types + typed `InstallProtocolInterface` to `efi.h` (~80 lines). GUIDs in `net.c`. |
| 2026-02-26 | Phase 5b: Network stack diagnostics. `efi/net_diag.c` (~100 lines) enumerates 8 UEFI network protocol handles (SNP, MNP, ARP, IP4, IP4Config2, DHCP4, TCP4, UDP4). `netdiag` REPL command. Guard `connect_all_controllers` behind `iwn_snp_is_installed()` to prevent double driver enumeration. `iwn_snp_is_installed()` getter. 4 new ServiceBinding GUIDs (MNP, ARP, IP4, DHCP4). Three-phase strategy: diagnostics → bundle DXE drivers → custom network stack. |
| 2026-02-26 | Phase 5b Dell testing: `netdiag` confirmed Dell has zero IP stack drivers. EDK2 DXE drivers tested — 4/6 loaded but ConnectController hangs (MnpDxe incompatible with Dell firmware). Three boots tested with different SNP configs. Fix: removed ConnectController from `iwn_snp_install()`, added `iwn_snp_get_protocol()` getter. DXE approach abandoned — custom network stack (Phase 5c) required. |
| 2026-02-26 | Phase 5c implemented: Custom network stack on raw SNP. 7 new files (~1,155 lines): `ns_net.h` (types/declarations), `ns_net.c` (frame dispatch), `ns_arp.c` (16-entry ARP table), `ns_ip.c` (IPv4 routing/checksum), `ns_udp.c` (port binding), `ns_dhcp.c` (full DHCP client), `ns_tcp.c` (HTTP/1.0 TCP state machine). Dual dispatch in `net.c` — each `tcp4_*()` function routes to custom stack when `ns_is_ready()`. `tcp4_conn_t` extended with `ns_tcp_conn *ns_conn`. mDNS updated for custom UDP. Builds with zero errors/warnings. |
| 2026-02-26 | Phase 5c Dell testing (round 1): SNP function pointer table hangs on `snp->Start(snp)` — bypassed with direct IWN data path functions (`iwn_snp_get_mac`, `iwn_snp_direct_tx`, `iwn_snp_direct_rx`). `ns_net.c` rewritten to call IWN directly instead of through SNP protocol. See [Phase 5c debug log](../research/phase5-debug-log.md). |
| 2026-02-26 | Phase 5c Dell testing (round 2): DHCP DISCOVER sent but no OFFER received. Three bugs found: (1) MAC hex display out-of-bounds (`"0123456789AB"` → `"0123456789ABCDEF"`), (2) TX ring buffer overflow — 310-byte DHCP frame overflows 80-byte inline space, fixed with 2-segment TX descriptor + 4 KB DMA page (`dev->tx_data`), (3) IP destination filter drops DHCP OFFER during DHCP — fixed by accepting all IP packets when `ip_ready=0`. |
