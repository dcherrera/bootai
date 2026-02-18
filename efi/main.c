/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → interactive echo REPL.
 * Stays in UEFI boot services mode (never calls ExitBootServices).
 */

#include "efi.h"

/* Globals */
EFI_SYSTEM_TABLE  *gST = 0;
EFI_BOOT_SERVICES *gBS = 0;

/* Protocol GUIDs */
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

EFI_GUID gEfiGraphicsOutputProtocolGuid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}
};

static void show_boot_menu(void) {
    efi_print_ascii("\r\n");
    efi_print_ascii("  BootAI v0.1\r\n");
    efi_print_ascii("  ==================\r\n");
    efi_print_ascii("\r\n");
    efi_print_ascii("  [1] Run Live\r\n");
    efi_print_ascii("  [2] Install to Hard Drive\r\n");
    efi_print_ascii("  [3] Settings\r\n");
    efi_print_ascii("\r\n");
    efi_print_ascii("  Select: ");
}

static void echo_repl(void) {
    char line[256];

    efi_print_ascii("\r\n");
    efi_print_ascii("  Echo REPL — type text and press Enter (type 'exit' to return to menu)\r\n");
    efi_print_ascii("\r\n");

    for (;;) {
        efi_print_ascii("> ");

        int len = efi_read_line(line, sizeof(line));

        if (len <= 0) continue;

        /* Check for exit command */
        if (len == 4 && line[0] == 'e' && line[1] == 'x' &&
            line[2] == 'i' && line[3] == 't') {
            break;
        }

        /* Echo back */
        efi_print_ascii("  You said: ");
        efi_print_line(line);
    }
}

/* EFI application entry point */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    gST = SystemTable;
    gBS = SystemTable->BootServices;

    /* Clear screen */
    efi_clear_screen();

    /* Initialize memory */
    efi_memory_init();

    /* Main loop: boot menu → action → back to menu */
    for (;;) {
        show_boot_menu();

        /* Wait for a menu selection key */
        EFI_INPUT_KEY key;
        for (;;) {
            efi_wait_key();
            EFI_STATUS status = efi_read_key(&key);
            if (status == EFI_SUCCESS && key.UnicodeChar >= '1' && key.UnicodeChar <= '3') {
                /* Echo the selection */
                CHAR16 echo[2] = { key.UnicodeChar, 0 };
                efi_print(echo);
                efi_print_ascii("\r\n");
                break;
            }
        }

        switch (key.UnicodeChar) {
        case '1':
            /* Run Live — drop into echo REPL for now */
            echo_repl();
            break;
        case '2':
            efi_print_ascii("\r\n  Install not yet implemented.\r\n");
            efi_print_ascii("  Press any key to return to menu...\r\n");
            efi_wait_key();
            /* Drain the key */
            efi_read_key(&key);
            break;
        case '3':
            efi_print_ascii("\r\n  Settings not yet implemented.\r\n");
            efi_print_ascii("  Press any key to return to menu...\r\n");
            efi_wait_key();
            efi_read_key(&key);
            break;
        }

        efi_clear_screen();
    }

    return EFI_SUCCESS;
}
