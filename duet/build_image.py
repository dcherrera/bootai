#!/usr/bin/env python3
"""
Build a bootable DUET Legacy Boot disk image entirely from file operations.

Creates an MBR-partitioned FAT32 disk image with:
  - boot0 code in the MBR (bytes 0-445)
  - boot1f32 code in the PBR (partition boot record) with preserved BPB
  - 14 random bytes at PBR offset 496-509
  - FAT32 filesystem with EFI directory structure

Usage:
    python3 build_image.py [--size SIZE_MB] [--output OUTPUT_PATH]

Dependencies: mformat, mcopy, mmd (from mtools via `brew install mtools`)
"""

import argparse
import os
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
SECTOR_SIZE = 512
PARTITION_START_LBA = 2048            # Standard 1 MB alignment
PARTITION_START_BYTES = PARTITION_START_LBA * SECTOR_SIZE  # 1,048,576

# MBR offsets
MBR_BOOT_CODE_SIZE = 446
MBR_PARTITION_TABLE_OFFSET = 446
MBR_PARTITION_ENTRY_SIZE = 16
MBR_SIGNATURE_OFFSET = 510

# PBR (FAT32 VBR) offsets
PBR_BPB_START = 3                     # BPB starts after jump instruction
PBR_BPB_END = 90                      # BPB is bytes 3-89 (87 bytes)
PBR_BPB_SIZE = PBR_BPB_END - PBR_BPB_START
PBR_RANDOM_OFFSET = 496              # 14 random bytes at 496-509
PBR_RANDOM_SIZE = 14

# CHS values for LBA mode (partition > 8 GB or LBA-only)
CHS_LBA_MODE = (0xFE, 0xFF, 0xFF)


def lba_to_chs_lba(lba):
    """For LBA mode, return the standard 0xFE/0xFF/0xFF tuple."""
    # For partitions that exceed CHS addressable range, or when we just
    # want LBA-only mode, use the standard sentinel values.
    return CHS_LBA_MODE


def build_partition_entry(status, part_type, lba_start, lba_count):
    """
    Build a 16-byte MBR partition table entry.

    Layout (16 bytes):
      [0]     Status byte (0x80 = active, 0x00 = inactive)
      [1-3]   CHS of first sector
      [4]     Partition type
      [5-7]   CHS of last sector
      [8-11]  LBA of first sector (little-endian uint32)
      [12-15] Number of sectors (little-endian uint32)
    """
    chs_start = lba_to_chs_lba(lba_start)
    chs_end = lba_to_chs_lba(lba_start + lba_count - 1)

    return struct.pack('<B3sB3sII',
                       status,
                       bytes(chs_start),
                       part_type,
                       bytes(chs_end),
                       lba_start,
                       lba_count)


def build_mbr(boot0_path, partition_lba_start, partition_lba_count):
    """
    Build a 512-byte MBR:
      - boot0 code in bytes 0-445
      - Partition 1: active FAT32 (type 0x0B)
      - Partitions 2-4: empty
      - 0x55AA signature
    """
    # Read boot0 (must be exactly 512 bytes, we use first 446)
    with open(boot0_path, 'rb') as f:
        boot0 = f.read()
    if len(boot0) < MBR_BOOT_CODE_SIZE:
        raise ValueError(f"boot0 is {len(boot0)} bytes, need at least {MBR_BOOT_CODE_SIZE}")

    mbr = bytearray(SECTOR_SIZE)

    # Boot code: bytes 0-445
    mbr[0:MBR_BOOT_CODE_SIZE] = boot0[:MBR_BOOT_CODE_SIZE]

    # Partition entry 1: active FAT32 (LBA)
    entry1 = build_partition_entry(
        status=0x80,          # Active/bootable
        part_type=0x0C,       # FAT32 with LBA (0x0C preferred over 0x0B for LBA)
        lba_start=partition_lba_start,
        lba_count=partition_lba_count
    )
    offset = MBR_PARTITION_TABLE_OFFSET
    mbr[offset:offset + MBR_PARTITION_ENTRY_SIZE] = entry1

    # Entries 2-4: zeroed (already zero from bytearray init)

    # Boot signature
    mbr[MBR_SIGNATURE_OFFSET] = 0x55
    mbr[MBR_SIGNATURE_OFFSET + 1] = 0xAA

    return bytes(mbr)


def build_pbr(boot1f32_path, bpb_from_mformat):
    """
    Build the PBR (Partition Boot Record / VBR) by merging:
      - boot1f32 boot code (jump + code after BPB)
      - BPB from mformat (bytes 3-89, preserves geometry/FAT params)
      - 14 random bytes at offset 496-509
    """
    with open(boot1f32_path, 'rb') as f:
        boot1 = bytearray(f.read())
    if len(boot1) != SECTOR_SIZE:
        raise ValueError(f"boot1f32 is {len(boot1)} bytes, expected {SECTOR_SIZE}")

    pbr = bytearray(boot1)

    # Preserve the BPB that mformat wrote (bytes 3-89)
    pbr[PBR_BPB_START:PBR_BPB_END] = bpb_from_mformat[PBR_BPB_START:PBR_BPB_END]

    # Write 14 random bytes at offset 496-509 (boot signature uniqueness)
    pbr[PBR_RANDOM_OFFSET:PBR_RANDOM_OFFSET + PBR_RANDOM_SIZE] = os.urandom(PBR_RANDOM_SIZE)

    # Ensure 0x55AA signature
    pbr[510] = 0x55
    pbr[511] = 0xAA

    return bytes(pbr)


def run(cmd, **kwargs):
    """Run a command, raise on failure."""
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        print(f"FAILED: {result.stderr.strip()}")
        sys.exit(1)
    return result


def main():
    parser = argparse.ArgumentParser(description='Build DUET Legacy Boot disk image')
    parser.add_argument('--size', type=int, default=4096,
                        help='Image size in MB (default: 4096 = 4 GB)')
    parser.add_argument('--output', default='bootai_duet.img',
                        help='Output image path (default: bootai_duet.img)')
    parser.add_argument('--boot0', default=None,
                        help='Path to boot0 (default: auto-detect)')
    parser.add_argument('--boot1f32', default=None,
                        help='Path to boot1f32 (default: auto-detect)')
    parser.add_argument('--label', default='BOOTAI',
                        help='FAT32 volume label (default: BOOTAI)')
    args = parser.parse_args()

    # Locate script directory for default boot file paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    boot0_path = args.boot0 or os.path.join(script_dir, 'boot0')
    boot1f32_path = args.boot1f32 or os.path.join(script_dir, 'boot1f32')

    for path, name in [(boot0_path, 'boot0'), (boot1f32_path, 'boot1f32')]:
        if not os.path.isfile(path):
            print(f"ERROR: {name} not found at {path}")
            sys.exit(1)

    image_path = os.path.abspath(args.output)
    image_size_bytes = args.size * 1024 * 1024
    total_sectors = image_size_bytes // SECTOR_SIZE
    partition_lba_count = total_sectors - PARTITION_START_LBA
    partition_size_bytes = partition_lba_count * SECTOR_SIZE

    print(f"=== DUET Legacy Boot Image Builder ===")
    print(f"  Image:      {image_path}")
    print(f"  Size:       {args.size} MB ({total_sectors} sectors)")
    print(f"  Partition:  LBA {PARTITION_START_LBA} - {total_sectors - 1} "
          f"({partition_lba_count} sectors, {partition_size_bytes // 1024 // 1024} MB)")
    print(f"  boot0:      {boot0_path}")
    print(f"  boot1f32:   {boot1f32_path}")
    print()

    # -----------------------------------------------------------------------
    # Step 1: Create empty image file
    # -----------------------------------------------------------------------
    print("[1/6] Creating empty image...")
    with open(image_path, 'wb') as f:
        f.seek(image_size_bytes - 1)
        f.write(b'\x00')
    print(f"  Created {args.size} MB sparse image")

    # -----------------------------------------------------------------------
    # Step 2: Write MBR (boot0 code + partition table)
    # -----------------------------------------------------------------------
    print("[2/6] Writing MBR with boot0 code + partition table...")
    mbr = build_mbr(boot0_path, PARTITION_START_LBA, partition_lba_count)
    with open(image_path, 'r+b') as f:
        f.seek(0)
        f.write(mbr)
    print(f"  MBR: boot0 code ({MBR_BOOT_CODE_SIZE} bytes) + "
          f"partition entry (active, FAT32 LBA, start={PARTITION_START_LBA})")

    # -----------------------------------------------------------------------
    # Step 3: Format the partition area as FAT32 using mformat
    # -----------------------------------------------------------------------
    print("[3/6] Formatting partition as FAT32 with mformat...")
    #
    # mformat parameters:
    #   -i image@@offset   Target the partition inside the image
    #   -F                 Force FAT32 (not FAT12/16)
    #   -v LABEL           Volume label
    #   -c 8               Cluster size = 8 sectors = 4 KB (good for large partitions)
    #   -T count           Total number of sectors in the partition
    #   -h 255 -s 63       Fake geometry for BPB (standard for LBA mode)
    #
    mformat_target = f"{image_path}@@{PARTITION_START_BYTES}"
    run(['mformat', '-i', mformat_target,
         '-F',                              # Force FAT32
         '-v', args.label,                  # Volume label
         '-c', '64',                        # 64 sectors/cluster = 32 KB (good for 4 GB)
         '-T', str(partition_lba_count),     # Total sectors
         '-h', '255',                       # Heads (BPB geometry)
         '-s', '63',                        # Sectors/track (BPB geometry)
         '::'])
    print(f"  Formatted {partition_size_bytes // 1024 // 1024} MB FAT32 partition")

    # -----------------------------------------------------------------------
    # Step 4: Read the BPB that mformat wrote, merge with boot1f32
    # -----------------------------------------------------------------------
    print("[4/6] Writing boot1f32 PBR (preserving BPB)...")

    # Read the PBR that mformat created (first sector of partition)
    with open(image_path, 'rb') as f:
        f.seek(PARTITION_START_BYTES)
        mformat_pbr = f.read(SECTOR_SIZE)

    # Merge: boot1f32 code + mformat's BPB + random signature
    pbr = build_pbr(boot1f32_path, mformat_pbr)

    # Write the merged PBR back
    with open(image_path, 'r+b') as f:
        f.seek(PARTITION_START_BYTES)
        f.write(pbr)

    print(f"  PBR: boot1f32 code + BPB from mformat + 14 random bytes at offset 496")

    # -----------------------------------------------------------------------
    # Step 5: Verify the image
    # -----------------------------------------------------------------------
    print("[5/6] Verifying image...")

    with open(image_path, 'rb') as f:
        # Check MBR
        f.seek(0)
        mbr_check = f.read(SECTOR_SIZE)
        assert mbr_check[510:512] == b'\x55\xaa', "MBR signature missing"
        assert mbr_check[446] == 0x80, "Partition 1 not active"
        assert mbr_check[450] == 0x0C, "Partition 1 not FAT32 LBA"
        lba_start = struct.unpack_from('<I', mbr_check, 454)[0]
        lba_count = struct.unpack_from('<I', mbr_check, 458)[0]
        assert lba_start == PARTITION_START_LBA, f"LBA start mismatch: {lba_start}"
        assert lba_count == partition_lba_count, f"LBA count mismatch: {lba_count}"

        # Check PBR
        f.seek(PARTITION_START_BYTES)
        pbr_check = f.read(SECTOR_SIZE)
        assert pbr_check[510:512] == b'\x55\xaa', "PBR signature missing"
        assert pbr_check[0:2] == b'\xeb\x63', "PBR jump instruction corrupted"
        # Verify BPB looks valid (bytes per sector should be 512)
        bps = struct.unpack_from('<H', pbr_check, 11)[0]
        assert bps == 512, f"BPB bytes_per_sector wrong: {bps}"
        # Verify random bytes were written (not all zeros)
        random_area = pbr_check[PBR_RANDOM_OFFSET:PBR_RANDOM_OFFSET + PBR_RANDOM_SIZE]
        assert random_area != b'\x00' * PBR_RANDOM_SIZE, "Random bytes area is all zeros"

    print(f"  MBR: OK (boot0, partition active, FAT32 LBA, LBA start={lba_start})")
    print(f"  PBR: OK (boot1f32, BPB valid, random signature present)")

    # -----------------------------------------------------------------------
    # Step 6: Create directory structure
    # -----------------------------------------------------------------------
    print("[6/6] Creating directory structure...")

    # Create directories using mmd
    for d in ['EFI', 'EFI/BOOT', 'EFI/OC', 'rwkvos', 'workspace']:
        run(['mmd', '-i', mformat_target, f'::{d}'])

    print(f"  Directories: EFI/BOOT, EFI/OC, rwkvos, workspace")

    # -----------------------------------------------------------------------
    # Done
    # -----------------------------------------------------------------------
    file_size = os.path.getsize(image_path)
    print()
    print(f"=== Image built successfully ===")
    print(f"  File:       {image_path}")
    print(f"  Size:       {file_size:,} bytes ({file_size // 1024 // 1024} MB)")
    print()
    print(f"To copy files into the image:")
    print(f"  mcopy -i {mformat_target} localfile.efi ::EFI/BOOT/BOOTX64.EFI")
    print(f"  mcopy -i {mformat_target} model.btw     ::rwkvos/model.btw")
    print()
    print(f"To write to USB (CAREFUL — destructive!):")
    print(f"  sudo dd if={image_path} of=/dev/rdiskN bs=1m")


if __name__ == '__main__':
    main()
