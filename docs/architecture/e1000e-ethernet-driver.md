# e1000e Ethernet Driver Architecture

> Bare-metal Intel e1000e wired Ethernet driver for BootAI, written from scratch for UEFI.

---

## Overview

The e1000e driver enables wired Ethernet networking on BootAI by driving Intel 82577-class NICs directly from UEFI boot services. Unlike the IWN WiFi driver (ported from OpenBSD), this is a clean-room implementation — e1000e hardware is simple enough that no reference port was needed.

The driver provides raw Ethernet frame TX/RX via DMA descriptor rings. It integrates with the custom ns_* network stack via 4 thin wrapper functions, giving BootAI DHCP + HTTP server capability over wired Ethernet.

## Target Hardware

Primary development target:

| Field | Value |
|-------|-------|
| Card | Intel 82577LM (Dell E6510 built-in) |
| PCI IDs | `8086:10EA` |
| PCI Location | Bus 0, Device 25, Function 0 (ICH9 standard) |
| MAC | `5c:26:0a:02:83:22` (Dell OUI) |
| PHY | Integrated copper, 10/100/1000 Mbps |

### Supported Cards

The driver includes a device ID table covering the Intel 82574/82577/82578/82579/I217/I218/I219 family:

| Device | PCI ID | Common In |
|--------|--------|-----------|
| 82577LM | `0x10EA` | Dell E6510, ThinkPad T510 |
| 82577LC | `0x10EB` | Consumer laptops 2010 |
| 82578DM | `0x10EF` | Desktop boards 2010 |
| 82579LM | `0x1502` | ThinkPad T420/T520, Dell E6420 |
| 82579V | `0x1503` | Consumer desktops 2011 |
| I217-LM | `0x153A` | ThinkPad T440, Dell E7440 |
| I217-V | `0x153B` | Consumer desktops 2013 |
| I218-LM | `0x155A` | ThinkPad T450, Dell E7450 |
| I218-V | `0x1559` | Consumer desktops 2014 |
| I219-LM | `0x156F` | ThinkPad T460+, Dell E7470+ |
| I219-V | `0x1570` | Consumer desktops 2015+ |
| 82574L | `0x10D3` | QEMU e1000e emulation |

## Architecture

### Single File Design

The entire driver lives in `e1000e/e1000e_hw.c` (~660 lines). Unlike IWN which requires 11 files for 802.11 complexity (firmware upload, scanning, association, WPA2 EAPOL, CCMP encryption), e1000e is pure hardware register programming:

```
e1000e/
  e1000e.h          # Public API (probe, init, TX/RX, SNP wrappers)
  e1000e_regs.h     # Register offsets and bit definitions
  e1000e_hw.c       # Complete driver implementation
```

### Register Access

All register access goes through UEFI `EFI_PCI_IO_PROTOCOL` (not volatile pointers):

```c
static uint32_t e1000e_read(uint32_t reg) {
    uint32_t val;
    g_e1000e.pci_io->Mem.Read(g_e1000e.pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
    return val;
}
```

This avoids the PE relocation bug that affects pointer-in-static-data on this UEFI build (no base relocations in the PE image).

### DMA Rings

Both RX and TX use hardware DMA descriptor rings allocated below 4 GB:

**RX Ring:**
- 64 descriptors x 16 bytes = 1024-byte descriptor ring (1 page)
- 64 x 2048-byte contiguous buffer pool = 128 KB (32 pages)
- Hardware writes frames directly to buffers, sets DD (Descriptor Done) bit
- Software polls DD, copies frame, recycles descriptor by advancing RDT

**TX Ring:**
- 64 descriptors x 16 bytes = 1024-byte descriptor ring (1 page)
- Single 2048-byte TX buffer (1 page) — TX is synchronous
- Software copies frame to buffer, fills descriptor (EOP+IFCS+RS), kicks TDT
- Polls DD bit with 1ms timeout

### Initialization Sequence

```
e1000e_probe()
  1. LocateHandleBuffer(PciIoProtocol)
  2. Walk handles, match vendor 0x8086 + device table
  3. Enable PCI MMIO + Bus Master attributes
  4. Save pci_io handle
  → e1000e_init()
      1. Disable interrupts (IMC = 0xFFFFFFFF)
      2. PBA/PBS errata for ICH9-class
      3. Global reset (CTRL.RST), wait 10ms
      4. Disable interrupts again
      5. Read MAC from RAL0/RAH0 (or EEPROM fallback)
      → e1000e_link_up()
          1. Set SLU + ASDE in CTRL
          2. Poll STATUS.LU for 5 seconds
          3. Decode speed/duplex from STATUS bits
      → e1000e_rx_init()
          1. Alloc descriptor ring + buffer pool (DMA below 4GB)
          2. Program RDBAL/RDBAH/RDLEN/RDH/RDT
          3. Enable RCTL (EN + BAM + SECRC + BSIZE_2K)
      → e1000e_tx_init()
          1. Alloc descriptor ring + single TX buffer
          2. Program TDBAL/TDBAH/TDLEN/TDH/TDT
          3. Set TIPG (standard copper inter-packet gap)
          4. Enable TCTL (EN + PSP + CT + COLD)
```

## Integration with ns_* Stack

The e1000e driver integrates with the custom network stack via 4 thin wrapper functions that match the IWN direct API pattern:

```c
int  e1000e_snp_is_installed(void);   // g_e1000e_found && link_up
void e1000e_snp_get_mac(uint8_t *mac);
int  e1000e_snp_direct_tx(dst_mac, ethertype, payload, len);  // → e1000e_send()
int  e1000e_snp_direct_rx(buf, buf_size, frame_len);          // → e1000e_rx_poll() with inverted return
```

The ns_* stack selects the driver at init time:

```c
// ns_net.c
if (e1000e_snp_is_installed())      → NS_DRIVER_E1000E (preferred)
else if (iwn_snp_is_installed())    → NS_DRIVER_IWN (WiFi fallback)
```

### Why No Separate SNP File?

IWN needs `iwn_snp.c` (422 lines) because:
- 802.11 frames must be encapsulated as Ethernet (LLC/SNAP headers)
- CCMP encryption/decryption on every frame
- TX buffer recycling via WaitForPacket event
- Full SNP function pointer table for UEFI protocol compatibility

e1000e needs none of this — it already sends/receives raw Ethernet frames. The 4 wrapper functions (15 lines) are sufficient.

## REPL Commands

| Command | Description |
|---------|-------------|
| `e1000e` | PCI probe (manual, also runs at startup) |
| `e1000e poll` | Live frame capture (interactive, press any key to stop) |
| `e1000e arp` | Send gratuitous ARP broadcast |

## Startup Flow

e1000e is auto-probed at startup, before network initialization:

```
app_main()
  → efi_load_drivers()
  → e1000e_probe()          ← auto-probe wired Ethernet
  → efi_net_init()
      → try UEFI IP4 stack  ← fails on Dell (no DXE drivers)
      → e1000e_snp_is_installed()? → ns_init() → ns_dhcp_run()
  → efi_wifi_auto_connect() ← WiFi as secondary
```

If no e1000e device is found, or no cable is connected (link_up=0), the probe is non-fatal and falls through to WiFi.

## Key Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `E1000E_RX_DESC_COUNT` | 64 | RX descriptor ring size |
| `E1000E_RX_BUF_SIZE` | 2048 | Per-descriptor RX buffer |
| `E1000E_TX_DESC_COUNT` | 64 | TX descriptor ring size |
| `E1000E_TX_BUF_SIZE` | 2048 | Single shared TX buffer |
| TIPG | `0x0060200A` | Standard copper inter-packet gap |
| TCTL CT | `0x10` | Collision threshold |
| TCTL COLD | `0x40` | Collision distance (full-duplex) |

## Memory Budget

| Allocation | Size | Pages |
|------------|------|-------|
| RX descriptor ring | 1 KB | 1 |
| RX buffer pool | 128 KB | 32 |
| TX descriptor ring | 1 KB | 1 |
| TX buffer | 4 KB | 1 |
| **Total** | **~133 KB** | **35** |

All DMA allocations are below 4 GB (required by 32-bit descriptor addresses).

## Comparison: e1000e vs IWN

| Aspect | e1000e | IWN |
|--------|--------|-----|
| Transport | Wired Ethernet | 802.11 WiFi |
| Complexity | ~660 lines, 1 file | ~4,000 lines, 11 files |
| Firmware | None | ~400 KB microcode upload |
| Encryption | None (cleartext) | CCMP (AES-128-CCM) |
| Link setup | Auto-negotiate, 2-5s | Scan + auth + assoc + EAPOL, 10-30s |
| DMA | Simple descriptor rings | Complex ring buffer + scheduler queues |
| SNP wrapper | 4 functions, 15 lines | Full SNP protocol, 422 lines |
| ns_* priority | Preferred (1) | Fallback (2) |
