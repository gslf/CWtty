/*
 * decoder.h - Adaptive CW timing decoder.
 *
 * Fed with key-down / key-up transitions (from the keyer, a straight key or
 * the receiver's tone detector), it classifies marks into dits and dahs and
 * gaps into element, character and word spaces. Speed is tracked
 * continuously; weighting errors (marks systematically longer or shorter
 * than spaces, typical of filtered receivers) are compensated.
 */
#ifndef CWTTY_DECODER_H
#define CWTTY_DECODER_H

#define DEC_MAX_ELEMS 9
#define DEC_QUEUE 32
#define DEC_HIST 24 /* recent marks / spaces used for speed estimation */

typedef enum { TOK_CHAR, TOK_PROSIGN, TOK_UNKNOWN, TOK_SPACE } dec_kind;

typedef struct {
    int  kind;
    char text[8];  /* "A", "<AR>", "*" or " " */
    char code[12]; /* the received pattern */
} dec_token;

typedef struct {
    double dit_ms, dah_ms, ispace_ms; /* running averages */
    int    have_ispace;
    int    locked;
    double lock_dit_ms;

    int    started;
    int    down;
    double edge_ms; /* time of the last transition */
    char   pat[DEC_MAX_ELEMS + 1];
    int    plen;
    int    after_char;
    int    space_sent;

    double marks[DEC_HIST];  /* recent mark durations (ring) */
    int    nmarks, mhead;
    double spaces[DEC_HIST]; /* recent completed gaps (ring) */
    int    nspaces, shead;

    dec_token q[DEC_QUEUE];
    int       qhead, qlen;
} cw_decoder;

void dec_init(cw_decoder *d, int wpm);
/* Sets the expected speed; locked => fixed timing, no adaptation. */
void dec_set_speed(cw_decoder *d, int wpm, int locked);
/* Drops the character in progress and pending output, and waits for the
 * next transition to re-synchronise (the speed estimate is kept). */
void dec_clear(cw_decoder *d);

void dec_key(cw_decoder *d, int down, double t_ms);
/* Call regularly: emits characters and word spaces on long gaps. */
void dec_update(cw_decoder *d, double now_ms);
int  dec_pop(cw_decoder *d, dec_token *out);

double      dec_wpm(const cw_decoder *d);
double      dec_unit_ms(const cw_decoder *d);
double      dec_ratio(const cw_decoder *d); /* average dah / dit */
const char *dec_pattern(const cw_decoder *d);

#endif
