/*
 * e1000e_hw.c — PCI bus probe for Intel e1000e Ethernet controller
 *
 * Phase 0: Walk EFI_PCI_IO_PROTOCOL handles, match against device ID table.
 * Phase 1+: HW init, MAC read, link detect, TX/RX rings (not yet).
 */

#include "e1000e.h"
#include "e1000e_regs.h"

/* PCI IO Protocol GUID — defined in iwn_pci.c, declared extern in efi.h */
/* Do NOT redefine here; the linker resolves it from iwn_pci.o */

/* Device state — persists across REPL commands */
typedef struct {
    EFI_PCI_IO_PROTOCOL *pci_io;
    uint16_t device_id;
    const char *device_name;
    uint8_t mac[6];     /* filled in Phase 1 */
    int link_up;        /* filled in Phase 1+ */
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
