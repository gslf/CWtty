/*
 * test_filen.c - Self-checking tests for the cloud sync of the statistics:
 * the JSON reader and writer, the cryptography (checked byte for byte
 * against values from Filen's official SDK, tests/filen_vectors.h), the
 * Filen protocol against a fake server, and the sync of two devices.
 * Offline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fcrypto.h"
#include "filen.h"
#include "filen_vectors.h"
#include "json.h"
#include "statsbook.h"
#include "statsync.h"

static int failures;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

static void hex_of(const void *p, size_t n, char *out)
{
    fc_hex(p, n, out);
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */
/* ------------------------------------------------------------------------ */

static void test_json(void)
{
    static const char doc[] =
        " {\"status\":true,\"message\":\"ok\",\"code\":\"x\",\"data\":{\"apiKey\":\"k\\u00e9y\\\"\\ud83d\\ude00\","
        "\"n\":-42,\"s\":\"17\",\"list\":[{\"a\":1},[],\"x,y]\",null,false],\"empty\":{}}} ";
    char buf[256], out[64];
    json_t d = json_doc(doc, -1), v, it = {NULL, 0};
    long long n = 0;
    int k = 0;
    jw_t w;

    printf("json: reader and a writer like JSON.stringify\n");
    CHECK(json_type(d) == JSON_OBJ, "a document");
    CHECK(json_path(d, "data.apiKey", &v) && json_str(v, out, sizeof out) == 9 && strcmp(out, "k\xc3\xa9y\"\xf0\x9f\x98\x80") == 0,
          "escapes: %s", out);
    CHECK(json_str(v, out, 4) == -1 && out[0] == 0, "a short buffer was not reported");
    CHECK(json_path(d, "data.n", &v) && json_int(v, &n) && n == -42, "a number");
    CHECK(json_path(d, "data.s", &v) && json_int(v, &n) && n == 17, "a numeric string");
    CHECK(json_get(d, "status", &v) && json_truthy(v) && !json_get(d, "nope", &v) && !json_path(d, "data.x.y", &v),
          "members");
    CHECK(json_path(d, "data.list", &v) && json_type(v) == JSON_ARR, "an array");
    while (json_next(v, &it))
        k++;
    CHECK(k == 5, "array items: %d", k);
    CHECK(json_path(d, "data.empty", &v) && (it.p = NULL, !json_next(v, &it)), "an empty object");
    CHECK(json_get_str(d, "missing", out, sizeof out) == 0 && out[0] == 0, "a missing string");
    CHECK(json_type(json_doc("{\"a\":1", -1)) == JSON_NONE && json_type(json_doc("{} x", -1)) == JSON_NONE &&
              json_type(json_doc("[1,]", -1)) == JSON_NONE,
          "broken documents were accepted");

    /* The writer: the same bytes as JSON.stringify() in the SDK. */
    jw_init(&w, buf, sizeof buf);
    jw_open(&w, '{');
    jw_key(&w, "email");
    jw_str(&w, "op@example.org");
    jw_key(&w, "password");
    jw_str(&w, "x\"y\\z\n\t\x01\xc3\xa9\xe2\x9c\x93");
    jw_key(&w, "twoFactorCode");
    jw_str(&w, "XXXXXX");
    jw_key(&w, "authVersion");
    jw_int(&w, 2);
    jw_key(&w, "ok");
    jw_bool(&w, 1);
    jw_key(&w, "list");
    jw_open(&w, '[');
    jw_int(&w, 1);
    jw_str(&w, "two");
    jw_bool(&w, 0);
    jw_close(&w, ']');
    jw_close(&w, '}');
    CHECK(jw_end(&w) > 0 && strcmp(buf, FV_BODY_JSON) == 0, "writer:\n    %s\n    %s", buf, FV_BODY_JSON);
    {
        uint8_t h[64];
        char hex[129];
        fc_sha512(buf, strlen(buf), h);
        hex_of(h, sizeof h, hex);
        CHECK(strcmp(hex, FV_BODY_CHECKSUM) == 0, "the Checksum of a body");
    }
    jw_init(&w, buf, 8);
    jw_open(&w, '{');
    jw_key(&w, "long key");
    jw_close(&w, '}');
    CHECK(jw_end(&w) == -1 && strlen(buf) == 7, "an overflow was not reported");
}

/* ------------------------------------------------------------------------ */
/* Cryptography                                                              */
/* ------------------------------------------------------------------------ */

static void test_crypto(void)
{
    uint8_t dk[64], h[64], key[32], ct[256], tag[16], pt[256], salt[64];
    char hex[260], b64[256];
    static uint8_t work[FC_ARGON2_WORK(65536)];
    int n;

    printf("crypto: against the official SDK (derivation, metadata, names, chunks)\n");

    /* Random: refused until seeded, then never the same twice. */
    CHECK(!fc_seeded() && fc_random(dk, 8) == -1, "random before seeding");
    fc_seed("0123456789abcdef0123456789abcdef", 32);
    fc_random(dk, 32);
    fc_random(h, 32);
    CHECK(fc_seeded() && memcmp(dk, h, 32) != 0, "random repeats");

    /* Auth v2: PBKDF2-HMAC-SHA512, 200000 rounds; the login password is
     * the SHA-512 of the second half, as text. */
    fc_pbkdf2_sha512(FV_PASSWORD, strlen(FV_PASSWORD), FV_SALT_V2, strlen(FV_SALT_V2), 200000, dk, 64);
    hex_of(dk, 64, hex);
    CHECK(strncmp(hex, FV_V2_MASTER_KEY, 64) == 0, "v2 master key %.64s", hex);
    fc_sha512(hex + 64, 64, h);
    hex_of(h, 64, b64);
    CHECK(strcmp(b64, FV_V2_PASSWORD) == 0, "v2 password");

    /* Auth v3: Argon2id, t=3, 64 MiB, 4 lanes. */
    n = fc_unhex(FV_SALT_V3, strlen(FV_SALT_V3), salt, sizeof salt);
    fc_argon2id(FV_PASSWORD, strlen(FV_PASSWORD), salt, (size_t)n, 3, 65536, 4, dk, 64, work);
    hex_of(dk, 64, hex);
    CHECK(strncmp(hex, FV_V3_MASTER_KEY, 64) == 0 && strcmp(hex + 64, FV_V3_PASSWORD) == 0, "v3 keys %s", hex);

    /* Metadata 002: key PBKDF2(key, key, 1 round), IV 12 characters. */
    fc_pbkdf2_sha512(FV_KEY2, 32, FV_KEY2, 32, 1, key, 32);
    n = fc_base64_decode(FV_META_002 + 15, strlen(FV_META_002 + 15), ct, sizeof ct);
    CHECK(n == (int)strlen(FV_META) + 16 && fc_gcm_decrypt(key, FV_META_002 + 3, 12, ct, (size_t)n - 16, ct + n - 16, pt) == 0 &&
              memcmp(pt, FV_META, strlen(FV_META)) == 0,
          "002 decrypt");
    fc_gcm_encrypt(key, FV_META_002 + 3, 12, FV_META, strlen(FV_META), ct, tag);
    memcpy(ct + strlen(FV_META), tag, 16);
    fc_base64_encode(ct, strlen(FV_META) + 16, b64, sizeof b64);
    CHECK(strcmp(b64, FV_META_002 + 15) == 0, "002 encrypt");
    ct[0] ^= 1;
    CHECK(fc_gcm_decrypt(key, FV_META_002 + 3, 12, ct, strlen(FV_META), tag, pt) == -1 && pt[0] == 0,
          "a forged message was opened");

    /* Metadata 003: hex key, IV 12 bytes as hex. */
    {
        uint8_t k3[32], iv[12];
        fc_unhex(FV_KEY3, 64, k3, sizeof k3);
        fc_unhex(FV_META_003 + 3, 24, iv, sizeof iv);
        n = fc_base64_decode(FV_META_003 + 27, strlen(FV_META_003 + 27), ct, sizeof ct);
        CHECK(n > 16 && fc_gcm_decrypt(k3, iv, 12, ct, (size_t)n - 16, ct + n - 16, pt) == 0 &&
                  memcmp(pt, FV_META, strlen(FV_META)) == 0,
              "003 decrypt");
    }

    /* The HMAC key of v3 accounts, from the private key. */
    n = fc_base64_decode(FV_PRIVATE_KEY, strlen(FV_PRIVATE_KEY), ct, sizeof ct);
    fc_hkdf_sha256(ct, (size_t)n, "", 0, "hmac-sha256-key", 15, key, 32);
    hex_of(key, 32, hex);
    CHECK(strcmp(hex, FV_HMAC_KEY) == 0, "HKDF %s", hex);

    /* Name hashes. */
    fc_sha512("stats-v1", 8, h);
    hex_of(h, 64, b64);
    fc_sha1(b64, 128, h);
    hex_of(h, 20, hex);
    CHECK(strcmp(hex, FV_NAME_V2) == 0, "v2 name hash");
    fc_hmac_sha256(key, 32, "stats-v1", 8, h);
    hex_of(h, 32, hex);
    CHECK(strcmp(hex, FV_NAME_V3) == 0, "v3 name hash");

    /* A file chunk (v2): IV || ciphertext || tag, key the 32 characters. */
    {
        static uint8_t want[256], got[256];
        size_t len = strlen(FV_CHUNK);
        int wn = fc_unhex(FV_CHUNK_ENC, strlen(FV_CHUNK_ENC), want, sizeof want);
        memcpy(got, want, 12);
        fc_gcm_encrypt((const uint8_t *)FV_KEY2, got, 12, FV_CHUNK, len, got + 12, got + 12 + len);
        CHECK(wn == (int)len + 28 && memcmp(got, want, (size_t)wn) == 0, "chunk encrypt");
    }

    /* base64 and hex edges. */
    CHECK(fc_base64_encode("ab", 2, b64, 4) == -1 && fc_base64_encode("ab", 2, b64, 5) == 4 && strcmp(b64, "YWI=") == 0,
          "base64 room");
    CHECK(fc_base64_decode("Y*I=", 4, ct, sizeof ct) == -1 && fc_unhex("abc", 3, ct, 8) == -1 &&
              fc_unhex("zz", 2, ct, 8) == -1 && fc_unhex("ABcd", 4, ct, 8) == 2 && ct[0] == 0xAB,
          "bad input accepted");
}


/* ------------------------------------------------------------------------ */
/* A fake Filen                                                              */
/* ------------------------------------------------------------------------ */

#define OLD_KEY "OldMasterKeyFromBefore2023xxxxxx" /* a second master key */
#define DEK "00112233445566778899aabbccddeeff00112233445566778899aabbccddeef0"

typedef struct {
    char uuid[FILEN_UUID], parent[FILEN_UUID], hash[65], name[256];
} fake_folder;

typedef struct {
    char          uuid[FILEN_UUID], parent[FILEN_UUID], hash[65], upload_key[40];
    char          metadata[2048];
    unsigned char chunk[FILEN_BODY_CAP];
    int           len, done, old; /* old: an earlier version */
    long long     ts;
} fake_file;

static struct {
    int         version;  /* 2 or 3 */
    int         two_factor;
    char        api_key[40];
    fake_folder folders[8];
    int         nfolders;
    fake_file   files[24];
    int         nfiles;
    long long   clock;
    int         requests, downloads, uploads, lists, bad;
    char        last_error[200];
} fs;

static void fake_reset(int version, int two_factor)
{
    memset(&fs, 0, sizeof fs);
    fs.version = version;
    fs.two_factor = two_factor;
    strcpy(fs.api_key, "api-key-0001");
}

static void reply_json(filen_t *f, int status, const char *json)
{
    int n = (int)strlen(json);
    memcpy(f->resp, json, (size_t)n + 1);
    filen_reply(f, status, n);
}

static void reply_ok(filen_t *f, const char *data)
{
    static char buf[16384];
    snprintf(buf, sizeof buf, "{\"status\":true,\"message\":\"ok\",\"code\":\"ok\",\"data\":%s}", data ? data : "{}");
    reply_json(f, 200, buf);
}

static void reply_err(filen_t *f, const char *code, const char *message)
{
    char buf[512];
    snprintf(buf, sizeof buf, "{\"status\":false,\"message\":\"%s\",\"code\":\"%s\"}", message, code);
    reply_json(f, 200, buf);
}

/* A protocol mistake of the client: the test fails. */
static void bad(filen_t *f, const char *why)
{
    fs.bad++;
    snprintf(fs.last_error, sizeof fs.last_error, "%s (%.120s)", why, f->req.url);
    reply_err(f, "bad_request", why);
}

static void query(const char *url, const char *key, char *out, int cap)
{
    char pat[40];
    const char *p, *e;
    snprintf(pat, sizeof pat, "%s=", key);
    out[0] = 0;
    p = strstr(url, "?");
    while (p && (p = strstr(p, pat)) != NULL && p[-1] != '?' && p[-1] != '&')
        p++;
    if (!p)
        return;
    p += strlen(pat);
    e = strchr(p, '&');
    snprintf(out, (size_t)cap, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
}

static fake_folder *folder_by(const char *parent, const char *hash)
{
    int i;
    for (i = 0; i < fs.nfolders; i++)
        if (strcmp(fs.folders[i].parent, parent) == 0 && strcmp(fs.folders[i].hash, hash) == 0)
            return &fs.folders[i];
    return NULL;
}

static void serve(filen_t *f)
{
    static char body[FILEN_BODY_CAP + 1], out[8192], tmp[4096];
    char expect[200];
    const char *url = f->req.url, *path;
    json_t d;
    uint8_t h[64];
    char hex[129];
    int i;

    fs.requests++;
    fs.clock += 1000;
    memcpy(body, f->req.body ? (const char *)f->req.body : "", (size_t)f->req.body_len);
    body[f->req.body_len] = 0;
    d = json_doc(body, f->req.body_len);
    path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : url;

    /* Downloads: no key, the chunk as it is. */
    if (strncmp(url, FILEN_EGEST "/", strlen(FILEN_EGEST) + 1) == 0) {
        fs.downloads++;
        for (i = 0; i < fs.nfiles; i++)
            if (strstr(url, fs.files[i].uuid) && fs.files[i].done) {
                memcpy(f->resp, fs.files[i].chunk, (size_t)fs.files[i].len);
                filen_reply(f, 200, fs.files[i].len);
                return;
            }
        reply_json(f, 404, "not found");
        return;
    }

    /* JSON requests carry the hash of their body. */
    if (f->req.json) {
        fc_sha512(body, strlen(body), h);
        fc_hex(h, 64, hex);
        if (f->req.method != FILEN_POST || strcmp(hex, f->req.checksum) != 0 || json_type(d) != JSON_OBJ)
            return bad(f, "checksum");
    }
    snprintf(expect, sizeof expect, "Bearer %s", fs.api_key);
    if (strcmp(path, "/v3/auth/info") == 0 || strcmp(path, "/v3/login") == 0) {
        if (strcmp(f->req.auth, "Bearer anonymous") != 0)
            return bad(f, "auth before login");
    } else if (strcmp(f->req.auth, expect) != 0) {
        reply_json(f, 401, "{\"status\":false,\"message\":\"Invalid API key.\",\"code\":\"api_key_not_found\"}");
        return;
    }

    if (strcmp(path, "/v3/auth/info") == 0) {
        snprintf(out, sizeof out, "{\"email\":\"op@example.org\",\"authVersion\":%d,\"salt\":\"%s\",\"id\":7}",
                 fs.version, fs.version == 3 ? FV_SALT_V3 : FV_SALT_V2);
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/login") == 0) {
        char email[64], pw[200], code[16];
        long long v = 0;
        json_t jv;
        json_get_str(d, "email", email, sizeof email);
        json_get_str(d, "password", pw, sizeof pw);
        json_get_str(d, "twoFactorCode", code, sizeof code);
        json_get(d, "authVersion", &jv);
        json_int(jv, &v);
        if (strcmp(email, "op@example.org") != 0 || strcmp(pw, fs.version == 3 ? FV_V3_PASSWORD : FV_V2_PASSWORD) != 0 ||
            v != fs.version)
            return reply_err(f, "email_or_password_wrong", "Invalid email or password.");
        if (fs.two_factor && strcmp(code, "XXXXXX") == 0)
            return reply_err(f, "enter_2fa", "Please enter your Two Factor Authentication code.");
        if (fs.two_factor && strcmp(code, "123456") != 0)
            return reply_err(f, "wrong_2fa", "Invalid Two Factor Authentication code.");
        snprintf(out, sizeof out, "{\"apiKey\":\"%s\"}", fs.api_key);
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/user/baseFolder") == 0) {
        reply_ok(f, "{\"uuid\":\"base-0000-4000-8000-000000000000\"}");
    } else if (strcmp(path, "/v3/user/masterKeys") == 0) {
        /* The client sends its keys encrypted with the last: the account's
         * come back, encrypted with the same. */
        char enc[2048], plain[512];
        json_get_str(d, "masterKeys", enc, sizeof enc);
        if (fs.version != 2 || filen_meta_decrypt(FV_V2_MASTER_KEY, enc, plain, sizeof plain) < 0 ||
            strcmp(plain, FV_V2_MASTER_KEY) != 0)
            return bad(f, "master keys");
        snprintf(plain, sizeof plain, "%s|%s", OLD_KEY, FV_V2_MASTER_KEY);
        filen_meta_encrypt(FV_V2_MASTER_KEY, plain, (int)strlen(plain), enc, sizeof enc);
        snprintf(out, sizeof out, "{\"keys\":\"%s\"}", enc);
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/user/dek") == 0) {
        filen_meta_encrypt(FV_V3_MASTER_KEY, DEK, 64, tmp, sizeof tmp);
        snprintf(out, sizeof out, "{\"dek\":\"%s\"}", tmp);
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/user/keyPair/info") == 0) {
        filen_meta_encrypt(DEK, FV_PRIVATE_KEY, (int)strlen(FV_PRIVATE_KEY), tmp, sizeof tmp);
        snprintf(out, sizeof out, "{\"publicKey\":\"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA\",\"privateKey\":\"%s\"}", tmp);
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/dir/exists") == 0) {
        char parent[FILEN_UUID], hash[65];
        fake_folder *ff;
        json_get_str(d, "parent", parent, sizeof parent);
        json_get_str(d, "nameHashed", hash, sizeof hash);
        ff = folder_by(parent, hash);
        snprintf(out, sizeof out, "{\"exists\":%s,\"uuid\":\"%s\"}", ff ? "true" : "false", ff ? ff->uuid : "");
        reply_ok(f, out);
    } else if (strcmp(path, "/v3/dir/create") == 0) {
        fake_folder *ff = &fs.folders[fs.nfolders];
        char enc[512], plain[256], name[64];
        json_get_str(d, "uuid", ff->uuid, sizeof ff->uuid);
        json_get_str(d, "parent", ff->parent, sizeof ff->parent);
        json_get_str(d, "nameHashed", ff->hash, sizeof ff->hash);
        json_get_str(d, "name", enc, sizeof enc);
        if (filen_meta_decrypt(fs.version == 3 ? DEK : OLD_KEY, enc, plain, sizeof plain) < 0 ||
            json_get_str(json_doc(plain, -1), "name", name, sizeof name) <= 0 || strlen(ff->uuid) != 36)
            return bad(f, "folder metadata (not with the last key?)");
        strcpy(ff->name, name);
        fs.nfolders++;
        reply_ok(f, NULL);
    } else if (strcmp(path, "/v3/dir/content") == 0) {
        char parent[FILEN_UUID];
        int k = 0, any = 0;
        fs.lists++;
        json_get_str(d, "uuid", parent, sizeof parent);
        for (i = 0; i < fs.nfolders && strcmp(fs.folders[i].uuid, parent) != 0; i++) {
        }
        if (i == fs.nfolders)
            return reply_err(f, "folder_not_found", "Folder not found.");
        k += snprintf(out + k, sizeof out - (size_t)k, "{\"uploads\":[");
        for (i = 0; i < fs.nfiles; i++) {
            fake_file *ff = &fs.files[i];
            if (!ff->done || ff->old || strcmp(ff->parent, parent) != 0)
                continue;
            k += snprintf(out + k, sizeof out - (size_t)k,
                          "%s{\"uuid\":\"%s\",\"metadata\":\"%s\",\"rm\":\"x\",\"timestamp\":%lld,\"chunks\":1,"
                          "\"size\":%d,\"bucket\":\"filen-1\",\"region\":\"de-1\",\"parent\":\"%s\",\"version\":2}",
                          any ? "," : "", ff->uuid, ff->metadata, ff->ts, ff->len, ff->parent);
            any = 1;
        }
        snprintf(out + k, sizeof out - (size_t)k, "],\"folders\":[]}");
        reply_ok(f, out);
    } else if (strncmp(url, FILEN_INGEST "/v3/upload?", strlen(FILEN_INGEST) + 11) == 0) {
        fake_file *ff = &fs.files[fs.nfiles];
        char hash[129], params[1024];
        jw_t w;
        memset(ff, 0, sizeof *ff);
        query(url, "uuid", ff->uuid, sizeof ff->uuid);
        query(url, "parent", ff->parent, sizeof ff->parent);
        query(url, "uploadKey", ff->upload_key, sizeof ff->upload_key);
        query(url, "hash", hash, sizeof hash);
        fc_sha512(f->req.body, (size_t)f->req.body_len, h);
        fc_hex(h, 64, hex);
        if (strcmp(hex, hash) != 0)
            return bad(f, "chunk hash");
        jw_init(&w, params, sizeof params);
        jw_open(&w, '{');
        jw_key(&w, "uuid");
        jw_str(&w, ff->uuid);
        jw_key(&w, "index");
        jw_str(&w, "0");
        jw_key(&w, "parent");
        jw_str(&w, ff->parent);
        jw_key(&w, "uploadKey");
        jw_str(&w, ff->upload_key);
        jw_key(&w, "hash");
        jw_str(&w, hash);
        jw_close(&w, '}');
        jw_end(&w);
        fc_sha512(params, strlen(params), h);
        fc_hex(h, 64, hex);
        if (strcmp(hex, f->req.checksum) != 0 || f->req.json)
            return bad(f, "upload checksum");
        memcpy(ff->chunk, f->req.body, (size_t)f->req.body_len);
        ff->len = f->req.body_len;
        fs.nfiles++;
        reply_ok(f, "{\"bucket\":\"filen-1\",\"region\":\"de-1\"}");
    } else if (strcmp(path, "/v3/upload/done") == 0) {
        char uuid[FILEN_UUID], key[40], plain[1024], hash[65];
        fake_file *ff = NULL;
        long long v = 0;
        json_t jv;
        json_get_str(d, "uuid", uuid, sizeof uuid);
        for (i = 0; i < fs.nfiles; i++)
            if (strcmp(fs.files[i].uuid, uuid) == 0)
                ff = &fs.files[i];
        json_get_str(d, "uploadKey", key, sizeof key);
        json_get_str(d, "metadata", ff ? ff->metadata : tmp, sizeof ff->metadata);
        json_get_str(d, "nameHashed", hash, sizeof hash);
        json_get(d, "version", &jv);
        json_int(jv, &v);
        if (!ff || strcmp(key, ff->upload_key) != 0 || v != 2 ||
            filen_meta_decrypt(fs.version == 3 ? DEK : OLD_KEY, ff->metadata, plain, sizeof plain) < 0)
            return bad(f, "upload done");
        /* A file of the same name becomes an earlier version. */
        for (i = 0; i < fs.nfiles; i++)
            if (&fs.files[i] != ff && fs.files[i].done && strcmp(fs.files[i].hash, hash) == 0 &&
                strcmp(fs.files[i].parent, ff->parent) == 0)
                fs.files[i].old = 1;
        strcpy(ff->hash, hash);
        ff->done = 1;
        ff->ts = fs.clock / 1000;
        fs.uploads++;
        reply_ok(f, NULL);
    } else {
        bad(f, "unknown endpoint");
    }
}

/* Runs the task against the fake Filen. */
static int drive(filen_t *f)
{
    int guard = 0;
    while (filen_run(f) == FILEN_WAIT && guard++ < 200)
        serve(f);
    return f->state;
}

static filen_t fa, fb;
static statsync_t ya, yb;
static statsbook_t ba, bb;

static void train(statsbook_t *b, int dir, int n, int day)
{
    int i;
    for (i = 0; i < n; i++)
        stats_answer(&b->own.s, dir, 1, 1000, day);
    statsbook_changed(b);
}

static int sync_once(filen_t *f, statsync_t *y, statsbook_t *b)
{
    f->now_ms = 1.7e12;
    statsync_start(y, f, b);
    if (drive(f) != FILEN_DONE)
        return -1;
    return statsync_merge(y, b);
}

static void test_protocol(void)
{
    static uint8_t work[FC_ARGON2_WORK(FILEN_ARGON2_KIB)];
    static char text[4096];
    char out[4096], hash[65];
    filen_session s2;
    int n;

    printf("filen: sign in with two-factor codes, keys, folders (a fake server)\n");
    /* The SDK's own values. */
    CHECK(filen_meta_decrypt(FV_KEY2, FV_META_002, out, sizeof out) == (int)strlen(FV_META) && strcmp(out, FV_META) == 0 &&
              filen_meta_decrypt(FV_KEY3, FV_META_003, out, sizeof out) > 0 && strcmp(out, FV_META) == 0 &&
              filen_meta_decrypt(FV_KEY2, FV_META_003, out, sizeof out) == -1,
          "metadata of the SDK");
    {
        static uint8_t enc[256], plain[256];
        int len = fc_unhex(FV_CHUNK_ENC, strlen(FV_CHUNK_ENC), enc, sizeof enc);
        n = filen_chunk_decrypt(FV_KEY2, enc, len, plain);
        CHECK(n == (int)strlen(FV_CHUNK) && memcmp(plain, FV_CHUNK, (size_t)n) == 0, "a chunk of the SDK");
    }
    memset(&s2, 0, sizeof s2);
    s2.auth_version = 2;
    filen_name_hash(&s2, "Stats-V1", hash);
    CHECK(strcmp(hash, FV_NAME_V2) == 0, "v2 name hash");
    s2.auth_version = 3;
    strcpy(s2.hmac_key, FV_HMAC_KEY);
    filen_name_hash(&s2, "Stats-V1", hash);
    CHECK(strcmp(hash, FV_NAME_V3) == 0, "v3 name hash");
    n = filen_meta_encrypt(FV_KEY2, "hello", 5, out, sizeof out);
    CHECK(n > 15 && strncmp(out, "002", 3) == 0 && filen_meta_decrypt(FV_KEY2, out, text, sizeof text) == 5 &&
              strcmp(text, "hello") == 0,
          "metadata round trip");

    /* Version 2 with a two-factor code. */
    fake_reset(2, 1);
    filen_init(&fa, STATSYNC_FOLDER);
    filen_login(&fa, " op@example.org ", FV_PASSWORD, "");
    CHECK(drive(&fa) == FILEN_FAIL && fa.need_code && strstr(fa.error, "two-factor") && !filen_signed_in(&fa),
          "no code: %s", fa.error);
    filen_login(&fa, "op@example.org", FV_PASSWORD, "654 321");
    CHECK(drive(&fa) == FILEN_FAIL && fa.need_code && strcmp(fa.error, "Wrong two-factor code") == 0, "wrong code");
    filen_login(&fa, "op@example.org", "wrong", "123456");
    CHECK(drive(&fa) == FILEN_FAIL && !fa.need_code && strcmp(fa.error, "Wrong email or password") == 0,
          "wrong password: %s", fa.error);
    filen_login(&fa, "op@example.org", FV_PASSWORD, "123 456");
    CHECK(drive(&fa) == FILEN_DONE && filen_signed_in(&fa) && fs.bad == 0, "sign in: %s %s", fa.error, fs.last_error);
    CHECK(fa.s.nkeys == 2 && strcmp(fa.s.keys[0], FV_V2_MASTER_KEY) == 0 && strcmp(fa.s.keys[1], OLD_KEY) == 0 &&
              strcmp(fa.s.email, "op@example.org") == 0,
          "master keys: %d", fa.s.nkeys);
    CHECK(fs.nfolders == 2 && strcmp(fs.folders[0].name, "CWtty") == 0 && strcmp(fs.folders[1].name, "stats-v1") == 0 &&
              strcmp(fs.folders[1].parent, fs.folders[0].uuid) == 0 &&
              strcmp(fa.s.data_folder, fs.folders[1].uuid) == 0,
          "folders made");
    CHECK(fa.l_password[0] == 0 && fa.l_derived[0] == 0, "the password was kept");

    /* The session as text, and back. */
    n = filen_session_format(&fa.s, text, sizeof text);
    CHECK(n > 0 && !strstr(text, "password") && filen_session_parse(&s2, text) == 0 &&
              memcmp(&s2, &fa.s, sizeof s2) == 0,
          "session round trip");
    CHECK(filen_session_parse(&s2, "email=x\n") == -1, "an empty session was accepted");

    /* Signing in again finds the folders. */
    filen_init(&fb, STATSYNC_FOLDER);
    filen_login(&fb, "op@example.org", FV_PASSWORD, "123456");
    CHECK(drive(&fb) == FILEN_DONE && fs.nfolders == 2 && strcmp(fb.s.data_folder, fa.s.data_folder) == 0,
          "second device: %s", fb.error);

    printf("statsync: two devices, only what changed goes up and down\n");
    statsbook_init(&ba, "aa01", "Terminal");
    statsbook_init(&bb, "bb02", "Web");
    statsync_init(&ya);
    statsync_init(&yb);
    CHECK(sync_once(&fa, &ya, &ba) == 0 && fs.uploads == 0 && fs.lists == 1, "nothing to send: %d up", fs.uploads);
    train(&ba, STATS_COPY, 5, 20100);
    CHECK(sync_once(&fa, &ya, &ba) == 0 && fs.uploads == 1 && ya.sent == 1 && fs.bad == 0, "first upload: %s %s",
          fa.error, fs.last_error);
    train(&bb, STATS_SEND, 3, 20101);
    n = fs.downloads;
    CHECK(sync_once(&fb, &yb, &bb) == 1 && fs.downloads == n + 1 && fs.uploads == 2 && bb.others == 1 &&
              bb.total.dir[STATS_COPY].answers == 5 && bb.total.dir[STATS_SEND].answers == 3 && bb.total.days == 2,
          "the second device: %d answers", bb.total.dir[0].answers + bb.total.dir[1].answers);
    n = fs.downloads;
    CHECK(sync_once(&fa, &ya, &ba) == 1 && fs.downloads == n + 1 && fs.uploads == 2 &&
              ba.total.dir[STATS_SEND].answers == 3,
          "only the other device's file came down");
    n = fs.downloads;
    CHECK(sync_once(&fa, &ya, &ba) == 0 && fs.downloads == n && fs.uploads == 2, "nothing changed, nothing moved");
    /* Training during a sync: the live book keeps it, the next sync sends it. */
    statsync_start(&ya, &fa, &ba);
    train(&ba, STATS_COPY, 2, 20102);
    CHECK(drive(&fa) == FILEN_DONE && statsync_merge(&ya, &ba) == 0 && ba.own.s.dir[STATS_COPY].answers == 7,
          "training during a sync was lost");
    CHECK(sync_once(&fa, &ya, &ba) == 0 && fs.uploads == 3, "the training after the sync went up");
    CHECK(sync_once(&fb, &yb, &bb) == 1 && bb.total.dir[STATS_COPY].answers == 7, "the second device caught up");
    {
        int i, versions = 0;
        for (i = 0; i < fs.nfiles; i++)
            versions += fs.files[i].old;
        CHECK(versions == 1, "earlier contents kept as versions: %d", versions);
    }

    /* An import brings a device the cloud does not have: it goes up too. */
    {
        static statsbook_t other;
        static char exp[BOOK_TEXT_CAP];
        statsbook_init(&other, "cc03", "Terminal");
        train(&other, STATS_COPY, 4, 20090);
        statsbook_format(&other, exp, sizeof exp);
        statsbook_import(&bb, exp); /* bb02 empties, cc03 joins */
        /* The first device's section, dropped by the import, comes back. */
        CHECK(sync_once(&fb, &yb, &bb) == 1 && fs.uploads == 5 && bb.total.dir[STATS_COPY].answers == 11,
              "import sync: %d up, %d answers", fs.uploads, bb.total.dir[STATS_COPY].answers);
        CHECK(sync_once(&fa, &ya, &ba) == 1 && ba.others == 2 && ba.total.dir[STATS_COPY].answers == 11 &&
                  ba.total.dir[STATS_SEND].answers == 0,
              "the import reached the first device: %d", ba.total.dir[STATS_COPY].answers);
    }

    /* The folder deleted on filen.io: made again. */
    fs.nfolders = 0;
    n = sync_once(&fa, &ya, &ba);
    CHECK(n >= 0 && fs.nfolders == 2 && fs.bad == 0, "folder made again: %s", fa.error);

    /* The key refused: signed out. */
    strcpy(fs.api_key, "another-key");
    CHECK(sync_once(&fa, &ya, &ba) == -1 && fa.signed_out && strstr(fa.error, "sign in again"), "signed out: %s",
          fa.error);
    /* No connection. */
    statsync_start(&ya, &fa, &ba);
    CHECK(filen_run(&fa) == FILEN_WAIT, "a request");
    filen_reply(&fa, 0, 0);
    CHECK(filen_run(&fa) == FILEN_FAIL && strcmp(fa.error, "No connection to Filen") == 0 && !fa.signed_out,
          "offline: %s", fa.error);

    printf("filen: version 3 (Argon2id, data key, hashed names)\n");
    fake_reset(3, 0);
    filen_init(&fa, STATSYNC_FOLDER);
    filen_login(&fa, "op@example.org", FV_PASSWORD, "");
    CHECK(drive(&fa) == FILEN_FAIL && strstr(fa.error, "memory"), "no work area: %s", fa.error);
    fa.work = work;
    filen_login(&fa, "op@example.org", FV_PASSWORD, "");
    CHECK(drive(&fa) == FILEN_DONE && fa.s.nkeys == 1 && strcmp(fa.s.keys[0], DEK) == 0 &&
              strcmp(fa.s.hmac_key, FV_HMAC_KEY) == 0 && fs.bad == 0,
          "v3 sign in: %s %s", fa.error, fs.last_error);
    filen_name_hash(&fa.s, "stats-v1", hash);
    CHECK(fs.nfolders == 2 && strcmp(fs.folders[1].hash, hash) == 0 && strlen(hash) == 64, "v3 folder names");
    statsbook_init(&ba, "aa01", "Terminal");
    statsync_init(&ya);
    train(&ba, STATS_COPY, 2, 20100);
    CHECK(sync_once(&fa, &ya, &ba) == 0 && fs.uploads == 1 && fs.bad == 0, "v3 upload: %s", fs.last_error);
}

int main(void)
{
    unsigned char seed[32] = {1, 2, 3};
    test_json();
    test_crypto();
    fc_seed(seed, sizeof seed);
    test_protocol();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
