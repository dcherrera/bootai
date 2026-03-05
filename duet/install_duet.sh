#!/bin/bash
# Write DUET Legacy Boot image to USB
# Usage: sudo ./install_duet.sh /dev/diskN
#
# Writes bootai_duet.img (built by build_image.py) to the target disk.

set -e
cd "$(dirname "$0")" || exit 1

IMG="bootai_duet.img"

if [ -z "$1" ]; then
    echo "Usage: sudo ./install_duet.sh /dev/diskN"
    echo ""
    echo "Available external disks:"
    diskutil list external
    exit 1
fi

# Accept /dev/diskN or just diskN
DISK="$1"
RDISK="${DISK/disk/rdisk}"

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found. Run build_image.py first."
    exit 1
fi

IMG_SIZE=$(stat -f%z "$IMG")
echo "=== DUET Legacy Boot Installer ==="
echo "  Image:  $IMG ($((IMG_SIZE / 1024 / 1024)) MB)"
echo "  Target: $RDISK"
echo ""

# Safety check
read -p "This will ERASE $DISK. Continue? [y/N] " confirm
if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
    echo "Aborted."
    exit 1
fi

echo "[1/3] Unmounting disk..."
diskutil unmountDisk "$DISK" || true

echo "[2/3] Writing image..."
dd if="$IMG" of="$RDISK" bs=1m

echo "[3/3] Ejecting..."
diskutil eject "$DISK"

echo ""
echo "=== Done ==="
echo "Plug into Dell and boot. Legacy BIOS will auto-chain: boot0 → boot1f32 → DUET → BootAI"
