# Networking Architecture

Deep dive into BootAI's network stack: DHCP, TCP4, UDP4 DNS, HTTP/1.0, and the custom network stack for hardware without UEFI IP stack drivers.

Networking dispatch lives in `efi/net.c`. Protocol type definitions are in `efi/efi.h`. Custom stack code lives in `efi/ns_*.c` files.

## Overview

BootAI has two networking paths, selected automatically based on hardware capabilities:

```
        http_server.c / mdns.c / main.c (callers — unchanged)
        tcp4_listen, tcp4_accept, tcp4_send, tcp4_recv, tcp4_close
                            ↓
                     net.c (dual dispatch)
                ┌───────────┴───────────────┐
                ↓                           ↓
        UEFI TCP4/UDP4/IP4           Custom ns_* Stack
        (QEMU, wired Ethernet)       (Dell E6510 WiFi)
                ↓                           ↓
        IP4Config2 (DHCP)           ns_tcp.c (TCP state machine)
        ConnectController           ns_udp.c (UDP send/recv)
        MnpDxe → ArpDxe →          ns_ip.c  (IPv4 build/parse)
         Ip4Dxe → Tcp4Dxe          ns_arp.c (ARP table/resolve)
                ↓                   ns_dhcp.c (DHCP client)
                ↓                   ns_net.c (frame dispatch)
        EFI_SIMPLE_NETWORK_PROTOCOL (SNP)
                ↓                           ↓
        ┌────────────────┬──────────────────┐
        │ QEMU/wired     │ Dell E6510 WiFi  │
        │ VirtioNetDxe   │ IWN SNP wrapper  │
        │ (built into    │ (iwn_snp.c)      │
        │  firmware)     │      ↓           │
        │                │ iwn driver       │
        │                │ (802.11 + CCMP)  │
        └────────────────┴──────────────────┘
```

**QEMU path** (left): UEFI firmware includes full IP stack DXE drivers. `ConnectController` binds MnpDxe → ArpDxe → Ip4Dxe → Tcp4Dxe automatically. Existing code runs unchanged.

**Dell path** (right): Firmware lacks IP stack drivers. Custom `ns_*` stack implements ARP, DHCP, IPv4, UDP, and TCP directly on raw SNP frames. Callers never change — `net.c` dispatches based on which path is available.

## DHCP

IP configuration is handled by `EFI_IP4_CONFIG2_PROTOCOL`.

1. Locate protocol via `LocateHandleBuffer(SEARCH_BY_PROTOCOL, &gEfiIp4Config2ProtocolGuid, ...)`
2. Set DHCP policy: `ip4_config2->SetData(IP4_CONFIG2_DATA_TYPE_POLICY, DHCP)`
3. Poll `GetData(IP4_CONFIG2_DATA_TYPE_INTERFACEINFO)` until `EFI_BUFFER_TOO_SMALL` (indicates IP assigned)
4. Timeout after 8 seconds if no response

After DHCP completes, a post-DHCP `ConnectController` pass binds upper-layer protocols (DNS4, etc.) that depend on IP configuration.

## TCP4 Lifecycle

TCP4 uses the UEFI service binding pattern — each connection gets its own protocol instance.

### Connection Setup

```
tcp4_sb->CreateChild(&child_handle)
  → gBS->HandleProtocol(child_handle, &gEfiTcp4ProtocolGuid, &tcp4)
  → tcp4->Configure(config)     // UseDefaultAddress=TRUE, retry on EFI_NO_MAPPING
  → tcp4->Connect(token)        // Poll CompletionToken.Status
```

### Data Transfer

```
tcp4->Transmit(tx_token)        // Push=TRUE, single fragment
tcp4->Receive(rx_token)         // Poll for data, handle EFI_CONNECTION_FIN
```

### Connection Teardown

```
tcp4->Close(close_token)        // Graceful close, poll completion
tcp4_sb->DestroyChild(child_handle)
```

### Configuration

```c
EFI_TCP4_CONFIG_DATA config = {
    .TimeToLive = 64,
    .AccessPoint = {
        .UseDefaultAddress = TRUE,   // Use DHCP-assigned IP
        .StationPort = 0,            // Ephemeral port
        .RemoteAddress = host_ip,
        .RemotePort = port,
        .ActiveFlag = TRUE           // Client (active open)
    }
};
```

`Configure` may return `EFI_NO_MAPPING` if DHCP hasn't completed. Retry with 250ms delay up to 5 seconds.

### Completion Tokens

All async operations use `EVT_NOTIFY_SIGNAL` events with polling loops:

```c
EFI_TCP4_IO_TOKEN token;
gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, empty_notify, NULL, &token.CompletionToken.Event);
token.CompletionToken.Status = EFI_NOT_READY;

tcp4->Transmit(tcp4, &token);

// Poll — WaitForEvent doesn't work with EVT_NOTIFY_SIGNAL
for (int i = 0; i < 100; i++) {
    if (token.CompletionToken.Status != EFI_NOT_READY) break;
    gBS->Stall(100000);  // 100ms
}
gBS->CloseEvent(token.CompletionToken.Event);
```

## UDP4 DNS Resolution

### Why Not DNS4?

OVMF's `DNS4` driver crashes with a General Protection Fault (#GP) when accessing freed pool memory. This is a firmware bug, not a protocol design issue. Rather than waiting for a fix, we implement DNS directly over `EFI_UDP4_PROTOCOL`.

### DNS Wire Format (RFC 1035)

**Query structure:**

```
+--+--+--+--+--+--+--+
|        Header       |   12 bytes: ID, flags (RD=1), QDCOUNT=1
+--+--+--+--+--+--+--+
|       Question      |   Label-encoded hostname + QTYPE(A) + QCLASS(IN)
+--+--+--+--+--+--+--+
```

Label encoding: `example.com` becomes `\x07example\x03com\x00`

**Response parsing:**
1. Verify QR=1 (response bit), RCODE=0 (no error), ANCOUNT > 0
2. Skip question section (walk labels + 4 bytes QTYPE/QCLASS)
3. Walk answer RRs, handling 0xC0 compression pointers
4. Find first TYPE=A (1), CLASS=IN (1), RDLENGTH=4
5. Extract 4-byte IPv4 address from RDATA

### UDP4 Lifecycle

Same service binding pattern as TCP4:

```
udp4_sb->CreateChild(&child_handle)
  → HandleProtocol(child_handle, &gEfiUdp4ProtocolGuid, &udp4)
  → udp4->Configure(config)          // UseDefaultAddress=TRUE, StationPort=0
  → Build DNS query (dns_build_query)
  → udp4->Transmit(tx_token)         // dest=10.0.2.3:53
  → udp4->Receive(rx_token)          // Poll with udp4->Poll()
  → Parse response (dns_parse_response)
  → Signal RecycleSignal             // Return buffer to firmware
  → udp4_sb->DestroyChild(child_handle)
```

### DNS Server

QEMU user-mode networking provides a DNS forwarder at `10.0.2.3`. This is hardcoded as the DNS server address.

### Critical Details

- Must call `udp4->Poll()` in the receive loop to drive packet processing
- Must signal `RecycleSignal` on received data to prevent buffer leaks
- UDP4 completion token uses a `Packet` union with `RxData`/`TxData` pointers (no separate IO token wrapper like TCP4)
- Retry on `EFI_NO_MAPPING` during Configure (same DHCP race as TCP4)

## mDNS / Bonjour Discovery (RFC 6762)

BootAI advertises itself as `bootai.local` on the local network via multicast DNS. Any device on the same LAN can discover the HTTP API without knowing the IP.

### How It Works

```
ba_mdns_init()      → Create UDP4 child on port 5353, join 224.0.0.251 multicast
ba_mdns_announce()  → Send A-record response twice (RFC 6762 §8.3)
ba_mdns_poll()      → Check for incoming queries, respond if bootai.local is asked
ba_mdns_stop()      → Send goodbye (TTL=0), leave multicast, destroy child
```

### Integration

mDNS is integrated into the `serve` command — init and announce before the HTTP server starts, poll every accept cycle (~500ms), stop on exit. Also available as a standalone `mdns` REPL command for testing.

### Packet Format

**Announcement/Response (40 bytes):**

```
Header (12 bytes):
  ID=0, Flags=0x8400 (QR=1 response, AA=1 authoritative)
  QDCOUNT=0, ANCOUNT=1, NSCOUNT=0, ARCOUNT=0

Answer:
  NAME:    \x06bootai\x05local\x00
  TYPE:    A (1)
  CLASS:   IN (1) | cache-flush (0x8000)
  TTL:     120 seconds (0 for goodbye)
  RDLEN:   4
  RDATA:   <our IPv4 address>
```

### Query Matching

`mdns_is_our_query()` checks incoming packets:
- QR=0 (query), QDCOUNT >= 1
- QNAME matches `bootai.local` (case-insensitive label comparison)
- QTYPE is A (1) or ANY (255)
- QCLASS is IN (1) or ANY (255), ignoring unicast-response bit

### UDP4 mDNS Lifecycle

```
LocateHandleBuffer(UDP4_SERVICE_BINDING)
  → CreateChild(&child_handle)
  → HandleProtocol(child_handle, &UDP4, &udp4)
  → udp4->Configure(port=5353, AllowDuplicatePort=1, TTL=255)
  → udp4->Groups(TRUE, 224.0.0.251)    // Join multicast
  → [announce + poll loop]
  → udp4->Groups(FALSE, 224.0.0.251)   // Leave multicast
  → DestroyChild(child_handle)
```

### Local IP Retrieval

`efi_get_local_ip()` reads the DHCP-assigned IP from `ip4_config2->GetData(INTERFACEINFO)`. The station address is at byte offset 104 in the `EFI_IP4_CONFIG2_INTERFACE_INFO` struct (after Name[32] + IfType + padding + HwAddrSize + HwAddr[32]).

### Client Usage

From any machine on the same network:

```bash
# macOS
ping bootai.local
curl http://bootai.local:8080/health
dns-sd -Q bootai.local

# Linux (avahi)
avahi-resolve -n bootai.local
curl http://bootai.local:8080/v1/models
```

### Limitations

- **QEMU**: Does not work in QEMU user-mode networking (SLIRP doesn't forward multicast). Real hardware only.
- **IPv4 only**: No AAAA records.
- **Single hostname**: Only `bootai.local` is advertised. No service records (SRV/TXT) yet.
- **No conflict detection**: If another device claims `bootai.local`, we don't rename.

## HTTP/1.0

Raw TCP4, not the UEFI HTTP protocol. Simple and reliable.

### Request Format

```
GET /path HTTP/1.0\r\n
Host: hostname\r\n
Connection: close\r\n
\r\n
```

### Response Handling

1. Connect to resolved IP:port via TCP4
2. Send HTTP/1.0 request
3. Receive in 4KB chunks until connection close (`EFI_CONNECTION_FIN`)
4. Find `\r\n\r\n` boundary between headers and body
5. Return body only (headers stripped)

### URL Parsing

`parse_url()` extracts host, port (default 80), and path from `http://host:port/path`.

## OVMF Firmware

Stock OVMF builds (Homebrew, edk2-nightly) don't include network drivers. Must build from EDK2 source with:

```bash
-D NETWORK_ENABLE=TRUE -D NETWORK_IP4_ENABLE=TRUE -D NETWORK_HTTP_ENABLE=TRUE -D FD_SIZE_4MB
```

Uses split pflash:
- `ovmf-code.fd` — readonly firmware code (pflash unit 0)
- `ovmf-vars.fd` — writable NVRAM (pflash unit 1, copied per-session)

## QEMU Networking

QEMU user-mode networking (`-netdev user`) provides:

| Address | Purpose |
|---------|---------|
| `10.0.2.2` | Gateway (host machine) |
| `10.0.2.3` | DNS forwarder (forwards to host resolver) |
| `10.0.2.15` | Guest IP (default DHCP lease) |

To test HTTP against a local server:
```bash
python3 -m http.server 8888    # Run on host
# In BootAI REPL:
fetch http://10.0.2.2:8888/    # Reaches host via QEMU gateway
```

## Real Hardware vs QEMU: IP Stack Gap

### The Problem

QEMU/OVMF includes the full EDK2 IP stack DXE drivers (MnpDxe, ArpDxe, Ip4Dxe, Tcp4Dxe, Udp4Dxe, Dhcp4Dxe) built into the firmware. When `ConnectController` is called on an SNP handle, MnpDxe detects it and automatically creates the MNP → ARP → IP4 → TCP4/UDP4 protocol chain.

Real hardware firmware (e.g. Dell E6510) may **not** include these drivers because:
- The firmware was designed for wired Ethernet NICs with vendor-specific driver stacks
- WiFi was typically handled by the OS, not UEFI
- The IP stack DXE drivers are optional EDK2 components

This means that after `iwn_snp_install()` creates a valid SNP handle on the Dell, `efi_net_init()` finds no `IP4Config2` protocol because there's no MnpDxe to bridge SNP to IP.

### Diagnostics: `netdiag` Command

The `netdiag` REPL command (implemented in `efi/net_diag.c`) enumerates 8 key network protocol handles to diagnose exactly which layer is missing:

```
SNP, MNP SB, ARP SB, IP4 SB, IP4Config2, DHCP4 SB, TCP4 SB, UDP4 SB
```

Run before and after `iwn connect` to see what protocols the firmware creates in response to SNP appearing.

### `connect_all_controllers` Guard

`efi_net_init()` originally called `connect_all_controllers()` twice unconditionally. This is correct for wired Ethernet (the NIC needs driver binding), but harmful for IWN WiFi because:

1. IWN already called `ConnectController(snp_handle)` during SNP install
2. Broad controller enumeration hits the PCI WiFi device, potentially re-triggering driver binding that conflicts with IWN's direct hardware access

The fix: skip `connect_all_controllers()` when `iwn_snp_is_installed()` returns true.

### Resolution Strategy

| Phase | Approach | Status |
|-------|----------|--------|
| 1 | `netdiag` to identify which protocols are missing | **DONE** — Dell has zero IP stack protocols |
| 2 | Bundle EDK2 DXE drivers on USB `\drivers\` | **FAILED** — ConnectController hangs on Dell firmware |
| 3 | Custom ARP/DHCP/IP/UDP/TCP on raw SNP | **DONE** — Phase 5c implemented |

Phase 2 was tested with 6 EDK2 DXE drivers extracted from OVMF. Four loaded successfully but `Udp4Dxe.efi` and `TcpDxe.efi` failed at `StartImage`. When `ConnectController(snp_handle)` was called, MnpDxe's internal initialization hung — OVMF DXE drivers are incompatible with Dell E6510 firmware. ConnectController was removed from `iwn_snp_install()`.

Phase 3 implemented a custom network stack (ARP/DHCP/IP/UDP/TCP) directly on raw SNP. See the [Custom Network Stack](#custom-network-stack-phase-5c) section below for full details.

---

## Custom Network Stack (Phase 5c)

A complete ARP/DHCP/IPv4/UDP/TCP stack built on raw `EFI_SIMPLE_NETWORK_PROTOCOL` frames. Used on hardware where UEFI firmware lacks IP stack DXE drivers (e.g. Dell E6510 with IWN WiFi driver).

### Architecture

```
┌──────────────────────────────────────────┐
│     http_server.c / mdns.c / main.c      │  callers — unchanged
│  tcp4_listen, tcp4_accept, tcp4_send,    │
│  tcp4_recv, tcp4_close, efi_get_local_ip │
├──────────────────────────────────────────┤
│              net.c (dual dispatch)       │  checks ns_is_ready()
├────────────┬─────────────────────────────┤
│  UEFI TCP4 │    ns_tcp.c (~400 lines)    │  TCP state machine
│  (QEMU)    │    ns_udp.c (~95 lines)     │  UDP send/recv + port binding
│            │    ns_ip.c  (~145 lines)    │  IPv4 build/parse/checksum
│            │    ns_arp.c (~130 lines)    │  ARP table/resolve/reply
│            │    ns_dhcp.c (~280 lines)   │  DHCP discover/offer/request/ack
│            │    ns_net.c (~105 lines)    │  Frame dispatch + raw TX
├────────────┴─────────────────────────────┤
│              SNP (iwn_snp.c)             │  Layer 2
└──────────────────────────────────────────┘
```

**Total**: ~1,155 lines of new code across 7 files (6 `ns_*.c` + `ns_net.h`), ~100 lines modified in existing files.

**RAM budget**: ~12 KB static (ARP table 160B, DHCP buffers 1KB, TCP rx_buf 8KB, ns_state 32B, misc).

### Files

| File | Lines | Purpose |
|------|-------|---------|
| `efi/ns_net.h` | ~110 | Shared types (`ns_state`, `ns_tcp_conn`, `ns_arp_entry`), TCP states, public API declarations |
| `efi/ns_net.c` | ~105 | Core: `ns_init()`, `ns_poll()` (EtherType dispatch), `ns_raw_send()` (Ethernet frame TX) |
| `efi/ns_arp.c` | ~130 | 16-entry ARP table, `ns_arp_resolve()` (blocking 3s), `ns_arp_handle()` (learn + reply) |
| `efi/ns_ip.c` | ~145 | `ns_ip_send()` (subnet routing, checksum), `ns_ip_handle()` (parse, dispatch to UDP/TCP) |
| `efi/ns_udp.c` | ~95 | `ns_udp_send()`, `ns_udp_handle()`, 4-entry port binding table |
| `efi/ns_dhcp.c` | ~280 | Full DHCP exchange: discover→offer→request→ack, raw broadcast (no IP yet) |
| `efi/ns_tcp.c` | ~400 | TCP state machine: listen/accept/send/recv/close, retransmit, MSS 1460 |

### Dual Dispatch in `net.c`

The `tcp4_conn_t` struct holds both paths:

```c
typedef struct {
    EFI_TCP4_PROTOCOL *tcp4;       /* UEFI path (QEMU) */
    EFI_HANDLE child_handle;       /* UEFI path */
    ns_tcp_conn *ns_conn;          /* Custom path (Dell) — NULL on UEFI path */
} tcp4_conn_t;
```

Each function checks which path to use:

| Function | UEFI path | Custom path |
|----------|-----------|-------------|
| `efi_net_init()` | IP4Config2 DHCP | `ns_init()` + `ns_dhcp_run()` |
| `efi_get_local_ip()` | IP4Config2 GetData | `ns_get_ip()` |
| `tcp4_listen()` | TCP4 ServiceBinding | `ns_tcp_listen()` |
| `tcp4_accept()` | TCP4 Accept token | `ns_tcp_accept()` (500ms timeout) |
| `tcp4_send()` | TCP4 Transmit token | `ns_tcp_send()` |
| `tcp4_recv()` | TCP4 Receive token | `ns_tcp_recv()` (10s timeout) |
| `tcp4_close()` | TCP4 Close + DestroyChild | `ns_tcp_close()` |

The custom path activates when `ns_is_ready()` returns true — which only happens after `ns_init()` + `ns_dhcp_run()` succeeds. On QEMU, the UEFI path runs unchanged.

### Layer Details

#### ns_net.c — Core Frame Dispatch

- **`ns_init(snp)`**: Stores SNP pointer, copies MAC from `snp->Mode->CurrentAddress`, ensures SNP is INITIALIZED (calls Start+Initialize if needed)
- **`ns_raw_send(dst_mac, ethertype, payload, len)`**: Builds 14-byte Ethernet header + payload, calls `snp->Transmit()` with HeaderSize=0, recycles TX buffer via `GetStatus()`
- **`ns_poll()`**: Calls `snp->Receive()`, dispatches by EtherType:
  - `0x0806` → `ns_arp_handle()`
  - `0x0800` → `ns_ip_handle()`

#### ns_arp.c — ARP

- 16-entry static table (`NS_ARP_TABLE_SIZE`)
- `ns_arp_resolve(ip, mac_out)`: Table hit → immediate return. Miss → broadcast ARP request → poll for 3 seconds
- `ns_arp_handle(frame, len)`: Learns sender MAC→IP from every ARP frame. Replies to requests for our IP
- Same 28-byte ARP packet format as `iwn_data.c:331-358`

#### ns_ip.c — IPv4

- **`ns_ip_send(dst_ip, protocol, payload, len)`**:
  - Subnet routing: on-subnet → ARP resolve directly, off-subnet → ARP resolve gateway
  - Multicast: destination MAC calculated as `01:00:5E:(IP[1]&0x7F):IP[2]:IP[3]`
  - 20-byte IPv4 header: version=4, IHL=5, DF=1, TTL=64, no options
  - Internet checksum (RFC 1071) over header
- **`ns_ip_handle(frame, len)`**:
  - Parses IPv4 header from offset 14 (after Ethernet header)
  - Verifies header checksum, checks destination (our IP, broadcast, subnet broadcast)
  - Dispatches: protocol 17 → `ns_udp_handle()`, protocol 6 → `ns_tcp_handle()`

#### ns_udp.c — UDP

- 4-entry port binding table (`NS_UDP_MAX_BINDINGS`)
- Used by DHCP (port 68) and mDNS (port 5353)
- `ns_udp_send()`: 8-byte UDP header, checksum=0 (valid for IPv4), passes to `ns_ip_send()` with protocol=17
- `ns_udp_handle()`: Dispatches to registered callback by destination port

#### ns_dhcp.c — DHCP Client

Full blocking DHCP exchange (~5 seconds):

1. **DISCOVER**: Raw Ethernet+IP+UDP broadcast (can't use `ns_ip_send()` — no IP yet). Source 0.0.0.0:68 → dest 255.255.255.255:67. DHCP magic cookie `0x63825363`
2. **OFFER**: Parse server response. Extract offered IP, server IP, gateway, subnet, DNS from DHCP options (TLV at offset 240)
3. **REQUEST**: Broadcast requesting the offered IP with server identifier
4. **ACK**: Confirm. Store IP/gateway/subnet/DNS in `ns_state`, set `ip_ready=1`

DHCP options parsed: subnet mask (1), router/gateway (3), DNS server (6), message type (53), server identifier (54).

#### ns_tcp.c — TCP State Machine

Minimal TCP for HTTP/1.0 server (Connection: close, single connection at a time):

**Server flow**:
1. `ns_tcp_listen(port, conn)` → state = LISTEN, registered as global listener
2. `ns_tcp_accept(listener, client, timeout_ms)` → wait for SYN → send SYN+ACK (with MSS option) → wait for ACK → ESTABLISHED
3. `ns_tcp_recv(conn, buf, max_len, timeout_ms)` → return buffered data from 8KB rx_buf
4. `ns_tcp_send(conn, data, len)` → PSH+ACK segments, 1460-byte MSS chunks
5. `ns_tcp_close(conn)` → FIN+ACK, wait 2s for remote FIN

**State machine**: CLOSED → LISTEN → SYN_RCVD → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2 → CLOSING → TIME_WAIT → CLOSED

**TCP checksum**: Full pseudo-header checksum (src IP, dst IP, protocol, TCP length) + header + data.

**Simplifications** (HTTP/1.0 on LAN only):
- No window scaling (window = 8192, matching rx_buf)
- No out-of-order reassembly (drop OOO segments)
- No Nagle, no congestion control
- Single connection at a time
- ISN increments by 64000 per connection

### mDNS Integration

When the custom stack is active, mDNS uses `ns_udp_send()` for multicast instead of UEFI UDP4 ServiceBinding:

- `ba_mdns_init()`: If `ns_is_ready()` → binds port 5353 via `ns_udp_bind()`, skips UEFI UDP4 setup
- `ba_mdns_poll()`: If `ns_is_ready()` → calls `ns_poll()`, checks query flag
- `mdns_send()`: If `ns_is_ready()` → `ns_udp_send()` to multicast 224.0.0.251:5353
- `ba_mdns_stop()`: If `ns_is_ready()` → `ns_udp_unbind(5353)`

### Automatic Activation

The custom stack activates automatically during `efi_net_init()`:

```
efi_net_init()
  ├── Try UEFI IP4Config2 (existing code)
  ├── If found → UEFI path (QEMU, wired Ethernet)
  └── If not found AND iwn_snp_is_installed()
      ├── ns_init(iwn_snp_get_protocol())
      ├── ns_dhcp_run()
      ├── If DHCP succeeds → net_available = 1
      └── efi_get_local_ip() reads from ns_state
```

On QEMU (where UEFI IP4Config2 exists), the custom stack is never initialized.

---

## Known Limitations

- **No HTTPS/TLS** — would require a crypto library (mbedTLS or similar)
- **No IPv6** — only IPv4 protocols are used
- **QEMU user-mode only** — external IPs don't route on macOS; use `10.0.2.2` for host access (UEFI path)
- **Single DNS server** — hardcoded to `10.0.2.3` (QEMU forwarder, UEFI path only)
- **No DNS caching** — every hostname lookup makes a new UDP query
- **Single TCP connection** — custom stack supports one connection at a time (HTTP/1.0)
- **No TCP retransmission** — UEFI path relies on firmware; custom stack has no retransmit (LAN only)
- **No DNS on custom stack** — DNS resolution not implemented (custom stack is for server mode only)
