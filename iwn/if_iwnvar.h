/*
 * if_iwnvar.h — Intel Wireless N driver state for BootAI (Phase 1+2)
 *
 * Device struct, DMA ring structs, firmware state, device ID table.
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

/* ---- Phase 2: DMA ring structures ---- */

/* DMA memory descriptor (physical address + virtual address + size) */
typedef struct {
    uint64_t    paddr;      /* physical address */
    void       *vaddr;      /* virtual address (== paddr in UEFI) */
    uint32_t    size;       /* bytes allocated */
} iwn_dma_info;

/* RX ring — 64 descriptor slots + status page + 64 x 4KB buffers */
typedef struct {
    iwn_dma_info    desc;       /* 64 x uint32_t descriptor array */
    iwn_dma_info    stat;       /* status page (hw read pointer) */
    iwn_dma_info    buf[IWN_RX_RING_COUNT]; /* 64 x 4KB buffers */
    int             cur;        /* current read index */
} iwn_rx_ring;

/* TX ring — 256 descriptors (128 bytes each) + 256 commands (140 bytes each) */
typedef struct {
    iwn_dma_info    desc;       /* 256 x 128-byte descriptors */
    iwn_dma_info    cmd;        /* 256 x 140-byte commands */
    int             qid;        /* queue ID */
    int             cur;        /* current write index */
    int             queued;     /* entries queued */
} iwn_tx_ring;

/* Parsed firmware image sections */
typedef struct {
    const uint8_t  *main_text;  uint32_t main_text_len;
    const uint8_t  *main_data;  uint32_t main_data_len;
    const uint8_t  *init_text;  uint32_t init_text_len;
    const uint8_t  *init_data;  uint32_t init_data_len;
    uint32_t        fw_api;     /* firmware API version */
} iwn_fw;

/* Calibration result (saved from init ucode, replayed to runtime ucode) */
typedef struct {
    uint8_t     data[2048];     /* raw calibration command data */
    uint32_t    len;            /* bytes used */
} iwn_calib_cmd;

#define IWN_MAX_CALIB_RESULTS  16

/* FH DMA firmware upload chunk size (128 KB) */
#define IWN_FW_CHUNK_SIZE      (128 * 1024)

/* ---- Scan result entry ---- */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];       /* 32 + NUL */
    uint8_t  ssid_len;
    int8_t   rssi;           /* dBm */
    uint8_t  channel;
    uint8_t  security;       /* 0=open, 1=WPA, 2=WPA2, 3=WPA+WPA2 */
    uint16_t beacon_interval; /* TU from beacon frame */
} iwn_scan_result;

#define IWN_MAX_SCAN_RESULTS 32

/* ---- Device struct ---- */
typedef struct iwn_dev {
    /* Phase 1: PCI + EEPROM state */
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

    /* Phase 2: DMA rings */
    iwn_dma_info    kw;                     /* keep-warm page (4 KB) */
    iwn_dma_info    sched;                  /* TX scheduler table */
    iwn_tx_ring     txq[5];                 /* TX queues 0-4 (4 = cmd) */
    iwn_rx_ring     rxq;                    /* RX queue */

    /* Phase 2: Firmware */
    iwn_fw          fw;                     /* parsed firmware sections */
    void           *fw_rawbuf;              /* raw file buffer (freed after parse) */
    UINTN           fw_rawsize;             /* raw file size */
    iwn_calib_cmd   calib[IWN_MAX_CALIB_RESULTS]; /* saved calibration */
    int             n_calib;                /* number of saved calibration results */
    int             fw_loaded;              /* 1 if runtime ucode running */

    /* Phase 2: DMA scratch buffer for firmware upload */
    iwn_dma_info    fw_dma;                 /* DMA buffer for firmware sections */

    /* Phase 3: Scan state */
    iwn_scan_result  scan_results[IWN_MAX_SCAN_RESULTS];
    int              n_scan_results;
    iwn_dma_info     cmd_dma;               /* DMA buffer for big commands (4KB) */

    /* Phase 4: Association state */
    uint8_t  state;              /* IWN_STATE_* */
    uint8_t  target_bssid[6];
    char     target_ssid[33];
    uint8_t  target_ssid_len;
    uint8_t  target_channel;
    uint16_t target_beacon_interval;
    uint8_t  target_security;    /* from scan result */
    uint16_t assoc_id;           /* AID from assoc response */
    char     password[64];       /* WPA2 passphrase (for Phase 4c) */
    uint8_t  rsn_ie[32];         /* saved RSN IE for EAPOL replay */
    uint8_t  rsn_ie_len;

    /* Phase 4c: EAPOL state */
    uint8_t  ptk[48];            /* Pairwise Transient Key (KCK+KEK+TK) */
    uint8_t  gtk[16];            /* Group Temporal Key */
    uint8_t  gtk_kid;            /* GTK key index (0-3) */
    uint8_t  keys_installed;     /* 1 after PTK+GTK installed */
} iwn_dev;

/* ---- Connection state machine ---- */
#define IWN_STATE_INIT   0
#define IWN_STATE_SCAN   1
#define IWN_STATE_AUTH   2
#define IWN_STATE_ASSOC  3
#define IWN_STATE_RUN    4

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
