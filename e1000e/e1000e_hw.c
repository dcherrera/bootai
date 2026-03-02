/*
 * e1000e_hw.c — Intel e1000e Ethernet driver for BootAI
 *
 * Phase 0: PCI probe — walk EFI_PCI_IO_PROTOCOL handles, match device IDs.
 * Phase 1: HW init — reset, MAC read, disable interrupts.
 * Phase 2: Link detect, DMA-based RX ring, poll-based frame receive.
 */

#include "e1000e.h"
#include "e1000e_regs.h"

/* PCI IO Protocol GUID — defined in iwn_pci.c, declared extern in efi.h */
/* Do NOT redefine here; the linker resolves it from iwn_pci.o */

/* ---- DMA memory descriptor ---- */
typedef struct {
    uint64_t paddr;
    void    *vaddr;
    uint32_t size;
} e1000e_dma_info;

/* ---- RX ring constants ---- */
#define E1000E_RX_DESC_COUNT  64
#define E1000E_RX_BUF_SIZE    2048

/* ---- RX ring state ---- */
typedef struct {
    e1000e_dma_info  desc_dma;   /* descriptor ring: 64 x 16 = 1024 bytes */
    e1000e_dma_info  buf_dma;    /* buffer pool: 64 x 2048 = 131072 bytes */
    e1000e_rx_desc  *descs;      /* == desc_dma.vaddr */
    uint32_t         tail;       /* software tail index */
} e1000e_rx_ring;

/* ---- TX ring constants ---- */
#define E1000E_TX_DESC_COUNT  64
#define E1000E_TX_BUF_SIZE    2048

/* ---- TX ring state ---- */
typedef struct {
    e1000e_dma_info  desc_dma;   /* descriptor ring: 64 x 16 = 1024 bytes */
    e1000e_dma_info  buf_dma;    /* single TX buffer: 2048 bytes */
    e1000e_tx_desc  *descs;      /* == desc_dma.vaddr */
    uint32_t         cur;        /* current descriptor index */
} e1000e_tx_ring;

/* Device state — persists across REPL commands */
typedef struct {
    EFI_PCI_IO_PROTOCOL *pci_io;
    uint16_t device_id;
    const char *device_name;
    uint8_t mac[6];         /* filled in Phase 1 */
    int link_up;            /* filled in Phase 2 */
    uint16_t link_speed;    /* 10, 100, or 1000 */
    e1000e_rx_ring rx;      /* RX ring state */
    e1000e_tx_ring tx;      /* TX ring state */
} e1000e_dev;

static e1000e_dev g_e1000e;
int g_e1000e_found = 0;

/* ---- MMIO register access (via PCI IO Protocol, BAR0) ---- */

static uint32_t e1000e_read(uint32_t reg) {
    uint32_t val;
    g_e1000e.pci_io->Mem.Read(g_e1000e.pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
    return val;
}

static void e1000e_write(uint32_t reg, uint32_t val) {
    g_e1000e.pci_io->Mem.Write(g_e1000e.pci_io, EfiPciIoWidthUint32, 0, reg, 1, &val);
}

/* ---- EEPROM read fallback (if RAL0/RAH0 not populated) ---- */

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

/* ---- Print helpers (print_hex8 from iwn_hw.c, declared extern) ---- */

extern void print_hex8(uint8_t val);

static void print_mac(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (i > 0) efi_print_ascii(":");
        print_hex8(mac[i]);
    }
}

/* ---- Freestanding helpers ---- */

static void print_dec_e1000e(UINTN val) {
    if (val == 0) { efi_print_ascii("0"); return; }
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    while (val > 0) {
        buf[--i] = '0' + (int)(val % 10);
        val /= 10;
    }
    efi_print_ascii(&buf[i]);
}

static void print_hex16(uint16_t val) {
    char buf[5];
    for (int i = 3; i >= 0; i--) {
        int nibble = val & 0xF;
        buf[i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
        val >>= 4;
    }
    buf[4] = '\0';
    efi_print_ascii(buf);
}

/* ---- DMA memory allocation (below 4 GB, IWN pattern) ---- */

static int e1000e_dma_alloc(e1000e_dma_info *dma, uint32_t size) {
    UINTN pages = (size + 4095) / 4096;
    uint64_t addr = 0xFFFFFFFF;  /* allocate below 4 GB */

    EFI_STATUS s = gBS->AllocatePages(1 /* AllocateMaxAddress */,
                                       EfiLoaderData, pages, &addr);
    if (s != EFI_SUCCESS) return -1;

    dma->paddr = addr;
    dma->vaddr = (void *)(uintptr_t)addr;
    dma->size = (uint32_t)(pages * 4096);

    uint8_t *p = (uint8_t *)dma->vaddr;
    for (uint32_t i = 0; i < dma->size; i++)
        p[i] = 0;

    return 0;
}

static void e1000e_dma_free(e1000e_dma_info *dma) {
    if (dma->vaddr) {
        gBS->FreePages(dma->paddr, dma->size / 4096);
        dma->vaddr = 0;
        dma->paddr = 0;
        dma->size = 0;
    }
}

/* ---- Phase 2: Link detection ---- */

static void e1000e_link_up(void) {
    /* Set Link Up + Auto-Speed Detection, clear Force Speed/Duplex */
    uint32_t ctrl = e1000e_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPX);
    e1000e_write(E1000_CTRL, ctrl);

    /* Poll STATUS.LU for up to 5 seconds */
    efi_print_ascii("  e1000e: waiting for link");
    for (int i = 0; i < 50; i++) {
        gBS->Stall(100000);  /* 100ms */
        uint32_t status = e1000e_read(E1000_STATUS);
        if (status & E1000_STATUS_LU) {
            /* Decode speed from STATUS bits */
            uint16_t speed = 10;
            if (status & E1000_STATUS_SPEED_1000) speed = 1000;
            else if (status & E1000_STATUS_SPEED_100) speed = 100;
            const char *duplex = (status & E1000_STATUS_FD) ? "full" : "half";
            g_e1000e.link_up = 1;
            g_e1000e.link_speed = speed;
            efi_print_ascii(" UP ");
            print_dec_e1000e(speed);
            efi_print_ascii(" Mbps ");
            efi_print_ascii(duplex);
            efi_print_ascii("-duplex\r\n");
            return;
        }
        efi_print_ascii(".");
    }

    /* No link after 5 seconds */
    g_e1000e.link_up = 0;
    g_e1000e.link_speed = 0;
    efi_print_ascii(" NO LINK (no cable?)\r\n");
}

/* ---- Phase 2: RX ring initialization ---- */

static int e1000e_rx_init(void) {
    e1000e_rx_ring *rx = &g_e1000e.rx;

    /* Allocate descriptor ring: 64 x 16 = 1024 bytes (1 page) */
    if (e1000e_dma_alloc(&rx->desc_dma, E1000E_RX_DESC_COUNT * sizeof(e1000e_rx_desc)) != 0) {
        efi_print_ascii("  e1000e: RX desc alloc failed\r\n");
        return -1;
    }

    /* Allocate contiguous buffer pool: 64 x 2048 = 131072 bytes (32 pages) */
    if (e1000e_dma_alloc(&rx->buf_dma, E1000E_RX_DESC_COUNT * E1000E_RX_BUF_SIZE) != 0) {
        efi_print_ascii("  e1000e: RX buf alloc failed\r\n");
        e1000e_dma_free(&rx->desc_dma);
        return -1;
    }

    rx->descs = (e1000e_rx_desc *)rx->desc_dma.vaddr;
    rx->tail = E1000E_RX_DESC_COUNT - 1;

    /* Fill each descriptor: point into contiguous buffer at offset i * 2048 */
    for (int i = 0; i < E1000E_RX_DESC_COUNT; i++) {
        rx->descs[i].addr = rx->buf_dma.paddr + (uint64_t)(i * E1000E_RX_BUF_SIZE);
        rx->descs[i].length = 0;
        rx->descs[i].checksum = 0;
        rx->descs[i].status = 0;
        rx->descs[i].errors = 0;
        rx->descs[i].special = 0;
    }

    /* Program RX descriptor base address */
    e1000e_write(E1000_RDBAL, (uint32_t)(rx->desc_dma.paddr & 0xFFFFFFFF));
    e1000e_write(E1000_RDBAH, (uint32_t)(rx->desc_dma.paddr >> 32));

    /* Descriptor ring length in bytes (must be 128-byte aligned) */
    e1000e_write(E1000_RDLEN, E1000E_RX_DESC_COUNT * sizeof(e1000e_rx_desc));

    /* Head = 0, Tail = N-1 (all descriptors available to HW) */
    e1000e_write(E1000_RDH, 0);
    e1000e_write(E1000_RDT, E1000E_RX_DESC_COUNT - 1);

    /* Enable receiver: accept broadcast, strip CRC, 2K buffers */
    e1000e_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                              E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2K);

    efi_print_ascii("  e1000e: RX ring ready (64 descs, 2048B bufs)\r\n");
    return 0;
}

/* ---- Phase 3: TX ring initialization ---- */

static int e1000e_tx_init(void) {
    e1000e_tx_ring *tx = &g_e1000e.tx;

    /* Allocate descriptor ring: 64 x 16 = 1024 bytes */
    if (e1000e_dma_alloc(&tx->desc_dma, E1000E_TX_DESC_COUNT * sizeof(e1000e_tx_desc)) != 0) {
        efi_print_ascii("  e1000e: TX desc alloc failed\r\n");
        return -1;
    }

    /* Allocate single TX buffer: 2048 bytes */
    if (e1000e_dma_alloc(&tx->buf_dma, E1000E_TX_BUF_SIZE) != 0) {
        efi_print_ascii("  e1000e: TX buf alloc failed\r\n");
        e1000e_dma_free(&tx->desc_dma);
        return -1;
    }

    tx->descs = (e1000e_tx_desc *)tx->desc_dma.vaddr;
    tx->cur = 0;

    /* All descriptors point to the same buffer; only the active one matters */
    for (int i = 0; i < E1000E_TX_DESC_COUNT; i++) {
        tx->descs[i].addr = tx->buf_dma.paddr;
        tx->descs[i].length = 0;
        tx->descs[i].cso = 0;
        tx->descs[i].cmd = 0;
        tx->descs[i].status = 0;
        tx->descs[i].css = 0;
        tx->descs[i].special = 0;
    }

    /* Program TX descriptor base address */
    e1000e_write(E1000_TDBAL, (uint32_t)(tx->desc_dma.paddr & 0xFFFFFFFF));
    e1000e_write(E1000_TDBAH, (uint32_t)(tx->desc_dma.paddr >> 32));

    /* Descriptor ring length in bytes */
    e1000e_write(E1000_TDLEN, E1000E_TX_DESC_COUNT * sizeof(e1000e_tx_desc));

    /* Head = 0, Tail = 0 (ring starts empty) */
    e1000e_write(E1000_TDH, 0);
    e1000e_write(E1000_TDT, 0);

    /* Inter-Packet Gap: standard copper values */
    e1000e_write(E1000_TIPG, 0x0060200A);

    /* Enable transmitter: EN + PSP + CT=0x10 + COLD=0x40 (full-duplex) */
    e1000e_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                              (0x10 << E1000_TCTL_CT_SHIFT) |
                              (0x40 << E1000_TCTL_COLD_SHIFT));

    efi_print_ascii("  e1000e: TX ring ready (64 descs, 2048B buf)\r\n");
    return 0;
}

/* ---- Phase 3: TX diagnostic counters ---- */

static uint32_t tx_ok_count = 0;
static uint32_t tx_fail_count = 0;

uint32_t e1000e_tx_ok_count(void)   { return tx_ok_count; }
uint32_t e1000e_tx_fail_count(void) { return tx_fail_count; }

/* ---- Phase 3: Transmit raw Ethernet frame ---- */

int e1000e_transmit(const uint8_t *frame, int len) {
    if (len < 14 || len > E1000E_TX_BUF_SIZE)
        return -1;

    e1000e_tx_ring *tx = &g_e1000e.tx;

    /* Clear pending interrupt causes to prevent NIC stalls */
    (void)e1000e_read(E1000_ICR);

    /* Copy frame to DMA buffer */
    uint8_t *dst = (uint8_t *)tx->buf_dma.vaddr;
    for (int i = 0; i < len; i++)
        dst[i] = frame[i];

    /* Fill descriptor at current index */
    e1000e_tx_desc *desc = &tx->descs[tx->cur];
    desc->addr = tx->buf_dma.paddr;
    desc->length = (uint16_t)len;
    desc->cso = 0;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    /* Kick doorbell: advance TDT */
    uint32_t next = (tx->cur + 1) % E1000E_TX_DESC_COUNT;
    e1000e_write(E1000_TDT, next);

    /* Poll DD bit (100 iterations x 10us = 1ms timeout) */
    for (int i = 0; i < 100; i++) {
        if (desc->status & E1000_TXD_STAT_DD) {
            tx->cur = next;
            tx_ok_count++;
            return 0;
        }
        gBS->Stall(10);
    }

    /* Timeout — try recovery: re-read ICR, check link status */
    (void)e1000e_read(E1000_ICR);
    tx->cur = next;
    tx_fail_count++;
    return -1;
}

/* ---- Phase 3: Send with Ethernet header ---- */

int e1000e_send(const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, int payload_len) {
    int total = 14 + payload_len;
    if (total > E1000E_TX_BUF_SIZE)
        return -1;

    /* Build frame in DMA buffer directly */
    uint8_t *buf = (uint8_t *)g_e1000e.tx.buf_dma.vaddr;

    /* Destination MAC [0..5] */
    for (int i = 0; i < 6; i++) buf[i] = dst_mac[i];
    /* Source MAC [6..11] */
    for (int i = 0; i < 6; i++) buf[6 + i] = g_e1000e.mac[i];
    /* Ethertype [12..13] big-endian */
    buf[12] = (uint8_t)(ethertype >> 8);
    buf[13] = (uint8_t)(ethertype & 0xFF);
    /* Payload [14..] */
    for (int i = 0; i < payload_len; i++) buf[14 + i] = payload[i];

    return e1000e_transmit(buf, total);
}

/* ---- Phase 3: Gratuitous ARP command ---- */

void e1000e_arp_cmd(void) {
    if (!g_e1000e_found) {
        efi_print_ascii("  e1000e: not probed yet. Run 'e1000e' first.\r\n");
        return;
    }

    efi_print_ascii("  e1000e: sending gratuitous ARP...\r\n");

    /* Build 28-byte ARP payload */
    uint8_t arp[28];
    /* Hardware type: Ethernet (1) */
    arp[0] = 0x00; arp[1] = 0x01;
    /* Protocol type: IPv4 (0x0800) */
    arp[2] = 0x08; arp[3] = 0x00;
    /* Hardware addr len: 6, Protocol addr len: 4 */
    arp[4] = 6; arp[5] = 4;
    /* Opcode: ARP request (1) */
    arp[6] = 0x00; arp[7] = 0x01;
    /* Sender MAC */
    for (int i = 0; i < 6; i++) arp[8 + i] = g_e1000e.mac[i];
    /* Sender IP: 0.0.0.0 (no DHCP yet) */
    arp[14] = 0; arp[15] = 0; arp[16] = 0; arp[17] = 0;
    /* Target MAC: 00:00:00:00:00:00 */
    for (int i = 0; i < 6; i++) arp[18 + i] = 0x00;
    /* Target IP: 0.0.0.0 */
    arp[24] = 0; arp[25] = 0; arp[26] = 0; arp[27] = 0;

    /* Send to broadcast, ethertype 0x0806 (ARP) */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int ret = e1000e_send(bcast, 0x0806, arp, 28);

    if (ret == 0) {
        efi_print_ascii("  e1000e: ARP sent (28 bytes, src=");
        print_mac(g_e1000e.mac);
        efi_print_ascii(")\r\n");
    } else {
        efi_print_ascii("  e1000e: ARP send failed (TX timeout)\r\n");
    }
}

/* ---- Phase 2: RX poll (check for received frames) ---- */

int e1000e_rx_poll(uint8_t *buf, int buf_size, int *frame_len) {
    e1000e_rx_ring *rx = &g_e1000e.rx;
    uint32_t next = (rx->tail + 1) % E1000E_RX_DESC_COUNT;
    e1000e_rx_desc *desc = &rx->descs[next];

    if (!(desc->status & E1000_RXD_STAT_DD))
        return 0;  /* no frame ready */

    /* Copy frame to caller's buffer */
    int len = desc->length;
    if (len > buf_size) len = buf_size;
    uint8_t *src = (uint8_t *)(uintptr_t)desc->addr;
    for (int i = 0; i < len; i++)
        buf[i] = src[i];
    *frame_len = len;

    /* Recycle descriptor: clear status, HW can reuse */
    desc->status = 0;
    desc->length = 0;
    desc->errors = 0;

    /* Advance tail and write RDT so HW knows it can use this descriptor */
    rx->tail = next;
    e1000e_write(E1000_RDT, rx->tail);

    return 1;
}

/* ---- Phase 2: Poll command (REPL handler) ---- */

void e1000e_poll_cmd(void) {
    if (!g_e1000e_found) {
        efi_print_ascii("  e1000e: not probed yet. Run 'e1000e' first.\r\n");
        return;
    }

    efi_print_ascii("  e1000e: polling for frames (press any key to stop)...\r\n");

    uint8_t frame[2048];
    int frame_len = 0;
    uint32_t count = 0;

    for (;;) {
        /* Check for received frame */
        if (e1000e_rx_poll(frame, sizeof(frame), &frame_len)) {
            count++;
            efi_print_ascii("  RX #");
            print_dec_e1000e(count);
            efi_print_ascii(": ");
            print_dec_e1000e((UINTN)frame_len);
            efi_print_ascii(" bytes");

            /* Parse ethertype from Ethernet header (bytes 12-13, big-endian) */
            if (frame_len >= 14) {
                uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];
                efi_print_ascii(", ethertype=0x");
                print_hex16(ethertype);
                if (ethertype == 0x0800)      efi_print_ascii(" (IPv4)");
                else if (ethertype == 0x0806) efi_print_ascii(" (ARP)");
                else if (ethertype == 0x86DD) efi_print_ascii(" (IPv6)");

                /* Print source MAC (bytes 6-11) */
                efi_print_ascii(", src=");
                print_mac(&frame[6]);
            }
            efi_print_ascii("\r\n");
        }

        /* Check for keypress (non-blocking) */
        EFI_INPUT_KEY key;
        if (efi_read_key(&key) == EFI_SUCCESS)
            break;

        gBS->Stall(1000);  /* 1ms between polls */
    }

    efi_print_ascii("  e1000e: stopped. ");
    print_dec_e1000e(count);
    efi_print_ascii(" frames received.\r\n");
}

/* ---- PCI Probe ---- */

EFI_STATUS e1000e_probe(void) {
    EFI_STATUS status;
    UINTN handle_count = 0;
    EFI_HANDLE *handles = 0;

    /* Find all PCI IO protocol instances */
    status = gBS->LocateHandleBuffer(
        2,  /* ByProtocol */
        &gEfiPciIoProtocolGuid,
        0,
        &handle_count,
        &handles);

    if (status != EFI_SUCCESS || handle_count == 0) {
        efi_print_ascii("  e1000e: no PCI devices found\r\n");
        return status;
    }

    /* Walk each device, match against e1000e device ID table */
    const struct e1000e_ident *match = 0;
    EFI_PCI_IO_PROTOCOL *matched_pci_io = 0;
    uint16_t matched_device_id = 0;
    UINTN matched_bus = 0, matched_dev = 0, matched_func = 0;

    for (UINTN i = 0; i < handle_count; i++) {
        EFI_PCI_IO_PROTOCOL *pci_io = 0;
        status = gBS->HandleProtocol(
            handles[i],
            &gEfiPciIoProtocolGuid,
            (void **)&pci_io);
        if (status != EFI_SUCCESS) continue;

        uint16_t vendor_id = 0, device_id = 0;
        pci_io->Pci.Read(pci_io, EfiPciIoWidthUint16, 0x00, 1, &vendor_id);
        pci_io->Pci.Read(pci_io, EfiPciIoWidthUint16, 0x02, 1, &device_id);

        /* Check against device ID table */
        if (vendor_id != E1000E_VENDOR_INTEL) continue;

        for (const struct e1000e_ident *id = e1000e_ident_table; id->device != 0; id++) {
            if (device_id == id->device) {
                match = id;
                matched_pci_io = pci_io;
                matched_device_id = device_id;
                pci_io->GetLocation(pci_io, &(UINTN){0},
                                    &matched_bus, &matched_dev, &matched_func);
                break;
            }
        }
        if (match) break;
    }

    gBS->FreePool(handles);

    if (!match) {
        efi_print_ascii("  e1000e: no Intel Ethernet found\r\n");
        return EFI_NOT_FOUND;
    }

    /* Print matched card info */
    efi_print_ascii("  e1000e: ");
    efi_print_ascii(match->name);
    efi_print_ascii(" [0x");
    print_hex16(matched_device_id);
    efi_print_ascii("] at ");
    print_dec_e1000e(matched_bus);
    efi_print_ascii(":");
    print_dec_e1000e(matched_dev);
    efi_print_ascii(".");
    print_dec_e1000e(matched_func);
    efi_print_ascii("\r\n");

    /* Enable PCI MMIO + Bus Master before any BAR0 access */
    #define EFI_PCI_IO_ATTRIBUTE_MEMORY      0x0200
    #define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER  0x0400
    uint64_t pci_attrs = EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    status = matched_pci_io->Attributes(matched_pci_io, 2 /* Enable */, pci_attrs, 0);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  e1000e: failed to enable PCI attributes\r\n");
        return status;
    }

    /* Save device state */
    g_e1000e.pci_io = matched_pci_io;
    g_e1000e.device_id = matched_device_id;
    g_e1000e.device_name = match->name;
    g_e1000e.link_up = 0;
    g_e1000e_found = 1;

    /* Phase 1: reset + read MAC */
    status = e1000e_init();
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  e1000e: init failed, probe continues\r\n");
    }

    efi_print_ascii("  e1000e: PCI probe OK\r\n");
    return EFI_SUCCESS;
}

/* ---- Phase 1: HW init (reset + MAC read) ---- */

EFI_STATUS e1000e_init(void) {
    /* Disable all interrupts */
    e1000e_write(E1000_IMC, 0xFFFFFFFF);

    /* PBA/PBS errata for ICH9-class (82577LM etc.) */
    e1000e_write(E1000_PBA, 0x000A);
    e1000e_write(E1000_PBS, 0x0010);

    /* Global reset: set RST bit in CTRL */
    uint32_t ctrl = e1000e_read(E1000_CTRL);
    e1000e_write(E1000_CTRL, ctrl | E1000_CTRL_RST);

    /* Wait 10ms for reset to complete */
    gBS->Stall(10000);

    /* Disable interrupts again (reset clears IMC) */
    e1000e_write(E1000_IMC, 0xFFFFFFFF);

    /* Read MAC address from RAL0/RAH0 (hardware auto-loads from EEPROM on reset) */
    uint32_t ral = e1000e_read(E1000_RAL0);
    uint32_t rah = e1000e_read(E1000_RAH0);

    if (rah & E1000_RAH_AV) {
        /* MAC valid in RAL0/RAH0 */
        g_e1000e.mac[0] = (uint8_t)(ral >>  0);
        g_e1000e.mac[1] = (uint8_t)(ral >>  8);
        g_e1000e.mac[2] = (uint8_t)(ral >> 16);
        g_e1000e.mac[3] = (uint8_t)(ral >> 24);
        g_e1000e.mac[4] = (uint8_t)(rah >>  0);
        g_e1000e.mac[5] = (uint8_t)(rah >>  8);
    } else {
        /* Fallback: read MAC from EEPROM words 0, 1, 2 */
        uint16_t w0, w1, w2;
        if (e1000e_read_eeprom(0, &w0) || e1000e_read_eeprom(1, &w1) || e1000e_read_eeprom(2, &w2)) {
            efi_print_ascii("  e1000e: EEPROM read failed\r\n");
            return EFI_DEVICE_ERROR;
        }
        g_e1000e.mac[0] = (uint8_t)(w0 >> 0);
        g_e1000e.mac[1] = (uint8_t)(w0 >> 8);
        g_e1000e.mac[2] = (uint8_t)(w1 >> 0);
        g_e1000e.mac[3] = (uint8_t)(w1 >> 8);
        g_e1000e.mac[4] = (uint8_t)(w2 >> 0);
        g_e1000e.mac[5] = (uint8_t)(w2 >> 8);
    }

    /* Print MAC */
    efi_print_ascii("  e1000e: MAC ");
    print_mac(g_e1000e.mac);
    efi_print_ascii("\r\n");

    /* Phase 2: bring up link + RX ring */
    e1000e_link_up();
    e1000e_rx_init();

    /* Phase 3: TX ring */
    e1000e_tx_init();

    efi_print_ascii("  e1000e: reset OK\r\n");
    return EFI_SUCCESS;
}

/* ---- Phase 1+ stubs ---- */

int e1000e_is_up(void) {
    return g_e1000e.link_up;
}

void e1000e_get_mac(uint8_t *mac) {
    for (int i = 0; i < 6; i++)
        mac[i] = g_e1000e.mac[i];
}

/* ---- Phase 4: SNP-compatible wrappers for ns_* network stack ---- */

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
