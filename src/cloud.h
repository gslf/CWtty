/*
 * cloud.h - The terminal app's side of the statistics sync with Filen
 * (filen.h, statsync.h): the session kept in a private file (cwtty.filen
 * next to the settings, 0600, never the password) and a worker thread that
 * signs in and syncs over HTTPS while the interface goes on.
 */
#ifndef CWTTY_CLOUD_H
#define CWTTY_CLOUD_H

#include "statsbook.h"

enum { CLOUD_OFF, CLOUD_READY, CLOUD_BUSY };

/* What cloud_poll() found finished. */
enum {
    CLOUD_SIGNED_IN = 1,  /* the sign-in worked: a sync follows */
    CLOUD_LOGIN_FAILED = 2,
    CLOUD_SYNCED = 4,
    CLOUD_SYNC_FAILED = 8,
    CLOUD_CHANGED = 16,   /* the sync changed the book: save it */
    CLOUD_SIGNED_OUT = 32 /* Filen refused the session: signed out */
};

/* Loads the session (if any) and starts the worker. 1 if signed in. */
int cloud_start(const char *session_path);

int         cloud_state(void);
const char *cloud_email(void);
const char *cloud_error(void);   /* the last failure, "" if none */
int         cloud_need_code(void); /* the last sign-in asked for a 2FA code */
const char *cloud_synced(void);  /* "10:42" of the last sync, "" if none */

/* Starts signing in (CLOUD_OFF) or a sync of a copy of book (CLOUD_READY). */
void cloud_login(const char *email, const char *password, const char *code);
void cloud_sync(const statsbook_t *book);

/* Each tick: what finished (CLOUD_* flags), the results merged into book. */
int cloud_poll(statsbook_t *book);

/* Forgets the session (and the file). */
void cloud_sign_out(void);

/* Quitting: one last sync of book if signed in, waiting up to ms. */
void cloud_finish(const statsbook_t *book, int ms);

#endif
