/*
 * shim/stdlib.h — Freestanding stdlib.h for bare-metal UEFI
 *
 * malloc/calloc/realloc/free are redirected to UEFI-backed implementations
 * via #define in uefi_libc.h. This header provides type declarations and
 * utility functions (abs, strtol, qsort, bsearch).
 */

#ifndef SHIM_STDLIB_H
#define SHIM_STDLIB_H

#include <stddef.h>
#include <stdint.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

/* Memory — redirected by uefi_libc.h #defines */
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t n);
void  free(void *ptr);

/* Process — stub on bare metal */
void abort(void);
void exit(int status);

/* Simple PRNG — adequate for non-crypto uses */
static unsigned int __shim_rand_seed = 1;
static inline void srand(unsigned int seed) { __shim_rand_seed = seed; }
static inline int rand(void) {
    __shim_rand_seed = __shim_rand_seed * 1103515245 + 12345;
    return (int)((__shim_rand_seed >> 16) & RAND_MAX);
}

static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }

static inline int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return neg ? -n : n;
}

static inline long strtol(const char *s, char **endp, int base) {
    long n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        n = n * base + d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return neg ? -n : n;
}

typedef int (*__compar_fn_t)(const void *, const void *);

static inline void qsort(void *base, size_t nmemb, size_t size, __compar_fn_t compar) {
    /* Simple insertion sort — adequate for small arrays in inference */
    char *arr = (char *)base;
    char tmp[256]; /* max element size */
    if (size > sizeof(tmp)) return;
    for (size_t i = 1; i < nmemb; i++) {
        __builtin_memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, tmp) > 0) {
            __builtin_memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        }
        __builtin_memcpy(arr + j * size, tmp, size);
    }
}

#endif /* SHIM_STDLIB_H */
