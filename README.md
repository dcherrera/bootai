## BootAI

Boot straight into AI. A UEFI application that runs language model inference on bare metal — no kernel, no OS.

Plug in a USB drive, power on, and you get a REPL with a language model running directly on UEFI boot services. Networking, WiFi, and TCP/IP are hand-written on top of firmware protocols and raw Intel NIC drivers.

## What It Does

- **Chat / code generation** — Qwen2/Qwen3-family transformer inference (RoPE, GQA, RMSNorm, SwiGLU), INT8 weights, running in boot services mode

- **REPL** — `chat`, `ls`, `cat`, `fetch`, `wifi`, `serve`, `install`, `models`, `load`, `meminfo`, and more

- **WiFi** — from-scratch Intel wireless driver (`iwn/`): PCI probe, firmware load, scan, association, EAPOL/CCMP crypto

- **Ethernet** — from-scratch Intel `e1000e` driver

- **TCP/IP stack** — hand-rolled ARP, IP, UDP, TCP, and DHCP over the raw NIC drivers, for firmware with no UEFI network stack

- **HTTP server** — management and chat API over the LAN (`serve`), bearer-token authenticated

- **Network discovery** — advertises `bootai.local` via a real mDNS responder (started by `serve`)

- **Install to disk** — standalone bootable install, with DUET legacy-BIOS support

### Not yet implemented

Scaffolded in the tree but not working — listed so the feature list above stays honest:

- **Tool use / agentic layer** — `tools/*.c` are stubs; the model cannot call tools

- **Web search** — not implemented

- **RWKV-X** — `model/rwkvx.c` and `model/wkv7.c` are TODO stubs; all inference dispatches to the Qwen path

- **Q4 quantization** — only Q8 (INT8) is implemented end to end

- **Conversation history** — not persisted; `bootai.log` is a console transcript, not a conversation store

## Models

Weights are exported to a flat `.btw` format with a paired `.btv` vocab.

| Model | Size | Quantization | Notes |
| --- | --- | --- | --- |
| Qwen2.5-Coder-0.5B-Instruct | 0.5B | Q8 (INT8) | Default build target |
| Qwen2.5-Coder-3B-Instruct | 3B | Q8 (INT8) | Largest tested |
| SmolLM2-135M-Instruct | 135M | Q8 (INT8) | Smallest fallback |

Load order: flat `\rwkvos\model.btw`, then `qwen25-coder-05b`, `qwen25-coder-3b`, `smollm-135m`, then the raw BTAI partition on hard-drive installs.

Weights are **not** shipped in this repo — export them yourself:

```
python3 tools/export_weights.py --model Qwen/Qwen2.5-Coder-0.5B-Instruct --quant q8 --output model.btw
```

RAM has to fit the weights plus KV cache and activations. There is no enforced minimum in code.

## How It Works

No kernel. No OS. The `.efi` binary never calls `ExitBootServices`, so firmware services stay available:

- **Filesystem** — Simple File System Protocol (FAT32)

- **Networking** — TCP4 and UDP4 protocols; `Ip4Config2` for DHCP where firmware provides it, otherwise the hand-rolled stack in `efi/ns_*.c` over `iwn` / `e1000e`

- **HTTP** — hand-rolled HTTP/1.0 over TCP4, plain HTTP only, no TLS. `EFI_HTTP_PROTOCOL` is not used

- **Console** — text mode via `ConOut` / `ConIn` (Simple Text Input, with a direct PS/2 fallback for DUET)

- **NVRAM** — `Boot0001` / `BootOrder` boot-manager entries during install. App settings live in `\wifi.conf` on the FAT partition, not in NVRAM

- **Events** — async I/O completion tokens via `CreateEvent` / `WaitForEvent`

## Stack

```
Inference REPL (prompt → tokens → generate → display)
    ↓
Model Forward Pass (Qwen2/Qwen3, Q8 — hand-written float math)
    ↓
uefi_libc (efi/uefi_libc.c — malloc, math, string)
    ↓
UEFI Boot Services  +  iwn / e1000e / ns_* network stack
    ↓
x86_64 Hardware
```

## Build

```
# Prerequisites (macOS)
brew install x86_64-elf-gcc qemu mtools

# Build
make efi              # all_bootai.efi
make PROD=1 efi       # boots straight into chat, no REPL
make release          # PROD + flashable GPT disk image
make clean

# Run in QEMU
make run

# Create bootable USB (macOS)
make usb DISK=/dev/diskN
```

QEMU networking needs an EDK2 build of OVMF with `NETWORK_IP4_ENABLE=TRUE`. The OVMF bundled with the `qemu` formula boots fine but has no network stack — see `docs/architecture/networking.md`.

## Project Structure

```
bootai/
├── efi/           # UEFI entry point, console, input, fs, install, HTTP server
│   └── ns_*.c     # Hand-rolled ARP/IP/UDP/TCP/DHCP stack
├── iwn/           # Intel wireless driver (PCI, firmware, scan, crypto, EAPOL)
├── e1000e/        # Intel Ethernet driver
├── model/         # Forward pass, loader, KV cache, sampling
├── tools/         # Tool-use scaffolding (stubs) + export_weights.py
├── tokenizer/     # BPE tokenizer
├── duet/          # DUET legacy-BIOS boot chain
├── drivers/       # Optional UEFI drivers loaded at runtime
└── docs/          # Architecture docs, build notes
```

## Booting

The default build boots into the REPL; `PROD=1` boots straight into chat. Type `chat` to start inference, or `serve 8080` to expose the HTTP API and `bootai.local` on the LAN.

### Installing to disk

The `install` command writes a standalone bootable system to an internal disk.

> **This erases the entire target disk.** It selects the largest internal block device, destroys any existing partition table (both MBR and GPT), and writes a fresh ESP plus data partition. It is not a dual-boot installer and it does not preserve an existing OS. Back up first.

## Community

- **X** — [@TeamIDElab](https://x.com/TeamIDElab)

- **Discord** — [discord.gg/pRpR52A2tk](https://discord.gg/pRpR52A2tk)

## License

**AGPL-3.0** — see [`LICENSE`](LICENSE).

A free, automatic exception applies to the people this was written for: **individuals, academic and non-commercial research, nonprofits, and organizations under 100 people / $1M revenue may use BootAI under Apache-2.0 terms instead** — no paperwork, nothing to pay, nothing to sign. Organizations above that line — anyone shipping hardware that boots BootAI, or serving it as a product — need a commercial license: [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md).

Contributions come inbound under Apache-2.0 with a DCO sign-off — see [`CONTRIBUTING.md`](CONTRIBUTING.md). You keep your copyright.

`JACLibc/` and `foundry/` are registered submodules (MIT and AGPL-3.0 respectively) that the current build does not use.
