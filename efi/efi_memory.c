/*
 * efi_memory.c — UEFI memory manager with RAM reclaim
 *
 * At boot, scans the UEFI memory map and reclaims large Boot Services
 * regions that are likely mistyped RAM (common on machines with upgraded
 * memory, e.g. Dell E6510 reports 4.6GB as BS Code after 4→8GB upgrade).
 *
 * Provides a bump allocator for large allocations (model weights, KV cache).
 * uefi_malloc falls back to this pool when AllocatePool fails.
 *
 * On machines with correct memory maps, the pool stays empty and all
 * allocations go through the normal AllocatePool path.
 */

#include "efi.h"

/* Pool region — a contiguous chunk of reclaimed memory */
typedef struct {
    uint8_t *base;
    uint64_t size;
    uint64_t used;  /* bump pointer offset */
} ba_pool_region_t;

#define MAX_POOL_REGIONS 32
#define RECLAIM_MIN_BYTES (2 * 1024 * 1024)  /* only reclaim regions >= 2MB */
#define POOL_ALIGN 16

static ba_pool_region_t pool[MAX_POOL_REGIONS];
static int pool_count;
static uint64_t pool_total;
static uint64_t pool_used;

void ba_mem_init(void) {
    pool_count = 0;
    pool_total = 0;
    pool_used = 0;

    if (!gBS) return;

    /* Get memory map size */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_ver = 0;
    gBS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 4 * desc_size;

    void *map_buf = 0;
    if (gBS->AllocatePool(EfiLoaderData, map_size, &map_buf) != EFI_SUCCESS)
        return;

    if (gBS->GetMemoryMap(&map_size, map_buf, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS) {
        gBS->FreePool(map_buf);
        return;
    }

    /* Scan for reclaimable BS Code/Data regions.
     * Real firmware code regions are small (4KB-700KB). Large regions
     * (>= 2MB) are almost certainly mistyped RAM. We record them in
     * our pool and use the physical addresses directly. */
    uint8_t *p = (uint8_t *)map_buf;
    uint8_t *end = p + map_size;

    while (p < end && pool_count < MAX_POOL_REGIONS) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
        uint64_t region_bytes = d->NumberOfPages * 4096;

        if ((d->Type == EfiBootServicesCode || d->Type == EfiBootServicesData)
            && region_bytes >= RECLAIM_MIN_BYTES) {
            pool[pool_count].base = (uint8_t *)(uintptr_t)d->PhysicalStart;
            pool[pool_count].size = region_bytes;
            pool[pool_count].used = 0;
            pool_count++;
            pool_total += region_bytes;
        }

        p += desc_size;
    }

    gBS->FreePool(map_buf);
}

/* Allocate from reclaimed pool (bump allocator, 16-byte aligned).
 * Memory cannot be individually freed — this is a permanent allocator
 * suited for model weights, KV cache, and other long-lived buffers. */
void *ba_mem_alloc(uint64_t size) {
    if (size == 0) return 0;

    /* Round up to alignment */
    size = (size + POOL_ALIGN - 1) & ~(uint64_t)(POOL_ALIGN - 1);

    /* Find first region with enough space */
    for (int i = 0; i < pool_count; i++) {
        uint64_t avail = pool[i].size - pool[i].used;
        if (avail >= size) {
            void *ptr = pool[i].base + pool[i].used;
            pool[i].used += size;
            pool_used += size;
            return ptr;
        }
    }

    return 0;  /* no region large enough */
}

uint64_t ba_mem_pool_total(void)   { return pool_total; }
uint64_t ba_mem_pool_used(void)    { return pool_used; }
uint64_t ba_mem_pool_free(void)    { return pool_total - pool_used; }
int      ba_mem_pool_regions(void) { return pool_count; }
