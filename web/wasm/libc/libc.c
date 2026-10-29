/*
 * libc.c - The C library functions the shared sources need: memory and
 * strings, and a sort for the decoder's short arrays.
 */
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strcat(char *dst, const char *src)
{
    memcpy(dst + strlen(dst), src, strlen(src) + 1);
    return dst;
}

/* Insertion sort: the decoder sorts a few dozen doubles (elements of up to
 * 16 bytes). */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *a = base, tmp[16];
    size_t i, j;
    for (i = 1; i < n; i++) {
        memcpy(tmp, a + i * size, size);
        for (j = i; j > 0 && cmp(a + (j - 1) * size, tmp) > 0; j--)
            memcpy(a + j * size, a + (j - 1) * size, size);
        memcpy(a + j * size, tmp, size);
    }
}
