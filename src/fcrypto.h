/*
 * fcrypto.h - The cryptography of the Filen client (filen.c): hashes,
 * HMAC, PBKDF2, HKDF, AES-256-GCM, base64 and hex on Mbed TLS, Argon2id
 * on Monocypher, and a random generator.
 *
 * The same code in the terminal app and in the web app (WebAssembly, where
 * the browser only moves the bytes), so both produce the same requests.
 * No allocation: Argon2's work area comes from the caller.
 */
#ifndef CWTTY_FCRYPTO_H
#define CWTTY_FCRYPTO_H

#include <stddef.h>
#include <stdint.h>

/* The random generator: ChaCha20 with fast key erasure, seeded by the
 * platform (the operating system, or crypto.getRandomValues() in the
 * browser). fc_random() fails until it is seeded. More seeding only adds. */
void fc_seed(const void *seed, size_t n);
int  fc_seeded(void);
int  fc_random(void *out, size_t n); /* 0, or -1 if not seeded */

void fc_sha1(const void *in, size_t n, uint8_t out[20]);
void fc_sha256(const void *in, size_t n, uint8_t out[32]);
void fc_sha512(const void *in, size_t n, uint8_t out[64]);

void fc_hmac_sha256(const void *key, size_t klen, const void *in, size_t n, uint8_t out[32]);
void fc_hmac_sha512(const void *key, size_t klen, const void *in, size_t n, uint8_t out[64]);

void fc_pbkdf2_sha512(const void *pw, size_t pwlen, const void *salt, size_t slen, unsigned iterations,
                      uint8_t *out, size_t outlen);
/* RFC 5869, outlen <= 255 * 32. */
void fc_hkdf_sha256(const void *ikm, size_t ikmlen, const void *salt, size_t slen, const void *info,
                    size_t infolen, uint8_t *out, size_t outlen);

/* AES-256-GCM, 16-byte tag. Decryption returns -1 when the tag is wrong
 * (out is then cleared). */
int fc_gcm_encrypt(const uint8_t key[32], const void *iv, size_t ivlen, const void *in, size_t n, uint8_t *out,
                   uint8_t tag[16]);
int fc_gcm_decrypt(const uint8_t key[32], const void *iv, size_t ivlen, const void *in, size_t n,
                   const uint8_t tag[16], uint8_t *out);

/* Standard base64 with padding: the length written (NUL-terminated for
 * encode), or -1 if it does not fit or is not base64. */
int fc_base64_encode(const void *in, size_t n, char *out, size_t cap);
int fc_base64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

/* Lower-case hex, NUL-terminated (out holds 2n + 1). */
void fc_hex(const void *in, size_t n, char *out);
/* Hex digits (either case) to bytes: the count, or -1. */
int fc_unhex(const char *in, size_t n, uint8_t *out, size_t cap);

/* Argon2id, version 0x13. work: m_kib KiB, 8-byte aligned. */
#define FC_ARGON2_WORK(m_kib) ((size_t)(m_kib) * 1024)
void fc_argon2id(const void *pw, size_t pwlen, const void *salt, size_t slen, unsigned t, unsigned m_kib,
                 unsigned lanes, uint8_t *out, size_t outlen, void *work);

/* Wipes secrets. */
void fc_wipe(void *p, size_t n);

#endif
