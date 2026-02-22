/*
 * uefi_libc.c — UEFI-backed libc replacements for bare-metal tensor operations
 *
 * Provides malloc/free/calloc/realloc via UEFI AllocatePool/FreePool.
 * Provides printf/fprintf/snprintf/sprintf with a minimal formatter.
 * Provides all math functions needed by foundry tensor.c (no __builtin_*).
 * Provides non-inline memcpy/memset/memmove/memcmp.
 *
 * This file does NOT include shim headers to avoid circular #define issues.
 * It uses only <stdint.h>, <stddef.h>, <stdarg.h> (compiler-provided) and efi.h.
 */

#include "efi.h"
#include <stdarg.h>

/* Forward declarations */
void* uefi_malloc(size_t n);
void* uefi_calloc(size_t count, size_t size);
void* uefi_realloc(void *ptr, size_t n);
void  uefi_free(void *ptr);
int   uefi_printf(const char *fmt, ...);
int   uefi_fprintf(void *stream, const char *fmt, ...);
int   uefi_snprintf(char *buf, size_t n, const char *fmt, ...);
int   uefi_sprintf(char *buf, const char *fmt, ...);

/* ================================================================= */
/* abort / exit — referenced by stdlib.h shim and assert              */
/* ================================================================= */

void abort(void) {
    efi_print_ascii("ABORT\r\n");
    for (;;) __asm__ volatile("hlt");
}

void exit(int status) {
    (void)status;
    for (;;) __asm__ volatile("hlt");
}

/* ================================================================= */
/* stdio streams — referenced by stdio.h shim                        */
/* ================================================================= */

typedef struct { int _unused; } FILE_stub;
static FILE_stub _stdin_stub, _stdout_stub, _stderr_stub;
void *stdin  = &_stdin_stub;
void *stdout = &_stdout_stub;
void *stderr = &_stderr_stub;

/* ================================================================= */
/* Freestanding libc: memcpy/memset/memmove/memcmp                   */
/* GCC may emit calls to these even with -ffreestanding.             */
/* Must be non-inline so the linker can resolve them.                */
/* ================================================================= */

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *memset(void *dest, int c, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t val = (uint8_t)c;
    for (size_t i = 0; i < n; i++) d[i] = val;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

/* ================================================================= */
/* Math: fundamental float functions                                  */
/* All implementations are standalone — no __builtin_* calls.         */
/* ================================================================= */

/* --- fabsf / fabs --- */
float fabsf(float x) { return x < 0.0f ? -x : x; }
double fabs(double x) { return x < 0.0 ? -x : x; }

/* --- sqrtf / sqrt via SSE hardware --- */
float sqrtf(float x) {
    float result;
    __asm__ ("sqrtss %1, %0" : "=x" (result) : "x" (x));
    return result;
}

double sqrt(double x) {
    double result;
    __asm__ ("sqrtsd %1, %0" : "=x" (result) : "x" (x));
    return result;
}

/* --- floorf / floor --- */
float floorf(float x) {
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

double floor(double x) {
    long long i = (long long)x;
    return (double)(x < (double)i ? i - 1 : i);
}

/* --- ceilf / ceil --- */
float ceilf(float x) {
    int i = (int)x;
    return (float)(x > (float)i ? i + 1 : i);
}

double ceil(double x) {
    long long i = (long long)x;
    return (double)(x > (double)i ? i + 1 : i);
}

/* --- roundf / round --- */
float roundf(float x) {
    return x >= 0.0f ? floorf(x + 0.5f) : ceilf(x - 0.5f);
}

double round(double x) {
    return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

/* --- truncf / trunc --- */
float truncf(float x) {
    return (float)(int)x;
}

double trunc(double x) {
    return (double)(long long)x;
}

/* --- fmodf / fmod --- */
float fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f / 0.0f; /* NaN */
    return x - truncf(x / y) * y;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0 / 0.0;
    return x - trunc(x / y) * y;
}

/* --- fmaxf / fmax / fminf / fmin --- */
float fmaxf(float x, float y) {
    if (x != x) return y; /* x is NaN */
    if (y != y) return x; /* y is NaN */
    return x > y ? x : y;
}

double fmax(double x, double y) {
    if (x != x) return y;
    if (y != y) return x;
    return x > y ? x : y;
}

float fminf(float x, float y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

double fmin(double x, double y) {
    if (x != x) return y;
    if (y != y) return x;
    return x < y ? x : y;
}

/* --- copysignf / copysign --- */
float copysignf(float x, float y) {
    union { float f; uint32_t i; } ux = {x}, uy = {y};
    ux.i = (ux.i & 0x7FFFFFFF) | (uy.i & 0x80000000);
    return ux.f;
}

double copysign(double x, double y) {
    union { double f; uint64_t i; } ux = {x}, uy = {y};
    ux.i = (ux.i & 0x7FFFFFFFFFFFFFFFULL) | (uy.i & 0x8000000000000000ULL);
    return ux.f;
}

/* --- ldexp / frexp --- */
double ldexp(double x, int e) {
    union { double f; uint64_t i; } scale;
    /* Clamp to prevent overflow of the exponent field */
    if (e > 1023) e = 1023;
    if (e < -1022) e = -1022;
    scale.i = (uint64_t)(e + 1023) << 52;
    return x * scale.f;
}

double frexp(double x, int *e) {
    if (x == 0.0) { *e = 0; return 0.0; }
    union { double f; uint64_t i; } u = {x};
    int exp_bits = (int)((u.i >> 52) & 0x7FF);
    *e = exp_bits - 1022;
    u.i = (u.i & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    return u.f;
}

/* ================================================================= */
/* Math: exponential and logarithmic functions                        */
/* ================================================================= */

/* --- expf: Cody-Waite reduction + degree-6 minimax polynomial --- */
float expf(float x) {
    if (x != x) return x;                       /* NaN */
    if (x >= 88.72f) return 1e38f * 1e38f;      /* overflow → +inf */
    if (x <= -87.33f) return 0.0f;               /* underflow → 0 */

    const float inv_ln2 = 1.4426950408889634f;
    const float ln2_hi  = 0.6931381225585938f;
    const float ln2_lo  = 9.058001005859191e-06f;

    float kf = x * inv_ln2;
    int k = (int)kf;
    if (kf < 0.0f && (float)k != kf) k--;

    float r = x - (float)k * ln2_hi - (float)k * ln2_lo;

    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
              r * (0.041666668f + r * (0.0083333310f + r * 0.0013888889f)))));

    union { float f; uint32_t i; } scale;
    scale.i = (uint32_t)(k + 127) << 23;
    return p * scale.f;
}

double exp(double x) {
    if (x != x) return x;
    if (x >= 709.78) return 1e308 * 10.0;
    if (x <= -708.39) return 0.0;

    const double inv_ln2 = 1.4426950408889634;
    const double ln2_hi  = 0.6931471805599453;
    const double ln2_lo  = 2.3190468138462996e-17;

    double kf = x * inv_ln2;
    long long k = (long long)kf;
    if (kf < 0.0 && (double)k != kf) k--;

    double r = x - (double)k * ln2_hi - (double)k * ln2_lo;

    double p = 1.0 + r * (1.0 + r * (0.5 + r * (1.0/6.0 +
              r * (1.0/24.0 + r * (1.0/120.0 + r * (1.0/720.0 +
              r * (1.0/5040.0 + r * (1.0/40320.0))))))));

    union { double f; uint64_t i; } scale;
    scale.i = (uint64_t)(k + 1023) << 52;
    return p * scale.f;
}

/* --- exp2f / exp2 --- */
float exp2f(float x) { return expf(x * 0.6931471805599453f); }
double exp2(double x) { return exp(x * 0.6931471805599453); }

/* --- expm1f / expm1 --- */
float expm1f(float x) {
    if (fabsf(x) < 1e-4f) return x + 0.5f * x * x; /* Taylor for small x */
    return expf(x) - 1.0f;
}

double expm1(double x) {
    if (fabs(x) < 1e-8) return x + 0.5 * x * x;
    return exp(x) - 1.0;
}

/* --- logf: IEEE 754 decomposition + minimax polynomial --- */
float logf(float x) {
    if (x != x) return x;
    if (x <= 0.0f) {
        if (x == 0.0f) return -1.0f / 0.0f; /* -inf */
        return 0.0f / 0.0f;                  /* NaN */
    }

    union { float f; uint32_t i; } u;
    u.f = x;
    int k = (int)((u.i >> 23) & 0xFF) - 127;
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;
    float m = u.f;

    float f = m - 1.0f;
    float p = f * (-0.5f + f * (0.33333333f + f * (-0.25f + f * 0.2f)));
    p = f + p;

    return (float)k * 0.6931471805599453f + p;
}

double log(double x) {
    if (x != x) return x;
    if (x <= 0.0) {
        if (x == 0.0) return -1.0 / 0.0;
        return 0.0 / 0.0;
    }

    union { double f; uint64_t i; } u;
    u.f = x;
    long long k = (long long)((u.i >> 52) & 0x7FF) - 1023;
    u.i = (u.i & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = u.f;

    double f = m - 1.0;
    double p = f * (-0.5 + f * (1.0/3.0 + f * (-0.25 + f * (0.2 +
              f * (-1.0/6.0 + f * (1.0/7.0))))));
    p = f + p;

    return (double)k * 0.6931471805599453 + p;
}

/* --- log2f / log2 --- */
float log2f(float x) { return logf(x) * 1.4426950408889634f; }
double log2(double x) { return log(x) * 1.4426950408889634; }

/* --- log10f / log10 --- */
float log10f(float x) { return logf(x) * 0.4342944819032518f; }
double log10(double x) { return log(x) * 0.4342944819032518; }

/* --- log1pf / log1p --- */
float log1pf(float x) {
    if (fabsf(x) < 1e-4f) return x - 0.5f * x * x; /* Taylor for small x */
    return logf(1.0f + x);
}

double log1p(double x) {
    if (fabs(x) < 1e-8) return x - 0.5 * x * x;
    return log(1.0 + x);
}

/* --- powf / pow --- */
float powf(float base, float e) {
    if (e == 0.0f) return 1.0f;
    if (base == 0.0f) return 0.0f;
    if (base == 1.0f) return 1.0f;
    if (base < 0.0f) {
        /* Integer exponent check */
        int ie = (int)e;
        if ((float)ie == e) {
            float r = expf(e * logf(-base));
            return (ie & 1) ? -r : r;
        }
        return 0.0f / 0.0f; /* NaN for non-integer exp of negative base */
    }
    return expf(e * logf(base));
}

double pow(double base, double e) {
    if (e == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;
    if (base < 0.0) {
        long long ie = (long long)e;
        if ((double)ie == e) {
            double r = exp(e * log(-base));
            return (ie & 1) ? -r : r;
        }
        return 0.0 / 0.0;
    }
    return exp(e * log(base));
}

/* ================================================================= */
/* Math: trigonometric functions                                       */
/* ================================================================= */

/* --- sinf / sin --- */
float sinf(float x) {
    const float pi = 3.14159265358979f;
    const float two_pi = 6.28318530717959f;

    x = x - floorf(x / two_pi) * two_pi;
    if (x > pi) x -= two_pi;

    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f +
           x2 * (-0.00019841270f + x2 * 2.7557319e-06f))));
}

double sin(double x) { return (double)sinf((float)x); }

/* --- cosf / cos --- */
float cosf(float x) { return sinf(x + 1.5707963267948966f); }
double cos(double x) { return (double)cosf((float)x); }

/* --- tanf / tan --- */
float tanf(float x) {
    float s = sinf(x);
    float c = cosf(x);
    if (c == 0.0f) return copysignf(1e38f, s);
    return s / c;
}

double tan(double x) { return (double)tanf((float)x); }

/* --- asinf / asin --- */
float asinf(float x) {
    if (x < -1.0f || x > 1.0f) return 0.0f / 0.0f;
    if (x == 1.0f) return 1.5707963f;
    if (x == -1.0f) return -1.5707963f;
    /* Polynomial approximation for |x| < 0.5, identity for larger */
    if (fabsf(x) <= 0.5f) {
        float x2 = x * x;
        return x * (1.0f + x2 * (0.16666667f + x2 * (0.075f +
               x2 * (0.04464286f + x2 * 0.03038194f))));
    }
    /* asin(x) = pi/2 - 2*asin(sqrt((1-x)/2)) for |x| > 0.5 */
    float sign = x < 0.0f ? -1.0f : 1.0f;
    x = fabsf(x);
    float half = (1.0f - x) * 0.5f;
    float s = sqrtf(half);
    /* Polynomial approx of asin for small argument (the sqrt result) */
    float s2 = half;
    float a = s * (1.0f + s2 * (0.16666667f + s2 * (0.075f +
              s2 * (0.04464286f + s2 * 0.03038194f))));
    return sign * (1.5707963f - 2.0f * a);
}

double asin(double x) { return (double)asinf((float)x); }

/* --- acosf / acos --- */
float acosf(float x) { return 1.5707963267948966f - asinf(x); }
double acos(double x) { return (double)acosf((float)x); }

/* --- atanf / atan --- */
float atanf(float x) {
    if (x > 1.0f) return 1.5707963f - atanf(1.0f / x);
    if (x < -1.0f) return -1.5707963f - atanf(1.0f / x);
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.3333333f + x2 * (0.2f +
           x2 * (-0.142857f + x2 * 0.111111f))));
}

double atan(double x) { return (double)atanf((float)x); }

/* --- atan2f / atan2 --- */
float atan2f(float y, float x) {
    if (x > 0.0f) return atanf(y / x);
    if (x < 0.0f && y >= 0.0f) return atanf(y / x) + 3.14159265f;
    if (x < 0.0f && y < 0.0f) return atanf(y / x) - 3.14159265f;
    if (x == 0.0f && y > 0.0f) return 1.5707963f;
    if (x == 0.0f && y < 0.0f) return -1.5707963f;
    return 0.0f; /* x == 0 && y == 0 */
}

double atan2(double y, double x) { return (double)atan2f((float)y, (float)x); }

/* ================================================================= */
/* Math: hyperbolic functions                                         */
/* ================================================================= */

float sinhf(float x) {
    if (fabsf(x) < 1e-4f) return x; /* Taylor for small x */
    float ex = expf(x);
    return (ex - 1.0f / ex) * 0.5f;
}

float coshf(float x) {
    float ex = expf(fabsf(x));
    return (ex + 1.0f / ex) * 0.5f;
}

float tanhf(float x) {
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return -1.0f;
    float e2x = expf(2.0f * x);
    return (e2x - 1.0f) / (e2x + 1.0f);
}

double sinh(double x) { return (double)sinhf((float)x); }
double cosh(double x) { return (double)coshf((float)x); }
double tanh(double x) { return (double)tanhf((float)x); }

/* ================================================================= */
/* Memory: AllocatePool / FreePool wrappers                          */
/* ================================================================= */

typedef struct {
    void    *raw;     /* original AllocatePool pointer (for FreePool) */
    uint64_t size;    /* user-requested size */
    uint64_t magic;   /* corruption guard */
} alloc_header_t;     /* 24 bytes on x86_64 */

#define ALLOC_MAGIC 0xBABECAFE00C0FFEEULL

void* uefi_malloc(size_t n) {
    if (!gBS || n == 0) return (void *)0;

    size_t total = n + sizeof(alloc_header_t) + 16;
    void *raw = (void *)0;
    EFI_STATUS status = gBS->AllocatePool(EfiLoaderData, total, &raw);
    if (status != EFI_SUCCESS || !raw) return (void *)0;

    uintptr_t user = ((uintptr_t)raw + sizeof(alloc_header_t) + 15) & ~(uintptr_t)15;

    alloc_header_t *hdr = ((alloc_header_t *)user) - 1;
    hdr->raw = raw;
    hdr->size = n;
    hdr->magic = ALLOC_MAGIC;

    return (void *)user;
}

void* uefi_calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (total == 0) return (void *)0;

    void *ptr = uefi_malloc(total);
    if (!ptr) return (void *)0;

    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < total; i++) p[i] = 0;
    return ptr;
}

void* uefi_realloc(void *ptr, size_t n) {
    if (!ptr) return uefi_malloc(n);
    if (n == 0) { uefi_free(ptr); return (void *)0; }

    alloc_header_t *hdr = ((alloc_header_t *)ptr) - 1;
    if (hdr->magic != ALLOC_MAGIC) return (void *)0;

    size_t old_size = hdr->size;

    void *new_ptr = uefi_malloc(n);
    if (!new_ptr) return (void *)0;

    size_t copy_size = old_size < n ? old_size : n;
    uint8_t *dst = (uint8_t *)new_ptr;
    uint8_t *src = (uint8_t *)ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    uefi_free(ptr);
    return new_ptr;
}

void uefi_free(void *ptr) {
    if (!ptr || !gBS) return;

    alloc_header_t *hdr = ((alloc_header_t *)ptr) - 1;
    if (hdr->magic != ALLOC_MAGIC) return;
    hdr->magic = 0;
    gBS->FreePool(hdr->raw);
}

/* ================================================================= */
/* Minimal printf formatter                                          */
/* ================================================================= */

static int fmt_putc(char *buf, size_t buf_size, size_t *pos, char c) {
    if (buf && *pos < buf_size - 1) {
        buf[*pos] = c;
    }
    (*pos)++;
    return 1;
}

static int fmt_puts(char *buf, size_t buf_size, size_t *pos, const char *s) {
    int count = 0;
    while (*s) {
        fmt_putc(buf, buf_size, pos, *s++);
        count++;
    }
    return count;
}

static void fmt_pad(char *buf, size_t buf_size, size_t *pos, char c, int count) {
    for (int i = 0; i < count; i++) {
        fmt_putc(buf, buf_size, pos, c);
    }
}

static void reverse_str(char *s, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
        i++; j--;
    }
}

static int fmt_uint(char *tmp, uint64_t val, int base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }
    reverse_str(tmp, len);
    tmp[len] = '\0';
    return len;
}

static int fmt_int(char *tmp, int64_t val, int base) {
    int neg = 0;
    uint64_t uval;
    if (val < 0) {
        neg = 1;
        uval = (uint64_t)(-(val + 1)) + 1;
    } else {
        uval = (uint64_t)val;
    }
    int len = fmt_uint(tmp + neg, uval, base, 0);
    if (neg) {
        tmp[0] = '-';
        len++;
    }
    return len;
}

static int fmt_double(char *tmp, double val, int precision, char spec) {
    int len = 0;

    if (val != val) { tmp[0]='n'; tmp[1]='a'; tmp[2]='n'; tmp[3]='\0'; return 3; }
    if (val > 1e308) { tmp[0]='i'; tmp[1]='n'; tmp[2]='f'; tmp[3]='\0'; return 3; }
    if (val < -1e308) { tmp[0]='-'; tmp[1]='i'; tmp[2]='n'; tmp[3]='f'; tmp[4]='\0'; return 4; }

    if (precision < 0) precision = 6;

    if (spec == 'g' || spec == 'G') {
        double abs_val = val < 0 ? -val : val;
        if (abs_val >= 1e-4 && abs_val < 1e6) spec = 'f';
        else spec = (spec == 'G') ? 'E' : 'e';
    }

    if (spec == 'e' || spec == 'E') {
        int neg = 0;
        if (val < 0) { neg = 1; val = -val; }
        int exponent = 0;
        if (val > 0) {
            while (val >= 10.0) { val /= 10.0; exponent++; }
            while (val < 1.0) { val *= 10.0; exponent--; }
        }
        if (neg) tmp[len++] = '-';
        int int_part = (int)val;
        tmp[len++] = '0' + int_part;
        if (precision > 0) {
            tmp[len++] = '.';
            double frac = val - (double)int_part;
            for (int i = 0; i < precision; i++) {
                frac *= 10.0; int d = (int)frac;
                if (d > 9) d = 9; if (d < 0) d = 0;
                tmp[len++] = '0' + d; frac -= (double)d;
            }
        }
        tmp[len++] = (spec == 'E') ? 'E' : 'e';
        tmp[len++] = exponent >= 0 ? '+' : '-';
        if (exponent < 0) exponent = -exponent;
        if (exponent < 10) { tmp[len++] = '0'; tmp[len++] = '0' + exponent; }
        else if (exponent < 100) { tmp[len++] = '0' + exponent/10; tmp[len++] = '0' + exponent%10; }
        else { tmp[len++] = '0' + exponent/100; tmp[len++] = '0' + (exponent/10)%10; tmp[len++] = '0' + exponent%10; }
        tmp[len] = '\0';
        return len;
    }

    /* %f */
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (neg) tmp[len++] = '-';

    uint64_t int_part = (uint64_t)val;
    double frac = val - (double)int_part;
    double round_add = 0.5;
    for (int i = 0; i < precision; i++) round_add /= 10.0;
    frac += round_add;
    if (frac >= 1.0) { int_part++; frac -= 1.0; }

    char int_buf[24];
    int int_len = fmt_uint(int_buf, int_part, 10, 0);
    for (int i = 0; i < int_len; i++) tmp[len++] = int_buf[i];

    if (precision > 0) {
        tmp[len++] = '.';
        for (int i = 0; i < precision; i++) {
            frac *= 10.0; int d = (int)frac;
            if (d > 9) d = 9; if (d < 0) d = 0;
            tmp[len++] = '0' + d; frac -= (double)d;
        }
    }

    tmp[len] = '\0';
    return len;
}

static int uefi_vsnprintf(char *buf, size_t buf_size, const char *fmt, va_list ap) {
    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') { fmt_putc(buf, buf_size, &pos, *fmt++); continue; }
        fmt++;

        int left_align = 0, zero_pad = 0, plus_sign = 0, space_sign = 0, hash_flag = 0;
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else if (*fmt == '#') { hash_flag = 1; fmt++; }
            else break;
        }
        (void)plus_sign; (void)space_sign; (void)hash_flag;

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else { while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); fmt++; } }

        int precision = -1;
        if (*fmt == '.') {
            fmt++; precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; }
            else { while (*fmt >= '0' && *fmt <= '9') { precision = precision*10 + (*fmt-'0'); fmt++; } }
        }

        int length = 0;
        if (*fmt == 'l') { fmt++; if (*fmt == 'l') { length = 2; fmt++; } else length = 1; }
        else if (*fmt == 'z') { length = 3; fmt++; }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') { length = 5; fmt++; } else length = 4; }

        char tmp[80]; int slen = 0; const char *str_arg;

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t val;
            if (length == 2) val = va_arg(ap, long long);
            else if (length == 1) val = va_arg(ap, long);
            else if (length == 3) val = (int64_t)va_arg(ap, size_t);
            else val = va_arg(ap, int);
            slen = fmt_int(tmp, val, 10); break;
        }
        case 'u': {
            uint64_t val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else if (length == 3) val = (uint64_t)va_arg(ap, size_t);
            else val = va_arg(ap, unsigned int);
            slen = fmt_uint(tmp, val, 10, 0); break;
        }
        case 'x': case 'X': {
            uint64_t val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else if (length == 3) val = (uint64_t)va_arg(ap, size_t);
            else val = va_arg(ap, unsigned int);
            slen = fmt_uint(tmp, val, 16, *fmt == 'X'); break;
        }
        case 'o': {
            uint64_t val;
            if (length == 2) val = va_arg(ap, unsigned long long);
            else if (length == 1) val = va_arg(ap, unsigned long);
            else val = va_arg(ap, unsigned int);
            slen = fmt_uint(tmp, val, 8, 0); break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double val = va_arg(ap, double);
            slen = fmt_double(tmp, val, precision, *fmt);
            precision = -1; break;
        }
        case 's': {
            str_arg = va_arg(ap, const char *);
            if (!str_arg) str_arg = "(null)";
            slen = 0;
            while (str_arg[slen] && (precision < 0 || slen < precision)) slen++;
            if (!left_align && width > slen) fmt_pad(buf, buf_size, &pos, ' ', width - slen);
            for (int i = 0; i < slen; i++) fmt_putc(buf, buf_size, &pos, str_arg[i]);
            if (left_align && width > slen) fmt_pad(buf, buf_size, &pos, ' ', width - slen);
            fmt++; continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left_align && width > 1) fmt_pad(buf, buf_size, &pos, ' ', width - 1);
            fmt_putc(buf, buf_size, &pos, c);
            if (left_align && width > 1) fmt_pad(buf, buf_size, &pos, ' ', width - 1);
            fmt++; continue;
        }
        case 'p': {
            uint64_t val = (uint64_t)(uintptr_t)va_arg(ap, void *);
            fmt_puts(buf, buf_size, &pos, "0x");
            slen = fmt_uint(tmp, val, 16, 0); break;
        }
        case '%': fmt_putc(buf, buf_size, &pos, '%'); fmt++; continue;
        case 'n': { int *n_ptr = va_arg(ap, int *); if (n_ptr) *n_ptr = (int)pos; fmt++; continue; }
        default: fmt_putc(buf, buf_size, &pos, '%'); fmt_putc(buf, buf_size, &pos, *fmt); fmt++; continue;
        }

        char pad_char = (zero_pad && !left_align) ? '0' : ' ';
        if (!left_align && width > slen) fmt_pad(buf, buf_size, &pos, pad_char, width - slen);
        for (int i = 0; i < slen; i++) fmt_putc(buf, buf_size, &pos, tmp[i]);
        if (left_align && width > slen) fmt_pad(buf, buf_size, &pos, ' ', width - slen);
        fmt++;
    }

    if (buf) {
        if (pos < buf_size) buf[pos] = '\0';
        else if (buf_size > 0) buf[buf_size - 1] = '\0';
    }
    return (int)pos;
}

/* ================================================================= */
/* Public printf functions                                           */
/* ================================================================= */

int uefi_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = uefi_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    efi_print_ascii(buf);
    return n;
}

int uefi_fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = uefi_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    efi_print_ascii(buf);
    return n;
}

int uefi_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = uefi_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

int uefi_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = uefi_vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}
