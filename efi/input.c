/*
 * input.c — Keyboard input via UEFI Simple Text Input Protocol
 *
 * Uses gST->ConIn->ReadKeyStroke() for keystroke reading.
 * Uses gBS->WaitForEvent() on ConIn->WaitForKey to block until a key is pressed.
 * Functions: efi_read_key(), efi_wait_key(), efi_read_line()
 */

#include "efi.h"

EFI_STATUS efi_read_key(EFI_INPUT_KEY *key) {
    if (!gST || !gST->ConIn || !key) {
        return EFI_NOT_READY;
    }

    return gST->ConIn->ReadKeyStroke(gST->ConIn, key);
}

void efi_wait_key(void) {
    if (!gBS || !gST || !gST->ConIn) return;

    UINTN index;
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &index);
}

/* UEFI scan codes for special keys */
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_DELETE 0x08

/* Redraw line from cursor position to end, then reposition cursor */
static void redraw_from(const char *buf, int len, int cursor) {
    /* Write chars from cursor to end */
    for (int i = cursor; i < len; i++) {
        CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[i], 0 };
        efi_print(ch);
    }
    /* Overwrite any leftover character (after deletion) */
    efi_print_ascii(" ");
    /* Move cursor back to correct position */
    for (int i = 0; i < len - cursor + 1; i++)
        efi_print_ascii("\b");
}

int efi_read_line(char *buf, int max_len) {
    if (!buf || max_len <= 0) return 0;

    int len = 0;     /* total characters in buffer */
    int cursor = 0;  /* cursor position within buffer */
    EFI_INPUT_KEY key;

    while (len < max_len - 1) {
        efi_wait_key();

        EFI_STATUS status = efi_read_key(&key);
        if (status != EFI_SUCCESS) continue;

        /* Handle scan codes (arrow keys, delete, home, end) */
        if (key.UnicodeChar == 0 && key.ScanCode != 0) {
            switch (key.ScanCode) {
            case SCAN_LEFT:
                if (cursor > 0) {
                    cursor--;
                    efi_print_ascii("\b");
                }
                break;
            case SCAN_RIGHT:
                if (cursor < len) {
                    CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[cursor], 0 };
                    efi_print(ch);
                    cursor++;
                }
                break;
            case SCAN_HOME:
                while (cursor > 0) { cursor--; efi_print_ascii("\b"); }
                break;
            case SCAN_END:
                while (cursor < len) {
                    CHAR16 ch[2] = { (CHAR16)(unsigned char)buf[cursor], 0 };
                    efi_print(ch);
                    cursor++;
                }
                break;
            case SCAN_DELETE:
                /* macOS Terminal sends 0x7F for backspace, which OVMF
                 * maps to SCAN_DELETE. Treat as backspace. */
                if (cursor > 0) {
                    cursor--;
                    for (int i = cursor; i < len - 1; i++)
                        buf[i] = buf[i + 1];
                    len--;
                    efi_print_ascii("\b");
                    redraw_from(buf, len, cursor);
                }
                break;
            }
            continue;
        }

        /* Enter — submit the line */
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            efi_print_ascii("\r\n");
            break;
        }

        /* Backspace */
        if (key.UnicodeChar == '\b' || key.UnicodeChar == 127) {
            if (cursor > 0) {
                cursor--;
                for (int i = cursor; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                efi_print_ascii("\b");
                redraw_from(buf, len, cursor);
            }
            continue;
        }

        /* Printable ASCII — insert at cursor position */
        if (key.UnicodeChar >= 0x20 && key.UnicodeChar < 0x7F) {
            if (len >= max_len - 1) continue;

            /* Shift chars right to make room */
            for (int i = len; i > cursor; i--)
                buf[i] = buf[i - 1];
            buf[cursor] = (char)key.UnicodeChar;
            len++;
            cursor++;

            /* Redraw from previous cursor position */
            redraw_from(buf, len, cursor - 1);
            /* Advance cursor on screen past the inserted char */
            CHAR16 echo[2] = { key.UnicodeChar, 0 };
            efi_print(echo);
        }
    }

    buf[len] = '\0';
    return len;
}
