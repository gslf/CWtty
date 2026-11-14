/*
 * cwtty_mbedtls_wasm_config.h - Mbed TLS in the web app: only the hashes,
 * AES-GCM and base64 that the Filen client (src/fcrypto.c) uses. No TLS
 * (the browser does it), no allocation, no platform layer.
 */
#ifndef CWTTY_MBEDTLS_WASM_CONFIG_H
#define CWTTY_MBEDTLS_WASM_CONFIG_H

#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_BLOCK_CIPHER_C /* GCM without the generic cipher layer */

#define MBEDTLS_BASE64_C

#endif
