/*
 * filen.c - A small client for Filen, written from the protocol.
 *
 * No libc beyond the string functions: also compiled to WebAssembly.
 */
#include "filen.h"

#include <string.h>

#include "fcrypto.h"
#include "util.h"

static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void cat(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(dst);
    while (*s && n + 1 < cap)
        dst[n++] = *s++;
    dst[n] = 0;
}

static int is_hex(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F')))
            return 0;
    return (int)strlen(s) == n;
}

int filen_random_text(char *out, int n)
{
    unsigned char r[64];
    int i;
    if (n > (int)sizeof r || fc_random(r, (size_t)n) != 0)
        return -1;
    for (i = 0; i < n; i++)
        out[i] = CHARSET[r[i] % 64];
    out[n] = 0;
    return 0;
}

int filen_uuid(char out[FILEN_UUID])
{
    unsigned char r[16];
    char h[33];
    if (fc_random(r, sizeof r) != 0)
        return -1;
    r[6] = (unsigned char)((r[6] & 0x0F) | 0x40);
    r[8] = (unsigned char)((r[8] & 0x3F) | 0x80);
    fc_hex(r, sizeof r, h);
    memcpy(out, h, 8);
    out[8] = '-';
    memcpy(out + 9, h + 8, 4);
    out[13] = '-';
    memcpy(out + 14, h + 12, 4);
    out[18] = '-';
    memcpy(out + 19, h + 16, 4);
    out[23] = '-';
    memcpy(out + 24, h + 20, 12);
    out[36] = 0;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Metadata, names, chunks                                                   */
/* ------------------------------------------------------------------------ */

/* The AES key of "002": the key text through one round of PBKDF2. */
static void key_002(const char *key, uint8_t out[32])
{
    fc_pbkdf2_sha512(key, strlen(key), key, strlen(key), 1, out, 32);
}

int filen_meta_encrypt(const char *key, const char *plain, int n, char *out, int cap)
{
    static uint8_t buf[FILEN_BODY_CAP];
    uint8_t k[32];
    char iv[13];
    int len;
    if (n + 16 > (int)sizeof buf || cap < 16 || filen_random_text(iv, 12) != 0)
        return -1;
    key_002(key, k);
    fc_gcm_encrypt(k, iv, 12, plain, (size_t)n, buf, buf + n);
    fc_wipe(k, sizeof k);
    memcpy(out, "002", 3);
    memcpy(out + 3, iv, 12);
    len = fc_base64_encode(buf, (size_t)n + 16, out + 15, (size_t)cap - 15);
    return len < 0 ? -1 : 15 + len;
}

int filen_meta_decrypt(const char *key, const char *meta, char *out, int cap)
{
    static uint8_t buf[FILEN_BODY_CAP];
    uint8_t k[32], iv[12];
    const char *iv_text, *b64;
    int n, ok;
    size_t ivlen;
    if (strncmp(meta, "002", 3) == 0 && strlen(meta) > 15) {
        key_002(key, k);
        iv_text = meta + 3;
        ivlen = 12;
        b64 = meta + 15;
    } else if (strncmp(meta, "003", 3) == 0 && strlen(meta) > 27 && is_hex(key, 64)) {
        fc_unhex(key, 64, k, sizeof k);
        if (fc_unhex(meta + 3, 24, iv, sizeof iv) != 12)
            return -1;
        iv_text = (const char *)iv;
        ivlen = 12;
        b64 = meta + 27;
    } else {
        return -1; /* version 1 (CryptoJS) is not read */
    }
    n = fc_base64_decode(b64, strlen(b64), buf, sizeof buf);
    if (n < 16 || n - 16 >= cap) {
        fc_wipe(k, sizeof k);
        return -1;
    }
    ok = fc_gcm_decrypt(k, iv_text, ivlen, buf, (size_t)n - 16, buf + n - 16, (uint8_t *)out) == 0;
    fc_wipe(k, sizeof k);
    if (!ok)
        return -1;
    out[n - 16] = 0;
    return n - 16;
}

int filen_meta_open(const filen_session *s, const char *meta, char *out, int cap)
{
    int i, n;
    for (i = s->nkeys - 1; i >= 0; i--)
        if ((n = filen_meta_decrypt(s->keys[i], meta, out, cap)) >= 0)
            return n;
    return -1;
}

void filen_name_hash(const filen_session *s, const char *name, char out[65])
{
    char lower[256];
    uint8_t h[64], key[32];
    char hex[129];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof lower; i++)
        lower[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
    lower[i] = 0;
    if (s->auth_version >= 3 && fc_unhex(s->hmac_key, strlen(s->hmac_key), key, sizeof key) == 32) {
        fc_hmac_sha256(key, sizeof key, lower, i, h);
        fc_hex(h, 32, out);
        return;
    }
    fc_sha512(lower, i, h);
    fc_hex(h, 64, hex);
    fc_sha1(hex, 128, h);
    fc_hex(h, 20, out);
}

/* The AES key of a file: 32 characters as they are, or 64 hex digits. */
static int file_key(const char *key, uint8_t out[32])
{
    if (strlen(key) == 32) {
        memcpy(out, key, 32);
        return 0;
    }
    return is_hex(key, 64) && fc_unhex(key, 64, out, 32) == 32 ? 0 : -1;
}

int filen_chunk_encrypt(const char *key, const void *in, int n, unsigned char *out)
{
    uint8_t k[32];
    if (file_key(key, k) != 0)
        return -1;
    if (strlen(key) == 32) {
        char iv[13];
        if (filen_random_text(iv, 12) != 0)
            return -1;
        memcpy(out, iv, 12); /* version 2: the IV is text too */
    } else if (fc_random(out, 12) != 0) {
        return -1;
    }
    fc_gcm_encrypt(k, out, 12, in, (size_t)n, out + 12, out + 12 + n);
    fc_wipe(k, sizeof k);
    return n + 28;
}

int filen_chunk_decrypt(const char *key, const void *in, int n, unsigned char *out)
{
    const unsigned char *p = (const unsigned char *)in;
    uint8_t k[32];
    int ok;
    if (n < 28 || file_key(key, k) != 0)
        return -1;
    ok = fc_gcm_decrypt(k, p, 12, p + 12, (size_t)n - 28, p + n - 16, out) == 0;
    fc_wipe(k, sizeof k);
    return ok ? n - 28 : -1;
}

/* ------------------------------------------------------------------------ */
/* Requests and replies                                                      */
/* ------------------------------------------------------------------------ */

static void set_auth(filen_t *f)
{
    str_copy(f->req.auth, sizeof f->req.auth, "Bearer ");
    cat(f->req.auth, sizeof f->req.auth, f->s.api_key[0] ? f->s.api_key : "anonymous");
}

void filen_post(filen_t *f, const char *path)
{
    uint8_t h[64];
    memset(&f->req, 0, sizeof f->req);
    f->req.method = FILEN_POST;
    str_copy(f->req.url, sizeof f->req.url, FILEN_GATEWAY);
    cat(f->req.url, sizeof f->req.url, path);
    set_auth(f);
    f->req.json = 1;
    f->req.body = f->body;
    f->req.body_len = (int)strlen((const char *)f->body);
    fc_sha512(f->body, (size_t)f->req.body_len, h);
    fc_hex(h, sizeof h, f->req.checksum);
}

void filen_post_bytes(filen_t *f, const char *url, int len, const char *checksum)
{
    memset(&f->req, 0, sizeof f->req);
    f->req.method = FILEN_POST;
    str_copy(f->req.url, sizeof f->req.url, url);
    set_auth(f);
    str_copy(f->req.checksum, sizeof f->req.checksum, checksum);
    f->req.body = f->body;
    f->req.body_len = len;
}

void filen_get(filen_t *f, const char *url, int auth)
{
    memset(&f->req, 0, sizeof f->req);
    f->req.method = FILEN_GET;
    str_copy(f->req.url, sizeof f->req.url, url);
    if (auth)
        set_auth(f);
}

int filen_fail(filen_t *f, const char *why)
{
    if (why)
        str_copy(f->error, sizeof f->error, why);
    return FILEN_FAIL;
}

/* An API message for people: printable, short. */
static void set_message(filen_t *f, const char *m)
{
    int k = 0;
    for (; *m && k < (int)sizeof f->error - 1; m++)
        if ((unsigned char)*m >= 0x20)
            f->error[k++] = *m;
    f->error[k] = 0;
}

int filen_ok(filen_t *f, json_t *data)
{
    json_t d = json_doc(f->resp, f->resp_len), v;
    char msg[160];
    f->code[0] = 0;
    if (f->status == 0)
        return filen_fail(f, "No connection to Filen"), 0;
    if (json_type(d) != JSON_OBJ || !json_get(d, "status", &v)) {
        if (f->status == 401)
            f->signed_out = 1;
        filen_fail(f, f->status == 401 ? "Signed out by Filen: sign in again" : "Filen sent an unexpected reply");
        return 0;
    }
    if (json_truthy(v) && f->status / 100 == 2) {
        if (!json_get(d, "data", data))
            data->p = NULL, data->n = 0;
        return 1;
    }
    json_get_str(d, "code", f->code, sizeof f->code);
    json_get_str(d, "message", msg, sizeof msg);
    if (f->status == 401 || strstr(f->code, "api_key") || strstr(f->code, "unauthorized")) {
        f->signed_out = 1;
        filen_fail(f, "Signed out by Filen: sign in again");
    } else {
        set_message(f, msg[0] ? msg : "Filen refused the request");
    }
    return 0;
}

void filen_reply(filen_t *f, int status, int len)
{
    f->status = status;
    f->resp_len = len < 0 ? 0 : (len > FILEN_RESP_CAP ? FILEN_RESP_CAP : len);
    f->resp[f->resp_len] = 0;
}

/* ------------------------------------------------------------------------ */
/* Tasks                                                                     */
/* ------------------------------------------------------------------------ */

static void wipe_login(filen_t *f)
{
    fc_wipe(f->l_password, sizeof f->l_password);
    fc_wipe(f->l_derived, sizeof f->l_derived);
    fc_wipe(f->l_code, sizeof f->l_code);
}

void filen_init(filen_t *f, const char *data_name)
{
    memset(f, 0, sizeof *f);
    str_copy(f->data_name, sizeof f->data_name, data_name);
}

void filen_start(filen_t *f, filen_task task, void *ctx)
{
    f->task = task;
    f->ctx = ctx;
    f->step = 0;
    f->state = FILEN_WAIT;
    f->error[0] = f->code[0] = 0;
    f->need_code = f->signed_out = 0;
    f->status = f->resp_len = 0;
}

int filen_run(filen_t *f)
{
    if (f->state != FILEN_WAIT || !f->task)
        return f->state;
    f->state = f->task(f);
    if (f->state != FILEN_WAIT)
        wipe_login(f);
    return f->state;
}

int filen_signed_in(const filen_t *f)
{
    return f->s.api_key[0] && f->s.nkeys > 0 && f->s.base_folder[0];
}

void filen_sign_out(filen_t *f)
{
    fc_wipe(&f->s, sizeof f->s);
    wipe_login(f);
    f->signed_out = 0;
}

/* ---- Folders ------------------------------------------------------------- */

static void folder_exists(filen_t *f, const char *parent, const char *name)
{
    jw_t w;
    char hash[65];
    filen_name_hash(&f->s, name, hash);
    jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
    jw_open(&w, '{');
    jw_key(&w, "parent");
    jw_str(&w, parent);
    jw_key(&w, "nameHashed");
    jw_str(&w, hash);
    jw_close(&w, '}');
    jw_end(&w);
    filen_post(f, "/v3/dir/exists");
}

static int folder_create(filen_t *f, const char *parent, const char *name)
{
    jw_t w;
    char hash[65], meta[256], enc[512];
    int n;
    jw_init(&w, meta, sizeof meta);
    jw_open(&w, '{');
    jw_key(&w, "name");
    jw_str(&w, name);
    jw_close(&w, '}');
    n = jw_end(&w);
    if (n < 0 || filen_uuid(f->l_folder) != 0 ||
        filen_meta_encrypt(f->s.keys[f->s.nkeys - 1], meta, n, enc, sizeof enc) < 0)
        return filen_fail(f, "Could not make the folder");
    filen_name_hash(&f->s, name, hash);
    jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
    jw_open(&w, '{');
    jw_key(&w, "uuid");
    jw_str(&w, f->l_folder);
    jw_key(&w, "name");
    jw_str(&w, enc);
    jw_key(&w, "nameHashed");
    jw_str(&w, hash);
    jw_key(&w, "parent");
    jw_str(&w, parent);
    jw_close(&w, '}');
    jw_end(&w);
    filen_post(f, "/v3/dir/create");
    return FILEN_WAIT;
}

/* After dir/exists: 1 and its uuid when it is there. */
static int exists_reply(filen_t *f, json_t d, char *uuid)
{
    json_t v;
    if (!json_get(d, "exists", &v) || !json_truthy(v))
        return 0;
    return json_get_str(d, "uuid", uuid, FILEN_UUID) > 0;
}

int filen_folders(filen_t *f, int first)
{
    json_t d;
    for (;;) {
        switch (f->step - first) {
        case 0:
            f->step++;
            if (f->s.app_folder[0])
                continue;
            folder_exists(f, f->s.base_folder, FILEN_APP_FOLDER);
            return FILEN_WAIT;
        case 1:
            if (!f->s.app_folder[0]) {
                if (!filen_ok(f, &d))
                    return FILEN_FAIL;
                if (!exists_reply(f, d, f->s.app_folder)) {
                    f->step++;
                    return folder_create(f, f->s.base_folder, FILEN_APP_FOLDER);
                }
            }
            f->step = first + 3;
            continue;
        case 2:
            if (!filen_ok(f, &d))
                return FILEN_FAIL;
            str_copy(f->s.app_folder, sizeof f->s.app_folder, f->l_folder);
            f->step++;
            continue;
        case 3:
            f->step++;
            if (f->s.data_folder[0])
                continue;
            folder_exists(f, f->s.app_folder, f->data_name);
            return FILEN_WAIT;
        case 4:
            if (!f->s.data_folder[0]) {
                if (!filen_ok(f, &d))
                    return FILEN_FAIL;
                if (!exists_reply(f, d, f->s.data_folder)) {
                    f->step++;
                    return folder_create(f, f->s.app_folder, f->data_name);
                }
            }
            f->step = first + FILEN_FOLDER_STEPS;
            return FILEN_DONE;
        case 5:
            if (!filen_ok(f, &d))
                return FILEN_FAIL;
            str_copy(f->s.data_folder, sizeof f->s.data_folder, f->l_folder);
            f->step = first + FILEN_FOLDER_STEPS;
            return FILEN_DONE;
        default:
            return FILEN_DONE;
        }
    }
}

/* ---- Sign in ------------------------------------------------------------- */

/* Derives the login password and the first master key. */
static int derive(filen_t *f, int version)
{
    uint8_t dk[64], h[64], salt[300];
    char hex[129];
    int n;
    if (version == 2) {
        fc_pbkdf2_sha512(f->l_password, strlen(f->l_password), f->l_salt, strlen(f->l_salt), 200000, dk, 64);
        fc_hex(dk, 64, hex);
        fc_sha512(hex + 64, 64, h); /* the second half, as text */
        fc_hex(h, 64, f->l_derived);
    } else {
        n = fc_unhex(f->l_salt, strlen(f->l_salt), salt, sizeof salt);
        if (!f->work && f->get_work)
            f->work = f->get_work();
        if (n < 0 || !f->work)
            return -1;
        fc_argon2id(f->l_password, strlen(f->l_password), salt, (size_t)n, 3, FILEN_ARGON2_KIB, 4, dk, 64, f->work);
        fc_hex(dk, 64, hex);
        str_copy(f->l_derived, sizeof f->l_derived, hex + 64);
    }
    hex[64] = 0;
    str_copy(f->s.keys[0], sizeof f->s.keys[0], hex);
    f->s.nkeys = 1;
    fc_wipe(dk, sizeof dk);
    fc_wipe(hex, sizeof hex);
    fc_wipe(f->l_password, sizeof f->l_password);
    return 0;
}

static void add_key(filen_session *s, const char *k, int n)
{
    int i;
    if (n <= 0 || n >= FILEN_KEY_CAP || s->nkeys >= FILEN_KEYS)
        return;
    for (i = 0; i < s->nkeys; i++)
        if ((int)strlen(s->keys[i]) == n && memcmp(s->keys[i], k, (size_t)n) == 0)
            return;
    memcpy(s->keys[s->nkeys], k, (size_t)n);
    s->keys[s->nkeys][n] = 0;
    s->nkeys++;
}

static int login_task(filen_t *f)
{
    static char text[8192];
    json_t d, v;
    jw_t w;
    long long version = 0;
    int n, i;

    switch (f->step) {
    case 0:
        jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
        jw_open(&w, '{');
        jw_key(&w, "email");
        jw_str(&w, f->s.email);
        jw_close(&w, '}');
        jw_end(&w);
        filen_post(f, "/v3/auth/info");
        f->step = 1;
        return FILEN_WAIT;

    case 1:
        if (!filen_ok(f, &d))
            return FILEN_FAIL;
        if (!json_get(d, "authVersion", &v) || !json_int(v, &version) ||
            json_get_str(d, "salt", f->l_salt, sizeof f->l_salt) <= 0)
            return filen_fail(f, "Filen sent an unexpected reply");
        if (version != 2 && version != 3)
            return filen_fail(f, "This Filen account is too old for CWtty: change its password on filen.io first");
        f->s.auth_version = (int)version;
        if (derive(f, (int)version) != 0)
            return filen_fail(f, "Not enough memory to sign in");
        jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
        jw_open(&w, '{');
        jw_key(&w, "email");
        jw_str(&w, f->s.email);
        jw_key(&w, "password");
        jw_str(&w, f->l_derived);
        jw_key(&w, "twoFactorCode");
        jw_str(&w, f->l_code[0] ? f->l_code : "XXXXXX");
        jw_key(&w, "authVersion");
        jw_int(&w, version);
        jw_close(&w, '}');
        jw_end(&w);
        filen_post(f, "/v3/login");
        fc_wipe(f->l_derived, sizeof f->l_derived);
        f->step = 2;
        return FILEN_WAIT;

    case 2:
        fc_wipe(f->body, sizeof f->body); /* it held the derived password */
        if (!filen_ok(f, &d)) {
            if (strcmp(f->code, "enter_2fa") == 0) {
                f->need_code = 1;
                return filen_fail(f, "Enter the two-factor code of your Filen account");
            }
            if (strcmp(f->code, "wrong_2fa") == 0) {
                f->need_code = 1;
                return filen_fail(f, "Wrong two-factor code");
            }
            if (strcmp(f->code, "email_or_password_wrong") == 0)
                return filen_fail(f, "Wrong email or password");
            return FILEN_FAIL;
        }
        if (json_get_str(d, "apiKey", f->s.api_key, sizeof f->s.api_key) <= 0)
            return filen_fail(f, "Filen sent an unexpected reply");
        filen_get(f, FILEN_GATEWAY "/v3/user/baseFolder", 1);
        f->step = 3;
        return FILEN_WAIT;

    case 3:
        if (!filen_ok(f, &d))
            return FILEN_FAIL;
        if (json_get_str(d, "uuid", f->s.base_folder, sizeof f->s.base_folder) <= 0)
            return filen_fail(f, "Filen sent an unexpected reply");
        if (f->s.auth_version == 3) {
            filen_get(f, FILEN_GATEWAY "/v3/user/dek", 1);
            f->step = 5;
            return FILEN_WAIT;
        }
        /* Version 2: the other master keys, sent encrypted with the last. */
        n = filen_meta_encrypt(f->s.keys[0], f->s.keys[0], (int)strlen(f->s.keys[0]), text, sizeof text);
        jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
        jw_open(&w, '{');
        jw_key(&w, "masterKeys");
        jw_strn(&w, text, n);
        jw_close(&w, '}');
        if (n < 0 || jw_end(&w) < 0)
            return filen_fail(f, "Could not read the keys");
        filen_post(f, "/v3/user/masterKeys");
        f->step = 4;
        return FILEN_WAIT;

    case 4:
        if (!filen_ok(f, &d))
            return FILEN_FAIL;
        {
            char enc[4096];
            if (json_get_str(d, "keys", enc, sizeof enc) > 0 && filen_meta_open(&f->s, enc, text, sizeof text) > 0) {
                const char *p = text, *bar;
                while (*p) {
                    bar = strchr(p, '|');
                    n = bar ? (int)(bar - p) : (int)strlen(p);
                    add_key(&f->s, p, n);
                    p += n + (bar ? 1 : 0);
                }
            }
        }
        f->step = 7;
        return filen_folders(f, 7);

    case 5: /* version 3: the data key, encrypted with the derived key */
        if (!filen_ok(f, &d))
            return FILEN_FAIL;
        if (json_get_str(d, "dek", text, sizeof text) <= 0)
            return filen_fail(f, "Sign in to filen.io once first, then here");
        {
            char dek[FILEN_KEY_CAP];
            n = filen_meta_decrypt(f->s.keys[0], text, dek, sizeof dek);
            if (n != 64)
                return filen_fail(f, "Could not read the keys");
            str_copy(f->s.keys[0], sizeof f->s.keys[0], dek);
            fc_wipe(dek, sizeof dek);
        }
        filen_get(f, FILEN_GATEWAY "/v3/user/keyPair/info", 1);
        f->step = 6;
        return FILEN_WAIT;

    case 6: /* version 3: names are hashed with a key from the private key */
        if (!filen_ok(f, &d))
            return FILEN_FAIL;
        {
            static uint8_t der[4096];
            char enc[8192];
            uint8_t key[32];
            if (json_get_str(d, "privateKey", enc, sizeof enc) <= 0 ||
                (n = filen_meta_open(&f->s, enc, text, sizeof text)) <= 16 ||
                (i = fc_base64_decode(text, (size_t)n, der, sizeof der)) <= 0)
                return filen_fail(f, "Could not read the keys");
            fc_hkdf_sha256(der, (size_t)i, "", 0, "hmac-sha256-key", 15, key, sizeof key);
            fc_hex(key, sizeof key, f->s.hmac_key);
            fc_wipe(key, sizeof key);
            fc_wipe(der, sizeof der);
            fc_wipe(text, sizeof text);
        }
        f->step = 7;
        return filen_folders(f, 7);

    default:
        if (f->step >= 7)
            return filen_folders(f, 7);
        break;
    }
    return FILEN_FAIL;
}

void filen_login(filen_t *f, const char *email, const char *password, const char *code)
{
    int k = 0;
    while (*email == ' ')
        email++;
    fc_wipe(&f->s, sizeof f->s);
    str_copy(f->s.email, sizeof f->s.email, email);
    for (k = (int)strlen(f->s.email); k > 0 && f->s.email[k - 1] == ' '; k--)
        f->s.email[k - 1] = 0;
    str_copy(f->l_password, sizeof f->l_password, password);
    for (k = 0; *code && k < (int)sizeof f->l_code - 1; code++)
        if (*code >= '0' && *code <= '9')
            f->l_code[k++] = *code;
    f->l_code[k] = 0;
    f->signed_out = 0;
    filen_start(f, login_task, NULL);
}

/* ------------------------------------------------------------------------ */
/* The session as text                                                       */
/* ------------------------------------------------------------------------ */

static void line(char *out, int cap, int *n, const char *key, const char *value)
{
    int k = (int)strlen(key), v = (int)strlen(value);
    if (*n + k + v + 2 < cap) {
        memcpy(out + *n, key, (size_t)k);
        out[*n + k] = '=';
        memcpy(out + *n + k + 1, value, (size_t)v);
        out[*n + k + 1 + v] = '\n';
    }
    *n += k + v + 2;
}

int filen_session_format(const filen_session *s, char *out, int cap)
{
    int n = 0, i;
    char version[4] = {(char)('0' + s->auth_version % 10), 0};
    line(out, cap, &n, "email", s->email);
    line(out, cap, &n, "api_key", s->api_key);
    line(out, cap, &n, "auth_version", version);
    for (i = 0; i < s->nkeys; i++)
        line(out, cap, &n, "master_key", s->keys[i]);
    line(out, cap, &n, "hmac_key", s->hmac_key);
    line(out, cap, &n, "base_folder", s->base_folder);
    line(out, cap, &n, "app_folder", s->app_folder);
    line(out, cap, &n, "data_folder", s->data_folder);
    if (n >= cap)
        return -1;
    out[n] = 0;
    return n;
}

int filen_session_parse(filen_session *s, const char *text)
{
    memset(s, 0, sizeof *s);
    while (*text) {
        const char *key = text, *eq = text, *end;
        char value[256];
        int vn;
        while (*eq && *eq != '=' && *eq != '\n')
            eq++;
        end = eq;
        while (*end && *end != '\n')
            end++;
        if (*eq == '=') {
            int kn = (int)(eq - key);
            vn = (int)(end - eq - 1);
            if (vn > 0 && end[-1] == '\r')
                vn--;
            if (vn >= (int)sizeof value)
                vn = (int)sizeof value - 1;
            memcpy(value, eq + 1, (size_t)vn);
            value[vn] = 0;
#define IS(k) (kn == (int)sizeof(k) - 1 && memcmp(key, k, sizeof(k) - 1) == 0)
            if (IS("email"))
                str_copy(s->email, sizeof s->email, value);
            else if (IS("api_key"))
                str_copy(s->api_key, sizeof s->api_key, value);
            else if (IS("auth_version"))
                s->auth_version = value[0] - '0';
            else if (IS("master_key"))
                add_key(s, value, vn);
            else if (IS("hmac_key"))
                str_copy(s->hmac_key, sizeof s->hmac_key, value);
            else if (IS("base_folder"))
                str_copy(s->base_folder, sizeof s->base_folder, value);
            else if (IS("app_folder"))
                str_copy(s->app_folder, sizeof s->app_folder, value);
            else if (IS("data_folder"))
                str_copy(s->data_folder, sizeof s->data_folder, value);
#undef IS
            fc_wipe(value, sizeof value);
        }
        text = *end ? end + 1 : end;
    }
    return s->api_key[0] && s->nkeys > 0 && (s->auth_version == 2 || s->auth_version == 3) ? 0 : -1;
}
