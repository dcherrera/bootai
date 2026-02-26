# IWN WiFi Driver Specification

## Goal

Port the OpenBSD `iwn` WiFi driver to UEFI so BootAI can connect to WiFi networks on bare metal. This enables mDNS discovery and HTTP API access over WiFi — the primary way clients will interact with BootAI on real hardware.

## Target Hardware

| Field | Value |
|-------|-------|
| Card | Intel Centrino Advanced-N 6200 |
| Chip family | Intel 6000 series ("Puma Peak") |
| PCI Vendor/Device | `8086:422C` or `8086:4239` |
| Bands | 2.4 GHz + 5 GHz |
| MIMO | 2x2 (2T2R) |
| MAC type | **SoftMAC** (host handles 802.11 MAC) |
| Firmware | `iwlwifi-6000-4.ucode` (renamed `iwn-6000` by OpenBSD) |
| Dell E6510 subsystem | `8086:1301` (AGN) |

## Source Driver

**OpenBSD `iwn`** — `sys/dev/pci/if_iwn.c`

| File | Lines | Purpose |
|------|-------|---------|
| `if_iwn.c` | ~4,500 | Driver implementation |
| `if_iwnreg.h` | ~2,000 | Hardware registers, command structs, descriptor layouts |
| `if_iwnvar.h` | ~500 | Driver-private structs (`iwn_softc`, rings, ops vtable) |

Total: ~7,000 lines. Far smaller than Linux's iwlwifi (~60K for the 6200 path).

### Why OpenBSD over Linux?

- Single self-contained file vs 8+ subdirectories
- Minimal kernel API surface (~15 function families to shim)
- Successfully ported to macOS by the `itlwm` project
- Clean `iwn_ops` vtable abstracts chip differences
- The register headers can be copied verbatim

## Architecture

### Integration with UEFI Network Stack

```
  Existing BootAI Code (unchanged)
  ────────────────────────────────
  HTTP Server / mDNS / DNS / HTTP GET
          ↓
  UEFI TCP4 / UDP4 / IP4 Protocols
          ↓
  UEFI IP4 / ARP / MNP stack
          ↓
  EFI_SIMPLE_NETWORK_PROTOCOL (SNP)    ← We implement this
          ↓
  iwn driver (ported from OpenBSD)
          ↓
  Intel 6200 hardware + firmware
```

The key insight: by implementing `EFI_SIMPLE_NETWORK_PROTOCOL`, all existing BootAI networking code works unchanged. SNP is the UEFI equivalent of a Linux netdev — it provides Initialize, Start, Stop, Transmit, Receive, GetStatus.

### SNP Interface

We expose these 11 SNP functions:

| Function | Purpose |
|----------|---------|
| `Start` | Power on the NIC |
| `Stop` | Power off |
| `Initialize` | Allocate buffers, load firmware, scan, associate |
| `Reset` | Restart the NIC |
| `Shutdown` | Clean shutdown |
| `ReceiveFilters` | Set multicast/broadcast/promiscuous |
| `StationAddress` | Get/set MAC address |
| `Statistics` | TX/RX packet counts |
| `MCastIpToMac` | Multicast IP to MAC conversion |
| `Transmit` | Send an Ethernet frame (802.3 → 802.11 encap inside) |
| `Receive` | Receive an Ethernet frame (802.11 → 802.3 decap inside) |
| `GetStatus` | Check interrupt status, TX completion |

### Driver Internal Architecture (Actual)

```
iwn.h              — Public API (probe, init, attach, scan, connect, password)
iwn_internal.h     — Shared internal header (MMIO, prph, lock, DMA, FW, scan, assoc, EAPOL)
iwn_shim.h         — UEFI shim: OpenBSD types → stdint, byte-order no-ops
if_iwnreg.h        — Hardware registers (from OpenBSD, with additions)
if_iwnvar.h        — Driver structs (iwn_dev, DMA rings, scan results, assoc/EAPOL state)
iwn_pci.c          — PCI enumeration, device matching, iwn_init() entry point
iwn_hw.c           — MMIO primitives, NIC lock, APM init/stop, EEPROM read
iwn_dma.c          — DMA memory allocation, TX/RX ring setup, scheduler table
iwn_fw.c           — Firmware parse/upload, hw_init, calibration, host commands
iwn_scan.c         — Radio config, scan command, beacon parsing, result display
iwn_assoc.c        — 802.11 Open System auth + association + post-assoc config
iwn_eapol.c        — WPA2-PSK EAPOL 4-way handshake
iwn_crypto.h       — Crypto API header
iwn_crypto.c       — SHA-1, HMAC-SHA1, PBKDF2, AES-128, AES Unwrap, PRF-384
test_crypto.c      — Host-testable: 13 standard test vectors
iwn_snp.c          — (planned) SNP protocol wrapper
```

## Functional Requirements

### FR1: PCI Discovery ✓ DONE (Phase 0)
- Enumerate PCI devices using `EFI_PCI_IO_PROTOCOL`
- Match Intel WiFi by vendor `0x8086` against full device ID table (~38 cards)
- Enable MEMORY + BUS_MASTER PCI attributes for BAR0 MMIO access
- REPL command: `iwn`

### FR2: Firmware Loading ✓ DONE (Phase 2)
- Load `iwn-6000` firmware from EFI System Partition (`\drivers\iwn-6000`)
- Parse legacy v2 header (not TLV — the 6200 uses legacy format)
- Upload via FH DMA service channel 9 in 128KB chunks
- Wait for `IWN_UC_READY` notification
- Full init→calibration→power cycle→runtime firmware boot sequence

### FR3: DMA Ring Management ✓ DONE (Phase 2)
- Allocate physically contiguous DMA buffers via `AllocatePages()` below 4GB
- TX rings: queues 0 (data) and 4 (command) allocated, 256 entries each
- RX ring: 64 entries, pre-allocated 4KB buffers
- Scheduler byte count table for TX queue management
- ~2.5 MB total DMA memory

### FR4: WiFi Scanning ✓ DONE (Phase 3)
- 8-command radio config sequence before scan
- Send `IWN_CMD_SCAN` (code 128) for 2.4 GHz channels 1-11
- Firmware scans, sends probe requests at CCK 1Mbps
- Parse beacons/probe responses: SSID, BSSID, RSSI, channel, WPA/WPA2 security
- REPL command: `iwn scan`

### FR5: Association — CODE COMPLETE, UNTESTED (Phase 4b)
- State machine: INIT → AUTH → ASSOC → RUN
- `IWN_CMD_RXON` for channel + BSSID, `IWN_CMD_TIMING` for beacon interval
- Open System authentication (TX auth request, poll auth response)
- Association request with RSN IE for WPA2 (CCMP + PSK)
- Post-assoc: RXON with AID, ADD_NODE for AP (id=0), full rate table
- REPL commands: `iwn pass <password>`, `iwn connect <SSID>`

### FR6: WPA2-PSK Authentication — CRYPTO DONE, EAPOL UNTESTED (Phase 4a + 4c)
- Crypto primitives (Phase 4a, ~490 lines): SHA-1, HMAC-SHA1, PBKDF2-SHA1, AES-128, AES Key Unwrap, PRF-384. 13/13 test vectors passing.
- EAPOL 4-way handshake (Phase 4c, ~420 lines):
  - PMK = PBKDF2-SHA1(password, SSID, 4096, 32)
  - Msg 1: AP → STA (ANonce) — poll RX for data frame with LLC/SNAP 0x888E
  - Msg 2: STA → AP (SNonce + RSN IE + MIC) — SNonce from RDTSC+SHA-1
  - Msg 3: AP → STA (encrypted GTK + MIC) — verify MIC, AES-unwrap, parse GTK KDE
  - Msg 4: STA → AP (MIC confirmation)
- PTK = PRF-384(PMK, addresses, nonces) → KCK + KEK + TK
- Install TK on AP node (id=0, CCMP) and GTK on broadcast node (id=15, CCMP+GROUP)
- Hardware handles per-frame CCMP encrypt/decrypt after key install

### FR7: Data TX/RX — PLANNED (Phase 5)
- **TX**: Receive Ethernet frame from SNP → add 802.11 header → submit to TX ring → firmware encrypts (CCMP) and transmits
- **RX**: Firmware receives and decrypts → DMA to RX ring → strip 802.11 header → deliver Ethernet frame via SNP Receive
- Polling-based (no interrupts): read `CSR_INT` register periodically

### FR8: SNP Integration — PLANNED (Phase 5)
- Install `EFI_SIMPLE_NETWORK_PROTOCOL` on a new handle
- UEFI's `ConnectController` will bind IP4/TCP4/UDP4 on top
- After SNP + DHCP, all existing BootAI networking works

### FR9: User Interface — PARTIAL
- `iwn` — probe + init + firmware load ✓
- `iwn scan` — show available networks ✓
- `iwn pass <password>` — set WPA2 passphrase ✓
- `iwn connect <SSID>` — associate + WPA2 handshake ✓
- Bridge to `wifi` commands via SNP — PLANNED (Phase 5)

## Non-Requirements (Explicit Scope Limits)

- **Only WPA2-PSK** — no WPA3, no EAP/Enterprise, no WEP
- **Only Intel 6200** — no other chips (though the iwn_ops vtable supports them)
- **Only 2.4 GHz** — 5 GHz is nice-to-have, not required
- **No power management** — stay fully powered
- **No A-MPDU aggregation** — single frame TX only
- **No roaming** — connect to one AP, stay there
- **No IPv6** — existing stack is IPv4 only
- **Polling only** — no interrupt handler, poll CSR_INT in main loop

## Crypto Primitives — IMPLEMENTED

| Algorithm | Standard | Lines (actual) | File |
|-----------|----------|---------------|------|
| SHA-1 | FIPS 180-4 | ~100 | `iwn_crypto.c` |
| HMAC-SHA1 | RFC 2104 | ~40 | `iwn_crypto.c` |
| PBKDF2-SHA1 | RFC 2898 | ~40 | `iwn_crypto.c` |
| AES-128 (encrypt + decrypt) | FIPS 197 | ~240 | `iwn_crypto.c` |
| AES Key Unwrap | RFC 3394 | ~50 | `iwn_crypto.c` |
| PRF-384 | IEEE 802.11i | ~30 | `iwn_crypto.c` |
| **Total** | | **~490** | |

Plus 13 test vectors in `test_crypto.c` (~300 lines). Build: `make test-iwn-crypto`.

Per-frame CCMP encryption is handled by the Intel 6200 firmware/hardware. We only need crypto for the WPA2 handshake.

## Files on USB Drive

```
BOOTAI/
├── drivers/
│   └── iwn-6000              ← Intel 6200 firmware (renamed from iwlwifi-6000-4.ucode)
├── wifi.conf                 ← Saved SSID + PSK (already exists)
└── ...
```

## Success Criteria

1. ✓ `iwn scan` shows local networks with SSID, channel, RSSI, and security type — verified on Dell E6510
2. `iwn connect <SSID>` authenticates, associates, and completes WPA2 EAPOL handshake — code complete, **untested on hardware**
3. CCMP keys installed — AP does not deauth (connection stays alive) — **untested on hardware**
4. DHCP assigns an IP address automatically — requires Phase 5 (SNP)
5. `serve` starts HTTP server on the WiFi interface — requires Phase 5 (SNP)
6. `curl http://bootai.local:8080/health` returns `{"status":"ok"}` — requires Phase 5 (SNP)

## Performance Targets

| Metric | Target |
|--------|--------|
| Scan time | < 5 seconds |
| Association + WPA2 | < 3 seconds |
| DHCP | < 5 seconds |
| HTTP latency (health check) | < 100ms |
| Throughput | Not critical (inference is the bottleneck, not network) |

## Reference Material

| Resource | URL |
|----------|-----|
| OpenBSD if_iwn.c | `github.com/openbsd/src/blob/master/sys/dev/pci/if_iwn.c` |
| OpenBSD if_iwnreg.h | `github.com/openbsd/src/blob/master/sys/dev/pci/if_iwnreg.h` |
| OpenBSD if_iwnvar.h | `github.com/openbsd/src/blob/master/sys/dev/pci/if_iwnvar.h` |
| itlwm (macOS port) | `github.com/OpenIntelWireless/itlwm` |
| OpenBSD iwn(4) man page | `man.openbsd.org/iwn.4` |
| Intel firmware | `firmware.openbsd.org/firmware-dist/` |
| OSDev Intel WiFi | `wiki.osdev.org/Intel_Dual_Band_Wireless` |
| UEFI SNP spec | UEFI Spec 2.10, Chapter 24 |
