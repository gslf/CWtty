/*
 * math.h - Square root and absolute value are WebAssembly instructions,
 * the transcendental functions are imported from JavaScript's Math.
 */
#ifndef CWTTY_LIBC_MATH_H
#define CWTTY_LIBC_MATH_H

#define MATH_IMPORT(name) __attribute__((import_module("math"), import_name(#name)))

MATH_IMPORT(sin) double sin(double x);
MATH_IMPORT(cos) double cos(double x);
MATH_IMPORT(exp) double exp(double x);
MATH_IMPORT(log) double log(double x);
MATH_IMPORT(log10) double log10(double x);
MATH_IMPORT(pow) double pow(double x, double y);

static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }

static inline float sqrtf(float x) { return __builtin_sqrtf(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline float sinf(float x) { return (float)sin(x); }
static inline float cosf(float x) { return (float)cos(x); }
static inline float expf(float x) { return (float)exp(x); }
static inline float log10f(float x) { return (float)log10(x); }
static inline float powf(float x, float y) { return (float)pow(x, y); }

#endif
