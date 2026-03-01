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

static EFI_GUID gEfiFileInfoGuid_install = EFI_FILE_INFO_GUID;

/* ================================================================== */
/* Find internal (non-removable) disk                                 */
/* ================================================================== */

static EFI_STATUS find_internal_disk(EFI_BLOCK_IO_PROTOCOL **disk_out,
                                      EFI_HANDLE *handle_out) {
    UINTN n_handles = 0;
    EFI_HANDLE *handles = 0;

    EFI_STATUS status = gBS->LocateHandleBuffer(
        2 /* ByProtocol */, &gEfiBlockIoProtocolGuid, 0, &n_handles, &handles);
    if (status != EFI_SUCCESS || n_handles == 0)
        return EFI_NOT_FOUND;

    for (UINTN i = 0; i < n_handles; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        status = gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid, (void **)&bio);
        if (status != EFI_SUCCESS || !bio) continue;
        if (!bio->Media) continue;
        if (!bio->Media->MediaPresent) continue;
        if (bio->Media->RemovableMedia) continue;
        if (bio->Media->LogicalPartition) continue;
        if (bio->Media->BlockSize != 512) continue;

        *disk_out = bio;
        *handle_out = handles[i];
        gBS->FreePool(handles);
        return EFI_SUCCESS;
    }

    gBS->FreePool(handles);
    return EFI_NOT_FOUND;
}

/* ================================================================== */
/* Write protective MBR (LBA 0)                                       */
/* ================================================================== */

static EFI_STATUS write_protective_mbr(EFI_BLOCK_IO_PROTOCOL *disk) {
    uint8_t *mbr = 0;
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, 512, (void **)&mbr);
    if (status != EFI_SUCCESS) return status;
    memset(mbr, 0, 512);

    /* Partition entry 1 at offset 446 */
    uint8_t *pe = &mbr[446];
    pe[0] = 0x00;                                       /* Not bootable */
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;          /* CHS start */
    pe[4] = 0xEE;                                       /* GPT protective */
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;          /* CHS end */
    pe[8] = 1; pe[9] = 0; pe[10] = 0; pe[11] = 0;     /* LBA start = 1 */

    uint64_t last = disk->Media->LastBlock;
    uint32_t sz = (last > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)last;
    pe[12] = (uint8_t)(sz);
    pe[13] = (uint8_t)(sz >> 8);
    pe[14] = (uint8_t)(sz >> 16);
    pe[15] = (uint8_t)(sz >> 24);

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
    uint32_t spc = 8;          /* 4KB clusters */
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
    *(uint32_t *)&bpb[28] = (uint32_t)ESP_START_LBA;       /* HiddSec */
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
/* Write raw partition TOC + copy model data                           */
/* ================================================================== */

static EFI_STATUS copy_model_to_raw(EFI_BLOCK_IO_PROTOCOL *disk,
                                     uint64_t raw_start_lba,
                                     uint64_t raw_size_bytes) {
    EFI_FILE_PROTOCOL *usb_root = efi_fs_root();
    if (!usb_root) return EFI_NOT_FOUND;

    /* Open model file on USB */
    uint16_t wpath[32];
    ascii_to_ucs2("rwkvos\\model.btw", wpath, 32);

    EFI_FILE_PROTOCOL *src = 0;
    EFI_STATUS status = usb_root->Open(usb_root, &src, wpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) return EFI_NOT_FOUND;

    /* Get file size */
    uint8_t info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = src->GetInfo(src, &gEfiFileInfoGuid_install, &info_size, info_buf);
    if (status != EFI_SUCCESS) { src->Close(src); return status; }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    uint64_t model_size = info->FileSize;

    efi_print_ascii("        model.btw (");
    print_size_human(model_size);
    efi_print_ascii(")\r\n");

    /* Check space */
    if (BTAI_DATA_OFFSET + model_size > raw_size_bytes) {
        efi_print_ascii("        ERROR: Not enough space. Need ");
        print_size_human(BTAI_DATA_OFFSET + model_size);
        efi_print_ascii(", have ");
        print_size_human(raw_size_bytes);
        efi_print_ascii("\r\n");
        src->Close(src);
        return EFI_VOLUME_FULL;
    }

    /* Allocate 1MB chunk buffer */
    UINTN chunk_size = 1024 * 1024;
    uint8_t *chunk = 0;
    status = gBS->AllocatePool(EfiLoaderData, chunk_size, (void **)&chunk);
    if (status != EFI_SUCCESS) { src->Close(src); return status; }

    /* Copy model data to raw partition at BTAI_DATA_OFFSET */
    uint32_t media_id = disk->Media->MediaId;
    uint64_t data_lba = raw_start_lba + (BTAI_DATA_OFFSET / 512);
    uint64_t written = 0;

    while (written < model_size) {
        UINTN to_read = chunk_size;
        if (written + to_read > model_size)
            to_read = (UINTN)(model_size - written);

        status = src->Read(src, &to_read, chunk);
        if (status != EFI_SUCCESS || to_read == 0) break;

        /* Pad to sector boundary */
        UINTN aligned = (to_read + 511) & ~(UINTN)511;
        if (to_read < aligned)
            memset(chunk + to_read, 0, aligned - to_read);

        status = disk->WriteBlocks(disk, media_id, data_lba, aligned, chunk);
        if (status != EFI_SUCCESS) {
            efi_print_ascii("\r\n        Write failed at LBA ");
            print_dec64(data_lba);
            efi_print_ascii("\r\n");
            break;
        }

        written += to_read;
        data_lba += aligned / 512;
        print_progress(written, model_size);
    }

    src->Close(src);
    gBS->FreePool(chunk);

    if (written < model_size)
        return EFI_DEVICE_ERROR;

    /* Write TOC header with model entry */
    uint8_t *toc = 0;
    status = gBS->AllocatePool(EfiLoaderData, BTAI_DATA_OFFSET, (void **)&toc);
    if (status != EFI_SUCCESS) return status;
    memset(toc, 0, BTAI_DATA_OFFSET);

    btai_header *hdr = (btai_header *)toc;
    hdr->magic = BTAI_MAGIC;
    hdr->version = BTAI_VERSION;
    hdr->entry_count = 1;
    hdr->partition_size = raw_size_bytes;
    hdr->data_offset = BTAI_DATA_OFFSET;
    hdr->next_free = BTAI_DATA_OFFSET + model_size;

    btai_entry *ent = (btai_entry *)(toc + sizeof(btai_header));
    /* Copy name character by character */
    const char *name = "model.btw";
    for (int i = 0; name[i]; i++) ent->name[i] = name[i];
    ent->offset = BTAI_DATA_OFFSET;
    ent->size = model_size;
    ent->flags = 0;

    /* Write TOC (first 8 sectors of raw partition) */
    status = disk->WriteBlocks(disk, media_id, raw_start_lba,
                                BTAI_DATA_OFFSET, toc);
    gBS->FreePool(toc);

    disk->FlushBlocks(disk);
    return status;
}

/* ================================================================== */
/* Reconnect controllers and find the new FAT32 volume                */
/* ================================================================== */

static EFI_STATUS reconnect_and_find_volume(EFI_HANDLE disk_handle,
                                             EFI_FILE_PROTOCOL **vol_root) {
    /* Disconnect all drivers from this controller */
    gBS->DisconnectController(disk_handle, 0, 0);
    gBS->Stall(500000);  /* 500ms */

    /* Reconnect recursively — UEFI will re-scan partitions */
    gBS->ConnectController(disk_handle, 0, 0, 1);
    gBS->Stall(2000000);  /* 2s for filesystem binding */

    /* Find SimpleFileSystem on a non-removable logical partition */
    UINTN n_handles = 0;
    EFI_HANDLE *handles = 0;
    EFI_STATUS status = gBS->LocateHandleBuffer(
        2 /* ByProtocol */, &gEfiSimpleFileSystemProtocolGuid, 0,
        &n_handles, &handles);
    if (status != EFI_SUCCESS || n_handles == 0)
        return EFI_NOT_FOUND;

    for (UINTN i = 0; i < n_handles; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        status = gBS->HandleProtocol(handles[i], &gEfiBlockIoProtocolGuid,
                                      (void **)&bio);
        if (status != EFI_SUCCESS || !bio) continue;
        if (!bio->Media) continue;
        if (bio->Media->RemovableMedia) continue;
        if (!bio->Media->LogicalPartition) continue;

        /* This is a non-removable partition with a filesystem — our ESP */
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        status = gBS->HandleProtocol(handles[i],
                                      &gEfiSimpleFileSystemProtocolGuid,
                                      (void **)&fs);
        if (status != EFI_SUCCESS || !fs) continue;

        status = fs->OpenVolume(fs, vol_root);
        if (status == EFI_SUCCESS) {
            gBS->FreePool(handles);
            return EFI_SUCCESS;
        }
    }

    gBS->FreePool(handles);
    return EFI_NOT_FOUND;
}

/* ================================================================== */
/* File operations on the new ESP volume                               */
/* ================================================================== */

static EFI_STATUS create_dir(EFI_FILE_PROTOCOL *root, const char *path) {
    uint16_t wpath[256];
    ascii_to_ucs2(path, wpath, 256);

    EFI_FILE_PROTOCOL *dir = 0;
    EFI_STATUS status = root->Open(root, &dir, wpath,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        EFI_FILE_DIRECTORY);
    if (status == EFI_SUCCESS && dir) dir->Close(dir);
    return status;
}

static EFI_STATUS copy_file_to_esp(EFI_FILE_PROTOCOL *src_root,
                                    const char *src_path,
                                    EFI_FILE_PROTOCOL *dst_root,
                                    const char *dst_path,
                                    const char *display_name) {
    uint16_t wsrc[256], wdst[256];
    ascii_to_ucs2(src_path, wsrc, 256);
    ascii_to_ucs2(dst_path, wdst, 256);

    /* Open source */
    EFI_FILE_PROTOCOL *src = 0;
    EFI_STATUS status = src_root->Open(src_root, &src, wsrc,
                                        EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        efi_print_ascii("        ");
        efi_print_ascii(display_name);
        efi_print_ascii(" — not found, skipped\r\n");
        return EFI_SUCCESS;  /* Non-fatal */
    }

    /* Get size */
    uint8_t info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = src->GetInfo(src, &gEfiFileInfoGuid_install, &info_size, info_buf);
    if (status != EFI_SUCCESS) { src->Close(src); return status; }

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    uint64_t file_size = info->FileSize;

    efi_print_ascii("        ");
    efi_print_ascii(display_name);
    efi_print_ascii(" (");
    print_size_human(file_size);
    efi_print_ascii(")...");

    /* Read entire file into memory */
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
        efi_print_ascii(" READ FAILED\r\n");
        return status;
    }

    /* Create and write destination */
    EFI_FILE_PROTOCOL *dst = 0;
    status = dst_root->Open(dst_root, &dst, wdst,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (status != EFI_SUCCESS) {
        gBS->FreePages(phys, pages);
        efi_print_ascii(" CREATE FAILED\r\n");
        return status;
    }

    UINTN write_size = read_size;
    status = dst->Write(dst, &write_size, buf);
    dst->Close(dst);
    gBS->FreePages(phys, pages);

    efi_print_ascii(status == EFI_SUCCESS ? " OK\r\n" : " WRITE FAILED\r\n");
    return status;
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

    /* User confirmation */
    efi_print_ascii("  WARNING: This will ERASE ALL DATA on the internal disk.\r\n");
    efi_print_ascii("  All existing partitions and data will be destroyed.\r\n\r\n");
    efi_print_ascii("  Type 'yes' to continue: ");

    char confirm[16];
    int clen = efi_read_line(confirm, sizeof(confirm));
    if (clen != 3 || confirm[0] != 'y' || confirm[1] != 'e' || confirm[2] != 's') {
        efi_print_ascii("\r\n  Aborted.\r\n");
        return;
    }
    efi_print_ascii("\r\n");

    /* Step 2: Write GPT */
    efi_print_ascii("  [1/6] Writing GPT partition table...");
    EFI_STATUS st = write_protective_mbr(disk);
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED (MBR)\r\n");
        return;
    }
    st = write_gpt(disk);
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED (GPT)\r\n");
        return;
    }
    efi_print_ascii(" OK\r\n");

    /* Step 3: Format ESP */
    efi_print_ascii("  [2/6] Formatting FAT32 (100 MB ESP)...");
    st = format_fat32(disk);
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED\r\n");
        return;
    }
    efi_print_ascii(" OK\r\n");

    /* Step 4: Copy model to raw partition */
    uint64_t raw_end = disk->Media->LastBlock - 33;
    uint64_t raw_size = (raw_end - RAW_START_LBA + 1) * 512ULL;

    efi_print_ascii("  [3/6] Copying model to data partition (");
    print_size_human(raw_size);
    efi_print_ascii(" available)...\r\n");

    st = copy_model_to_raw(disk, RAW_START_LBA, raw_size);
    if (st == EFI_SUCCESS) {
        efi_print_ascii("\r\n        OK\r\n");
    } else if (st == EFI_NOT_FOUND) {
        efi_print_ascii("        model.btw not found on USB — skipped\r\n");
    } else {
        efi_print_ascii("\r\n        FAILED\r\n");
        return;
    }

    /* Step 5: Mount new ESP */
    efi_print_ascii("  [4/6] Mounting new filesystem...");
    EFI_FILE_PROTOCOL *hd_root = 0;
    st = reconnect_and_find_volume(disk_handle, &hd_root);
    if (st != EFI_SUCCESS) {
        /* Retry once after longer delay */
        efi_print_ascii(" retrying...");
        gBS->Stall(3000000);
        st = reconnect_and_find_volume(disk_handle, &hd_root);
    }
    if (st != EFI_SUCCESS) {
        efi_print_ascii(" FAILED\r\n");
        efi_print_ascii("  Firmware could not mount the new partition.\r\n");
        return;
    }
    efi_print_ascii(" OK\r\n");

    /* Step 6a: Create directories */
    efi_print_ascii("  [5/6] Creating directories...");
    create_dir(hd_root, "EFI");
    create_dir(hd_root, "EFI\\BOOT");
    create_dir(hd_root, "rwkvos");
    create_dir(hd_root, "workspace");
    efi_print_ascii(" OK\r\n");

    /* Step 6b: Copy files to ESP */
    efi_print_ascii("  [6/6] Copying files:\r\n");
    EFI_FILE_PROTOCOL *usb_root = efi_fs_root();

    copy_file_to_esp(usb_root, "EFI\\BOOT\\BOOTX64.EFI",
                     hd_root, "EFI\\BOOT\\BOOTX64.EFI", "BOOTX64.EFI");
    copy_file_to_esp(usb_root, "rwkvos\\vocab.btv",
                     hd_root, "rwkvos\\vocab.btv", "vocab.btv");
    copy_file_to_esp(usb_root, "wifi.conf",
                     hd_root, "wifi.conf", "wifi.conf");

    hd_root->Close(hd_root);

    /* Summary */
    efi_print_ascii("\r\n  Installation complete!\r\n");
    efi_print_ascii("  Remove USB drive and reboot to boot from hard drive.\r\n\r\n");

    /* Offer reboot */
    efi_print_ascii("  Reboot now? (y/n): ");
    char yn[8];
    efi_read_line(yn, sizeof(yn));
    if (yn[0] == 'y' || yn[0] == 'Y') {
        efi_reboot();
    }
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
        if (bio->Media->RemovableMedia) continue;
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
