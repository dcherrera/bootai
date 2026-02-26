# IWN WiFi Driver — Phased Implementation Plan

Port OpenBSD's `iwn` driver to UEFI for Intel 6200 WiFi on the Dell E6510.

## Phase Overview

| Phase | Name | Deliverable | Status |
|-------|------|-------------|--------|
| 0 | Setup & Headers | Register headers, build system, PCI probe | **DONE** |
| 1 | MMIO & EEPROM | Read hardware registers, dump EEPROM/MAC address | **DONE** |
| 2 | DMA & Firmware | Allocate DMA rings, load firmware, get `UC_READY` | **DONE** |
| 3 | Scan | Send scan command, parse results, print SSIDs | **DONE** |
| 4a | Crypto Primitives | SHA-1, HMAC-SHA1, PBKDF2, AES-128, AES Unwrap, PRF-384 | **DONE** |
| 4b | Auth + Association | RXON state machine, 802.11 auth/assoc frames | **CODE COMPLETE** |
| 4c | WPA2 Handshake | PMK derivation, EAPOL 4-way, key install | **CODE COMPLETE** |
| 4d | Data Frame TX/RX | 802.11 data encap/decap, TX ring 0 | Pending |
| 5 | SNP Integration | Wrap as EFI_SIMPLE_NETWORK_PROTOCOL, DHCP, full stack | Pending |
| 6 | Polish | Error handling, reconnect, wifi.c UI integration | Pending |

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

## Phase 4: Association + WPA2

**Goal:** Connect to the home WiFi network (WPA2-PSK) and exchange encrypted data frames. This is the core networking phase — after this, the Intel 6200 can send and receive IP packets.

The phase is split into four sub-phases, each independently testable:

| Sub-phase | Name | Test Method | Hardware Needed |
|-----------|------|-------------|-----------------|
| 4a | Crypto primitives | Host unit tests (`make test-iwn-crypto`) | No |
| 4b | Auth + association | Dell E6510 → `iwn connect Herrera` | Yes |
| 4c | WPA2 handshake | Dell E6510 → full connect with password | Yes |
| 4d | Data frame TX/RX | Dell E6510 → `iwn ping` (manual ARP/ICMP) | Yes |

---

### Phase 4a: Crypto Primitives

**Goal:** Implement SHA-1, HMAC-SHA1, PBKDF2, AES-128, AES Key Unwrap, and PRF-384. All testable on the Mac host — no hardware needed.

#### Tasks

- [x] Implement SHA-1 (FIPS 180-4):
  - `iwn_sha1_init(ctx)`, `iwn_sha1_update(ctx, data, len)`, `iwn_sha1_final(ctx, digest)`
  - Streaming interface: 512-bit blocks, 80 rounds, W[16..79] expansion via ROTL1
  - One-shot convenience: `iwn_sha1(data, len, digest)`
- [x] Implement HMAC-SHA1 (RFC 2104):
  - `iwn_hmac_sha1(key, key_len, data, data_len, mac)`
  - `HMAC(K, text) = SHA1((K ^ opad) || SHA1((K ^ ipad) || text))`
  - ipad=0x36, opad=0x5C; keys > 64 bytes hashed to 20 bytes first
- [x] Implement PBKDF2-SHA1 (RFC 2898):
  - `iwn_pbkdf2_sha1(password, password_len, salt, salt_len, iterations, output, output_len)`
  - WPA2: 4096 iterations, 32-byte output (two PBKDF2 blocks: 20 + 12 bytes)
  - T initialized from U1, then XORed with U2..U_c (not zero-initialized)
- [x] Implement AES-128 (FIPS 197):
  - `iwn_aes128_init(ctx, key)` — precompute 44-word key schedule
  - `iwn_aes128_encrypt(ctx, in, out)` / `iwn_aes128_decrypt(ctx, in, out)`
  - Static S-box (256 bytes), inverse S-box (256 bytes), Rcon[10]
  - Column-major state: `state[r + 4*c]`
  - MixColumns via `xtime(a) = (a << 1) ^ ((a >> 7) * 0x1b)`
- [x] Implement AES Key Unwrap (RFC 3394 Section 2.2.2):
  - `iwn_aes_unwrap(kek, cipher, cipher_len, plain)` — returns 0 on success, -1 on integrity failure
  - Default IV: `0xA6 × 8`, 6 rounds over n 64-bit blocks
  - Only unwrap needed — we're the supplicant
- [x] Implement PRF-384 (IEEE 802.11i Section 8.5.1.1):
  - `iwn_prf384(key, label, label_len, data, data_len, output)`
  - 3 × HMAC-SHA1(key, label || 0x00 || data || i) → 60 bytes → truncate to 48
- [x] Write host-runnable unit tests (13 test vectors):
  - SHA-1: 3 tests — `"abc"`, `""`, 56-byte two-block (all FIPS 180-4)
  - HMAC-SHA1: 3 tests — RFC 2202 cases 1-3
  - PBKDF2: 2 tests — IEEE 802.11i (`"password"/"IEEE"`, `"ThisIsAPassword"/"ThisIsASSID"`)
  - AES-128: 3 tests — FIPS 197 App B encrypt, roundtrip decrypt, NIST SP 800-38A F.1.1
  - AES Unwrap: 1 test — RFC 3394 Section 4.1 (128-bit KEK + 128-bit data)
  - PRF-384: 1 test — consistency check against manual HMAC-SHA1
- [x] Create Makefile target: `make test-iwn-crypto` (native cc, no cross-compiler)
- [x] Verify EFI cross-compilation: `make efi` — no warnings from `iwn_crypto.c`

**RNG deferred to Phase 4c** — needs UEFI (`EFI_RNG_PROTOCOL`) or x86 (`RDTSC`), belongs in `iwn_wpa.c`.

#### Crypto NOT Needed

- **HMAC-MD5**: WPA only (Key Descriptor Version 1). WPA2 uses HMAC-SHA1 (Version 2).
- **RC4**: WPA TKIP only. WPA2 uses AES Key Wrap.
- **AES-CMAC**: 802.11w PMF only. Not needed for basic WPA2-PSK.
- **SHA-256**: Only for WPA2 with SHA256-based AKM (00:0F:AC:06). Standard PSK uses SHA-1.
- **CCMP encrypt/decrypt**: Firmware handles this after key install via `IWN_CMD_ADD_NODE`.

#### Results (2026-02-25)

```
$ make test-iwn-crypto
IWN Crypto Test Suite
=====================

SHA-1:
  [ 1] sha1_abc                       PASS
  [ 2] sha1_empty                     PASS
  [ 3] sha1_two_blocks                PASS

HMAC-SHA1:
  [ 4] hmac_sha1_case1                PASS
  [ 5] hmac_sha1_case2                PASS
  [ 6] hmac_sha1_case3                PASS

PBKDF2-SHA1:
  [ 7] pbkdf2_ieee1                   PASS
  [ 8] pbkdf2_ieee2                   PASS

AES-128:
  [ 9] aes128_encrypt                 PASS
  [10] aes128_decrypt                 PASS
  [11] aes128_nist                    PASS

AES Key Unwrap:
  [12] aes_unwrap                     PASS

PRF-384:
  [13] prf384                         PASS

=====================
Results: 13/13 passed
```

- All 13 test vectors pass on host (Apple clang, `-std=c11 -Wall -Wextra -O2`)
- EFI cross-compilation (`x86_64-elf-gcc`) produces zero warnings from `iwn_crypto.c`
- Module is fully self-contained: own `crypto_memcpy`/`crypto_memset`/`crypto_memcmp`, no libc

#### Code Size

| Component | Lines |
|-----------|-------|
| `iwn_crypto.h` | 65 |
| `iwn_crypto.c` | ~490 |
| `test_crypto.c` | ~280 |
| **Total** | **~835** |

#### Files

```
bootai/iwn/
├── iwn_crypto.h        ← API: SHA-1, HMAC-SHA1, PBKDF2, AES-128, AES Unwrap, PRF-384
├── iwn_crypto.c        ← Self-contained implementation (no libc, no UEFI)
└── test_crypto.c       ← 13 standard test vectors (FIPS, RFC, IEEE)
```

---

### Phase 4b: Authentication + Association

**Goal:** Send 802.11 management frames to authenticate and associate with an AP. Even WPA2 networks use Open System authentication first — the real security is the EAPOL handshake after association.

#### 802.11 Association Flow

```
                    iwn_auth()
                    ├── RXON (BSSID=target, chan=target, STA mode)
                    ├── TIMING (beacon interval)
                    ├── ADD_NODE (broadcast, id=15)
                    └── LINK_QUALITY (broadcast)

STA ───────────────────────────────────────────────── AP
 │  Authentication Request  (algo=0, seq=1, status=0)  │
 │ ──────────────────────────────────────────────────>  │
 │  Authentication Response (algo=0, seq=2, status=0)   │
 │ <──────────────────────────────────────────────────  │
 │                                                      │
 │  Association Request (SSID + Rates + RSN IE)         │
 │ ──────────────────────────────────────────────────>  │
 │  Association Response (status=0, AID)                │
 │ <──────────────────────────────────────────────────  │

                    iwn_run()
                    ├── RXON_ASSOC (flags|=BSS, associd=AID)
                    ├── ADD_NODE (AP, id=0, BSSID MAC)
                    └── LINK_QUALITY (AP, full rate table)
```

#### Tasks

- [x] Add state fields to `iwn_dev` in `if_iwnvar.h`:
  - `uint8_t state` — `IWN_STATE_INIT/SCAN/AUTH/ASSOC/RUN`
  - `uint8_t target_bssid[6]` — AP we're connecting to
  - `uint8_t target_channel` — AP's channel
  - `uint8_t target_ssid[32]`, `uint8_t target_ssid_len`
  - `uint16_t assoc_id` — AID from association response (14-bit, mask 0x3FFF)
  - `uint16_t beacon_interval` — from scan results (typically 100 TU)
  - `struct iwn_rxon rxon` — saved RXON config (updated per state)
  - `uint8_t pmk[32]` — pre-shared key material (for Phase 4c)
- [x] Store beacon_interval during scan (update `iwn_parse_beacon` in `iwn_scan.c`):
  - Beacon fixed fields: bytes 24-31 = timestamp, 32-33 = beacon interval, 34-35 = capability
  - Save `beacon_interval` in `iwn_scan_result`
- [x] Implement `iwn_auth()` — configure radio for BSS mode:
  - Update saved RXON: set `bssid` = target AP, `chan` = target channel, `mode` = `IWN_MODE_STA`
  - Clear `IWN_FILTER_BSS` (not associated yet), `associd` = 0
  - Keep flags: `IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF | IWN_RXON_AUTO | IWN_RXON_24GHZ`
  - Send `IWN_CMD_RXON` (code 16, `IWN5000_RXONSZ` bytes)
  - Send `IWN_CMD_TIMING` (code 20) with beacon interval from scan
  - Send `IWN_CMD_ADD_NODE` (code 24) — broadcast node (id=15, MAC FF:FF:FF:FF:FF:FF)
  - Send `IWN_CMD_LINK_QUALITY` (code 78) — CCK 1Mbps rate table for broadcast
  - All commands synchronous via `iwn_wait_cmd()`
- [x] Implement management frame TX via TX ring 0:
  - `iwn_tx_mgmt(dev, frame, frame_len)` — submit 802.11 management frame
  - Build `iwn_cmd_data` TX descriptor: `flags = IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ`
  - Rate: CCK 1Mbps (`plcp=10, rflags=IWN_RFLAG_CCK | IWN_RFLAG_ANT(txant)`)
  - Station ID: `IWN5000_ID_BROADCAST` (15) for auth, `IWN_ID_BSS` (0) after AP node added
  - `security = 0` (management frames are never encrypted in basic WPA2)
  - Submit on TX ring 0 (data queue, not command queue 4)
  - **Note:** TX ring 0 descriptors + commands were allocated in Phase 2 but ring not activated. Need to activate queue 0 in scheduler (SCHED_QUEUE_STATUS, TXFACT bit 0).
- [x] Build Authentication Request frame (30 bytes):
  ```
  802.11 Header (24 bytes):
    FC = 0x00B0 (mgmt, subtype=11 authentication)
    Duration = 0x0000
    Addr1 (DA) = target BSSID
    Addr2 (SA) = our MAC
    Addr3 (BSSID) = target BSSID
    SeqCtrl = 0x0000 (firmware fills via AUTO_SEQ)
  Fixed Fields (6 bytes):
    Auth Algorithm = 0x0000 (Open System)
    Auth Seq Number = 0x0001
    Status Code = 0x0000
  ```
- [x] Parse Authentication Response (RX ring, `IWN_MPDU_RX_DONE` type 193):
  - Check FC subtype == 11 (authentication)
  - Check auth seq == 2 (response) and status == 0 (success)
  - Status != 0 → print reason, abort
- [x] Build Association Request frame (~80-120 bytes):
  ```
  802.11 Header (24 bytes):
    FC = 0x0000 (mgmt, subtype=0 association request)
    Addr1/Addr3 = BSSID, Addr2 = our MAC
  Fixed Fields (4 bytes):
    Capability Info = 0x0431 (ESS | short preamble | short slot)
    Listen Interval = 0x000A (10 beacon intervals)
  Information Elements:
    [Tag 0]   SSID (target SSID bytes)
    [Tag 1]   Supported Rates (1/2/5.5/11/6/9/12/18 Mbps)
    [Tag 50]  Extended Rates (24/36/48/54 Mbps)
    [Tag 48]  RSN IE (22 bytes):
              Version = 1
              Group Cipher = 00:0F:AC:04 (CCMP)
              Pairwise Count = 1, Cipher = 00:0F:AC:04 (CCMP)
              AKM Count = 1, Suite = 00:0F:AC:02 (PSK)
              RSN Capabilities = 0x000C
  ```
  - The RSN IE tells the AP we support WPA2-PSK with CCMP
  - Same RSN IE must be replayed in EAPOL msg 2 (save a copy)
- [x] Parse Association Response:
  - Check FC subtype == 1 (association response)
  - Extract status code (bytes 24-25). Status 0 = success.
  - Extract AID (bytes 26-27, mask with 0x3FFF)
  - Status != 0 → print reason code, abort (common: 17 = AP full, 12 = wrong SSID)
- [x] Implement `iwn_run()` — post-association firmware config:
  - Update saved RXON: `associd = AID | 0xC000`, add `IWN_FILTER_BSS`
  - Add `IWN_RXON_SHSLOT | IWN_RXON_SHPREAMBLE` if AP supports them
  - Send `IWN_CMD_RXON_ASSOC` (code 17, `struct iwn_rxon_assoc`) — fast-path update
  - Send `IWN_CMD_ADD_NODE` (code 24) — AP node (id=0, MAC=BSSID, no keys yet)
  - Send `IWN_CMD_LINK_QUALITY` (code 78) — rate table for AP node:
    - Descending retry chain: OFDM 54→48→36→24→18→12→9→6, then CCK 11→5.5→2→1
    - `antmsk_1stream = IWN_LSB(txchainmask)`, `ampdu_max = NO_AGG`
  - Set state = `IWN_STATE_RUN`
- [x] Wire to REPL: `iwn connect <SSID>` (or by scan result number)
  - Select target from `dev->scan_results[]`
  - If WPA2: prompt for password (store for Phase 4c)
  - Run auth + association sequence
  - Print "Associated with <SSID> (AID=N)" on success

#### Test on Hardware

Boot → `iwn` → `iwn scan` → `iwn connect Herrera` → should print "Associated with Herrera (AID=1)".

For WPA2 networks, association succeeds but the AP will deauthenticate after ~1 second if no EAPOL handshake follows. This is expected — Phase 4c adds the handshake.

**Fallback test:** If no WPA2 testing is possible at this stage, temporarily create a phone hotspot with no password to verify open association.

#### Files

```
bootai/iwn/
└── iwn_assoc.c         ← iwn_auth(), iwn_run(), mgmt frame TX/RX, association state machine
```

---

### Phase 4c: WPA2-PSK 4-Way Handshake

**Goal:** After association, complete the EAPOL 4-way handshake to derive and install encryption keys.

#### 4-Way Handshake Flow

```
Step 0: PMK = PBKDF2-SHA1(password, SSID, 4096, 32)  [before association]

STA ──────────────────────────────────────────────────── AP
 │  (associated, waiting for EAPOL)                       │
 │                                                        │
 │  EAPOL Message 1 (ANonce)                              │
 │ <────────────────────────────────────────────────────  │
 │  [Extract ANonce, generate SNonce]                     │
 │  [PTK = PRF-384(PMK, "Pairwise key expansion",        │
 │         Min(AA,SPA)||Max(AA,SPA)||                     │
 │         Min(ANonce,SNonce)||Max(ANonce,SNonce))]        │
 │  [PTK = KCK(16) || KEK(16) || TK(16)]                 │
 │                                                        │
 │  EAPOL Message 2 (SNonce, RSN IE, MIC)                 │
 │ ────────────────────────────────────────────────────>  │
 │                                                        │
 │  EAPOL Message 3 (ANonce, encrypted GTK, MIC)          │
 │ <────────────────────────────────────────────────────  │
 │  [Verify MIC, decrypt GTK via AES Key Unwrap(KEK)]     │
 │                                                        │
 │  EAPOL Message 4 (MIC)                                 │
 │ ────────────────────────────────────────────────────>  │
 │                                                        │
 │  [Install PTK → firmware CCMP for unicast]             │
 │  [Install GTK → firmware CCMP for broadcast]           │
 │  Connection secured!                                   │
```

#### EAPOL-Key Frame Structure (99+ bytes)

```c
struct eapol_key {
    /* EAPOL header (4 bytes) */
    uint8_t  version;      /* 2 = 802.1X-2004 */
    uint8_t  type;         /* 3 = EAPOL-Key */
    uint16_t length;       /* big-endian, body length after this field */

    /* Key descriptor (95 bytes minimum) */
    uint8_t  desc_type;    /* 2 = RSN */
    uint16_t info;         /* big-endian, Key Info bitfield */
    uint16_t keylen;       /* big-endian, 16 for CCMP */
    uint8_t  replaycnt[8]; /* big-endian, 64-bit counter */
    uint8_t  nonce[32];    /* ANonce (msg 1,3) or SNonce (msg 2) */
    uint8_t  iv[16];       /* zeros for RSN */
    uint8_t  rsc[8];       /* key RSC */
    uint8_t  reserved[8];
    uint8_t  mic[16];      /* HMAC-SHA1(KCK, frame)[0:15] */
    uint16_t paylen;       /* big-endian, key data length */
    /* uint8_t payload[];   variable-length key data */
};
```

Key Info bitfield:
```
Bits 0-2:  Key Descriptor Version (2 = HMAC-SHA1 + AES Key Wrap)
Bit  3:    Key Type (1 = Pairwise)
Bit  6:    Install (1 in msg 3)
Bit  7:    Key ACK (1 in msg 1, 3)
Bit  8:    Key MIC (1 in msg 2, 3, 4)
Bit  9:    Secure (1 in msg 3, 4)
Bit  12:   Encrypted Key Data (1 in msg 3)
```

#### Tasks

- [x] Implement PSK derivation:
  - `iwn_derive_pmk(password, ssid, ssid_len, pmk_out)`
  - `PMK = PBKDF2-SHA1(password, SSID, 4096, 32)`
  - Compute once on `iwn connect`, cache in `dev->pmk[32]`
  - ~1-2 seconds on E6510 (Westmere i5, 4096 iterations of HMAC-SHA1)
- [x] Implement EAPOL frame detection in RX path:
  - EAPOL arrives as 802.11 data frame with LLC/SNAP EtherType `0x888E`
  - 802.11 header (24 bytes) + LLC/SNAP (8 bytes) + EAPOL frame
  - Data frames have FC type=2 (data), check `from_ds=1` (AP→STA)
  - Register EAPOL handler in `iwn_poll_rx` for `IWN_MPDU_RX_DONE` type 193
- [x] Implement 4-way handshake state machine (in `iwn_eapol.c`):
  - **Message 1 received** (Key ACK=1, Key MIC=0):
    - Extract ANonce (32 bytes) from `eapol_key.nonce`
    - Save replay counter
    - Generate SNonce (32 random bytes via `iwn_get_random`)
    - Derive PTK:
      ```
      B = Min(AA, SPA) || Max(AA, SPA) || Min(ANonce, SNonce) || Max(ANonce, SNonce)
      PTK = PRF-384(PMK, "Pairwise key expansion", B)  [48 bytes]
      KCK = PTK[0:15]   — Key Confirmation Key (MIC)
      KEK = PTK[16:31]  — Key Encryption Key (unwrap)
      TK  = PTK[32:47]  — Temporal Key (CCMP data)
      ```
      - AA = AP BSSID (6 bytes), SPA = our MAC (6 bytes)
      - Min/Max = lexicographic byte comparison
  - **Send Message 2**:
    - Build EAPOL-Key: version=2, type=3, desc_type=2
    - Key Info: Version=2 | Pairwise | MIC (= `0x010A`)
    - Set nonce = SNonce, replay counter = same as msg 1
    - Append RSN IE (saved from association request) as key data
    - Zero MIC field, compute `MIC = HMAC-SHA1(KCK, entire_eapol)[0:15]`
    - Insert MIC into frame
    - Encapsulate in 802.11 data frame, send via TX ring 0
  - **Message 3 received** (Key ACK=1, Key MIC=1, Install=1, Encrypted=1):
    - Verify replay counter > msg 1 counter
    - Verify MIC: zero MIC field in copy, compute HMAC-SHA1(KCK, frame), compare
    - Decrypt key data: `AES-Key-Unwrap(KEK, encrypted_key_data)` → get GTK
    - Parse GTK KDE: type=0xDD, OUI=00:0F:AC, data_type=1, extract key_id and 16-byte GTK
  - **Send Message 4**:
    - Build EAPOL-Key: Key Info = Version=2 | Pairwise | MIC | Secure (= `0x030A`)
    - Replay counter = same as msg 3, nonce = zeros, no key data
    - Compute and insert MIC
    - Send via TX ring 0
- [x] Install CCMP keys via `IWN_CMD_ADD_NODE`:
  - **PTK (unicast)**: Update AP node (id=0):
    - `node.control = IWN_NODE_UPDATE`
    - `node.flags = IWN_FLAG_SET_KEY`
    - `node.kflags = IWN_KFLAG_CCMP | IWN_KFLAG_MAP | IWN_KFLAG_KID(0)`
    - `memcpy(node.key, TK, 16)`
  - **GTK (broadcast/multicast)**: Update broadcast node (id=15):
    - Same as above but add `IWN_KFLAG_GROUP` and use `IWN_KFLAG_KID(key_id)`
    - `memcpy(node.key, GTK, 16)`
  - After key install, firmware automatically handles CCMP encrypt/decrypt for all data frames
- [x] Handle handshake timeout:
  - AP sends msg 1 within ~1 second of association
  - If no msg 1 after 3 seconds, retry association
  - If msg 3 doesn't arrive within 1 second of msg 2, AP will retry msg 1 (restart)
- [x] EAPOL frame TX helper:
  - EAPOL frames are 802.11 data frames (FC type=2, subtype=0)
  - LLC/SNAP header with EtherType `0x888E`
  - `security = 0` (EAPOL frames are sent unencrypted, even after key install)
  - `to_ds=1`: Addr1=BSSID, Addr2=STA MAC, Addr3=BSSID

#### Test on Hardware

Boot → `iwn` → `iwn scan` → `iwn connect Herrera` → enter password → should print:
```
PMK derived (1.2s)
Associated with Herrera (AID=1)
EAPOL msg 1: ANonce received
EAPOL msg 2: SNonce sent
EAPOL msg 3: GTK received, MIC verified
EAPOL msg 4: handshake complete
PTK installed (CCMP, unicast)
GTK installed (CCMP, broadcast)
Connected to Herrera (WPA2)
```

#### Files

```
bootai/iwn/
└── iwn_eapol.c         ← PMK derivation, EAPOL parsing, 4-way handshake, key install (~420 lines)
```

---

### Phase 4d: Data Frame TX/RX

**Goal:** Send and receive 802.11 data frames after association + key install. Enables actual IP traffic.

#### 802.11 Data Frame Encapsulation

```
TX (Ethernet → 802.11): STA sends to AP
┌──────────────────────────────────────────────────┐
│ 802.11 Header (24 bytes)                          │
│   FC = 0x0801 (data, ToDS=1) + 0x40 (Protected)  │
│   Addr1 (RA)    = BSSID (AP MAC)                  │
│   Addr2 (TA)    = STA MAC (us)                     │
│   Addr3 (DA)    = Ethernet DST (final destination) │
│ LLC/SNAP Header (8 bytes)                          │
│   AA:AA:03:00:00:00 + EtherType                    │
│ Payload (from Ethernet frame)                      │
└──────────────────────────────────────────────────┘

RX (802.11 → Ethernet): AP sends to STA
┌──────────────────────────────────────────────────┐
│ 802.11 Header (24 bytes)                          │
│   FC = 0x0802 (data, FromDS=1) + 0x40 (Protected)│
│   Addr1 (RA)    = STA MAC (us)                     │
│   Addr2 (TA)    = BSSID (AP MAC)                   │
│   Addr3 (SA)    = Original source MAC              │
│ [CCMP header removed by firmware]                  │
│ LLC/SNAP Header (8 bytes)                          │
│ Payload → becomes Ethernet payload                 │
└──────────────────────────────────────────────────┘

Reconstructed Ethernet frame:
  DST = Addr1 for TX (or Addr3 source for RX)
  SRC = Addr2 for TX (or Addr3 for RX)
  EtherType = from LLC/SNAP bytes 6-7
  Payload = after LLC/SNAP
```

#### Tasks

- [ ] Activate TX ring 0 in scheduler:
  - TX ring 0 descriptors were allocated in Phase 2 but queue not activated
  - Set `SCHED_QUEUE_STATUS(0)` active, add bit 0 to `TXFACT`
  - Ring 0 = AC_BE (best effort) — used for both management and data frames
- [ ] Implement `iwn_tx_data(dev, ring, tx_desc, frame, frame_len)`:
  - Write `iwn_cmd_data` + frame payload into TX ring descriptor
  - Same mechanism as `iwn_cmd()` but on ring 0, not ring 4
  - Update scheduler byte count table for ring 0
  - Kick hardware via `IWN_HBUS_TARG_WRPTR`
- [ ] Implement `iwn_encap_tx(dev, dst_mac, ethertype, payload, len)`:
  - Build 802.11 data frame header (24 bytes):
    - `FC[0]=0x08` (data), `FC[1]=0x41` (ToDS=1, Protected=1 if keys installed, else 0x01)
    - Addr1=BSSID, Addr2=our MAC, Addr3=dst_mac
  - Add LLC/SNAP (8 bytes): `AA:AA:03:00:00:00` + big-endian EtherType
  - Append payload
  - TX descriptor: `flags = IWN_TX_NEED_ACK | IWN_TX_AUTO_SEQ`, `id = IWN_ID_BSS` (0)
  - `security = IWN_CIPHER_CCMP` if keys installed, else 0
  - Rate: start at OFDM 24Mbps (`plcp` from rate table), firmware uses LINK_QUALITY retry chain
  - Submit via `iwn_tx_data(dev, 0, ...)`
- [ ] Implement `iwn_decap_rx(dev, rx_data, rx_len, eth_frame, eth_len)`:
  - Check FC type == 2 (data) and `from_ds == 1`
  - Extract: DST = Addr1 (us), SRC = Addr3 (original source), EtherType from SNAP
  - Build Ethernet frame: `[DST(6)][SRC(6)][EtherType(2)][payload]`
  - Skip frames not addressed to us (check Addr1 == our MAC or broadcast)
- [ ] Handle `IWN_TX_DONE` (type 28) notifications:
  - Parse TX status: success, retry count, failure reason
  - Update TX ring read pointer (free descriptor)
  - Log failures for debugging
- [ ] Implement simple TX/RX test (`iwn ping`):
  - Craft ARP request: who-has gateway IP (from hypothetical DHCP, or hardcoded `192.168.1.1`)
  - Send via `iwn_encap_tx(dev, ff:ff:ff:ff:ff:ff, 0x0806, arp_payload, 28)`
  - Poll RX for ARP reply
  - If reply received, data path is working
  - This is a manual smoke test — full IP comes with SNP in Phase 5
- [ ] Implement RX data frame buffer:
  - Ring buffer of received Ethernet frames (e.g., 16 × 2KB)
  - `iwn_rx_pending()` — check if frames available
  - `iwn_rx_dequeue(buf, len)` — dequeue next frame
  - Fed by RX interrupt handler in `iwn_poll_rx` when type=193 and FC is data frame
  - This buffer is what SNP's `Receive()` will read from in Phase 5

#### Test on Hardware

Boot → `iwn` → `iwn scan` → `iwn connect Herrera` → `iwn ping` → should show:
```
TX: ARP who-has 192.168.1.1
RX: ARP reply from 192.168.1.1 is-at AA:BB:CC:DD:EE:FF
Data path working!
```

#### Files

```
bootai/iwn/
└── iwn_data.c          ← TX ring 0 activation, 802.11 encap/decap, TX/RX data path
```

---

## Phase 5: SNP Integration

**Goal:** Expose the driver as `EFI_SIMPLE_NETWORK_PROTOCOL` so UEFI's TCP4/UDP4/IP4 stack works. After this, all existing BootAI networking (HTTP server, mDNS, DNS, API) works over WiFi.

### Tasks

- [ ] Define `EFI_SIMPLE_NETWORK_PROTOCOL` in `efi/efi.h`:
  - SNP mode structure: state, HwAddressSize (6), MediaHeaderSize (14), MaxPacketSize (1500)
  - Function pointers: Start, Stop, Initialize, Reset, Shutdown, ReceiveFilters, StationAddress, Statistics, MCastIpToMac, NvData, GetStatus, Transmit, Receive
- [ ] Implement SNP wrapper in `iwn_snp.c`:
  - `iwn_snp_start()` → already initialized (firmware loaded in `iwn_init()`)
  - `iwn_snp_initialize()` → scan for saved network, associate, WPA2 handshake
  - `iwn_snp_transmit(header_size, buf_size, buf)`:
    - Parse Ethernet header (first 14 bytes: DST + SRC + EtherType)
    - Call `iwn_encap_tx(dev, dst, ethertype, payload, len)`
  - `iwn_snp_receive(header_size, buf_size, buf)`:
    - Call `iwn_rx_dequeue(buf, &len)`
    - Return `EFI_NOT_READY` if no frames pending
  - `iwn_snp_get_status(interrupt_status, tx_buf)`:
    - Poll `IWN_INT` register, process completions
    - Report TX recycled buffers
  - `iwn_snp_shutdown()` → send disassociation frame, power down radio
  - Stub remaining functions (Statistics, NvData, etc.) → return `EFI_UNSUPPORTED`
- [ ] Install SNP on a new device handle:
  - `gBS->InstallMultipleProtocolInterfaces(&handle, &gEfiSnpGuid, &iwn_snp, &gEfiDevicePathGuid, &iwn_device_path, NULL)`
  - Need a minimal device path (PCI device path from the Intel 6200's BDF)
- [ ] Trigger UEFI stack binding:
  - `gBS->ConnectController(handle, NULL, NULL, TRUE)` — recursive
  - UEFI firmware should bind MNP → ARP → IP4 → TCP4/UDP4 on the new SNP
- [ ] Configure DHCP on the new interface:
  - Use `EFI_IP4_CONFIG2_PROTOCOL` with `Ip4Config2PolicyDhcp`
  - Wait for DHCP lease (existing code from Phase 2 works here)
- [ ] Verify end-to-end:
  - `netinfo` shows IP address on WiFi interface
  - `fetch http://example.com/` works
  - `serve` starts HTTP server, accessible from Mac

### Test on Hardware

Boot → `iwn` → `iwn scan` → auto-connect → `netinfo` shows IP → `serve` → from Mac: `curl http://<ip>:8080/health` → `{"status":"ok"}`

### Files

```
bootai/iwn/
└── iwn_snp.c           ← SNP protocol wrapper, device path, UEFI stack binding
```

---

## Phase 6: Polish & Integration

**Goal:** Error handling, reconnection, UI integration, documentation.

### Tasks

- [ ] Handle connection failures gracefully:
  - Scan timeout → retry
  - Association rejection → print reason code (status codes from 802.11)
  - WPA2 handshake failure → print which message failed, retry
  - Deauthentication → attempt reconnect
- [ ] Implement beacon miss detection:
  - Handle `IWN_BEACON_MISSED` (type 162) notification
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
- [ ] Runtime calibration:
  - Implement `iwn_send_sensitivity()` and `iwn_set_gains()`
  - Poll calibration every ~500ms while connected
  - Improves signal quality over time
- [ ] Documentation:
  - Update `bootai/CLAUDE.md` with iwn build instructions
  - Update `bootai/docs/architecture/iwn-wifi-driver.md` with association + WPA2 details
  - Update `bootai/README.md` with WiFi feature
- [ ] Update `bootai/docs/specs/bootai-roadmap.md` — mark WiFi phase complete

### Test on Hardware

Full end-to-end: Boot USB → auto-connect WiFi → `serve` → chat from Mac via `bootai.local`.

---

## Build Integration

### EFI_SRC in Makefile (current)

IWN sources are listed directly in `EFI_SRC` (no separate variable):

```makefile
EFI_SRC = ... iwn/iwn_pci.c iwn/iwn_hw.c iwn/iwn_dma.c iwn/iwn_fw.c iwn/iwn_scan.c iwn/iwn_crypto.c iwn/iwn_assoc.c iwn/iwn_eapol.c
```

Future phases will append: `iwn/iwn_data.c iwn/iwn_snp.c`

### Crypto test target (host-runnable, Phase 4a — done)

```makefile
test-iwn-crypto: iwn/iwn_crypto.c iwn/test_crypto.c
	cc -std=c11 -Wall -Wextra -O2 -o test_crypto iwn/test_crypto.c iwn/iwn_crypto.c && ./test_crypto && rm -f test_crypto
```

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| ~~Firmware won't load (DMA timing)~~ | ~~Blocks Phase 2+~~ | ~~Resolved: 9 bugs fixed, both init+runtime firmware alive~~ |
| ~~EEPROM read fails on E6510~~ | ~~Blocks Phase 1~~ | ~~Resolved: OTP path works, MAC/domain/RF read successfully~~ |
| ~~Scan returns no results~~ | ~~Blocks Phase 3~~ | ~~Resolved: 10 bugs fixed. Scan works — 7 networks found on Dell E6510~~ |
| ~~Not enough memory for DMA~~ | ~~Blocks Phase 2~~ | ~~Resolved: ~2.5MB allocated below 4GB, no issues~~ |
| ~~Calibration timeout~~ | ~~Degrades Phase 3+~~ | ~~Resolved: missing WiMAX coex command before calibration~~ |
| ~~RSSI calculation off~~ | ~~Minor Phase 3~~ | ~~Resolved: wrong data offset + missing IWN_RSSI_TO_DBM. Formula: max(rssi) - agc - 44~~ |
| PBKDF2 too slow on E6510 | UX (Phase 4c) | Crypto implemented, needs E6510 timing. Westmere i5 should do 4096 iters in ~1-2s. Cache PMK for reconnect. |
| No RDRAND on E6510 | Blocks SNonce gen (Phase 4c) | E6510 is Westmere (no RDRAND). RNG deferred to `iwn_wpa.c`. Fallback: RDTSC + MAC + ANonce entropy via SHA-1 |
| Auth/assoc rejected by AP | Blocks Phase 4b | Print status code. Common: wrong channel, AP full, wrong RSN IE. Compare with OpenBSD byte-for-byte. |
| WPA2 handshake timing | Blocks Phase 4c | AP typically retries msg 1 after ~1s. Add 3s timeout + retry. If MIC fails, re-derive PTK. |
| EAPOL frame not received | Blocks Phase 4c | AP sends msg 1 after association. If missed, AP will deauth after ~1s and we retry. Check RX filter includes data frames. |
| TX ring 0 not activated | Blocks Phase 4b | Ring 0 allocated in Phase 2 but not in TXFACT. Must activate scheduler queue 0 before mgmt/data TX. |
| SNP binding fails | Blocks Phase 5 | May need `InstallMultipleProtocolInterfaces` with PCI device path. Test with `ConnectController` recursive. |
| Dell E6510 firmware quirks | Any phase | Test each phase on hardware, not just QEMU |

## Dependencies

- **Firmware file**: Download `iwlwifi-6000-4.ucode` from linux-firmware repo, rename to `iwn-6000`
- **OpenBSD source**: Clone headers from `github.com/openbsd/src` (reference for association flow)
- **Dell E6510**: Physical hardware for every phase test
- **WPA2 network**: Home WiFi (Herrera) for Phase 4c+ testing
- **No open network needed**: Phase 4b can be tested against WPA2 (association succeeds, AP deauths after ~1s without handshake — expected behavior)
