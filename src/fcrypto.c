/*
 * fcrypto.c - The cryptography of the Filen client.
 */
#include "fcrypto.h"

#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "monocypher.h"

void fc_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}

/* ------------------------------------------------------------------------ */
/* Random                                                                    */
/* ------------------------------------------------------------------------ */

static uint8_t  rng_key[32];
static uint64_t rng_count;
static int      rng_ready;

void fc_seed(const void *seed, size_t n)
{
    uint8_t mixed[32];
    crypto_blake2b_keyed(mixed, sizeof mixed, rng_key, sizeof rng_key, (const uint8_t *)seed, n);
    memcpy(rng_key, mixed, sizeof rng_key);
    crypto_wipe(mixed, sizeof mixed);
    rng_ready = n >= 16 || rng_ready;
}

int fc_seeded(void)
{
    return rng_ready;
}

int fc_random(void *out, size_t n)
{
    uint8_t nonce[8];
    int i;
    if (!rng_ready)
        return -1;
    for (i = 0; i < 8; i++)
        nonce[i] = (uint8_t)(rng_count >> (8 * i));
    rng_count++;
    /* Block 0 becomes the next key, the output starts at block 1. */
    if (n)
        crypto_chacha20_djb((uint8_t *)out, NULL, n, rng_key, nonce, 1);
    crypto_chacha20_djb(rng_key, NULL, sizeof rng_key, rng_key, nonce, 0);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Hashes and HMAC                                                           */
/* ------------------------------------------------------------------------ */

void fc_sha1(const void *in, size_t n, uint8_t out[20])
{
    mbedtls_sha1((const unsigned char *)in, n, out);
}

void fc_sha256(const void *in, size_t n, uint8_t out[32])
{
    mbedtls_sha256((const unsigned char *)in, n, out, 0);
}

void fc_sha512(const void *in, size_t n, uint8_t out[64])
{
    mbedtls_sha512((const unsigned char *)in, n, out, 0);
}

/* HMAC with the inner and outer states kept after their pad blocks: each
 * message then costs two finishes (PBKDF2 runs 200000 of them). */
typedef struct {
    int big; /* SHA-512, else SHA-256 */
    union {
        mbedtls_sha256_context s;
        mbedtls_sha512_context b;
    } in, out;
} hmac_t;

static void h_start(int big, void *c)
{
    if (big)
        mbedtls_sha512_starts((mbedtls_sha512_context *)c, 0);
    else
        mbedtls_sha256_starts((mbedtls_sha256_context *)c, 0);
}

static void h_update(int big, void *c, const void *p, size_t n)
{
    if (big)
        mbedtls_sha512_update((mbedtls_sha512_context *)c, (const unsigned char *)p, n);
    else
        mbedtls_sha256_update((mbedtls_sha256_context *)c, (const unsigned char *)p, n);
}

static void h_finish(int big, void *c, uint8_t *out)
{
    if (big)
        mbedtls_sha512_finish((mbedtls_sha512_context *)c, out);
    else
        mbedtls_sha256_finish((mbedtls_sha256_context *)c, out);
}

static void hmac_init(hmac_t *h, int big, const void *key, size_t klen)
{
    uint8_t pad[128], kh[64];
    size_t block = big ? 128 : 64, i;
    memset(h, 0, sizeof *h);
    h->big = big;
    if (klen > block) {
        if (big)
            fc_sha512(key, klen, kh);
        else
            fc_sha256(key, klen, kh);
        key = kh;
        klen = big ? 64 : 32;
    }
    memset(pad, 0, sizeof pad);
    memcpy(pad, key, klen);
    for (i = 0; i < block; i++)
        pad[i] ^= 0x36;
    h_start(big, &h->in);
    h_update(big, &h->in, pad, block);
    for (i = 0; i < block; i++)
        pad[i] ^= 0x36 ^ 0x5c;
    h_start(big, &h->out);
    h_update(big, &h->out, pad, block);
    crypto_wipe(pad, sizeof pad);
    crypto_wipe(kh, sizeof kh);
}

/* HMAC of a || b. */
static void hmac_run(const hmac_t *h, const void *a, size_t an, const void *b, size_t bn, uint8_t *out)
{
    hmac_t c = *h;
    uint8_t inner[64];
    size_t dlen = h->big ? 64 : 32;
    h_update(c.big, &c.in, a, an);
    if (bn)
        h_update(c.big, &c.in, b, bn);
    h_finish(c.big, &c.in, inner);
    h_update(c.big, &c.out, inner, dlen);
    h_finish(c.big, &c.out, out);
    crypto_wipe(&c, sizeof c);
    crypto_wipe(inner, sizeof inner);
}

void fc_hmac_sha256(const void *key, size_t klen, const void *in, size_t n, uint8_t out[32])
{
    hmac_t h;
    hmac_init(&h, 0, key, klen);
    hmac_run(&h, in, n, NULL, 0, out);
    crypto_wipe(&h, sizeof h);
}

void fc_hmac_sha512(const void *key, size_t klen, const void *in, size_t n, uint8_t out[64])
{
    hmac_t h;
    hmac_init(&h, 1, key, klen);
    hmac_run(&h, in, n, NULL, 0, out);
    crypto_wipe(&h, sizeof h);
}

void fc_pbkdf2_sha512(const void *pw, size_t pwlen, const void *salt, size_t slen, unsigned iterations,
                      uint8_t *out, size_t outlen)
{
    hmac_t h;
    uint8_t u[64], t[64], idx[4];
    uint32_t block = 1;
    unsigned it;
    size_t k, take;
    hmac_init(&h, 1, pw, pwlen);
    while (outlen > 0) {
        idx[0] = (uint8_t)(block >> 24);
        idx[1] = (uint8_t)(block >> 16);
        idx[2] = (uint8_t)(block >> 8);
        idx[3] = (uint8_t)block;
        hmac_run(&h, salt, slen, idx, 4, u);
        memcpy(t, u, sizeof t);
        for (it = 1; it < iterations; it++) {
            hmac_run(&h, u, sizeof u, NULL, 0, u);
            for (k = 0; k < sizeof t; k++)
                t[k] ^= u[k];
        }
        take = outlen < sizeof t ? outlen : sizeof t;
        memcpy(out, t, take);
        out += take;
        outlen -= take;
        block++;
    }
    crypto_wipe(&h, sizeof h);
    crypto_wipe(u, sizeof u);
    crypto_wipe(t, sizeof t);
}

void fc_hkdf_sha256(const void *ikm, size_t ikmlen, const void *salt, size_t slen, const void *info,
                    size_t infolen, uint8_t *out, size_t outlen)
{
    static const uint8_t zeros[32];
    uint8_t prk[32], t[32 + 1], prev[32];
    hmac_t h;
    size_t have = 0, take;
    uint8_t i = 1;
    if (slen == 0) {
        salt = zeros;
        slen = sizeof zeros;
    }
    fc_hmac_sha256(salt, slen, ikm, ikmlen, prk);
    hmac_init(&h, 0, prk, sizeof prk);
    while (outlen > 0) {
        /* T(i) = HMAC(PRK, T(i-1) || info || i) */
        hmac_t c = h;
        h_update(0, &c.in, prev, have);
        h_update(0, &c.in, info, infolen);
        t[0] = i++;
        h_update(0, &c.in, t, 1);
        {
            uint8_t inner[32];
            h_finish(0, &c.in, inner);
            h_update(0, &c.out, inner, 32);
            h_finish(0, &c.out, prev);
            crypto_wipe(inner, sizeof inner);
        }
        crypto_wipe(&c, sizeof c);
        have = 32;
        take = outlen < 32 ? outlen : 32;
        memcpy(out, prev, take);
        out += take;
        outlen -= take;
    }
    crypto_wipe(&h, sizeof h);
    crypto_wipe(prk, sizeof prk);
    crypto_wipe(prev, sizeof prev);
}

/* ------------------------------------------------------------------------ */
/* AES-256-GCM                                                               */
/* ------------------------------------------------------------------------ */

int fc_gcm_encrypt(const uint8_t key[32], const void *iv, size_t ivlen, const void *in, size_t n, uint8_t *out,
                   uint8_t tag[16])
{
    mbedtls_gcm_context g;
    int rc;
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, (const unsigned char *)iv, ivlen, NULL, 0,
                                       (const unsigned char *)in, out, 16, tag);
    mbedtls_gcm_free(&g);
    return rc == 0 ? 0 : -1;
}

int fc_gcm_decrypt(const uint8_t key[32], const void *iv, size_t ivlen, const void *in, size_t n,
                   const uint8_t tag[16], uint8_t *out)
{
    mbedtls_gcm_context g;
    int rc;
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&g, n, (const unsigned char *)iv, ivlen, NULL, 0, tag, 16,
                                      (const unsigned char *)in, out);
    mbedtls_gcm_free(&g);
    if (rc != 0 && n)
        memset(out, 0, n);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Text                                                                      */
/* ------------------------------------------------------------------------ */

int fc_base64_encode(const void *in, size_t n, char *out, size_t cap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, (const unsigned char *)in, n) != 0)
        return -1;
    return (int)olen;
}

int fc_base64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    size_t olen = 0;
    if (mbedtls_base64_decode(out, cap, &olen, (const unsigned char *)in, n) != 0)
        return -1;
    return (int)olen;
}

void fc_hex(const void *in, size_t n, char *out)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = "0123456789abcdef"[p[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[p[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
}

int fc_unhex(const char *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i;
    if (n % 2 || n / 2 > cap)
        return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

/* ------------------------------------------------------------------------ */
/* Argon2id                                                                  */
/* ------------------------------------------------------------------------ */

void fc_argon2id(const void *pw, size_t pwlen, const void *salt, size_t slen, unsigned t, unsigned m_kib,
                 unsigned lanes, uint8_t *out, size_t outlen, void *work)
{
    crypto_argon2_config cfg;
    crypto_argon2_inputs in;
    cfg.algorithm = CRYPTO_ARGON2_ID;
    cfg.nb_blocks = m_kib;
    cfg.nb_passes = t;
    cfg.nb_lanes = lanes;
    in.pass = (const uint8_t *)pw;
    in.pass_size = (uint32_t)pwlen;
    in.salt = (const uint8_t *)salt;
    in.salt_size = (uint32_t)slen;
    crypto_argon2(out, (uint32_t)outlen, work, cfg, in, crypto_argon2_no_extras);
}
