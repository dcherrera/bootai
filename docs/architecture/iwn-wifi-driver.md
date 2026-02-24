# IWN WiFi Driver Architecture

> Bare-metal Intel WiFi driver for BootAI, ported from OpenBSD's `iwn` driver to UEFI.

---

## Overview

The IWN driver enables WiFi networking on BootAI by driving the Intel Centrino Advanced-N 6200 directly from UEFI boot services. It is a phased port of OpenBSD's `sys/dev/pci/if_iwn.c`, adapted to run without a kernel by shimming OpenBSD's bus/DMA APIs onto UEFI protocols.

The driver exposes `EFI_SIMPLE_NETWORK_PROTOCOL` (SNP), which plugs into UEFI's existing IP4/TCP4/UDP4 stack. This means all existing BootAI networking code (HTTP server, mDNS, DNS, HTTP GET) works unchanged over WiFi.

## Target Hardware

| Field | Value |
|-------|-------|
| Card | Intel Centrino Advanced-N 6200 (Dell E6510) |
| PCI IDs | `8086:422C`, `8086:4239` |
| Chip family | Intel 6000 series ("Puma Peak") |
| MAC type | SoftMAC (host handles 802.11 MAC) |
| Firmware | `iwn-6000` (OpenBSD-renamed `iwlwifi-6000-4.ucode`) |

## File Layout

```
bootai/iwn/
├── iwn.h              # Public API (iwn_probe, future: iwn_init, iwn_scan, ...)
├── iwn_shim.h         # UEFI shim: OpenBSD types → stdint, bus_space stubs
├── if_iwnreg.h        # Hardware registers (copied from OpenBSD, verbatim)
├── if_iwnvar.h        # Driver structs (Phase 0: stub with PCI IDs only)
└── iwn_pci.c          # PCI enumeration and device matching
```

Future phases will add:

```
├── iwn_driver.c       # Core driver (init, scan, associate, TX/RX rings)
├── iwn_fw.c           # Firmware loading (TLV parse, DMA upload)
├── iwn_cmd.c          # Firmware command interface (RXON, scan, add_node)
├── iwn_crypto.c       # WPA2-PSK (PBKDF2, HMAC-SHA1, AES-128, 4-way handshake)
├── iwn_snp.c          # EFI_SIMPLE_NETWORK_PROTOCOL wrapper
└── iwn_shim.c         # bus_dma, bus_space, timeout UEFI implementations
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

### Phase 0: PCI Probe (current)

- Enumerate PCI bus via `EFI_PCI_IO_PROTOCOL`
- Match Intel 6200 by vendor:device ID
- Print all PCI devices with BDF locations
- REPL command: `iwn`

### Phase 1: MMIO + Firmware Load

- Map BAR0 for register access
- Implement `bus_space_read_4`/`bus_space_write_4` via PCI IO Mem
- Load firmware from `\drivers\iwn-6000` on the EFI partition
- Parse TLV firmware header, upload via DMA

### Phase 2: TX/RX Rings + Scanning

- Allocate DMA rings via `AllocatePages()`
- Initialize TX queues (command queue + data queues)
- Initialize RX ring (64 entries, 4KB buffers)
- Send `IWN_CMD_SCAN`, collect results

### Phase 3: Association + WPA2

- Implement RXON state machine (INIT → SCAN → AUTH → ASSOC → RUN)
- WPA2-PSK crypto: SHA-1, HMAC-SHA1, PBKDF2, AES-128, PRF-384
- 4-way EAPOL handshake
- Install temporal key for hardware CCMP

### Phase 4: SNP + Integration

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
