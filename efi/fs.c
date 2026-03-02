/*
 * fs.c - File operations via UEFI Simple File System Protocol
 *
 * Provides file I/O on the EFI System Partition using
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. Used by model loader and file tools.
 */

#include "efi.h"

static EFI_FILE_PROTOCOL *fs_root = 0;

EFI_FILE_PROTOCOL *efi_fs_root(void) { return fs_root; }

static EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_GUID;

/* Convert ASCII path to UCS-2, replacing '/' with '\\' for UEFI */
static void ascii_to_ucs2(const char *src, CHAR16 *dst, int max_chars) {
    int i = 0;
    while (src[i] && i < max_chars - 1) {
        dst[i] = (src[i] == '/') ? (CHAR16)'\\' : (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

EFI_STATUS efi_fs_init(void) {
    if (fs_root) return EFI_SUCCESS;

    /* Enumerate ALL SimpleFileSystem handles and probe each.
     * Dell E6510 has multiple volumes (HP Port Rep 126MB FAT16 +
     * BOOTAI USB/HD). LocateProtocol finds the wrong one. */
    UINTN n_fs = 0;
    EFI_HANDLE *fs_handles = 0;
    EFI_STATUS status = gBS->LocateHandleBuffer(
        2 /* ByProtocol */, &gEfiSimpleFileSystemProtocolGuid, 0,
        &n_fs, &fs_handles);
    if (status != EFI_SUCCESS || n_fs == 0)
        return EFI_NOT_FOUND;

    for (UINTN i = 0; i < n_fs; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        status = gBS->HandleProtocol(fs_handles[i],
            &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
        if (status != EFI_SUCCESS || !fs) continue;

        EFI_FILE_PROTOCOL *vol = 0;
        status = fs->OpenVolume(fs, &vol);
        if (status != EFI_SUCCESS || !vol) continue;

        /* Probe: try opening our EFI binary */
        CHAR16 probe_path[32];
        ascii_to_ucs2("\\EFI\\BOOT\\BOOTX64.EFI", probe_path, 32);
        EFI_FILE_PROTOCOL *probe = 0;
        status = vol->Open(vol, &probe, probe_path, EFI_FILE_MODE_READ, 0);
        if (status == EFI_SUCCESS && probe) {
            probe->Close(probe);
            fs_root = vol;
            gBS->FreePool(fs_handles);
            return EFI_SUCCESS;
        }
        vol->Close(vol);
    }

    gBS->FreePool(fs_handles);
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_file_size(const char *path, UINTN *size_out) {
    if (!fs_root || !path || !size_out) return EFI_NOT_FOUND;

    CHAR16 wpath[256];
    ascii_to_ucs2(path, wpath, 256);

    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = fs_root->Open(fs_root, &file, wpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) return status;

    /* Query file info to get size */
    uint8_t info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, info_buf);
    if (status != EFI_SUCCESS) { file->Close(file); return status; }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    *size_out = (UINTN)info->FileSize;

    file->Close(file);
    return EFI_SUCCESS;
}

/* Print a decimal number to serial */
static void print_dec(uint64_t n) {
    char buf[24]; int i = 0;
    if (n == 0) { efi_print_ascii("0"); return; }
    char tmp[24]; int ti = 0;
    while (n > 0) { tmp[ti++] = '0' + (int)(n % 10); n /= 10; }
    while (ti > 0) buf[i++] = tmp[--ti];
    buf[i] = '\0';
    efi_print_ascii(buf);
}

static void print_hex_status(EFI_STATUS s) {
    char buf[20] = "0x";
    const char *hex = "0123456789ABCDEF";
    uint64_t v = (uint64_t)s;
    for (int i = 15; i >= 0; i--)
        buf[2 + (15 - i)] = hex[(v >> (i * 4)) & 0xF];
    buf[18] = '\0';
    efi_print_ascii(buf);
}

EFI_STATUS efi_read_file(const char *path, void **buffer, UINTN *size) {
    if (!fs_root || !path || !buffer || !size) return EFI_NOT_FOUND;

    CHAR16 wpath[256];
    ascii_to_ucs2(path, wpath, 256);

    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = fs_root->Open(fs_root, &file, wpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  fs: Open failed: "); print_hex_status(status); efi_print_ascii("\r\n");
        return status;
    }

    /* Query file size */
    uint8_t info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = file->GetInfo(file, &gEfiFileInfoGuid, &info_size, info_buf);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("  fs: GetInfo failed: "); print_hex_status(status); efi_print_ascii("\r\n");
        file->Close(file);
        return status;
    }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN file_size = (UINTN)info->FileSize;
    efi_print_ascii("  fs: file size = "); print_dec(file_size); efi_print_ascii(" bytes\r\n");

    /* Allocate buffer via AllocatePages (not AllocatePool).
     * AllocatePool fails for GB-scale files; AllocatePages works at any size. */
    void *buf = 0;
    UINTN pages = (file_size + 4095) / 4096;
    uint64_t phys_addr = 0;
    efi_print_ascii("  fs: allocating "); print_dec(pages); efi_print_ascii(" pages...\r\n");
    status = gBS->AllocatePages(0 /* AllocateAnyPages */, EfiLoaderData, pages, &phys_addr);
    if (status == EFI_SUCCESS) {
        buf = (void *)(UINTN)phys_addr;
        efi_print_ascii("  fs: allocated at 0x"); print_hex_status(phys_addr); efi_print_ascii("\r\n");
    } else {
        efi_print_ascii("  fs: AllocatePages failed: "); print_hex_status(status); efi_print_ascii("\r\n");
        file->Close(file);
        return status;
    }

    /* Read file in chunks (some UEFI implementations limit single reads) */
    uint8_t *dst = (uint8_t *)buf;
    UINTN remaining = file_size;
    UINTN total_read = 0;
    UINTN chunk = 64 * 1024 * 1024; /* 64 MB chunks */

    efi_print_ascii("  fs: reading");
    while (remaining > 0) {
        UINTN to_read = remaining < chunk ? remaining : chunk;
        status = file->Read(file, &to_read, dst);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("\r\n  fs: Read failed at offset ");
            print_dec(total_read); efi_print_ascii(": ");
            print_hex_status(status); efi_print_ascii("\r\n");
            gBS->FreePages((uint64_t)(UINTN)buf, pages);
            file->Close(file);
            return status;
        }
        if (to_read == 0) break;
        dst += to_read;
        total_read += to_read;
        remaining -= to_read;
        efi_print_ascii(".");
    }
    efi_print_ascii(" done ("); print_dec(total_read); efi_print_ascii(" bytes)\r\n");

    *buffer = buf;
    *size = total_read;
    file->Close(file);
    return EFI_SUCCESS;
}

EFI_STATUS efi_write_file(const char *path, const void *data, UINTN size) {
    if (!fs_root || !path || !data) return EFI_NOT_FOUND;

    CHAR16 wpath[256];
    ascii_to_ucs2(path, wpath, 256);

    /* Delete existing file first to avoid stale data at end.
     * UEFI Write() doesn't truncate — if the old file was larger,
     * leftover bytes from the previous session persist. */
    {
        EFI_FILE_PROTOCOL *old = 0;
        EFI_STATUS s = fs_root->Open(fs_root, &old, wpath,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (s == EFI_SUCCESS)
            old->Delete(old);  /* deletes file and closes handle */
    }

    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status = fs_root->Open(fs_root, &file, wpath,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (status != EFI_SUCCESS) return status;

    UINTN write_size = size;
    status = file->Write(file, &write_size, (void *)data);
    file->Close(file);
    return status;
}

EFI_STATUS efi_list_dir(const char *path) {
    if (!fs_root) return EFI_NOT_FOUND;

    CHAR16 wpath[256];
    if (!path || path[0] == '\0') {
        wpath[0] = '\\'; wpath[1] = 0;
    } else {
        ascii_to_ucs2(path, wpath, 256);
    }

    EFI_FILE_PROTOCOL *dir = 0;
    EFI_STATUS status = fs_root->Open(fs_root, &dir, wpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) return status;

    uint8_t buf[512];
    for (;;) {
        UINTN buf_size = sizeof(buf);
        status = dir->Read(dir, &buf_size, buf);
        if (status != EFI_SUCCESS || buf_size == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;

        /* Convert filename from UCS-2 to ASCII for display */
        char name[128];
        int i = 0;
        while (info->FileName[i] && i < 127) {
            name[i] = (char)(info->FileName[i] & 0xFF);
            i++;
        }
        name[i] = '\0';

        if (info->Attribute & EFI_FILE_DIRECTORY) {
            efi_print_ascii("  [DIR] ");
        } else {
            efi_print_ascii("  ");
            /* Print file size */
            char size_str[20];
            uint64_t sz = info->FileSize;
            int si = 0;
            if (sz == 0) {
                size_str[si++] = '0';
            } else {
                char tmp[20]; int ti = 0;
                while (sz > 0) { tmp[ti++] = '0' + (int)(sz % 10); sz /= 10; }
                while (ti > 0) size_str[si++] = tmp[--ti];
            }
            size_str[si] = '\0';
            efi_print_ascii(size_str);
            efi_print_ascii(" ");
        }
        efi_print_ascii(name);
        efi_print_ascii("\r\n");
    }

    dir->Close(dir);
    return EFI_SUCCESS;
}
