/*
 * statsync.c - The statistics in the cloud.
 */
#include "statsync.h"

#include <string.h>

#include "fcrypto.h"
#include "json.h"
#include "util.h"

#define MIME "text/plain"

enum {
    S_FOLDERS = 0, /* .. FILEN_FOLDER_STEPS - 1 */
    S_LIST = 10,
    S_LISTED,
    S_DOWNLOAD,
    S_DOWNLOADED,
    S_UPLOAD = 20,
    S_UPLOADED,
    S_DONE_SENT,
};

void statsync_init(statsync_t *y)
{
    memset(y, 0, sizeof *y);
}

static sync_file *file_of(statsync_t *y, const char *id)
{
    int i;
    for (i = 0; i < y->nfiles; i++)
        if (strcmp(y->files[i].id, id) == 0)
            return &y->files[i];
    return NULL;
}

/* The device id of "<id>.stats", or 0. */
static int stats_name(const char *name, char id[BOOK_ID + 1])
{
    const char *dot = strrchr(name, '.');
    int n = dot ? (int)(dot - name) : 0;
    if (n <= 0 || n > BOOK_ID || strcmp(dot, ".stats") != 0)
        return 0;
    memcpy(id, name, (size_t)n);
    id[n] = 0;
    return statsbook_valid_id(id);
}

/* The folder's files: what each is, from its encrypted metadata. */
static void read_listing(statsync_t *y, filen_t *f, json_t data)
{
    static char meta[4096], plain[4096];
    json_t uploads, it = {NULL, 0}, v;
    int i, k;
    for (i = 0; i < y->nfiles; i++)
        y->files[i].uuid[0] = 0; /* not listed until seen */
    if (!json_get(data, "uploads", &uploads))
        return;
    while (json_next(uploads, &it)) {
        char name[64], id[BOOK_ID + 1], uuid[FILEN_UUID];
        long long ts = 0;
        sync_file *sf;
        json_t m;
        if (json_get_str(it, "metadata", meta, sizeof meta) <= 0 || json_get_str(it, "uuid", uuid, sizeof uuid) <= 0 ||
            filen_meta_open(&f->s, meta, plain, sizeof plain) < 0)
            continue;
        m = json_doc(plain, -1);
        if (json_get_str(m, "name", name, sizeof name) <= 0 || !stats_name(name, id))
            continue;
        if (json_get(it, "timestamp", &v))
            json_int(v, &ts);
        sf = file_of(y, id);
        if (!sf) {
            /* Room for it, dropping a file no longer listed if needed. */
            if (y->nfiles < STATSYNC_FILES) {
                sf = &y->files[y->nfiles++];
            } else {
                for (k = 0; k < y->nfiles && y->files[k].uuid[0]; k++) {
                }
                if (k == y->nfiles)
                    continue;
                sf = &y->files[k];
            }
            memset(sf, 0, sizeof *sf);
            str_copy(sf->id, sizeof sf->id, id);
            sf->rev = -1;
        } else if (sf->uuid[0] && ts < sf->ts) {
            continue; /* two of them: the newest counts */
        }
        str_copy(sf->uuid, sizeof sf->uuid, uuid);
        sf->ts = ts;
        json_get_str(m, "key", sf->key, sizeof sf->key);
        json_get_str(it, "region", sf->region, sizeof sf->region);
        json_get_str(it, "bucket", sf->bucket, sizeof sf->bucket);
    }
}

static int must_read(const statsync_t *y, const sync_file *sf)
{
    const book_section *sec;
    if (!sf->uuid[0])
        return 0;
    if (strcmp(sf->uuid, sf->read_uuid) != 0)
        return 1;
    sec = statsbook_find(&y->book, sf->id);
    return !sec || sec->rev < sf->rev;
}

/* The section to upload at cursor (0 this device, then the others), or
 * NULL past the last. */
static const book_section *section_at(const statsync_t *y, int i)
{
    if (i == 0)
        return &y->book.own;
    return i - 1 < y->book.others ? &y->book.other[i - 1] : NULL;
}

/* Encrypts the section into f->body and asks for its upload. */
static int upload_start(statsync_t *y, filen_t *f, const book_section *sec)
{
    static char text[STATS_TEXT_CAP + 256];
    uint8_t h[64];
    char url[640], params[512], sum[129];
    jw_t w;
    int n = statsbook_section_text(sec, text, sizeof text), len;
    if (n < 0 || filen_uuid(y->up_uuid) != 0 || filen_random_text(y->up_key, 32) != 0 ||
        filen_random_text(y->up_rm, 32) != 0 || filen_random_text(y->up_upload_key, 32) != 0)
        return filen_fail(f, "Could not prepare the upload");
    fc_sha512(text, (size_t)n, h);
    fc_hex(h, sizeof h, y->up_hash);
    y->up_len = n;
    y->up_rev = sec->rev;
    str_copy(y->up_id, sizeof y->up_id, sec->id);
    if (n + 28 > FILEN_BODY_CAP || (len = filen_chunk_encrypt(y->up_key, text, n, f->body)) < 0)
        return filen_fail(f, "Could not prepare the upload");
    fc_sha512(f->body, (size_t)len, h);
    fc_hex(h, sizeof h, sum);

    /* The Checksum is of the query's parameters, as JSON. */
    jw_init(&w, params, sizeof params);
    jw_open(&w, '{');
    jw_key(&w, "uuid");
    jw_str(&w, y->up_uuid);
    jw_key(&w, "index");
    jw_str(&w, "0");
    jw_key(&w, "parent");
    jw_str(&w, f->s.data_folder);
    jw_key(&w, "uploadKey");
    jw_str(&w, y->up_upload_key);
    jw_key(&w, "hash");
    jw_str(&w, sum);
    jw_close(&w, '}');
    jw_end(&w);
    str_copy(url, sizeof url, FILEN_INGEST "/v3/upload?uuid=");
    strcat(url, y->up_uuid);
    strcat(url, "&index=0&parent=");
    strcat(url, f->s.data_folder);
    strcat(url, "&uploadKey=");
    strcat(url, y->up_upload_key);
    strcat(url, "&hash=");
    strcat(url, sum);
    fc_sha512(params, strlen(params), h);
    fc_hex(h, sizeof h, sum);
    filen_post_bytes(f, url, len, sum);
    return FILEN_WAIT;
}

/* The file's name, size and type, and what clients read first. */
static int upload_done(statsync_t *y, filen_t *f)
{
    static char meta[1024], enc_meta[2048];
    char name[32], size[16], enc_name[128], enc_size[96], enc_mime[96], hash[65];
    jw_t w;
    int n, k;
    long long now = (long long)f->now_ms;
    str_copy(name, sizeof name, y->up_id);
    strcat(name, ".stats");
    for (k = 0, n = y->up_len; k == 0 || n; n /= 10)
        size[k++] = (char)('0' + n % 10);
    size[k] = 0;
    for (n = 0; n < k / 2; n++) { /* the digits came out reversed */
        char c = size[n];
        size[n] = size[k - 1 - n];
        size[k - 1 - n] = c;
    }
    jw_init(&w, meta, sizeof meta);
    jw_open(&w, '{');
    jw_key(&w, "name");
    jw_str(&w, name);
    jw_key(&w, "size");
    jw_int(&w, y->up_len);
    jw_key(&w, "mime");
    jw_str(&w, MIME);
    jw_key(&w, "key");
    jw_str(&w, y->up_key);
    jw_key(&w, "lastModified");
    jw_int(&w, now);
    jw_key(&w, "creation");
    jw_int(&w, now);
    jw_key(&w, "hash");
    jw_str(&w, y->up_hash);
    jw_close(&w, '}');
    if ((n = jw_end(&w)) < 0 ||
        filen_meta_encrypt(f->s.keys[f->s.nkeys - 1], meta, n, enc_meta, sizeof enc_meta) < 0 ||
        filen_meta_encrypt(y->up_key, name, (int)strlen(name), enc_name, sizeof enc_name) < 0 ||
        filen_meta_encrypt(y->up_key, size, (int)strlen(size), enc_size, sizeof enc_size) < 0 ||
        filen_meta_encrypt(y->up_key, MIME, (int)strlen(MIME), enc_mime, sizeof enc_mime) < 0)
        return filen_fail(f, "Could not prepare the upload");
    filen_name_hash(&f->s, name, hash);
    jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
    jw_open(&w, '{');
    jw_key(&w, "uuid");
    jw_str(&w, y->up_uuid);
    jw_key(&w, "name");
    jw_str(&w, enc_name);
    jw_key(&w, "nameHashed");
    jw_str(&w, hash);
    jw_key(&w, "size");
    jw_str(&w, enc_size);
    jw_key(&w, "chunks");
    jw_int(&w, 1);
    jw_key(&w, "mime");
    jw_str(&w, enc_mime);
    jw_key(&w, "rm");
    jw_str(&w, y->up_rm);
    jw_key(&w, "metadata");
    jw_str(&w, enc_meta);
    jw_key(&w, "version");
    jw_int(&w, 2);
    jw_key(&w, "uploadKey");
    jw_str(&w, y->up_upload_key);
    jw_close(&w, '}');
    if (jw_end(&w) < 0)
        return filen_fail(f, "Could not prepare the upload");
    filen_post(f, "/v3/upload/done");
    return FILEN_WAIT;
}

static int sync_task(filen_t *f)
{
    static unsigned char plain[FILEN_RESP_CAP];
    statsync_t *y = (statsync_t *)f->ctx;
    json_t d;
    int r;

    for (;;) {
        if (f->step < FILEN_FOLDER_STEPS) {
            r = filen_folders(f, S_FOLDERS);
            if (r != FILEN_DONE)
                return r;
            f->step = S_LIST;
        }
        switch (f->step) {
        case S_LIST:
            {
                jw_t w;
                jw_init(&w, (char *)f->body, FILEN_BODY_CAP);
                jw_open(&w, '{');
                jw_key(&w, "uuid");
                jw_str(&w, f->s.data_folder);
                jw_close(&w, '}');
                jw_end(&w);
            }
            filen_post(f, "/v3/dir/content");
            f->step = S_LISTED;
            return FILEN_WAIT;

        case S_LISTED:
            if (!filen_ok(f, &d)) {
                /* The folder went (deleted on filen.io): make it again, once. */
                if (!f->signed_out && f->status != 0 && !y->retried) {
                    y->retried = 1;
                    f->s.app_folder[0] = f->s.data_folder[0] = 0;
                    f->error[0] = 0;
                    f->step = S_FOLDERS;
                    continue;
                }
                return FILEN_FAIL;
            }
            read_listing(y, f, d);
            y->cursor = 0;
            f->step = S_DOWNLOAD;
            continue;

        case S_DOWNLOAD:
            /* The next file whose contents are not known, or newer than the
             * book's copy (an import may have replaced it). */
            while (y->cursor < y->nfiles && !must_read(y, &y->files[y->cursor]))
                y->cursor++;
            if (y->cursor >= y->nfiles) {
                y->cursor = 0;
                f->step = S_UPLOAD;
                continue;
            }
            {
                sync_file *sf = &y->files[y->cursor];
                char url[512];
                str_copy(url, sizeof url, FILEN_EGEST "/");
                strcat(url, sf->region);
                strcat(url, "/");
                strcat(url, sf->bucket);
                strcat(url, "/");
                strcat(url, sf->uuid);
                strcat(url, "/0");
                filen_get(f, url, 0);
            }
            f->step = S_DOWNLOADED;
            return FILEN_WAIT;

        case S_DOWNLOADED:
            {
                sync_file *sf = &y->files[y->cursor];
                char id[BOOK_ID + 1];
                int n, rev;
                if (f->status != 200)
                    return filen_fail(f, f->status ? "Could not download the statistics" : "No connection to Filen");
                n = filen_chunk_decrypt(sf->key, f->resp, f->resp_len, plain);
                if (n < 0)
                    return filen_fail(f, "Could not read the statistics in the cloud");
                plain[n] = 0;
                if (statsbook_header((const char *)plain, id, &rev) == 0 && strcmp(id, sf->id) == 0) {
                    if (statsbook_adopt(&y->book, (const char *)plain) == 1)
                        y->got++;
                    sf->rev = rev;
                }
                str_copy(sf->read_uuid, sizeof sf->read_uuid, sf->uuid);
                y->cursor++;
            }
            f->step = S_DOWNLOAD;
            continue;

        case S_UPLOAD:
            /* The next section newer than the cloud's copy. */
            for (;; y->cursor++) {
                const book_section *sec = section_at(y, y->cursor);
                sync_file *sf;
                if (!sec)
                    return FILEN_DONE;
                sf = file_of(y, sec->id);
                if (sec->rev > 0 && (!sf || !sf->uuid[0] || sec->rev > sf->rev))
                    break;
            }
            f->step = S_UPLOADED;
            return upload_start(y, f, section_at(y, y->cursor));

        case S_UPLOADED:
            if (!filen_ok(f, &d))
                return FILEN_FAIL;
            f->step = S_DONE_SENT;
            return upload_done(y, f);

        case S_DONE_SENT:
            if (!filen_ok(f, &d))
                return FILEN_FAIL;
            {
                /* What the cloud now holds for that device. */
                sync_file *sf = file_of(y, y->up_id);
                if (!sf && y->nfiles < STATSYNC_FILES) {
                    sf = &y->files[y->nfiles++];
                    memset(sf, 0, sizeof *sf);
                    str_copy(sf->id, sizeof sf->id, y->up_id);
                }
                if (sf) {
                    str_copy(sf->uuid, sizeof sf->uuid, y->up_uuid);
                    str_copy(sf->read_uuid, sizeof sf->read_uuid, y->up_uuid);
                    str_copy(sf->key, sizeof sf->key, y->up_key);
                    sf->rev = y->up_rev;
                }
                y->sent++;
                y->cursor++;
            }
            f->step = S_UPLOAD;
            continue;

        default:
            return filen_fail(f, "Sync went astray");
        }
    }
}

void statsync_start(statsync_t *y, filen_t *f, const statsbook_t *book)
{
    y->book = *book;
    y->cursor = y->retried = y->got = y->sent = 0;
    filen_start(f, sync_task, y);
    str_copy(f->data_name, sizeof f->data_name, STATSYNC_FOLDER);
}

int statsync_pending(const statsync_t *y, const statsbook_t *book)
{
    const book_section *sec;
    int i;
    for (i = 0; (sec = i == 0 ? &book->own : (i - 1 < book->others ? &book->other[i - 1] : NULL)) != NULL; i++) {
        const sync_file *sf = NULL;
        int k;
        for (k = 0; k < y->nfiles; k++)
            if (strcmp(y->files[k].id, sec->id) == 0)
                sf = &y->files[k];
        if (sec->rev > 0 && (!sf || sec->rev > sf->rev))
            return 1;
    }
    return 0;
}

int statsync_merge(statsync_t *y, statsbook_t *live)
{
    int changed = 0, i;
    changed |= statsbook_take(live, &y->book.own);
    for (i = 0; i < y->book.others; i++)
        changed |= statsbook_take(live, &y->book.other[i]);
    return changed;
}
