/*
 * ns_net.h — Custom network stack types and declarations
 *
 * Phase 5c: ARP/DHCP/IP/UDP/TCP directly on raw SNP frames.
 * Used on Dell E6510 where UEFI IP stack DXE drivers are absent.
 */

#ifndef BOOTAI_NS_NET_H
#define BOOTAI_NS_NET_H

#include "efi.h"

/* ---- Driver selection ---- */

#define NS_DRIVER_NONE   0
#define NS_DRIVER_IWN    1
#define NS_DRIVER_E1000E 2

/* ---- Network state ---- */

typedef struct {
    EFI_SIMPLE_NETWORK_PROTOCOL *snp;
    int     driver;           /* NS_DRIVER_* */
    uint8_t mac[6];           /* Our MAC */
    uint8_t ip[4];            /* Our IP (from DHCP) */
    uint8_t gateway[4];       /* Gateway IP */
    uint8_t subnet[4];        /* Subnet mask */
    uint8_t dns[4];           /* DNS server */
    int     ip_ready;         /* 1 after DHCP completes */
} ns_state;

/* ---- ARP table entry ---- */

#define NS_ARP_TABLE_SIZE 64

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
} ns_arp_entry;

/* ---- TCP connection ---- */

#define NS_TCP_RX_BUF_SIZE 65536

struct ns_tcp_conn {
    uint8_t  remote_ip[4];
    uint8_t  remote_mac[6];
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t seq;             /* Our sequence number */
    uint32_t ack;             /* Their sequence number */
    int      state;
    uint8_t  rx_buf[NS_TCP_RX_BUF_SIZE];
    int      rx_len;          /* Bytes in rx_buf */
    int      rx_fin;          /* FIN received */
};

/* TCP states */
#define NS_TCP_CLOSED      0
#define NS_TCP_LISTEN      1
#define NS_TCP_SYN_RCVD    2
#define NS_TCP_ESTABLISHED 3
#define NS_TCP_FIN_WAIT_1  4
#define NS_TCP_FIN_WAIT_2  5
#define NS_TCP_CLOSING     6
#define NS_TCP_TIME_WAIT   7

/* ---- UDP port callback ---- */

#define NS_UDP_MAX_BINDINGS 4

typedef void (*ns_udp_callback)(const uint8_t *data, int len,
                                const uint8_t *src_ip, uint16_t src_port);

/* ---- Public API: init + poll ---- */

void     ns_init(EFI_SIMPLE_NETWORK_PROTOCOL *snp);
int      ns_is_ready(void);      /* 1 after DHCP */
void     ns_get_ip(uint8_t *ip);  /* Copy our IP */
void     ns_get_mac(uint8_t *mac);
void     ns_poll(void);           /* Process one pending frame */

/* ---- ARP ---- */

int      ns_arp_resolve(const uint8_t *ip, uint8_t *mac_out);
void     ns_arp_handle(const uint8_t *frame, int len);
void     ns_arp_learn(const uint8_t *ip, const uint8_t *mac);
void     ns_arp_dump(void);  /* Print ARP table for diagnostics */

/* ---- DHCP ---- */

int      ns_dhcp_run(void);  /* blocking: returns 0 on success */

/* ---- IP ---- */

int      ns_ip_send(const uint8_t *dst_ip, uint8_t protocol,
                    const uint8_t *payload, int len);
void     ns_ip_handle(const uint8_t *frame, int len);

/* ---- UDP ---- */

void     ns_udp_send(const uint8_t *dst_ip, uint16_t src_port,
                     uint16_t dst_port, const uint8_t *data, int len);
void     ns_udp_handle(const uint8_t *ip_payload, int len,
                       const uint8_t *src_ip);
int      ns_udp_bind(uint16_t port, ns_udp_callback cb);
void     ns_udp_unbind(uint16_t port);

/* ---- TCP ---- */

int      ns_tcp_listen(uint16_t port, ns_tcp_conn *conn);
int      ns_tcp_accept(ns_tcp_conn *listener, ns_tcp_conn *client,
                       int timeout_ms);
int      ns_tcp_send(ns_tcp_conn *conn, const void *data, int len);
int      ns_tcp_recv(ns_tcp_conn *conn, void *buf, int max_len,
                     int timeout_ms);
void     ns_tcp_close(ns_tcp_conn *conn);
void     ns_tcp_handle(const uint8_t *ip_payload, int len,
                       const uint8_t *src_ip);

/* ---- Internal: raw frame send ---- */

void     ns_raw_send(const uint8_t *dst_mac, uint16_t ethertype,
                     const uint8_t *payload, int len);

/* ---- Internal: access global state ---- */

ns_state *ns_get_state(void);

/* ---- Debug: frame counters + diagnostics ---- */

int      ns_poll_rx_count(void);  /* Total frames received by ns_poll() */
void     ns_tcp_diag(void);       /* Print TCP state for diagnostics */

#endif /* BOOTAI_NS_NET_H */
