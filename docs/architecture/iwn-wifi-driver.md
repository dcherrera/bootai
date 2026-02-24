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
├── iwn.h              # Public API (iwn_probe, iwn_init, iwn_attach, iwn_scan_cmd)
├── iwn_internal.h     # Shared internal header (MMIO, prph, lock, DMA, FW, scan functions)
├── iwn_shim.h         # UEFI shim: OpenBSD types → stdint, byte-order no-ops, bus_space stubs
├── if_iwnreg.h        # Hardware registers (from OpenBSD, with additions)
├── if_iwnvar.h        # iwn_dev struct, DMA ring structs, scan results, device ID table, flags
├── iwn_pci.c          # PCI enumeration, device matching, iwn_init() entry point
├── iwn_hw.c           # MMIO primitives, NIC lock, APM init/stop, EEPROM read, iwn_attach()
├── iwn_dma.c          # DMA memory allocation, TX/RX ring setup, scheduler table
├── iwn_fw.c           # Firmware parse/upload, hw_init sequence, calibration, host commands (big + small)
└── iwn_scan.c         # Radio config, scan command, beacon parsing, result display
```

Future phases will add:

```
├── iwn_crypto.c       # WPA2-PSK (PBKDF2, HMAC-SHA1, AES-128, 4-way handshake)
└── iwn_snp.c          # EFI_SIMPLE_NETWORK_PROTOCOL wrapper
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
15. Send crystal calibration (PHY_CALIB) via TX ring 4
16. Poll for CALIBRATION_DONE, save CALIBRATION_RESULT entries
17. Power cycle (APM stop → APM init → NIC config)
18. Reset rings, re-program hardware
19. Upload RUNTIME firmware, release CPU → poll for UC_READY
20. Clear RFKILL, activate scheduler, replay calibration results
21. Intel WiFi ready!
```

#### Calibration Flow

After init firmware is alive, the driver sends a crystal calibration command derived from EEPROM data:

```c
struct iwn5000_phy_calib_crystal crystal_cmd;
crystal_cmd.code = IWN5000_PHY_CALIB_CRYSTAL;  /* type 15 */
crystal_cmd.cap_pin[0] = eeprom_crystal & 0xff;
crystal_cmd.cap_pin[1] = (eeprom_crystal >> 16) & 0xff;
```

The firmware responds with `CALIBRATION_RESULT` (type 102) notifications containing calibration data, followed by `CALIBRATION_DONE` (type 103). The driver saves all calibration results and replays them to the runtime firmware after the power cycle.

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

#### Big Command Support

The scan command is ~900 bytes, but `iwn_tx_cmd.data[]` is only 136 bytes. The driver now supports big commands via 2-segment TX descriptors:

```
Small command (≤136 bytes):     Big command (>136 bytes):
┌──────────────┐                ┌──────────────┐    ┌──────────────┐
│ Segment 0    │                │ Segment 0    │    │ Segment 1    │
│ cmd header + │                │ cmd header   │    │ payload      │
│ payload      │                │ (4 bytes)    │    │ (from DMA    │
│ (from ring)  │                │ (from ring)  │    │  buffer)     │
└──────────────┘                └──────────────┘    └──────────────┘
desc->nsegs = 1                 desc->nsegs = 2
```

Segment 0 points to the 4-byte command header (code, flags, idx, qid) in the ring's `cmd` slot. Segment 1 points to the payload in a lazily-allocated 4 KB DMA page (`dev->cmd_dma`). The scheduler byte count covers both segments: `(4 + len + 3) / 4`.

#### Pre-Scan Radio Configuration

Four commands sent before the scan, all using the small-command (≤136 byte) path:

| # | Command | Code | Size | Purpose |
|---|---------|------|------|---------|
| 1 | RXON | 16 | ~50 B | Configure radio: STA mode, channel 1, 2.4 GHz, broadcast BSSID, RX chain config |
| 2 | TX_ANT_CONFIG | 152 | 4 B | Set TX antenna chain mask |
| 3 | ADD_NODE | 24 | ~92 B | Add broadcast node (ID 15, MAC FF:FF:FF:FF:FF:FF) |
| 4 | BT_COEX | 155 | ~65 B | Disable Bluetooth coexistence |

RXON flags: `IWN_RXON_24GHZ | IWN_RXON_AUTO | IWN_RXON_SHSLOT`
Filter: `IWN_FILTER_MULTICAST`
RX chain: `DRIVER_FORCE | VALID(chains) | FORCE_SEL(chains) | IDLE_COUNT(2) | MIMO_COUNT(2)`

#### Scan Command Layout

The scan command (code 128, `IWN_CMD_SCAN`) is a variable-length buffer built in `iwn_send_scan()`:

```
Offset   Size        Content
0        32 B        iwn_scan_hdr — nchan=11, rxchain, flags, filter, crc_threshold=0xffff
32       44 B        iwn_cmd_data — TX params for probe requests (6 Mbps OFDM, auto-seq)
76       680 B       20 × iwn_scan_essid — all zeroed (broadcast/wildcard scan)
756      16 B        Probe body — Supported Rates IE (8 rates) + Extended Rates IE (4 rates)
772      132 B       11 × iwn_scan_chan — channels 1-11, active scan, 30ms/110ms dwell
                     ─────────
Total    ~904 B      Sent via big-command path
```

Channel parameters: `IWN_CHAN_ACTIVE | IWN_CHAN_NPBREQS(2)`, `rf_gain=0x28`, `dsp_gain=0x50`.

The `crc_threshold` is set to `IWN_GOOD_CRC_TH_NEVER` (0xffff) to work around a firmware bug where `CRC_TH_DISABLED` (0) triggers a sysassert on radar channels.

#### RX Polling and Beacon Parsing

After sending the scan command, `iwn_poll_scan_results()` polls the RX ring for up to 8 seconds, handling these notification types:

| Type | Code | Action |
|------|------|--------|
| `IWN_START_SCAN` | 130 | Print "Scanning ch N" for each channel |
| `IWN_RX_PHY` | 192 | Cache RSSI: max of 3 antenna chains, subtract 44 dBm offset |
| `IWN_MPDU_RX_DONE` | 193 | Parse beacon frame from `iwn_rx_mpdu` → `iwn_parse_beacon()` |
| `IWN_STOP_SCAN` | 132 | Scan complete — print summary, return |
| Other | — | Ignored (stats, calibration remnants, etc.) |

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

#### Expected Output

```
> iwn scan
  Configuring radio...
  Scanning ch 1 2 3 4 5 6 7 8 9 10 11
  Scan complete: 11 channels, 5 networks found

  #  SSID                              CH  RSSI  Security
  1  HomeNetwork                        6  -42   WPA2
  2  CoffeeShop                         1  -68   Open
  3  Neighbor5G                         11  -75   WPA2
  4  (hidden)                            3  -80   WPA2
  5  ATT-XXXX                            9  -82   WPA+WPA2
```

#### Debug Guide

If scanning doesn't work on first try:

| Symptom | Likely Cause | Check |
|---------|-------------|-------|
| No `IWN_START_SCAN` (130) | RXON rejected or scan cmd malformed | Check `IWN_INT` for `SW_ERR` after RXON |
| `IWN_START_SCAN` but no `IWN_MPDU_RX_DONE` (193) | Radio not receiving | Check antenna config, RF/DSP gain values |
| `IWN_STOP_SCAN` with `nchan=0` | Firmware aborted scan | Check channel list validity |
| RSSI all -100 | PHY notifications missing | Firmware may not send `IWN_RX_PHY` on this API version |
| No scan results but frames received | Frame parsing issue | Check frame control, BSSID extraction offset |

**Incremental testing** if full scan fails:
1. Send only RXON → poll for any RX notification
2. Send RXON + scan → check for `IWN_START_SCAN`
3. Full scan with frame parsing

### Phase 4: Association + WPA2

- Implement RXON state machine (INIT → SCAN → AUTH → ASSOC → RUN)
- WPA2-PSK crypto: SHA-1, HMAC-SHA1, PBKDF2, AES-128, PRF-384
- 4-way EAPOL handshake
- Install temporal key for hardware CCMP

### Phase 5: SNP + Integration

- Implement `EFI_SIMPLE_NETWORK_PROTOCOL` (Transmit/Receive/GetStatus)
- Install SNP on new handle
- UEFI stack binds IP4/TCP4/UDP4 automatically
- Existing BootAI networking works over WiFi

## Key Design Decisions

### Why OpenBSD over Linux?

OpenBSD's `iwn` driver is ~7,000 lines in 3 files. Linux's `iwlwifi` is ~60,000 lines across 8+ subdirectories. OpenBSD's driver has been successfully ported to macOS (`itlwm` project), validating that it can work outside a Unix kernel.

### Why SNP instead of custom networking?

By implementing UEFI's standard network protocol, we get the entire TCP/IP stack for free. UEFI firmware already includes IP4, ARP, DHCP, TCP4, UDP4, and DNS4. All existing BootAI code (HTTP server, mDNS, API) works without modification.

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
| 2026-02-24 | Phase 3 implemented: big-command path (2-segment TX descriptors), radio config (RXON + node + antenna + BT coex), scan command (~900 bytes, 11 channels), beacon/probe response parsing, RSSI calculation, security detection (WPA/WPA2), sorted result display |
