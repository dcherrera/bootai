/*
 * iwn_dma.c — DMA memory allocation and TX/RX ring setup for Intel Wireless N
 *
 * UEFI identity-maps all memory (virt == phys), so no IOMMU mapping needed.
 * All DMA buffers are allocated below 4 GB using AllocatePages(AllocateMaxAddress).
 *
 * Reference: OpenBSD sys/dev/pci/if_iwn.c (ISC license)
 */

#include "iwn_internal.h"

/* ---- DMA allocation primitives ---- */

static int iwn_dma_alloc(iwn_dma_info *dma, uint32_t size) {
    UINTN pages = (size + 4095) / 4096;
    uint64_t addr = 0xFFFFFFFF;  /* allocate below 4 GB */

    EFI_STATUS s = gBS->AllocatePages(
        1,  /* AllocateMaxAddress */
        EfiLoaderData,
        pages,
        &addr);

    if (s != EFI_SUCCESS)
        return -1;

    dma->paddr = addr;
    dma->vaddr = (void *)(uintptr_t)addr;
    dma->size = (uint32_t)(pages * 4096);

    /* Zero-fill */
    uint8_t *p = (uint8_t *)dma->vaddr;
    for (uint32_t i = 0; i < dma->size; i++)
        p[i] = 0;

    return 0;
}

static void iwn_dma_free(iwn_dma_info *dma) {
    if (dma->vaddr) {
        gBS->FreePages(dma->paddr, dma->size / 4096);
        dma->vaddr = 0;
        dma->paddr = 0;
        dma->size = 0;
    }
}

/* ---- Keep-warm page (4 KB) ---- */

int iwn_alloc_kw(iwn_dev *dev) {
    if (iwn_dma_alloc(&dev->kw, 4096) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (kw)\r\n");
        return -1;
    }
    return 0;
}

/* ---- TX scheduler table ---- */
/*
 * 5000 series: 20 queues x (256 + 64) entries x 2 bytes = 12,800 bytes
 * We allocate IWN5000_SCHEDSZ bytes.
 */

int iwn_alloc_sched(iwn_dev *dev) {
    if (iwn_dma_alloc(&dev->sched, IWN5000_SCHEDSZ) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (sched)\r\n");
        return -1;
    }
    return 0;
}

/* ---- TX ring allocation ---- */
/*
 * Each TX ring has:
 * - 256 x 128-byte descriptors (struct iwn_tx_desc)
 * - 256 x 140-byte commands (struct iwn_tx_cmd)
 */

int iwn_alloc_tx_ring(iwn_dev *dev, int qid) {
    iwn_tx_ring *ring = &dev->txq[qid];
    ring->qid = qid;
    ring->cur = 0;
    ring->queued = 0;

    /* Descriptors: 256 x 128 = 32768 bytes */
    if (iwn_dma_alloc(&ring->desc, IWN_TX_RING_COUNT * sizeof(struct iwn_tx_desc)) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (txdesc)\r\n");
        return -1;
    }

    /* Commands: 256 x 140 = 35840 bytes */
    if (iwn_dma_alloc(&ring->cmd, IWN_TX_RING_COUNT * sizeof(struct iwn_tx_cmd)) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (txcmd)\r\n");
        iwn_dma_free(&ring->desc);
        return -1;
    }

    return 0;
}

void iwn_reset_tx_ring(iwn_dev *dev, int qid) {
    iwn_tx_ring *ring = &dev->txq[qid];
    ring->cur = 0;
    ring->queued = 0;

    /* Zero descriptors and commands */
    uint8_t *p;
    p = (uint8_t *)ring->desc.vaddr;
    for (uint32_t i = 0; i < ring->desc.size; i++) p[i] = 0;
    p = (uint8_t *)ring->cmd.vaddr;
    for (uint32_t i = 0; i < ring->cmd.size; i++) p[i] = 0;
}

/* ---- RX ring allocation ---- */
/*
 * RX ring has:
 * - 64 x uint32_t descriptor array (physical addresses of buffers, shifted >> 8)
 * - 1 status page (struct iwn_rx_status)
 * - 64 x 4 KB buffers
 */

int iwn_alloc_rx_ring(iwn_dev *dev) {
    iwn_rx_ring *ring = &dev->rxq;
    ring->cur = 0;

    /* Descriptor array: 64 x 4 = 256 bytes (allocate 1 page) */
    if (iwn_dma_alloc(&ring->desc, IWN_RX_RING_COUNT * sizeof(uint32_t)) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (rxdesc)\r\n");
        return -1;
    }

    /* Status page */
    if (iwn_dma_alloc(&ring->stat, sizeof(struct iwn_rx_status)) != 0) {
        efi_print_ascii("  iwn: DMA alloc failed (rxstat)\r\n");
        goto fail_stat;
    }

    /* 64 x 4 KB receive buffers */
    uint32_t *desc_arr = (uint32_t *)ring->desc.vaddr;
    for (int i = 0; i < IWN_RX_RING_COUNT; i++) {
        if (iwn_dma_alloc(&ring->buf[i], IWN_RBUF_SIZE) != 0) {
            efi_print_ascii("  iwn: DMA alloc failed (rxbuf)\r\n");
            /* Free already allocated buffers */
            for (int j = 0; j < i; j++)
                iwn_dma_free(&ring->buf[j]);
            goto fail_bufs;
        }
        /* Write physical address >> 8 into descriptor */
        desc_arr[i] = (uint32_t)(ring->buf[i].paddr >> 8);
    }

    return 0;

fail_bufs:
    iwn_dma_free(&ring->stat);
fail_stat:
    iwn_dma_free(&ring->desc);
    return -1;
}

void iwn_reset_rx_ring(iwn_dev *dev) {
    iwn_rx_ring *ring = &dev->rxq;
    ring->cur = 0;

    /* Zero status page */
    uint8_t *p = (uint8_t *)ring->stat.vaddr;
    for (uint32_t i = 0; i < ring->stat.size; i++) p[i] = 0;

    /* Re-zero all RX buffers */
    for (int i = 0; i < IWN_RX_RING_COUNT; i++) {
        p = (uint8_t *)ring->buf[i].vaddr;
        for (uint32_t j = 0; j < ring->buf[i].size; j++) p[j] = 0;
    }

    /* Re-write descriptor physical addresses */
    uint32_t *desc_arr = (uint32_t *)ring->desc.vaddr;
    for (int i = 0; i < IWN_RX_RING_COUNT; i++)
        desc_arr[i] = (uint32_t)(ring->buf[i].paddr >> 8);
}

/* ---- Free all DMA allocations ---- */

void iwn_free_dma(iwn_dev *dev) {
    /* Keep-warm */
    iwn_dma_free(&dev->kw);

    /* Scheduler */
    iwn_dma_free(&dev->sched);

    /* TX rings */
    for (int q = 0; q < 5; q++) {
        iwn_dma_free(&dev->txq[q].desc);
        iwn_dma_free(&dev->txq[q].cmd);
    }

    /* RX ring */
    for (int i = 0; i < IWN_RX_RING_COUNT; i++)
        iwn_dma_free(&dev->rxq.buf[i]);
    iwn_dma_free(&dev->rxq.stat);
    iwn_dma_free(&dev->rxq.desc);

    /* Firmware DMA scratch */
    iwn_dma_free(&dev->fw_dma);
}
