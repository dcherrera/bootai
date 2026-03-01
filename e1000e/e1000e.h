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
