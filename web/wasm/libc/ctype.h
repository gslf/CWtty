/*
 * ctype.h - ASCII character classes.
 */
#ifndef CWTTY_LIBC_CTYPE_H
#define CWTTY_LIBC_CTYPE_H

static inline int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static inline int toupper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static inline int tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

#endif
