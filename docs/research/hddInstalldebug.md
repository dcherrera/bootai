# HD Install Debug Log

## Goal

Install BootAI to the Dell E6510's internal SATA hard drive so it boots without USB. User should not need to touch BIOS settings — it should just work.

## Hardware

- **Machine:** Dell Latitude E6510
- **Firmware:** Phoenix SecureCore Tiano (Insyde H2O based)
- **Internal disk:** 465.7 GB SATA HDD
- **USB:** 8 GB FAT32, MBR partition table — boots fine when selecting "UEFI USB" from Dell boot menu
- **Network:** Ethernet (e1000e), IP 192.168.1.85, API token `bootai`

## Current Status: NOT WORKING

HD install completes successfully (all data verified), but booting from HD gives a **blank screen with blinking cursor**. "BootAI" appears in the Dell UEFI boot menu, but selecting it does nothing useful.

## What Works

- USB boot via "UEFI USB" selection in Dell boot menu
- All install steps complete without error
- CRC32 hash verification: every file on HD matches USB bit-for-bit
- NVRAM Boot0001 entry created, BootOrder updated (BootAI first)
- "BootAI" shows up in Dell boot options menu
- `hdinfo` command confirms correct partition table, FAT32 BPB, directory structure, file sizes, BTAI TOC

## What We've Tried

### 1. GPT + Protective MBR (Original Plan)
- GPT partition table with protective MBR (type 0xEE) at LBA 0
- Primary GPT header at LBA 1, entries at LBA 2-33
- Backup entries at LastBlock-32, backup header at LastBlock
- Partition 1: ESP (type GUID C12A7328-...), LBA 2048, 100MB FAT32
- Partition 2: Raw data (custom GUID B007A1DA-...), rest of disk
- NVRAM entry with GPT signature (MBRType=0x02, SignatureType=0x02, Partition GUID)
- **Result:** Blank screen with cursor

### 2. MBR with Partition Type 0x0C (FAT32 LBA)
- Switched from GPT to real MBR with two partition entries
- Partition 1: type 0x0C, bootable flag, 100MB at LBA 2048
- Partition 2: type 0xDA (custom BootAI data), rest of disk
- NVRAM entry with MBR signature (MBRType=0x01, SignatureType=0x01, DiskID=0xB007A1DA)
- **Result:** Blank screen with cursor

### 3. MBR with Partition Type 0xEF (EFI System Partition)
- Changed partition 1 type from 0x0C to 0xEF per UEFI spec (ESP on MBR should be 0xEF)
- **Result:** Blank screen with cursor

### 4. MBR + GPT Wipe
- Added code to zero LBA 1-33 and backup GPT at end of disk
- Rationale: UEFI spec says firmware MUST prefer GPT over MBR if valid GPT exists
- **Result:** Blank screen with cursor

### 5. GPT with Protective MBR (Second Attempt, Current)
- Reverted to GPT based on internet research
- Added proper protective MBR (type 0xEE, covering entire disk)
- NVRAM entry with GPT GUID signature pointing to ESP unique GUID
- **Result:** Blank screen with cursor (latest test, 2026-03-01)

### 6. Early Debug Output
- Added "BootAI OK" print as first thing in `efi_main()` (before `efi_clear_screen()`)
- Uses raw `ConOut->OutputString` — no dependencies
- **Result:** Never seen on HD boot — suggests the binary never loads at all

## Verified Data Integrity

All files CRC32 verified (USB source vs HD destination):

| File | Size | USB CRC32 | HD CRC32 | Status |
|------|------|-----------|----------|--------|
| BOOTX64.EFI | 529 KB | 0x82265A60 | 0x82265A60 | MATCH |
| vocab.btv | 1 MB | 0x368DE988 | 0x368DE988 | MATCH |
| wifi.conf | 17 B | 0x4E8B7B44 | 0x4E8B7B44 | MATCH |
| model.btw | 256 MB | 0x739EC923 | 0x739EC923 | MATCH |

## Partition Layout (Current — GPT)

```
LBA 0:        Protective MBR (type 0xEE)
LBA 1:        Primary GPT Header
LBA 2-33:     Partition Entry Array (128 entries x 128 bytes)
LBA 2048:     Partition 1 start — ESP (100MB FAT32)
  ESP Layout:
    Cluster 2: Root dir → EFI/, RWKVOS/, WORK/, WIFI.CON
    Cluster 3: EFI/ → BOOT/
    Cluster 4: BOOT/ → BOOTX64.EFI
    Cluster 5: RWKVOS/ → VOCAB.BTV
    Cluster 6: WORK/ (empty)
    Cluster 7+: BOOTX64.EFI data (529 KB)
    After EFI: vocab.btv data (1 MB)
    After vocab: wifi.conf data (17 B)
LBA 206848:   Partition 2 start — Raw data (BTAI TOC + model.btw)
  BTAI Header: magic=0x49415442, version=1, 1 entry
  Entry 0: "model.btw" offset=4096 size=256MB
LBA N-32:     Backup Partition Entries
LBA N:        Backup GPT Header
```

## FAT32 BPB Details

```
OEM:         BOOTAI
BytsPerSec:  512
SecPerClus:  8 (4KB clusters)
RsvdSecCnt:  32
NumFATs:     2
TotSec32:    204800
FATSz32:     200
RootClus:    2
FSInfo:      1
BkBootSec:   6
VolLabel:    BOOTAI
FSType:      FAT32
```

## NVRAM Boot Entry (Current)

```
Boot0001:
  Attributes: LOAD_OPTION_ACTIVE (0x00000001)
  Description: "BootAI" (UCS-2)
  Device Path:
    HD Node (42 bytes):
      Type=0x04 (Media), SubType=0x01 (Hard Drive)
      PartitionNumber=1
      PartitionStart=2048 (ESP_START_LBA)
      PartitionSize=204800 (ESP_SECTORS)
      Signature=ESP unique GUID (B007A1DA-0001-4000-8000-000000000001)
      MBRType=0x02 (GPT)
      SignatureType=0x02 (GUID)
    File Path Node:
      \EFI\BOOT\BOOTX64.EFI
    End Node
  BootOrder: 0x0001 first, then existing entries
```

## Internet Research Findings

### UEFI Removable vs Fixed Media Boot
- **Removable media (USB):** Firmware auto-scans for `\EFI\BOOT\BOOTX64.EFI` — no NVRAM entry needed
- **Fixed media (HD):** Firmware uses NVRAM boot variables with device paths
- The fallback path `\EFI\BOOT\BOOTX64.EFI` is NOT guaranteed on fixed disks (UEFI 2.3+ added optional support)
- **However:** USB on this Dell also doesn't auto-discover — requires manual "UEFI USB" selection

### MBR vs GPT on Internal Drives
- Many UEFI implementations switch to legacy/CSM boot when they see MBR on internal drives
- Blank screen with blinking cursor = classic symptom of firmware falling through to empty MBR boot code
- UEFI spec technically allows ESP on MBR (type 0xEF) but many firmware don't support it for fixed disks

### Dell E6510 Specifics
- Phoenix SecureCore Tiano firmware
- Has "Boot List Option" in BIOS: Legacy or UEFI
- Known difficulties with UEFI boot entries on this model
- Fast Boot setting may affect UEFI boot (should be "Auto" or "Through")
- BIOS updates up to A17 available — may fix UEFI boot issues

## Hypotheses (Untested)

### H1: Dell firmware doesn't support NVRAM boot entries for custom partitions
The firmware shows "BootAI" in the menu (proving it reads the NVRAM variable), but may not be able to resolve the device path to the actual disk/partition. The blank screen could mean it found the entry but failed to locate the partition.

### H2: Device path format is wrong
The HD device path node may need additional parent nodes (PCI root, SATA controller, etc.) for the firmware to traverse the device tree and find the disk. Our device path starts directly at the HD node — the firmware may need the full path from root.

### H3: ConnectController issue
After install, `ConnectController` reports only 2 SimpleFileSystem handles (same as before install). The firmware may not recognize the new GPT/FAT32 on the HD until a full power cycle (not just reboot). However, the NVRAM approach should bypass this since it stores the device path.

### H4: BIOS settings
The Dell BIOS "Boot List Option" may need to be set to "UEFI" explicitly. Or Fast Boot may need adjustment. User hasn't checked these settings.

### H5: The firmware's UEFI boot path needs the ESP to be discovered during POST
Some firmware only discovers ESPs during POST enumeration, not from NVRAM device paths. If the firmware doesn't mount our GPT ESP during POST, the NVRAM entry resolves to nothing. This would explain why it appears in the menu but can't actually boot.

### H6: Need to test with a known-good UEFI OS
Install a minimal Linux (like Arch or Ubuntu) via standard UEFI install to see if the Dell can UEFI-boot from HD at all. If it can't, the problem is the Dell's BIOS settings. If it can, compare the working partition table/NVRAM entries against ours.

## Key Files

- `efi/install.c` — Main install code (~1900 lines): GPT/MBR write, FAT32 format, file copy, CRC32 verify, NVRAM entry
- `efi/main.c` — Entry point, `install` command dispatch, early "BootAI OK" debug output
- `efi/efi.h` — UEFI protocol types (Block IO, Runtime Services, etc.)
- `efi/fs.c` — Filesystem init (enumerates all SimpleFileSystem handles)
- `model/loader.c` — Model loader (tries raw partition first, falls back to filesystem)

## Commands for Debugging

```bash
# Push firmware via OTA
curl -H "Authorization: Bearer bootai" --data-binary @all_bootai.efi http://192.168.1.85:8080/api/update

# Run install (non-interactive, no confirmation)
curl -H "Authorization: Bearer bootai" -d '{"cmd":"install"}' http://192.168.1.85:8080/api/cmd

# Inspect HD contents
curl -H "Authorization: Bearer bootai" -d '{"cmd":"hdinfo"}' http://192.168.1.85:8080/api/cmd

# Reboot
curl -H "Authorization: Bearer bootai" -d '{"cmd":"reboot"}' http://192.168.1.85:8080/api/cmd

# Check health
curl -H "Authorization: Bearer bootai" http://192.168.1.85:8080/api/health
```

## Next Steps to Try

1. **Check Dell BIOS settings** — ensure Boot List Option = UEFI, check Fast Boot setting
2. **Update Dell BIOS** to latest (A17) — may fix UEFI boot path resolution
3. **Test with Linux** — install Ubuntu/Arch via standard UEFI to confirm Dell can HD-boot UEFI at all
4. **Full device path** — add PCI root + SATA controller nodes before the HD node in NVRAM entry
5. **Compare with working entry** — if Linux boots, dump its NVRAM boot entry (`efibootmgr -v`) and compare device path format
6. **Try without NVRAM** — see if Dell auto-discovers the GPT ESP during POST (just write GPT + FAT32, no NVRAM entry, and check if it appears in boot menu automatically)
