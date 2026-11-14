/*
 * filen.h - A small client for Filen (filen.io), the end-to-end encrypted
 * cloud where the statistics are kept in sync: sign in (with two-factor
 * codes), keys, folders, and small files of one chunk.
 *
 * Written from the protocol, no I/O: a task prepares a request (f->req),
 * the platform performs it (HTTPS in a thread in the terminal, fetch() in
 * the browser), hands back the reply, and the task goes on:
 *
 *     filen_login(f, email, password, code);
 *     while (filen_run(f) == FILEN_WAIT) {
 *         ... perform f->req, the reply body into f->resp ...
 *         filen_reply(f, status, length);
 *     }
 *     FILEN_DONE, or FILEN_FAIL with f->error.
 *
 * The session keeps what later requests need: the API key, the master keys
 * (or the data key of newer accounts), the HMAC key that names are hashed
 * with (newer accounts) and the folders. Never the password.
 *
 * Accounts of authentication versions 2 and 3; version 1 (before 2021) is
 * not supported.
 */
#ifndef CWTTY_FILEN_H
#define CWTTY_FILEN_H

#include <stdint.h>

#include "json.h"

#define FILEN_KEYS 8
#define FILEN_KEY_CAP 72     /* a master key: 64 characters */
#define FILEN_UUID 37
#define FILEN_BODY_CAP 40960 /* a request body: a chunk of up to 32 KB */
#define FILEN_RESP_CAP 262144
#define FILEN_ARGON2_KIB 65536 /* the work area of version 3 sign-in */

#define FILEN_GATEWAY "https://gateway.filen.io"
#define FILEN_INGEST "https://ingest.filen.io"
#define FILEN_EGEST "https://egest.filen.io"
#define FILEN_APP_FOLDER "CWtty"

enum { FILEN_IDLE, FILEN_WAIT, FILEN_DONE, FILEN_FAIL };
enum { FILEN_GET, FILEN_POST };

typedef struct {
    char email[256];
    char api_key[160];
    int  auth_version;
    char keys[FILEN_KEYS][FILEN_KEY_CAP]; /* the last one encrypts */
    int  nkeys;
    char hmac_key[65]; /* hex, version 3 */
    char base_folder[FILEN_UUID];
    char app_folder[FILEN_UUID]; /* CWtty */
    char data_folder[FILEN_UUID]; /* CWtty/<data_name> */
} filen_session;

typedef struct {
    int                  method;
    char                 url[768];
    char                 auth[192];    /* the Authorization header, "" for none */
    char                 checksum[129]; /* the Checksum header, "" for none */
    int                  json;          /* Content-Type: application/json */
    const unsigned char *body;
    int                  body_len;
} filen_req;

typedef struct filen filen_t;

/* A task: called with f->step, returns FILEN_WAIT with f->req ready, or
 * FILEN_DONE / FILEN_FAIL. */
typedef int (*filen_task)(filen_t *f);

struct filen {
    filen_session s;
    char          data_name[64]; /* the folder of this app's files in CWtty/ */
    double        now_ms;        /* wall clock (ms since 1970) for file dates */

    filen_req     req;
    unsigned char body[FILEN_BODY_CAP];
    char          resp[FILEN_RESP_CAP + 1]; /* the reply, NUL-terminated */
    int           status, resp_len;

    filen_task    task;
    void         *ctx; /* the task's own state (statsync.c) */
    int           step;
    int           state; /* FILEN_IDLE, FILEN_WAIT, FILEN_DONE, FILEN_FAIL */

    char          error[160]; /* why it failed, for people */
    char          code[64];   /* the API's error code */
    int           need_code;  /* the account wants a two-factor code */
    int           signed_out; /* the API key is no longer valid */

    void         *work; /* FILEN_ARGON2_KIB KiB for version 3 sign-in, or NULL */
    void *(*get_work)(void); /* gives it when needed (NULL: none) */

    /* Sign-in in progress (wiped when done). */
    char          l_password[256];
    char          l_code[16];
    char          l_salt[600];
    char          l_derived[160];  /* the password sent to the API: 128 hex digits */
    char          l_folder[FILEN_UUID];     /* a folder being made */
};

void filen_init(filen_t *f, const char *data_name);

/* Starts signing in; code: the two-factor code, "" if none. */
void filen_login(filen_t *f, const char *email, const char *password, const char *code);

/* Starts a task of the caller's (statsync.c) with ctx. */
void filen_start(filen_t *f, filen_task task, void *ctx);

/* Advances the task. */
int  filen_run(filen_t *f);
void filen_reply(filen_t *f, int status, int len);

/* Signed in: a session that can make requests. */
int  filen_signed_in(const filen_t *f);
void filen_sign_out(filen_t *f);

/* The session as text ("key=value" lines) to keep, and back. 0 on
 * success. */
int filen_session_format(const filen_session *s, char *out, int cap);
int filen_session_parse(filen_session *s, const char *text);

/* ---- For tasks ---------------------------------------------------------- */

/* Requests. JSON bodies are written into f->body (jw_init(&w, (char *)
 * f->body, FILEN_BODY_CAP)) first; path starts with '/'. */
void filen_post(filen_t *f, const char *path);
void filen_post_bytes(filen_t *f, const char *url, int len, const char *checksum);
void filen_get(filen_t *f, const char *url, int auth);

/* The reply's envelope: 1 with its data when it says so, else 0 with the
 * error set (and signed_out on a refused key). */
int filen_ok(filen_t *f, json_t *data);
/* Sets the error: FILEN_FAIL. */
int filen_fail(filen_t *f, const char *why);

/* Makes sure CWtty/<data_name> exists, as steps first .. first + 5 of a
 * task: FILEN_WAIT, FILEN_FAIL, or FILEN_DONE once both folders are known
 * (the task then goes on with its own steps). */
#define FILEN_FOLDER_STEPS 6
int filen_folders(filen_t *f, int first);

/* Metadata ("002": the key as a password; "003": a hex key): out holds
 * the encrypted text. -1 if it does not fit. */
int filen_meta_encrypt(const char *key, const char *plain, int n, char *out, int cap);
/* Decrypts with one key, or with each master key in turn: the length. */
int filen_meta_decrypt(const char *key, const char *meta, char *out, int cap);
int filen_meta_open(const filen_session *s, const char *meta, char *out, int cap);

/* The hash a name is looked up by (lower case first). */
void filen_name_hash(const filen_session *s, const char *name, char out[65]);

/* Random text from Filen's 64 characters, a random UUID v4. */
int filen_random_text(char *out, int n);
int filen_uuid(char out[FILEN_UUID]);

/* A file chunk: IV || ciphertext || tag. The key is 32 characters (v2) or
 * 64 hex digits (v3). out holds n + 28 bytes; the length or -1. */
int filen_chunk_encrypt(const char *key, const void *in, int n, unsigned char *out);
int filen_chunk_decrypt(const char *key, const void *in, int n, unsigned char *out);

#endif
