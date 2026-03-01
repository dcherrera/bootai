# Intel e1000e SNP Driver Specification

> Bare-metal Intel wired Ethernet driver for BootAI, providing `EFI_SIMPLE_NETWORK_PROTOCOL` (SNP) for the custom `ns_*` network stack. Same architecture as the IWN WiFi driver but dramatically simpler — no firmware, no crypto, no association.

---

## Motivation

BootAI needs a working network connection on real hardware to:

1. **Debug WiFi (IWN)** — The CCMP MIC verification is broken. The management API (`/api/cmd`, `/api/log`) lets us debug remotely, but requires a working network. Chicken-and-egg: can't debug WiFi over WiFi.
2. **Run the HTTP server** — `serve 8080` needs TCP, which needs IP, which needs an SNP providing raw Ethernet frames.
3. **Work on any machine** — QEMU works because OVMF includes a complete network stack. Real hardware (Dell E6510) has no UEFI Ethernet driver in firmware. The Dell's wired NIC (Intel 82577LM) sits idle.

The IWN WiFi driver already proved the architecture: write a driver that installs SNP, then the `ns_*` custom stack (ARP, DHCP, IP, UDP, TCP) handles everything above. The e1000e driver follows the same pattern with ~10% of the complexity.

### Why Not Load EDK2 DXE Drivers?

We tried. `ConnectController()` hangs on Dell E6510 firmware (2010-era UEFI). OVMF's DXE drivers (SnpDxe, MnpDxe, Ip4Dxe, etc.) load successfully but cannot bind to hardware. This is a firmware compatibility issue we can't fix — we must drive the hardware ourselves.

---

## Target Hardware

### Primary: Intel 82577LM (Dell E6510)

| Field | Value |
|-------|-------|
| Chip | Intel 82577LM Gigabit Network Connection |
| PCI Vendor | `0x8086` (Intel) |
| PCI Device | `0x10EA` |
| Family | e1000e (ICH9/ICH10/5-Series PCH integrated LAN) |
| Bus | PCI Express (integrated in PCH) |
| Speed | 10/100/1000 Mbps auto-negotiation |
| PHY | Integrated 82577 GbE PHY (connected via internal MDIO) |
| MAC | In the PCH — registers at BAR0 |
| EEPROM | Flash-based (in PCH), MAC auto-loaded to RAL0/RAH0 at power-on |

### Architecture

Unlike discrete NICs (e.g., Intel 82540), the 82577LM is a **split design**:

```
┌──────────────────────────────┐
│  Intel 5-Series PCH          │
│  ┌────────────────────────┐  │
│  │ MAC (GbE Controller)   │  │  ← PCI function, MMIO registers at BAR0
│  │  TX/RX rings, DMA      │  │
│  │  CTRL, STATUS, RCTL... │  │
│  └──────────┬─────────────┘  │
│             │ internal MDIO   │
│  ┌──────────┴─────────────┐  │
│  │ PHY (82577LM)          │  │  ← Auto-negotiation, link status
│  │  10/100/1000 GbE       │  │
│  └──────────┬─────────────┘  │
└─────────────┼────────────────┘
              │ MDI (twisted pair)
         RJ-45 jack
```

The MAC and PHY are both inside the PCH. We access the MAC directly via MMIO (BAR0). The PHY is managed indirectly via MDIO through the MAC's control registers, but for basic operation we can let auto-negotiation handle link setup autonomously — we just need to set `CTRL.SLU` (Set Link Up) and `CTRL.ASDE` (Auto-Speed Detection Enable).

---

## Complexity Comparison: IWN WiFi vs e1000e Wired

| Component | IWN WiFi | e1000e Wired |
|-----------|----------|--------------|
| PCI enumeration | Yes | Yes |
| BAR0 MMIO mapping | Yes | Yes |
| DMA ring buffers | Yes (proprietary format) | Yes (standard 16-byte descriptors) |
| Firmware upload | Yes (454KB ucode via DMA) | **No** |
| EEPROM read | Yes (32-bit address, multi-step) | **Minimal** (MAC in RAL0/RAH0) |
| Radio calibration | Yes (5 calibration sequences) | **No** |
| 802.11 scanning | Yes (probe frames, beacon parse) | **No** |
| 802.11 auth/assoc | Yes (~700 lines) | **No** |
| WPA2 EAPOL handshake | Yes (~420 lines) | **No** |
| CCMP encrypt/decrypt | Yes (~350 lines) | **No** |
| Crypto (SHA-1, AES, PBKDF2) | Yes (~490 lines) | **No** |
| 802.11 frame encap/decap | Yes (Ethernet ↔ 802.11) | **No** (raw Ethernet) |
| SNP integration | Yes (~280 lines) | Yes (~250 lines) |
| **Total estimate** | ~5,600 lines (13 files) | **~600-800 lines (4 files)** |

The e1000e driver is essentially just the PCI + DMA parts of IWN. No firmware, no crypto, no protocol state machines.

---

## Hardware Register Map

All registers are memory-mapped from BAR0. Only the registers needed for basic operation are listed.

### Control / Status

| Register | Offset | R/W | Description |
|----------|--------|-----|-------------|
| CTRL | `0x0000` | R/W | Device Control |
| STATUS | `0x0008` | R | Device Status |
| CTRL_EXT | `0x0018` | R/W | Extended Device Control |
| ICR | `0x00C0` | R/C | Interrupt Cause Read (read clears) |
| IMS | `0x00D0` | W | Interrupt Mask Set |
| IMC | `0x00D8` | W | Interrupt Mask Clear |

### CTRL Register Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | FD | Full Duplex |
| 5 | ASDE | Auto-Speed Detection Enable |
| 6 | SLU | Set Link Up |
| 26 | RST | Device Reset |
| 31 | PHY_RST | PHY Reset (**do NOT use on 82577**) |

### STATUS Register Bits

| Bit | Name | Description |
|-----|------|-------------|
| 0 | FD | Full Duplex indication |
| 1 | LU | Link Up (poll this for link state) |
| 6 | SPEED_100 | 100 Mbps |
| 7 | SPEED_1000 | 1000 Mbps |

### Receive

| Register | Offset | Description |
|----------|--------|-------------|
| RCTL | `0x0100` | Receive Control |
| RDBAL | `0x2800` | RX Descriptor Base Address Low |
| RDBAH | `0x2804` | RX Descriptor Base Address High |
| RDLEN | `0x2808` | RX Descriptor Ring Length (bytes) |
| RDH | `0x2810` | RX Descriptor Head (HW advances) |
| RDT | `0x2818` | RX Descriptor Tail (SW advances) |

### RCTL Bits

| Bit(s) | Name | Value | Description |
|--------|------|-------|-------------|
| 1 | EN | 1 | Receiver Enable |
| 3 | UPE | 0 | Unicast Promiscuous (off for normal) |
| 4 | MPE | 0 | Multicast Promiscuous (off) |
| 15 | BAM | 1 | Broadcast Accept Mode |
| 16:17 | BSIZE | 00 | Buffer Size 2048 bytes |
| 26 | SECRC | 1 | Strip Ethernet CRC |

### Transmit

| Register | Offset | Description |
|----------|--------|-------------|
| TCTL | `0x0400` | Transmit Control |
| TIPG | `0x0410` | Transmit Inter-Packet Gap |
| TDBAL | `0x3800` | TX Descriptor Base Address Low |
| TDBAH | `0x3804` | TX Descriptor Base Address High |
| TDLEN | `0x3808` | TX Descriptor Ring Length (bytes) |
| TDH | `0x3810` | TX Descriptor Head (HW advances) |
| TDT | `0x3818` | TX Descriptor Tail (SW advances) |

### TCTL Bits

| Bit(s) | Name | Value | Description |
|--------|------|-------|-------------|
| 1 | EN | 1 | Transmitter Enable |
| 3 | PSP | 1 | Pad Short Packets |
| 4:11 | CT | 0x10 | Collision Threshold |
| 12:21 | COLD | 0x40 | Collision Distance |

### MAC Address

| Register | Offset | Description |
|----------|--------|-------------|
| RAL0 | `0x5400` | Receive Address Low (MAC bytes 0-3) |
| RAH0 | `0x5404` | Receive Address High (MAC bytes 4-5 + AV flag) |

RAH0 bit 31 (AV = Address Valid) is set when the address is loaded. The BIOS/firmware auto-loads the MAC from Flash at power-on, so these registers should already contain the correct MAC address.

### EEPROM (fallback)

| Register | Offset | Description |
|----------|--------|-------------|
| EERD | `0x0014` | EEPROM Read (word-addressed) |

EERD format for 82577 (small format):
- Bits [7:0]: Start read (write `0x01`)
- Bits [15:8]: Word address
- Bit 4: Done flag
- Bits [31:16]: 16-bit data word

MAC address in EEPROM at word offsets 0, 1, 2 (little-endian).

---

## Descriptor Ring Format

Both TX and RX use the **legacy 16-byte descriptor** format. This is the simplest format — no extended descriptors, no advanced features needed.

### RX Descriptor (16 bytes, hardware-written)

```
Offset  Size    Field
──────  ────    ─────
0x00    8       buffer_addr     Physical address of 2KB receive buffer
0x08    2       length          Received frame length (set by HW)
0x0A    2       checksum        Packet checksum (set by HW)
0x0C    1       status          Status bits (set by HW)
0x0D    1       errors          Error bits (set by HW)
0x0E    2       special         VLAN tag (set by HW)
```

```c
typedef struct {
    uint64_t buffer_addr;   /* physical address of RX buffer */
    uint16_t length;        /* received frame length (HW) */
    uint16_t checksum;      /* packet checksum (HW) */
    uint8_t  status;        /* DD=0x01, EOP=0x02 */
    uint8_t  errors;        /* error flags */
    uint16_t special;       /* VLAN */
} __attribute__((packed)) e1000e_rx_desc;
```

**Status bits:** `DD` (bit 0) = Descriptor Done, `EOP` (bit 1) = End of Packet.

**RX flow:** HW reads descriptor at head, DMAs frame into `buffer_addr`, sets `status.DD=1` and `length`, advances head. SW polls `status.DD`, processes frame, clears status, writes new tail.

### TX Descriptor (16 bytes, software-written)

```
Offset  Size    Field
──────  ────    ─────
0x00    8       buffer_addr     Physical address of frame data
0x08    2       length          Frame data length
0x0A    1       cso             Checksum offset (unused)
0x0B    1       cmd             Command: EOP=0x01, IFCS=0x02, RS=0x08
0x0C    1       status          DD=0x01 (set by HW when done)
0x0D    1       css             Checksum start (unused)
0x0E    2       special         VLAN tag (unused)
```

```c
typedef struct {
    uint64_t buffer_addr;   /* physical address of frame data */
    uint16_t length;        /* frame length */
    uint8_t  cso;           /* checksum offset (0) */
    uint8_t  cmd;           /* EOP|IFCS|RS */
    uint8_t  status;        /* DD set by HW on completion */
    uint8_t  css;           /* checksum start (0) */
    uint16_t special;       /* VLAN (0) */
} __attribute__((packed)) e1000e_tx_desc;
```

**TX command bits:** `EOP` (bit 0) = End of Packet, `IFCS` (bit 1) = Insert FCS/CRC, `RS` (bit 3) = Report Status (sets DD when complete).

**TX flow:** SW fills descriptor with `buffer_addr`, `length`, `cmd=EOP|IFCS|RS`, writes new tail to TDT register (doorbell). HW reads descriptor, DMAs frame data, transmits, sets `status.DD=1`.

### Ring Sizing

| Ring | Count | Descriptor Size | Total |
|------|-------|----------------|-------|
| RX | 64 | 16 bytes | 1 KB |
| TX | 64 | 16 bytes | 1 KB |

64 descriptors is sufficient for a polled single-threaded server. Each ring must be 128-byte aligned (64 * 16 = 1024, naturally aligned).

### Buffer Allocation

| Buffer | Size | Count | Total |
|--------|------|-------|-------|
| RX descriptor ring | 1 KB | 1 | 1 KB |
| TX descriptor ring | 1 KB | 1 | 1 KB |
| RX frame buffers | 2 KB | 64 | 128 KB |
| TX frame buffer | 2 KB | 1 | 2 KB |

**Total DMA memory: ~132 KB** (vs ~2.5 MB for IWN)

All DMA allocations must be below 4GB (32-bit physical addresses) using `gBS->AllocatePages(AllocateMaxAddress, ...)` — same pattern as IWN.

---

## Initialization Sequence

```
1. PCI Enumerate
   ├── LocateHandleBuffer(ByProtocol, PciIoProtocol)
   ├── Walk all handles, read vendor/device ID
   ├── Match against device table (0x8086:0x10EA for 82577LM)
   └── Enable PCI attributes: MEMORY + BUS_MASTER

2. Map BAR0 (MMIO)
   ├── pci_io->GetBarAttributes(BAR0) or read BAR0 from PCI config space
   └── Cast to volatile pointer for register access

3. Device Reset
   ├── Disable interrupts: write 0xFFFFFFFF to IMC (0x00D8)
   ├── Global reset: set CTRL.RST (bit 26)
   ├── Wait 10ms
   ├── Disable interrupts again (reset re-enables defaults)
   └── NOTE: Do NOT reset PHY on 82577 (iPXE INTEL_NO_PHY_RST flag)

4. Read MAC Address
   ├── Read RAL0 (0x5400) and RAH0 (0x5404)
   ├── Check RAH0.AV (bit 31) — should be set by BIOS
   ├── If invalid: fallback to EERD read at word offsets 0, 1, 2
   └── Store MAC for SNP mode struct

5. Setup RX Ring
   ├── Allocate descriptor ring (1 KB, 128-byte aligned, below 4GB)
   ├── Allocate 64 frame buffers (2 KB each, below 4GB)
   ├── Fill each descriptor: buffer_addr = physical addr of frame buffer
   ├── Zero all status fields
   ├── Write RDBAL/RDBAH = ring physical address
   ├── Write RDLEN = 1024 (64 * 16)
   ├── Write RDH = 0
   └── Write RDT = 63 (N-1, tells HW all buffers are available)

6. Setup TX Ring
   ├── Allocate descriptor ring (1 KB, 128-byte aligned, below 4GB)
   ├── Allocate 1 shared TX frame buffer (2 KB, below 4GB)
   ├── Zero all descriptors (DD=0 initially)
   ├── Write TDBAL/TDBAH = ring physical address
   ├── Write TDLEN = 1024 (64 * 16)
   ├── Write TDH = 0
   └── Write TDT = 0

7. Enable TX
   ├── Write TIPG = 0x0060200A (standard inter-packet gap)
   └── Write TCTL = EN | PSP | CT(0x10) | COLD(0x40)

8. Enable RX
   └── Write RCTL = EN | BAM | SECRC | BSIZE_2048

9. Set Link Up
   ├── Write CTRL = SLU | ASDE (auto-speed + set link up)
   ├── Do NOT set FRCSPD or FRCDPX (let autoneg work)
   └── Poll STATUS.LU (bit 1) — link up in 2-5 seconds

10. Install SNP
    ├── Fill EFI_SIMPLE_NETWORK_MODE with MAC, sizes, capabilities
    ├── Build SNP function table (Start, Initialize, Transmit, Receive, GetStatus, ...)
    ├── Create WaitForPacket event
    ├── InstallProtocolInterface(SNP + DevicePath)
    └── Provide direct TX/RX bypass functions for ns_* stack
```

---

## SNP Integration

The e1000e driver exposes `EFI_SIMPLE_NETWORK_PROTOCOL` following the exact same pattern as IWN's `iwn_snp.c`.

### SNP Callbacks

| Callback | Implementation |
|----------|---------------|
| `Start` | Set state STOPPED → STARTED |
| `Stop` | Set state → STOPPED |
| `Initialize` | Hardware init (or confirm already done), state → INITIALIZED |
| `Reset` | Device reset + re-init |
| `Shutdown` | Disable TX/RX, state → STOPPED |
| `ReceiveFilters` | Store filter mask (unicast + broadcast) |
| `StationAddress` | Return MAC from mode struct |
| `Statistics` | Return NULL (not implemented) |
| `MCastIpToMac` | Return NULL (not implemented) |
| `NvData` | Return NULL (not implemented) |
| `GetStatus` | Poll RX, return interrupt status + recycled TX buffer |
| `Transmit` | Build Ethernet frame, write TX descriptor, kick doorbell |
| `Receive` | Poll RX ring, copy frame to caller's buffer |

### Direct Bypass Functions

On Dell E6510, SNP function pointers called via EFIAPI indirect call convention can hang (PIC/relocation issue observed with IWN). Provide direct bypass functions like IWN does:

```c
void    e1000e_snp_get_mac(uint8_t *mac);
int     e1000e_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                             const uint8_t *payload, int len);
int     e1000e_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len);
int     e1000e_snp_is_installed(void);
```

### ns_* Stack Integration

The custom network stack (`ns_net.c`) currently calls IWN's direct functions:

```c
// Current (WiFi):
iwn_snp_get_mac(g_ns.mac);
iwn_snp_direct_tx(dst_mac, ethertype, payload, len);
iwn_snp_direct_rx(frame, sizeof(frame), &frame_len);
```

For e1000e, `ns_net.c` needs to dispatch to whichever driver is available:

```c
// Updated (WiFi OR Ethernet):
if (e1000e_snp_is_installed()) {
    e1000e_snp_get_mac(g_ns.mac);
    // TX/RX via e1000e_snp_direct_*
} else if (iwn_snp_is_installed()) {
    iwn_snp_get_mac(g_ns.mac);
    // TX/RX via iwn_snp_direct_*
}
```

This is a small change to `ns_net.c` — add an e1000e path alongside the existing IWN path.

---

## TX Path

### `e1000e_transmit(dst_mac, ethertype, payload, len)`

```
1. Build Ethernet frame in TX buffer:
   ├── [0:5]   dst_mac (6 bytes)
   ├── [6:11]  src_mac (6 bytes, from our MAC)
   ├── [12:13] ethertype (2 bytes, big-endian)
   └── [14:]   payload (len bytes)

2. Fill TX descriptor:
   ├── buffer_addr = physical address of TX buffer
   ├── length = 14 + len (header + payload)
   ├── cmd = EOP | IFCS | RS
   └── status = 0

3. Write TDT = (cur + 1) % ring_size  (doorbell — HW starts transmit)

4. Poll status.DD until set (or timeout)
   └── HW sets DD when frame is on the wire

5. Advance cur index
```

For the SNP `Transmit` callback, the frame may arrive pre-assembled (HeaderSize == 0) or as separate components (HeaderSize != 0 with DestAddr/Protocol parameters). Both cases produce the same Ethernet frame.

---

## RX Path

### `e1000e_receive(buf, buf_size, frame_len)`

```
1. Check RX descriptor at current head:
   └── If status.DD == 0: return NOT_READY (no frame)

2. Read frame:
   ├── length = descriptor.length
   ├── Copy from RX buffer (descriptor.buffer_addr) to caller's buf
   └── *frame_len = length

3. Recycle descriptor:
   ├── Clear status (status = 0)
   └── buffer_addr remains (same buffer reused)

4. Advance tail:
   ├── cur = (cur + 1) % ring_size
   └── Write RDT = previous cur (return buffer to HW)

5. Return SUCCESS
```

The RX ring is a simple circular buffer. HW writes frames starting at head, advances head. SW reads frames starting at its `cur` pointer, recycles buffers by advancing tail.

---

## File Layout

```
bootai/e1000e/
├── e1000e.h            # Public API: probe, init, transmit, receive
├── e1000e_regs.h       # Register offsets and bit definitions
├── e1000e_hw.c         # PCI probe, MMIO, reset, MAC read, link setup
├── e1000e_ring.c       # DMA allocation, TX/RX ring setup, transmit, receive
└── e1000e_snp.c        # SNP protocol wrapper + direct bypass functions
```

**~600-800 lines total across 5 files.**

### Dependency on Existing Code

| What | Where | How Used |
|------|-------|----------|
| UEFI types (`EFI_STATUS`, `gBS`, etc.) | `efi/efi.h` | All files |
| PCI I/O protocol | `efi/efi.h` | `e1000e_hw.c` — PCI enumeration |
| SNP protocol types | `efi/efi.h` | `e1000e_snp.c` — protocol install |
| Custom stack dispatch | `efi/ns_net.c` | Calls `e1000e_snp_direct_*` |
| Main REPL | `efi/main.c` | Calls `e1000e_probe()` at startup |
| Makefile | `Makefile` | Add `.c` files to `EFI_SRC` |

No new dependencies. No new submodules. No firmware files to ship.

---

## Supported Device IDs

### Must-Have (Dell E6510)

| Device | PCI ID | Notes |
|--------|--------|-------|
| 82577LM | `0x10EA` | Primary target — Dell E6510 |
| 82577LC | `0x10EB` | Consumer variant of 82577 |

### Should-Have (same register interface, trivial to add)

These are all e1000e family — same register layout, same descriptor format, same initialization. Adding support is just adding PCI device IDs to the match table.

| Device | PCI ID | Common In |
|--------|--------|-----------|
| 82578DM | `0x10EF` | Desktop boards (2010) |
| 82578DC | `0x10F0` | Desktop boards (2010) |
| 82579LM | `0x1502` | ThinkPad T420/T520, Dell E6420 |
| 82579V | `0x1503` | Consumer desktops (2011) |
| I217-LM | `0x153A` | ThinkPad T440, Dell E7440 |
| I217-V | `0x153B` | Consumer desktops (Haswell) |
| I218-LM | `0x155A` | ThinkPad T450, Dell E7450 |
| I218-V | `0x1559` | Consumer desktops (Broadwell) |
| I219-LM | `0x156F` | ThinkPad T460+, Dell Latitude 5000+ |
| I219-V | `0x1570` | Consumer desktops (Skylake+) |

### Nice-to-Have (older e1000, different init quirks)

| Device | PCI ID | Common In |
|--------|--------|-----------|
| 82574L | `0x10D3` | Server boards, QEMU default e1000e |
| 82573L | `0x109A` | Older workstations |

### Not In Scope (different driver)

| Family | Why |
|--------|-----|
| Realtek RTL8168/8111 | Different register interface, separate driver needed |
| Broadcom NetXtreme | Different register interface |
| Intel igb (82575+) | Multi-queue, different descriptor format |
| Intel ixgbe (10GbE) | Different architecture entirely |

---

## QEMU Compatibility

QEMU can emulate an e1000e device for development:

```bash
# Replace virtio-net with e1000e in QEMU command:
-device e1000e,netdev=net0    # instead of: -device virtio-net-pci,netdev=net0
```

This puts an Intel 82574L (`0x10D3`) on the virtual PCI bus. Our driver can probe it, enabling full testing without real hardware. OVMF's built-in e1000 driver may also claim the device — we'd need to either:
1. Add `0x10D3` to our device table and probe before OVMF binds
2. Or use QEMU with `-device e1000e` and skip `ConnectController` (since we never call it anyway)

For development, option 2 is simplest. Our driver probes PCI directly and doesn't rely on UEFI's driver binding model.

---

## ICH/PCH Quirks

### Software Semaphore (may not be needed)

The ICH/PCH MAC shares NVM/PHY access with Intel Management Engine (ME/AMT). The Linux driver acquires a software semaphore before PHY/NVM access. However:

- We only read the MAC from RAL0/RAH0 (no NVM access needed)
- We don't configure the PHY directly (auto-negotiation handles link)
- iPXE skips PHY reset on 82577 (`INTEL_NO_PHY_RST`)

**Plan:** Skip the semaphore initially. If RAL0/RAH0 reads fail, add semaphore logic.

### PBA/PBS Errata

Some ICH/PCH variants need the Packet Buffer Allocation register (PBA, `0x1000`) and Packet Buffer Size register (PBS, `0x1008`) configured before device reset. iPXE handles this with the `INTEL_PBS_ERRATA` flag.

**Plan:** Set PBA and PBS to safe defaults before reset. Copy values from iPXE:
```c
write_reg(PBA, 0x000A);   /* 10KB RX, 6KB TX */
write_reg(PBS, 0x0010);   /* 16KB total */
```

### No PHY Reset

iPXE flags 82577/82578/82579 with `INTEL_NO_PHY_RST`. The PHY is expected to be initialized by BIOS. Do NOT write CTRL.PHY_RST on these parts.

---

## Error Handling

### Link Not Coming Up

If STATUS.LU doesn't set within 5 seconds:
1. Print diagnostic: CTRL, STATUS, PHY status
2. Try toggling SLU off then on
3. If still no link: report "no cable" and allow retry via REPL command

### MAC Address Invalid

If RAL0/RAH0 contain all-zeros or all-FF:
1. Try EERD fallback (3 word reads)
2. If EERD also fails: print error, do not install SNP

### DMA Allocation Failure

If `AllocatePages` fails for any DMA buffer:
1. Free all already-allocated buffers
2. Print error with requested/available memory
3. Return failure from `e1000e_init()`

---

## REPL Integration

New commands in `main.c`:

| Command | Action |
|---------|--------|
| `e1000e` | Show status: link up/down, speed, MAC address |
| `e1000e reset` | Reset device and re-initialize |

The driver is probed automatically at startup (same as IWN). If an e1000e device is found, it prints the MAC and link status. If WiFi (IWN) is also available, both are initialized — the custom stack prefers Ethernet over WiFi (wired is more reliable for debugging).

---

## Priority Order for Network Stack

When both Ethernet and WiFi are available:

```
1. UEFI IP4/TCP4 stack (QEMU with OVMF — already works)
2. e1000e → ns_* custom stack (wired Ethernet on real hardware)
3. IWN → ns_* custom stack (WiFi, when CCMP is fixed)
```

`net.c` already dispatches between UEFI stack and custom stack. The custom stack in `ns_net.c` needs to prefer e1000e over IWN when both are available.

---

## References

| Resource | URL / Path |
|----------|------------|
| iPXE intel.c (cleanest bare-metal impl) | https://dox.ipxe.org/intel_8c_source.html |
| iPXE intel.h (register defs) | https://dox.ipxe.org/intel_8h_source.html |
| OSDev Intel Ethernet i217 | https://wiki.osdev.org/Intel_Ethernet_i217 |
| OSDev Intel 8254x | https://wiki.osdev.org/Intel_8254x |
| Linux e1000e ich8lan.c | https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/intel/e1000e/ich8lan.c |
| Intel 82577 PHY Datasheet | https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82577-gbe-phy-datasheet.pdf |
| Intel 82574 MAC Datasheet | https://courses.cs.washington.edu/courses/cse451/16au/readings/e1000e.pdf |
| IWN driver (architecture reference) | `bootai/docs/architecture/iwn-wifi-driver.md` |
| IWN SNP integration | `bootai/iwn/iwn_snp.c` |
| IWN DMA patterns | `bootai/iwn/iwn_dma.c` |
| IWN PCI enumeration | `bootai/iwn/iwn_pci.c` |
| Custom network stack | `bootai/efi/ns_net.c`, `ns_net.h` |
