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

### Driver Internal Architecture

```
iwn_snp.c          — SNP protocol wrapper (Transmit/Receive/GetStatus)
iwn_driver.c       — Core driver (init, scan, associate, TX/RX rings)
iwn_fw.c           — Firmware loading (parse TLV, DMA upload)
iwn_cmd.c          — Firmware command interface (RXON, scan, add_node)
iwn_crypto.c       — WPA2-PSK: PBKDF2, HMAC-SHA1, AES-128, 4-way handshake
iwn_shim.c         — UEFI shims for OpenBSD bus_dma, bus_space, timeout APIs
if_iwnreg.h        — Hardware registers (copied from OpenBSD, verbatim)
if_iwnvar.h        — Driver structs (copied from OpenBSD, modified)
```

## Functional Requirements

### FR1: PCI Discovery
- Enumerate PCI devices using `EFI_PCI_IO_PROTOCOL`
- Match Intel WiFi by vendor `0x8086`, device `0x422C` or `0x4239`
- Map BAR0 for MMIO register access

### FR2: Firmware Loading
- Load `iwn-6000` firmware from EFI System Partition (`\drivers\iwn-6000`)
- Parse TLV header: signature `IWL\n` (`0x0a4c5749`), extract text/data/init sections
- Upload via DMA engine in 128KB chunks
- Wait for `IWN_UC_READY` notification

### FR3: DMA Ring Management
- Allocate physically contiguous DMA buffers via `AllocatePages()`
- TX rings: 20 queues, 256 entries each (only queues 0-4 needed for basic operation)
- RX ring: 64 entries, pre-allocated 4KB buffers
- Command queue: TX queue 4 for host→firmware commands

### FR4: WiFi Scanning
- Send `IWN_CMD_SCAN` (0x80) for 2.4 GHz channels
- Firmware autonomously scans, sends probe requests
- Collect scan results from `IWN_RX_DONE` notifications
- Present available networks (SSID, RSSI, security)

### FR5: Association
- State machine: INIT → SCAN → AUTH → ASSOC → RUN
- `IWN_CMD_RXON` (0x10) to configure channel, BSSID, security
- `IWN_CMD_TIMING` (0x14) for beacon interval
- `IWN_CMD_ADD_NODE` (0x18) to add AP to station table
- `IWN_CMD_LINK_QUALITY` (0x4E) for rate selection

### FR6: WPA2-PSK Authentication
- Derive PMK from passphrase + SSID via PBKDF2-SHA1 (4096 iterations)
- 4-way EAPOL handshake:
  - Msg 1: AP → STA (ANonce)
  - Msg 2: STA → AP (SNonce + MIC)
  - Msg 3: AP → STA (GTK + MIC)
  - Msg 4: STA → AP (confirmation)
- Derive PTK via PRF-384 (HMAC-SHA1 based)
- Install temporal key via `IWN_CMD_ADD_NODE` with `IWN_CIPHER_CCMP`
- Hardware handles per-frame CCMP encrypt/decrypt after key install

### FR7: Data TX/RX
- **TX**: Receive Ethernet frame from SNP → add 802.11 header → submit to TX ring → firmware encrypts (CCMP) and transmits
- **RX**: Firmware receives and decrypts → DMA to RX ring → strip 802.11 header → deliver Ethernet frame via SNP Receive
- Polling-based (no interrupts): read `CSR_INT` register periodically

### FR8: SNP Integration
- Install `EFI_SIMPLE_NETWORK_PROTOCOL` on a new handle
- UEFI's `ConnectController` will bind IP4/TCP4/UDP4 on top
- After SNP + DHCP, all existing BootAI networking works

### FR9: User Interface
- `wifi scan` — show available networks (already exists in wifi.c)
- `wifi connect <SSID>` — prompt for password, associate + WPA2 handshake
- `wifi disconnect` — deassociate
- `wifi save` — persist credentials to `wifi.conf`
- Bridge to existing `efi/wifi.c` UI code

## Non-Requirements (Explicit Scope Limits)

- **Only WPA2-PSK** — no WPA3, no EAP/Enterprise, no WEP
- **Only Intel 6200** — no other chips (though the iwn_ops vtable supports them)
- **Only 2.4 GHz** — 5 GHz is nice-to-have, not required
- **No power management** — stay fully powered
- **No A-MPDU aggregation** — single frame TX only
- **No roaming** — connect to one AP, stay there
- **No IPv6** — existing stack is IPv4 only
- **Polling only** — no interrupt handler, poll CSR_INT in main loop

## Crypto Primitives Required

| Algorithm | Use | Lines (est.) |
|-----------|-----|-------------|
| SHA-1 | PBKDF2, HMAC, PRF | ~300 |
| HMAC-SHA1 | Key derivation, MIC | ~100 |
| PBKDF2-SHA1 | PSK → PMK (4096 iters) | ~100 |
| AES-128 | Key wrap, EAPOL-Key decryption | ~500 |
| PRF-384 | PMK + nonces → PTK | ~50 |
| **Total** | | **~1,050** |

Note: Per-frame CCMP encryption is handled by the Intel 6200 firmware/hardware. We only need crypto for the WPA2 handshake.

## Files on USB Drive

```
BOOTAI/
├── drivers/
│   └── iwn-6000              ← Intel 6200 firmware (renamed from iwlwifi-6000-4.ucode)
├── wifi.conf                 ← Saved SSID + PSK (already exists)
└── ...
```

## Success Criteria

1. `wifi scan` shows local networks with SSID and signal strength
2. `wifi connect MyNetwork` associates and completes WPA2 handshake
3. DHCP assigns an IP address automatically
4. `serve` starts HTTP server + mDNS on the WiFi interface
5. From a Mac on the same network: `curl http://bootai.local:8080/health` returns `{"status":"ok"}`
6. `chat` over the HTTP API works end-to-end (prompt → inference → response)

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
