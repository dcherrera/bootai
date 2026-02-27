/*
 * drivers.c — Generic UEFI driver loader
 *
 * Scans \drivers\ directory at boot, loads and starts all .efi driver files.
 * This allows users to drop WiFi, GPU, or other UEFI drivers onto the USB
 * and have them loaded automatically before network init.
 */

#include "efi.h"

static int g_drivers_loaded = 0;

int efi_drivers_were_loaded(void) { return g_drivers_loaded; }

/* Freestanding helpers */
static void drv_memset(void *dst, int val, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

/* Check if UCS-2 filename ends with ".efi" (case-insensitive) */
static int is_efi_file(CHAR16 *name) {
    int len = 0;
    while (name[len]) len++;
    if (len < 5) return 0; /* minimum: "x.efi" */

    CHAR16 c3 = name[len - 4];
    CHAR16 c2 = name[len - 3];
    CHAR16 c1 = name[len - 2];
    CHAR16 c0 = name[len - 1];

    /* Case-insensitive .efi check */
    if (c3 != '.' ) return 0;
    if (c2 != 'e' && c2 != 'E') return 0;
    if (c1 != 'f' && c1 != 'F') return 0;
    if (c0 != 'i' && c0 != 'I') return 0;
    return 1;
}

/* Convert UCS-2 filename to ASCII for display */
static void ucs2_to_ascii(CHAR16 *src, char *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (char)(src[i] & 0xFF);
        i++;
    }
    dst[i] = '\0';
}

/* Build full path "\\drivers\\<filename>" in UCS-2 */
static void build_driver_path(CHAR16 *filename, CHAR16 *path, int max_chars) {
    CHAR16 prefix[] = { '\\', 'd', 'r', 'i', 'v', 'e', 'r', 's', '\\', 0 };
    int i = 0;

    /* Copy prefix */
    for (int j = 0; prefix[j] && i < max_chars - 1; j++)
        path[i++] = prefix[j];

    /* Copy filename */
    for (int j = 0; filename[j] && i < max_chars - 1; j++)
        path[i++] = filename[j];

    path[i] = 0;
}

void efi_load_drivers(void) {
    EFI_STATUS status;

    /* Ensure filesystem is ready */
    status = efi_fs_init();
    if (status != EFI_SUCCESS) return;

    efi_print_ascii("  Loading drivers from \\drivers\\...\r\n");

    /* Get filesystem root via LocateProtocol */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
    status = gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid, 0, (void **)&fs);
    if (status != EFI_SUCCESS || !fs) return;

    EFI_FILE_PROTOCOL *root = 0;
    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root) return;

    /* Open \drivers\ directory */
    CHAR16 drivers_dir[] = { '\\', 'd', 'r', 'i', 'v', 'e', 'r', 's', 0 };
    EFI_FILE_PROTOCOL *dir = 0;
    status = root->Open(root, &dir, drivers_dir, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        /* \drivers\ doesn't exist — silently continue */
        efi_print_ascii("  No \\drivers\\ directory found.\r\n");
        root->Close(root);
        return;
    }

    /* Iterate directory entries */
    int loaded = 0;
    uint8_t entry_buf[512];

    for (;;) {
        UINTN buf_size = sizeof(entry_buf);
        drv_memset(entry_buf, 0, sizeof(entry_buf));
        status = dir->Read(dir, &buf_size, entry_buf);
        if (status != EFI_SUCCESS || buf_size == 0) break;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)entry_buf;

        /* Skip directories */
        if (info->Attribute & EFI_FILE_DIRECTORY) continue;

        /* Skip non-.efi files */
        if (!is_efi_file(info->FileName)) continue;

        /* Convert filename for display */
        char name_ascii[128];
        ucs2_to_ascii(info->FileName, name_ascii, sizeof(name_ascii));

        /* Build full path "\\drivers\\filename.efi" */
        CHAR16 full_path[256];
        build_driver_path(info->FileName, full_path, 256);

        /* Read the .efi file into memory */
        void *file_buf = 0;
        UINTN file_size = 0;
        status = efi_read_file(name_ascii[0] == '\\' ? name_ascii : name_ascii, &file_buf, &file_size);

        /* We need to read using the full path — use the file protocol directly */
        if (file_buf) {
            gBS->FreePool(file_buf);
            file_buf = 0;
        }

        /* Open and read the driver file directly */
        EFI_FILE_PROTOCOL *drv_file = 0;
        status = root->Open(root, &drv_file, full_path, EFI_FILE_MODE_READ, 0);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("  WARN: cannot open ");
            efi_print_ascii(name_ascii);
            efi_print_ascii("\r\n");
            continue;
        }

        /* Get file size */
        uint8_t finfo_buf[256];
        UINTN finfo_size = sizeof(finfo_buf);
        EFI_GUID file_info_guid = EFI_FILE_INFO_GUID;
        status = drv_file->GetInfo(drv_file, &file_info_guid, &finfo_size, finfo_buf);
        if (status != EFI_SUCCESS) {
            drv_file->Close(drv_file);
            continue;
        }
        EFI_FILE_INFO *finfo = (EFI_FILE_INFO *)finfo_buf;
        file_size = (UINTN)finfo->FileSize;

        /* Allocate buffer */
        status = gBS->AllocatePool(EfiLoaderData, file_size, &file_buf);
        if (status != EFI_SUCCESS) {
            drv_file->Close(drv_file);
            continue;
        }

        /* Read file */
        UINTN read_size = file_size;
        status = drv_file->Read(drv_file, &read_size, file_buf);
        drv_file->Close(drv_file);

        if (status != EFI_SUCCESS || read_size != file_size) {
            gBS->FreePool(file_buf);
            continue;
        }

        /* Load the driver image from memory buffer */
        EFI_HANDLE driver_handle = 0;
        status = gBS->LoadImage(0, gImageHandle, 0, file_buf, file_size, &driver_handle);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("  WARN: LoadImage failed for ");
            efi_print_ascii(name_ascii);
            efi_print_ascii("\r\n");
            gBS->FreePool(file_buf);
            continue;
        }

        /* Start the driver */
        status = gBS->StartImage(driver_handle, 0, 0);
        gBS->FreePool(file_buf);

        if (status == EFI_SUCCESS) {
            efi_print_ascii("  Loaded driver: ");
            efi_print_ascii(name_ascii);
            efi_print_ascii("\r\n");
            loaded++;
        } else {
            efi_print_ascii("  WARN: StartImage failed for ");
            efi_print_ascii(name_ascii);
            efi_print_ascii("\r\n");
        }
    }

    dir->Close(dir);
    root->Close(root);

    if (loaded == 0) {
        efi_print_ascii("  No drivers found.\r\n");
    } else {
        g_drivers_loaded = 1;
    }
    /* NOTE: We do NOT call connect_all_controllers() here.
     * Loaded DXE drivers (MnpDxe etc.) hang on Dell E6510 firmware
     * during ConnectController. The custom network stack (ns_*)
     * works directly on SNP without needing DXE driver binding. */
}
