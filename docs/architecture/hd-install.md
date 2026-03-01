# Hard Drive Install

Boot menu option `[2] Install to Hard Drive` wipes an internal disk, creates a GPT partition table, and copies all BootAI files to make a standalone bootable system. After install, the machine boots directly from the HD without needing the USB.

## Disk Layout

The installer creates two partitions: a small FAT32 ESP for UEFI boot files, and a raw data partition for model weights with no filesystem and no file size limits.

```
GPT Partition Table (512-byte sectors)
┌──────────────────────────────────────────┐
│ LBA 0:        Protective MBR             │
│ LBA 1:        Primary GPT Header         │
│ LBA 2-33:     Partition Entry Array       │
├──────────────────────────────────────────┤
│ LBA 2048:     Partition 1 — FAT32 ESP    │  100 MB
│               EFI/BOOT/BOOTX64.EFI       │
│               rwkvos/vocab.btv            │
│               wifi.conf                   │
│               workspace/                  │
├──────────────────────────────────────────┤
│ LBA 206848:   Partition 2 — Raw Data     │  Rest of disk
│               [TOC header: 4 KB]          │
│               [model.btw: contiguous]     │
│               [future: LoRAs, models]     │
├──────────────────────────────────────────┤
│ Last-32:      Backup Partition Entries    │
│ Last LBA:     Backup GPT Header          │
└──────────────────────────────────────────┘
```

### Why two partitions?

FAT32 has a 4 GB file size limit. Current model weights are ~3.6 GB (fits), but larger models are coming. The raw data partition stores model weights as contiguous bytes via Block IO — no filesystem overhead, no size limits. A 50 GB model is just bytes at known offsets.

The ESP only holds files UEFI needs to boot (~100 MB): the EFI binary, tokenizer vocab, and WiFi config.

## Raw Data Partition — TOC Format

The raw partition has no filesystem. Instead, a 4 KB table of contents (TOC) at the start tracks what's stored and where.

```
Offset 0:     btai_header (40 bytes)
Offset 40:    btai_entry[0] (56 bytes)
Offset 96:    btai_entry[1] (56 bytes)
...
Offset 4096:  Data region starts
```

### Header

```c
typedef struct {
    uint32_t magic;           /* 0x49415442 = "BTAI" */
    uint32_t version;         /* 1 */
    uint32_t entry_count;     /* number of active entries */
    uint32_t _reserved;
    uint64_t partition_size;  /* total bytes in partition */
    uint64_t data_offset;    /* 4096 (where data starts) */
    uint64_t next_free;       /* byte offset of next free space */
} btai_header;
```

### Entry

```c
typedef struct {
    char     name[32];        /* e.g. "model.btw" */
    uint64_t offset;          /* byte offset from partition start */
    uint64_t size;            /* size in bytes */
    uint32_t flags;           /* 0 = active, 1 = deleted */
    uint32_t _pad;
} btai_entry;
```

### Space tracking

Free space is calculated as `partition_size - next_free`. Before every write, the installer checks this and refuses if there isn't enough room. Data is append-only — deletion marks entries as deleted but doesn't reclaim space until a future compaction operation.

### Custom partition type GUID

The raw partition uses a custom GPT type GUID so it won't be confused with any standard partition type:

```
B007A1DA-7A00-4000-8000-000000000001
```

UEFI firmware creates a Block IO handle for it (partition driver scans all GPT entries regardless of type) but won't bind a filesystem driver to it.

## Install Flow

```
User selects [2] from boot menu
    │
    ├─ 1. Find internal disk
    │     Enumerate EFI_BLOCK_IO_PROTOCOL handles
    │     Filter: MediaPresent && !RemovableMedia && !LogicalPartition && BlockSize==512
    │
    ├─ 2. User confirmation
    │     Display disk size, warn about data loss
    │     Require typing "yes" to proceed
    │
    ├─ 3. Write GPT
    │     Protective MBR (LBA 0)
    │     Primary GPT header + 128 partition entries (LBA 1-33)
    │     Backup entries + header (last 33 LBAs)
    │     CRC32 checksums on header and entry array
    │
    ├─ 4. Format FAT32
    │     BPB + FSINFO + backup boot sector (reserved area, 32 sectors)
    │     Two FAT copies (each ~100 KB for 100 MB partition)
    │     Empty root directory cluster
    │
    ├─ 5. Copy model to raw partition
    │     Read model.btw from USB via filesystem (1 MB chunks)
    │     Write to raw partition via Block IO WriteBlocks
    │     Write TOC header with model entry
    │     Progress bar displayed during copy
    │
    ├─ 6. Mount new ESP
    │     DisconnectController + ConnectController (forces UEFI re-enumeration)
    │     Find new SimpleFileSystem on non-removable logical partition
    │     Retry once after 3s if first attempt fails
    │
    ├─ 7. Create directories + copy files
    │     Create: EFI/BOOT/, rwkvos/, workspace/
    │     Copy: BOOTX64.EFI, vocab.btv, wifi.conf
    │     Missing source files are skipped with a warning
    │
    └─ 8. Offer reboot
```

## Boot Path — Loading from HD

When booted from the HD, the model is on the raw partition (not the filesystem). The boot path handles this transparently:

```
ensure_model_loaded()
    │
    ├─ Try raw partition first (HD boot)
    │    Scan Block IO handles for non-removable logical partition
    │    Read first 4 KB, check for BTAI magic
    │    Find "model.btw" entry in TOC
    │    Allocate pages, read data via ReadBlocks (1 MB chunks)
    │    Pass buffer to ba_model_load_buffer()
    │
    └─ Fall back to filesystem (USB boot)
         efi_read_file("\\rwkvos\\model.btw")
         ba_model_load() — reads file, parses buffer
```

Vocab and wifi.conf are always read from the filesystem (ESP). On HD boot, `efi_fs_init()` finds the ESP's SimpleFileSystem via `LocateProtocol`, which works identically whether the ESP is on USB or HD.

## Files

| File | Role |
|------|------|
| `efi/install.c` | Install orchestrator: GPT, FAT32, file copy, raw partition, TOC, `ba_raw_read_entry()` |
| `efi/efi.h` | `EFI_BLOCK_IO_PROTOCOL` types, `ba_install_to_hd()` and `ba_raw_read_entry()` declarations |
| `efi/main.c` | Boot menu case '2' wiring, `ensure_model_loaded()` raw partition fallback, Block IO GUID |
| `model/loader.c` | `ba_model_load_buffer()` — loads model from pre-allocated memory buffer |
| `model/model.h` | `ba_model_load_buffer()` declaration |

## FAT32 Parameters (100 MB ESP)

| Field | Value |
|-------|-------|
| BytsPerSec | 512 |
| SecPerClus | 8 (4 KB clusters) |
| RsvdSecCnt | 32 |
| NumFATs | 2 |
| FATSz32 | ~200 sectors |
| RootClus | 2 |
| FSInfo | sector 1 |
| BkBootSec | sector 6 |
| OEM Name | `BOOTAI` |
| Volume Label | `BOOTAI` |

## GPT Details

| Field | Value |
|-------|-------|
| Partition 1 type | `C12A7328-F81F-11D2-BA4B-00A0C93EC93B` (EFI System Partition) |
| Partition 2 type | `B007A1DA-7A00-4000-8000-000000000001` (BootAI Data) |
| ESP start | LBA 2048 (1 MB aligned) |
| ESP size | 204,800 sectors (100 MB) |
| Raw start | LBA 206,848 |
| Raw end | Last LBA - 33 |
| Entry array | 128 entries x 128 bytes = 32 sectors |

CRC32 (standard polynomial `0xEDB88320`) is used for both the GPT header and partition entry array checksums.

## Error Handling

| Scenario | Behavior |
|----------|----------|
| No internal disk found | Error message, return to menu |
| Disk is read-only | Refuse, return to menu |
| Disk too small (<200 MB) | Refuse, return to menu |
| User doesn't type "yes" | Abort, return to menu |
| WriteBlocks fails | Print failing LBA, abort |
| Model not found on USB | Skip with warning, install continues |
| ConnectController fails to mount | Retry once after 3s, then abort |
| Not enough space on raw partition | Print needed vs available, abort |

## Testing

### QEMU

Add a second non-removable drive to test the install:

```bash
# Create a blank 10 GB disk
qemu-img create -f qcow2 hd.qcow2 10G

# Add to QEMU launch (in addition to existing USB drive):
-drive file=hd.qcow2,format=qcow2,if=sata
```

The SATA drive appears as non-removable, while the existing virtio USB drive may also appear non-removable. For proper testing, use USB mass storage for the boot drive:

```bash
-device usb-storage,drive=usb0
-drive id=usb0,file=bootai.img,format=raw,if=none
```

After install, verify the disk externally:

```bash
# Check GPT
sgdisk -p hd.qcow2

# Mount and inspect ESP (Linux)
sudo losetup -P /dev/loop0 hd.qcow2
sudo mount /dev/loop0p1 /mnt
ls /mnt/EFI/BOOT/
```

### Real Hardware (Dell E6510)

1. Boot from USB, select `[2] Install to Hard Drive`
2. Verify it finds the internal SATA disk (non-removable)
3. Type `yes` to confirm
4. Watch progress — model copy takes several minutes for large models
5. Remove USB, reboot — Dell boots from HD
6. Verify: `chat`, `ls`, `meminfo` all work
