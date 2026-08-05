# Contributing to BootAI

Thanks for your interest in BootAI. This document covers the basics.

## Licensing of contributions — read this first

BootAI ships under **AGPL-3.0** with a commercial-licensing option (see
[`LICENSE`](./LICENSE) and [`COMMERCIAL-LICENSE.md`](./COMMERCIAL-LICENSE.md)).
That arrangement only works if every line in the tree can be included in a
commercial build. So:

> **Contributions are made inbound under the Apache License 2.0**, even though the
> project as a whole ships outbound under AGPL-3.0.

**You keep your copyright.** You are not assigning anything to anyone. You are
licensing your contribution permissively so that it can be distributed both under
the AGPL and under a commercial license. This is the same inbound/outbound split
used by other dual-licensed projects, and it is deliberately lighter than a signed
CLA — there is nothing to print, scan, or route through your legal department.

If you would rather not contribute under Apache-2.0, open an issue describing the
change instead of a pull request. An idea costs nothing to accept.

### Sign your commits (DCO)

Every commit must carry a `Signed-off-by` line certifying the
[Developer Certificate of Origin](https://developercertificate.org/) — that you
wrote the code, or otherwise have the right to submit it:

```bash
git commit -s -m "your message"
```

which appends:

```
Signed-off-by: Your Name <your.email@example.com>
```

Use your real name and a working email address. If you are contributing work your
employer owns, confirm you actually have the right to do so before you sign off.

## Getting Started

You need `x86_64-elf-gcc` to cross-compile, and QEMU + OVMF firmware to boot what
you built.

```bash
make efi              # build all_bootai.efi (boot menu + REPL)
make PROD=1 efi       # production build — boots straight into chat
make release          # PROD build + flashable GPT disk image
make run              # boot it in QEMU with OVMF
make clean
```

| Flag | What | When |
|------|------|------|
| `PROD=1` | Skip the boot menu and REPL, go straight to chat | Production/demo builds |

## Code Style

- **C99**, `-ffreestanding`, no hosted libc — [JACLibc](./JACLibc/) is
  header-only and bare-metal
- **`ba_` prefix** for boot/app functions, **`efi_` prefix** for UEFI wrappers;
  standard C naming otherwise
- Errors: return `NULL` or an `EFI_STATUS` — nothing calls `exit()`, there is
  nothing to exit to
- `snprintf`, never `sprintf`
- No warnings under `-Wall -Wextra`

Remember where this runs: UEFI boot services, no kernel, no MMU tricks, no
scheduler. Allocation goes through boot services and is not free. Anything that
blocks blocks the whole machine.

## Testing

Test in QEMU with OVMF (`make run`) before opening a PR, and say in the PR whether
you also booted it on real hardware — firmware behavior diverges from OVMF often
enough that it matters, especially for anything touching disk, NVRAM, network, or
keyboard input.

**If you change a forward pass, a kernel, or quantized matmul, show numerical
parity.** The bar is argmax-exact against the reference path on a real model — not
"it produces plausible text." Include how you measured it in the PR.

## Pull Request Guidelines

- One logical change per PR
- Parity evidence for numerics; a QEMU boot log for anything in the boot path
- Update docs when behavior or the management API changes
- Zero warnings, clean build

Commit messages describe the change and why. Keep them plain — no tool,
assistant, or generator attribution.

## Reporting Bugs

Open an issue with:

- what you expected, and what actually happened
- minimal reproduction steps
- the model and quantization involved (many bugs are format-specific)
- the machine, its firmware vendor and version, and whether it reproduces in QEMU
- your `bootai.log` from the USB drive if you have one — it is the whole session
  transcript and usually the fastest path to a diagnosis

Bugs that only appear on one vendor's firmware, at a particular context depth, or
after an install-to-disk are common here — please say which, since it is usually
the whole clue.
