/*
 * wordlists.h - Training material: common English words, Q-codes, CW
 * abbreviations and the Koch character order.
 */
#ifndef CWTTY_WORDLISTS_H
#define CWTTY_WORDLISTS_H

typedef struct {
    const char *code;
    const char *meaning;
} cw_code;

extern const char *const WORDS_EN[];
extern const int         WORDS_EN_COUNT;

extern const cw_code CW_CODES[];
extern const int     CW_CODES_COUNT;

/* Koch method order (LCWO), as single-character tokens. */
extern const char KOCH_ORDER[];

#endif
