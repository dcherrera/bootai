# EDK2 Network DXE Drivers

Pre-built UEFI DXE drivers extracted from the custom OVMF build (`ovmf-code.fd`).

These drivers provide the IP stack that bridges SNP (Layer 2) to TCP4/UDP4 (Layer 4).
Required on real hardware (e.g. Dell E6510) whose firmware lacks the EDK2 IP stack.
QEMU/OVMF has them built-in and does not need these.

## Files

| File | Size | Purpose |
|------|------|---------|
| `01-MnpDxe.efi` | 24K | Managed Network Protocol — bridges SNP to ARP/IP |
| `02-ArpDxe.efi` | 12K | Address Resolution Protocol |
| `03-Dhcp4Dxe.efi` | 28K | DHCP4 client |
| `04-Ip4Dxe.efi` | 61K | IPv4 stack |
| `05-Udp4Dxe.efi` | 24K | UDP4 transport |
| `06-TcpDxe.efi` | 44K | TCP4/TCP6 transport |

## Naming Convention

Numbered prefixes ensure alphabetical load order matches the dependency chain:
MNP → ARP → DHCP4 → IP4 → UDP4 → TCP4.

`efi/drivers.c` loads all `.efi` files from `\drivers\` on the USB alphabetically.

## Source

Extracted from `bootai/ovmf-code.fd` (built from EDK2 source with
`-D NETWORK_ENABLE=TRUE -D NETWORK_IP4_ENABLE=TRUE`).

License: BSD-2-Clause (EDK2/TianoCore).

## USB Installation

Copy to `\drivers\` on the BOOTAI USB partition alongside the IWN firmware:

```
\drivers\01-MnpDxe.efi
\drivers\02-ArpDxe.efi
\drivers\03-Dhcp4Dxe.efi
\drivers\04-Ip4Dxe.efi
\drivers\05-Udp4Dxe.efi
\drivers\06-TcpDxe.efi
\drivers\iwn-6000           (IWN firmware, already present)
```
