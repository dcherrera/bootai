/*
 * iwn_fw.c — Firmware loading and hardware init for Intel Wireless N
 *
 * Phase 2: Parse TLV firmware, upload via FH DMA service channel (ch 9),
 * poll for UC_READY, post-alive calibration, power-cycle,
 * load runtime firmware, replay calibration.
 *
 * The 6200 (5000+ series) uses FH DMA to load firmware, not BSM bootstrap.
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 *            itlwm (macOS Intel WiFi port)
 */

#include "iwn_internal.h"

/* ---- Helper: memcpy for freestanding (no libc) ---- */

static void iwn_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static void iwn_memset(void *dst, uint8_t val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = val;
}

/* ---- Read a little-endian uint32 from a byte pointer ---- */

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Legacy firmware header (v2, for API >= 2) ---- */
/*
 * Layout (28 bytes):
 *   uint32_t ver             — major.minor.API.serial
 *   uint32_t build           — build number
 *   uint32_t inst_size       — main text (runtime code)
 *   uint32_t data_size       — main data (runtime data)
 *   uint32_t init_size       — init text
 *   uint32_t init_data_size  — init data
 *   uint32_t boot_size       — boot text (unused on 5000+)
 *
 * Sections follow concatenated: inst, data, init, init_data, boot
 */
#define IWN_FW_LEGACY_HDR_SIZE  28

/* ---- Firmware parser (legacy + TLV) ---- */

int iwn_fw_parse(iwn_dev *dev) {
    EFI_STATUS s;

    /* Load firmware file from USB/disk */
    s = efi_read_file("drivers/iwn-6000", &dev->fw_rawbuf, &dev->fw_rawsize);
    if (s != EFI_SUCCESS) {
        efi_print_ascii("  iwn: firmware not found at drivers/iwn-6000\r\n");
        return -1;
    }

    efi_print_ascii("  Loading firmware: drivers/iwn-6000 (");
    print_dec((uint32_t)dev->fw_rawsize);
    efi_print_ascii(" bytes)\r\n");

    if (dev->fw_rawsize < 28) {
        efi_print_ascii("  iwn: firmware too small\r\n");
        return -1;
    }

    uint8_t *raw = (uint8_t *)dev->fw_rawbuf;

    dev->fw.main_text = 0; dev->fw.main_text_len = 0;
    dev->fw.main_data = 0; dev->fw.main_data_len = 0;
    dev->fw.init_text = 0; dev->fw.init_text_len = 0;
    dev->fw.init_data = 0; dev->fw.init_data_len = 0;

    /* Detect format: TLV has first uint32 == 0, legacy has version */
    uint32_t first_word = read_le32(raw);

    if (first_word == 0) {
        /* ---- TLV format ---- */
        struct iwn_fw_tlv_hdr *hdr = (struct iwn_fw_tlv_hdr *)raw;
        if (hdr->signature != IWN_FW_SIGNATURE) {
            efi_print_ascii("  iwn: invalid firmware signature\r\n");
            return -1;
        }

        dev->fw.fw_api = IWN_FW_API(hdr->rev);
        efi_print_ascii("  FW format: TLV, API ");
        print_dec(dev->fw.fw_api);
        efi_print_ascii("\r\n");

        uint8_t *ptr = raw + sizeof(struct iwn_fw_tlv_hdr);
        uint8_t *end = raw + dev->fw_rawsize;

        while (ptr + sizeof(struct iwn_fw_tlv) <= end) {
            struct iwn_fw_tlv *tlv = (struct iwn_fw_tlv *)ptr;
            uint8_t *data = ptr + sizeof(struct iwn_fw_tlv);
            uint32_t len = tlv->len;
            uint32_t padded = (len + 3) & ~3u;
            if (data + padded > end) break;

            switch (tlv->type) {
            case IWN_FW_TLV_MAIN_TEXT: dev->fw.main_text = data; dev->fw.main_text_len = len; break;
            case IWN_FW_TLV_MAIN_DATA: dev->fw.main_data = data; dev->fw.main_data_len = len; break;
            case IWN_FW_TLV_INIT_TEXT: dev->fw.init_text = data; dev->fw.init_text_len = len; break;
            case IWN_FW_TLV_INIT_DATA: dev->fw.init_data = data; dev->fw.init_data_len = len; break;
            }
            ptr = data + padded;
        }
    } else {
        /* ---- Legacy format (v2: API >= 2) ---- */
        uint32_t ver = first_word;
        dev->fw.fw_api = (ver >> 8) & 0xff;

        efi_print_ascii("  FW format: legacy v2, API ");
        print_dec(dev->fw.fw_api);
        efi_print_ascii("\r\n");

        if (dev->fw_rawsize < IWN_FW_LEGACY_HDR_SIZE) {
            efi_print_ascii("  iwn: firmware header too small\r\n");
            return -1;
        }

        /* v2 header: ver(4) + build(4) + 5 sizes(20) = 28 bytes */
        uint32_t inst_size      = read_le32(raw + 8);
        uint32_t data_size      = read_le32(raw + 12);
        uint32_t init_size      = read_le32(raw + 16);
        uint32_t init_data_size = read_le32(raw + 20);
        /* boot_size at raw + 24, unused on 5000+ */

        /* Validate sizes fit in file */
        uint32_t total = IWN_FW_LEGACY_HDR_SIZE + inst_size + data_size +
                         init_size + init_data_size;
        if (total > dev->fw_rawsize) {
            efi_print_ascii("  iwn: firmware sections exceed file size\r\n");
            return -1;
        }

        /* Sections are concatenated after header */
        uint8_t *data_ptr = raw + IWN_FW_LEGACY_HDR_SIZE;

        dev->fw.main_text     = data_ptr;
        dev->fw.main_text_len = inst_size;
        data_ptr += inst_size;

        dev->fw.main_data     = data_ptr;
        dev->fw.main_data_len = data_size;
        data_ptr += data_size;

        dev->fw.init_text     = data_ptr;
        dev->fw.init_text_len = init_size;
        data_ptr += init_size;

        dev->fw.init_data     = data_ptr;
        dev->fw.init_data_len = init_data_size;
    }

    /* Print section sizes */
    efi_print_ascii("  Init text: ");
    print_dec(dev->fw.init_text_len);
    efi_print_ascii(" bytes, Init data: ");
    print_dec(dev->fw.init_data_len);
    efi_print_ascii(" bytes\r\n");

    efi_print_ascii("  Main text: ");
    print_dec(dev->fw.main_text_len);
    efi_print_ascii(" bytes, Main data: ");
    print_dec(dev->fw.main_data_len);
    efi_print_ascii(" bytes\r\n");

    /* Validate we got the essential sections */
    if (!dev->fw.init_text || !dev->fw.main_text) {
        efi_print_ascii("  iwn: firmware missing text sections\r\n");
        return -1;
    }

    return 0;
}

/* ---- Stop all FH DMA channels ---- */

static void iwn_stop_fh_dma(iwn_dev *dev) {
    /* Stop each TX DMA channel */
    for (int chnl = 0; chnl < IWN5000_NDMACHNLS; chnl++)
        iwn_write(dev, IWN_FH_TX_CONFIG(chnl), 0);

    /* Wait for DMA channels to go idle */
    for (int i = 0; i < 200; i++) {
        uint32_t status = iwn_read(dev, IWN_FH_TX_STATUS);
        /* All 8 channels idle */
        if ((status & 0x00FF0000) == 0x00FF0000)
            return;
        gBS->Stall(10);
    }
    efi_print_ascii("  iwn: FH DMA stop timeout\r\n");
}

/* ---- Upload one firmware section via FH DMA service channel (ch 9) ---- */
/*
 * Copies section data into a DMA-accessible buffer, programs FH channel 9
 * (IWN_SRVC_DMACHNL), and waits for the transfer to complete.
 * Handles sections > 128KB by uploading in chunks.
 *
 * Reference: OpenBSD iwn5000_load_firmware_section()
 */

static int iwn_load_section(iwn_dev *dev, const uint8_t *data,
                            uint32_t len, uint32_t dst_sram) {
    /* Ensure DMA scratch buffer is allocated (reuse for all sections) */
    if (!dev->fw_dma.vaddr) {
        uint32_t alloc_size = IWN_FW_CHUNK_SIZE;
        if (len < alloc_size)
            alloc_size = ((len + 4095) / 4096) * 4096;
        /* Allocate below 4 GB */
        UINTN pages = alloc_size / 4096;
        uint64_t addr = 0xFFFFFFFF;
        EFI_STATUS s = gBS->AllocatePages(1, EfiLoaderData, pages, &addr);
        if (s != EFI_SUCCESS) {
            efi_print_ascii("  iwn: DMA alloc failed (fw_dma)\r\n");
            return -1;
        }
        dev->fw_dma.paddr = addr;
        dev->fw_dma.vaddr = (void *)(uintptr_t)addr;
        dev->fw_dma.size = (uint32_t)(pages * 4096);
    }

    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > IWN_FW_CHUNK_SIZE)
            chunk = IWN_FW_CHUNK_SIZE;

        /* Copy chunk to DMA buffer */
        iwn_memcpy(dev->fw_dma.vaddr, data + offset, chunk);

        /* Acquire NIC lock — held for entire FH programming sequence */
        if (iwn_nic_lock(dev) != 0) {
            efi_print_ascii("  iwn: nic lock timeout (fw load)\r\n");
            return -1;
        }

        /* Pause FH service DMA channel */
        iwn_write(dev, IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL), 0);

        /* Program FH service channel:
         * - SRAM destination address
         * - DMA source physical address
         * - Byte count
         * - Transfer buffer status (required for FH to recognize valid data)
         * - Start DMA
         */
        iwn_write(dev, IWN_FH_SRAM_ADDR(IWN_SRVC_DMACHNL),
                  dst_sram + offset);
        iwn_write(dev, IWN_FH_TFBD_CTRL0(IWN_SRVC_DMACHNL),
                  IWN_LOADDR(dev->fw_dma.paddr));
        iwn_write(dev, IWN_FH_TFBD_CTRL1(IWN_SRVC_DMACHNL), chunk);
        iwn_write(dev, IWN_FH_TXBUF_STATUS(IWN_SRVC_DMACHNL),
                  IWN_FH_TXBUF_STATUS_TBNUM(1) |
                  IWN_FH_TXBUF_STATUS_TBIDX(1) |
                  IWN_FH_TXBUF_STATUS_TFBD_VALID);
        iwn_write(dev, IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL),
                  IWN_FH_TX_CONFIG_DMA_ENA |
                  IWN_FH_TX_CONFIG_CIRQ_HOST_ENDTFD);

        iwn_nic_unlock(dev);

        /* Poll for service channel idle (DMA complete) */
        int done = 0;
        for (int i = 0; i < 5000; i++) {
            uint32_t status = iwn_read(dev, IWN_FH_TX_STATUS);
            if (status & IWN_FH_TX_STATUS_IDLE(IWN_SRVC_DMACHNL)) {
                done = 1;
                break;
            }
            gBS->Stall(10);  /* 10 us */
        }
        if (!done) {
            efi_print_ascii("  iwn: FH DMA timeout (section upload)\r\n");
            return -1;
        }

        offset += chunk;
    }

    return 0;
}

/* ---- Load firmware (init or runtime) and wait for UC_READY ---- */

int iwn5000_load_firmware(iwn_dev *dev, const uint8_t *text, uint32_t text_len,
                          const uint8_t *data, uint32_t data_len) {
    /* Upload text section to NIC SRAM at 0x00000000 */
    if (text_len > 0) {
        if (iwn_load_section(dev, text, text_len, IWN_FW_TEXT_BASE) != 0)
            return -1;
    }

    /* Upload data section to NIC SRAM at 0x00800000 */
    if (data_len > 0) {
        if (iwn_load_section(dev, data, data_len, IWN_FW_DATA_BASE) != 0)
            return -1;
    }

    /* Release CPU from reset — start executing firmware */
    iwn_write(dev, IWN_RESET, 0);

    return 0;
}

/* ---- Poll RX ring for a specific notification type ---- */

int iwn_poll_rx(iwn_dev *dev, uint8_t expected_type, int timeout_ms) {
    struct iwn_rx_status *stat = (struct iwn_rx_status *)dev->rxq.stat.vaddr;

    for (int t = 0; t < timeout_ms; t++) {
        /* HW write index from status page (closed_count is updated by HW) */
        uint32_t hw_idx = stat->closed_count & 0x0fff;

        while ((uint32_t)dev->rxq.cur != hw_idx) {
            /* Get the RX buffer at current index */
            void *buf = dev->rxq.buf[dev->rxq.cur].vaddr;
            struct iwn_rx_desc *desc = (struct iwn_rx_desc *)buf;

            if (desc->type == expected_type)
                return 0;

            /* Save calibration results inline */
            if (desc->type == IWN5000_CALIBRATION_RESULT) {
                if (dev->n_calib < IWN_MAX_CALIB_RESULTS) {
                    /* Calibration data follows the rx_desc header */
                    uint8_t *calib_data = (uint8_t *)buf + sizeof(struct iwn_rx_desc);
                    uint32_t calib_len = (desc->len & IWN_RX_DESC_LEN_MASK) -
                                         sizeof(struct iwn_rx_desc);
                    if (calib_len > sizeof(dev->calib[0].data))
                        calib_len = sizeof(dev->calib[0].data);
                    iwn_memcpy(dev->calib[dev->n_calib].data,
                               calib_data, calib_len);
                    dev->calib[dev->n_calib].len = calib_len;
                    dev->n_calib++;
                }
            }

            /* Advance read index */
            dev->rxq.cur = (dev->rxq.cur + 1) % IWN_RX_RING_COUNT;

            /* Tell HW we consumed this buffer — update RX write pointer */
            iwn_write(dev, IWN_FH_RX_WPTR,
                      ((dev->rxq.cur + IWN_RX_RING_COUNT - 1) % IWN_RX_RING_COUNT) & ~7);
        }

        gBS->Stall(1000);  /* 1 ms */
    }

    return -1;  /* timeout */
}

/* ---- Send a host command via TX ring 4 (command queue) ---- */

int iwn_cmd(iwn_dev *dev, uint8_t code, const void *payload, uint32_t len) {
    iwn_tx_ring *ring = &dev->txq[4];
    int idx = ring->cur;

    if (len > sizeof(((struct iwn_tx_cmd *)0)->data)) {
        efi_print_ascii("  iwn: cmd payload too large\r\n");
        return -1;
    }

    /* Build TX command */
    struct iwn_tx_cmd *cmd = (struct iwn_tx_cmd *)
        ((uint8_t *)ring->cmd.vaddr + idx * sizeof(struct iwn_tx_cmd));
    iwn_memset(cmd, 0, sizeof(*cmd));
    cmd->code = code;
    cmd->flags = 0;
    cmd->idx = (uint8_t)idx;
    cmd->qid = 4;
    if (payload && len > 0)
        iwn_memcpy(cmd->data, payload, len);

    /* Build TX descriptor — single segment pointing to command */
    struct iwn_tx_desc *desc = (struct iwn_tx_desc *)
        ((uint8_t *)ring->desc.vaddr + idx * sizeof(struct iwn_tx_desc));
    iwn_memset(desc, 0, sizeof(*desc));
    desc->nsegs = 1;
    uint64_t cmd_paddr = ring->cmd.paddr + idx * sizeof(struct iwn_tx_cmd);
    uint16_t totlen = (uint16_t)(4 + len);  /* 4-byte cmd header + payload */
    desc->segs[0].addr = IWN_LOADDR(cmd_paddr);
    desc->segs[0].len = (uint16_t)(IWN_HIADDR(cmd_paddr) | (totlen << 4));

    /* Update scheduler byte count table */
    uint16_t *sched = (uint16_t *)dev->sched.vaddr;
    /* Queue 4, slot idx: offset = qid * (IWN_TX_RING_COUNT + IWN_SCHED_WINSZ) + idx */
    int sched_idx = 4 * IWN5000_SCHED_COUNT + idx;
    uint16_t bc = (uint16_t)(4 + len + 3);  /* 4-byte cmd header + payload, 4-byte aligned */
    bc = (bc / 4) & 0x0fff;
    sched[sched_idx] = bc;
    sched[sched_idx + IWN_TX_RING_COUNT] = bc;  /* window shadow */

    /* Kick hardware: advance write pointer */
    ring->cur = (idx + 1) % IWN_TX_RING_COUNT;
    ring->queued++;
    iwn_write(dev, IWN_HBUS_TARG_WRPTR, ring->cur | (4 << 8));

    return 0;
}

/* ---- Post-alive: full scheduler setup + calibration (init firmware) ---- */
/* Reference: OpenBSD/FreeBSD iwn5000_post_alive() */

void iwn5000_post_alive(iwn_dev *dev) {
    dev->n_calib = 0;

    /* Clear RFKILL (3 times, per OpenBSD) */
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);

    /* ---- Full TX scheduler setup (must be done after firmware ALIVE) ---- */

    if (iwn_nic_lock(dev) != 0) {
        efi_print_ascii("  iwn: nic lock failed (post_alive)\r\n");
        return;
    }

    /* Read scheduler base address from firmware */
    uint32_t sched_base = iwn_prph_read(dev, IWN_SCHED_SRAM_ADDR);

    /* Clear TX scheduler context in SRAM */
    iwn_mem_set_region_4(dev, sched_base + IWN5000_SCHED_CTX_OFF, 0,
                         IWN5000_SCHED_CTX_LEN / 4);

    /* Set physical address of TX scheduler rings (1KB aligned) */
    iwn_prph_write(dev, IWN5000_SCHED_DRAM_ADDR,
                   (uint32_t)(dev->sched.paddr >> 10));

    /* Disable scheduler chain extension */
    iwn_prph_write(dev, IWN5000_SCHED_CHAINEXT_EN, 0);

    /* Enable scheduler retry */
    iwn_setbits(dev, IWN_FH_TX_CHICKEN, IWN_FH_TX_CHICKEN_SCHED_RETRY);

    /* Chain mode for all queues except command queue (queue 4) */
    iwn_prph_write(dev, IWN5000_SCHED_QCHAIN_SEL, 0xfffef);
    iwn_prph_write(dev, IWN5000_SCHED_AGGR_SEL, 0);

    /* Reset all queue pointers and set window/limit in SRAM */
    for (int qid = 0; qid < IWN5000_NTXQUEUES; qid++) {
        iwn_prph_write(dev, IWN5000_SCHED_QUEUE_RDPTR(qid), 0);
        iwn_write(dev, IWN_HBUS_TARG_WRPTR, (qid << 8) | 0);

        iwn_mem_write(dev, sched_base + IWN5000_SCHED_QUEUE_OFFSET(qid), 0);
        /* Window size and frame limit */
        iwn_mem_write(dev, sched_base + IWN5000_SCHED_QUEUE_OFFSET(qid) + 4,
                      (IWN_SCHED_LIMIT << 16) | IWN_SCHED_WINSZ);
    }

    /* Enable interrupts for all 20 queues */
    iwn_prph_write(dev, IWN5000_SCHED_INTR_MASK, 0xfffff);

    /* Identify TX FIFO rings 0-7 */
    iwn_prph_write(dev, IWN5000_SCHED_TXFACT, 0xff);

    /* Mark queues 0-6 as active with FIFO mapping */
    /* Queue-to-FIFO: { 3, 2, 1, 0, 7, 5, 6 } */
    static const uint8_t qid2fifo[] = { 3, 2, 1, 0, 7, 5, 6 };
    for (int qid = 0; qid < 7; qid++) {
        iwn_prph_write(dev, IWN5000_SCHED_QUEUE_STATUS(qid),
                       IWN5000_TXQ_STATUS_ACTIVE | qid2fifo[qid]);
    }

    iwn_nic_unlock(dev);

    /* ---- Send calibration commands ---- */

    /* Crystal calibration from EEPROM */
    struct iwn5000_phy_calib_crystal crystal_cmd;
    iwn_memset(&crystal_cmd, 0, sizeof(crystal_cmd));
    crystal_cmd.code = IWN5000_PHY_CALIB_CRYSTAL;
    crystal_cmd.ngroups = 1;
    crystal_cmd.isvalid = 1;
    crystal_cmd.cap_pin[0] = (uint8_t)(dev->eeprom_crystal & 0xff);
    crystal_cmd.cap_pin[1] = (uint8_t)((dev->eeprom_crystal >> 16) & 0xff);

    iwn_cmd(dev, IWN_CMD_PHY_CALIB, &crystal_cmd, sizeof(crystal_cmd));

    /* Query calibration — triggers firmware to run full calibration sequence */
    struct iwn5000_calib_config calib_cfg;
    iwn_memset(&calib_cfg, 0, sizeof(calib_cfg));
    calib_cfg.ucode.once.enable = 0xffffffff;
    calib_cfg.ucode.once.start  = 0xffffffff;
    calib_cfg.ucode.once.send   = 0xffffffff;
    calib_cfg.ucode.once.apply  = 0xffffffff;
    calib_cfg.ucode.flags       = 0xffffffff;

    iwn_cmd(dev, IWN5000_CMD_CALIB_CONFIG, &calib_cfg, sizeof(calib_cfg));

    /* Wait for calibration done (type 103) — up to 5 seconds */
    if (iwn_poll_rx(dev, IWN5000_CALIBRATION_DONE, 5000) != 0) {
        efi_print_ascii("  iwn: calibration timeout\r\n");
    } else {
        efi_print_ascii("  Calibration complete (");
        print_dec((uint32_t)dev->n_calib);
        efi_print_ascii(" results saved)\r\n");
    }
}

/* ---- Replay calibration results to runtime firmware ---- */

void iwn_replay_calibration(iwn_dev *dev) {
    for (int i = 0; i < dev->n_calib; i++) {
        iwn_cmd(dev, IWN_CMD_PHY_CALIB,
                dev->calib[i].data, dev->calib[i].len);
    }
}

/* ---- Program rings in hardware ---- */
/* Reference: OpenBSD iwn_init() ring programming + DMA enable sequence */

static void iwn_program_rings(iwn_dev *dev) {
    /* Keep-warm page (16-byte aligned) */
    iwn_write(dev, IWN_FH_KW_ADDR, (uint32_t)(dev->kw.paddr >> 4));

    /* TX ring descriptor base addresses */
    iwn_write(dev, IWN_FH_CBBC_QUEUE(4), (uint32_t)(dev->txq[4].desc.paddr >> 8));
    iwn_write(dev, IWN_FH_CBBC_QUEUE(0), (uint32_t)(dev->txq[0].desc.paddr >> 8));

    /* RX ring */
    iwn_write(dev, IWN_FH_RX_BASE, (uint32_t)(dev->rxq.desc.paddr >> 8));
    iwn_write(dev, IWN_FH_RX_CONFIG,
              IWN_FH_RX_CONFIG_ENA |
              IWN_FH_RX_CONFIG_NRBD(IWN_RX_RING_COUNT_LOG) |
              IWN_FH_RX_CONFIG_IRQ_DST_HOST |
              IWN_FH_RX_CONFIG_SINGLE_FRAME |
              IWN_FH_RX_CONFIG_RB_TIMEOUT(0) |
              IWN_FH_RX_CONFIG_IGN_RXF_EMPTY);
    /* Tell HW we have RX_RING_COUNT-1 buffers (rounded down to 8) */
    iwn_write(dev, IWN_FH_RX_WPTR, (IWN_RX_RING_COUNT - 1) & ~7);

    /* RX status area (16-byte aligned) */
    iwn_write(dev, IWN_FH_STATUS_WPTR, (uint32_t)(dev->rxq.stat.paddr >> 4));

    /* Enable DMA channels 0-7 (required before firmware load) */
    for (int chnl = 0; chnl < IWN5000_NDMACHNLS; chnl++) {
        iwn_write(dev, IWN_FH_TX_CONFIG(chnl),
                  IWN_FH_TX_CONFIG_DMA_ENA |
                  IWN_FH_TX_CONFIG_DMA_CREDIT_ENA);
    }

    /* Enable shadow registers (required for 6000+ series) */
    if (dev->hw_type >= IWN_HW_REV_TYPE_6000)
        iwn_setbits(dev, IWN_SHADOW_REG_CTRL, 0x800fffff);

    /* Set VMAIN power source */
    if (iwn_nic_lock(dev) == 0) {
        iwn_prph_clrbits(dev, IWN_APMG_PS, IWN_APMG_PS_PWR_SRC_MASK);
        iwn_nic_unlock(dev);
    }
}

/* ---- Full hardware init sequence (Phase 2 entry point) ---- */

int iwn_hw_init(iwn_dev *dev) {
    /* 1. APM init (power on) */
    if (iwn_apm_init(dev) != 0) {
        efi_print_ascii("  iwn: APM init failed\r\n");
        return -1;
    }

    /* 2. NIC config — write radio/antenna to HW_IF_CONFIG */
    iwn5000_nic_config(dev);

    /* 3. Allocate all DMA rings */
    if (iwn_alloc_kw(dev) != 0) return -1;
    if (iwn_alloc_sched(dev) != 0) return -1;
    if (iwn_alloc_tx_ring(dev, 4) != 0) return -1;  /* cmd queue */
    if (iwn_alloc_tx_ring(dev, 0) != 0) return -1;  /* data queue */
    if (iwn_alloc_rx_ring(dev) != 0) return -1;

    efi_print_ascii("  DMA allocated: KW, sched, TX x2, RX (");
    print_dec(IWN_RX_RING_COUNT);
    efi_print_ascii(" buffers)\r\n");

    /* 4. Stop all FH DMA channels */
    iwn_stop_fh_dma(dev);

    /* 5. Program rings in hardware */
    iwn_program_rings(dev);

    /* 6. Disable interrupts (we poll, not interrupt-driven) */
    iwn_write(dev, IWN_INT_MASK, 0);
    iwn_write(dev, IWN_INT, 0xffffffff);
    iwn_write(dev, IWN_FH_INT, 0xffffffff);

    /* 7. Parse firmware file */
    if (iwn_fw_parse(dev) != 0)
        goto fail;

    /* 8. Load INIT firmware */
    if (iwn5000_load_firmware(dev,
            dev->fw.init_text, dev->fw.init_text_len,
            dev->fw.init_data, dev->fw.init_data_len) != 0) {
        efi_print_ascii("  iwn: init firmware load failed\r\n");
        goto fail;
    }

    /* 9. Poll for UC_READY (init ucode alive) — 5 second timeout */
    if (iwn_poll_rx(dev, IWN_UC_READY, 5000) != 0) {
        /* Check for hardware/software errors */
        uint32_t intr = iwn_read(dev, IWN_INT);
        if (intr & IWN_INT_SW_ERR)
            efi_print_ascii("  iwn: SW_ERR set\r\n");
        if (intr & IWN_INT_HW_ERR)
            efi_print_ascii("  iwn: HW_ERR set\r\n");
        efi_print_ascii("  iwn: init UC_READY timeout\r\n");
        goto fail;
    }
    efi_print_ascii("  Init firmware alive!\r\n");

    /* 10. Post-alive: send calibration commands, collect results */
    iwn5000_post_alive(dev);

    /* 11. Power cycle for runtime firmware */
    iwn_apm_stop(dev);
    gBS->Stall(10000);  /* 10 ms settle */
    if (iwn_apm_init(dev) != 0) {
        efi_print_ascii("  iwn: APM re-init failed\r\n");
        goto fail;
    }
    iwn5000_nic_config(dev);

    /* 12. Stop DMA and reset rings (same buffers, reset pointers) */
    iwn_stop_fh_dma(dev);
    iwn_reset_rx_ring(dev);
    iwn_reset_tx_ring(dev, 4);
    iwn_reset_tx_ring(dev, 0);

    /* 13. Re-program rings in hardware */
    iwn_program_rings(dev);

    /* 14. Clear interrupts again */
    iwn_write(dev, IWN_INT_MASK, 0);
    iwn_write(dev, IWN_INT, 0xffffffff);
    iwn_write(dev, IWN_FH_INT, 0xffffffff);

    /* 15. Load RUNTIME firmware */
    if (iwn5000_load_firmware(dev,
            dev->fw.main_text, dev->fw.main_text_len,
            dev->fw.main_data, dev->fw.main_data_len) != 0) {
        efi_print_ascii("  iwn: runtime firmware load failed\r\n");
        goto fail;
    }

    /* 16. Poll for UC_READY (runtime ucode alive) — 5 second timeout */
    if (iwn_poll_rx(dev, IWN_UC_READY, 5000) != 0) {
        uint32_t intr = iwn_read(dev, IWN_INT);
        if (intr & IWN_INT_SW_ERR)
            efi_print_ascii("  iwn: SW_ERR set\r\n");
        if (intr & IWN_INT_HW_ERR)
            efi_print_ascii("  iwn: HW_ERR set\r\n");
        efi_print_ascii("  iwn: runtime UC_READY timeout\r\n");
        goto fail;
    }
    efi_print_ascii("  Runtime firmware alive!\r\n");

    /* 17. Full post-alive scheduler setup for runtime firmware
     * Same sequence as iwn5000_post_alive but skips calibration commands
     * and instead replays saved calibration results from init firmware.
     * Reference: OpenBSD iwn_init() after runtime UC_READY */

    /* Clear RFKILL (3 times, per OpenBSD) */
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
    iwn_write(dev, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);

    if (iwn_nic_lock(dev) == 0) {
        /* Read scheduler base from firmware */
        uint32_t rt_sched_base = iwn_prph_read(dev, IWN_SCHED_SRAM_ADDR);

        /* Clear scheduler context in SRAM */
        iwn_mem_set_region_4(dev, rt_sched_base + IWN5000_SCHED_CTX_OFF, 0,
                             IWN5000_SCHED_CTX_LEN / 4);

        /* Set physical address of TX scheduler rings */
        iwn_prph_write(dev, IWN5000_SCHED_DRAM_ADDR,
                       (uint32_t)(dev->sched.paddr >> 10));

        iwn_prph_write(dev, IWN5000_SCHED_CHAINEXT_EN, 0);
        iwn_setbits(dev, IWN_FH_TX_CHICKEN, IWN_FH_TX_CHICKEN_SCHED_RETRY);
        iwn_prph_write(dev, IWN5000_SCHED_QCHAIN_SEL, 0xfffef);
        iwn_prph_write(dev, IWN5000_SCHED_AGGR_SEL, 0);

        /* Reset all queue pointers and set window/limit */
        for (int qid = 0; qid < IWN5000_NTXQUEUES; qid++) {
            iwn_prph_write(dev, IWN5000_SCHED_QUEUE_RDPTR(qid), 0);
            iwn_write(dev, IWN_HBUS_TARG_WRPTR, (qid << 8) | 0);
            iwn_mem_write(dev, rt_sched_base + IWN5000_SCHED_QUEUE_OFFSET(qid), 0);
            iwn_mem_write(dev, rt_sched_base + IWN5000_SCHED_QUEUE_OFFSET(qid) + 4,
                          (IWN_SCHED_LIMIT << 16) | IWN_SCHED_WINSZ);
        }

        iwn_prph_write(dev, IWN5000_SCHED_INTR_MASK, 0xfffff);
        iwn_prph_write(dev, IWN5000_SCHED_TXFACT, 0xff);

        /* Mark queues 0-6 active with FIFO mapping */
        static const uint8_t rt_qid2fifo[] = { 3, 2, 1, 0, 7, 5, 6 };
        for (int qid = 0; qid < 7; qid++) {
            iwn_prph_write(dev, IWN5000_SCHED_QUEUE_STATUS(qid),
                           IWN5000_TXQ_STATUS_ACTIVE | rt_qid2fifo[qid]);
        }

        iwn_nic_unlock(dev);
    }

    /* 18. Replay saved calibration results to runtime ucode */
    iwn_replay_calibration(dev);

    dev->fw_loaded = 1;

    /* Free firmware raw buffer — no longer needed */
    if (dev->fw_rawbuf) {
        gBS->FreePool(dev->fw_rawbuf);
        dev->fw_rawbuf = 0;
    }

    /* Free DMA scratch buffer */
    if (dev->fw_dma.vaddr) {
        gBS->FreePages(dev->fw_dma.paddr, dev->fw_dma.size / 4096);
        dev->fw_dma.vaddr = 0;
    }

    return 0;

fail:
    iwn_free_dma(dev);
    if (dev->fw_rawbuf) {
        gBS->FreePool(dev->fw_rawbuf);
        dev->fw_rawbuf = 0;
    }
    return -1;
}
