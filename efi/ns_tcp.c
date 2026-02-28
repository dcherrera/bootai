/*
 * ns_tcp.c — Minimal TCP for custom network stack
 *
 * Phase 5c: TCP state machine for HTTP/1.0 server.
 * Single connection at a time, no window scaling, no OOO reassembly,
 * no congestion control. LAN only with near-zero packet loss.
 *
 * Server flow: listen → accept (SYN→SYN+ACK→ACK) → recv/send → close
 */

#include "ns_net.h"

/* ---- Freestanding helpers ---- */

static void tcp_memset(void *dst, int val, int n) {
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0) *d++ = (uint8_t)val;
}

static void tcp_memcpy(void *dst, const void *src, int n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0) *d++ = *s++;
}

static int tcp_ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* ---- TCP flags ---- */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* ---- Internet checksum (RFC 1071) ---- */

/* TCP checksum with pseudo-header */
static uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                             const uint8_t *tcp_seg, int tcp_len) {
    /* Pseudo-header: src_ip(4) + dst_ip(4) + zero(1) + proto(1) + tcp_len(2) = 12 */
    uint8_t pseudo[12];
    tcp_memcpy(pseudo, src_ip, 4);
    tcp_memcpy(pseudo + 4, dst_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = 6;  /* TCP protocol */
    pseudo[10] = (uint8_t)(tcp_len >> 8);
    pseudo[11] = (uint8_t)(tcp_len);

    uint32_t sum = 0;
    /* Sum pseudo-header */
    for (int i = 0; i < 12; i += 2)
        sum += ((uint16_t)pseudo[i] << 8) | pseudo[i + 1];
    /* Sum TCP segment */
    for (int i = 0; i < tcp_len - 1; i += 2)
        sum += ((uint16_t)tcp_seg[i] << 8) | tcp_seg[i + 1];
    if (tcp_len & 1)
        sum += (uint16_t)tcp_seg[tcp_len - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---- Active connections ---- */

/* Single listener + single client (HTTP/1.0 one-at-a-time) */
static ns_tcp_conn *g_listener;
static ns_tcp_conn *g_client;

/* Initial sequence number */
static uint32_t tcp_isn = 0x10000;

/* ---- Build and send a TCP segment ---- */

static void tcp_send_segment(ns_tcp_conn *conn, uint8_t flags,
                             const uint8_t *data, int data_len) {
    ns_state *ns = ns_get_state();

    /* TCP header: 20 bytes (no options), or 24 with MSS in SYN+ACK */
    int hdr_len = 20;
    int has_mss = (flags & TCP_SYN) ? 1 : 0;
    if (has_mss) hdr_len = 24;

    uint8_t seg[1480];
    int seg_len = hdr_len + data_len;
    if (seg_len > (int)sizeof(seg)) return;

    tcp_memset(seg, 0, hdr_len);

    /* Source port */
    seg[0] = (uint8_t)(conn->local_port >> 8);
    seg[1] = (uint8_t)(conn->local_port);
    /* Destination port */
    seg[2] = (uint8_t)(conn->remote_port >> 8);
    seg[3] = (uint8_t)(conn->remote_port);
    /* Sequence number */
    seg[4] = (uint8_t)(conn->seq >> 24);
    seg[5] = (uint8_t)(conn->seq >> 16);
    seg[6] = (uint8_t)(conn->seq >> 8);
    seg[7] = (uint8_t)(conn->seq);
    /* Acknowledgment number */
    seg[8] = (uint8_t)(conn->ack >> 24);
    seg[9] = (uint8_t)(conn->ack >> 16);
    seg[10] = (uint8_t)(conn->ack >> 8);
    seg[11] = (uint8_t)(conn->ack);
    /* Data offset (header length / 4) + flags */
    seg[12] = (uint8_t)((hdr_len / 4) << 4);
    seg[13] = flags;
    /* Window size: 8192 */
    seg[14] = (uint8_t)(NS_TCP_RX_BUF_SIZE >> 8);
    seg[15] = (uint8_t)(NS_TCP_RX_BUF_SIZE);
    /* Checksum: filled below */
    /* Urgent pointer: 0 */

    /* MSS option in SYN+ACK: Kind=2, Len=4, MSS=1460 */
    if (has_mss) {
        seg[20] = 2;    /* Kind: MSS */
        seg[21] = 4;    /* Length */
        seg[22] = 0x05; /* MSS high: 1460 */
        seg[23] = 0xB4; /* MSS low */
    }

    /* Copy data payload */
    if (data_len > 0)
        tcp_memcpy(seg + hdr_len, data, data_len);

    /* Compute TCP checksum */
    uint16_t csum = tcp_checksum(ns->ip, conn->remote_ip, seg, seg_len);
    seg[16] = (uint8_t)(csum >> 8);
    seg[17] = (uint8_t)(csum);

    /* Update sequence number */
    conn->seq += (uint32_t)data_len;
    if (flags & TCP_SYN) conn->seq++;
    if (flags & TCP_FIN) conn->seq++;

    /* Send via IP layer */
    ns_ip_send(conn->remote_ip, 6, seg, seg_len);
}

/* Send a RST to reject unexpected packets */
static void tcp_send_rst(const uint8_t *src_ip, uint16_t src_port,
                         const uint8_t *dst_ip, uint16_t dst_port,
                         uint32_t seq_num, uint32_t ack_num) {
    uint8_t seg[20];
    tcp_memset(seg, 0, 20);

    seg[0] = (uint8_t)(src_port >> 8);
    seg[1] = (uint8_t)(src_port);
    seg[2] = (uint8_t)(dst_port >> 8);
    seg[3] = (uint8_t)(dst_port);
    seg[4] = (uint8_t)(seq_num >> 24);
    seg[5] = (uint8_t)(seq_num >> 16);
    seg[6] = (uint8_t)(seq_num >> 8);
    seg[7] = (uint8_t)(seq_num);
    seg[8] = (uint8_t)(ack_num >> 24);
    seg[9] = (uint8_t)(ack_num >> 16);
    seg[10] = (uint8_t)(ack_num >> 8);
    seg[11] = (uint8_t)(ack_num);
    seg[12] = 0x50; /* Data offset: 5 (20 bytes) */
    seg[13] = TCP_RST | TCP_ACK;
    seg[14] = 0; seg[15] = 0; /* Window = 0 */

    uint16_t csum = tcp_checksum(src_ip, dst_ip, seg, 20);
    seg[16] = (uint8_t)(csum >> 8);
    seg[17] = (uint8_t)(csum);

    ns_ip_send(dst_ip, 6, seg, 20);
}

/* ---- Public API ---- */

int ns_tcp_listen(uint16_t port, ns_tcp_conn *conn) {
    tcp_memset(conn, 0, sizeof(*conn));
    conn->local_port = port;
    conn->state = NS_TCP_LISTEN;
    g_listener = conn;
    return 0;
}

int ns_tcp_accept(ns_tcp_conn *listener, ns_tcp_conn *client,
                  int timeout_ms) {
    tcp_memset(client, 0, sizeof(*client));
    client->state = NS_TCP_CLOSED;
    g_client = client;
    g_client->local_port = listener->local_port;

    /* Poll for incoming SYN */
    for (int t = 0; t < timeout_ms; t++) {
        ns_poll();

        if (g_client->state == NS_TCP_ESTABLISHED) {
            g_listener = listener; /* restore listener for next accept */
            return 0;
        }

        gBS->Stall(1000); /* 1 ms */
    }

    g_client = (void *)0;
    return -1; /* timeout */
}

int ns_tcp_send(ns_tcp_conn *conn, const void *data, int len) {
    if (conn->state != NS_TCP_ESTABLISHED) return -1;

    const uint8_t *p = (const uint8_t *)data;
    int sent = 0;

    while (sent < len) {
        /* Send in MSS-sized chunks (1460 max data per segment) */
        int chunk = len - sent;
        if (chunk > 1460) chunk = 1460;

        uint8_t flags = TCP_ACK | TCP_PSH;
        tcp_send_segment(conn, flags, p + sent, chunk);
        sent += chunk;

        /* Brief stall to let remote process */
        gBS->Stall(100);
    }

    return sent;
}

int ns_tcp_recv(ns_tcp_conn *conn, void *buf, int max_len,
                int timeout_ms) {
    /* Return any buffered data first */
    if (conn->rx_len > 0) {
        int n = conn->rx_len;
        if (n > max_len) n = max_len;
        tcp_memcpy(buf, conn->rx_buf, n);

        /* Shift remaining data */
        int remaining = conn->rx_len - n;
        if (remaining > 0) {
            for (int i = 0; i < remaining; i++)
                conn->rx_buf[i] = conn->rx_buf[n + i];
        }
        conn->rx_len = remaining;
        return n;
    }

    /* If FIN received and buffer empty, connection is done */
    if (conn->rx_fin) return 0;

    /* Poll for new data */
    for (int t = 0; t < timeout_ms; t++) {
        ns_poll();

        if (conn->rx_len > 0) {
            int n = conn->rx_len;
            if (n > max_len) n = max_len;
            tcp_memcpy(buf, conn->rx_buf, n);

            int remaining = conn->rx_len - n;
            if (remaining > 0) {
                for (int i = 0; i < remaining; i++)
                    conn->rx_buf[i] = conn->rx_buf[n + i];
            }
            conn->rx_len = remaining;
            return n;
        }

        if (conn->rx_fin) return 0;
        if (conn->state == NS_TCP_CLOSED) return -1;

        gBS->Stall(1000); /* 1 ms */
    }

    return -1; /* timeout */
}

void ns_tcp_close(ns_tcp_conn *conn) {
    if (conn->state == NS_TCP_ESTABLISHED ||
        conn->state == NS_TCP_SYN_RCVD) {
        /* Send FIN+ACK */
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, (void *)0, 0);
        conn->state = NS_TCP_FIN_WAIT_1;

        /* Wait briefly for FIN+ACK from remote */
        for (int t = 0; t < 2000; t++) {
            ns_poll();
            if (conn->state == NS_TCP_CLOSED ||
                conn->state == NS_TCP_TIME_WAIT)
                break;
            gBS->Stall(1000);
        }
    }

    conn->state = NS_TCP_CLOSED;

    if (g_client == conn) g_client = (void *)0;
}

/* ---- Handle incoming TCP segment ---- */

void ns_tcp_handle(const uint8_t *ip_payload, int len,
                   const uint8_t *src_ip) {
    if (len < 20) return;

    /* Parse TCP header */
    uint16_t src_port = ((uint16_t)ip_payload[0] << 8) | ip_payload[1];
    uint16_t dst_port = ((uint16_t)ip_payload[2] << 8) | ip_payload[3];
    uint32_t seq = ((uint32_t)ip_payload[4] << 24) |
                   ((uint32_t)ip_payload[5] << 16) |
                   ((uint32_t)ip_payload[6] << 8) |
                   ip_payload[7];
    /* ack number parsed but only needed for state transitions */
    (void)ip_payload[8]; /* remote ack — used implicitly by state machine */
    int hdr_len = ((ip_payload[12] >> 4) & 0x0F) * 4;
    uint8_t flags = ip_payload[13];

    if (hdr_len < 20 || hdr_len > len) return;

    const uint8_t *data = ip_payload + hdr_len;
    int data_len = len - hdr_len;

    ns_state *ns = ns_get_state();

    /* RST — reset connection */
    if (flags & TCP_RST) {
        if (g_client && g_client->local_port == dst_port &&
            g_client->remote_port == src_port &&
            tcp_ip_eq(g_client->remote_ip, src_ip)) {
            g_client->state = NS_TCP_CLOSED;
        }
        return;
    }

    /* ---- Check active client connection first ---- */

    if (g_client && g_client->state != NS_TCP_CLOSED &&
        g_client->state != NS_TCP_LISTEN &&
        g_client->local_port == dst_port &&
        g_client->remote_port == src_port &&
        tcp_ip_eq(g_client->remote_ip, src_ip)) {

        ns_tcp_conn *c = g_client;

        switch (c->state) {
        case NS_TCP_SYN_RCVD:
            /* Expecting ACK to complete handshake */
            if (flags & TCP_ACK) {
                c->state = NS_TCP_ESTABLISHED;
            }
            break;

        case NS_TCP_ESTABLISHED:
            /* Data or FIN */
            if (data_len > 0) {
                /* Append to rx_buf if space */
                int space = NS_TCP_RX_BUF_SIZE - c->rx_len;
                int copy = data_len;
                if (copy > space) copy = space;
                if (copy > 0) {
                    tcp_memcpy(c->rx_buf + c->rx_len, data, copy);
                    c->rx_len += copy;
                }
                c->ack = seq + (uint32_t)data_len;
                /* Send ACK */
                tcp_send_segment(c, TCP_ACK, (void *)0, 0);
            }
            if (flags & TCP_FIN) {
                c->rx_fin = 1;
                c->ack = seq + (uint32_t)data_len + 1;
                tcp_send_segment(c, TCP_ACK, (void *)0, 0);
                c->state = NS_TCP_CLOSING;
            }
            break;

        case NS_TCP_FIN_WAIT_1:
            if (flags & TCP_ACK) {
                if (flags & TCP_FIN) {
                    c->ack = seq + 1;
                    tcp_send_segment(c, TCP_ACK, (void *)0, 0);
                    c->state = NS_TCP_CLOSED;
                } else {
                    c->state = NS_TCP_FIN_WAIT_2;
                }
            }
            break;

        case NS_TCP_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                c->ack = seq + 1;
                tcp_send_segment(c, TCP_ACK, (void *)0, 0);
                c->state = NS_TCP_CLOSED;
            }
            break;

        case NS_TCP_CLOSING:
            if (flags & TCP_ACK) {
                c->state = NS_TCP_CLOSED;
            }
            break;
        }
        return;
    }

    /* ---- Check listener for new SYN ---- */

    if (g_listener && g_listener->state == NS_TCP_LISTEN &&
        g_listener->local_port == dst_port &&
        (flags & TCP_SYN) && !(flags & TCP_ACK)) {

        /* New incoming connection */
        if (!g_client) {
            /* No accept pending — RST */
            tcp_send_rst(ns->ip, dst_port, src_ip, src_port,
                         0, seq + 1);
            return;
        }

        /* Initialize client connection from SYN */
        ns_tcp_conn *c = g_client;
        tcp_memcpy(c->remote_ip, src_ip, 4);
        c->remote_port = src_port;
        c->local_port = dst_port;
        c->ack = seq + 1;
        c->seq = tcp_isn;
        tcp_isn += 64000;
        c->rx_len = 0;
        c->rx_fin = 0;
        c->state = NS_TCP_SYN_RCVD;

        /* Resolve remote MAC for sending */
        ns_arp_resolve(src_ip, c->remote_mac);

        /* Send SYN+ACK */
        tcp_send_segment(c, TCP_SYN | TCP_ACK, (void *)0, 0);
        return;
    }

    /* No matching connection — send RST */
    if (!(flags & TCP_RST)) {
        uint32_t rst_seq = 0;
        uint32_t rst_ack = seq + (uint32_t)data_len;
        if (flags & TCP_SYN) rst_ack++;
        if (flags & TCP_FIN) rst_ack++;
        tcp_send_rst(ns->ip, dst_port, src_ip, src_port,
                     rst_seq, rst_ack);
    }
}
