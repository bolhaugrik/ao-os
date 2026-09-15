/* AO-OS: matematikai fuggvenyek deklaracioi; a torzsuk a MicroPython lib/libm_dbl (musl) forrasaibol jon,
 * a fabs es a nan a port/libc_ao.c-ben van (fordito-beepitett). */
#pragma once

typedef float float_t;
typedef double double_t;
#define FLT_EVAL_METHOD 0

#define M_E        2.7182818284590452354
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_LN2      0.69314718055994530942
#define M_SQRT2    1.41421356237309504880

#define NAN        __builtin_nan("")
#define INFINITY   __builtin_inf()
#define HUGE_VAL   __builtin_huge_val()
#define DBL_MAX    __DBL_MAX__
#define DBL_MIN    __DBL_MIN__

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)

double acos(double);
double acosh(double);
double asin(double);
double asinh(double);
double atan(double);
double atan2(double, double);
double atanh(double);
double ceil(double);
double copysign(double, double);
double cos(double);
double cosh(double);
double erf(double);
double erfc(double);
double exp(double);
double expm1(double);
double fabs(double);
double floor(double);
double fmod(double, double);
double frexp(double, int *);
double ldexp(double, int);
double lgamma(double);
double log(double);
double log10(double);
double log1p(double);
double log2(double);
double modf(double, double *);
double nan(const char *);
double nearbyint(double);
double pow(double, double);
double rint(double);
double round(double);
double scalbn(double, int);
double sin(double);
double sinh(double);
double sqrt(double);
double tan(double);
double tanh(double);
double tgamma(double);
double trunc(double);
