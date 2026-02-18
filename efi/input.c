/*
 * input.c - Keyboard input via UEFI Simple Text Input Protocol
 *
 * Provides keystroke reading via gST->ConIn for interactive input.
 * Functions: efi_read_key(), efi_read_line()
 */

#include "efi.h"

EFI_STATUS efi_read_key(EFI_INPUT_KEY *key) {
    /* TODO: Call gST->ConIn->ReadKeyStroke() to get a single keystroke */
    (void)key;
    return EFI_NOT_FOUND;
}

int efi_read_line(char *buf, int max_len) {
    /* TODO: Loop reading keys until Enter, handle Backspace, echo chars */
    (void)buf;
    (void)max_len;
    return 0;
}
