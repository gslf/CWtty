/*
 * mbedtls_glue.c - What the few Mbed TLS files of the web build take from
 * its platform layer, which is not built here.
 */
#include <stddef.h>

void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--)
        *p++ = 0;
}
