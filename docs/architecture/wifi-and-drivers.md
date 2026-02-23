# WiFi & Driver Loader Architecture

UEFI driver loading from disk and WiFi connectivity via standard UEFI protocols.

Driver loader lives in `efi/drivers.c`. WiFi management lives in `efi/wifi.c`. Protocol types are in `efi/efi.h`.

## Overview

```
Boot Sequence:
  efi_fs_init()           ← Filesystem ready
      ↓
  efi_load_drivers()      ← Scan \drivers\, LoadImage/StartImage each .efi
      ↓
  connect_all_controllers() ← Bind new drivers to hardware
      ↓
  efi_net_init()          ← Wired Ethernet + DHCP (existing)
      ↓
  efi_wifi_auto_connect() ← Read \wifi.conf, try each network
      ↓
  REPL                    ← wifi commands available
```

## Driver Loader

### Purpose

Generic `.efi` driver loader. Users drop UEFI driver files into `\drivers\` on the USB/disk. At boot, BootAI loads and starts them all before network initialization. This supports:

- WiFi drivers (OEM-specific, e.g. Intel AX201)
- GPU drivers
- Storage controller drivers
- Any UEFI DXE driver in PE32+ format

### How It Works

```
efi_load_drivers():
  1. Open \drivers\ directory via Simple File System
  2. Iterate entries with dir->Read()
  3. For each .efi file:
     a. Read file into memory (AllocatePool + file->Read)
     b. gBS->LoadImage(FALSE, gImageHandle, NULL, buffer, size, &handle)
     c. gBS->StartImage(handle, NULL, NULL)
     d. FreePool(buffer)
  4. connect_all_controllers() — bind loaded drivers to hardware
  5. If \drivers\ doesn't exist → silently continue
```

### LoadImage from Memory

`LoadImage` with `SourceBuffer != NULL` loads a PE32+ image from an in-memory buffer. This avoids constructing a file device path (which requires knowing the device handle, partition GUID, etc.). The firmware parses the PE header, relocates sections, and creates a loaded image protocol on the returned handle.

```c
gBS->LoadImage(
    FALSE,           // BootPolicy = FALSE (not a boot option)
    gImageHandle,    // ParentImageHandle (our own image)
    NULL,            // DevicePath = NULL (loading from buffer)
    buffer,          // SourceBuffer (file contents)
    size,            // SourceSize
    &driver_handle   // Output: handle for the loaded image
);
```

### gImageHandle

`efi_main` receives `ImageHandle` from firmware. We save it as the global `gImageHandle` — required as the parent image handle for `LoadImage`.

### connect_all_controllers

After loading drivers, `connect_all_controllers()` iterates all handles in the system and calls `ConnectController` recursively. This binds newly-loaded drivers to hardware they support (e.g. a WiFi driver binds to the PCI WiFi adapter). The function does two passes — the first may create new child handles that need connecting in the second pass.

This function was originally `static` in `net.c` (used for DHCP bring-up). Now non-static, declared in `efi.h`, shared by `drivers.c` and `net.c`.

## WiFi

### Protocol Stack

```
efi_wifi_connect(ssid, password)
    ↓
EFI_SUPPLICANT_PROTOCOL           ← Set SSID + PSK credentials
    ↓
EFI_WIRELESS_MAC_CONNECTION_II    ← ConnectNetwork (802.11 association)
    ↓
connect_all_controllers()         ← Bind IP stack to WiFi interface
    ↓
efi_net_init()                    ← DHCP over WiFi
```

### WiFi2 Protocol (UEFI 2.7+)

`EFI_WIRELESS_MAC_CONNECTION_II_PROTOCOL` provides three operations:

| Function | Purpose |
|----------|---------|
| `GetNetworks` | Scan for visible SSIDs, returns signal strength + security |
| `ConnectNetwork` | Associate with a network (after Supplicant sets credentials) |
| `DisconnectNetwork` | Disassociate from current network |

All three are asynchronous — use `EVT_NOTIFY_SIGNAL` completion tokens with polling loops (same pattern as TCP4/UDP4).

### Supplicant Protocol

`EFI_SUPPLICANT_PROTOCOL` handles WPA2-PSK authentication. Before calling `ConnectNetwork`, set credentials:

```c
g_supplicant->SetData(g_supplicant, EFI_SUPPLICANT_DATA_TYPE_SSID,
                      &ssid_data, sizeof(ssid_data));
g_supplicant->SetData(g_supplicant, EFI_SUPPLICANT_DATA_TYPE_PSK,
                      password, password_len);
```

The Supplicant is optional — open networks don't need it. If the protocol isn't found on the WiFi handle, credential setting is skipped.

### Initialization

`efi_wifi_init()` runs lazily on first call to any WiFi function:

1. `LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiWiFi2ProtocolGuid, ...)`
2. If no handles → no WiFi hardware, all functions return silently
3. `HandleProtocol` to get `WiFi2` and `Supplicant` interfaces from the same handle

### Hardware Compatibility

| Hardware | WiFi2 Available | Notes |
|----------|-----------------|-------|
| QEMU/OVMF | No | No WiFi emulation. Code reports "No WiFi adapter" |
| Surface Pro 7 | No | WiFi chip present but no UEFI driver in firmware |
| HP EliteBook (2020+) | Yes | Intel AX201/AC-9560 with UEFI WiFi2 support |
| HP ZBook (2020+) | Yes | Same Intel WiFi with UEFI drivers |
| Lenovo ThinkPad | Varies | Some models have UEFI WiFi, most don't |

On hardware without WiFi2 in firmware, users can potentially load a standalone `.efi` WiFi driver via the driver loader — if one exists for their hardware.

## wifi.conf

Plain-text config file on the USB root. Parsed at boot for auto-connect. Editable with any text editor before booting.

### Format

```
# wifi.conf — BootAI WiFi Configuration
# Comments start with #. Blank lines are ignored.

[network]
ssid=HomeNetwork
password=MyPassword123

[network]
ssid=CoffeeShop
password=coffee2024

[network]
ssid=OpenNetwork
```

### Parser Details

- Line-by-line, no dynamic allocation
- `[network]` starts a new entry
- `ssid=` and `password=` within each block
- Max 16 networks, 64-char SSID/password
- First match wins during auto-connect
- Networks without `password=` are treated as open

### Auto-Connect Flow

```
efi_wifi_auto_connect():
  1. Check WiFi adapter present (lazy init)
  2. Read \wifi.conf via efi_read_file()
  3. Parse into wifi_network_entry array
  4. For each entry: efi_wifi_connect(ssid, password)
  5. Stop on first success
  6. If no file or no match → silently continue
```

## REPL Commands

| Command | Action |
|---------|--------|
| `wifi` | Show adapter presence + connection status |
| `wifi scan` | Scan and list nearby networks with signal/security |
| `wifi connect <SSID>` | Connect — prompts for password, offers to save |
| `wifi disconnect` | Disconnect from current network |
| `wifi save <SSID> [password]` | Append network to `wifi.conf` |

### wifi connect Flow

```
boot> wifi connect HomeNetwork
  Password: ********
  Connecting to HomeNetwork...
  Connected to HomeNetwork
  Save to wifi.conf? (y/n) y
  Saved HomeNetwork to wifi.conf
```

### wifi save

Reads existing `wifi.conf`, checks for duplicate SSID, appends a new `[network]` block, writes back. Deduplication prevents saving the same network twice.

## Build

Sources are included in `EFI_SRC` in the Makefile:

```makefile
EFI_SRC = efi/main.c ... efi/drivers.c efi/wifi.c
```

The disk image (`img` target) creates `\drivers\` directory and copies `wifi.conf.template` as `\wifi.conf`:

```makefile
mmd -i bootai.img ::drivers
mcopy -i bootai.img wifi.conf.template ::wifi.conf
```

## Testing

### QEMU (build verification)

```bash
make clean && make phase4   # Compile
make run                    # Boot in QEMU (you run this)
```

Expected at boot:
```
  Loading drivers from \drivers\...
  No drivers found.
  ...
```

WiFi commands will report "No WiFi adapter" in QEMU — this is correct.

### Real Hardware (HP with UEFI WiFi)

```bash
make img                    # Create disk image
make usb DISK=/dev/diskN    # Write to USB
```

Boot from USB, then:
```
boot> wifi scan
  1. HomeNetwork (WPA2), -45 dBm
  2. Neighbor5G (WPA2), -72 dBm
boot> wifi connect HomeNetwork
  Password: ********
  Connected to HomeNetwork
boot> serve
  HTTP server on port 8080
```

## Known Limitations

- **No WiFi in QEMU** — OVMF has no WiFi emulation. WiFi can only be tested on real hardware with UEFI WiFi2 support.
- **No standalone WiFi drivers exist** — Intel/Qualcomm WiFi `.efi` drivers are OEM-embedded, not distributed separately. The driver loader is future-proofing.
- **WPA2-PSK only** — WPA3/Enterprise (802.1X) would require extending the Supplicant setup.
- **WiFi2 struct layout** — Protocol structs match UEFI 2.10 spec. If a specific firmware uses a different version, field offsets may not match.
- **DHCP re-init** — After WiFi connects, `efi_net_init()` is called to run DHCP. This re-initializes the full network stack, which may reset wired Ethernet state if both are present.
- **No background reconnect** — If WiFi drops, user must manually reconnect via REPL.
