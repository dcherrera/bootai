# IWN WiFi Driver — Phased Implementation Plan

Port OpenBSD's `iwn` driver to UEFI for Intel 6200 WiFi on the Dell E6510.

## Phase Overview

| Phase | Name | Deliverable | Status |
|-------|------|-------------|--------|
| 0 | Setup & Headers | Register headers, build system, PCI probe | **DONE** |
| 1 | MMIO & EEPROM | Read hardware registers, dump EEPROM/MAC address | **DONE** |
| 2 | DMA & Firmware | Allocate DMA rings, load firmware, get `UC_READY` | **DONE** |
| 3 | Scan | Send scan command, parse results, print SSIDs | **DONE** |
| 4 | Associate (Open) | Connect to an open network (no encryption) | Pending |
| 5 | Crypto | AES-128, SHA-1, HMAC, PBKDF2, PRF | Pending |
| 6 | WPA2-PSK | 4-way handshake, key install, encrypted data | Pending |
| 7 | SNP Integration | Wrap as EFI_SIMPLE_NETWORK_PROTOCOL, DHCP, full stack | Pending |
| 8 | Polish | Error handling, reconnect, wifi.c UI integration | Pending |

Each phase is self-contained and testable on real hardware (Dell E6510 via USB boot).

---

## Phase 0: Setup & Headers

**Goal:** Build system, register headers, PCI device detection.

### Tasks

- [x] Copy `if_iwnreg.h` and `if_iwnvar.h` from OpenBSD into `bootai/iwn/`
- [x] Strip OpenBSD kernel includes from headers, replace with `<stdint.h>` types
- [x] Create `iwn.h` — public API declarations for BootAI integration
- [x] Create `iwn_shim.h` — UEFI shim typedefs for `bus_space_tag_t`, `bus_addr_t`, `bus_size_t`
- [x] Create `iwn_pci.c` — PCI probe using `EFI_PCI_IO_PROTOCOL`:
  - `LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiPciIoProtocolGuid, ...)`
  - Walk handles, read vendor/device ID via `PciIo->Pci.Read()`
  - Match `0x8086:0x422C` or `0x8086:0x4239`
  - Print "Intel 6200 found" on match
- [x] Add `EFI_PCI_IO_PROTOCOL` definition to `efi/efi.h` (Pci.Read, Mem.Read/Write, AllocateBuffer, Map)
- [x] Add `iwn_pci.c` to Makefile
- [x] Add `iwn` REPL command to `efi/main.c` that calls `iwn_probe()`

### Test on Hardware

Boot Dell E6510 → `iwn` → should print "Intel 6200 found at BDF XX:XX.X"

### Results (2026-02-23)

- **Dell E6510**: Intel 6200 detected successfully on PCI bus
- **Surface Pro 7**: 21 PCI devices enumerated (all Intel 8086:xxxx + one 1e0f:0001). WiFi is `8086:34f0` (AX201, needs `iwx` driver — not `iwn`). No Intel 6200 match, as expected.

### Files

```
bootai/iwn/
├── if_iwnreg.h        ← copied from OpenBSD, stripped
├── if_iwnvar.h        ← copied from OpenBSD, stripped
├── iwn.h               ← public API
├── iwn_shim.h          ← UEFI shim types
└── iwn_pci.c           ← PCI enumeration
```

---

## Phase 1: MMIO & EEPROM

**Goal:** Read hardware registers and EEPROM. Confirm we're talking to the hardware.

### Tasks

- [x] Enable PCI MEMORY + BUS_MASTER attributes before BAR0 access
- [x] Implement MMIO access primitives:
  - `iwn_read(dev, reg)` → `PciIo->Mem.Read(EfiPciIoWidthUint32, 0, reg, 1, &val)`
  - `iwn_write(dev, reg, val)` → `PciIo->Mem.Write(...)`
  - `iwn_write_1(dev, reg, val)` — 8-bit BAR0 write
  - `iwn_setbits(dev, reg, mask)` / `iwn_clrbits(dev, reg, mask)` — read-modify-write
- [x] Implement peripheral register access (NIC-internal at 0x3000+):
  - `iwn_prph_read(dev, addr)` / `iwn_prph_write(dev, addr, val)` — via PRPH_RADDR/WADDR registers
  - Address flag: `IWN_PRPH_DWORD | addr`
  - Compiler barrier after address writes
- [x] Implement `iwn_nic_lock()` / `iwn_nic_unlock()` — MAC_ACCESS_REQ handshake via `IWN_GP_CNTRL`
- [x] Implement `iwn_clock_wait()` — set INIT_DONE, poll MAC_CLOCK_READY
- [x] Implement `iwn_hw_prepare()` — take NIC from Intel AMT (NIC_READY → PREPARE → retry)
- [x] Find PCIe capability offset for ASPM L1 check
- [x] Implement `iwn_apm_init()` — full power-on sequence:
  - GIO_CHICKEN workarounds (DIS_L0S_TIMER, L1A_NO_L0S_RX)
  - HPET threshold
  - HAP_WAKE_L1A
  - PCIe ASPM L1 check → set/clear GIO L0S_ENA
  - Clock wait
  - NIC lock → DMA clock enable → L1A disable → NIC unlock
- [x] Implement `iwn_apm_stop()` — stop master, SW reset, clear INIT_DONE
- [x] Implement `iwn_eeprom_lock()` / `iwn_eeprom_unlock()`
- [x] Implement `iwn_init_otprom()` — OTP init for 6000+ series (APMG reset, auto clock gate, clear ECC)
- [x] Implement `iwn_read_prom_data()` — read 16-bit words with READ_VALID polling and OTP ECC checking
- [x] Implement `iwn_attach()` — full Phase 1 sequence:
  - Disable interrupts
  - Read HW_REV → set chip-specific chain masks
  - hw_prepare → check OTP → apm_init → verify ROM signature
  - EEPROM lock → OTP init → read MAC, SKU, RF config, regulatory domain, calibration
  - APM stop → EEPROM unlock → print results
- [x] Expand `if_iwnvar.h` with `iwn_dev` struct and full device ID table (~38 cards)
- [x] Rewrite `iwn_pci.c` to match against ID table and call `iwn_attach()`

### Test on Hardware

Boot → `iwn` → prints MAC address, HW type, regulatory domain, RF config.

### Results (2026-02-23)

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

### Bugs Encountered

1. **HW type 31 (all 1s)**: BAR0 reads returned `0xffffffff` because PCI MMIO wasn't enabled. Fix: call `Attributes(Enable, MEMORY | BUS_MASTER)` before any register access.
2. **Multiple definition of `iwn_mcs2ridx`**: Non-static array in `if_iwnreg.h` included by both `.c` files. Fix: add `static` qualifier.

### Files

```
bootai/iwn/
├── if_iwnvar.h        ← iwn_dev struct, device ID table (~38 IDs), flag constants
├── iwn_pci.c          ← PCI probe, attribute enable, call iwn_attach()
└── iwn_hw.c           ← MMIO, prph, NIC lock, APM, OTP, EEPROM, iwn_attach() (~340 lines)
```

---

## Phase 2: DMA & Firmware Loading

**Goal:** Allocate DMA rings, load firmware onto the chip, get `IWN_UC_READY`.

### Tasks

- [x] Implement DMA allocation below 4GB:
  - `AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr)` with max `0xFFFFFFFF`
  - Identity-mapped in UEFI (virt == phys), no IOMMU needed
  - `iwn_dma_free()` → `gBS->FreePages()`
- [x] Implement `iwn_alloc_rx_ring()` — 64 RX descriptors + status page + 64 pre-allocated 4KB buffers
- [x] Implement `iwn_alloc_tx_ring()` — 256 TX descriptors (128B each) + 256 commands (140B each) per queue
- [x] Implement `iwn_alloc_sched()` — TX scheduler byte count table (320 entries per queue × 20 queues)
- [x] Implement `iwn_alloc_kw()` — "keep warm" page (4KB)
- [x] Implement firmware parser (`iwn_fw_parse`):
  - Load `\drivers\iwn-6000` from EFI filesystem via `efi_read_file()`
  - Auto-detect format: legacy v2 (first word != 0) or TLV (first word == 0, check signature)
  - Extract sections: init text, init data, runtime text, runtime data
  - The 6200 uses legacy v2 format (API 4), not TLV
- [x] Implement `iwn5000_load_firmware()`:
  - Upload firmware sections via FH DMA service channel 9 (`IWN_SRVC_DMACHNL`)
  - 128KB chunk transfers with DMA idle polling
  - Release CPU from reset (`IWN_RESET = 0`) to start firmware execution
- [x] Implement RX ring notification polling (`iwn_poll_rx`):
  - Read `closed_count` from RX status page (hardware-updated)
  - Walk RX ring buffers, parse `iwn_rx_desc.type`
  - Handle `IWN_UC_READY` (type 1), `IWN5000_CALIBRATION_RESULT` (type 102), `IWN5000_CALIBRATION_DONE` (type 103)
  - Save calibration results for replay to runtime firmware
  - Update `IWN_FH_RX_WPTR` after consuming buffers
- [x] Implement host command sending (`iwn_cmd`) via TX ring 4:
  - Build `iwn_tx_cmd` (code + payload) and `iwn_tx_desc` (single segment)
  - TX descriptor len field: `(totlen << 4) | HIADDR` encoding (not raw byte count)
  - Update scheduler byte count table
  - Kick hardware via `IWN_HBUS_TARG_WRPTR`
- [x] Implement `iwn5000_nic_config()` — write radio/antenna config to `IWN_HW_IF_CONFIG`
- [x] Implement `iwn_mem_read/write/set_region_4` — NIC-internal SRAM access via `IWN_MEM_RADDR/WADDR`
- [x] Implement `iwn5000_post_alive()` — full post-alive sequence:
  - Clear RFKILL (3x)
  - Read scheduler base from firmware (`IWN_SCHED_SRAM_ADDR`)
  - Clear scheduler context in SRAM, set DRAM address, reset all 20 queue pointers
  - Set window/limit, FIFO mapping for queues 0-6, enable TXFACT + interrupts
  - Send crystal calibration (`IWN_CMD_PHY_CALIB`) and `IWN5000_CMD_CALIB_CONFIG` (code 101)
  - Poll for `IWN5000_CALIBRATION_DONE`
- [x] Implement `iwn_replay_calibration()` — replay saved calib results to runtime firmware
- [x] Implement `iwn_hw_init()` — full 18-step hardware init sequence:
  - APM init → NIC config → DMA alloc → stop FH → program rings → clear interrupts
  - Parse firmware → load init firmware → poll UC_READY → post-alive calibration
  - Power cycle → reset rings → re-program → load runtime firmware → poll UC_READY
  - Full scheduler setup for runtime → replay calibration

### Test on Hardware

Boot → `iwn` → prints "Init firmware alive!" + "Runtime firmware alive!" + "Intel WiFi ready."

### Results (2026-02-24)

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
  DMA allocated: KW, sched, TX x2, RX (64 buffers)
  Loading firmware: drivers/iwn-6000 (454608 bytes)
  FW format: legacy v2, API 4
  Init text: 145124 bytes, Init data: 81920 bytes
  Main text: 145616 bytes, Main data: 81920 bytes
  Init firmware alive!
  iwn: calibration timeout
  Runtime firmware alive!
  Intel WiFi ready.
```

- Both init and runtime firmware alive — Phase 2 goal achieved
- Calibration timeout was resolved in Phase 3 (missing WiMAX coex command before calibration)
- Total DMA allocation: ~2.5 MB below 4GB
- Firmware file: `iwlwifi-6000-4.ucode` (454,608 bytes, legacy v2, API 4)

### Bugs Encountered (9 total for UC_READY, 5 for calibration)

**UC_READY fixes (init firmware wouldn't boot):**

1. **Wrong DMA channel**: Used FH channel 0 instead of service channel 9 (`IWN_SRVC_DMACHNL`). The 5000+ series uploads firmware via the service channel, not the regular TX channels.
2. **Missing TXBUF_STATUS write**: FH DMA requires `IWN_FH_TXBUF_STATUS` to be programmed with `TBNUM(1) | TBIDX(1) | TFBD_VALID` before starting transfer.
3. **NIC lock not held during FH writes**: The FH register programming sequence must be done under `iwn_nic_lock()` to prevent concurrent access issues.
4. **Missing DMA channel enable**: Channels 0-7 must be enabled with `DMA_ENA | DMA_CREDIT_ENA` before firmware load. Without this, the RX ring can't receive notifications.
5. **Missing shadow register enable**: 6000+ series requires shadow register control bit (`0x800fffff`) for write pointer updates to propagate.
6. **Missing VMAIN power source**: `IWN_APMG_PS` PWR_SRC_MASK must be cleared to select VMAIN power.
7. **Wrong iwn5000_nic_config**: Was clearing `RADIO_SI`/`MAC_SI` bits instead of setting them. Also needed `IWN_APMG_DIGITAL_SVR_DIS_EARLY_PWROFF` to prevent early poweroff.
8. **Missing RFKILL clear after firmware alive**: RF kill override must be cleared 3x after firmware boots.
9. **Premature scheduler activation**: TX scheduler DRAM address was being set before firmware was alive. Firmware must be running before scheduler setup.

**Calibration fixes (still timing out — deferred to Phase 3):**

1. **TX descriptor len encoding**: `segs[0].len` was raw byte count, should be `(totlen << 4) | HIADDR(cmd_paddr)`. Firmware couldn't read command payloads.
2. **Missing CALIB_CONFIG command**: Crystal calibration alone doesn't trigger the calibration sequence. Must send `IWN5000_CMD_CALIB_CONFIG` (code 101) with all enable/start/send/apply flags set.
3. **Incomplete scheduler setup**: Missing SRAM context clear, queue pointer resets, FIFO mapping, chain/interrupt config. Full setup needed for firmware to process commands.
4. **Missing RX WPTR update**: After consuming RX buffers, must write `IWN_FH_RX_WPTR` to tell hardware those slots are available. Without this, hardware runs out of RX buffers.
5. **Simplified runtime scheduler**: Runtime firmware had a 4-line scheduler activation instead of the full setup. Now uses same full sequence as init firmware.

### Files

```
bootai/iwn/
├── iwn_internal.h     ← shared internal header (MMIO, prph, lock, APM, mem, DMA, FW declarations)
├── iwn_dma.c          ← DMA allocation, ring setup/reset/free (~200 lines)
├── iwn_fw.c           ← FW parse, FH DMA upload, hw_init, post_alive, cmd, poll_rx (~730 lines)
└── iwn_hw.c           ← MMIO, prph, NIC lock, APM, EEPROM, NIC config, mem access (~570 lines)
```

---

## Phase 3: Scanning

**Goal:** Scan for WiFi networks, display SSIDs and signal strength.

### Tasks

- [x] `iwn_cmd()` — host command sending via TX queue 4 (implemented in Phase 2)
- [x] Debug calibration timeout — resolved: missing WiMAX coex command before calibration
- [x] Implement `iwn_wait_cmd()` — synchronous command wait (poll RX ring for response)
- [x] Send 8 pre-scan configuration commands (matching OpenBSD `iwn_config()` exactly):
  - `IWN_CMD_BT_COEX` (155) — simple Bluetooth coexistence (12-byte `iwn_bluetooth` struct for 6000 series)
  - `IWN5000_CMD_TX_ANT_CONFIG` (152) — TX antenna chain mask
  - `IWN_CMD_RXON` (16) — STA mode, 2.4 GHz, channel 1, broadcast BSSID, HT masks
  - `IWN_CMD_ADD_NODE` (24) — broadcast node (ID 15)
  - `IWN_CMD_LINK_QUALITY` (78) — CCK 1Mbps rate table for broadcast node
  - `IWN_CMD_TXPOWER_DBM` (149) — 16 dBm TX power
  - `IWN_CMD_SET_CRITICAL_TEMP` (164) — 110°C safety threshold
  - `IWN_CMD_SET_POWER_MODE` (119) — CAM (continuously awake)
- [x] Implement big command support for scan (>136 bytes via ring buffer DMA)
- [x] Implement `iwn_send_scan()`:
  - Build `IWN_CMD_SCAN` (128) with channel list (2.4 GHz: channels 1-11)
  - Proper 802.11 probe request frame header (24 bytes) + SSID IE + rates IEs
  - CCK 1Mbps TX rate with `IWN_RFLAG_CCK | IWN_RFLAG_ANT(B)` for 2.4 GHz
  - Scan header with `IWN_FILTER_BEACON`, quiet_time=10ms, crc_threshold=NEVER
  - ~938 bytes total, sent async via big-command path
- [x] Implement scan result parsing:
  - Poll RX ring for `IWN_START_SCAN` (130), `IWN_RX_PHY` (192), `IWN_MPDU_RX_DONE` (193), `IWN_STOP_SCAN` (132)
  - Parse beacon/probe response frames (802.11 management frame TLV walk)
  - Extract SSID (tag 0), channel (tag 3), WPA2 (tag 48), WPA (tag 221/OUI)
  - RSSI: max of 3 antenna chains, subtract IWN_RSSI_TO_DBM (44)
  - Clear both IWN_INT and IWN_FH_INT every iteration for continued DMA
- [x] Store scan results in `dev->scan_results[]` (max 32 networks, deduplicated by BSSID)
- [x] Print results sorted by RSSI: SSID, channel, RSSI, security type
- [x] Wire to REPL command: `iwn scan`

### Test on Hardware

Boot → `iwn` → `iwn scan` → lists nearby WiFi networks.

### Results (2026-02-24)

```
> iwn scan
  INT before scan: 0x8c000001
  Configuring radio...
  BT_COEX ok
  TX_ANT ok
  RXON: flags=0x40008005 chain=0x2806 sz=50
  RXON ok
  ADD_NODE ok
  rx[192]  rx[193]  rx[192]  rx[193]  rx[192]  rx[193]  LINKQ ok
  rx[192]  TXPOW ok
  rx[192]  rx[193]  rx[192]  rx[193]  CTEMP ok
  rx[192]  rx[193]  POWER ok
  SCAN cmd: len=938
  rx[128]  Scanning ch 1  rx[131] 2  rx[131] 3  rx[131] 4  rx[131] 5  rx[131] 6
  rx[131] 7  rx[131] 8  rx[131] 9  rx[131] 10  rx[131] 11  rx[131]
  Scan complete: 11 channels, 7 networks found

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
- Beacons (rx[192]/rx[193]) stream during config waits — radio is active
- rx[128] = scan command acknowledged, rx[131] = per-channel scan complete
- RSSI values are proper negative dBm (-32 strong to -86 weak)

### Bugs Encountered (10 total)

See [IWN Phase 3 Debug Log](../docs/research/iwn-phase3-debug-log.md) for full details.

| # | Bug | Fix |
|---|-----|-----|
| 1 | Fire-and-forget commands → SW_ERR | `iwn_wait_cmd()` polls for response |
| 2 | BT_COEX_PRIOTABLE (cmd 204) crashes 6000 FW | Remove — 6005/6050 only |
| 3 | Advanced btcoex struct (~80B) crashes 6000 FW | Use simple `iwn_bluetooth` (12B) |
| 4 | FH_INT not cleared → DMA stalls | Clear `IWN_FH_INT` (0x010) in all loops |
| 5 | Missing LINK_QUALITY cmd | Add cmd 78 (CCK 1Mbps rate table) |
| 6 | Missing TXPOWER_DBM cmd | Add cmd 149 (16 dBm) |
| 7 | Missing CRITICAL_TEMP cmd | Add cmd 164 (110°C) |
| 8 | Missing 802.11 probe frame header | Add 24-byte MAC header + SSID IE |
| 9 | Wrong TX rate (OFDM 6Mbps) | CCK 1Mbps: `plcp=10, rflags=CCK\|ANT(B)` |
| 10 | Missing IWN_FILTER_BEACON | Add to scan header filter |
| 11 | RSSI wrong data offset | Cast through `iwn_rx_stat.phybuf` |
| 12 | RSSI missing -44 offset | Formula: `max(rssi) - agc - 44` |

### Files

```
bootai/iwn/
└── iwn_scan.c          ← Radio config, scan command, beacon parsing, result display (~786 lines)
```

---

## Phase 4: Association (Open Network)

**Goal:** Connect to an open (unencrypted) WiFi network and exchange data frames.

### Tasks

- [ ] Implement `iwn_auth()`:
  - Set BSSID, channel in `IWN_CMD_RXON`
  - Send `IWN_CMD_TIMING` with beacon interval
  - Update RXON filter flags for BSS mode
- [ ] Implement `iwn_run()`:
  - Send `IWN_CMD_RXON_ASSOC` (0x11) with association ID
  - `IWN_CMD_ADD_NODE` (0x18) for the AP node
  - `IWN_CMD_LINK_QUALITY` (0x4E) — rate table for the AP
  - Start calibration
- [ ] Implement `iwn_newstate()` state machine:
  - INIT → SCAN → AUTH → ASSOC → RUN
  - Handle transition callbacks
- [ ] Implement 802.11 management frame TX:
  - Build Authentication frame (open system: seq 1)
  - Build Association Request frame (SSID + rates)
  - Send via TX ring
- [ ] Implement 802.11 management frame RX:
  - Parse Authentication Response
  - Parse Association Response (extract AID)
- [ ] Implement data frame TX path:
  - Accept Ethernet frame
  - Add 802.11 header (to_ds=1, BSSID, SA, DA)
  - Submit to TX ring with rate info
- [ ] Implement data frame RX path:
  - Receive 802.11 data frame from RX ring
  - Strip 802.11 header
  - Deliver as Ethernet frame
- [ ] Test with an open hotspot (no encryption)

### Test on Hardware

Boot → `wifi connect OpenNetwork` → should associate. Manually send/receive frames (no TCP/IP yet).

### Files

```
bootai/iwn/
├── iwn_assoc.c         ← Association state machine
└── iwn_data.c          ← Data frame TX/RX path
```

---

## Phase 5: Crypto Primitives

**Goal:** Implement the crypto algorithms needed for WPA2-PSK.

### Tasks

- [ ] Implement SHA-1 (FIPS 180-4):
  - `sha1_init()`, `sha1_update()`, `sha1_final()`
  - Test vector: `SHA1("abc") = a9993e36...`
- [ ] Implement HMAC-SHA1 (RFC 2104):
  - `hmac_sha1(key, key_len, data, data_len, mac)`
  - Test vector from RFC 2202
- [ ] Implement PBKDF2-SHA1 (RFC 2898):
  - `pbkdf2_sha1(passphrase, ssid, ssid_len, iterations, output, output_len)`
  - 4096 iterations, 256-bit output
  - Test vector: PBKDF2("password", "IEEE", 4096) = `f42c...`
- [ ] Implement AES-128 (FIPS 197):
  - `aes128_encrypt(key, plaintext, ciphertext)` — single 16-byte block
  - Test vector from FIPS 197 Appendix B
- [ ] Implement AES key unwrap (RFC 3394):
  - Needed for GTK decryption in EAPOL message 3
- [ ] Implement PRF-384 (IEEE 802.11i):
  - `prf384(key, label, data, output)` — based on HMAC-SHA1
  - Derives PTK from PMK + ANonce + SNonce + MAC addresses
- [ ] Write unit tests for each primitive (can run on host, not just UEFI)
- [ ] Create test target in Makefile: `make test-iwn-crypto`

### Test on Host (macOS)

Build crypto tests with native compiler, run test vectors. No hardware needed.

### Files

```
bootai/iwn/
├── iwn_crypto.c        ← SHA-1, HMAC, PBKDF2, AES, PRF
├── iwn_crypto.h        ← Crypto API
└── test_crypto.c       ← Test vectors (host-runnable)
```

---

## Phase 6: WPA2-PSK 4-Way Handshake

**Goal:** Complete WPA2-PSK authentication and install encryption keys.

### Tasks

- [ ] Implement EAPOL frame parsing:
  - Detect EAPOL-Key frames (EtherType 0x888E)
  - Parse key descriptor: type, info, length, replay counter, nonce, MIC, key data
- [ ] Implement 4-way handshake state machine:
  - **State 0 → Msg 1 received**: Extract ANonce from AP
  - **State 1 → Send Msg 2**:
    - Generate SNonce (random 32 bytes — use hardware RNG or UEFI RNG protocol)
    - Derive PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(ANonce,SNonce))
    - Build EAPOL-Key frame with SNonce
    - Compute MIC = HMAC-SHA1(KCK, EAPOL frame) — truncated to 16 bytes
    - Send Msg 2
  - **State 2 → Msg 3 received**:
    - Verify MIC
    - Decrypt key data (AES key unwrap with KEK) to get GTK
    - Send Msg 4 (empty, with MIC)
  - **State 3 → Handshake complete**:
    - Install PTK (temporal key, bytes 32-47) via `IWN_CMD_ADD_NODE` with `IWN_CIPHER_CCMP`
    - Install GTK for broadcast/multicast
- [ ] Implement random number generation:
  - Try `EFI_RNG_PROTOCOL` first
  - Fallback: use TSC + EEPROM data as entropy seed
- [ ] Implement PSK derivation:
  - On `wifi connect <SSID>` + password entry:
  - PMK = PBKDF2-SHA1(password, SSID, SSID_len, 4096, 32)
  - Cache PMK for reconnection
- [ ] Integrate with association flow:
  - Associate first (Phase 4 flow)
  - After association, wait for EAPOL Msg 1
  - Run 4-way handshake
  - On success, mark connection as secured

### Test on Hardware

Boot → `wifi connect MyWPA2Network` → enter password → should complete handshake and show "Connected (WPA2)".

### Files

```
bootai/iwn/
└── iwn_wpa.c           ← EAPOL parsing, 4-way handshake, key install
```

---

## Phase 7: SNP Integration

**Goal:** Expose the driver as `EFI_SIMPLE_NETWORK_PROTOCOL` so UEFI's TCP4/UDP4/IP4 stack works.

### Tasks

- [ ] Define `EFI_SIMPLE_NETWORK_PROTOCOL` in `efi/efi.h`:
  - SNP mode structure: state, HwAddressSize, MediaHeaderSize, MaxPacketSize, etc.
  - Function pointers: Start, Stop, Initialize, Reset, Shutdown, ReceiveFilters, StationAddress, Statistics, MCastIpToMac, NvData, GetStatus, Transmit, Receive
- [ ] Implement SNP wrapper in `iwn_snp.c`:
  - `iwn_snp_start()` → `iwn_init()` (power on, load firmware)
  - `iwn_snp_initialize()` → scan for saved network, associate, WPA2 handshake
  - `iwn_snp_transmit(header, buf, len)`:
    - Receive Ethernet frame (header + payload)
    - Encapsulate in 802.11 data frame
    - Submit to TX ring
  - `iwn_snp_receive(header, buf, len)`:
    - Poll RX ring for data frames
    - Decapsulate 802.11 → Ethernet
    - Copy to caller's buffer
  - `iwn_snp_get_status()`:
    - Poll `CSR_INT` register
    - Process TX completions, RX notifications
    - Report interrupt status to UEFI
  - `iwn_snp_stop()` → `iwn_stop()` (deassociate, power down)
- [ ] Install SNP on a new handle:
  - `gBS->InstallProtocolInterface(&handle, &gEfiSnpGuid, EFI_NATIVE_INTERFACE, &iwn_snp)`
- [ ] Call `ConnectController` to bind IP4/TCP4/UDP4 on top
- [ ] Set DHCP policy on the new interface
- [ ] Wait for DHCP to complete
- [ ] Verify: existing `efi_net_init()` picks up the WiFi interface

### Test on Hardware

Boot → WiFi auto-connects → `netinfo` shows IP → `serve` starts HTTP + mDNS → `curl http://bootai.local:8080/health` from Mac.

### Files

```
bootai/iwn/
└── iwn_snp.c           ← SNP protocol wrapper
```

---

## Phase 8: Polish & Integration

**Goal:** Error handling, reconnection, UI integration, documentation.

### Tasks

- [ ] Handle connection failures gracefully:
  - Scan timeout → retry
  - Association rejection → report reason code
  - WPA2 handshake failure → report and retry
  - Deauthentication → attempt reconnect
- [ ] Implement beacon miss detection:
  - Handle `IWN_BEACON_MISSED` notification
  - Attempt reassociation after N misses
- [ ] Integrate with existing `efi/wifi.c`:
  - `efi_wifi_available()` → return 1 if iwn probe succeeded
  - `efi_wifi_scan()` → call iwn scan, print results
  - `efi_wifi_connect(ssid, password)` → full connect flow
  - `efi_wifi_disconnect()` → deassociate
  - `efi_wifi_auto_connect()` → read `wifi.conf`, connect to saved network
  - `efi_wifi_status()` → show connected SSID, signal, IP
- [ ] Auto-connect on boot:
  - If iwn probe succeeds and `wifi.conf` exists, auto-connect before REPL
  - Print "WiFi: connected to MyNetwork (192.168.1.x)"
- [ ] Calibration:
  - Implement `iwn_send_sensitivity()` and `iwn_set_gains()`
  - Run calibration timer (every 500ms while connected)
  - Improves signal quality over time
- [ ] Documentation:
  - Update `bootai/CLAUDE.md` with iwn build instructions
  - Update `bootai/docs/architecture/networking.md` with WiFi architecture
  - Update `bootai/README.md` with WiFi feature
- [ ] Update `bootai/docs/specs/bootai-roadmap.md` — mark WiFi phase complete

### Test on Hardware

Full end-to-end: Boot USB → auto-connect WiFi → `serve` → chat from Mac via `bootai.local`.

---

## Build Integration

### Makefile additions (phased)

```makefile
# Phase 0
IWN_SRC = iwn/iwn_pci.c

# Phase 1
IWN_SRC += iwn/iwn_hw.c

# Phase 2
IWN_SRC += iwn/iwn_dma.c iwn/iwn_fw.c

# Phase 3
IWN_SRC += iwn/iwn_cmd.c iwn/iwn_scan.c

# Phase 4
IWN_SRC += iwn/iwn_assoc.c iwn/iwn_data.c

# Phase 5-6
IWN_SRC += iwn/iwn_crypto.c iwn/iwn_wpa.c

# Phase 7
IWN_SRC += iwn/iwn_snp.c

# Add to ALL_SRC
ALL_SRC = $(EFI_SRC) $(LIBC_SRC) ... $(IWN_SRC)
```

### Crypto test target (host-runnable)

```makefile
test-iwn-crypto: iwn/iwn_crypto.c iwn/test_crypto.c
	cc -o test_crypto iwn/iwn_crypto.c iwn/test_crypto.c && ./test_crypto
```

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| ~~Firmware won't load (DMA timing)~~ | ~~Blocks Phase 2+~~ | ~~Resolved: 9 bugs fixed, both init+runtime firmware alive~~ |
| ~~EEPROM read fails on E6510~~ | ~~Blocks Phase 1~~ | ~~Resolved: OTP path works, MAC/domain/RF read successfully~~ |
| ~~Scan returns no results~~ | ~~Blocks Phase 3~~ | ~~Resolved: 10 bugs fixed. Scan works — 7 networks found on Dell E6510~~ |
| WPA2 handshake timing | Blocks Phase 6 | AP typically retries; add timeout + retry logic |
| SNP binding fails | Blocks Phase 7 | May need `InstallMultipleProtocolInterfaces` with device path |
| ~~Not enough memory for DMA~~ | ~~Blocks Phase 2~~ | ~~Resolved: ~2.5MB allocated below 4GB, no issues~~ |
| ~~Calibration timeout~~ | ~~Degrades Phase 3+~~ | ~~Resolved: missing WiMAX coex command before calibration~~ |
| ~~RSSI calculation off~~ | ~~Minor Phase 3~~ | ~~Resolved: wrong data offset + missing IWN_RSSI_TO_DBM. Formula: max(rssi) - agc - 44~~ |
| Dell E6510 firmware quirks | Any phase | Test each phase on hardware, not just QEMU |

## Dependencies

- **Firmware file**: Download `iwlwifi-6000-4.ucode` from linux-firmware repo, rename to `iwn-6000`
- **OpenBSD source**: Clone headers from `github.com/openbsd/src`
- **Dell E6510**: Physical hardware for every phase test
- **Open WiFi network**: For Phase 4 testing (or phone hotspot with no password)
- **WPA2 network**: For Phase 6 testing (home WiFi)
