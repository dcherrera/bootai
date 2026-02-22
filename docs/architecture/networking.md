# Networking Architecture

Deep dive into BootAI's network stack: DHCP, TCP4, UDP4 DNS, and HTTP/1.0.

All networking code lives in `efi/net.c`. Protocol type definitions are in `efi/efi.h`.

## Overview

```
HTTP/1.0 GET (efi_http_get)
    ↓
DNS Resolution (efi_dns_resolve)          ← UDP4 wire-format DNS client
    ↓
TCP4 Connect/Send/Recv/Close              ← Service binding pattern
    ↓
IP4Config2 (DHCP)                         ← Automatic IP configuration
    ↓
ConnectController                         ← Driver chain binding
    ↓
VirtioNetDxe → MnpDxe → ArpDxe → Ip4Dxe → Tcp4Dxe / Udp4Dxe
```

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

## Known Limitations

- **No HTTPS/TLS** — would require a crypto library (mbedTLS or similar)
- **No IPv6** — only IPv4 protocols are used
- **QEMU user-mode only** — external IPs don't route on macOS; use `10.0.2.2` for host access
- **Single DNS server** — hardcoded to `10.0.2.3` (QEMU forwarder)
- **No DNS caching** — every hostname lookup makes a new UDP query
- **No TCP retransmission** — relies on firmware's TCP4 implementation
