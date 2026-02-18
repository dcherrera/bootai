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

int efi_read_line(char *buf, int max_len) {
    if (!buf || max_len <= 0) return 0;

    int pos = 0;
    EFI_INPUT_KEY key;

    while (pos < max_len - 1) {
        /* Block until a key is available */
        efi_wait_key();

        EFI_STATUS status = efi_read_key(&key);
        if (status != EFI_SUCCESS) continue;

        /* Ignore scan codes (arrows, function keys, etc.) */
        if (key.UnicodeChar == 0) continue;

        /* Enter — submit the line */
        if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n') {
            efi_print_ascii("\r\n");
            break;
        }

        /* Backspace */
        if (key.UnicodeChar == '\b' || key.UnicodeChar == 127) {
            if (pos > 0) {
                pos--;
                /* Erase character on screen: backspace, space, backspace */
                efi_print_ascii("\b \b");
            }
            continue;
        }

        /* Printable ASCII (ignore non-ASCII for now) */
        if (key.UnicodeChar >= 0x20 && key.UnicodeChar < 0x7F) {
            buf[pos++] = (char)key.UnicodeChar;

            /* Echo the character */
            CHAR16 echo[2] = { key.UnicodeChar, 0 };
            efi_print(echo);
        }
    }

    buf[pos] = '\0';
    return pos;
}
