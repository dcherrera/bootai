/*
 * uefi_libc.h — Shim header redirecting libc calls to UEFI-backed implementations
 *
 * Force-included (-include efi/uefi_libc.h) before everything else when
 * compiling foundry sources for bare-metal UEFI. Redirects malloc/free/printf
 * to our implementations in uefi_libc.c that use UEFI Boot Services.
 *
 * Math and string functions are provided by shim/ headers (declarations only)
 * with implementations in uefi_libc.c.
 */

#ifndef BOOTAI_UEFI_LIBC_H
#define BOOTAI_UEFI_LIBC_H

#include <stddef.h>
#include <stdarg.h>

/* Redirect libc memory functions to UEFI-backed implementations */
#define malloc   uefi_malloc
#define calloc   uefi_calloc
#define realloc  uefi_realloc
#define free     uefi_free

/* Redirect libc I/O functions to UEFI ConOut */
#define printf   uefi_printf
#define fprintf  uefi_fprintf
#define snprintf uefi_snprintf
#define sprintf  uefi_sprintf

/* Function declarations */
void* uefi_malloc(size_t n);
void* uefi_calloc(size_t count, size_t size);
void* uefi_realloc(void *ptr, size_t n);
void  uefi_free(void *ptr);

int uefi_printf(const char *fmt, ...);
int uefi_fprintf(void *stream, const char *fmt, ...);
int uefi_snprintf(char *buf, size_t n, const char *fmt, ...);
int uefi_sprintf(char *buf, const char *fmt, ...);

#endif /* BOOTAI_UEFI_LIBC_H */
