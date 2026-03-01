/*
 * e1000e.h — Intel e1000e Ethernet driver public API for BootAI
 *
 * Phase 0: PCI probe (detect Intel Ethernet on PCI bus)
 * Phase 1+: HW init, MAC read, link, TX/RX, SNP
 */

#pragma once

#include "../efi/efi.h"

/* Phase 0: PCI probe — enumerate PCI bus, find Intel Ethernet */
EFI_STATUS e1000e_probe(void);

/* Phase 1: HW init — reset, read MAC, disable interrupts */
EFI_STATUS e1000e_init(void);

/* Status / MAC */
int  e1000e_is_up(void);
void e1000e_get_mac(uint8_t *mac);

/* Phase 2: RX poll + interactive poll command */
int  e1000e_rx_poll(uint8_t *buf, int buf_size, int *frame_len);
void e1000e_poll_cmd(void);

/* Phase 3: TX + ARP */
int  e1000e_transmit(const uint8_t *frame, int len);
int  e1000e_send(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, int payload_len);
void e1000e_arp_cmd(void);

/* Phase 4: SNP-compatible wrappers for ns_* network stack */
int  e1000e_snp_is_installed(void);
void e1000e_snp_get_mac(uint8_t *mac);
int  e1000e_snp_direct_tx(const uint8_t *dst_mac, uint16_t ethertype,
                           const uint8_t *payload, int len);
int  e1000e_snp_direct_rx(uint8_t *buf, int buf_size, int *frame_len);
