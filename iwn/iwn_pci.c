/*
 * iwn_pci.c — PCI bus enumeration for Intel Wireless N driver
 *
 * Phase 0: Walk EFI_PCI_IO_PROTOCOL handles, read vendor/device IDs,
 * and match the Intel Centrino Advanced-N 6200 WiFi card.
 */

#include "iwn.h"
#include "if_iwnvar.h"

/* PCI IO Protocol GUID (from UEFI spec) */
EFI_GUID gEfiPciIoProtocolGuid = {
    0x4cf5b200, 0x68b8, 0x4ca5,
    {0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a}
};

/* ---- Freestanding helpers ---- */

static const char hex[] = "0123456789abcdef";

static void print_hex16(uint16_t val) {
    char buf[5];
    buf[0] = hex[(val >> 12) & 0xf];
    buf[1] = hex[(val >>  8) & 0xf];
    buf[2] = hex[(val >>  4) & 0xf];
    buf[3] = hex[(val >>  0) & 0xf];
    buf[4] = '\0';
    efi_print_ascii(buf);
}

static void print_dec(UINTN val) {
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

    efi_print_ascii("  Scanning ");
    print_dec(handle_count);
    efi_print_ascii(" PCI devices...\r\n");

    /* Walk each device, read vendor/device ID from config space */
    int found = 0;
    for (UINTN i = 0; i < handle_count; i++) {
        EFI_PCI_IO_PROTOCOL *pci_io = 0;
        status = gBS->HandleProtocol(
            handles[i],
            &gEfiPciIoProtocolGuid,
            (void **)&pci_io);
        if (status != EFI_SUCCESS) continue;

        /* PCI config space offset 0x00 = vendor ID (16-bit) */
        /* PCI config space offset 0x02 = device ID (16-bit) */
        uint16_t vendor_id = 0, device_id = 0;
        pci_io->Pci.Read(pci_io, EfiPciIoWidthUint16, 0x00, 1, &vendor_id);
        pci_io->Pci.Read(pci_io, EfiPciIoWidthUint16, 0x02, 1, &device_id);

        /* Print every device for diagnostic purposes */
        efi_print_ascii("    ");
        print_hex16(vendor_id);
        efi_print_ascii(":");
        print_hex16(device_id);

        /* Get BDF (bus:device.function) location */
        UINTN seg = 0, bus = 0, dev = 0, func = 0;
        pci_io->GetLocation(pci_io, &seg, &bus, &dev, &func);

        efi_print_ascii(" @ ");
        print_dec(bus);
        efi_print_ascii(":");
        print_dec(dev);
        efi_print_ascii(".");
        print_dec(func);

        /* Check for Intel 6200 match */
        if (vendor_id == PCI_VENDOR_INTEL &&
            (device_id == IWN_DID_6200_1 || device_id == IWN_DID_6200_2)) {
            efi_print_ascii("  <-- Intel 6200 WiFi!");
            found = 1;
            /* TODO Phase 1: save pci_io handle for MMIO access */
        }

        efi_print_ascii("\r\n");
    }

    gBS->FreePool(handles);

    if (found) {
        efi_print_ascii("  Intel 6200 WiFi detected.\r\n");
    } else {
        efi_print_ascii("  No Intel 6200 WiFi card detected.\r\n");
    }

    return found ? EFI_SUCCESS : EFI_NOT_FOUND;
}
