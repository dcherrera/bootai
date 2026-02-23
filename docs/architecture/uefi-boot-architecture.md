# UEFI Boot Architecture

Internal architecture documentation for BootAI's UEFI boot environment.

## Overview

BootAI is a UEFI application that runs entirely in boot services mode. It never calls `ExitBootServices`, which means the firmware continues to provide protocol services (filesystem, networking, display, keyboard) throughout the application's lifetime.

The binary is a PE32+ executable loaded by the UEFI firmware from the EFI System Partition (FAT32) at `\EFI\BOOT\BOOTX64.EFI`.

## Boot Flow

```
UEFI Firmware
  → Load BOOTX64.EFI from ESP
  → Call efi_main(ImageHandle, SystemTable)
     → Save gST, gBS globals
     → efi_clear_screen()
     → efi_memory_init()          — set up bump allocator on AllocatePages
     → efi_net_init()             — ConnectController, DHCP, locate TCP4/UDP4
     → ba_model_load()            — load model.btw + vocab.btv from \rwkvos\
     → show_boot_menu()           — [1] Run Live  [2] Install  [3] Settings
     → command_repl()             — chat/fetch/netinfo/exit loop
```

## Memory Model

### Allocation Strategy

UEFI provides two allocation APIs:
- `AllocatePages(type, memtype, pages, &addr)` — 4KB-aligned page allocation
- `AllocatePool(memtype, size, &buf)` — arbitrary-sized pool allocation

BootAI uses `AllocatePages` with `EfiLoaderData` memory type for a 1MB bump heap (`efi_memory.c`). This avoids per-allocation overhead and gives us a simple, fast allocator.

### Memory Map

```
0x00000000 - 0x000FFFFF    Reserved (real-mode IVT, BDA, etc.)
0x00100000 - ...           UEFI firmware, drivers, protocols
              ...          BootAI .text, .data, .bss (loaded by firmware)
              ...          1MB bump heap (AllocatePages)
              ...          Network buffers, TCP4/UDP4 child instances
              ...          Free conventional memory
```

### Future: JACLibc Integration

JACLibc's bare-metal path (`os/none.h`) stubs all syscalls to `-ENOSYS`. To enable `malloc`/`printf`:
- Override `mmap()` to call `gBS->AllocatePages()`
- Wire `stdout` to `gST->ConOut->OutputString()`
- Wire `stdin` to `gST->ConIn->ReadKeyStroke()`

## Driver Binding

UEFI drivers are loaded into memory by the firmware but not automatically bound to devices. The `ConnectController` call triggers the driver binding protocol, which matches drivers to devices.

### The Problem

Network protocols (TCP4, UDP4, DNS4) depend on a chain of drivers:

```
VirtioNetDxe (SNP)  →  MnpDxe  →  ArpDxe  →  Ip4Dxe  →  Tcp4Dxe / Udp4Dxe
```

Each driver's DEPEX (dependency expression) requires the previous layer's protocol to be installed. A single `ConnectController` pass may not bind the full chain because child handles are created during binding.

### The Fix

Two-pass recursive connection over all handles:

```c
static void connect_all_controllers(void) {
    // Pass 1: bind all existing handles
    LocateHandleBuffer(ALL_HANDLES) → ConnectController(handle, NULL, NULL, TRUE)

    // Pass 2: bind newly-created child handles
    LocateHandleBuffer(ALL_HANDLES) → ConnectController(handle, NULL, NULL, TRUE)
}
```

Called twice at init (`efi_net_init`) to ensure the full driver chain is loaded. Called again after DHCP completes (`efi_net_post_dhcp_connect`) to bind upper-layer protocols that depend on IP configuration.

## Protocol Usage

| Protocol | GUID Variable | Purpose |
|----------|--------------|---------|
| `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` | (in SystemTable) | Serial/screen text output |
| `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` | (in SystemTable) | Keyboard input |
| `EFI_GRAPHICS_OUTPUT_PROTOCOL` | `gEfiGraphicsOutputProtocolGuid` | Framebuffer rendering |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` | `gEfiSimpleFileSystemProtocolGuid` | FAT32 file access |
| `EFI_IP4_CONFIG2_PROTOCOL` | `gEfiIp4Config2ProtocolGuid` | DHCP configuration |
| `EFI_TCP4_PROTOCOL` | `gEfiTcp4ProtocolGuid` | TCP connections |
| `EFI_UDP4_PROTOCOL` | `gEfiUdp4ProtocolGuid` | DNS queries over UDP |

## Build Pipeline

### Compilation

```bash
x86_64-elf-gcc -ffreestanding -mno-red-zone -msse4.2 -fno-stack-protector \
               -fPIC -fshort-wchar -O2 -Wall -Wextra -std=c11 -I . -c file.c -o file.o
```

Key flags:
- `-ffreestanding` — no libc, no standard startup
- `-mno-red-zone` — UEFI uses Microsoft ABI; red zone isn't safe
- `-msse4.2` — enable SSE up to SSE4.2 (UEFI guarantees SSE state is configured)
- `-O2` — optimization (critical for inference performance on real hardware)
- `-fPIC` — position-independent code (firmware loads at arbitrary address)
- `-fshort-wchar` — `wchar_t` = 16-bit (matches UEFI `CHAR16`)

### Linking

```bash
x86_64-elf-ld -T efi/uefi.lds -nostdlib -shared -Bsymbolic -o bootai.so *.o
```

Key flags:
- `-shared -Bsymbolic` — produces a shared ELF with RIP-relative addressing. This is critical: static linking allows the linker to relax GOT-relative accesses to absolute addresses, which break when the firmware loads the binary at a non-zero `ImageBase`.
- `-T efi/uefi.lds` — custom linker script with `ENTRY(efi_main)` and proper section layout

### PE32+ Conversion

```bash
x86_64-elf-objcopy -j .text -j .data -j .bss -j .dynamic -j .dynsym \
                   -j .rel -j .rela -j .reloc \
                   -O pei-x86-64 --subsystem efi-app \
                   bootai.so bootai.efi
```

This converts the ELF shared object to a PE32+ executable that UEFI firmware can load. The `.reloc` section contains PE relocations that the firmware applies at load time.

## Key Gotchas

### Microsoft x64 ABI

All UEFI function pointers must be declared with `__attribute__((ms_abi))` (the `EFIAPI` macro). GCC defaults to System V AMD64 ABI (args in RDI, RSI, RDX, RCX), while UEFI uses Microsoft ABI (args in RCX, RDX, R8, R9). Mismatched calling conventions cause silent corruption.

### Shared Linking is Mandatory

Using `-shared -Bsymbolic` instead of static linking preserves RIP-relative addressing. Without this, global variable accesses resolve to absolute address 0 (since `ImageBase=0` in our linker script), causing memory corruption.

### virtio-rng-pci Required

EDK2's network drivers have a hard dependency on `gEfiRngProtocolGuid` (added in `DxeNetLib.inf`). Without a hardware RNG device, all network drivers fail their DEPEX check and never load. Fix: add `-device virtio-rng-pci` to QEMU.

### EVT_NOTIFY_SIGNAL Polling

UEFI's `WaitForEvent` does not work with `EVT_NOTIFY_SIGNAL` events (returns `EFI_INVALID_PARAMETER`). All async operations (TCP4 connect/send/recv, UDP4 transmit/receive) must use polling loops that check `CompletionToken.Status != EFI_NOT_READY`.

### Custom OVMF Build

Stock Homebrew and edk2-nightly OVMF builds don't include IPv4/TCP4 network drivers. Must build from EDK2 source:

```bash
GCC5_BIN=/opt/homebrew/bin/x86_64-elf- build -a X64 -t GCC5 \
    -p OvmfPkg/OvmfPkgX64.dsc -b RELEASE \
    -D NETWORK_ENABLE=TRUE -D NETWORK_IP4_ENABLE=TRUE \
    -D NETWORK_HTTP_ENABLE=TRUE -D FD_SIZE_4MB
```

### Split pflash

OVMF uses split pflash: `ovmf-code.fd` (readonly code) and `ovmf-vars.fd` (writable NVRAM). The vars file is copied per-session to avoid corruption.

### No AVX on UEFI

UEFI firmware does not enable AVX. The `CR4.OSXSAVE` bit is not set and XSAVE state is not configured, so any VEX-encoded instruction (including `vmovaps`, `vcvtph2ps`, etc.) triggers a `#UD` (Invalid Opcode) exception.

This means:
- Never use `-mavx`, `-mavx2`, `-mf16c`, or `-mfma` compiler flags
- SSE (including SSE4.2) is safe — UEFI guarantees SSE state
- The F16C hardware instruction for f16→f32 conversion uses VEX encoding and cannot be used
- GCC will emit VEX instructions throughout the binary (not just in f16 code) when AVX-family flags are enabled

BootAI was initially compiled with `-mf16c` which worked in QEMU (which enables AVX) but caused `#UD` crashes on real hardware (Surface Pro 7).

## Source Files

| File | Purpose |
|------|---------|
| `efi/main.c` | Entry point, boot menu, command REPL |
| `efi/efi.h` | UEFI type definitions, protocol structs, function declarations |
| `efi/net.c` | Networking: DHCP, TCP4, UDP4 DNS, HTTP/1.0 |
| `efi/console.c` | Framebuffer text rendering via GOP |
| `efi/input.c` | Keyboard input via Simple Text Input |
| `efi/fs.c` | File operations via Simple File System |
| `efi/efi_memory.c` | Memory allocation (bump heap on AllocatePages) |
| `efi/reloc.c` | PE relocation fixup handler |
| `efi/uefi.lds` | Linker script for ELF → PE32+ conversion |
| `efi/uefi_libc.c` | Freestanding libc: malloc, printf, 29 math functions |
| `model/qwen2.c` | Qwen2/SmolLM forward pass (RMSNorm, GQA, SwiGLU, RoPE) |
| `model/loader.c` | Weight loader (.btw binary format) |
| `model/sampling.c` | Token sampling (temperature, top-k, top-p) |
| `model/rope.c` | Rotary position embeddings |
| `model/kv_cache.c` | KV cache (per-layer, oldest-half eviction) |
| `tokenizer/bpe.c` | BPE tokenizer with special token handling |
| `tools/export_weights.py` | Model converter (HuggingFace → .btw/.btv) |
