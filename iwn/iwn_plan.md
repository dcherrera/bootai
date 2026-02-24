# IWN WiFi Driver — Phased Implementation Plan

Port OpenBSD's `iwn` driver to UEFI for Intel 6200 WiFi on the Dell E6510.

## Phase Overview

| Phase | Name | Deliverable | Est. Time |
|-------|------|-------------|-----------|
| 0 | Setup & Headers | Register headers, build system, PCI probe | 1 week |
| 1 | MMIO & EEPROM | Read hardware registers, dump EEPROM/MAC address | 1 week |
| 2 | DMA & Firmware | Allocate DMA rings, load firmware, get `UC_READY` | 2 weeks |
| 3 | Scan | Send scan command, parse results, print SSIDs | 2 weeks |
| 4 | Associate (Open) | Connect to an open network (no encryption) | 2 weeks |
| 5 | Crypto | AES-128, SHA-1, HMAC, PBKDF2, PRF | 1-2 weeks |
| 6 | WPA2-PSK | 4-way handshake, key install, encrypted data | 2-3 weeks |
| 7 | SNP Integration | Wrap as EFI_SIMPLE_NETWORK_PROTOCOL, DHCP, full stack | 2 weeks |
| 8 | Polish | Error handling, reconnect, wifi.c UI integration | 1-2 weeks |

**Total: ~14-18 weeks** (3.5-4.5 months)

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

- [ ] Implement MMIO access shims:
  - `iwn_read(sc, reg)` → `PciIo->Mem.Read(EfiPciIoWidthUint32, 0, reg, 1, &val)`
  - `iwn_write(sc, reg, val)` → `PciIo->Mem.Write(...)`
  - `iwn_barrier(sc)` → memory barrier (compiler fence + `PciIo->Flush()`)
- [ ] Implement `iwn_nic_lock()` / `iwn_nic_unlock()` — acquire exclusive NIC access via `IWN_GP_CNTRL`
- [ ] Implement `iwn_apm_init()` — power management init (enable clocks, set power source)
- [ ] Implement `iwn_read_eeprom()`:
  - Lock EEPROM via `IWN_OTP_GP` register
  - Read MAC address (offset varies by chip — 6200 is 5000+ series)
  - Read regulatory domain
  - Read available channels
- [ ] Print MAC address and regulatory domain
- [ ] Read `IWN_HW_REV` register, confirm chip identification

### Test on Hardware

Boot → `iwn` → should print MAC address (e.g., `MAC: 00:24:D7:xx:xx:xx`) and chip revision.

### Files

```
bootai/iwn/
└── iwn_hw.c            ← MMIO, EEPROM, APM init
```

---

## Phase 2: DMA & Firmware Loading

**Goal:** Allocate DMA rings, load firmware onto the chip, get `IWN_UC_READY`.

### Tasks

- [ ] Implement DMA allocation shims:
  - `iwn_dma_alloc(size, alignment)` → `gBS->AllocatePages()` with 4K alignment
  - Map physical address (identity-mapped in UEFI, so virt == phys)
  - `iwn_dma_free()` → `gBS->FreePages()`
- [ ] Implement `iwn_alloc_rx_ring()` — 64 RX descriptors + 64 pre-allocated 4KB buffers
- [ ] Implement `iwn_alloc_tx_ring()` — 256 TX descriptors per queue (allocate queues 0-4 only)
- [ ] Implement `iwn_alloc_sched()` — TX scheduler byte count table
- [ ] Implement `iwn_alloc_kw()` — "keep warm" page
- [ ] Implement firmware parser:
  - Load `\drivers\iwn-6000` from EFI filesystem via `efi_read_file()`
  - Parse TLV header: verify signature `0x0a4c5749`
  - Extract sections: init text, init data, runtime text, runtime data
- [ ] Implement `iwn5000_load_firmware()`:
  - Reset DMA engine
  - Upload firmware sections in 128KB chunks via DMA
  - Wait for `IWN_UC_READY` notification on RX ring
- [ ] Implement basic RX notification polling:
  - Read `IWN_FH_RX_STATUS` register for new RX descriptors
  - Parse `iwn_rx_desc.type` — handle `IWN_UC_READY` (type 1)
- [ ] Implement `iwn_hw_init()` — full hardware init sequence:
  - Stop DMA channels
  - Configure TX rings and RX ring in hardware registers
  - Load firmware
  - Post-alive calibration (`iwn5000_post_alive()`)
  - NIC config (`iwn5000_nic_config()`)

### Test on Hardware

Boot → `iwn` → should print "Firmware loaded, UC_READY received"

### Files

```
bootai/iwn/
├── iwn_dma.c           ← DMA allocation, ring setup
└── iwn_fw.c            ← Firmware parsing and loading
```

---

## Phase 3: Scanning

**Goal:** Scan for WiFi networks, display SSIDs and signal strength.

### Tasks

- [ ] Implement `iwn_cmd()` — send host command to firmware via TX queue 4:
  - Build `iwn_tx_cmd` with opcode and payload
  - Write to TX ring
  - Kick hardware via write to `IWN_HBUS_TARG_WRPTR`
  - Poll for completion (check TX ring read pointer)
- [ ] Send initial configuration commands:
  - `IWN_CMD_RXON` (0x10) — set MAC address, channel, filter flags
  - `IWN_CMD_ADD_NODE` (0x18) — add broadcast node
  - `IWN_CMD_SET_LED` (0x48)
  - `IWN_CMD_SET_POWER_MODE` (0x77)
  - `IWN_CMD_BT_COEX` (0x9B)
  - `IWN5000_CMD_TX_ANT_CONFIG` (0x98)
- [ ] Implement `iwn_scan()`:
  - Build `IWN_CMD_SCAN` (0x80) with channel list (2.4 GHz: channels 1-11)
  - Include probe request template in scan command
  - Send command
- [ ] Implement scan result parsing:
  - Poll RX ring for `IWN_RX_DONE` notifications
  - Parse beacon/probe response frames:
    - Extract SSID from information elements (IE tag 0)
    - Extract RSSI from `iwn_rx_stat`
    - Extract security type (RSN IE tag 48 → WPA2, WPA IE → WPA)
  - Handle `IWN_SCAN_DONE` notification — scan complete
- [ ] Store scan results in a simple array (max 32 networks)
- [ ] Print results: `SSID [signal dBm] [WPA2/Open]`
- [ ] Wire to existing `wifi scan` REPL command

### Test on Hardware

Boot → `wifi scan` → should list nearby WiFi networks with names and signal strength.

### Files

```
bootai/iwn/
├── iwn_cmd.c           ← Firmware command send/receive
└── iwn_scan.c          ← Scan logic, result parsing
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
| Firmware won't load (DMA timing) | Blocks Phase 2+ | Compare register sequences with itlwm, add delays |
| EEPROM read fails on E6510 | Blocks Phase 1 | Use OTP (one-time programmable) path for 6000 series |
| Scan returns no results | Blocks Phase 3 | Check antenna config, TX power, regulatory domain |
| WPA2 handshake timing | Blocks Phase 6 | AP typically retries; add timeout + retry logic |
| SNP binding fails | Blocks Phase 7 | May need `InstallMultipleProtocolInterfaces` with device path |
| Not enough memory for DMA | Blocks Phase 2 | TX/RX rings need ~5MB total; E6510 has 4-8GB |
| Dell E6510 firmware quirks | Any phase | Test each phase on hardware, not just QEMU |

## Dependencies

- **Firmware file**: Download `iwlwifi-6000-4.ucode` from linux-firmware repo, rename to `iwn-6000`
- **OpenBSD source**: Clone headers from `github.com/openbsd/src`
- **Dell E6510**: Physical hardware for every phase test
- **Open WiFi network**: For Phase 4 testing (or phone hotspot with no password)
- **WPA2 network**: For Phase 6 testing (home WiFi)
