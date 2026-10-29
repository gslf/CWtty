/*
 * stdlib.h - Sorting and absolute values.
 */
#ifndef CWTTY_LIBC_STDLIB_H
#define CWTTY_LIBC_STDLIB_H

#include <stddef.h>

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

static inline long long llabs(long long v)
{
    return v < 0 ? -v : v;
}

/* Only the settings file parser uses it: never linked (see stdio.h). */
long strtol(const char *s, char **end, int base);

#endif
