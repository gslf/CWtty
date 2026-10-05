/*
 * util.h - Small helpers shared by every module: clamping, UTF-8, RNG and
 * string utilities.
 */
#ifndef CWTTY_UTIL_H
#define CWTTY_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define CW_PI 3.14159265358979323846
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int mini(int a, int b) { return a < b ? a : b; }
static inline int maxi(int a, int b) { return a > b ? a : b; }

/* Encodes a codepoint as UTF-8. Returns the number of bytes written (1..4). */
int utf8_encode(uint32_t cp, char out[4]);

/* Decodes one codepoint. Returns bytes consumed (>= 1 unless *s == 0). */
int utf8_decode(const char *s, uint32_t *cp);

/* Number of codepoints (== terminal columns for the glyphs we use). */
int utf8_width(const char *s);

/* Small, fast, seedable PRNG (xorshift32). */
void     rng_seed(uint32_t seed);
uint32_t rng_u32(void);
int      rng_int(int n); /* uniform in [0, n) */

/* Bounded string copy that always NUL-terminates. */
void str_copy(char *dst, size_t cap, const char *src);

/* Upper-cases ASCII letters in place. */
void str_upper(char *s);

/* Case-insensitive ASCII comparison. */
int str_ieq(const char *a, const char *b);

#endif
