/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → model loading → inference REPL with tool use.
 * Stays in UEFI boot services mode (never calls ExitBootServices).
 */

#include "efi.h"

/* Globals */
EFI_SYSTEM_TABLE  *gST = NULL;
EFI_BOOT_SERVICES *gBS = NULL;

/* Protocol GUIDs */
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

/* EFI application entry point */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    gST = SystemTable;
    gBS = SystemTable->BootServices;

    /* Clear screen */
    efi_clear_screen();

    /* Initialize memory (wire JACLibc malloc to UEFI) */
    efi_memory_init();

    /* Initialize filesystem */
    efi_fs_init();

    /* Initialize networking */
    efi_net_init();

    /* Boot menu */
    efi_print_ascii("BootAI v0.1\r\n\r\n");
    efi_print_ascii("[1] Run Live\r\n");
    efi_print_ascii("[2] Install to Hard Drive\r\n");
    efi_print_ascii("[3] Settings\r\n");
    efi_print_ascii("\r\nSelect: ");

    /* TODO: boot menu selection + inference REPL */

    /* Hang — never exit boot services */
    for (;;) {
        __asm__ volatile("hlt");
    }

    return EFI_SUCCESS;
}
