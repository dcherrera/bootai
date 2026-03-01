/*
 * e1000e_regs.h — Intel e1000e register offsets, bit definitions, descriptors
 *
 * Covers 82577LM/LC, 82578, 82579, I217, I218, I219, 82574L (QEMU).
 * Register offsets from Intel PCH/ICH datasheet + Linux e1000e driver.
 */

#pragma once

#include <stdint.h>

/* ---- General registers ---- */

#define E1000_CTRL      0x0000  /* Device Control */
#define E1000_STATUS    0x0008  /* Device Status */
#define E1000_EERD      0x0014  /* EEPROM Read */
#define E1000_ICR       0x00C0  /* Interrupt Cause Read */
#define E1000_IMS       0x00D0  /* Interrupt Mask Set */
#define E1000_IMC       0x00D8  /* Interrupt Mask Clear */

/* ---- Receive registers ---- */

#define E1000_RCTL      0x0100  /* Receive Control */
#define E1000_RDBAL     0x2800  /* RX Descriptor Base Low */
#define E1000_RDBAH     0x2804  /* RX Descriptor Base High */
#define E1000_RDLEN     0x2808  /* RX Descriptor Length */
#define E1000_RDH       0x2810  /* RX Descriptor Head */
#define E1000_RDT       0x2818  /* RX Descriptor Tail */

/* ---- Transmit registers ---- */

#define E1000_TCTL      0x0400  /* Transmit Control */
#define E1000_TIPG      0x0410  /* TX Inter-Packet Gap */
#define E1000_TDBAL     0x3800  /* TX Descriptor Base Low */
#define E1000_TDBAH     0x3804  /* TX Descriptor Base High */
#define E1000_TDLEN     0x3808  /* TX Descriptor Length */
#define E1000_TDH       0x3810  /* TX Descriptor Head */
#define E1000_TDT       0x3818  /* TX Descriptor Tail */

/* ---- MAC / Packet Buffer registers ---- */

#define E1000_RAL0      0x5400  /* Receive Address Low (MAC bytes 0-3) */
#define E1000_RAH0      0x5404  /* Receive Address High (MAC bytes 4-5 + AV) */
#define E1000_RAH_AV    (1u << 31) /* Address Valid */
#define E1000_PBA       0x1000  /* Packet Buffer Allocation */
#define E1000_PBS       0x1008  /* Packet Buffer Size */

/* ---- CTRL register bits ---- */

#define E1000_CTRL_FD       (1 << 0)   /* Full Duplex */
#define E1000_CTRL_ASDE     (1 << 5)   /* Auto-Speed Detection Enable */
#define E1000_CTRL_SLU      (1 << 6)   /* Set Link Up */
#define E1000_CTRL_RST      (1 << 26)  /* Device Reset */
#define E1000_CTRL_PHY_RST  (1 << 31)  /* PHY Reset */
#define E1000_CTRL_FRCSPD   (1 << 11)  /* Force Speed */
#define E1000_CTRL_FRCDPX   (1 << 12)  /* Force Duplex */

/* ---- STATUS register bits ---- */

#define E1000_STATUS_FD         (1 << 0)  /* Full Duplex */
#define E1000_STATUS_LU         (1 << 1)  /* Link Up */
#define E1000_STATUS_SPEED_100  (1 << 6)  /* Speed 100 Mbps */
#define E1000_STATUS_SPEED_1000 (1 << 7)  /* Speed 1000 Mbps */

/* ---- RCTL register bits ---- */

#define E1000_RCTL_EN       (1 << 1)   /* Receiver Enable */
#define E1000_RCTL_BAM      (1 << 15)  /* Broadcast Accept Mode */
#define E1000_RCTL_SECRC    (1 << 26)  /* Strip Ethernet CRC */
#define E1000_RCTL_UPE      (1 << 3)   /* Unicast Promiscuous Enable */
#define E1000_RCTL_MPE      (1 << 4)   /* Multicast Promiscuous Enable */
#define E1000_RCTL_BSIZE_2K 0          /* Buffer size 2048 (default) */

/* ---- TCTL register bits ---- */

#define E1000_TCTL_EN       (1 << 1)   /* Transmit Enable */
#define E1000_TCTL_PSP      (1 << 3)   /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT 4          /* Collision Threshold */
#define E1000_TCTL_COLD_SHIFT 12       /* Collision Distance */

/* ---- TX command bits (in tx descriptor) ---- */

#define E1000_TXD_CMD_EOP   (1 << 0)  /* End Of Packet */
#define E1000_TXD_CMD_IFCS  (1 << 1)  /* Insert FCS/CRC */
#define E1000_TXD_CMD_RS    (1 << 3)  /* Report Status */

/* ---- RX/TX status bits (in descriptors) ---- */

#define E1000_RXD_STAT_DD   (1 << 0)  /* Descriptor Done */
#define E1000_RXD_STAT_EOP  (1 << 1)  /* End Of Packet */
#define E1000_TXD_STAT_DD   (1 << 0)  /* Descriptor Done */

/* ---- EEPROM Read register bits ---- */

#define E1000_EERD_START    (1 << 0)  /* Start Read */
#define E1000_EERD_DONE     (1 << 4)  /* Read Done */
#define E1000_EERD_ADDR_SHIFT 8       /* Address shift */
#define E1000_EERD_DATA_SHIFT 16      /* Data shift */

/* ---- Descriptor structs (16 bytes each, hardware layout) ---- */

typedef struct __attribute__((packed)) {
    uint64_t addr;      /* Buffer physical address */
    uint16_t length;    /* Packet length */
    uint16_t checksum;  /* Packet checksum */
    uint8_t  status;    /* Descriptor status */
    uint8_t  errors;    /* Descriptor errors */
    uint16_t special;   /* VLAN tag */
} e1000e_rx_desc;

typedef struct __attribute__((packed)) {
    uint64_t addr;      /* Buffer physical address */
    uint16_t length;    /* Data length */
    uint8_t  cso;       /* Checksum offset */
    uint8_t  cmd;       /* Command field */
    uint8_t  status;    /* Descriptor status (DD) */
    uint8_t  css;       /* Checksum start */
    uint16_t special;   /* VLAN tag */
} e1000e_tx_desc;

/* ---- Device ID table ---- */

#define E1000E_VENDOR_INTEL  0x8086

/*
 * NOTE: Use char[] not char* for name. UEFI PE images have no base
 * relocations (reloc.c is a dummy), so pointer-valued static data
 * is broken at runtime. Inline char arrays avoid this.
 * Sentinel: check id->device != 0 (not id->name).
 */
struct e1000e_ident {
    uint16_t device;
    char     name[12];
};

static const struct e1000e_ident e1000e_ident_table[] = {
    { 0x10EA, "82577LM"  },      /* Dell E6510, ThinkPad T410 */
    { 0x10EB, "82577LC"  },      /* Consumer variant */
    { 0x10EF, "82578DM"  },      /* Desktop */
    { 0x1502, "82579LM"  },      /* ThinkPad T520, Dell E6520 */
    { 0x1503, "82579V"   },      /* Consumer variant */
    { 0x153A, "I217-LM"  },      /* ThinkPad T440, Dell E7440 */
    { 0x153B, "I217-V"   },      /* Consumer variant */
    { 0x15A0, "I218-LM"  },      /* ThinkPad T450 */
    { 0x15A1, "I218-V"   },      /* Consumer variant */
    { 0x156F, "I219-LM"  },      /* ThinkPad T480+ */
    { 0x1570, "I219-V"   },      /* Consumer variant */
    { 0x10D3, "82574L"   },      /* QEMU e1000e device */
    { 0,      ""          }      /* sentinel */
};
