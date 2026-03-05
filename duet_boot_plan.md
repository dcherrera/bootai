# DUET Legacy Boot Plan

## Problem

BootAI is a UEFI application. Most old machines (including Dell E6510) default to Legacy BIOS boot. Users must press F12 and manually select "UEFI USB" every time. This is unacceptable for a plug-and-play product.

## Solution

Add **DUET** (Developer's UEFI Environment) to the BootAI USB. DUET is a UEFI firmware-in-RAM that runs on Legacy BIOS machines. It provides full UEFI Boot Services, then chainloads our EFI app.

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
  - Loads EFILDR20 into memory at ~0x20000
  │
  ▼
EFILDR20 / Start32 (first stage of DUET):
  - Loads remaining sectors via INT 13h
  - Calls INT 15h/E820 to build memory map
  - Switches CPU: Real Mode → Protected Mode → Long Mode
  - Decompresses DxeCore + all DXE drivers from firmware volume
  │
  ▼
DxeCore (standard EDK2):
  - Initializes Boot Services (AllocatePages, HandleProtocol, etc.)
  - Initializes Runtime Services (GetVariable, SetVariable, etc.)
  - Dispatches DXE drivers: FAT, BlockIO, USB, ConIn, ConOut, etc.
  - BDS scans for EFI/BOOT/BOOTX64.EFI
  │
  ▼
BOOTX64.EFI (BootAI) — running with full UEFI Boot Services
```

Total added boot time: ~2-5 seconds.

## Files Needed

### Boot Sectors (written to raw disk sectors, not as files)

| Component | Location | Size | Purpose |
|-----------|----------|------|---------|
| **boot0** | MBR (disk sector 0, bytes 0-445) | 446 bytes | Finds active partition, loads PBR |
| **boot1f32** | PBR (partition sector 0, bytes 90-509) | 420 bytes | Parses FAT32, loads `boot` file |

**Critical**: The PBR's BPB (bytes 3-89) must be preserved from the original formatted partition. The partition table (MBR bytes 446-511) is also preserved.

### Files on FAT32 Filesystem

| File | Size | Purpose |
|------|------|---------|
| `/boot` | ~300 KB | DUET firmware image (EFILDR20 renamed) |
| `/EFIVAR.BIN` | ~64 KB | Emulated NVRAM storage (created automatically if missing) |
| `/EFI/BOOT/BOOTX64.EFI` | ~370 KB | BootAI (already exists, unchanged) |

## Source of Pre-Built Binaries

**OpenCorePkg** (recommended) — actively maintained, well-tested fork of DuetPkg.

Download: https://github.com/acidanthera/OpenCorePkg/releases

Inside the release ZIP, `Utilities/LegacyBoot/` contains:
- `boot0` — MBR boot code
- `boot1f32` — FAT32 PBR boot code
- `bootx64` — DUET firmware (rename to `boot`)
- `BootInstall_X64.tool` — macOS install script

## Installation Steps (macOS)

```bash
# 1. Download OpenCorePkg release
curl -LO https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.6/OpenCore-1.0.6-RELEASE.zip
unzip OpenCore-1.0.6-RELEASE.zip

# 2. Identify USB disk
diskutil list external
# e.g., /dev/disk2, partition disk2s1

# 3. Unmount (don't eject)
diskutil unmountDisk disk2

# 4. Write MBR boot code (preserves partition table)
sudo fdisk -uy -f Utilities/LegacyBoot/boot0 /dev/rdisk2

# 5. Merge PBR: boot1f32 code + original BPB
sudo dd if=/dev/rdisk2s1 count=1 of=/tmp/origbs
cp Utilities/LegacyBoot/boot1f32 /tmp/newbs
sudo dd if=/tmp/origbs of=/tmp/newbs skip=3 seek=3 bs=1 count=87 conv=notrunc

# 6. Write merged PBR
sudo dd if=/tmp/newbs of=/dev/rdisk2s1

# 7. Mount and copy DUET firmware
diskutil mount disk2s1
cp Utilities/LegacyBoot/bootx64 /Volumes/BOOTAI/boot

# 8. Set partition active flag
sudo fdisk -e /dev/rdisk2 <<EOF
f 1
w
y
q
EOF

# 9. Verify existing files intact
ls -la /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI
ls -la /Volumes/BOOTAI/rwkvos/
```

## UEFI Services Compatibility

### Supported by DUET (BootAI uses all of these)

| Service | Status | Notes |
|---------|--------|-------|
| HandleProtocol | YES | Core DxeCore |
| LocateProtocol | YES | Core DxeCore |
| LocateHandleBuffer | YES | Core DxeCore |
| AllocatePages / AllocatePool | YES | Full system RAM via E820 |
| GetMemoryMap | YES | Built from E820 |
| CreateEvent / WaitForEvent / SignalEvent | YES | 8254 timer driver |
| SetVariable / GetVariable | YES | Via EFIVAR.BIN (64KB) |
| ResetSystem | YES | ACPI FADT reset register |
| Simple Text Input | YES | PS/2 + USB keyboard |
| Simple Text Output | YES | INT 10h VBE thunk |
| Simple File System | YES | EnhancedFatDxe |
| Block IO | YES | SATA/AHCI + USB mass storage |

### NOT in DUET (but not a problem)

| Protocol | Status | Why It's OK |
|----------|--------|-------------|
| TCP4 | NO | BootAI uses custom ns_* stack on Dell, not UEFI TCP4 |
| SNP | NO | BootAI has its own e1000e driver with direct HW access |
| DHCP4 | NO | BootAI has custom ns_dhcp implementation |
| IP4 Config2 | NO | Not needed with custom stack |
| WiFi2 / Supplicant | NO | BootAI has custom IWN driver |

**Key insight**: BootAI's custom network stack (e1000e + ns_*) bypasses the UEFI network protocol stack entirely. It talks directly to hardware via PCI IO. This works perfectly on DUET because PCI IO protocol is available.

## What Changes on the USB

| Component | Before | After |
|-----------|--------|-------|
| MBR boot code (446 bytes) | Generic | boot0 |
| MBR partition table | **Unchanged** | **Unchanged** |
| PBR BPB (bytes 3-89) | **Unchanged** | **Unchanged** |
| PBR boot code (bytes 90-509) | Generic FAT32 | boot1f32 |
| `/boot` (new file) | N/A | DUET firmware (~300KB) |
| Everything else | **Unchanged** | **Unchanged** |

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| EFIVAR.BIN corruption (power loss during write) | Boot hangs | Delete file, auto-recreated |
| DUET <470KB size limit for EFILDR | Can't add more drivers | Not needed — BootAI has its own drivers |
| Older DUET >4GB RAM pointer bug | Crash on 8GB machine | Fixed in OpenCorePkg builds |
| DUET GOP limited resolution | Low-res display | BootAI uses text mode only |
| PCI IO protocol needed for e1000e | No network | DUET includes PCI driver — should work |

## Implementation Tasks

### Phase 1: Validate on Dell E6510
1. Download OpenCorePkg release binaries
2. Install boot0 + boot1f32 + boot file onto existing BootAI USB
3. Boot Dell E6510 via Legacy USB — verify BootAI loads through DUET
4. Test: console I/O, filesystem, model loading, e1000e networking, HTTP server

### Phase 2: Automate in Makefile
1. Add `make usb-legacy` target that installs DUET onto a mounted USB
2. Store boot0, boot1f32, bootx64 binaries in `bootai/duet/` directory
3. Update `make release` to produce a hybrid Legacy+UEFI disk image

### Phase 3: HD Install Integration
1. Update `ba_install_to_hd()` to write DUET boot sectors to internal disk
2. Copy `boot` file to ESP alongside BOOTX64.EFI
3. HD now boots via Legacy BIOS → DUET → BootAI (no BIOS changes needed)

## References

- [OpenCorePkg Releases](https://github.com/acidanthera/OpenCorePkg/releases) — Pre-built DUET binaries
- [BootDuet](https://github.com/migle/BootDuet) — PBR boot sector source
- [A BIOS to UEFI Transformation](http://www.rodsbooks.com/bios2uefi/) — Rod Smith's comprehensive guide
- [Creating a DUET USB stick](https://pete.akeo.ie/2011/07/creating-bootable-uefi-duet-usb-stick.html) — Practical walkthrough
- [TianoCore DuetPkg](https://github.com/tianocore/tianocore.github.io/wiki/DuetPkg) — Official docs
- [Clover on Dell E6510](https://www.tonymacx86.com/threads/clover-efi-boot-dell-latitude-e6510.142499/) — Confirms DUET works on this hardware
