/*
 * training.h - Training sessions, independent of the interface: learn to
 * copy by ear (the program sends, you type) and to send (the program
 * shows, you key). Material: single characters (Koch method or fixed
 * sets), common English words, Q-codes and CW abbreviations. Characters
 * you miss come back more often.
 *
 * Calls that move a session forward return TRAIN_* flags: what the
 * interface has to play, reset or tell the student.
 */
#ifndef CWTTY_TRAINING_H
#define CWTTY_TRAINING_H

#include <stddef.h>

#include "decoder.h"
#include "settings.h"

enum { TRAIN_COPY = 0, TRAIN_SEND = 1 };
enum { TRAIN_CHARS, TRAIN_WORDS, TRAIN_CODES };
enum { CHARSET_KOCH, CHARSET_LETTERS, CHARSET_DIGITS, CHARSET_PUNCT, CHARSET_ALNUM, CHARSET_ALL };
enum { TS_SETUP, TS_LISTEN, TS_RESULT, TS_SEND, TS_SEND_RESULT };

enum {
    TRAIN_PLAY = 1,   /* copy: play the target from the start */
    TRAIN_RESEND = 2, /* send: clear the decoder, sending starts over */
    TRAIN_WRONG = 4,  /* wrong answer: the same item starts again */
    TRAIN_KOCH = 8    /* 90% correct: ready for the next Koch lesson */
};

typedef struct {
    int         state;
    int         sel;          /* selected row in the setup form */
    char        target[48];   /* the item to copy or to send */
    const char *meaning;      /* explanation of a Q-code / abbreviation */
    char        typed[48];    /* receive: what the student typed */
    int         ntyped;
    char        sent[96];     /* send: what the decoder read */
    int         ok;           /* result of the last item */
    int         total, correct, streak, best;
    int         err[128], seen[128]; /* per character statistics */
    unsigned char hist[32];          /* recent results, 1 = correct */
    int         nhist;
    int         last_pick;
    double      send_wpm, send_ratio; /* timing of the last sent item */
} training_t;

extern const char *const TRAIN_CHARSET_NAMES[]; /* CHARSET_* */
extern const char *const TRAIN_CONTENT_NAMES[]; /* TRAIN_CHARS, ... */

/* The characters of the chosen set ("KMURES" for Koch lesson 6). */
void training_charset(const settings_t *c, char *out, size_t cap);

/* A new session, the next item, the same item again. */
int training_start(training_t *t, const settings_t *c);
int training_next(training_t *t, const settings_t *c);
int training_retry(training_t *t, const settings_t *c);

/* Enter: checks the answer, or moves on from a result. */
int training_enter(training_t *t, const settings_t *c, const cw_decoder *dec);

/* Copy: a typed character, checked as soon as it is wrong or complete. */
int training_type(training_t *t, const settings_t *c, int ch);

/* Backspace: deletes a typed character (copy), starts again (send). */
int training_backspace(training_t *t, const settings_t *c);

/* Send: a character read by the decoder of the operator's keying. */
int training_sent(training_t *t, const settings_t *c, const dec_token *tok, const cw_decoder *dec);

/* Send: the operator touched the key. */
int training_pressed(training_t *t, const settings_t *c);

/* The session takes keying from the operator. */
int training_keying(const training_t *t, const settings_t *c);

/* The characters missed most (up to cap). Returns the count. */
int training_weak(const training_t *t, char *out, int cap);

#endif
