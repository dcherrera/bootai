/*
 * console.c - Framebuffer text output via UEFI Graphics Output Protocol (GOP)
 *
 * Provides text output using gST->ConOut for console display.
 * Functions: efi_print(), efi_print_ascii(), efi_clear_screen()
 */

#include "efi.h"

void efi_print(const CHAR16 *str) {
    /* TODO: Call gST->ConOut->OutputString() to display UCS-2 string */
    (void)str;
}

void efi_print_ascii(const char *str) {
    /* TODO: Convert ASCII to CHAR16 on stack, then call efi_print() */
    (void)str;
}

void efi_clear_screen(void) {
    /* TODO: Call gST->ConOut->ClearScreen() */
}
