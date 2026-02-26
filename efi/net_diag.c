/*
 * net_diag.c — Network protocol enumeration diagnostic
 *
 * Enumerates UEFI network protocol handles and prints counts.
 * Helps diagnose whether the Dell E6510 firmware has IP stack
 * DXE drivers (MNP, ARP, IP4, TCP4, UDP4) after IWN SNP install.
 */

#include "efi.h"

/* ---- New diagnostic GUIDs (not already in net.c) ---- */

EFI_GUID gEfiMnpServiceBindingGuid = {
    0xF36FF770, 0xA7E1, 0x42CF,
    {0x9E, 0xD2, 0x56, 0xF0, 0xF2, 0x71, 0xF4, 0x4E}
};

EFI_GUID gEfiArpServiceBindingGuid = {
    0xF44C00EE, 0x1F2C, 0x4A00,
    {0xAA, 0x09, 0x1C, 0x9F, 0x7E, 0x44, 0xE0, 0x38}
};

EFI_GUID gEfiIp4ServiceBindingGuid = {
    0xC51711E7, 0xB4BF, 0x404A,
    {0xBF, 0xB8, 0x0A, 0x04, 0x8E, 0xF1, 0xFF, 0xE4}
};

EFI_GUID gEfiDhcp4ServiceBindingGuid = {
    0x9D9A39D8, 0xBD42, 0x4A73,
    {0xA4, 0xD5, 0x8E, 0xE9, 0x4B, 0xE1, 0x13, 0x80}
};

/* ---- Integer-to-ASCII helper ---- */

static void diag_print_int(int val) {
    char buf[12];
    int i = 0;
    if (val == 0) { buf[i++] = '0'; }
    else {
        char tmp[12]; int ti = 0;
        while (val > 0) { tmp[ti++] = '0' + val % 10; val /= 10; }
        while (ti > 0) buf[i++] = tmp[--ti];
    }
    buf[i] = '\0';
    efi_print_ascii(buf);
}

/* ---- Check one protocol ---- */

static void diag_check(const char *name, EFI_GUID *guid) {
    UINTN count = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS s = gBS->LocateHandleBuffer(2/*SEARCH_BY_PROTOCOL*/, guid, 0, &count, &handles);

    efi_print_ascii("  ");
    efi_print_ascii(name);
    if (s == EFI_SUCCESS) {
        efi_print_ascii(": ");
        diag_print_int((int)count);
        efi_print_ascii("\r\n");
    } else {
        efi_print_ascii(": none\r\n");
    }
    if (handles) gBS->FreePool(handles);
}

/* ---- Public API ---- */

void efi_net_diag(void) {
    efi_print_ascii("  Network protocol handles:\r\n");

    diag_check("SNP            ", &gEfiSimpleNetworkProtocolGuid);
    diag_check("MNP SB         ", &gEfiMnpServiceBindingGuid);
    diag_check("ARP SB         ", &gEfiArpServiceBindingGuid);
    diag_check("IP4 SB         ", &gEfiIp4ServiceBindingGuid);
    diag_check("IP4Config2     ", &gEfiIp4Config2ProtocolGuid);
    diag_check("DHCP4 SB       ", &gEfiDhcp4ServiceBindingGuid);
    diag_check("TCP4 SB        ", &gEfiTcp4ServiceBindingGuid);
    diag_check("UDP4 SB        ", &gEfiUdp4ServiceBindingGuid);

    /* Print IWN SNP status */
    efi_print_ascii("  IWN SNP installed: ");
    efi_print_ascii(iwn_snp_is_installed() ? "yes" : "no");
    efi_print_ascii("\r\n");

    /* Print current IP if available */
    EFI_IPv4_ADDRESS ip;
    if (efi_get_local_ip(&ip) == EFI_SUCCESS) {
        efi_print_ascii("  Local IP: ");
        char buf[20];
        int pos = 0;
        for (int i = 0; i < 4; i++) {
            int v = ip.Addr[i];
            if (v >= 100) buf[pos++] = '0' + v / 100;
            if (v >= 10)  buf[pos++] = '0' + (v / 10) % 10;
            buf[pos++] = '0' + v % 10;
            if (i < 3) buf[pos++] = '.';
        }
        buf[pos] = '\0';
        efi_print_ascii(buf);
        efi_print_ascii("\r\n");
    } else {
        efi_print_ascii("  Local IP: not assigned\r\n");
    }
}
