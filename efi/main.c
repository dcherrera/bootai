/*
 * main.c — BootAI UEFI entry point
 *
 * Boot menu → interactive command REPL with networking.
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

/* Simple string comparison helpers (freestanding — no libc) */
static int str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

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

#define HTTP_RESPONSE_MAX 8192
#define DISPLAY_MAX       2048

static void command_repl(void) {
    char line[512];
    char response[HTTP_RESPONSE_MAX];

    efi_print_ascii("\r\n");
    efi_print_ascii("  Command REPL — commands: fetch <url>, netinfo, exit\r\n");
    efi_print_ascii("\r\n");

    for (;;) {
        efi_print_ascii("> ");

        int len = efi_read_line(line, sizeof(line));

        if (len <= 0) continue;

        /* exit */
        if (str_eq(line, "exit")) {
            break;
        }

        /* netinfo */
        if (str_eq(line, "netinfo")) {
            if (efi_net_available()) {
                efi_print_ascii("  Network: ready\r\n");
            } else {
                efi_print_ascii("  Network: unavailable\r\n");
            }
            continue;
        }

        /* fetch <url> */
        if (str_starts_with(line, "fetch ")) {
            const char *url = line + 6;
            /* Skip leading spaces */
            while (*url == ' ') url++;

            if (*url == '\0') {
                efi_print_ascii("  Usage: fetch <url>\r\n");
                continue;
            }

            if (!efi_net_available()) {
                efi_print_ascii("  Network not available\r\n");
                continue;
            }

            efi_print_ascii("  Fetching: ");
            efi_print_ascii(url);
            efi_print_ascii("\r\n");

            UINTN out_size = 0;
            EFI_STATUS status = efi_http_get(url, response, sizeof(response), &out_size);

            if (status == EFI_SUCCESS && out_size > 0) {
                efi_print_ascii("\r\n--- Response (");
                /* Print size */
                char size_str[12];
                int si = 0;
                UINTN sv = out_size;
                if (sv == 0) {
                    size_str[si++] = '0';
                } else {
                    char tmp[12];
                    int ti = 0;
                    while (sv > 0) {
                        tmp[ti++] = '0' + (int)(sv % 10);
                        sv /= 10;
                    }
                    while (ti > 0) size_str[si++] = tmp[--ti];
                }
                size_str[si] = '\0';
                efi_print_ascii(size_str);
                efi_print_ascii(" bytes) ---\r\n");

                /* Truncate for display */
                if (out_size > DISPLAY_MAX) {
                    response[DISPLAY_MAX] = '\0';
                    out_size = DISPLAY_MAX;
                }

                /* Print response, converting \n to \r\n */
                for (UINTN i = 0; i < out_size; i++) {
                    if (response[i] == '\n') {
                        efi_print_ascii("\r\n");
                    } else if (response[i] == '\r') {
                        /* skip — we handle \n above */
                    } else if (response[i] >= 0x20 && response[i] < 0x7F) {
                        CHAR16 ch[2] = { (CHAR16)(unsigned char)response[i], 0 };
                        efi_print(ch);
                    }
                }

                efi_print_ascii("\r\n--- End ---\r\n");

                if (out_size == DISPLAY_MAX) {
                    efi_print_ascii("  (truncated to 2KB for display)\r\n");
                }
            } else {
                efi_print_ascii("  Fetch failed or empty response\r\n");
            }
            continue;
        }

        /* Default: echo back */
        efi_print_ascii("  ");
        efi_print_line(line);
    }
}

/* EFI application entry point — must use MS x64 calling convention */
EFI_STATUS __attribute__((ms_abi)) efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;

    gST = SystemTable;
    gBS = SystemTable->BootServices;

    /* Clear screen */
    efi_clear_screen();

    /* Initialize memory */
    efi_memory_init();

    /* Initialize networking */
    efi_print_ascii("  Initializing network...\r\n");
    EFI_STATUS net_status = efi_net_init();
    if (net_status == EFI_SUCCESS && efi_net_available()) {
        efi_print_ascii("  Network: ready\r\n");
    } else {
        efi_print_ascii("  Network: unavailable\r\n");
    }

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
            /* Run Live — command REPL with networking */
            command_repl();
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
