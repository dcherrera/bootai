/*
 * install.c - Install BootAI to internal hard drive
 *
 * Wipes the internal HD, creates a GPT partition table with:
 *   Partition 1: 100MB FAT32 ESP (EFI binary, vocab, wifi.conf)
 *   Partition 2: Raw data partition (model weights, no size limit)
 *
 * The raw data partition uses a simple table of contents (TOC) header
 * at the start of the partition, followed by contiguous data blobs.
 * No filesystem overhead, no file size limits.
 */

#include "efi.h"

/* Freestanding helpers */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);

/* Printf (provided by uefi_libc.c) */
int uefi_printf(const char *fmt, ...);

/* ================================================================== */
/* CRC32 — required for GPT header checksums                         */
/* ================================================================== */

static uint32_t crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32(const void *data, int len) {
    if (!crc32_ready) crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* ================================================================== */
/* BTAI raw partition TOC format                                      */
/* ================================================================== */

#define BTAI_MAGIC       0x49415442  /* "BTAI" little-endian */
#define BTAI_VERSION     1
#define BTAI_MAX_ENTRIES 64
#define BTAI_DATA_OFFSET 4096        /* data starts at 4KB into partition */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t _reserved;
    uint64_t partition_size;   /* total bytes in partition */
    uint64_t data_offset;     /* byte offset where data region starts */
    uint64_t next_free;       /* byte offset of next free space */
} btai_header;                /* 40 bytes */

typedef struct {
    char     name[32];
    uint64_t offset;           /* byte offset from partition start */
    uint64_t size;             /* size in bytes */
    uint32_t flags;            /* 0 = active, 1 = deleted */
    uint32_t _pad;
} btai_entry;                 /* 56 bytes */

/* ================================================================== */
/* GPT structures                                                      */
/* ================================================================== */

/* EFI System Partition type GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const EFI_GUID esp_type_guid = {
    0xC12A7328, 0xF81F, 0x11D2,
    {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}
};

/* BootAI Raw Data partition type GUID (custom):
 * B007A1DA-7A00-4000-8000-000000000001 */
static const EFI_GUID raw_type_guid = {
    0xB007A1DA, 0x7A00, 0x4000,
    {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}
};

/* Hardcoded unique partition GUIDs */
static const EFI_GUID esp_unique_guid = {
    0xB007A1DA, 0x0001, 0x4000,
    {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}
};

static const EFI_GUID raw_unique_guid = {
    0xB007A1DA, 0x0002, 0x4000,
    {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}
};

static const EFI_GUID disk_guid = {
    0xB007A1DA, 0xD15C, 0x4000,
    {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};

/* GPT header (92 bytes, written into 512-byte sector) */
typedef struct __attribute__((packed)) {
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    EFI_GUID disk_guid;
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_entry_crc32;
} gpt_header;

/* GPT partition entry (128 bytes) */
typedef struct __attribute__((packed)) {
    EFI_GUID type_guid;
    EFI_GUID unique_guid;
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} gpt_partition_entry;

/* ================================================================== */
/* Layout constants                                                    */
/* ================================================================== */

#define ESP_START_LBA    2048
#define ESP_SIZE_MB      100
#define ESP_SECTORS      ((ESP_SIZE_MB * 1024ULL * 1024) / 512)  /* 204800 */
#define RAW_START_LBA    (ESP_START_LBA + ESP_SECTORS)           /* 206848 */

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static void print_dec64(uint64_t n) {
    char buf[24];
    int i = 0;
    if (n == 0) { efi_print_ascii("0"); return; }
    char tmp[24];
    int ti = 0;
    while (n > 0) { tmp[ti++] = '0' + (int)(n % 10); n /= 10; }
    while (ti > 0) buf[i++] = tmp[--ti];
    buf[i] = '\0';
    efi_print_ascii(buf);
}

static void print_size_human(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        uint64_t gb = bytes / (1024ULL * 1024 * 1024);
        uint64_t frac = (bytes % (1024ULL * 1024 * 1024)) * 10 / (1024ULL * 1024 * 1024);
        print_dec64(gb);
        efi_print_ascii(".");
        print_dec64(frac);
        efi_print_ascii(" GB");
    } else if (bytes >= 1024 * 1024) {
        print_dec64(bytes / (1024 * 1024));
        efi_print_ascii(" MB");
    } else if (bytes >= 1024) {
        print_dec64(bytes / 1024);
        efi_print_ascii(" KB");
    } else {
        print_dec64(bytes);
        efi_print_ascii(" B");
    }
}

static void set_ucs2(uint16_t *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (uint16_t)(unsigned char)src[i];
        i++;
    }
    while (i < max) dst[i++] = 0;
}

/* Convert ASCII path to UCS-2 with \ separators */
static void ascii_to_ucs2(const char *src, uint16_t *dst, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = (src[i] == '/') ? (uint16_t)'\\' : (uint16_t)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

static void print_progress(uint64_t done, uint64_t total) {
    int pct = (int)((done * 100) / total);
    int filled = pct / 5;
    efi_print_ascii("\r        [");
    for (int i = 0; i < 20; i++)
        efi_print_ascii(i < filled ? "=" : " ");
    efi_print_ascii("] ");
    char buf[8];
    int bi = 0;
    if (pct >= 100) { buf[bi++]='1'; buf[bi++]='0'; buf[bi++]='0'; }
    else if (pct >= 10) { buf[bi++] = '0' + pct/10; buf[bi++] = '0' + pct%10; }
    else { buf[bi++] = '0' + pct; }
    buf[bi++] = '%';
    buf[bi] = 0;
    efi_print_ascii(buf);
}

static void print_hex8(uint8_t v) {
    const char *hex = "0123456789ABCDEF";
    char buf[3] = { hex[v >> 4], hex[v & 0xF], 0 };
    efi_print_ascii(buf);
}

static void print_hex32(uint32_t v) {
    efi_print_ascii("0x");
    for (int i = 7; i >= 0; i--) {
        const char *hex = "0123456789ABCDEF";
        char c[2] = { hex[(v >> (i * 4)) & 0xF], 0 };
        efi_print_ascii(c);
    }
}

static EFI_GUID gEfiFileInfoGuid_install = EFI_FILE_INFO_GUID;

/* ================================================================== */
/* Find internal (non-removable) disk                                 */
/*                                                                     */
/* SAFETY: Never returns the boot device. Uses LoadedImage protocol    */
/* to find which disk we booted from and excludes it. If only the      */
/* boot device exists (no internal HDD), returns EFI_NOT_FOUND.        */
/* ================================================================== */

static EFI_STATUS find_internal_disk(EFI_BLOCK_IO_PROTOCOL **disk_out,
                                      EFI_HANDLE *handle_out) {
    UINTN n_handles = 0;
    EFI_HANDLE *handles = 0;

    EFI_STATUS status = gBS->LocateHandleBuffer(
        2 /* ByProtocol */, &gEfiBlockIoProtocolGuid, 0, &n_handles, &handles);
    if (status != EFI_SUCCESS || n_handles == 0)
        return EFI_NOT_FOUND;

    /* --- Identify boot device so we can exclude it --- */
    EFI_HANDLE boot_disk_handle = 0;
    {
        EFI_LOADED_IMAGE_PROTOCOL *li = 0;
        status = gBS->HandleProtocol(gImageHandle,
                     &gEfiLoadedImageProtocolGuid, (void **)&li);
        if (status == EFI_SUCCESS && li && li->DeviceHandle) {
            /* DeviceHandle is the partition we booted from.
             * Find its parent whole-disk: the whole-disk Block IO handle
             * whose MediaId matches and is NOT a LogicalPartition. */
            EFI_BLOCK_IO_PROTOCOL *boot_part_bio = 0;
            gBS->HandleProtocol(li->DeviceHandle,
                                &gEfiBlockIoProtocolGuid, (void **)&boot_part_bio);

            if (boot_part_bio && boot_part_bio->Media) {
                uint32_t boot_media_id = boot_part_bio->Media->MediaId;
                for (UINTN i = 0; i < n_handles; i++) {
                    EFI_BLOCK_IO_PROTOCOL *bio = 0;
                    gBS->HandleProtocol(handles[i],
                                        &gEfiBlockIoProtocolGuid, (void **)&bio);
                    if (!bio || !bio->Media) continue;
                    if (bio->Media->LogicalPartition) continue;
                    if (bio->Media->MediaId == boot_media_id) {
                        boot_disk_handle = handles[i];
                        break;
                    }
                }
            }

            /* Fallback: if we couldn't find a whole-disk parent,
             * at least exclude the partition handle itself */
            if (!boot_disk_handle)
                boot_disk_handle = li->DeviceHandle;
        }
    }

    /* Find the largest whole-disk device, excluding the boot device.
     * Some firmware (Dell E6510) marks internal SATA as removable,
     * so we skip the RemovableMedia check and pick by size instead. */
    EFI_BLOCK_IO_PROTOCOL *best_bio = 0;
    EFI_HANDLE best_handle = 0;
    uint64_t best_size = 0;

    for (UINTN i = 0; i < n_handles; i++) {
        if (handles[i] == boot_disk_handle) continue;  /* skip boot device */

        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        status = gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, (void **)&bio);
        if (status != EFI_SUCCESS || !bio) continue;
        if (!bio->Media) continue;
        if (!bio->Media->MediaPresent) continue;
        if (bio->Media->LogicalPartition) continue;
        if (bio->Media->BlockSize != 512) continue;
        if (bio->Media->ReadOnly) continue;

        uint64_t sz = (bio->Media->LastBlock + 1) * 512ULL;

        /* Debug: print candidate */
        efi_print_ascii("    ");
        print_size_human(sz);
        efi_print_ascii(" disk (MediaId ");
        print_dec64(bio->Media->MediaId);
        efi_print_ascii(")\r\n");

        if (sz > best_size) {
            best_size = sz;
            best_bio = bio;
            best_handle = handles[i];
        }
    }

    if (best_bio) {
        *disk_out = best_bio;
        *handle_out = best_handle;
        gBS->FreePool(handles);
        return EFI_SUCCESS;
    }

    efi_print_ascii("    No internal disk found (boot device excluded)\r\n");
    gBS->FreePool(handles);
    return EFI_NOT_FOUND;
}

/* ================================================================== */
/* Write MBR with real partition entries (no GPT)                      */
/*                                                                     */
/* Old Dell UEFI firmware (E6510) doesn't boot from GPT disks.         */
/* Using plain MBR with FAT32 + raw data partitions works because      */
/* it's the same format as the USB drive (which boots fine).            */
/* ================================================================== */

/* CHS encoding for MBR partition entries.
 * Always use FE FF FF (LBA-only marker) — matches how the DUET USB
 * image is built and avoids CHS translation issues with boot0. */
static void lba_to_chs(uint32_t lba, uint8_t *chs) {
    (void)lba;
    chs[0] = 0xFE; chs[1] = 0xFF; chs[2] = 0xFF;
}

static EFI_STATUS write_mbr(EFI_BLOCK_IO_PROTOCOL *disk,
                            const uint8_t *boot0_code) {
    uint8_t *mbr = 0;
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, 512, (void **)&mbr);
    if (status != EFI_SUCCESS) return status;
    memset(mbr, 0, 512);

    /* If DUET boot0 code provided, inject into MBR (first 446 bytes) */
    if (boot0_code)
        memcpy(mbr, boot0_code, 446);

    uint64_t last = disk->Media->LastBlock;
    uint32_t mid = disk->Media->MediaId;

    /* Destroy any leftover GPT (LBA 1-33 + backup at end of disk).
     * UEFI spec: firmware MUST prefer GPT over MBR if valid GPT exists.
     * Old installs wrote GPT headers here — must be zeroed. */
    {
        UINTN wipe_bytes = 33 * 512;
        uint8_t *zero = 0;
        if (gBS->AllocatePool(EfiLoaderData, wipe_bytes, (void **)&zero) == EFI_SUCCESS) {
            memset(zero, 0, wipe_bytes);
            disk->WriteBlocks(disk, mid, 1, wipe_bytes, zero);
            if (last > 33) {
                disk->WriteBlocks(disk, mid, last - 32, wipe_bytes, zero);
                disk->WriteBlocks(disk, mid, last, 512, zero);
            }
            gBS->FreePool(zero);
        }
    }

    /* Disk signature at offset 440 — must be zero to match working USB image.
     * DUET boot entry also uses zero signature. */

    /* Partition 1: FAT32 LBA, bootable, 100MB at LBA 2048 */
    uint8_t *pe1 = &mbr[446];
    pe1[0] = 0x80;                                      /* Bootable / active */
    lba_to_chs(ESP_START_LBA, &pe1[1]);                 /* CHS start */
    pe1[4] = boot0_code ? 0x0C : 0xEF;                   /* FAT32 LBA for DUET, EFI System otherwise */
    uint32_t esp_end_lba = ESP_START_LBA + (uint32_t)ESP_SECTORS - 1;
    lba_to_chs(esp_end_lba, &pe1[5]);                   /* CHS end */
    *(uint32_t *)&pe1[8] = ESP_START_LBA;               /* LBA start */
    *(uint32_t *)&pe1[12] = (uint32_t)ESP_SECTORS;      /* Sector count */

    /* Partition 2: Raw data, rest of disk */
    uint8_t *pe2 = &mbr[462];
    pe2[0] = 0x00;                                      /* Not bootable */
    lba_to_chs(RAW_START_LBA, &pe2[1]);                 /* CHS start */
    pe2[4] = 0xDA;                                      /* Custom type (0xDA = BootAI data) */
    uint32_t raw_end = (last > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)last;
    lba_to_chs(raw_end, &pe2[5]);                       /* CHS end */
    *(uint32_t *)&pe2[8] = RAW_START_LBA;               /* LBA start */
    uint32_t raw_sectors = raw_end - RAW_START_LBA + 1;
    *(uint32_t *)&pe2[12] = raw_sectors;                 /* Sector count */

    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    status = disk->WriteBlocks(disk, disk->Media->MediaId, 0, 512, mbr);
    gBS->FreePool(mbr);
    return status;
}

/* ================================================================== */
/* Write GPT (primary + backup headers and partition entries)          */
/* ================================================================== */

static EFI_STATUS write_gpt(EFI_BLOCK_IO_PROTOCOL *disk) {
    uint64_t last_lba = disk->Media->LastBlock;
    uint32_t media_id = disk->Media->MediaId;
    EFI_STATUS status;

    /* Write protective MBR at LBA 0 (required by GPT spec) */
    {
        uint8_t *mbr = 0;
        status = gBS->AllocatePool(EfiLoaderData, 512, (void **)&mbr);
        if (status != EFI_SUCCESS) return status;
        memset(mbr, 0, 512);

        /* Single partition entry covering entire disk, type 0xEE (GPT protective) */
        uint8_t *pe = &mbr[446];
        pe[0] = 0x00;            /* Not bootable */
        pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;  /* CHS start: 0/0/2 */
        pe[4] = 0xEE;            /* GPT protective */
        pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;  /* CHS end: max */
        *(uint32_t *)&pe[8] = 1;  /* LBA start = 1 */
        uint32_t mbr_size = (last_lba > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)last_lba;
        *(uint32_t *)&pe[12] = mbr_size;  /* Sector count */

        mbr[510] = 0x55;
        mbr[511] = 0xAA;

        status = disk->WriteBlocks(disk, media_id, 0, 512, mbr);
        gBS->FreePool(mbr);
        if (status != EFI_SUCCESS) return status;
    }

    uint64_t esp_end = ESP_START_LBA + ESP_SECTORS - 1;
    uint64_t raw_end = last_lba - 33;

    /* Build partition entry array (128 entries x 128 bytes = 16384 bytes) */
    uint32_t entry_count = 128;
    uint32_t entry_size = 128;
    uint32_t entries_bytes = entry_count * entry_size;

    uint8_t *entries = 0;
    status = gBS->AllocatePool(EfiLoaderData, entries_bytes, (void **)&entries);
    if (status != EFI_SUCCESS) return status;
    memset(entries, 0, entries_bytes);

    /* Entry 0: ESP (100MB FAT32) */
    gpt_partition_entry *e0 = (gpt_partition_entry *)&entries[0];
    memcpy(&e0->type_guid, &esp_type_guid, sizeof(EFI_GUID));
    memcpy(&e0->unique_guid, &esp_unique_guid, sizeof(EFI_GUID));
    e0->first_lba = ESP_START_LBA;
    e0->last_lba = esp_end;
    e0->attributes = 0;
    { uint16_t nb[36] = {0}; set_ucs2(nb, "EFI System", 36); memcpy(e0->name, nb, 72); }

    /* Entry 1: Raw data partition (rest of disk) */
    gpt_partition_entry *e1 = (gpt_partition_entry *)&entries[entry_size];
    memcpy(&e1->type_guid, &raw_type_guid, sizeof(EFI_GUID));
    memcpy(&e1->unique_guid, &raw_unique_guid, sizeof(EFI_GUID));
    e1->first_lba = RAW_START_LBA;
    e1->last_lba = raw_end;
    e1->attributes = 0;
    { uint16_t nb[36] = {0}; set_ucs2(nb, "BootAI Data", 36); memcpy(e1->name, nb, 72); }

    uint32_t entries_crc = crc32(entries, entries_bytes);

    /* Primary GPT header at LBA 1 */
    uint8_t *hdr_buf = 0;
    status = gBS->AllocatePool(EfiLoaderData, 512, (void **)&hdr_buf);
    if (status != EFI_SUCCESS) { gBS->FreePool(entries); return status; }
    memset(hdr_buf, 0, 512);

    gpt_header *hdr = (gpt_header *)hdr_buf;
    memcpy(hdr->signature, "EFI PART", 8);
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->header_crc32 = 0;
    hdr->reserved = 0;
    hdr->my_lba = 1;
    hdr->alternate_lba = last_lba;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = last_lba - 33;
    memcpy(&hdr->disk_guid, &disk_guid, sizeof(EFI_GUID));
    hdr->partition_entry_lba = 2;
    hdr->num_partition_entries = entry_count;
    hdr->partition_entry_size = entry_size;
    hdr->partition_entry_crc32 = entries_crc;
    hdr->header_crc32 = crc32(hdr_buf, 92);

    status = disk->WriteBlocks(disk, media_id, 1, 512, hdr_buf);
    if (status != EFI_SUCCESS) goto cleanup;

    /* Primary partition entries at LBA 2-33 */
    status = disk->WriteBlocks(disk, media_id, 2, entries_bytes, entries);
    if (status != EFI_SUCCESS) goto cleanup;

    /* Backup partition entries at (last_lba - 32) */
    status = disk->WriteBlocks(disk, media_id, last_lba - 32, entries_bytes, entries);
    if (status != EFI_SUCCESS) goto cleanup;

    /* Backup GPT header at last_lba */
    memset(hdr_buf, 0, 512);
    hdr = (gpt_header *)hdr_buf;
    memcpy(hdr->signature, "EFI PART", 8);
    hdr->revision = 0x00010000;
    hdr->header_size = 92;
    hdr->header_crc32 = 0;
    hdr->reserved = 0;
    hdr->my_lba = last_lba;
    hdr->alternate_lba = 1;
    hdr->first_usable_lba = 34;
    hdr->last_usable_lba = last_lba - 33;
    memcpy(&hdr->disk_guid, &disk_guid, sizeof(EFI_GUID));
    hdr->partition_entry_lba = last_lba - 32;
    hdr->num_partition_entries = entry_count;
    hdr->partition_entry_size = entry_size;
    hdr->partition_entry_crc32 = entries_crc;
    hdr->header_crc32 = crc32(hdr_buf, 92);

    status = disk->WriteBlocks(disk, media_id, last_lba, 512, hdr_buf);

cleanup:
    gBS->FreePool(hdr_buf);
    gBS->FreePool(entries);
    return status;
}

/* ================================================================== */
/* Format FAT32 on the ESP partition                                   */
/* ================================================================== */

static EFI_STATUS format_fat32(EFI_BLOCK_IO_PROTOCOL *disk) {
    uint32_t media_id = disk->Media->MediaId;
    uint32_t total_sectors = (uint32_t)ESP_SECTORS;
    uint32_t spc = 1;          /* 512B clusters — must keep cluster count ≥65525 for FAT32 */
    uint32_t reserved = 32;
    uint32_t num_fats = 2;
    EFI_STATUS status;

    /* FAT size: ceil((total - reserved) / (128 * spc + 2)) */
    uint32_t fat_size = (total_sectors - reserved + 128 * spc + 1) / (128 * spc + 2);

    /* Allocate reserved area buffer (32 sectors = 16KB) */
    UINTN rsv_bytes = reserved * 512;
    uint8_t *rsv = 0;
    status = gBS->AllocatePool(EfiLoaderData, rsv_bytes, (void **)&rsv);
    if (status != EFI_SUCCESS) return status;
    memset(rsv, 0, rsv_bytes);

    /* BPB at sector 0 */
    uint8_t *bpb = &rsv[0];
    bpb[0] = 0xEB; bpb[1] = 0x58; bpb[2] = 0x90;
    memcpy(&bpb[3], "BOOTAI  ", 8);
    *(uint16_t *)&bpb[11] = 512;                           /* BytsPerSec */
    bpb[13] = (uint8_t)spc;                                /* SecPerClus */
    *(uint16_t *)&bpb[14] = (uint16_t)reserved;            /* RsvdSecCnt */
    bpb[16] = (uint8_t)num_fats;                           /* NumFATs */
    bpb[21] = 0xF8;                                        /* Media (fixed) */
    *(uint16_t *)&bpb[24] = 63;                            /* SecPerTrk */
    *(uint16_t *)&bpb[26] = 255;                           /* NumHeads */
    *(uint32_t *)&bpb[28] = 0;                              /* HiddSec: 0 to match DUET boot1f32 expectations */
    *(uint32_t *)&bpb[32] = total_sectors;                 /* TotSec32 */
    *(uint32_t *)&bpb[36] = fat_size;                      /* FATSz32 */
    *(uint32_t *)&bpb[44] = 2;                             /* RootClus */
    *(uint16_t *)&bpb[48] = 1;                             /* FSInfo */
    *(uint16_t *)&bpb[50] = 6;                             /* BkBootSec */
    bpb[64] = 0x80;                                        /* BS_DrvNum */
    bpb[66] = 0x29;                                        /* BS_BootSig */
    *(uint32_t *)&bpb[67] = 0xB007A1DA;                    /* BS_VolID */
    memcpy(&bpb[71], "BOOTAI     ", 11);                   /* BS_VolLab */
    memcpy(&bpb[82], "FAT32   ", 8);                       /* BS_FilSysType */
    bpb[510] = 0x55; bpb[511] = 0xAA;

    /* FSINFO at sector 1 */
    uint8_t *fsi = &rsv[512];
    *(uint32_t *)&fsi[0] = 0x41615252;                     /* LeadSig */
    *(uint32_t *)&fsi[484] = 0x61417272;                   /* StrucSig */
    uint32_t data_clusters = (total_sectors - reserved - num_fats * fat_size) / spc;
    *(uint32_t *)&fsi[488] = data_clusters - 1;            /* Free_Count */
    *(uint32_t *)&fsi[492] = 3;                            /* Nxt_Free */
    *(uint32_t *)&fsi[508] = 0xAA550000;                   /* TrailSig */

    /* Backup boot sector at sector 6 */
    memcpy(&rsv[6 * 512], bpb, 512);

    /* Write entire reserved area */
    status = disk->WriteBlocks(disk, media_id, ESP_START_LBA, rsv_bytes, rsv);
    gBS->FreePool(rsv);
    if (status != EFI_SUCCESS) return status;

    /* Write FAT copies */
    UINTN fat_bytes = (UINTN)fat_size * 512;
    uint8_t *fat_buf = 0;
    status = gBS->AllocatePool(EfiLoaderData, fat_bytes, (void **)&fat_buf);
    if (status != EFI_SUCCESS) return status;
    memset(fat_buf, 0, fat_bytes);

    *(uint32_t *)&fat_buf[0] = 0x0FFFFFF8;     /* FAT[0]: media descriptor */
    *(uint32_t *)&fat_buf[4] = 0x0FFFFFFF;     /* FAT[1]: end-of-chain */
    *(uint32_t *)&fat_buf[8] = 0x0FFFFFFF;     /* FAT[2]: root dir EOC */

    uint64_t fat1_start = ESP_START_LBA + reserved;
    uint64_t fat2_start = fat1_start + fat_size;

    status = disk->WriteBlocks(disk, media_id, fat1_start, fat_bytes, fat_buf);
    if (status == EFI_SUCCESS)
        status = disk->WriteBlocks(disk, media_id, fat2_start, fat_bytes, fat_buf);
    gBS->FreePool(fat_buf);
    if (status != EFI_SUCCESS) return status;

    /* Zero root directory cluster (cluster 2) */
    UINTN cluster_bytes = spc * 512;
    uint8_t *zero_buf = 0;
    status = gBS->AllocatePool(EfiLoaderData, cluster_bytes, (void **)&zero_buf);
    if (status != EFI_SUCCESS) return status;
    memset(zero_buf, 0, cluster_bytes);

    uint64_t data_start = fat2_start + fat_size;
    status = disk->WriteBlocks(disk, media_id, data_start, cluster_bytes, zero_buf);
    gBS->FreePool(zero_buf);
    if (status != EFI_SUCCESS) return status;

    disk->FlushBlocks(disk);
    return EFI_SUCCESS;
}

/* ================================================================== */
/* Merge boot1f32 PBR code with FAT32 BPB for DUET Legacy Boot        */
/*                                                                     */
/* boot1f32 provides the boot code, format_fat32() provides the BPB.  */
/* The 14 pseudo-random bytes at offset 496 are critical — DUET's BDS */
/* uses them as a partition signature and rejects all-zeros.           */
/* ================================================================== */

static EFI_STATUS merge_pbr_boot1f32(EFI_BLOCK_IO_PROTOCOL *disk,
                                      const uint8_t *boot1f32) {
    uint32_t mid = disk->Media->MediaId;
    uint8_t *pbr = 0;
    EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, 512, (void **)&pbr);
    if (st != EFI_SUCCESS) return st;

    /* Read the PBR that format_fat32() wrote (has correct BPB) */
    st = disk->ReadBlocks(disk, mid, ESP_START_LBA, 512, pbr);
    if (st != EFI_SUCCESS) { gBS->FreePool(pbr); return st; }

    /* Save the BPB (bytes 3-89, 87 bytes) */
    uint8_t bpb_save[87];
    memcpy(bpb_save, &pbr[3], 87);

    /* Copy boot1f32 as template (has jump instruction + boot code) */
    memcpy(pbr, boot1f32, 512);

    /* Restore BPB from format_fat32 */
    memcpy(&pbr[3], bpb_save, 87);

    /* Write 14 pseudo-random bytes at offset 496-509.
     * DUET BDS uses these as a partition signature.
     * Use BPB VolID + disk geometry as entropy seed (no urandom available). */
    {
        uint32_t seed = *(uint32_t *)&bpb_save[64];  /* BS_VolID */
        seed ^= *(uint32_t *)&bpb_save[25];          /* HiddSec */
        seed ^= (uint32_t)disk->Media->LastBlock;
        for (int i = 0; i < 14; i++) {
            seed = seed * 1664525 + 1013904223;
            pbr[496 + i] = (uint8_t)(seed >> 16);
        }
    }

    /* Ensure boot signature */
    pbr[510] = 0x55;
    pbr[511] = 0xAA;

    /* Write PBR to partition start */
    st = disk->WriteBlocks(disk, mid, ESP_START_LBA, 512, pbr);
    if (st != EFI_SUCCESS) { gBS->FreePool(pbr); return st; }

    /* Also update backup boot sector at partition sector 6 */
    st = disk->WriteBlocks(disk, mid, ESP_START_LBA + 6, 512, pbr);

    gBS->FreePool(pbr);
    return st;
}

/* (Model copy is now done inline from pre-read buffer in ba_install_to_hd) */

/* ================================================================== */
/* Direct FAT32 file writer via Block IO                               */
/*                                                                     */
/* Dell UEFI won't re-enumerate partitions after ConnectController,    */
/* so we write files directly to the FAT32 sectors instead of relying  */
/* on the UEFI filesystem driver to mount the new partition.           */
/* ================================================================== */

/* Pre-read file from USB into memory buffer */
typedef struct {
    void  *data;
    UINTN  size;
    UINTN  pages;
} preread_file;

static EFI_STATUS preread_usb_file(EFI_FILE_PROTOCOL *usb_root,
                                    const char *path,
                                    preread_file *out) {
    out->data = 0;
    out->size = 0;
    out->pages = 0;

    uint16_t wpath[256];
    ascii_to_ucs2(path, wpath, 256);

    EFI_FILE_PROTOCOL *src = 0;
    EFI_STATUS status = usb_root->Open(usb_root, &src, wpath,
                                        EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) return EFI_NOT_FOUND;

    uint8_t info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = src->GetInfo(src, &gEfiFileInfoGuid_install, &info_size, info_buf);
    if (status != EFI_SUCCESS) { src->Close(src); return status; }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    uint64_t file_size = info->FileSize;

    UINTN pages = ((UINTN)file_size + 4095) / 4096;
    if (pages == 0) pages = 1;
    uint64_t phys = 0;
    status = gBS->AllocatePages(0, EfiLoaderData, pages, &phys);
    if (status != EFI_SUCCESS) { src->Close(src); return status; }

    uint8_t *buf = (uint8_t *)(UINTN)phys;
    UINTN read_size = (UINTN)file_size;
    status = src->Read(src, &read_size, buf);
    src->Close(src);

    if (status != EFI_SUCCESS) {
        gBS->FreePages(phys, pages);
        return status;
    }

    out->data = buf;
    out->size = read_size;
    out->pages = pages;
    return EFI_SUCCESS;
}

static void preread_free(preread_file *f) {
    if (f->data) {
        gBS->FreePages((uint64_t)(UINTN)f->data, f->pages);
        f->data = 0;
    }
}

/* FAT32 layout state — matches values from format_fat32() */
typedef struct {
    EFI_BLOCK_IO_PROTOCOL *disk;
    uint32_t media_id;
    uint32_t spc;            /* sectors per cluster */
    uint32_t reserved;       /* reserved sectors (32) */
    uint32_t fat_size;       /* FAT size in sectors */
    uint64_t fat1_lba;       /* LBA of first FAT */
    uint64_t fat2_lba;       /* LBA of second FAT */
    uint64_t data_lba;       /* LBA of data region (cluster 2) */
    uint32_t cluster_bytes;  /* spc * 512 */
    uint32_t next_cluster;   /* next free cluster to allocate */
    /* In-memory FAT (we keep a copy and flush both FAT1+FAT2 at end) */
    uint32_t *fat;
    uint32_t fat_entries;    /* max cluster entries */
    UINTN    fat_bytes;
} fat32_state;

static void fat32_init(fat32_state *f, EFI_BLOCK_IO_PROTOCOL *disk) {
    f->disk = disk;
    f->media_id = disk->Media->MediaId;
    f->spc = 1;  /* must match format_fat32() — keeps cluster count ≥65525 for FAT32 */
    f->reserved = 32;
    uint32_t total = (uint32_t)ESP_SECTORS;
    f->fat_size = (total - f->reserved + 128 * f->spc + 1) / (128 * f->spc + 2);
    f->fat1_lba = ESP_START_LBA + f->reserved;
    f->fat2_lba = f->fat1_lba + f->fat_size;
    f->data_lba = f->fat2_lba + f->fat_size;
    f->cluster_bytes = f->spc * 512;
    f->next_cluster = 3;  /* 0,1 reserved, 2 = root dir */
    f->fat_bytes = (UINTN)f->fat_size * 512;
    f->fat_entries = f->fat_bytes / 4;
    f->fat = 0;
}

static EFI_STATUS fat32_load_fat(fat32_state *f) {
    EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, f->fat_bytes, (void **)&f->fat);
    if (st != EFI_SUCCESS) return st;
    /* Read existing FAT (has entries 0-2 set by format_fat32) */
    return f->disk->ReadBlocks(f->disk, f->media_id, f->fat1_lba, f->fat_bytes, f->fat);
}

static EFI_STATUS fat32_flush_fat(fat32_state *f) {
    EFI_STATUS st = f->disk->WriteBlocks(f->disk, f->media_id,
                                          f->fat1_lba, f->fat_bytes, f->fat);
    if (st != EFI_SUCCESS) return st;
    return f->disk->WriteBlocks(f->disk, f->media_id,
                                f->fat2_lba, f->fat_bytes, f->fat);
}

static void fat32_free(fat32_state *f) {
    if (f->fat) { gBS->FreePool(f->fat); f->fat = 0; }
}

/* Get LBA for start of a cluster */
static uint64_t cluster_lba(fat32_state *f, uint32_t cluster) {
    return f->data_lba + (uint64_t)(cluster - 2) * f->spc;
}

/* Allocate N contiguous clusters, chain them in FAT, return first cluster */
static uint32_t fat32_alloc(fat32_state *f, uint32_t n_clusters) {
    uint32_t first = f->next_cluster;
    for (uint32_t i = 0; i < n_clusters; i++) {
        uint32_t c = first + i;
        if (c >= f->fat_entries) return 0;  /* out of space */
        f->fat[c] = (i + 1 < n_clusters) ? (c + 1) : 0x0FFFFFFF;
    }
    f->next_cluster = first + n_clusters;
    return first;
}

/* Write a FAT32 8.3 directory entry */
static void fat32_dir_entry(uint8_t *entry, const char *name83,
                             uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, 32);
    memcpy(entry, name83, 11);
    entry[11] = attr;
    /* First cluster high word */
    entry[20] = (uint8_t)(cluster >> 16);
    entry[21] = (uint8_t)(cluster >> 24);
    /* First cluster low word */
    entry[26] = (uint8_t)(cluster);
    entry[27] = (uint8_t)(cluster >> 8);
    /* File size */
    *(uint32_t *)&entry[28] = size;
}

/* Write file data to allocated clusters via Block IO */
static EFI_STATUS fat32_write_data(fat32_state *f, uint32_t cluster,
                                    const void *data, UINTN size) {
    const uint8_t *src = (const uint8_t *)data;
    UINTN remaining = size;
    uint32_t c = cluster;

    while (remaining > 0 && c < f->fat_entries && c >= 2) {
        UINTN chunk = remaining < f->cluster_bytes ? remaining : f->cluster_bytes;

        /* Allocate a cluster-sized buffer (Block IO needs sector alignment) */
        uint8_t *buf = 0;
        EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, f->cluster_bytes, (void **)&buf);
        if (st != EFI_SUCCESS) return st;
        memset(buf, 0, f->cluster_bytes);
        memcpy(buf, src, chunk);

        st = f->disk->WriteBlocks(f->disk, f->media_id,
                                   cluster_lba(f, c), f->cluster_bytes, buf);
        gBS->FreePool(buf);
        if (st != EFI_SUCCESS) return st;

        src += chunk;
        remaining -= chunk;

        /* Follow FAT chain */
        uint32_t next = f->fat[c];
        if (next >= 0x0FFFFFF8) break;
        c = next;
    }
    return EFI_SUCCESS;
}

/* Create a directory in the FAT32 filesystem */
static uint32_t fat32_mkdir(fat32_state *f, uint32_t parent_cluster,
                             uint8_t *parent_dir, int *parent_idx,
                             const char *name83) {
    uint32_t dir_cluster = fat32_alloc(f, 1);
    if (!dir_cluster) return 0;

    /* Write directory entry in parent */
    fat32_dir_entry(&parent_dir[*parent_idx * 32], name83, 0x10, dir_cluster, 0);
    (*parent_idx)++;

    /* Write . and .. in new directory */
    uint8_t *dir = 0;
    gBS->AllocatePool(EfiLoaderData, f->cluster_bytes, (void **)&dir);
    if (!dir) return 0;
    memset(dir, 0, f->cluster_bytes);

    fat32_dir_entry(&dir[0],  ".          ", 0x10, dir_cluster, 0);
    fat32_dir_entry(&dir[32], "..         ", 0x10, parent_cluster, 0);

    f->disk->WriteBlocks(f->disk, f->media_id,
                          cluster_lba(f, dir_cluster), f->cluster_bytes, dir);
    gBS->FreePool(dir);
    return dir_cluster;
}

/* Write all ESP files directly to FAT32 via Block IO.
 * Layout:
 *   Cluster 2: root dir -> EFI/, rwkvos/, workspace/, wifi.conf
 *   Cluster 3: EFI/     -> BOOT/
 *   Cluster 4: BOOT/    -> BOOTX64.EFI
 *   Cluster 5: rwkvos/  -> vocab.btv
 *   Cluster 6: workspace/ (empty)
 *   Cluster 7+: file data
 */
static EFI_STATUS fat32_write_esp_files(EFI_BLOCK_IO_PROTOCOL *disk,
                                         preread_file *efi_bin,
                                         preread_file *vocab,
                                         preread_file *wifi,
                                         preread_file *duet_fw) {
    fat32_state f;
    fat32_init(&f, disk);

    EFI_STATUS st = fat32_load_fat(&f);
    if (st != EFI_SUCCESS) return st;

    /* Root directory buffer (cluster 2, already allocated by format) */
    uint8_t *root_dir = 0;
    st = gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&root_dir);
    if (st != EFI_SUCCESS) { fat32_free(&f); return st; }
    memset(root_dir, 0, f.cluster_bytes);
    int root_idx = 0;

    /* Create EFI/ directory */
    uint8_t *efi_dir = 0;
    gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&efi_dir);
    memset(efi_dir, 0, f.cluster_bytes);
    int efi_idx = 2;  /* skip . and .. */

    uint32_t efi_cluster = fat32_mkdir(&f, 2, root_dir, &root_idx, "EFI        ");

    /* Create EFI/BOOT/ directory */
    uint8_t *boot_dir = 0;
    gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&boot_dir);
    memset(boot_dir, 0, f.cluster_bytes);
    int boot_idx = 2;  /* skip . and .. */

    uint32_t boot_cluster = fat32_mkdir(&f, efi_cluster, efi_dir, &efi_idx, "BOOT       ");

    /* Create rwkvos/ directory */
    uint8_t *rwkvos_dir = 0;
    gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&rwkvos_dir);
    memset(rwkvos_dir, 0, f.cluster_bytes);
    int rwkvos_idx = 2;  /* skip . and .. */

    uint32_t rwkvos_cluster = fat32_mkdir(&f, 2, root_dir, &root_idx, "RWKVOS     ");

    /* Create workspace/ directory */
    fat32_mkdir(&f, 2, root_dir, &root_idx, "WORK       ");

    /* DUET: Create EFI/OC/ directory for OpenCore BDS path */
    uint8_t *oc_dir = 0;
    int oc_idx = 0;
    uint32_t oc_cluster = 0;
    if (duet_fw && duet_fw->data) {
        gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&oc_dir);
        if (oc_dir) {
            memset(oc_dir, 0, f.cluster_bytes);
            oc_idx = 2;  /* skip . and .. */
            oc_cluster = fat32_mkdir(&f, efi_cluster, efi_dir, &efi_idx, "OC         ");
        }
    }

    /* Write BOOTX64.EFI */
    if (efi_bin->data && efi_bin->size > 0) {
        uint32_t n = (uint32_t)((efi_bin->size + f.cluster_bytes - 1) / f.cluster_bytes);
        uint32_t fc = fat32_alloc(&f, n);
        if (fc) {
            fat32_dir_entry(&boot_dir[boot_idx * 32],
                            "BOOTX64 EFI", 0x20, fc, (uint32_t)efi_bin->size);
            boot_idx++;
            st = fat32_write_data(&f, fc, efi_bin->data, efi_bin->size);
            if (st == EFI_SUCCESS) {
                efi_print_ascii("        BOOTX64.EFI (");
                print_size_human(efi_bin->size);
                efi_print_ascii(") OK\r\n");
            } else {
                efi_print_ascii("        BOOTX64.EFI WRITE FAILED\r\n");
            }
        }
    } else {
        efi_print_ascii("        BOOTX64.EFI -- not found on USB\r\n");
    }

    /* Write vocab.btv */
    if (vocab->data && vocab->size > 0) {
        uint32_t n = (uint32_t)((vocab->size + f.cluster_bytes - 1) / f.cluster_bytes);
        uint32_t fc = fat32_alloc(&f, n);
        if (fc) {
            fat32_dir_entry(&rwkvos_dir[rwkvos_idx * 32],
                            "VOCAB   BTV", 0x20, fc, (uint32_t)vocab->size);
            rwkvos_idx++;
            st = fat32_write_data(&f, fc, vocab->data, vocab->size);
            if (st == EFI_SUCCESS) {
                efi_print_ascii("        vocab.btv (");
                print_size_human(vocab->size);
                efi_print_ascii(") OK\r\n");
            } else {
                efi_print_ascii("        vocab.btv WRITE FAILED\r\n");
            }
        }
    } else {
        efi_print_ascii("        vocab.btv -- not found on USB\r\n");
    }

    /* Write wifi.conf */
    if (wifi->data && wifi->size > 0) {
        uint32_t n = (uint32_t)((wifi->size + f.cluster_bytes - 1) / f.cluster_bytes);
        uint32_t fc = fat32_alloc(&f, n);
        if (fc) {
            fat32_dir_entry(&root_dir[root_idx * 32],
                            "WIFI    CON", 0x20, fc, (uint32_t)wifi->size);
            root_idx++;
            st = fat32_write_data(&f, fc, wifi->data, wifi->size);
            if (st == EFI_SUCCESS) {
                efi_print_ascii("        wifi.conf (");
                print_size_human(wifi->size);
                efi_print_ascii(") OK\r\n");
            } else {
                efi_print_ascii("        wifi.conf WRITE FAILED\r\n");
            }
        }
    }

    /* DUET: Write /boot (DUET firmware) */
    if (duet_fw && duet_fw->data && duet_fw->size > 0) {
        uint32_t n = (uint32_t)((duet_fw->size + f.cluster_bytes - 1) / f.cluster_bytes);
        uint32_t fc = fat32_alloc(&f, n);
        if (fc) {
            fat32_dir_entry(&root_dir[root_idx * 32],
                            "BOOT       ", 0x20, fc, (uint32_t)duet_fw->size);
            root_idx++;
            st = fat32_write_data(&f, fc, duet_fw->data, duet_fw->size);
            if (st == EFI_SUCCESS) {
                efi_print_ascii("        boot (");
                print_size_human(duet_fw->size);
                efi_print_ascii(") OK\r\n");
            } else {
                efi_print_ascii("        boot WRITE FAILED\r\n");
            }
        }
    }

    /* DUET: Write /EFI/OC/OpenCore.efi (copy of BootAI EFI binary) */
    if (oc_dir && efi_bin->data && efi_bin->size > 0) {
        uint32_t n = (uint32_t)((efi_bin->size + f.cluster_bytes - 1) / f.cluster_bytes);
        uint32_t fc = fat32_alloc(&f, n);
        if (fc) {
            fat32_dir_entry(&oc_dir[oc_idx * 32],
                            "OPENCOREEFI", 0x20, fc, (uint32_t)efi_bin->size);
            oc_idx++;
            st = fat32_write_data(&f, fc, efi_bin->data, efi_bin->size);
            if (st == EFI_SUCCESS) {
                efi_print_ascii("        EFI/OC/OpenCore.efi (");
                print_size_human(efi_bin->size);
                efi_print_ascii(") OK\r\n");
            } else {
                efi_print_ascii("        EFI/OC/OpenCore.efi WRITE FAILED\r\n");
            }
        }
    }

    /* Write directory clusters to disk */
    f.disk->WriteBlocks(f.disk, f.media_id,
                         cluster_lba(&f, 2), f.cluster_bytes, root_dir);

    /* Re-write EFI/ dir with BOOT entry */
    {
        /* Read what fat32_mkdir wrote, patch in BOOT entry */
        uint8_t *buf = 0;
        gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&buf);
        memset(buf, 0, f.cluster_bytes);
        fat32_dir_entry(&buf[0],  ".          ", 0x10, efi_cluster, 0);
        fat32_dir_entry(&buf[32], "..         ", 0x10, 2, 0);
        /* Copy entries from efi_dir starting at index 2 */
        for (int i = 2; i < efi_idx; i++)
            memcpy(&buf[i * 32], &efi_dir[i * 32], 32);
        f.disk->WriteBlocks(f.disk, f.media_id,
                             cluster_lba(&f, efi_cluster), f.cluster_bytes, buf);
        gBS->FreePool(buf);
    }

    /* Re-write BOOT/ dir with BOOTX64.EFI entry */
    {
        uint8_t *buf = 0;
        gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&buf);
        memset(buf, 0, f.cluster_bytes);
        fat32_dir_entry(&buf[0],  ".          ", 0x10, boot_cluster, 0);
        fat32_dir_entry(&buf[32], "..         ", 0x10, efi_cluster, 0);
        for (int i = 2; i < boot_idx; i++)
            memcpy(&buf[i * 32], &boot_dir[i * 32], 32);
        f.disk->WriteBlocks(f.disk, f.media_id,
                             cluster_lba(&f, boot_cluster), f.cluster_bytes, buf);
        gBS->FreePool(buf);
    }

    /* Re-write rwkvos/ dir with vocab entry */
    {
        uint8_t *buf = 0;
        gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&buf);
        memset(buf, 0, f.cluster_bytes);
        fat32_dir_entry(&buf[0],  ".          ", 0x10, rwkvos_cluster, 0);
        fat32_dir_entry(&buf[32], "..         ", 0x10, 2, 0);
        for (int i = 2; i < rwkvos_idx; i++)
            memcpy(&buf[i * 32], &rwkvos_dir[i * 32], 32);
        f.disk->WriteBlocks(f.disk, f.media_id,
                             cluster_lba(&f, rwkvos_cluster), f.cluster_bytes, buf);
        gBS->FreePool(buf);
    }

    /* Re-write OC/ dir with OpenCore.efi entry (if DUET) */
    if (oc_dir && oc_cluster) {
        uint8_t *buf = 0;
        gBS->AllocatePool(EfiLoaderData, f.cluster_bytes, (void **)&buf);
        if (buf) {
            memset(buf, 0, f.cluster_bytes);
            fat32_dir_entry(&buf[0],  ".          ", 0x10, oc_cluster, 0);
            fat32_dir_entry(&buf[32], "..         ", 0x10, efi_cluster, 0);
            for (int i = 2; i < oc_idx; i++)
                memcpy(&buf[i * 32], &oc_dir[i * 32], 32);
            f.disk->WriteBlocks(f.disk, f.media_id,
                                 cluster_lba(&f, oc_cluster), f.cluster_bytes, buf);
            gBS->FreePool(buf);
        }
    }

    /* Flush FAT (both copies) */
    fat32_flush_fat(&f);

    /* Flush disk */
    f.disk->FlushBlocks(f.disk);

    gBS->FreePool(root_dir);
    gBS->FreePool(efi_dir);
    gBS->FreePool(boot_dir);
    gBS->FreePool(rwkvos_dir);
    if (oc_dir) gBS->FreePool(oc_dir);
    fat32_free(&f);
    return EFI_SUCCESS;
}

/* ================================================================== */
/* Create UEFI NVRAM boot entry                                        */
/*                                                                     */
/* Writes a Boot0001 variable and updates BootOrder so the firmware    */
/* knows to boot from our ESP on the HD. Without this, many machines   */
/* (including Dell E6510) show "No bootable devices".                  */
/* ================================================================== */

/* EFI Global Variable GUID */
static EFI_GUID gEfiGlobalVariableGuid = {
    0x8BE4DF61, 0x93CA, 0x11d2,
    {0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C}
};

/* Device path node header */
typedef struct __attribute__((packed)) {
    uint8_t  Type;
    uint8_t  SubType;
    uint16_t Length;
} EFI_DP_NODE;

/* Hard Drive device path node (42 bytes) */
typedef struct __attribute__((packed)) {
    EFI_DP_NODE Header;        /* Type=4, SubType=1, Length=42 */
    uint32_t PartitionNumber;
    uint64_t PartitionStart;
    uint64_t PartitionSize;
    uint8_t  Signature[16];    /* Partition GUID (mixed endian) */
    uint8_t  MBRType;          /* 0x02 = GPT */
    uint8_t  SignatureType;    /* 0x02 = GUID */
} HD_DEVICE_PATH;

static EFI_STATUS create_boot_entry(void) {
    EFI_RUNTIME_SERVICES *gRT = gST->RuntimeServices;

    /* Build the file path string in UCS-2 */
    const char *fpath = "\\EFI\\BOOT\\BOOTX64.EFI";
    int fpath_len = 0;
    while (fpath[fpath_len]) fpath_len++;
    uint16_t fpath_ucs2[32];
    for (int i = 0; i <= fpath_len; i++)
        fpath_ucs2[i] = (uint16_t)(unsigned char)fpath[i];

    /* Device path:
     *   HD node (42 bytes) + File node (4 + (fpath_len+1)*2) + End node (4) */
    uint16_t file_node_len = (uint16_t)(4 + (fpath_len + 1) * 2);
    uint16_t dp_total = 42 + file_node_len + 4;

    /* Description "BootAI" in UCS-2 (7 uint16_t including null) */
    uint16_t desc[] = { 'B','o','o','t','A','I', 0 };
    uint16_t desc_bytes = 7 * 2;

    /* Total EFI_LOAD_OPTION size */
    UINTN opt_size = 4 + 2 + desc_bytes + dp_total;
    uint8_t *opt = 0;
    EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, opt_size, (void **)&opt);
    if (st != EFI_SUCCESS) return st;
    memset(opt, 0, opt_size);

    uint8_t *p = opt;

    /* Attributes: LOAD_OPTION_ACTIVE */
    *(uint32_t *)p = 0x00000001;
    p += 4;

    /* FilePathListLength */
    *(uint16_t *)p = dp_total;
    p += 2;

    /* Description */
    memcpy(p, desc, desc_bytes);
    p += desc_bytes;

    /* HD device path node — MBR signature matching.
     * Disk signature is zero (matching MBR offset 440). */
    HD_DEVICE_PATH *hd = (HD_DEVICE_PATH *)p;
    hd->Header.Type = 0x04;      /* Media */
    hd->Header.SubType = 0x01;   /* Hard Drive */
    hd->Header.Length = 42;
    hd->PartitionNumber = 1;
    hd->PartitionStart = ESP_START_LBA;
    hd->PartitionSize = ESP_SECTORS;
    memset(hd->Signature, 0, 16);
    hd->MBRType = 0x01;          /* MBR */
    hd->SignatureType = 0x01;    /* 32-bit MBR disk signature */
    p += 42;

    /* File path device path node */
    EFI_DP_NODE *fp = (EFI_DP_NODE *)p;
    fp->Type = 0x04;             /* Media */
    fp->SubType = 0x04;          /* File Path */
    fp->Length = file_node_len;
    memcpy(p + 4, fpath_ucs2, (fpath_len + 1) * 2);
    p += file_node_len;

    /* End device path node */
    EFI_DP_NODE *end = (EFI_DP_NODE *)p;
    end->Type = 0x7F;
    end->SubType = 0xFF;
    end->Length = 4;

    /* Write Boot0001 variable */
    uint16_t boot_name[] = { 'B','o','o','t','0','0','0','1', 0 };
    uint32_t nv_attrs = EFI_VARIABLE_NON_VOLATILE |
                        EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS;

    st = gRT->SetVariable(boot_name, &gEfiGlobalVariableGuid, nv_attrs, opt_size, opt);
    gBS->FreePool(opt);

    if (st != EFI_SUCCESS) {
        efi_print_ascii("        SetVariable Boot0001 failed: ");
        print_dec64((uint64_t)st);
        efi_print_ascii("\r\n");
        return st;
    }
    efi_print_ascii("        Created Boot0001 NVRAM entry\r\n");

    /* Update BootOrder: read existing, prepend 0x0001 */
    uint16_t boot_order_name[] = { 'B','o','o','t','O','r','d','e','r', 0 };
    uint8_t order_buf[256];
    UINTN order_size = sizeof(order_buf);
    uint32_t order_attrs = 0;

    st = gRT->GetVariable(boot_order_name, &gEfiGlobalVariableGuid,
                           &order_attrs, &order_size, order_buf);

    uint8_t new_order[258];
    *(uint16_t *)new_order = 0x0001;  /* Our entry first */
    UINTN new_size = 2;

    if (st == EFI_SUCCESS && order_size > 0) {
        /* Append existing entries, skip any existing 0x0001 */
        uint16_t *entries = (uint16_t *)order_buf;
        int n = (int)(order_size / 2);
        for (int i = 0; i < n && new_size < 256; i++) {
            if (entries[i] != 0x0001) {
                *(uint16_t *)(new_order + new_size) = entries[i];
                new_size += 2;
            }
        }
    }

    st = gRT->SetVariable(boot_order_name, &gEfiGlobalVariableGuid, nv_attrs, new_size, new_order);
    if (st != EFI_SUCCESS) {
        efi_print_ascii("        SetVariable BootOrder failed: ");
        print_dec64((uint64_t)st);
        efi_print_ascii("\r\n");
        return st;
    }
    efi_print_ascii("        Updated BootOrder (BootAI first)\r\n");

    return EFI_SUCCESS;
}

/* ================================================================== */
/* Main install orchestrator                                           */
/* ================================================================== */

void ba_install_to_hd(void) {
    efi_print_ascii("\r\n  [Install to Hard Drive]\r\n\r\n");

    /* Ensure USB filesystem is available for reading source files */
    if (efi_fs_init() != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: Cannot access USB filesystem\r\n");
        return;
    }

    /* Step 1: Find internal disk */
    efi_print_ascii("  Scanning for internal drives...\r\n");
    EFI_BLOCK_IO_PROTOCOL *disk = 0;
    EFI_HANDLE disk_handle = 0;
    if (find_internal_disk(&disk, &disk_handle) != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: No internal drive found\r\n");
        return;
    }

    uint64_t disk_bytes = (disk->Media->LastBlock + 1) * 512ULL;
    efi_print_ascii("\r\n  Found: ");
    print_size_human(disk_bytes);
    efi_print_ascii(" internal disk (MediaId: ");
    print_dec64(disk->Media->MediaId);
    efi_print_ascii(")\r\n\r\n");

    if (disk->Media->ReadOnly) {
        efi_print_ascii("  ERROR: Disk is read-only\r\n");
        return;
    }

    /* Need at least 200MB (100MB ESP + some raw data) */
    if (disk_bytes < 200ULL * 1024 * 1024) {
        efi_print_ascii("  ERROR: Disk too small (need at least 200 MB)\r\n");
        return;
    }

    /* Step 1: Read ALL files from USB into memory FIRST.
     * Don't rely on efi_fs_root() — Dell has multiple SimpleFileSystem
     * volumes (HP Port Rep, BOOTAI USB) and LocateProtocol may find
     * the wrong one. Enumerate all volumes and find ours. */
    efi_print_ascii("  [1/8] Reading files from USB...\r\n");

    EFI_STATUS st;
    EFI_FILE_PROTOCOL *usb_root = 0;
    EFI_HANDLE usb_fs_handle = 0;
    {
        UINTN n_fs = 0;
        EFI_HANDLE *fs_handles = 0;
        st = gBS->LocateHandleBuffer(
            2 /* ByProtocol */, &gEfiSimpleFileSystemProtocolGuid, 0,
            &n_fs, &fs_handles);
        if (st == EFI_SUCCESS) {
            efi_print_ascii("        Found ");
            print_dec64(n_fs);
            efi_print_ascii(" filesystem(s), probing...\r\n");

            for (UINTN fi = 0; fi < n_fs; fi++) {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
                st = gBS->HandleProtocol(fs_handles[fi],
                    &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
                if (st != EFI_SUCCESS || !fs) continue;

                EFI_FILE_PROTOCOL *vol = 0;
                st = fs->OpenVolume(fs, &vol);
                if (st != EFI_SUCCESS || !vol) continue;

                /* Probe: try opening our EFI binary */
                uint16_t probe_path[32];
                ascii_to_ucs2("\\EFI\\BOOT\\BOOTX64.EFI", probe_path, 32);
                EFI_FILE_PROTOCOL *probe = 0;
                st = vol->Open(vol, &probe, probe_path, EFI_FILE_MODE_READ, 0);
                if (st == EFI_SUCCESS && probe) {
                    probe->Close(probe);
                    usb_root = vol;
                    usb_fs_handle = fs_handles[fi];
                    efi_print_ascii("        Found BOOTAI volume (fs ");
                    print_dec64(fi);
                    efi_print_ascii(")\r\n");
                    break;
                }
                vol->Close(vol);
            }
            gBS->FreePool(fs_handles);
        }
    }

    preread_file pr_efi = {0}, pr_vocab = {0}, pr_wifi = {0};
    if (usb_root) {
        EFI_STATUS ps;
        ps = preread_usb_file(usb_root, "\\EFI\\BOOT\\BOOTX64.EFI", &pr_efi);
        efi_print_ascii("        BOOTX64.EFI: ");
        if (pr_efi.data) { print_size_human(pr_efi.size); }
        else { efi_print_ascii("not found ("); print_dec64((uint64_t)ps); efi_print_ascii(")"); }
        efi_print_ascii("\r\n");

        ps = preread_usb_file(usb_root, "\\rwkvos\\vocab.btv", &pr_vocab);
        efi_print_ascii("        vocab.btv:   ");
        if (pr_vocab.data) { print_size_human(pr_vocab.size); }
        else { efi_print_ascii("not found ("); print_dec64((uint64_t)ps); efi_print_ascii(")"); }
        efi_print_ascii("\r\n");

        ps = preread_usb_file(usb_root, "\\wifi.conf", &pr_wifi);
        efi_print_ascii("        wifi.conf:   ");
        if (pr_wifi.data) { print_size_human(pr_wifi.size); }
        else { efi_print_ascii("not found ("); print_dec64((uint64_t)ps); efi_print_ascii(")"); }
        efi_print_ascii("\r\n");
    } else {
        efi_print_ascii("        ERROR: USB root is NULL\r\n");
    }

    /* Also pre-read model for raw partition copy */
    preread_file pr_model = {0};
    if (usb_root) {
        preread_usb_file(usb_root, "\\rwkvos\\model.btw", &pr_model);
        efi_print_ascii("        model.btw:   ");
        if (pr_model.data) { print_size_human(pr_model.size); }
        else { efi_print_ascii("not found"); }
        efi_print_ascii("\r\n");
    }

    /* DUET detection: read USB boot sectors to check for DUET boot chain.
     * Find the USB whole-disk Block IO (any whole disk that isn't our HD target)
     * and read sector 0 (MBR). If first byte is 0xFA (CLI instruction), it's
     * a DUET USB with boot0 code in the MBR. */
    int is_duet = 0;
    uint8_t boot0_buf[446];
    uint8_t boot1f32_buf[512];
    memset(boot0_buf, 0, 446);
    memset(boot1f32_buf, 0, 512);
    preread_file pr_duet_fw = {0};

    {
        /* Find USB whole-disk Block IO */
        EFI_BLOCK_IO_PROTOCOL *usb_disk_bio = 0;
        EFI_BLOCK_IO_PROTOCOL *usb_part_bio = 0;
        UINTN n_bio = 0;
        EFI_HANDLE *bio_handles = 0;
        st = gBS->LocateHandleBuffer(
            2 /* ByProtocol */, &gEfiBlockIoProtocolGuid, 0, &n_bio, &bio_handles);
        if (st == EFI_SUCCESS) {
            for (UINTN i = 0; i < n_bio; i++) {
                if (bio_handles[i] == disk_handle) continue;
                EFI_BLOCK_IO_PROTOCOL *bio = 0;
                gBS->HandleProtocol(bio_handles[i], &gEfiBlockIoProtocolGuid, (void **)&bio);
                if (!bio || !bio->Media || !bio->Media->MediaPresent) continue;
                if (bio->Media->BlockSize != 512) continue;
                if (!bio->Media->LogicalPartition) {
                    if (!usb_disk_bio) usb_disk_bio = bio;
                }
            }
            gBS->FreePool(bio_handles);
        }

        /* Also get USB partition Block IO from the filesystem handle */
        if (usb_fs_handle) {
            gBS->HandleProtocol(usb_fs_handle, &gEfiBlockIoProtocolGuid,
                                 (void **)&usb_part_bio);
        }

        /* Read USB MBR (sector 0) for boot0 code */
        if (usb_disk_bio) {
            uint8_t *usb_mbr = 0;
            st = gBS->AllocatePool(EfiLoaderData, 512, (void **)&usb_mbr);
            if (st == EFI_SUCCESS) {
                st = usb_disk_bio->ReadBlocks(usb_disk_bio,
                    usb_disk_bio->Media->MediaId, 0, 512, usb_mbr);
                if (st == EFI_SUCCESS && usb_mbr[0] == 0xFA) {
                    is_duet = 1;
                    memcpy(boot0_buf, usb_mbr, 446);
                    efi_print_ascii("        DUET USB detected (boot0 at MBR)\r\n");
                }
                gBS->FreePool(usb_mbr);
            }
        }

        /* Read USB PBR (partition sector 0) for boot1f32 code */
        if (is_duet && usb_part_bio) {
            uint8_t *usb_pbr = 0;
            st = gBS->AllocatePool(EfiLoaderData, 512, (void **)&usb_pbr);
            if (st == EFI_SUCCESS) {
                st = usb_part_bio->ReadBlocks(usb_part_bio,
                    usb_part_bio->Media->MediaId, 0, 512, usb_pbr);
                if (st == EFI_SUCCESS) {
                    memcpy(boot1f32_buf, usb_pbr, 512);
                    efi_print_ascii("        boot1f32 PBR read OK\r\n");
                }
                gBS->FreePool(usb_pbr);
            }
        }

        /* Pre-read /boot (DUET firmware) from USB */
        if (is_duet && usb_root) {
            preread_usb_file(usb_root, "\\boot", &pr_duet_fw);
            efi_print_ascii("        boot (DUET): ");
            if (pr_duet_fw.data) { print_size_human(pr_duet_fw.size); }
            else { efi_print_ascii("not found"); is_duet = 0; }
            efi_print_ascii("\r\n");
        }
    }

    if (is_duet)
        efi_print_ascii("        DUET Legacy Boot will be installed\r\n");

    /* Confirmation prompt — this wipes the entire disk */
    efi_print_ascii("  WARNING: This will ERASE ALL DATA on the disk.\r\n");
    efi_print_ascii("  Type 'yes' to continue: ");
    {
        char confirm[16] = {0};
        efi_read_line(confirm, sizeof(confirm));
        efi_print_ascii("\r\n");
        if (!(confirm[0] == 'y' && confirm[1] == 'e' && confirm[2] == 's' &&
              (confirm[3] == '\0' || confirm[3] == '\r' || confirm[3] == '\n'))) {
            efi_print_ascii("  Aborted.\r\n");
            goto cleanup;
        }
    }

    /* Step 2: Write MBR partition table.
     * Dell E6510 UEFI firmware doesn't boot from GPT on internal disks.
     * MBR with type 0xEF (EFI System) works — same format as the USB.
     * If DUET: boot0 code injected into MBR, partition type = 0x0C (FAT32 LBA). */
    efi_print_ascii("  [2/8] Writing MBR partition table");
    if (is_duet) efi_print_ascii(" (with boot0)");
    efi_print_ascii("...");
    st = write_mbr(disk, is_duet ? boot0_buf : 0);
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED\r\n");
        goto cleanup;
    }
    efi_print_ascii(" OK\r\n");

    /* Step 3: Format ESP */
    efi_print_ascii("  [3/8] Formatting FAT32 (100 MB ESP)...");
    st = format_fat32(disk);
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED\r\n");
        goto cleanup;
    }
    efi_print_ascii(" OK\r\n");

    /* Step 4: Merge boot1f32 into PBR (DUET only) */
    if (is_duet) {
        efi_print_ascii("  [4/8] Merging boot1f32 into PBR...");
        st = merge_pbr_boot1f32(disk, boot1f32_buf);
        if (st != EFI_SUCCESS) {
            efi_print_ascii(" FAILED\r\n");
            goto cleanup;
        }
        efi_print_ascii(" OK\r\n");
    } else {
        efi_print_ascii("  [4/8] PBR merge skipped (non-DUET USB)\r\n");
    }

    /* Step 5: Copy model to raw partition */
    {
        uint64_t raw_end = disk->Media->LastBlock - 33; /* backup GPT entries */
        uint64_t raw_size = (raw_end - RAW_START_LBA + 1) * 512ULL;

        efi_print_ascii("  [5/8] Copying model to data partition (");
        print_size_human(raw_size);
        efi_print_ascii(")...\r\n");

        if (pr_model.data && pr_model.size > 0) {
            /* Write model from pre-read buffer */
            efi_print_ascii("        model.btw (");
            print_size_human(pr_model.size);
            efi_print_ascii(")\r\n");

            if (BTAI_DATA_OFFSET + pr_model.size > raw_size) {
                efi_print_ascii("        ERROR: Not enough space\r\n");
            } else {
                uint32_t media_id = disk->Media->MediaId;
                uint64_t data_lba = RAW_START_LBA + (BTAI_DATA_OFFSET / 512);
                uint8_t *src = (uint8_t *)pr_model.data;
                uint64_t written = 0;
                UINTN chunk_size = 1024 * 1024;
                uint8_t *chunk = 0;
                gBS->AllocatePool(EfiLoaderData, chunk_size, (void **)&chunk);

                while (written < pr_model.size && chunk) {
                    UINTN to_copy = chunk_size;
                    if (written + to_copy > pr_model.size)
                        to_copy = (UINTN)(pr_model.size - written);
                    UINTN aligned = (to_copy + 511) & ~(UINTN)511;
                    memset(chunk, 0, aligned);
                    memcpy(chunk, src + written, to_copy);
                    st = disk->WriteBlocks(disk, media_id, data_lba, aligned, chunk);
                    if (st != EFI_SUCCESS) break;
                    written += to_copy;
                    data_lba += aligned / 512;
                    print_progress(written, pr_model.size);
                }
                if (chunk) gBS->FreePool(chunk);

                if (written >= pr_model.size) {
                    /* Write TOC */
                    uint8_t *toc = 0;
                    gBS->AllocatePool(EfiLoaderData, BTAI_DATA_OFFSET, (void **)&toc);
                    if (toc) {
                        memset(toc, 0, BTAI_DATA_OFFSET);
                        btai_header *hdr = (btai_header *)toc;
                        hdr->magic = BTAI_MAGIC;
                        hdr->version = BTAI_VERSION;
                        hdr->entry_count = 1;
                        hdr->partition_size = raw_size;
                        hdr->data_offset = BTAI_DATA_OFFSET;
                        hdr->next_free = BTAI_DATA_OFFSET + pr_model.size;
                        btai_entry *ent = (btai_entry *)(toc + sizeof(btai_header));
                        const char *nm = "model.btw";
                        for (int i = 0; nm[i]; i++) ent->name[i] = nm[i];
                        ent->offset = BTAI_DATA_OFFSET;
                        ent->size = pr_model.size;
                        ent->flags = 0;
                        disk->WriteBlocks(disk, media_id, RAW_START_LBA, BTAI_DATA_OFFSET, toc);
                        gBS->FreePool(toc);
                    }
                    disk->FlushBlocks(disk);
                    efi_print_ascii("\r\n        OK\r\n");
                } else {
                    efi_print_ascii("\r\n        WRITE FAILED\r\n");
                }
            }
        } else {
            efi_print_ascii("        model.btw not found on USB -- skipped\r\n");
        }
    }

    /* Step 6: Write files directly to FAT32 via Block IO */
    efi_print_ascii("  [6/8] Writing files to ESP:\r\n");

    st = fat32_write_esp_files(disk, &pr_efi, &pr_vocab, &pr_wifi,
                                is_duet ? &pr_duet_fw : 0);
    if (st != EFI_SUCCESS) {
        efi_print_ascii("        FAT32 write FAILED\r\n");
        goto cleanup;
    }
    /* Step 7: Verify data integrity (CRC32 hash comparison USB vs HD) */
    efi_print_ascii("  [7/8] Verifying data integrity (CRC32)...\r\n");
    {
        fat32_state vf;
        fat32_init(&vf, disk);
        uint32_t mid = disk->Media->MediaId;

        /* Track cluster offsets: dirs use 3,4,5,6 (+ 7 for OC/ if DUET) */
        uint32_t next_file_cluster = is_duet ? 8 : 7;

        /* Helper: verify a FAT32 file, re-write via raw Block IO + re-verify on mismatch */
        #define VERIFY_FAT32_FILE(label, src_data, src_size, file_cluster)       \
        do {                                                                     \
            uint32_t _usb_crc = crc32(src_data, (int)(src_size));                \
            uint32_t _n_cl = (uint32_t)(((src_size) + vf.cluster_bytes - 1) / vf.cluster_bytes); \
            UINTN _rd_total = (UINTN)_n_cl * vf.cluster_bytes;                  \
            uint64_t _phys = 0;                                                  \
            UINTN _pages = (_rd_total + 4095) / 4096;                            \
            efi_print_ascii("        " label "  USB: ");                         \
            print_hex32(_usb_crc);                                               \
            efi_print_ascii("\r\n");                                             \
            st = gBS->AllocatePages(0, EfiLoaderData, _pages, &_phys);           \
            if (st == EFI_SUCCESS) {                                             \
                uint8_t *_rb = (uint8_t *)(UINTN)_phys;                          \
                for (int _attempt = 0; _attempt < 2; _attempt++) {               \
                    disk->FlushBlocks(disk);                                      \
                    st = disk->ReadBlocks(disk, mid,                              \
                                           cluster_lba(&vf, file_cluster),       \
                                           _rd_total, _rb);                      \
                    uint32_t _hd_crc = (st == EFI_SUCCESS) ?                     \
                        crc32(_rb, (int)(src_size)) : 0;                         \
                    if (st == EFI_SUCCESS && _usb_crc == _hd_crc) {              \
                        efi_print_ascii("        " label "   HD: ");             \
                        print_hex32(_hd_crc);                                    \
                        efi_print_ascii(" MATCH\r\n");                           \
                        break;                                                   \
                    }                                                            \
                    if (_attempt == 0) {                                          \
                        if (st == EFI_SUCCESS) {                                  \
                            efi_print_ascii("        " label "   HD: ");         \
                            print_hex32(_hd_crc);                                \
                            efi_print_ascii(" MISMATCH -> rewriting...");        \
                        } else {                                                 \
                            efi_print_ascii("        " label "   HD: read fail -> rewriting..."); \
                        }                                                        \
                        /* Re-write via raw Block IO (contiguous clusters) */    \
                        uint64_t _wr_lba = cluster_lba(&vf, file_cluster);       \
                        const uint8_t *_src = (const uint8_t *)(src_data);       \
                        UINTN _rem = (src_size);                                 \
                        while (_rem > 0) {                                       \
                            UINTN _chunk = _rem < vf.cluster_bytes ? _rem : vf.cluster_bytes; \
                            memset(_rb, 0, vf.cluster_bytes);                    \
                            memcpy(_rb, _src, _chunk);                           \
                            disk->WriteBlocks(disk, mid, _wr_lba,                \
                                               vf.cluster_bytes, _rb);           \
                            _wr_lba += vf.cluster_bytes / 512;                   \
                            _src += _chunk;                                      \
                            _rem -= _chunk;                                      \
                        }                                                        \
                        disk->FlushBlocks(disk);                                 \
                    } else {                                                     \
                        efi_print_ascii("        " label "   HD: STILL MISMATCHED!\r\n"); \
                    }                                                            \
                }                                                                \
                gBS->FreePages(_phys, _pages);                                   \
            }                                                                    \
        } while (0)

        /* --- BOOTX64.EFI --- */
        if (pr_efi.data && pr_efi.size > 0) {
            VERIFY_FAT32_FILE("BOOTX64.EFI", pr_efi.data, pr_efi.size, next_file_cluster);
            next_file_cluster += (uint32_t)((pr_efi.size + vf.cluster_bytes - 1) / vf.cluster_bytes);
        }

        /* --- vocab.btv --- */
        if (pr_vocab.data && pr_vocab.size > 0) {
            VERIFY_FAT32_FILE("vocab.btv  ", pr_vocab.data, pr_vocab.size, next_file_cluster);
            uint32_t n_clusters = (uint32_t)((pr_vocab.size + vf.cluster_bytes - 1) / vf.cluster_bytes);
            next_file_cluster += n_clusters;
        }

        /* --- wifi.conf --- */
        if (pr_wifi.data && pr_wifi.size > 0) {
            VERIFY_FAT32_FILE("wifi.conf  ", pr_wifi.data, pr_wifi.size, next_file_cluster);
            uint32_t n_clusters = (uint32_t)((pr_wifi.size + vf.cluster_bytes - 1) / vf.cluster_bytes);
            next_file_cluster += n_clusters;
        }

        /* --- boot (DUET firmware, FAT32) --- */
        if (is_duet && pr_duet_fw.data && pr_duet_fw.size > 0) {
            VERIFY_FAT32_FILE("boot       ", pr_duet_fw.data, pr_duet_fw.size, next_file_cluster);
            uint32_t n_clusters = (uint32_t)((pr_duet_fw.size + vf.cluster_bytes - 1) / vf.cluster_bytes);
            next_file_cluster += n_clusters;
        }

        /* --- OpenCore.efi (copy of EFI binary in EFI/OC/, DUET only) --- */
        if (is_duet && pr_efi.data && pr_efi.size > 0) {
            VERIFY_FAT32_FILE("OpenCore.efi", pr_efi.data, pr_efi.size, next_file_cluster);
            uint32_t n_clusters = (uint32_t)((pr_efi.size + vf.cluster_bytes - 1) / vf.cluster_bytes);
            next_file_cluster += n_clusters;
        }

        #undef VERIFY_FAT32_FILE

        /* --- model.btw (raw partition, read in 1MB chunks) --- */
        if (pr_model.data && pr_model.size > 0) {
            uint32_t usb_crc = crc32(pr_model.data, (int)pr_model.size);
            efi_print_ascii("        model.btw    USB: ");
            print_hex32(usb_crc);
            efi_print_ascii("\r\n");

            uint64_t data_lba = RAW_START_LBA + (BTAI_DATA_OFFSET / 512);
            UINTN chunk = 1024 * 1024;
            uint8_t *cbuf = 0;
            gBS->AllocatePool(EfiLoaderData, chunk, (void **)&cbuf);
            if (cbuf) {
                for (int attempt = 0; attempt < 2; attempt++) {
                    uint32_t hd_crc = 0xFFFFFFFF;
                    uint64_t remaining = pr_model.size;
                    uint64_t cur_lba = data_lba;
                    int read_ok = 1;
                    while (remaining > 0) {
                        UINTN to_read = (remaining < chunk) ? (UINTN)remaining : chunk;
                        UINTN aligned = (to_read + 511) & ~(UINTN)511;
                        st = disk->ReadBlocks(disk, mid, cur_lba, aligned, cbuf);
                        if (st != EFI_SUCCESS) { read_ok = 0; break; }
                        const uint8_t *cp = cbuf;
                        for (UINTN ci = 0; ci < to_read; ci++)
                            hd_crc = (hd_crc >> 8) ^ crc32_table[(hd_crc ^ cp[ci]) & 0xFF];
                        cur_lba += aligned / 512;
                        remaining -= to_read;
                    }
                    hd_crc ^= 0xFFFFFFFF;
                    if (read_ok && usb_crc == hd_crc) {
                        efi_print_ascii("        model.btw     HD: ");
                        print_hex32(hd_crc);
                        efi_print_ascii(" MATCH\r\n");
                        break;
                    }
                    if (attempt == 0) {
                        if (read_ok) {
                            efi_print_ascii("        model.btw     HD: ");
                            print_hex32(hd_crc);
                            efi_print_ascii(" MISMATCH -> rewriting...\r\n");
                        } else {
                            efi_print_ascii("        model.btw     HD: read failed -> rewriting...\r\n");
                        }
                        /* Re-write model in 1MB chunks */
                        uint64_t wr_remaining = pr_model.size;
                        uint64_t wr_lba = data_lba;
                        const uint8_t *src = pr_model.data;
                        while (wr_remaining > 0) {
                            UINTN to_write = (wr_remaining < chunk) ? (UINTN)wr_remaining : chunk;
                            UINTN aligned = (to_write + 511) & ~(UINTN)511;
                            memcpy(cbuf, src, to_write);
                            disk->WriteBlocks(disk, mid, wr_lba, aligned, cbuf);
                            wr_lba += aligned / 512;
                            src += to_write;
                            wr_remaining -= to_write;
                        }
                        disk->FlushBlocks(disk);
                    } else {
                        efi_print_ascii("        model.btw     HD: STILL MISMATCHED!\r\n");
                    }
                }
                gBS->FreePool(cbuf);
            }
        }

        /* Try ConnectController to see if firmware can mount the partition */
        efi_print_ascii("        Reconnecting disk...\r\n");
        gBS->DisconnectController(disk_handle, 0, 0);
        gBS->ConnectController(disk_handle, 0, 0, 1);
        gBS->Stall(2000000);  /* 2 seconds */

        /* Check if firmware created SimpleFileSystem on the partition */
        UINTN n_fs2 = 0;
        EFI_HANDLE *fs2 = 0;
        gBS->LocateHandleBuffer(2, &gEfiSimpleFileSystemProtocolGuid, 0, &n_fs2, &fs2);
        efi_print_ascii("        SimpleFileSystem handles after reconnect: ");
        print_dec64(n_fs2);
        efi_print_ascii("\r\n");
        if (fs2) gBS->FreePool(fs2);
    }

    /* Step 8: Create UEFI boot entry in NVRAM.
     * DUET's BDS requires a Boot#### NVRAM entry to find the boot partition.
     * The boot entry uses MBR disk signature 0xB007A1DA (written to MBR offset
     * 440 in step 2) so it matches only the HD, not the USB (sig 0x00000000). */
    efi_print_ascii("  [8/8] Creating boot entry...\r\n");
    create_boot_entry();

cleanup:
    preread_free(&pr_efi);
    preread_free(&pr_vocab);
    preread_free(&pr_wifi);
    preread_free(&pr_model);
    preread_free(&pr_duet_fw);
    if (st != EFI_SUCCESS) return;

    /* Summary */
    efi_print_ascii("\r\n  Installation complete!\r\n");
    efi_print_ascii("  Remove USB drive and reboot to boot from hard drive.\r\n\r\n");

}

/* ================================================================== */
/* HD Inspector — read and dump HD contents via Block IO               */
/* ================================================================== */

static void print_guid(const EFI_GUID *g) {
    print_hex32(g->Data1);
    efi_print_ascii("-");
    /* Data2, Data3 as 16-bit */
    print_hex8((uint8_t)(g->Data2 >> 8));
    print_hex8((uint8_t)(g->Data2));
    efi_print_ascii("-");
    print_hex8((uint8_t)(g->Data3 >> 8));
    print_hex8((uint8_t)(g->Data3));
    efi_print_ascii("-");
    print_hex8(g->Data4[0]);
    print_hex8(g->Data4[1]);
    efi_print_ascii("-");
    for (int i = 2; i < 8; i++) print_hex8(g->Data4[i]);
}

void ba_hdinfo(void) {
    efi_print_ascii("\r\n  [HD Inspector]\r\n\r\n");

    /* Find internal disk */
    EFI_BLOCK_IO_PROTOCOL *disk = 0;
    EFI_HANDLE disk_handle = 0;
    if (find_internal_disk(&disk, &disk_handle) != EFI_SUCCESS) {
        efi_print_ascii("  ERROR: No internal drive found\r\n");
        return;
    }

    uint64_t disk_bytes = (disk->Media->LastBlock + 1) * 512ULL;
    uint32_t media_id = disk->Media->MediaId;
    efi_print_ascii("  Disk: ");
    print_size_human(disk_bytes);
    efi_print_ascii(" (MediaId ");
    print_dec64(media_id);
    efi_print_ascii(", LastBlock ");
    print_dec64(disk->Media->LastBlock);
    efi_print_ascii(")\r\n\r\n");

    /* Read LBA 0 (MBR) */
    uint8_t *sector = 0;
    EFI_STATUS st = gBS->AllocatePool(EfiLoaderData, 512, (void **)&sector);
    if (st != EFI_SUCCESS) { efi_print_ascii("  AllocatePool failed\r\n"); return; }

    st = disk->ReadBlocks(disk, media_id, 0, 512, sector);
    if (st != EFI_SUCCESS) {
        efi_print_ascii("  Read LBA 0 failed\r\n");
        gBS->FreePool(sector);
        return;
    }
    efi_print_ascii("  === MBR (LBA 0) ===\r\n");
    efi_print_ascii("  Boot signature: ");
    print_hex8(sector[510]); print_hex8(sector[511]);
    efi_print_ascii(sector[510] == 0x55 && sector[511] == 0xAA ? " (valid)\r\n" : " (INVALID)\r\n");
    efi_print_ascii("  Disk ID: ");
    print_hex32(*(uint32_t *)&sector[440]);
    efi_print_ascii("\r\n");

    /* Show MBR partition entries (4 entries at offsets 446, 462, 478, 494) */
    efi_print_ascii("\r\n  === MBR Partitions ===\r\n");
    for (int pi = 0; pi < 4; pi++) {
        uint8_t *pe = &sector[446 + pi * 16];
        uint8_t type = pe[4];
        if (type == 0) continue;  /* empty entry */

        uint32_t lba_start = *(uint32_t *)&pe[8];
        uint32_t lba_count = *(uint32_t *)&pe[12];

        efi_print_ascii("  [");
        char pnum[2] = { '1' + pi, 0 };
        efi_print_ascii(pnum);
        efi_print_ascii("] Type: ");
        print_hex8(type);
        if (type == 0xEF) efi_print_ascii(" (EFI System)");
        else if (type == 0x0C) efi_print_ascii(" (FAT32 LBA)");
        else if (type == 0xDA) efi_print_ascii(" (BootAI Data)");
        else if (type == 0xEE) efi_print_ascii(" (GPT protective)");
        efi_print_ascii(pe[0] == 0x80 ? "  *BOOTABLE*" : "");
        efi_print_ascii("\r\n      LBA ");
        print_dec64(lba_start);
        efi_print_ascii(" - ");
        print_dec64(lba_start + lba_count - 1);
        efi_print_ascii(" (");
        print_size_human((uint64_t)lba_count * 512);
        efi_print_ascii(")\r\n");
    }

    /* Read FAT32 BPB from ESP (LBA 2048) */
    efi_print_ascii("\r\n  === FAT32 BPB (ESP at LBA ");
    print_dec64(ESP_START_LBA);
    efi_print_ascii(") ===\r\n");
    st = disk->ReadBlocks(disk, media_id, ESP_START_LBA, 512, sector);
    if (st == EFI_SUCCESS) {
        efi_print_ascii("  OEM: ");
        for (int i = 3; i < 11; i++) {
            char c[2] = { (char)sector[i], 0 };
            efi_print_ascii(c);
        }
        efi_print_ascii("\r\n  BytsPerSec: ");
        print_dec64(*(uint16_t *)&sector[11]);
        efi_print_ascii("\r\n  SecPerClus: ");
        print_dec64(sector[13]);
        efi_print_ascii("\r\n  RsvdSecCnt: ");
        print_dec64(*(uint16_t *)&sector[14]);
        efi_print_ascii("\r\n  NumFATs:    ");
        print_dec64(sector[16]);
        efi_print_ascii("\r\n  TotSec32:   ");
        print_dec64(*(uint32_t *)&sector[32]);
        efi_print_ascii("\r\n  FATSz32:    ");
        print_dec64(*(uint32_t *)&sector[36]);
        efi_print_ascii("\r\n  RootClus:   ");
        print_dec64(*(uint32_t *)&sector[44]);
        efi_print_ascii("\r\n  Signature:  ");
        print_hex8(sector[510]); print_hex8(sector[511]);
        efi_print_ascii(sector[510] == 0x55 && sector[511] == 0xAA ? " (valid)" : " (INVALID)");
        efi_print_ascii("\r\n  VolLabel:   ");
        for (int i = 71; i < 82; i++) {
            char c[2] = { (char)sector[i], 0 };
            efi_print_ascii(c);
        }
        efi_print_ascii("\r\n  FSType:     ");
        for (int i = 82; i < 90; i++) {
            char c[2] = { (char)sector[i], 0 };
            efi_print_ascii(c);
        }
        efi_print_ascii("\r\n");

        /* Compute data region and read root directory */
        uint32_t spc = sector[13];
        uint32_t reserved = *(uint16_t *)&sector[14];
        uint32_t fat_size = *(uint32_t *)&sector[36];
        uint32_t num_fats = sector[16];
        uint64_t data_lba = ESP_START_LBA + reserved + num_fats * fat_size;

        efi_print_ascii("\r\n  === Root Directory (cluster 2 at LBA ");
        print_dec64(data_lba);
        efi_print_ascii(") ===\r\n");

        uint32_t cluster_bytes = spc * 512;
        uint8_t *root = 0;
        st = gBS->AllocatePool(EfiLoaderData, cluster_bytes, (void **)&root);
        if (st == EFI_SUCCESS) {
            st = disk->ReadBlocks(disk, media_id, data_lba, cluster_bytes, root);
            if (st == EFI_SUCCESS) {
                for (uint32_t i = 0; i < cluster_bytes / 32; i++) {
                    uint8_t *e = &root[i * 32];
                    if (e[0] == 0x00) break;      /* end of dir */
                    if (e[0] == 0xE5) continue;    /* deleted */
                    if (e[11] == 0x0F) continue;   /* LFN entry */

                    efi_print_ascii("  ");
                    /* Print 8.3 name */
                    for (int j = 0; j < 8; j++) {
                        char c[2] = { (char)e[j], 0 };
                        efi_print_ascii(c);
                    }
                    efi_print_ascii(".");
                    for (int j = 8; j < 11; j++) {
                        char c[2] = { (char)e[j], 0 };
                        efi_print_ascii(c);
                    }

                    uint32_t clust = ((uint32_t)e[20] << 16) | ((uint32_t)e[21] << 24) |
                                      (uint32_t)e[26] | ((uint32_t)e[27] << 8);
                    uint32_t fsize = *(uint32_t *)&e[28];

                    if (e[11] & 0x10) {
                        efi_print_ascii("  <DIR>");
                    } else {
                        efi_print_ascii("  ");
                        print_size_human(fsize);
                    }
                    efi_print_ascii("  (cluster ");
                    print_dec64(clust);
                    efi_print_ascii(")\r\n");

                    /* If it's a directory, list its contents too */
                    if ((e[11] & 0x10) && clust >= 2 && e[0] != '.') {
                        uint64_t sub_lba = data_lba + (uint64_t)(clust - 2) * spc;
                        uint8_t *sub = 0;
                        gBS->AllocatePool(EfiLoaderData, cluster_bytes, (void **)&sub);
                        if (sub) {
                            st = disk->ReadBlocks(disk, media_id, sub_lba, cluster_bytes, sub);
                            if (st == EFI_SUCCESS) {
                                for (uint32_t j = 0; j < cluster_bytes / 32; j++) {
                                    uint8_t *se = &sub[j * 32];
                                    if (se[0] == 0x00) break;
                                    if (se[0] == 0xE5) continue;
                                    if (se[11] == 0x0F) continue;

                                    efi_print_ascii("    ");
                                    for (int k = 0; k < 8; k++) {
                                        char c[2] = { (char)se[k], 0 };
                                        efi_print_ascii(c);
                                    }
                                    efi_print_ascii(".");
                                    for (int k = 8; k < 11; k++) {
                                        char c[2] = { (char)se[k], 0 };
                                        efi_print_ascii(c);
                                    }

                                    uint32_t sc = ((uint32_t)se[20] << 16) | ((uint32_t)se[21] << 24) |
                                                   (uint32_t)se[26] | ((uint32_t)se[27] << 8);
                                    uint32_t ss = *(uint32_t *)&se[28];
                                    if (se[11] & 0x10) {
                                        efi_print_ascii("  <DIR>");
                                    } else {
                                        efi_print_ascii("  ");
                                        print_size_human(ss);
                                    }
                                    efi_print_ascii("  (cluster ");
                                    print_dec64(sc);
                                    efi_print_ascii(")\r\n");

                                    /* 3rd level: list sub-subdirectory */
                                    if ((se[11] & 0x10) && sc >= 2 && se[0] != '.') {
                                        uint64_t sub2_lba = data_lba + (uint64_t)(sc - 2) * spc;
                                        uint8_t *sub2 = 0;
                                        gBS->AllocatePool(EfiLoaderData, cluster_bytes, (void **)&sub2);
                                        if (sub2) {
                                            disk->ReadBlocks(disk, media_id, sub2_lba, cluster_bytes, sub2);
                                            for (uint32_t m = 0; m < cluster_bytes / 32; m++) {
                                                uint8_t *te = &sub2[m * 32];
                                                if (te[0] == 0x00) break;
                                                if (te[0] == 0xE5) continue;
                                                if (te[11] == 0x0F) continue;
                                                efi_print_ascii("      ");
                                                for (int k = 0; k < 8; k++) {
                                                    char c[2] = { (char)te[k], 0 };
                                                    efi_print_ascii(c);
                                                }
                                                efi_print_ascii(".");
                                                for (int k = 8; k < 11; k++) {
                                                    char c[2] = { (char)te[k], 0 };
                                                    efi_print_ascii(c);
                                                }
                                                uint32_t tc = ((uint32_t)te[20] << 16) | ((uint32_t)te[21] << 24) |
                                                               (uint32_t)te[26] | ((uint32_t)te[27] << 8);
                                                uint32_t ts = *(uint32_t *)&te[28];
                                                if (te[11] & 0x10) {
                                                    efi_print_ascii("  <DIR>");
                                                } else {
                                                    efi_print_ascii("  ");
                                                    print_size_human(ts);
                                                }
                                                efi_print_ascii("  (cluster ");
                                                print_dec64(tc);
                                                efi_print_ascii(")\r\n");
                                            }
                                            gBS->FreePool(sub2);
                                        }
                                    }
                                }
                            }
                            gBS->FreePool(sub);
                        }
                    }
                }
            } else {
                efi_print_ascii("  Read root dir failed\r\n");
            }
            gBS->FreePool(root);
        }
    } else {
        efi_print_ascii("  Read ESP BPB failed\r\n");
    }

    /* Read BTAI TOC from raw partition */
    efi_print_ascii("\r\n  === BTAI Raw Partition (LBA ");
    print_dec64(RAW_START_LBA);
    efi_print_ascii(") ===\r\n");
    uint8_t *toc_buf = 0;
    st = gBS->AllocatePool(EfiLoaderData, 4096, (void **)&toc_buf);
    if (st == EFI_SUCCESS) {
        st = disk->ReadBlocks(disk, media_id, RAW_START_LBA, 4096, toc_buf);
        if (st == EFI_SUCCESS) {
            btai_header *bh = (btai_header *)toc_buf;
            efi_print_ascii("  Magic: ");
            print_hex32(bh->magic);
            if (bh->magic == BTAI_MAGIC) {
                efi_print_ascii(" (BTAI valid)\r\n");
                efi_print_ascii("  Version: ");
                print_dec64(bh->version);
                efi_print_ascii("\r\n  Entries: ");
                print_dec64(bh->entry_count);
                efi_print_ascii("\r\n  Partition size: ");
                print_size_human(bh->partition_size);
                efi_print_ascii("\r\n  Data offset: ");
                print_dec64(bh->data_offset);
                efi_print_ascii("\r\n  Next free: ");
                print_dec64(bh->next_free);
                efi_print_ascii("\r\n");

                btai_entry *ents = (btai_entry *)(toc_buf + sizeof(btai_header));
                for (uint32_t i = 0; i < bh->entry_count && i < BTAI_MAX_ENTRIES; i++) {
                    efi_print_ascii("  [");
                    print_dec64(i);
                    efi_print_ascii("] \"");
                    efi_print_ascii(ents[i].name);
                    efi_print_ascii("\" offset=");
                    print_dec64(ents[i].offset);
                    efi_print_ascii(" size=");
                    print_size_human(ents[i].size);
                    efi_print_ascii(" flags=");
                    print_dec64(ents[i].flags);
                    efi_print_ascii("\r\n");
                }
            } else {
                efi_print_ascii(" (NOT BTAI)\r\n");
            }
        }
        gBS->FreePool(toc_buf);
    }

    gBS->FreePool(sector);
    efi_print_ascii("\r\n");
}

/* ================================================================== */
/* Boot path: Read entry from raw data partition                       */
/*                                                                     */
/* Scans all Block IO handles for a non-removable logical partition    */
/* whose first sector contains the BTAI magic. Reads the TOC,          */
/* finds the named entry, allocates memory, and reads the data.        */
/* ================================================================== */

EFI_STATUS ba_raw_read_entry(const char *name, void **buffer, UINTN *size) {
    UINTN n_handles = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS status;

    status = gBS->LocateHandleBuffer(
        2 /* ByProtocol */, &gEfiBlockIoProtocolGuid, 0, &n_handles, &handles);
    if (status != EFI_SUCCESS || n_handles == 0)
        return EFI_NOT_FOUND;

    /* Allocate a sector buffer for probing */
    uint8_t *probe = 0;
    status = gBS->AllocatePool(EfiLoaderData, 4096, (void **)&probe);
    if (status != EFI_SUCCESS) { gBS->FreePool(handles); return status; }

    EFI_BLOCK_IO_PROTOCOL *raw_bio = 0;

    for (UINTN i = 0; i < n_handles; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        status = gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid,
                                      (void **)&bio);
        if (status != EFI_SUCCESS || !bio) continue;
        if (!bio->Media || !bio->Media->MediaPresent) continue;
        if (!bio->Media->LogicalPartition) continue;

        /* Read first 8 sectors (4KB TOC) */
        status = bio->ReadBlocks(bio, bio->Media->MediaId, 0, 4096, probe);
        if (status != EFI_SUCCESS) continue;

        if (*(uint32_t *)probe == BTAI_MAGIC) {
            raw_bio = bio;
            break;
        }
    }

    gBS->FreePool(handles);

    if (!raw_bio) {
        gBS->FreePool(probe);
        return EFI_NOT_FOUND;
    }

    /* Parse TOC — probe already contains it */
    btai_header *hdr = (btai_header *)probe;
    btai_entry *entries = (btai_entry *)(probe + sizeof(btai_header));

    /* Find entry by name */
    btai_entry *found = 0;
    for (uint32_t i = 0; i < hdr->entry_count && i < BTAI_MAX_ENTRIES; i++) {
        if (entries[i].flags != 0) continue;

        /* String compare */
        const char *a = name;
        const char *b = entries[i].name;
        int match = 1;
        while (*a || *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match) { found = &entries[i]; break; }
    }

    if (!found) {
        gBS->FreePool(probe);
        return EFI_NOT_FOUND;
    }

    uint64_t data_offset = found->offset;
    uint64_t data_size = found->size;
    gBS->FreePool(probe);

    /* Allocate buffer */
    UINTN pages = ((UINTN)data_size + 4095) / 4096;
    if (pages == 0) pages = 1;
    uint64_t phys = 0;
    status = gBS->AllocatePages(0, EfiLoaderData, pages, &phys);
    if (status != EFI_SUCCESS) return status;

    /* Read data in 1MB chunks */
    uint8_t *dst = (uint8_t *)(UINTN)phys;
    uint64_t lba = data_offset / 512;
    uint64_t remaining = data_size;
    UINTN chunk = 1024 * 1024;

    efi_print_ascii("  Reading ");
    print_size_human(data_size);
    efi_print_ascii(" from raw partition...");

    while (remaining > 0) {
        UINTN to_read = (remaining < chunk) ? (UINTN)remaining : chunk;
        UINTN aligned = (to_read + 511) & ~(UINTN)511;

        status = raw_bio->ReadBlocks(raw_bio, raw_bio->Media->MediaId,
                                      lba, aligned, dst);
        if (status != EFI_SUCCESS) {
            efi_print_ascii(" READ FAILED\r\n");
            gBS->FreePages(phys, pages);
            return status;
        }

        dst += to_read;
        lba += aligned / 512;
        remaining -= to_read;
    }

    efi_print_ascii(" done\r\n");

    *buffer = (void *)(UINTN)phys;
    *size = (UINTN)data_size;
    return EFI_SUCCESS;
}
