# Phase 2: UEFI Networking — Complete

## Status: WORKING

DHCP, TCP4, HTTP/1.0 GET, and DNS hostname resolution all work end-to-end in QEMU. DNS uses a custom UDP4 resolver (OVMF's DNS4 driver has a firmware bug).

## What's Done

### Networking Features
- **DHCP**: Automatic IP configuration via `EFI_IP4_CONFIG2_PROTOCOL` (POLICY_DHCP)
- **TCP4**: Full connect/send/recv/close over `EFI_TCP4_PROTOCOL` via service binding
- **HTTP/1.0 GET**: Raw TCP4 with `GET /path HTTP/1.0\r\n` — strips headers, returns body
- **URL parsing**: Extracts host, port (default 80), and path from `http://host:port/path`
- **REPL commands**: `fetch <url>`, `netinfo`, `exit`

### Test Results
```
> netinfo
  Network: ready

> fetch http://10.0.2.2:8888/
  --- Response (87 bytes) ---
  <h1>BootAI Phase 2 Complete!</h1>
  <p>UEFI networking works: DHCP + TCP4 + HTTP/1.0</p>
  --- End ---

> fetch http://10.0.2.2:8888/test
  --- Response (87 bytes) ---
  (same response — test server returns same HTML for all paths)
  --- End ---

> exit
  (returns to boot menu)
```

### Code Changes

| File | Changes |
|------|---------|
| `efi/efi.h` | Added `EFIAPI` (`ms_abi`) to all function pointers; TCP4/IP4Config2/DNS4 types and GUIDs |
| `efi/net.c` | Full rewrite: TCP4/DHCP/HTTP implementation, ConnectController, URL/IP parsing |
| `efi/main.c` | `ms_abi` on `efi_main`; command REPL with fetch/netinfo/exit; net init on boot |
| `Makefile` | `net.c` in phase1 build; `-shared -Bsymbolic` link; split OVMF pflash; `-device virtio-rng-pci` |

## Bugs Fixed

### 1. Calling Convention Mismatch (latent in Phase 1)
UEFI uses Microsoft x64 ABI (args in RCX, RDX, R8, R9) but `x86_64-elf-gcc` defaults to System V (RDI, RSI, RDX, RCX). Phase 1 "worked" because serial output uses direct port I/O.

**Fix**: Added `#define EFIAPI __attribute__((ms_abi))` to all UEFI function pointer types.

### 2. Position-Independent Code / Linker Relaxation
Static linking (`ld -nostdlib`) relaxed GOT-relative accesses to absolute addresses. With `ImageBase=0`, all globals pointed to wrong memory.

**Fix**: Changed to `ld -nostdlib -shared -Bsymbolic` — keeps RIP-relative addressing.

### 3. Network Drivers Not Loading (DEPEX unsatisfied)
All network drivers (MnpDxe, ArpDxe, Ip4Dxe, TcpDxe, etc.) were "discovered but not loaded" because EDK2's `DxeNetLib.inf` adds `gEfiRngProtocolGuid` as a hard dependency for all network drivers.

**Fix**: Added `-device virtio-rng-pci` to QEMU command to provide hardware RNG.

### 4. WaitForEvent with EVT_NOTIFY_SIGNAL
TCP4/DNS4 completion tokens use `EVT_NOTIFY_SIGNAL` events. UEFI spec forbids `WaitForEvent` with signal events — returns `EFI_INVALID_PARAMETER`.

**Fix**: Replaced all `WaitForEvent` calls with polling loops checking `CompletionToken.Status != EFI_NOT_READY`.

### 5. Custom OVMF Firmware Required
Neither Homebrew OVMF nor edk2-nightly include IPv4/TCP network stack drivers.

**Fix**: Built OVMF from EDK2 source with:
```
-D NETWORK_ENABLE=TRUE -D NETWORK_IP4_ENABLE=TRUE -D NETWORK_HTTP_ENABLE=TRUE -D FD_SIZE_4MB
```
Build command: `GCC5_BIN=/opt/homebrew/bin/x86_64-elf- build -a X64 -t GCC5 -p OvmfPkg/OvmfPkgX64.dsc -b RELEASE`

## Known Limitations

- **DNS via UDP4 only**: OVMF's DNS4 driver crashes with #GP. Workaround: custom UDP4 DNS resolver (RFC 1035 wire format). Hardcoded to QEMU forwarder at 10.0.2.3.
- **QEMU user-mode only**: External IPs don't route properly on macOS. Use `10.0.2.2` (QEMU gateway to host).
- **Duplicate serial output**: Both ConOut and serial show output lines (cosmetic issue).
- **HTTP only**: No HTTPS/TLS support.

## Architecture Notes

### UEFI Driver Model
Drivers are loaded but not bound until `ConnectController` is called. Our `connect_all_controllers()` iterates all handles twice, calling `gBS->ConnectController(handle, NULL, NULL, TRUE)` recursively to bind the full chain: VirtioNetDxe (SNP) → MnpDxe → ArpDxe → IP4Dxe → TCP4Dxe.

### TCP4 Service Binding Pattern
```
tcp4_sb->CreateChild(&child_handle)
gBS->HandleProtocol(child_handle, &gEfiTcp4ProtocolGuid, &tcp4)
tcp4->Configure(config)  // UseDefaultAddress=TRUE, retry on EFI_NO_MAPPING
// Poll CompletionToken.Status (NOT WaitForEvent)
tcp4->Connect(token)
tcp4->Transmit(token)
tcp4->Receive(token)
tcp4->Close(token)
tcp4_sb->DestroyChild(child_handle)
```

### HTTP/1.0 Strategy
Raw TCP4, not UEFI HTTP protocol. Send `GET /path HTTP/1.0\r\nHost: ...\r\nConnection: close\r\n\r\n`, receive until connection closes, strip headers at `\r\n\r\n` boundary.

### Build Pipeline
```
Compile:  x86_64-elf-gcc -fPIC -fshort-wchar -ffreestanding -mno-red-zone ...
Link:     x86_64-elf-ld -T efi/uefi.lds -nostdlib -shared -Bsymbolic -o bootai.so
Convert:  x86_64-elf-objcopy -j .text .data .bss .dynamic .dynsym .rel .rela .reloc
                              -O pei-x86-64 --subsystem efi-app bootai.so bootai.efi
```

### OVMF Firmware (split pflash)
```
ovmf-code.fd  — readonly code (pflash0)
ovmf-vars.fd  — writable NVRAM (pflash1, copied per-session)
```
Built from EDK2 source at `/tmp/edk2`. Not tracked in git — rebuild instructions above.
