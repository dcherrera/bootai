/*
 * shim/math.h — Freestanding math.h for bare-metal UEFI
 *
 * DECLARATIONS ONLY. All implementations are in uefi_libc.c as global symbols.
 * This prevents static inline __builtin_* wrappers from generating external
 * PLT calls that crash in PE/COFF UEFI binaries.
 *
 * Constants and classification macros use __builtin_* since those always
 * resolve at compile time.
 */

#ifndef SHIM_MATH_H
#define SHIM_MATH_H

/* Special values — always compile-time constants */
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")

/* Classification macros — always inlined by GCC */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0, 1, 4, 3, 2, (x))

/* Constants */
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_E        2.71828182845904523536
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* ------------------------------------------------------------------ */
/* Function declarations — implementations in uefi_libc.c             */
/* ------------------------------------------------------------------ */

/* double functions */
double fabs(double x);
double sqrt(double x);
double ceil(double x);
double floor(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double exp(double x);
double exp2(double x);
double expm1(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double copysign(double x, double y);
double ldexp(double x, int e);
double frexp(double x, int *e);

/* float functions */
float fabsf(float x);
float sqrtf(float x);
float ceilf(float x);
float floorf(float x);
float roundf(float x);
float truncf(float x);
float fmodf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float copysignf(float x, float y);

#endif /* SHIM_MATH_H */
