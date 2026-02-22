/*
 * shim/stdio.h — Freestanding stdio.h for bare-metal UEFI
 *
 * printf/fprintf/snprintf/sprintf are redirected to UEFI ConOut via
 * #define in uefi_libc.h. This header provides the declarations and
 * NULL/EOF/FILE stubs.
 */

#ifndef SHIM_STDIO_H
#define SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EOF (-1)

/* Opaque FILE type — all I/O goes to UEFI ConOut */
typedef struct { int _unused; } FILE;

/* Standard streams — all route to ConOut via uefi_fprintf */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* printf family — may be redirected by uefi_libc.h #defines */
#ifndef printf
int printf(const char *fmt, ...);
#endif
#ifndef fprintf
int fprintf(FILE *stream, const char *fmt, ...);
#endif
#ifndef snprintf
int snprintf(char *buf, size_t n, const char *fmt, ...);
#endif
#ifndef sprintf
int sprintf(char *buf, const char *fmt, ...);
#endif

/* Stubs for any remaining stdio usage */
static inline int fflush(FILE *stream) { (void)stream; return 0; }
static inline int fputc(int c, FILE *stream) { (void)c; (void)stream; return c; }
static inline int fputs(const char *s, FILE *stream) { (void)s; (void)stream; return 0; }

#endif /* SHIM_STDIO_H */
