/*
 * fs.c - File operations via UEFI Simple File System Protocol
 *
 * Provides file I/O on the EFI System Partition using
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. Used by model loader and file tools.
 * Functions: efi_fs_init(), efi_read_file(), efi_write_file(), efi_list_dir()
 */

#include "efi.h"

EFI_STATUS efi_fs_init(void) {
    /* TODO: Locate EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and open root volume */
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_read_file(const char *path, void **buffer, UINTN *size) {
    /* TODO: Open file, query size, allocate buffer, read contents, close */
    (void)path; (void)buffer; (void)size;
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_write_file(const char *path, const void *data, UINTN size) {
    /* TODO: Open/create file, write data, close */
    (void)path; (void)data; (void)size;
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_list_dir(const char *path) {
    /* TODO: Open directory, enumerate entries, print names via efi_print */
    (void)path;
    return EFI_NOT_FOUND;
}
