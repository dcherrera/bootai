# PROD Build Flag

The `PROD=1` make flag builds a production EFI binary that skips the boot menu and REPL, booting straight into the chat inference loop.

## Boot Flow Comparison

```
Normal:  efi_main → app_main → init → boot menu → select 1 → REPL → type "chat" → inference
PROD:    efi_main → app_main → init → load model → inference loop directly
```

## Usage

```bash
make run-prod                     # clean build + run in QEMU (straight to chat)
make clean && make PROD=1 efi    # build only (for flashing USB)
make release                      # build PROD + GPT disk image (flashable)
make clean && make efi            # normal menu (unchanged)
```

## How It Works

The Makefile passes `-DPROD` to CFLAGS when `PROD=1` is set. In `efi/main.c`, `app_main()` uses `#ifdef PROD` to choose between two paths:

- **PROD**: Calls `ensure_model_loaded()` then `command_chat()` directly. When the user exits chat (empty line), prints "Goodbye." and halts the CPU.
- **Normal**: Shows the boot menu in a loop, option 1 enters the command REPL where the user can type `chat` to start inference.

Initialization (filesystem, drivers, networking, WiFi auto-connect, tensor self-test) runs in both modes — only the menu/REPL is skipped.

## Release Image (`make release`)

`make release` builds a PROD EFI binary and packages it into a GPT disk image (`bootai-release.img`) that can be flashed to USB with balenaEtcher or `dd`.

### Why GPT?

The existing `bootai.img` is a bare FAT32 filesystem with no partition table. balenaEtcher writes byte-for-byte, so UEFI firmware won't find a bootable ESP without a GPT wrapper. The release image adds a proper GPT partition table with an EFI System Partition.

### Image Layout

```
bootai-release.img
├── GPT header (LBA 0-33)
├── Partition 1: EFI System Partition (EF00), starts at sector 2048
│   ├── EFI/BOOT/BOOTX64.EFI    ← PROD build of all_bootai.efi
│   ├── rwkvos/model.btw         ← quantized weights
│   ├── rwkvos/vocab.btv         ← tokenizer vocabulary
│   ├── wifi.conf                ← WiFi config template
│   ├── workspace/               ← user files
│   └── drivers/                 ← optional UEFI drivers
└── GPT backup header (last 34 sectors)
```

### How It Works

1. Clean build with `PROD=1` produces `all_bootai.efi`
2. Calculate image size: model + vocab + 50MB headroom + 2MB GPT overhead
3. `dd` creates a zeroed raw image
4. `sgdisk` writes a GPT with one EF00 (EFI System) partition starting at sector 2048
5. `mformat` formats the ESP as FAT32 using `@@offset` syntax to address the partition inside the image
6. `mmd`/`mcopy` populate the ESP with EFI binary, model, vocab, and directory structure

### Dependencies

- `sgdisk` — GPT partition tool (`brew install gptfdisk`)
- `mtools` — FAT32 manipulation without mounting (`brew install mtools`)

### Flashing

```bash
# balenaEtcher (GUI — recommended)
# Open balenaEtcher → Select bootai-release.img → Select USB → Flash

# dd (command line)
sudo dd if=bootai-release.img of=/dev/diskN bs=1m
diskutil eject /dev/diskN
```

### Verification

```bash
sgdisk -p bootai-release.img    # shows GPT with one EF00 partition
mdir -i bootai-release.img@@1048576 ::  # lists ESP root
```

## Files

| File | Change |
|------|--------|
| `Makefile` | `ifdef PROD` adds `-DPROD` to CFLAGS; `release` target creates GPT image |
| `efi/main.c` | `#ifdef PROD` around boot menu loop in `app_main()` |
