/*
 * iwn.h — Intel Wireless N driver public API for BootAI
 *
 * Phase 0: PCI probe (detect Intel WiFi on PCI bus)
 * Phase 1: MMIO register access, EEPROM read (MAC, domain, RF config)
 * Phase 2: DMA rings, firmware load, NIC alive
 */

#pragma once

#include "../efi/efi.h"

/* Forward declaration */
typedef struct iwn_dev iwn_dev;

/* Phase 0: PCI probe — enumerate PCI bus, find Intel WiFi card */
EFI_STATUS iwn_probe(void);

/* Phase 1: MMIO + EEPROM read — power on, read MAC/domain/RF config */
EFI_STATUS iwn_attach(iwn_dev *dev);

/* Phase 2: DMA + firmware — allocate rings, load firmware, get UC_READY */
int iwn_init(iwn_dev *dev);

/* Phase 3: WiFi scanning — scan for nearby networks */
void iwn_scan_cmd(void);

/* Phase 4: Association — connect to a network */
void iwn_connect_cmd(const char *ssid);
void iwn_set_password(const char *password);

/* Phase 4d: Data TX/RX — send/receive data frames */
void iwn_ping_cmd(void);
