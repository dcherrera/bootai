## BootAI

Boot straight into AI. A UEFI application that turns any x86_64 machine into an AI coding assistant — no OS required.

Plug in a USB drive, power on, and you're talking to a language model that can read/write files, search the web, and operate the system. All running on bare metal through UEFI boot services.

## What It Does

- **Chat / code generation** — full language model inference on bare metal

- **File operations** — read, write, list files via UEFI Simple File System

- **Web search** — search DuckDuckGo via UEFI TCP4 networking

- **URL fetching** — pull web pages via UEFI HTTP protocol

- **Network discovery** — advertises `bootai.local` via mDNS/Bonjour, accessible from any device on the LAN

- **Persistent settings** — saved in UEFI NVRAM across reboots

- **Conversation history** — saved to disk

## Models

| Model                     | Size | Quantization | Memory  |
| ------------------------- | ---- | ------------ | ------- |
| Qwen2.5-Coder-7B-Instruct | 7B   | Q4_K_M       | ~4 GB   |
| RWKV-X 3.6B               | 3.6B | Q8 (INT8)    | ~3.6 GB |

Minimum 8GB RAM required.

## How It Works

No kernel. No OS. The `.efi` binary stays in UEFI boot services mode, which gives us free access to:

- **Filesystem** — Simple File System Protocol (FAT32)

- **Networking** — TCP4, UDP4, HTTP, DHCP4, mDNS

- **Display** — Graphics Output Protocol

- **Keyboard** — Simple Text Input Protocol

- **Storage** — NVRAM variables

- **Timers** — Event services

## Stack

```
Tool Use / Agentic Layer
    ↓
Inference REPL (prompt → tokens → generate → display)
    ↓
Model Forward Pass (Qwen2.5 Q4 or RWKV-X Q8)
    ↓
Foundry Tensor Library (pure C — matmul, softmax, quantization)
    ↓
JACLibc (header-only libc — malloc, math, string)
    ↓
UEFI Boot Services (filesystem, networking, display, keyboard)
    ↓
x86_64 Hardware
```

## Build

```
# Prerequisites (macOS)
brew install x86_64-elf-gcc ovmf qemu

# Build
make efi

# Run in QEMU
make run

# Create bootable USB (macOS)
make usb DISK=/dev/diskN
```

## Project Structure

```
bootai/
├── efi/           # UEFI entry point, console, input, filesystem, networking
├── model/         # Model forward pass, loader, KV cache, sampling
├── tools/         # Tool use parser + backends (fs, net, system)
├── tokenizer/     # BPE (Qwen2.5) and RWKV World tokenizer
├── foundry/       # Submodule: pure C tensor runtime
├── JACLibc/       # Submodule: header-only libc
└── docs/          # Architecture docs, build notes
```

## USB Boot

```
[1] Run Live (from USB)       — nothing touches the hard drive
[2] Install to Hard Drive     — dual-boots alongside existing OS
[3] Settings                  — model, sampling, network config
```

## Community

- **X** — [@TeamIDElab](https://x.com/TeamIDElab)
- **Discord** — [discord.gg/pRpR52A2tk](https://discord.gg/pRpR52A2tk)

## License

**AGPL-3.0** — see [`LICENSE`](LICENSE).

A free, automatic exception applies to the people this was written for:
**individuals, academic and non-commercial research, nonprofits, and organizations under
100 people / $1M revenue may use BootAI under Apache-2.0 terms instead** — no paperwork,
nothing to pay, nothing to sign. Organizations above that line — anyone shipping hardware
that boots BootAI, or serving it as a product — need a commercial license:
[`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md).

Contributions come inbound under Apache-2.0 with a DCO sign-off — see
[`CONTRIBUTING.md`](CONTRIBUTING.md). You keep your copyright.

`JACLibc/` is a third-party submodule under MIT.

MIT-T