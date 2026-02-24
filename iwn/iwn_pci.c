/*
 * iwn_pci.c — PCI bus probe + attach + init for Intel Wireless N driver
 *
 * Phase 0: Walk EFI_PCI_IO_PROTOCOL handles, match against device ID table.
 * Phase 1: Save PCI IO handle, call iwn_attach() for MMIO/EEPROM access.
 * Phase 2: Call iwn_init() for DMA + firmware loading.
 */

#include "iwn_internal.h"

/* PCI IO Protocol GUID (from UEFI spec) */
EFI_GUID gEfiPciIoProtocolGuid = {
    0x4cf5b200, 0x68b8, 0x4ca5,
    {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}
};

/* Global device state — persists across REPL commands */
static iwn_dev g_iwn;
static int g_iwn_found = 0;

/* ---- Freestanding helpers ---- */

static void print_dec_pci(UINTN val) {
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

/* ---- PCI Probe ---- */

EFI_STATUS iwn_probe(void) {
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
        efi_print_ascii("  No PCI devices found\r\n");
        return status;
    }

    /* Walk each device, match against iwn device ID table */
    const struct iwn_ident *match = NULL;
    EFI_PCI_IO_PROTOCOL *matched_pci_io = NULL;
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
        for (const struct iwn_ident *id = iwn_ident_table; id->name != NULL; id++) {
            if (vendor_id == id->vendor && device_id == id->device) {
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
        efi_print_ascii("  No Intel WiFi card detected.\r\n");
        return EFI_NOT_FOUND;
    }

    /* Print matched card info */
    efi_print_ascii("  ");
    efi_print_ascii(match->name);
    efi_print_ascii(" found at ");
    print_dec_pci(matched_bus);
    efi_print_ascii(":");
    print_dec_pci(matched_dev);
    efi_print_ascii(".");
    print_dec_pci(matched_func);
    efi_print_ascii("\r\n");

    /* Enable PCI MMIO + Bus Master before any BAR0 access */
    #define EFI_PCI_IO_ATTRIBUTE_MEMORY      0x0200
    #define EFI_PCI_IO_ATTRIBUTE_BUS_MASTER  0x0400
    uint64_t pci_attrs = EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;
    status = matched_pci_io->Attributes(matched_pci_io, 2 /* Enable */, pci_attrs, NULL);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  iwn: failed to enable PCI attributes\r\n");
        return status;
    }

    /* Save device state and call attach */
    g_iwn.pci_io = matched_pci_io;
    g_iwn.device_id = matched_device_id;
    g_iwn.flags = 0;
    g_iwn.prom_base = 0;

    status = iwn_attach(&g_iwn);
    if (status != EFI_SUCCESS) {
        g_iwn_found = 0;
        return status;
    }

    /* Phase 2: DMA + firmware loading */
    if (iwn_init(&g_iwn) != 0) {
        g_iwn_found = 0;
        efi_print_ascii("  iwn: init failed\r\n");
        return EFI_DEVICE_ERROR;
    }

    g_iwn_found = 1;
    efi_print_ascii("  Intel WiFi ready.\r\n");
    return EFI_SUCCESS;
}

/* ---- Phase 2: DMA + firmware init wrapper ---- */

int iwn_init(iwn_dev *dev) {
    /* Parse firmware file, then run full hardware init */
    return iwn_hw_init(dev);
}
