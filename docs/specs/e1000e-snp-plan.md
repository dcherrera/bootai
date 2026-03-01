# e1000e SNP Driver — Phased Implementation Plan

> Step-by-step plan to bring up Intel wired Ethernet on the Dell E6510. Each phase is independently testable. Based on the [e1000e SNP Spec](e1000e-snp-spec.md).

---

## Phase Overview

| Phase | Description | Files | Est. Lines | Testable On |
|-------|-------------|-------|------------|-------------|
| 0 | PCI probe — find Intel 82577LM on bus | `e1000e.h`, `e1000e_regs.h`, `e1000e_hw.c` | ~200 | **DONE** — Dell confirmed |
| 1 | MMIO + Reset + MAC — PCI IO register access, reset, read MAC | `e1000e_hw.c`, `e1000e_regs.h`, `e1000e.h` | ~80 | **DONE** — Dell confirmed |
| 2 | Link + RX ring — link detect, DMA RX ring, poll-based frame receive | `e1000e_hw.c`, `e1000e.h`, `main.c` | ~180 | **DONE** — builds clean |
| 3 | TX ring + ARP — DMA TX ring, transmit, send, gratuitous ARP | `e1000e_hw.c`, `e1000e.h`, `main.c` | ~120 | **DONE** — builds clean |
| 4 | SNP + ns_* integration — wire into custom stack, DHCP | `e1000e_hw.c`, `ns_net.c`, `net.c`, `main.c` | ~70 | **DONE** — Dell confirmed |
| 5 | Dell hardware test — real hardware bring-up and debug | (fixes only) | ~50 | **DONE** — Dell confirmed |
| 6 | REPL commands + polish — `e1000e` status, priority over WiFi | `main.c` | ~50 | Both |

**Total: ~900 lines across 5 new files + minor edits to 3 existing files.**

---

## Phase 0: PCI Probe

**Goal:** Find the Intel 82577LM on the PCI bus and print its location.

### Files

**`e1000e/e1000e.h`** — Public API header

```c
/* Public API */
int  e1000e_probe(void);           /* Find device on PCI bus. Returns 0=found, -1=not found */
int  e1000e_init(void);            /* Full init: reset, MAC, rings, link. Returns 0=ok */
int  e1000e_is_up(void);           /* 1 if link up and ready */
void e1000e_get_mac(uint8_t *mac); /* Copy MAC address */

/* Data path (called by ns_net.c) */
int  e1000e_transmit(const uint8_t *frame, int len);
int  e1000e_receive(uint8_t *buf, int buf_size, int *frame_len);
```

**`e1000e/e1000e_regs.h`** — Register offsets and bit definitions

All register constants from the spec: CTRL, STATUS, RCTL, TCTL, descriptor ring registers, MAC registers, interrupt registers. Plus descriptor structs (`e1000e_rx_desc`, `e1000e_tx_desc`).

**`e1000e/e1000e_hw.c`** — PCI enumeration

```c
/* Device table */
static const struct { uint16_t device; const char *name; } e1000e_devices[] = {
    { 0x10EA, "82577LM" },
    { 0x10EB, "82577LC" },
    { 0x10EF, "82578DM" },
    { 0x1502, "82579LM" },
    { 0x1503, "82579V"  },
    { 0x153A, "I217-LM" },
    { 0x153B, "I217-V"  },
    { 0x155A, "I218-LM" },
    { 0x1559, "I218-V"  },
    { 0x156F, "I219-LM" },
    { 0x1570, "I219-V"  },
    { 0x10D3, "82574L"  },  /* QEMU e1000e */
    { 0, NULL }
};

int e1000e_probe(void) {
    /* LocateHandleBuffer(PciIoProtocol) */
    /* Walk handles, read vendor (0x8086) + device ID */
    /* Match against table */
    /* Enable MEMORY + BUS_MASTER attributes */
    /* Print: "e1000e: found <name> at bus:dev.func" */
    /* Save pci_io handle to global e1000e_dev struct */
}
```

### Pattern

Same as `iwn_pci.c`:
1. `gBS->LocateHandleBuffer(ByProtocol, &gEfiPciIoProtocolGuid, ...)`
2. Walk handles, `pci_io->Pci.Read()` for vendor/device
3. Match `0x8086` + device table
4. `pci_io->Attributes(Enable, MEMORY | BUS_MASTER)`
5. Save `pci_io` to global struct

### Makefile

Add to `EFI_SRC`:
```
e1000e/e1000e_hw.c e1000e/e1000e_ring.c e1000e/e1000e_snp.c
```

### Integration

In `main.c` startup (near `iwn_probe()`):
```c
e1000e_probe();  /* non-fatal if not found */
```

### Test

**QEMU:** Add `-device e1000e,netdev=net0` to QEMU flags. Build, run. Should print:
```
e1000e: found 82574L at 0:3.0
```

**Dell:** Build, flash USB, boot. Should print:
```
e1000e: found 82577LM at 0:25.0
```

If no e1000e device found, silently continue (not all machines have Intel wired Ethernet).

### Exit Criteria

- Builds with zero warnings
- Prints device name + PCI location on Dell and QEMU
- Does not interfere with IWN probe or existing network init

### Results (2026-03-01)

**Status: DONE**

Dell E6510 boot log:
```
e1000e:  found at 0:25.0
e1000e: PCI probe OK
```

- PCI probe found Intel device at bus 0, device 25, function 0 (standard ICH9 Ethernet location)
- PCI attributes (MMIO + Bus Master) enabled successfully
- No interference with IWN probe or existing network init (all other commands work)
- Zero new warnings in build

**Bug found and fixed:** Device name printed empty. Root cause: `static const` struct with `const char*` pointers — PE image has no base relocations (`reloc.c` is a dummy), so pointer values in static data are link-time addresses, invalid at runtime. Also broke sentinel check (`id->name != 0` was always true), causing table to iterate into garbage memory.

**Fix:** Changed `const char *name` to `char name[12]` (inline bytes, no pointer relocation needed). Changed sentinel check from `id->name != 0` to `id->device != 0`. Added hex device ID print for confirmation: `e1000e: 82577LM [0x10ea] at 0:25.0`.

**UEFI PE relocation lesson:** In this UEFI build (no real base relocations), never store pointers in static/global data. Use inline arrays or function-based lookups instead. This affects any future struct with `const char*`, function pointers, or other address-valued fields in static data.

---

## Phase 1: MMIO + Reset + MAC

**Goal:** Reset the device, read the MAC address via PCI IO Protocol.

### Key Design Decision: PCI IO Protocol (not volatile pointers)

The original plan proposed reading BAR0 manually and using `volatile uint32_t*` pointers. This was replaced with `pci_io->Mem.Read/Mem.Write` with BAR index 0 — the same pattern used by the IWN driver (`iwn_hw.c:15-27`). Benefits:
- No BAR0 address calculation needed
- No pointer-in-static-data relocation issue (the PE relocation bug from Phase 0)
- Proven working on Dell E6510 hardware

### Implementation in `e1000e_hw.c`

**Register access helpers** (matches `iwn_hw.c` pattern):
```c
static uint32_t e1000e_read(uint32_t reg) {
    uint32_t val;
    g_e1000e.pci_io->Mem.Read(g_e1000e.pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
    return val;
}

static void e1000e_write(uint32_t reg, uint32_t val) {
    g_e1000e.pci_io->Mem.Write(g_e1000e.pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
}
```

**EEPROM fallback** (if RAL0/RAH0 not populated after reset):
```c
static int e1000e_read_eeprom(uint16_t offset, uint16_t *data) {
    e1000e_write(E1000_EERD, ((uint32_t)offset << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);
    for (int i = 0; i < 1000; i++) {
        uint32_t val = e1000e_read(E1000_EERD);
        if (val & E1000_EERD_DONE) {
            *data = (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
            return 0;
        }
        gBS->Stall(1);
    }
    return -1;
}
```

**`e1000e_init()`** — called from `e1000e_probe()` after PCI match:
1. Disable interrupts: `IMC = 0xFFFFFFFF`
2. PBA/PBS errata (82577LM is ICH9-class): `PBA = 0x000A`, `PBS = 0x0010`
3. Global reset: read CTRL, set RST bit, write back
4. Stall 10ms
5. Disable interrupts again (reset clears IMC)
6. Read MAC from RAL0/RAH0 (hardware auto-loads from EEPROM on reset)
7. If RAH0.AV not set → EERD fallback at word offsets 0, 1, 2
8. Print MAC and "reset OK"

**New define in `e1000e_regs.h`:**
```c
#define E1000_RAH_AV (1u << 31)  /* Address Valid */
```

**MAC print** reuses `print_hex8()` from `iwn_hw.c` (declared extern).

### Test

**Dell:** Should print the real Dell MAC.

Expected output:
```
e1000e: 82577LM [0x10ea] at 0:25.0
e1000e: MAC xx:xx:xx:xx:xx:xx
e1000e: reset OK
e1000e: PCI probe OK
```

### Exit Criteria

- MAC address printed and valid (not all-zeros, not all-FF)
- Device reset completes without hang
- No interference with IWN or existing boot flow

### Results (2026-03-01)

**Status: DONE**

Dell E6510 boot log:
```
e1000e: 82577LM [0x10ea] at 0:25.0
e1000e: MAC 5c:26:0a:02:83:22
e1000e: reset OK
e1000e: PCI probe OK
```

- Reset completed in <10ms, no hang
- MAC read from RAL0/RAH0 (AV bit set, no EERD fallback needed)
- MAC `5c:26:0a` is a Dell OUI (Dell uses multiple OUI ranges)
- All existing commands still work (tensor self-test, netdiag, iwn)
- Zero new warnings in build
- ~80 lines added to `e1000e_hw.c`, 1 line to `e1000e_regs.h`, 1 line to `e1000e.h`

---

## Phase 2: Link Up + RX Ring

**Goal:** Add link detection, DMA-based RX ring, and poll-based frame receive. After this phase, `e1000e` brings up the link and prints speed, and `e1000e poll` receives and displays live Ethernet frames.

### Design Decisions

1. **Single file** — all code in `e1000e_hw.c` (no new `.c` files, no Makefile change).
2. **Contiguous buffer allocation** — one DMA alloc for 64x2048=128KB buffer pool (32 pages), carved by offset. Plus one alloc for the 64-entry descriptor ring (1 page). Two `AllocatePages` calls total.
3. **Direct RX** — no software FIFO. The hardware descriptor ring IS the queue. `e1000e_rx_poll()` checks DD bit directly.
4. **Link failure is non-fatal** — if no cable, print warning, still set up RX ring, return success.

### Implementation in `e1000e_hw.c` (~180 new lines)

**New structs:**
```c
typedef struct {
    uint64_t paddr;
    void    *vaddr;
    uint32_t size;
} e1000e_dma_info;

#define E1000E_RX_DESC_COUNT  64
#define E1000E_RX_BUF_SIZE    2048

typedef struct {
    e1000e_dma_info  desc_dma;   /* descriptor ring: 64 x 16 = 1024 bytes */
    e1000e_dma_info  buf_dma;    /* buffer pool: 64 x 2048 = 131072 bytes */
    e1000e_rx_desc  *descs;      /* == desc_dma.vaddr */
    uint32_t         tail;       /* software tail index */
} e1000e_rx_ring;
```

**Extended `e1000e_dev`** with `link_speed` and `rx` ring state.

**New functions:**

| Function | Visibility | Lines | Purpose |
|----------|-----------|-------|---------|
| `e1000e_dma_alloc()` | static | 15 | AllocatePages below 4GB, zero-fill (IWN pattern) |
| `e1000e_dma_free()` | static | 8 | FreePages cleanup |
| `e1000e_link_up()` | static | 30 | Set SLU+ASDE in CTRL, poll STATUS.LU for 5s, print speed/duplex |
| `e1000e_rx_init()` | static | 40 | Alloc desc ring + buffer pool, fill descs, program RDBAL/RDBAH/RDLEN/RDH/RDT, enable RCTL |
| `e1000e_rx_poll()` | public | 25 | Check DD bit, copy frame, recycle descriptor, advance RDT |
| `e1000e_poll_cmd()` | public | 40 | REPL handler: poll loop, print ethertype/length/src-MAC, exit on keypress |

**Modified `e1000e_init()`** — calls `e1000e_link_up()` + `e1000e_rx_init()` after MAC print, before "reset OK".

### Changes to Other Files

**`e1000e/e1000e.h`** (+4 lines) — declared `e1000e_rx_poll()` and `e1000e_poll_cmd()`.

**`efi/main.c`** (+6 lines) — added `e1000e poll` command (before existing `e1000e` check, blocked from API), updated help text to `e1000e [poll]`.

### Expected Output

**Dell with cable:**
```
> e1000e
  e1000e: 82577LM [0x10ea] at 0:25.0
  e1000e: MAC 5c:26:0a:02:83:22
  e1000e: waiting for link.... UP 1000 Mbps full-duplex
  e1000e: RX ring ready (64 descs, 2048B bufs)
  e1000e: reset OK
  e1000e: PCI probe OK
> e1000e poll
  e1000e: polling for frames (press any key to stop)...
  RX #1: 60 bytes, ethertype=0x0806 (ARP), src=aa:bb:cc:dd:ee:ff
  RX #2: 342 bytes, ethertype=0x0800 (IPv4), src=aa:bb:cc:dd:ee:ff
  e1000e: stopped. 2 frames received.
```

**Dell without cable:**
```
> e1000e
  e1000e: 82577LM [0x10ea] at 0:25.0
  e1000e: MAC 5c:26:0a:02:83:22
  e1000e: waiting for link.......... NO LINK (no cable?)
  e1000e: RX ring ready (64 descs, 2048B bufs)
  e1000e: reset OK
  e1000e: PCI probe OK
```

### Exit Criteria

- DMA allocation succeeds (no OOM)
- Link comes up within 5 seconds (Dell: 2-5s with cable)
- `e1000e poll` receives frames from network (ARP broadcasts from router, etc.)
- No memory leaks (all DMA freed on error paths)
- Zero new warnings in build

### Results (2026-03-01)

**Status: DONE** (builds clean, awaiting Dell hardware test)

- All code in single file `e1000e_hw.c` — no new files, no Makefile change
- DMA follows proven IWN pattern: `AllocatePages(AllocateMaxAddress)` below 4GB, byte-by-byte zero-fill
- Contiguous buffer pool: 1 alloc for 64x2048 buffers (32 pages) + 1 alloc for descriptor ring (1 page)
- Link detection: SLU+ASDE, polls 50x100ms, prints speed/duplex or "NO LINK"
- RX ring: RDLEN=1024 (128-byte aligned), RDH=0, RDT=63, RCTL=EN|BAM|SECRC|BSIZE_2K
- RX poll: checks `(tail+1) % 64`, DD bit, copies frame, recycles descriptor, writes RDT
- Poll command: non-blocking key check loop, prints ethertype labels (IPv4/ARP/IPv6), src MAC
- `e1000e poll` blocked from management API (interactive command)
- Zero new warnings in `make clean && make efi`
- ~180 lines added to `e1000e_hw.c`, 4 lines to `e1000e.h`, 6 lines to `main.c`

---

## Phase 3: TX Ring + ARP

**Goal:** Add DMA-based TX ring and transmit capability. `e1000e_transmit()` sends raw Ethernet frames, `e1000e_send()` builds Ethernet headers, and `e1000e arp` sends a gratuitous ARP broadcast verifiable on the network.

### Design Decisions

1. **Single file** — continue in `e1000e_hw.c`, no new files, no Makefile change.
2. **Single TX buffer** — unlike RX (64 contiguous buffers), TX uses one 2048-byte DMA buffer. TX is synchronous (copy frame, kick doorbell, poll DD, return), so only one descriptor is active at a time.
3. **Descriptor ring still 64 entries** — rotate through them to keep the ring valid. All point to the same buffer; only the active one matters.

### Implementation in `e1000e_hw.c` (~120 new lines)

**New structs:**
```c
#define E1000E_TX_DESC_COUNT  64
#define E1000E_TX_BUF_SIZE    2048

typedef struct {
    e1000e_dma_info  desc_dma;   /* descriptor ring: 64 x 16 = 1024 bytes */
    e1000e_dma_info  buf_dma;    /* single TX buffer: 2048 bytes */
    e1000e_tx_desc  *descs;      /* == desc_dma.vaddr */
    uint32_t         cur;        /* current descriptor index */
} e1000e_tx_ring;
```

**Extended `e1000e_dev`** with `e1000e_tx_ring tx` field.

**New functions:**

| Function | Visibility | Lines | Purpose |
|----------|-----------|-------|---------|
| `e1000e_tx_init()` | static | 30 | Alloc desc ring + single buffer, program TDBAL/TDBAH/TDLEN/TDH/TDT, set TIPG, enable TCTL |
| `e1000e_transmit()` | public | 25 | Copy frame to DMA buf, fill descriptor (EOP+IFCS+RS), kick TDT, poll DD (1ms timeout) |
| `e1000e_send()` | public | 15 | Build Ethernet header (dst+src MAC, ethertype), copy payload, call transmit |
| `e1000e_arp_cmd()` | public | 30 | Build+send gratuitous ARP broadcast, print result |

**Modified `e1000e_init()`** — calls `e1000e_tx_init()` after `e1000e_rx_init()`.

**TX init details:** TIPG = `0x0060200A` (standard inter-packet gap for copper). TCTL = EN | PSP | CT=0x10 | COLD=0x40 (standard full-duplex). TDH = 0, TDT = 0 (ring starts empty, unlike RX where all descs are available to HW).

**Transmit details:** Copy frame to `tx.buf_dma.vaddr`, fill descriptor at `tx.cur`: addr = `tx.buf_dma.paddr`, length = frame len, cmd = EOP|IFCS|RS, status = 0. Write `(cur+1) % 64` to TDT. Poll DD (100 iterations x 10us = 1ms timeout). Advance `tx.cur`. Return 0 on success, -1 on timeout or invalid length.

**Gratuitous ARP:** 28-byte ARP payload (hardware type=1, proto=0x0800, hlen=6, plen=4, opcode=1 request). Sender MAC = our MAC, sender IP = 0.0.0.0 (no DHCP yet). Target MAC = 00:00:00:00:00:00, target IP = 0.0.0.0. Dst Ethernet = FF:FF:FF:FF:FF:FF (broadcast). Ethertype = 0x0806.

### Changes to Other Files

**`e1000e/e1000e.h`** (+4 lines) — declared `e1000e_transmit()`, `e1000e_send()`, `e1000e_arp_cmd()`.

**`efi/main.c`** (+6 lines) — added `e1000e arp` command (before existing `e1000e` check), updated help text to `e1000e [poll|arp]`.

### Expected Output

```
> e1000e
  e1000e: 82577LM [0x10ea] at 0:25.0
  e1000e: MAC 5c:26:0a:02:83:22
  e1000e: waiting for link.... UP 1000 Mbps full-duplex
  e1000e: RX ring ready (64 descs, 2048B bufs)
  e1000e: TX ring ready (64 descs, 2048B buf)
  e1000e: reset OK
  e1000e: PCI probe OK
> e1000e arp
  e1000e: sending gratuitous ARP...
  e1000e: ARP sent (28 bytes, src=5c:26:0a:02:83:22)
```

Verify on Mac: `sudo tcpdump -i en0 -n arp` should show the gratuitous ARP from Dell's MAC.

### Exit Criteria

- TX descriptor written and completed (DD set by hardware)
- Frame appears on the wire (verified via tcpdump or QEMU monitor)
- No TX timeout or hang
- `e1000e poll` still works
- Zero new warnings in build

### Results (2026-03-01)

**Status: DONE** (builds clean, awaiting Dell hardware test)

- All code in single file `e1000e_hw.c` — no new files, no Makefile change
- TX ring: single 2048-byte DMA buffer shared across 64 descriptors, synchronous send
- TX init: TDLEN=1024, TDH=0, TDT=0, TIPG=0x0060200A, TCTL=EN|PSP|CT=0x10|COLD=0x40
- Transmit: copies frame to DMA buf, fills descriptor (EOP+IFCS+RS), kicks TDT, polls DD with 1ms timeout
- Send: builds 14-byte Ethernet header in DMA buf (dst MAC, src MAC, ethertype big-endian), copies payload, calls transmit
- Gratuitous ARP: 28-byte ARP request, sender=our MAC, IP=0.0.0.0, broadcast destination
- `e1000e arp` REPL command added, not blocked from API (non-interactive)
- Zero new warnings in `make clean && make efi`
- ~120 lines added to `e1000e_hw.c`, 4 lines to `e1000e.h`, 6 lines to `main.c`

---

## Phase 4: SNP + ns_* Stack Integration

**Goal:** Wire e1000e into the custom network stack so DHCP + HTTP server works on Dell hardware over wired Ethernet.

### Key Design Decision: No Separate SNP File

Unlike IWN, e1000e does **not** need a separate `e1000e_snp.c`:

- IWN's `iwn_snp.c` (422 lines) wraps 802.11 as Ethernet — handles encapsulation, CCMP, TX recycling, WaitForPacket, full SNP function pointer table
- e1000e is already raw Ethernet. `e1000e_send()` and `e1000e_rx_poll()` directly send/receive Ethernet frames
- On Dell, SNP function pointers **hang** (PE relocation issue). The ns_* stack bypasses SNP via `direct_tx/rx` functions
- Only 4 thin wrappers in `e1000e_hw.c` (~15 lines) needed to match the IWN direct API pattern

### Architecture: Driver Dispatch in ns_net.c

Added a `driver` field to `ns_state` with enum constants:

```c
#define NS_DRIVER_NONE   0
#define NS_DRIVER_IWN    1
#define NS_DRIVER_E1000E 2
```

Priority: **e1000e > IWN** (wired Ethernet preferred over WiFi).

### Implementation

**`e1000e/e1000e_hw.c`** — 4 thin SNP wrappers (+15 lines):

```c
int e1000e_snp_is_installed(void) {
    return g_e1000e_found && g_e1000e.link_up;
}

void e1000e_snp_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++) mac[i] = g_e1000e.mac[i];
}

int e1000e_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                          const uint8_t *payload, int len) {
    return e1000e_send(dst_mac, ethertype, payload, len);
}

int e1000e_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len) {
    return e1000e_rx_poll(buf, buf_size, frame_len) ? 0 : -1;
}
```

Note: `direct_rx` inverts the return value — `e1000e_rx_poll()` returns 1=got frame/0=nothing, but the ns_* convention is 0=got frame/-1=nothing.

**`efi/ns_net.h`** — Driver enum + field in `ns_state` (+5 lines)

**`efi/ns_net.c`** — Driver dispatch (~25 lines changed):

- `ns_init()`: auto-detect e1000e vs IWN (e1000e preferred)
- `ns_raw_send()`: dispatch to `e1000e_snp_direct_tx()` or `iwn_snp_direct_tx()`
- `ns_poll()`: dispatch to `e1000e_snp_direct_rx()` or `iwn_snp_direct_rx()`
- Removed `if (!g_ns.snp) return;` guards — replaced with driver enum checks

**`efi/net.c`** — e1000e custom stack path (~10 lines changed):

- Added `e1000e_snp_is_installed()` to ConnectController guard (skip on Dell with e1000e)
- Added e1000e custom stack path before IWN fallback in `efi_net_init()`

**`efi/main.c`** — Auto-probe e1000e at startup (~3 lines):

- `e1000e_probe()` called before `efi_net_init()` in `app_main()`, after `efi_load_drivers()`

### Startup Flow (Dell with Ethernet)

```
efi_main() → app_main()
  → efi_fs_init()
  → ba_mem_init()
  → efi_load_drivers()
  → e1000e_probe()          ← PCI probe + init + link + RX/TX
  → efi_net_init()
      → try UEFI IP4 stack  ← fails on Dell (no DXE drivers)
      → e1000e_snp_is_installed()?  ← YES (found + link up)
      → ns_init(NULL)       ← detects e1000e, sets driver=E1000E
      → ns_dhcp_run()       ← DHCP over e1000e
      → net_available = 1
  → efi_wifi_auto_connect() ← skipped or secondary
  → tensor_self_test()
  → command_repl()
```

### Files Changed

| File | Change | Lines |
|------|--------|-------|
| `e1000e/e1000e_hw.c` | 4 SNP wrapper functions | +15 |
| `e1000e/e1000e.h` | Declare 4 wrappers | +5 |
| `efi/efi.h` | Declare e1000e SNP functions | +6 |
| `efi/ns_net.h` | Driver enum + field in ns_state | +5 |
| `efi/ns_net.c` | Driver dispatch in init/send/poll | ~25 changed |
| `efi/net.c` | e1000e custom stack path + guard | ~10 changed |
| `efi/main.c` | Auto-probe e1000e before net init | ~3 changed |
| **Total** | | ~70 new/changed lines |

### Results (2026-03-01)

**Status: DONE — verified on Dell E6510 hardware**

Dell E6510 boot log:
```
e1000e: 82577LM [0x10ea] at 0:25.0
e1000e: MAC 5c:26:0a:02:83:22
e1000e: waiting for link UP 10 Mbps full-duplex
e1000e: RX ring ready (64 descs, 2048B bufs)
e1000e: TX ring ready (64 descs, 2048B buf)
e1000e: reset OK
e1000e: PCI probe OK
Initializing network...
No UEFI IP stack — using e1000e custom stack
ns: init MAC 5C:26:0A:02:83:22
DHCP: starting...
DHCP: DISCOVER (250 bytes)
DHCP: TX done
DHCP: OFFER 192.168.0.101 from 192.168.0.1
DHCP: REQUEST
DHCP: ACK — IP 192.168.0.101 GW 192.168.0.1 Mask 255.255.255.0
Network: ready
```

- e1000e auto-probed at startup, link came up at 10 Mbps full-duplex
- Custom ns_* stack detected e1000e driver, got MAC, ran DHCP successfully
- IP 192.168.0.101 assigned, HTTP server (`serve 8080`) functional
- `curl http://192.168.0.101:8080/api/health` returns JSON (auth required — needs `wifi.conf` token)
- Zero new warnings in build
- ~70 lines new/changed across 7 files — no new `.c` files, no Makefile changes

**Note:** Link speed shows 10 Mbps (expected 1000 Mbps). The 82577LM may need auto-negotiation time or the cable/switch is limiting. Functionally correct — DHCP and HTTP work fine at 10 Mbps.

---

## Phase 5: Dell Hardware Test

**Goal:** Verify everything works on the actual Dell E6510.

### Prerequisites

- Ethernet cable connected from Dell to router/switch (or directly to Mac with link-local)
- USB flashed with latest build
- Mac on same network

### Test Sequence

```
1. Flash USB:
   make clean && make DEBUG_REMOTE=1 efi
   # mount + copy to USB

2. Boot Dell → menu → [1] Run Live

3. Expected startup output:
   e1000e: found 82577LM at 0:25.0
   e1000e: MAC 00:24:e8:xx:xx:xx
   e1000e: reset complete
   e1000e: link up at 1000 Mbps
   Using e1000e with custom network stack
   DHCP: offered 192.168.1.xxx
   DHCP: bound to 192.168.1.xxx

4. At REPL:
   > serve 8080
   Starting HTTP server on port 8080...

5. From Mac:
   curl http://192.168.1.xxx:8080/api/health
   curl -X POST http://192.168.1.xxx:8080/api/cmd -d '{"cmd":"ls"}'
   curl http://192.168.1.xxx:8080/api/log
```

### Likely Issues (from IWN experience)

| Issue | Symptom | Fix |
|-------|---------|-----|
| BAR0 mapping wrong | Crash on first register read | Check if BAR is 64-bit, fix address calculation |
| Link timeout | "no cable?" | Check cable, try forcing speed (CTRL.FRCSPD + SPD_100) |
| No frames received | Poll returns -1 forever | Check RCTL settings, try promiscuous mode (UPE+MPE) |
| TX hangs | DD never set | Check TCTL, verify DMA buffer is below 4GB |
| DHCP fails | No DHCP offer received | Check ARP works first, verify broadcast frames are sent |
| SNP function pointers hang | Same Dell issue as IWN | Already mitigated with direct bypass functions |
| PBA/PBS errata | Reset hangs or silent failure | Verify PBA/PBS values before reset |

### Debug Strategy

If networking fails on Dell:
1. Read `bootai.log` from USB (mount on Mac)
2. Check register dumps: CTRL, STATUS, RCTL, TCTL, RDH/RDT, TDH/TDT
3. Use management API if wired Ethernet works but something else is broken (circular dependency — this IS the wired Ethernet test)
4. Add `e1000e dump` REPL command to print all register values

### Exit Criteria

- Management API accessible from Mac over wired Ethernet
- Can execute remote commands: `iwn scan`, `iwn connect`, etc.
- Can stream logs remotely
- Can debug CCMP WiFi issues without touching Dell keyboard

---

## Phase 6: REPL Commands + Polish

**Goal:** Add status commands and finalize priority order.

### REPL Commands

**`e1000e`** — Show status:
```
> e1000e
  e1000e: 82577LM
  MAC: 00:24:e8:xx:xx:xx
  Link: up (1000 Mbps, full duplex)
  RX: 1234 frames, TX: 56 frames
  IP: 192.168.1.100 (via DHCP)
```

**`e1000e reset`** — Reset device and re-initialize:
```
> e1000e reset
  e1000e: resetting...
  e1000e: link up at 1000 Mbps
```

### Network Priority

Finalize the priority order in `net.c` / `ns_net.c`:

```
1. UEFI IP4/TCP4 (QEMU with OVMF) — checked first
2. e1000e → ns_* stack (wired Ethernet) — preferred on real hardware
3. IWN → ns_* stack (WiFi) — fallback when wired not available
```

### Makefile — QEMU e1000e Option

Add a QEMU target that uses e1000e instead of virtio-net for development testing:

```makefile
run-e1000e: all_bootai.efi
	# Same as run but with -device e1000e instead of virtio-net-pci
```

### Exit Criteria

- `e1000e` command shows accurate status
- Network priority is correct (Ethernet > WiFi)
- Documentation updated (roadmap, architecture docs)
- Zero warnings in build

---

## Summary: Build + Test Quick Reference

```bash
# Build
cd bootai
make clean && make efi              # Normal build
make clean && make DEBUG_REMOTE=1 efi  # Debug build (no auth on management API)

# QEMU test (user runs — interactive)
# Edit Makefile: change virtio-net-pci → e1000e in run target
make run

# Dell test
diskutil mount /dev/diskNs1
cp all_bootai.efi /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI
diskutil eject /dev/diskN
# Boot Dell, plug in Ethernet cable

# Remote debug from Mac
curl http://<dell-ip>:8080/api/health
curl -X POST http://<dell-ip>:8080/api/cmd -d '{"cmd":"iwn scan"}'
curl http://<dell-ip>:8080/api/log
```

---

## Dependencies

| Dependency | Status | Notes |
|------------|--------|-------|
| PCI I/O protocol | Available | Dell E6510 UEFI exposes PCI I/O |
| 4GB DMA memory | Available | Dell has 8GB RAM, UEFI allocates below 4GB |
| Custom ns_* stack | Done | ARP, DHCP, IP, UDP, TCP all working (from IWN Phase 5c) |
| HTTP server | Done | Phase 5b |
| Management API | Done | Just implemented |
| Ethernet cable | **Required** | User must have an Ethernet cable for Dell |

---

## Timeline Estimate

| Phase | Effort |
|-------|--------|
| 0: PCI probe | Small — copy IWN pattern |
| 1: MMIO + MAC | Small — straightforward register reads |
| 2: RX ring | Medium — DMA allocation + descriptor setup |
| 3: TX ring | Small — similar to RX |
| 4: SNP + ns_* | Medium — integration + testing |
| 5: Dell test | Unknown — depends on hardware quirks |
| 6: Polish | Small — REPL commands + docs |
