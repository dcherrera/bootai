# DUET Legacy Boot

BootAI is a UEFI application. On machines that default to Legacy BIOS boot (like the Dell E6510), users would have to press F12 and select "UEFI USB" every boot. DUET eliminates this — the USB boots automatically via Legacy BIOS, no BIOS changes needed.

## What is DUET?

DUET (Developer's UEFI Environment) is a UEFI firmware-in-RAM. It runs on Legacy BIOS machines and provides full UEFI Boot Services, then chainloads our EFI app.

The USB becomes dual-boot:
- **Legacy BIOS**: MBR → boot0 → boot1f32 → DUET → BOOTX64.EFI
- **Native UEFI**: Firmware → BOOTX64.EFI (direct, as before)

Both paths end at BootAI. Plug and play on any x86-64 machine.

## Boot Flow (Legacy Path)

```
BIOS POST → reads MBR (sector 0) → jumps to boot0 at 0x7C00
  │
  ▼
boot0 (446 bytes in MBR):
  - Scans partition table for active partition (0x80 flag)
  - Reads PBR (first sector of FAT32 partition) via INT 13h
  - Jumps to PBR
  │
  ▼
boot1f32 (420 bytes in PBR, preserving BPB):
  - Parses FAT32 BPB (cluster size, FAT location, root dir cluster)
  - Uses LBA addressing via INT 13h extensions
  - Walks FAT32 root directory looking for "boot" file
  - Loads DUET firmware into memory
  │
  ▼
DUET (EFILDR20 / DxeCore):
  - Switches CPU: Real Mode → Protected Mode → Long Mode
  - Initializes Boot Services, Runtime Services
  - Dispatches DXE drivers (FAT, BlockIO, USB, keyboard, console)
  - BDS scans for \EFI\OC\OpenCore.efi
  │
  ▼
BOOTX64.EFI (BootAI) — running with full UEFI Boot Services
```

Total added boot time: ~2-5 seconds.

## Disk Layout

The DUET USB uses MBR partitioning with a single FAT32 partition:

```
Sector 0 (MBR):
  Bytes 0-445:   boot0 code (finds active partition, loads PBR)
  Bytes 446-509: Partition table (1 active FAT32 LBA entry)
  Bytes 510-511: 0x55AA signature

Sector 2048+ (Partition 1, FAT32):
  PBR (sector 0 of partition):
    Bytes 0-2:     Jump instruction (from boot1f32)
    Bytes 3-89:    BPB (from mformat, preserved during merge)
    Bytes 90-495:  boot1f32 code
    Bytes 496-509: 14 random bytes (BDS partition signature)
    Bytes 510-511: 0x55AA signature
```

### Files on FAT32

```
BOOTAI/
├── boot                           ← DUET firmware (bootX64 renamed, ~274 KB)
├── EFI/BOOT/BOOTX64.EFI          ← BootAI (native UEFI path)
├── EFI/OC/OpenCore.efi           ← BootAI (DUET/OpenCore BDS path)
├── rwkvos/
│   ├── model.btw                  ← Quantized weights
│   └── vocab.btv                  ← Tokenizer vocabulary
├── wifi.conf                      ← WiFi credentials
├── drivers/                       ← Optional UEFI drivers
└── workspace/                     ← User files
```

**Important**: The DUET BDS (from OpenCorePkg) looks for `\EFI\OC\OpenCore.efi` specifically — NOT `\EFI\BOOT\BOOTX64.EFI`. Both files must be present (identical copies of `all_bootai.efi`).

## Source of Pre-Built Binaries

All DUET boot binaries come from **OpenCorePkg** (actively maintained fork of DuetPkg):

- Download: https://github.com/acidanthera/OpenCorePkg/releases
- Inside the release ZIP: `Utilities/LegacyBoot/`

| Binary | Size | Purpose |
|--------|------|---------|
| `boot0` | 512 bytes | MBR boot code |
| `boot1f32` | 512 bytes | FAT32 PBR boot code |
| `bootX64` | ~274 KB | DUET firmware (renamed to `boot` on disk) |

These binaries are stored in `bootai/duet/`.

## Building the DUET Image

```bash
cd bootai/duet

# 1. Build the image (creates bootai_duet.img with MBR + FAT32 + boot sectors)
python3 build_image.py --size 4096 --output bootai_duet.img

# 2. Copy files into the image using mtools
IMG="bootai_duet.img@@1048576"
mcopy -i "$IMG" ../all_bootai.efi         ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" ../all_bootai.efi         ::EFI/OC/OpenCore.efi
mcopy -i "$IMG" bootX64                   ::boot
mcopy -i "$IMG" ../wifi.conf              ::wifi.conf
mcopy -i "$IMG" /path/to/model.btw        ::rwkvos/model.btw
mcopy -i "$IMG" /path/to/model.btv        ::rwkvos/vocab.btv
```

### What `build_image.py` Does

1. Creates a sparse image file
2. Writes MBR: boot0 code (bytes 0-445) + partition table (active FAT32 LBA at sector 2048)
3. Formats the partition as FAT32 using `mformat`
4. Merges boot1f32 code with mformat's BPB + 14 random bytes at offset 496
5. Verifies MBR signature, partition flags, PBR integrity
6. Creates directory structure (EFI/BOOT, EFI/OC, rwkvos, workspace, drivers)

The 14 random bytes at PBR offset 496-509 are critical — DUET's BDS uses them as a partition signature and rejects all-zeros ("BOOT MISMATCH! BOOT FAIL!").

## Flashing to USB

```bash
cd bootai/duet
sudo ./install_duet.sh /dev/diskN
```

The install script unmounts the target disk, writes the image via `dd`, and ejects.

### Updating the EFI Binary (Without Reflashing)

After rebuilding `all_bootai.efi`, you can update just the binary:

```bash
diskutil mount diskNs1
cp all_bootai.efi /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI
cp all_bootai.efi /Volumes/BOOTAI/EFI/OC/OpenCore.efi
diskutil eject diskN
```

## PS/2 Keyboard Under DUET

DUET's `BiosKeyboardDxe` driver uses INT 16h thunks to real mode for keyboard input. This doesn't work reliably on all hardware (e.g., Dell E6510).

BootAI includes a fallback: direct 8042 PS/2 controller access via port I/O (ports 0x60/0x64). The `read_key_auto()` function in `efi/input.c` tries UEFI `ReadKeyStroke` and 8042 PS/2 simultaneously — whichever responds first is used permanently for that session.

- **Native UEFI boot**: Uses `ReadKeyStroke` (standard UEFI path)
- **DUET boot**: Auto-detects and falls back to 8042 PS/2

## UEFI Services Compatibility

### Supported by DUET (BootAI uses all of these)

| Service | Notes |
|---------|-------|
| HandleProtocol, LocateProtocol | Core DxeCore |
| AllocatePages / AllocatePool | Full system RAM via E820 |
| GetMemoryMap | Built from E820 |
| CreateEvent / WaitForEvent | 8254 timer driver |
| SetVariable / GetVariable | Via EFIVAR.BIN (64KB) |
| ResetSystem | ACPI FADT reset register |
| Simple Text Output | INT 10h VBE thunk |
| Simple File System | EnhancedFatDxe |
| Block IO | SATA/AHCI + USB mass storage |

### Not in DUET (not needed)

| Protocol | Why It's OK |
|----------|-------------|
| TCP4, SNP, DHCP4 | BootAI uses custom e1000e + ns_* network stack |
| WiFi2, Supplicant | BootAI has custom IWN driver |
| Simple Text Input | Unreliable under DUET; 8042 fallback handles keyboard |

## Dependencies

- `mtools` (mformat, mcopy, mmd) — `brew install mtools`
- `python3` — for `build_image.py`
- OpenCorePkg release binaries (already in `bootai/duet/`)

## HD Install with DUET

When `install` (HD install) runs from a DUET USB, it automatically writes DUET boot sectors to the internal hard drive so it boots via Legacy BIOS without the USB.

### How It Works

The installer reads boot0/boot1f32 from the **source USB's boot sectors** at install time — no hex dumps or embedded arrays:

1. **Detect DUET**: Read USB MBR sector 0. If first byte is `0xFA` (CLI instruction), it's a DUET USB.
2. **Read boot0**: Extract bytes 0-445 from USB MBR.
3. **Read boot1f32**: Read USB partition's PBR (sector 0 of the FAT32 partition via Block IO).
4. **Read `/boot`**: Pre-read DUET firmware file from USB filesystem.
5. **Write MBR**: Inject boot0 code into HD MBR, set partition type to `0x0C` (FAT32 LBA).
6. **Merge PBR**: After formatting FAT32, merge boot1f32 code with the new BPB. Write 14 pseudo-random bytes at offset 496-509 (DUET BDS partition signature).
7. **Write DUET files**: Copy `/boot` and `/EFI/OC/OpenCore.efi` to HD ESP alongside standard files.

If the source USB is non-DUET (no boot0 in MBR), the installer skips all DUET steps and creates a UEFI-only install.

### HD Disk Layout (DUET)

```
Sector 0 (MBR):
  Bytes 0-445:   boot0 code (from USB)
  Bytes 446-461: Partition 1: active, type 0x0C (FAT32 LBA), 100 MB at LBA 2048
  Bytes 462-477: Partition 2: type 0xDA (BootAI Data), rest of disk
  Bytes 510-511: 0x55AA

LBA 2048 (Partition 1, FAT32 ESP):
  PBR: boot1f32 code merged with format_fat32() BPB + 14 random bytes
  Files: EFI/BOOT/BOOTX64.EFI, EFI/OC/OpenCore.efi, boot, rwkvos/vocab.btv, wifi.conf

LBA 206848 (Partition 2, Raw Data):
  BTAI TOC + model weights (same as UEFI-only install)
```

## Troubleshooting

| Problem | Cause | Fix |
|---------|-------|-----|
| "BOOT MISMATCH! BOOT FAIL!" | PBR offset 496-509 is all zeros | Rebuild image with `build_image.py` (writes random bytes) |
| Black screen, blinking cursor | boot1f32 can't find `/boot` file | Verify `boot` file exists in FAT32 root |
| USB not in F12 menu | Dell NVRAM corrupted | F2 → BIOS Setup → Load Defaults |
| Keyboard doesn't respond | DUET BiosKeyboardDxe failing | Already handled by 8042 PS/2 fallback in input.c |
| Boots on F12 but not auto | Partition not marked active | Check MBR partition entry has status byte 0x80 |
