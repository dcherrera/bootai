/*
 * efi_memory.c — UEFI memory backend
 *
 * Provides a simple bump allocator backed by UEFI AllocatePages().
 * For Phase 1 this is minimal — just proves we can allocate memory.
 * Later phases will wire this into JACLibc's malloc via mmap() override.
 */

#include "efi.h"

/* Simple heap state */
static uint8_t *heap_base;
static uint8_t *heap_ptr;
static uint64_t heap_size;

#define HEAP_PAGES 256  /* 1 MB initial heap (256 * 4096) */

void efi_memory_init(void) {
    if (!gBS) return;

    uint64_t addr = 0;
    EFI_STATUS status = gBS->AllocatePages(
        0,                  /* AllocateAnyPages */
        EfiLoaderData,
        HEAP_PAGES,
        &addr
    );

    if (status == EFI_SUCCESS) {
        heap_base = (uint8_t *)addr;
        heap_ptr = heap_base;
        heap_size = HEAP_PAGES * 4096ULL;
    } else {
        heap_base = 0;
        heap_ptr = 0;
        heap_size = 0;
    }
}
