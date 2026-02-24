/*
 * iwn_shim.h — UEFI shim types for OpenBSD driver headers
 *
 * Maps OpenBSD/BSD kernel types to standard C types so that
 * if_iwnreg.h can compile in a freestanding UEFI environment.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* OpenBSD -> standard C type mappings */
typedef uint8_t   u_int8_t;
typedef uint16_t  u_int16_t;
typedef uint32_t  u_int32_t;
typedef uint64_t  u_int64_t;

/* Bus space types (will be fleshed out in Phase 1 for MMIO) */
typedef void     *bus_space_tag_t;
typedef uint64_t  bus_space_handle_t;
typedef uint64_t  bus_addr_t;
typedef uint64_t  bus_size_t;

/* IEEE 802.11 constants */
#define IEEE80211_ADDR_LEN      6
#define IEEE80211_TKIP_MICLEN   8
#define IEEE80211_NWID_LEN      32

/* EDCA (Enhanced Distributed Channel Access) — 4 access categories */
#define EDCA_NUM_AC  4

/* PCI base address register 0 offset */
#define PCI_MAPREG_START  0x10

/* Bus space barrier flags (no-ops for now, Phase 1 will implement) */
#define BUS_SPACE_BARRIER_READ   0x01
#define BUS_SPACE_BARRIER_WRITE  0x02

/* Byte-order macros — x86 is little-endian, so these are no-ops */
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define letoh16(x) ((uint16_t)(x))
#define letoh32(x) ((uint32_t)(x))

/* Compiler attributes */
#ifndef __packed
#define __packed __attribute__((packed))
#endif
