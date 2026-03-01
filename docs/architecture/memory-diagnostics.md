# Memory Diagnostics (`meminfo`)

The `meminfo` REPL command reports a full breakdown of UEFI memory usage by calling `GetMemoryMap()` from Boot Services.

## Usage

```
> meminfo
  LoaderCode: 1 MB + 256 KB (320 pages)
  LoaderData: 2 MB (512 pages)
  BS Code:    8 MB (2048 pages)
  BS Data:    4 MB (1024 pages)
  RT Code:    512 KB (128 pages)
  RT Data:    1 MB (256 pages)
  Free:       7934 MB (2031104 pages)
  ----
  Total: 7951 MB
  Used:  17 MB
  Free:  7934 MB
```

## Memory Types

| Type | Label | What It Is |
|------|-------|------------|
| EfiLoaderCode | LoaderCode | BootAI's executable code (the `.text` section of `all_bootai.efi`) |
| EfiLoaderData | LoaderData | BootAI's data — heap, stack, model weights, KV cache, activations |
| EfiBootServicesCode | BS Code | UEFI firmware code (drivers, protocols) |
| EfiBootServicesData | BS Data | UEFI firmware data structures |
| EfiRuntimeServicesCode | RT Code | UEFI runtime services (persist after ExitBootServices) |
| EfiRuntimeServicesData | RT Data | UEFI runtime data (NVRAM, clock, etc.) |
| EfiConventionalMemory | Free | Unallocated RAM available for use |
| EfiReservedMemoryType | Reserved | Firmware-reserved, not usable |

## How It Works

1. Calls `gBS->GetMemoryMap()` with a NULL buffer to get required size
2. Allocates a buffer and calls again to get the full memory map
3. Iterates all `EFI_MEMORY_DESCRIPTOR` entries, summing pages per type
4. Prints each type with MB/KB breakdown and page count
5. Prints total/used/free summary

Each UEFI page is 4096 bytes (4 KB).

## Why This Matters

BootAI runs with a fixed 8GB RAM budget. Knowing exactly how much the EFI binary and firmware consume tells us how much is left for model weights, KV cache, and activations. This determines which models fit.

## Files

| File | What |
|------|------|
| `efi/main.c` | `meminfo` command handler in `ba_exec_cmd()` |
| `efi/efi.h` | `EFI_MEMORY_DESCRIPTOR` struct, `EFI_MEMORY_TYPE` enum |
