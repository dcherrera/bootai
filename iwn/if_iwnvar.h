/*
 * if_iwnvar.h — Intel Wireless N driver state for BootAI (Phase 1)
 *
 * Device struct, device ID table, flag/HW type constants.
 * Adapted from OpenBSD's if_iwnvar.h for freestanding UEFI.
 */

#pragma once

#include "iwn_shim.h"
#include "if_iwnreg.h"
#include "../efi/efi.h"

/* Intel WiFi PCI vendor ID */
#define PCI_VENDOR_INTEL      0x8086

/* ---- Device flags ---- */
#define IWN_FLAG_HAS_OTPROM   (1 << 0)
#define IWN_FLAG_HAS_11N      (1 << 1)
#define IWN_FLAG_INTERNAL_PA  (1 << 2)
#define IWN_FLAG_HAS_5GHZ     (1 << 3)
#define IWN_FLAG_ADV_BT_COEX  (1 << 4)
#define IWN_FLAG_PAN_SUPPORT  (1 << 5)

/* ---- Device struct ---- */
typedef struct iwn_dev {
    EFI_PCI_IO_PROTOCOL *pci_io;    /* UEFI PCI IO handle */
    uint16_t        device_id;      /* PCI device ID */
    uint8_t         hw_type;        /* (HW_REV >> 4) & 0x1f */
    uint32_t        flags;          /* IWN_FLAG_* */
    uint32_t        prom_base;      /* OTP/EEPROM base offset */
    uint8_t         pcie_cap_off;   /* PCIe capability offset in config space */
    uint8_t         mac_addr[6];    /* from EEPROM */
    char            eeprom_domain[4]; /* regulatory domain string */
    uint16_t        rfcfg;          /* RF config word from EEPROM */
    uint8_t         txchainmask;    /* TX antenna chain mask */
    uint8_t         rxchainmask;    /* RX antenna chain mask */
    uint8_t         calib_ver;      /* calibration version */
    uint32_t        eeprom_crystal; /* crystal calibration value */
} iwn_dev;

/* ---- Device identification table ---- */
struct iwn_ident {
    uint16_t    vendor;
    uint16_t    device;
    const char *name;
};

static const struct iwn_ident iwn_ident_table[] = {
    /* WiFi Link 4965 */
    { 0x8086, 0x4229, "WiFi Link 4965" },
    { 0x8086, 0x4230, "WiFi Link 4965" },

    /* WiFi Link 5100 */
    { 0x8086, 0x4232, "WiFi Link 5100" },
    { 0x8086, 0x4237, "WiFi Link 5100" },

    /* WiFi Link 5300 */
    { 0x8086, 0x4235, "WiFi Link 5300" },
    { 0x8086, 0x4236, "WiFi Link 5300" },

    /* WiFi Link 5350 */
    { 0x8086, 0x423a, "WiFi Link 5350" },
    { 0x8086, 0x423b, "WiFi Link 5350" },

    /* WiFi Link 5150 */
    { 0x8086, 0x423c, "WiFi Link 5150" },
    { 0x8086, 0x423d, "WiFi Link 5150" },

    /* Centrino Advanced-N 1000 */
    { 0x8086, 0x0083, "Centrino Wireless-N 1000" },
    { 0x8086, 0x0084, "Centrino Wireless-N 1000" },

    /* Centrino Advanced-N 6000 */
    { 0x8086, 0x422b, "Centrino Advanced-N 6000" },
    { 0x8086, 0x4238, "Centrino Advanced-N 6000" },

    /* Centrino Advanced-N 6200 (Dell E6510) */
    { 0x8086, 0x422c, "Centrino Advanced-N 6200" },
    { 0x8086, 0x4239, "Centrino Advanced-N 6200" },

    /* Centrino Advanced-N 6050 */
    { 0x8086, 0x422d, "Centrino Advanced-N 6050" },
    { 0x8086, 0x4233, "Centrino Advanced-N 6050" },

    /* Centrino Advanced-N 6005 */
    { 0x8086, 0x0082, "Centrino Advanced-N 6005" },
    { 0x8086, 0x0085, "Centrino Advanced-N 6005" },

    /* Centrino Advanced-N 6030 */
    { 0x8086, 0x0087, "Centrino Advanced-N 6030" },
    { 0x8086, 0x0089, "Centrino Advanced-N 6030" },

    /* Centrino Wireless-N 2200 */
    { 0x8086, 0x0890, "Centrino Wireless-N 2200" },
    { 0x8086, 0x0891, "Centrino Wireless-N 2200" },

    /* Centrino Wireless-N 2030 */
    { 0x8086, 0x0887, "Centrino Wireless-N 2030" },
    { 0x8086, 0x0888, "Centrino Wireless-N 2030" },

    /* Centrino Wireless-N 105 */
    { 0x8086, 0x0894, "Centrino Wireless-N 105" },
    { 0x8086, 0x0895, "Centrino Wireless-N 105" },

    /* Centrino Wireless-N 135 */
    { 0x8086, 0x0892, "Centrino Wireless-N 135" },
    { 0x8086, 0x0893, "Centrino Wireless-N 135" },

    /* Sentinel */
    { 0, 0, NULL }
};
