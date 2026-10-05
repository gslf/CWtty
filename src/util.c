/*
 * util.c - Small helpers shared by every module.
 */
#include "util.h"

#include <ctype.h>
#include <string.h>

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_decode(const char *str, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    if (s[0] == 0) {
        *cp = 0;
        return 0;
    }
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
        (s[0] != 0xE0 || s[1] >= 0xA0) && (s[0] != 0xED || s[1] < 0xA0) &&
        (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 &&
        (s[0] != 0xF0 || s[1] >= 0x90) && (s[0] != 0xF4 || s[1] < 0x90) && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD; /* invalid byte: replacement character */
    return 1;
}

int utf8_width(const char *s)
{
    int n = 0;
    uint32_t cp;
    while (*s) {
        s += utf8_decode(s, &cp);
        n++;
    }
    return n;
}

static uint32_t rng_state = 0x9E3779B9u;

void rng_seed(uint32_t seed)
{
    rng_state = seed ? seed : 0x9E3779B9u;
}

uint32_t rng_u32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

int rng_int(int n)
{
    if (n <= 1)
        return 0;
    return (int)(rng_u32() % (uint32_t)n);
}

void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (cap == 0)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void str_upper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}
