/*
 * if_iwnvar.h — Phase 0 stub for Intel Wireless N driver state
 *
 * The full OpenBSD if_iwnvar.h has ~339 lines of heavily OS-dependent
 * structs. We'll adapt them incrementally in Phases 1-2.
 * For Phase 0, we only need PCI device ID constants.
 */

#pragma once

#include "iwn_shim.h"
#include "if_iwnreg.h"

/* Intel WiFi PCI device IDs */
#define PCI_VENDOR_INTEL      0x8086

/* Centrino Advanced-N 6200 (2x2 AGN) — Dell E6510 WiFi card */
#define IWN_DID_6200_1        0x422C
#define IWN_DID_6200_2        0x4239

/* Forward declare — will be filled in Phase 1+ */
struct iwn_softc;
