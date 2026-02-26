# Console Logging (`bootai.log`)

All console output is tee'd to `\bootai.log` on the USB drive, giving a full transcript of each boot session. This is essential for debugging on real hardware where you can't scroll up.

## Architecture

```
efi_print_ascii("text")
    │
    ├──► log_buf[] (128KB static buffer, append)
    │
    └──► ConOut->OutputString() (screen)

efi_log_flush()  [called at top of REPL loop]
    │
    └──► efi_write_file("bootai.log", log_buf, log_pos)
              │
              └──► Open(CREATE) + Write + Close  (FAT32 on USB)
```

## Implementation

### Files

| File | Changes |
|------|---------|
| `efi/console.c` | Log buffer, `efi_log_flush()`, tee in `efi_print_ascii()` |
| `efi/fs.c` | `efi_fs_root()` getter for the static `fs_root` handle |
| `efi/efi.h` | Declarations for `efi_fs_root()`, `efi_log_flush()`, typed `Flush` in `EFI_FILE_PROTOCOL` |
| `efi/main.c` | `efi_log_flush()` call at top of REPL loop |

### Buffer approach (not persistent file handle)

Early attempts used a persistent `EFI_FILE_PROTOCOL` file handle — Open once, Write on every print, Flush to sync. This failed on Dell hardware: `Open` and `Write` both returned `EFI_SUCCESS`, but no data appeared on disk. The Dell's UEFI FAT driver does not honor `Flush()` on persistent handles.

The working approach: accumulate in a static 128KB buffer, then call `efi_write_file()` (open-write-close) to dump the entire buffer. This uses the same code path as `wifi.conf` saving, which is proven to work.

### Flush timing

`efi_log_flush()` runs at the top of the main REPL `for(;;)` loop, before printing `> `. This means:

- Every command's complete output is flushed before the next prompt
- `continue;` statements in command handlers jump to the top of the loop, triggering the flush
- The flush overwrites `bootai.log` with the full buffer contents each time (not append)

### What's captured vs. not

| Captured | Not captured |
|----------|-------------|
| All `efi_print_ascii()` output | `efi_print()` (UCS-2 direct) calls |
| Boot messages, driver output | User-typed command text (echoed via `efi_print()`) |
| IWN scan/connect output | Output from commands still running at power-off |
| Error messages | Output beyond 128KB |

## Reading the log

```bash
# Mount USB on Mac
diskutil list external           # Find disk (e.g. /dev/disk11)
diskutil mount /dev/disk11s1
cat /Volumes/BOOTAI/bootai.log

# Or to keep for later
cp /Volumes/BOOTAI/bootai.log ~/Desktop/bootai-session.log
```

## Debugging workflow

1. Flash USB with new build: `cp all_bootai.efi /Volumes/BOOTAI/EFI/BOOT/BOOTX64.EFI`
2. Eject: `diskutil eject /dev/disk11`
3. Boot Dell from USB, run commands
4. Wait for `> ` prompt (ensures flush)
5. Power off, plug USB into Mac
6. Read log: `cat /Volumes/BOOTAI/bootai.log`

## Limitations

- **128KB max** — static buffer, no dynamic allocation (UEFI environment)
- **No mid-command flush** — long-running commands like `iwn connect` won't be captured if you power off before they complete
- **Single session** — each boot overwrites the previous log
- **ASCII only** — `efi_print()` (UCS-2) calls bypass the log
