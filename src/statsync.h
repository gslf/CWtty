/*
 * statsync.h - The statistics in the cloud: on Filen, a file per device in
 * CWtty/stats-v1, "<id>.stats", holding that device's section of the book
 * (statsbook.h) in its text form.
 *
 * A sync works on a copy of the book (training goes on meanwhile):
 *
 *   1. lists the folder;
 *   2. downloads the files that changed since the last sync (all of them
 *      the first time) and takes the sections newer than the copy's;
 *   3. uploads the sections of the copy newer than the cloud's: this
 *      device's, and those an import brought;
 *
 * then statsync_merge() hands the newer sections to the live book. Filen
 * keeps a file's earlier contents as versions.
 */
#ifndef CWTTY_STATSYNC_H
#define CWTTY_STATSYNC_H

#include "filen.h"
#include "statsbook.h"

#define STATSYNC_FOLDER "stats-v1"
#define STATSYNC_FILES (BOOK_OTHERS + 4)

typedef struct {
    char      id[BOOK_ID + 1];
    char      uuid[FILEN_UUID]; /* in the listing */
    char      key[FILEN_KEY_CAP];
    char      region[40], bucket[80];
    long long ts;
    char      read_uuid[FILEN_UUID]; /* the contents known: downloaded or uploaded */
    int       rev;                   /* their revision */
} sync_file;

typedef struct {
    statsbook_t book; /* the copy being synced */
    sync_file   files[STATSYNC_FILES];
    int         nfiles;
    int         cursor;
    int         retried;  /* the folder was made again once */
    int         got, sent; /* sections downloaded and uploaded in this sync */

    /* The upload in progress. */
    char up_uuid[FILEN_UUID], up_key[33], up_rm[33], up_upload_key[33];
    char up_id[BOOK_ID + 1], up_hash[129];
    int  up_len, up_rev;
} statsync_t;

/* Forgets what the cloud held (a new account). */
void statsync_init(statsync_t *y);

/* Starts a sync of a copy of book on f (signed in). */
void statsync_start(statsync_t *y, filen_t *f, const statsbook_t *book);

/* Sections of book newer than the cloud's, as last seen. */
int statsync_pending(const statsync_t *y, const statsbook_t *book);

/* After FILEN_DONE: the sections the sync found newer go into the live
 * book. 1 if it changed. */
int statsync_merge(statsync_t *y, statsbook_t *live);

#endif
