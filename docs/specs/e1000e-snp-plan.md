# e1000e SNP Driver — Phased Implementation Plan

> Step-by-step plan to bring up Intel wired Ethernet on the Dell E6510. Each phase is independently testable. Based on the [e1000e SNP Spec](e1000e-snp-spec.md).

---

## Phase Overview

| Phase | Description | Files | Est. Lines | Testable On |
|-------|-------------|-------|------------|-------------|
| 0 | PCI probe — find Intel 82577LM on bus | `e1000e.h`, `e1000e_regs.h`, `e1000e_hw.c` | ~200 | **DONE** — Dell confirmed |
| 1 | MMIO + Reset + MAC — PCI IO register access, reset, read MAC | `e1000e_hw.c`, `e1000e_regs.h`, `e1000e.h` | ~80 | **DONE** — Dell confirmed |
| 2 | RX ring — allocate DMA, set up receive descriptors, receive frames | `e1000e_ring.c` | ~150 | QEMU (ping) |
| 3 | TX ring — set up transmit descriptors, send frames | `e1000e_ring.c` | ~100 | QEMU (ARP reply) |
| 4 | SNP + ns_* integration — install SNP, wire into custom stack, DHCP | `e1000e_snp.c`, `ns_net.c` | ~250 | QEMU (curl) |
| 5 | Dell hardware test — real hardware bring-up and debug | (fixes only) | ~50 | Dell |
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

## Phase 2: RX Ring

**Goal:** Set up receive descriptors and receive raw Ethernet frames.

### Implementation in `e1000e_ring.c`

**Constants:**
```c
#define E1000E_RX_RING_COUNT  64
#define E1000E_RX_BUF_SIZE    2048
```

**DMA allocation (following IWN pattern):**
```c
typedef struct {
    uint64_t paddr;
    void    *vaddr;
    uint32_t size;
} e1000e_dma;

static int e1000e_dma_alloc(e1000e_dma *dma, uint32_t size) {
    UINTN pages = (size + 4095) / 4096;
    uint64_t addr = 0xFFFFFFFF;  /* below 4GB */
    EFI_STATUS st = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &addr);
    if (st != EFI_SUCCESS) return -1;
    dma->paddr = addr;
    dma->vaddr = (void *)(uintptr_t)addr;
    dma->size = (uint32_t)(pages * 4096);
    /* zero-fill */
    uint8_t *p = dma->vaddr;
    for (uint32_t i = 0; i < dma->size; i++) p[i] = 0;
    return 0;
}
```

**RX ring setup:**
```c
int e1000e_rx_init(void) {
    /* Allocate descriptor ring: 64 * 16 = 1024 bytes */
    e1000e_dma_alloc(&g_dev.rx_ring_dma, E1000E_RX_RING_COUNT * sizeof(e1000e_rx_desc));

    /* Allocate 64 frame buffers (2KB each) */
    for (int i = 0; i < E1000E_RX_RING_COUNT; i++) {
        e1000e_dma_alloc(&g_dev.rx_buf[i], E1000E_RX_BUF_SIZE);
        /* Point descriptor at buffer */
        g_dev.rx_descs[i].buffer_addr = g_dev.rx_buf[i].paddr;
        g_dev.rx_descs[i].status = 0;
    }

    /* Program registers */
    e1000e_write(RDBAL, (uint32_t)(g_dev.rx_ring_dma.paddr & 0xFFFFFFFF));
    e1000e_write(RDBAH, (uint32_t)(g_dev.rx_ring_dma.paddr >> 32));
    e1000e_write(RDLEN, E1000E_RX_RING_COUNT * sizeof(e1000e_rx_desc));
    e1000e_write(RDH, 0);
    e1000e_write(RDT, E1000E_RX_RING_COUNT - 1);

    /* Enable receiver */
    e1000e_write(RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);

    g_dev.rx_cur = 0;
    return 0;
}
```

**Receive (poll-based):**
```c
int e1000e_receive(uint8_t *buf, int buf_size, int *frame_len) {
    e1000e_rx_desc *desc = &g_dev.rx_descs[g_dev.rx_cur];

    if (!(desc->status & E1000_RXD_STAT_DD))
        return -1;  /* no frame */

    int len = desc->length;
    if (len > buf_size) len = buf_size;

    /* Copy frame from DMA buffer to caller */
    uint8_t *src = (uint8_t *)g_dev.rx_buf[g_dev.rx_cur].vaddr;
    for (int i = 0; i < len; i++) buf[i] = src[i];
    *frame_len = len;

    /* Recycle descriptor */
    desc->status = 0;
    int prev = g_dev.rx_cur;
    g_dev.rx_cur = (g_dev.rx_cur + 1) % E1000E_RX_RING_COUNT;
    e1000e_write(RDT, prev);  /* return buffer to HW */

    return 0;
}
```

**Link up:**
```c
int e1000e_link_up(void) {
    /* Set Link Up + Auto-Speed Detection */
    uint32_t ctrl = e1000e_read(CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);  /* let autoneg work */
    e1000e_write(CTRL, ctrl);

    /* Poll for link up (5 second timeout) */
    for (int i = 0; i < 50; i++) {
        gBS->Stall(100000);  /* 100ms */
        if (e1000e_read(STATUS) & E1000_STATUS_LU) {
            /* Print speed */
            uint32_t status = e1000e_read(STATUS);
            int speed = (status & E1000_STATUS_SPEED_1000) ? 1000 :
                        (status & E1000_STATUS_SPEED_100) ? 100 : 10;
            efi_print_ascii("e1000e: link up at ");
            /* print speed */
            efi_print_ascii(" Mbps\r\n");
            return 0;
        }
    }
    efi_print_ascii("e1000e: link timeout (no cable?)\r\n");
    return -1;
}
```

### Test

**QEMU:** After link up, the QEMU host will send ARP/DHCP traffic. We can verify frames arrive by adding a temporary debug print in `e1000e_receive()` that shows frame length and ethertype.

Add a temporary `e1000e poll` REPL command:
```c
/* Poll and print one received frame (debug) */
uint8_t frame[2048];
int frame_len;
if (e1000e_receive(frame, sizeof(frame), &frame_len) == 0) {
    /* print ethertype, length */
}
```

### Exit Criteria

- DMA allocation succeeds (no OOM)
- Link comes up within 5 seconds (QEMU: instant, Dell: 2-5s with cable)
- Frames received from network (ARP broadcasts from router, etc.)
- No memory leaks (all DMA freed on error paths)

---

## Phase 3: TX Ring

**Goal:** Set up transmit descriptors and send raw Ethernet frames.

### Implementation in `e1000e_ring.c`

**TX ring setup:**
```c
#define E1000E_TX_RING_COUNT  64
#define E1000E_TX_BUF_SIZE    2048

int e1000e_tx_init(void) {
    /* Allocate descriptor ring */
    e1000e_dma_alloc(&g_dev.tx_ring_dma, E1000E_TX_RING_COUNT * sizeof(e1000e_tx_desc));

    /* Allocate shared TX buffer */
    e1000e_dma_alloc(&g_dev.tx_buf, E1000E_TX_BUF_SIZE);

    /* Program registers */
    e1000e_write(TDBAL, (uint32_t)(g_dev.tx_ring_dma.paddr & 0xFFFFFFFF));
    e1000e_write(TDBAH, (uint32_t)(g_dev.tx_ring_dma.paddr >> 32));
    e1000e_write(TDLEN, E1000E_TX_RING_COUNT * sizeof(e1000e_tx_desc));
    e1000e_write(TDH, 0);
    e1000e_write(TDT, 0);

    /* Inter-packet gap: standard value */
    e1000e_write(TIPG, 0x0060200A);

    /* Enable transmitter */
    e1000e_write(TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                       (0x10 << 4) |   /* CT = 0x10 */
                       (0x40 << 12));   /* COLD = 0x40 */

    g_dev.tx_cur = 0;
    return 0;
}
```

**Transmit:**
```c
int e1000e_transmit(const uint8_t *frame, int len) {
    if (len < 14 || len > E1000E_TX_BUF_SIZE) return -1;

    /* Copy frame to DMA buffer */
    uint8_t *dst = (uint8_t *)g_dev.tx_buf.vaddr;
    for (int i = 0; i < len; i++) dst[i] = frame[i];

    /* Fill TX descriptor */
    e1000e_tx_desc *desc = &g_dev.tx_descs[g_dev.tx_cur];
    desc->buffer_addr = g_dev.tx_buf.paddr;
    desc->length = (uint16_t)len;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;

    /* Kick doorbell */
    int next = (g_dev.tx_cur + 1) % E1000E_TX_RING_COUNT;
    e1000e_write(TDT, next);

    /* Poll for completion (1ms timeout) */
    for (int i = 0; i < 100; i++) {
        if (desc->status & E1000_TXD_STAT_DD) break;
        gBS->Stall(10);  /* 10us */
    }

    g_dev.tx_cur = next;
    return 0;
}
```

**Higher-level send (builds Ethernet header):**
```c
int e1000e_send(const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, int payload_len) {
    uint8_t *buf = (uint8_t *)g_dev.tx_buf.vaddr;
    int total = 14 + payload_len;
    if (total > E1000E_TX_BUF_SIZE) return -1;

    /* Build Ethernet header */
    for (int i = 0; i < 6; i++) buf[i] = dst_mac[i];         /* dst */
    for (int i = 0; i < 6; i++) buf[6 + i] = g_dev.mac[i];   /* src */
    buf[12] = (uint8_t)(ethertype >> 8);
    buf[13] = (uint8_t)(ethertype & 0xFF);

    /* Copy payload */
    for (int i = 0; i < payload_len; i++) buf[14 + i] = payload[i];

    return e1000e_transmit(buf, total);
}
```

### Test

**QEMU ARP test:**
1. Initialize e1000e (Phase 0-2 + Phase 3)
2. Receive an ARP request from the QEMU host
3. Build and send an ARP reply
4. Verify with tcpdump on host: `sudo tcpdump -i bridge0 -n arp`

Or simpler: send a gratuitous ARP broadcast announcing our MAC, verify it appears on the host.

**Temporary REPL command:**
```
> e1000e arp
e1000e: sent gratuitous ARP (00:24:e8:xx:xx:xx → broadcast)
```

### Exit Criteria

- TX descriptor written and completed (DD set by hardware)
- Frame appears on the wire (verified via tcpdump or QEMU monitor)
- No TX timeout or hang

---

## Phase 4: SNP + ns_* Stack Integration

**Goal:** Install SNP, wire into the custom network stack, get DHCP, run HTTP server.

### Implementation

**`e1000e/e1000e_snp.c`** — SNP protocol wrapper (~250 lines)

Following the exact pattern from `iwn_snp.c`:

1. **SNP mode struct** — MAC, sizes, capabilities
2. **SNP callbacks** — Start, Initialize, Transmit, Receive, GetStatus, etc.
3. **Protocol install** — `gBS->InstallProtocolInterface()` for SNP + DevicePath
4. **WaitForPacket event** — `gBS->CreateEvent(EVT_NOTIFY_WAIT, ...)`
5. **Direct bypass functions:**
   ```c
   void e1000e_snp_get_mac(uint8_t *mac);
   int  e1000e_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                              const uint8_t *payload, int len);
   int  e1000e_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len);
   int  e1000e_snp_is_installed(void);
   ```

**`efi/ns_net.c`** — Add e1000e dispatch

Current code calls `iwn_snp_direct_*` directly. Add e1000e as a higher-priority path:

```c
/* In ns_init(): */
if (e1000e_snp_is_installed()) {
    e1000e_snp_get_mac(g_ns.mac);
    g_ns.driver = DRIVER_E1000E;
} else if (iwn_snp_is_installed()) {
    iwn_snp_get_mac(g_ns.mac);
    g_ns.driver = DRIVER_IWN;
}

/* In ns_raw_send(): */
if (g_ns.driver == DRIVER_E1000E)
    e1000e_snp_direct_tx(dst_mac, ethertype, payload, len);
else
    iwn_snp_direct_tx(dst_mac, ethertype, payload, len);

/* In ns_poll(): */
if (g_ns.driver == DRIVER_E1000E)
    ret = e1000e_snp_direct_rx(frame, sizeof(frame), &frame_len);
else
    ret = iwn_snp_direct_rx(frame, sizeof(frame), &frame_len);
```

**`efi/net.c`** — Trigger custom stack for e1000e

The existing `efi_net_init()` tries the UEFI IP4 stack first (works on QEMU), then falls back to the custom ns_* stack. For e1000e, add detection:

```c
/* In efi_net_init(), after UEFI stack attempt: */
if (!net_ready && e1000e_snp_is_installed()) {
    efi_print_ascii("  Using e1000e with custom network stack\r\n");
    ns_init(NULL);   /* NULL SNP — ns_net uses direct functions */
    ns_dhcp_start();
    /* poll until DHCP completes or timeout */
}
```

**`efi/main.c`** — Startup integration

```c
/* Near existing iwn_probe(): */
if (e1000e_probe() == 0) {
    if (e1000e_init() == 0) {
        efi_print_ascii("  e1000e: ready\r\n");
    }
}
```

### Test

**QEMU with `-device e1000e`:**

1. Build: `make clean && make efi`
2. User runs: `make run` (with e1000e device in QEMU flags)
3. At REPL: `serve 8080`
4. From Mac: `curl http://localhost:8080/api/health`

Expected: DHCP assigns IP, HTTP server responds, management API works.

**Note:** May need to remove OVMF's built-in e1000 driver binding. If OVMF claims the device first, our driver won't find it via PCI. Options:
- Use QEMU `-device e1000e` without OVMF network stack (remove network-enabled OVMF)
- Or just test on Dell where there's no competing driver

### Exit Criteria

- DHCP completes successfully over e1000e
- `serve 8080` serves HTTP responses
- `curl http://localhost:8080/api/health` returns valid JSON
- `curl -X POST http://localhost:8080/api/cmd -d '{"cmd":"ls"}'` executes and returns output
- `curl http://localhost:8080/api/log` returns log buffer

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
