/*
 * iwn_internal.h — Shared internal header for Intel Wireless N driver
 *
 * Exposes MMIO, peripheral register, NIC lock, APM, and print functions
 * from iwn_hw.c for use by iwn_dma.c and iwn_fw.c.
 *
 * Not part of the public API — include only from iwn_*.c files.
 */

#pragma once

#include "iwn.h"
#include "if_iwnvar.h"

/* ---- MMIO (BAR0 register access) ---- */

uint32_t iwn_read(iwn_dev *dev, uint32_t reg);
void iwn_write(iwn_dev *dev, uint32_t reg, uint32_t val);
void iwn_write_1(iwn_dev *dev, uint32_t reg, uint8_t val);
void iwn_setbits(iwn_dev *dev, uint32_t reg, uint32_t mask);
void iwn_clrbits(iwn_dev *dev, uint32_t reg, uint32_t mask);

/* ---- Peripheral registers (require NIC lock held) ---- */

uint32_t iwn_prph_read(iwn_dev *dev, uint32_t addr);
void iwn_prph_write(iwn_dev *dev, uint32_t addr, uint32_t val);
void iwn_prph_setbits(iwn_dev *dev, uint32_t addr, uint32_t mask);
void iwn_prph_clrbits(iwn_dev *dev, uint32_t addr, uint32_t mask);

/* ---- NIC-internal SRAM access (require NIC lock held) ---- */

uint32_t iwn_mem_read(iwn_dev *dev, uint32_t addr);
void iwn_mem_write(iwn_dev *dev, uint32_t addr, uint32_t val);
void iwn_mem_set_region_4(iwn_dev *dev, uint32_t addr, uint32_t val, int count);

/* ---- NIC lock (10ms timeout) ---- */

int iwn_nic_lock(iwn_dev *dev);
void iwn_nic_unlock(iwn_dev *dev);

/* ---- APM (power management) ---- */

int iwn_apm_init(iwn_dev *dev);
void iwn_apm_stop(iwn_dev *dev);

/* ---- NIC config (write radio/antenna config to HW_IF_CONFIG) ---- */

void iwn5000_nic_config(iwn_dev *dev);

/* ---- EEPROM ---- */

int iwn_read_prom_data(iwn_dev *dev, uint32_t addr, void *data, int count);

/* ---- Memory helpers (freestanding, no libc) ---- */

void iwn_memcpy(void *dst, const void *src, uint32_t n);
void iwn_memset(void *dst, uint8_t val, uint32_t n);

/* ---- Print helpers ---- */

void print_dec(uint32_t val);
void print_hex8(uint8_t val);

/* ---- DMA allocation (iwn_dma.c) ---- */

int iwn_alloc_kw(iwn_dev *dev);
int iwn_alloc_sched(iwn_dev *dev);
int iwn_alloc_tx_ring(iwn_dev *dev, int qid);
int iwn_alloc_rx_ring(iwn_dev *dev);
void iwn_reset_tx_ring(iwn_dev *dev, int qid);
void iwn_reset_rx_ring(iwn_dev *dev);
void iwn_free_dma(iwn_dev *dev);

/* ---- Firmware (iwn_fw.c) ---- */

int iwn_fw_parse(iwn_dev *dev);
int iwn5000_load_firmware(iwn_dev *dev, const uint8_t *text, uint32_t text_len,
                          const uint8_t *data, uint32_t data_len);
int iwn_poll_rx(iwn_dev *dev, uint8_t expected_type, int timeout_ms);
int iwn_cmd(iwn_dev *dev, uint8_t code, const void *payload, uint32_t len);
void iwn5000_post_alive(iwn_dev *dev);
void iwn_replay_calibration(iwn_dev *dev);
int iwn_hw_init(iwn_dev *dev);
int iwn_fw_restart(iwn_dev *dev);

/* ---- Command wait (iwn_scan.c, shared with iwn_assoc.c) ---- */

int iwn_wait_cmd(iwn_dev *dev, uint8_t cmd_code, const char *cmd_name,
                 int timeout_ms);

/* ---- Scan (iwn_scan.c) ---- */

int iwn_scan(iwn_dev *dev);

/* ---- Association (iwn_assoc.c) ---- */

int iwn_connect(iwn_dev *dev, const char *ssid);

/* ---- EAPOL (iwn_eapol.c) ---- */

int iwn_eapol_handshake(iwn_dev *dev);

/* ---- CCMP software decrypt (iwn_ccmp.c) ---- */

void ccmp_self_test(void);
int iwn_ccmp_decrypt(uint8_t *frame, int frame_len, const uint8_t *key);

/* ---- Data TX/RX (iwn_data.c) ---- */

int iwn_tx_raw(iwn_dev *dev, const uint8_t *frame, uint16_t frame_len,
               uint8_t node_id, uint8_t security,
               uint8_t plcp, uint8_t rflags);
int iwn_encap_tx(iwn_dev *dev, const uint8_t *dst_mac,
                 uint16_t ethertype, const uint8_t *payload, uint16_t payload_len);
int iwn_rx_poll(iwn_dev *dev);
int iwn_rx_pending(iwn_dev *dev);
int iwn_rx_dequeue(iwn_dev *dev, uint8_t *buf, uint16_t *buf_len);
void iwn_ping_cmd(void);

/* ---- SNP wrapper (iwn_snp.c) ---- */

int iwn_snp_install(void);

/* ---- Global device state (iwn_pci.c) ---- */

extern iwn_dev g_iwn;
extern int g_iwn_found;
