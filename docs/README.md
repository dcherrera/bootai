# BootAI Documentation

UEFI application that boots directly into AI inference — no kernel, no OS.

## Specs & Plans

- [BootAI Roadmap](specs/bootai-roadmap.md) — 6-phase plan from boot to full AI assistant

## Architecture

- [UEFI Boot Architecture](architecture/uefi-boot-architecture.md) — Boot flow, memory model, build pipeline, driver binding
- [Networking](architecture/networking.md) — DHCP, TCP4, UDP4 DNS, HTTP/1.0, mDNS/Bonjour discovery
- [Inference on Bare Metal](architecture/inference-on-bare-metal.md) — Model conversion, disk image creation, running SmolLM2-135M
- [HTTP Server](architecture/http-server.md) — OpenAI-compatible API, TCP4 listen/accept, JSON parsing, chat completions
- [WiFi & Driver Loader](architecture/wifi-and-drivers.md) — UEFI driver loading from disk, WiFi scan/connect via WiFi2 protocol, wifi.conf auto-connect
- [IWN WiFi Driver](architecture/iwn-wifi-driver.md) — Native Intel 6200 WiFi driver ported from OpenBSD: PCI probe, MMIO/EEPROM, DMA/firmware, scanning, auth/assoc, WPA2 EAPOL, data TX/RX, SNP integration
- [Console Logging](architecture/console-logging.md) — Tee all console output to `\bootai.log` on USB for post-session debugging

## Research

- [Phase 2 Debug Log](research/phase2-debug-log.md) — Networking bring-up: bugs found, fixes applied
- [IWN Phase 3 Debug Log](research/iwn-phase3-debug-log.md) — WiFi scanning bring-up: 10 bugs found on Dell E6510 (BT coex, FH_INT, missing commands, probe frame)

## Adding Documentation

1. Pick the right category:
   - **specs/** — Plans, roadmaps, feature specifications
   - **architecture/** — Internal system documentation
   - **research/** — Investigation logs, debug sessions

2. Use kebab-case filenames (`uefi-boot-architecture.md`, not `UEFIBootArchitecture.md`)

3. Update this index with a link to your new document

## Quick Links

| Resource | Location |
|----------|----------|
| Main README | `../README.md` |
| Build commands | `../Makefile` |
| UEFI types | `../efi/efi.h` |
| Network stack | `../efi/net.c` |
| mDNS responder | `../efi/mdns.c` |
| WiFi management | `../efi/wifi.c` |
| Driver loader | `../efi/drivers.c` |
| HTTP server | `../efi/http_server.c` |
| API handlers | `../efi/api.c` |
| Entry point | `../efi/main.c` |
| Project guidelines | `../CLAUDE.md` |
