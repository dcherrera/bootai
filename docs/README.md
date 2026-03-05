# BootAI Documentation

UEFI application that boots directly into AI inference — no kernel, no OS.

## Specs & Plans

- [BootAI Roadmap](specs/bootai-roadmap.md) — 6-phase plan from boot to full AI assistant
- [e1000e SNP Driver](specs/e1000e-snp-spec.md) — Intel wired Ethernet driver spec: 82577LM hardware, register map, descriptor rings, SNP integration
- [e1000e SNP Plan](specs/e1000e-snp-plan.md) — Phased implementation plan: PCI probe → MMIO → RX/TX rings → SNP → Dell test

## Architecture

- [UEFI Boot Architecture](architecture/uefi-boot-architecture.md) — Boot flow, memory model, build pipeline, driver binding
- [Networking](architecture/networking.md) — DHCP, TCP4, UDP4 DNS, HTTP/1.0, mDNS/Bonjour, custom network stack (ARP/DHCP/IP/UDP/TCP on raw SNP)
- [Inference on Bare Metal](architecture/inference-on-bare-metal.md) — Model conversion, disk image creation, running SmolLM2-135M
- [HTTP Server](architecture/http-server.md) — OpenAI-compatible API, TCP4 listen/accept, JSON parsing, chat completions (non-streaming and SSE streaming)
- [WiFi & Driver Loader](architecture/wifi-and-drivers.md) — UEFI driver loading from disk, WiFi scan/connect via WiFi2 protocol, wifi.conf auto-connect
- [e1000e Ethernet Driver](architecture/e1000e-ethernet-driver.md) — Native Intel 82577LM wired Ethernet driver: PCI probe, DMA descriptor rings, raw TX/RX, ns_* stack integration
- [IWN WiFi Driver](architecture/iwn-wifi-driver.md) — Native Intel 6200 WiFi driver ported from OpenBSD: PCI probe, MMIO/EEPROM, DMA/firmware, scanning, auth/assoc, WPA2 EAPOL, data TX/RX, SNP integration
- [Console Logging](architecture/console-logging.md) — Tee all console output to `\bootai.log` on USB for post-session debugging
- [PROD Build](architecture/prod-build.md) — `PROD=1` flag to skip boot menu and boot straight into chat inference
- [Management API](architecture/management-api.md) — Remote debugging over Ethernet: execute commands, stream logs, health check (`/api/cmd`, `/api/log`, `/api/health`)
- [Memory Diagnostics](architecture/memory-diagnostics.md) — `meminfo` REPL command: UEFI memory map breakdown by type (loader, firmware, free)
- [HD Install](architecture/hd-install.md) — Install to hard drive: GPT partitioning, FAT32 ESP, raw data partition with BTAI TOC for unlimited model sizes, boot path model loading via Block IO
- [Weight Loading Strategy](architecture/weight-loading-strategy.md) — F32 pre-conversion for small models, F16/INT8 strategies for larger models, memory ownership and loader architecture
- [DUET Legacy Boot](architecture/duet-legacy-boot.md) — Legacy BIOS boot via DUET: MBR boot chain, image building, PS/2 keyboard fallback, plug-and-play on any x86-64 machine

## Guides

- [Connecting to BootAI](guides/connecting-to-bootai.md) — Connect from OpenCode, curl, Python, Open WebUI, or any OpenAI-compatible tool over Ethernet

## Research

- [Phase 2 Debug Log](research/phase2-debug-log.md) — Networking bring-up: bugs found, fixes applied
- [IWN Phase 3 Debug Log](research/iwn-phase3-debug-log.md) — WiFi scanning bring-up: 10 bugs found on Dell E6510 (BT coex, FH_INT, missing commands, probe frame)
- [Phase 5c Debug Log](research/phase5-debug-log.md) — Custom network stack bring-up: SNP function pointer hang on Dell, direct IWN bypass fix

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
| Custom stack types | `../efi/ns_net.h` |
| Custom stack core | `../efi/ns_net.c`, `ns_arp.c`, `ns_ip.c`, `ns_udp.c`, `ns_dhcp.c`, `ns_tcp.c` |
| Network diagnostics | `../efi/net_diag.c` |
| mDNS responder | `../efi/mdns.c` |
| e1000e driver | `../e1000e/e1000e_hw.c` |
| WiFi management | `../efi/wifi.c` |
| Driver loader | `../efi/drivers.c` |
| HTTP server | `../efi/http_server.c` |
| API handlers | `../efi/api.c` |
| Management API | `../efi/mgmt_api.c` |
| HD installer | `../efi/install.c` |
| Entry point | `../efi/main.c` |
| Project guidelines | `../CLAUDE.md` |
