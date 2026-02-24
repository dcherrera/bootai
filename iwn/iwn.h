/*
 * iwn.h — Intel Wireless N driver public API for BootAI
 *
 * Phase 0: PCI probe only (detect Intel 6200 on PCI bus)
 * Phase 1: MMIO register access, firmware load
 * Phase 2: TX/RX ring setup, 802.11 scanning
 */

#pragma once

#include "../efi/efi.h"

/* Phase 0: PCI probe — enumerate PCI bus, find Intel 6200 WiFi */
EFI_STATUS iwn_probe(void);
