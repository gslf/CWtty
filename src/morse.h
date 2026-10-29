/*
 * morse.h - The Morse code table and text <-> code helpers.
 *
 * Codes are written with '.' for a dit and '-' for a dah. Prosigns are
 * written as "<AR>", "<SK>", ... and are sent without inter-letter gaps.
 */
#ifndef CWTTY_MORSE_H
#define CWTTY_MORSE_H

#include <stddef.h>

typedef enum { MG_LETTER, MG_DIGIT, MG_PUNCT, MG_PROSIGN, MG_COUNT } morse_group;

enum {
    MF_NODECODE = 1 /* duplicate code: another entry is preferred when decoding */
};

typedef struct {
    const char *text;  /* "A", "?", "<AR>" */
    const char *code;  /* ".-" */
    int         group; /* morse_group */
    int         flags;
    const char *name;  /* human description for punctuation / prosigns */
} morse_entry;

extern const morse_entry MORSE_TABLE[];
extern const int         MORSE_TABLE_COUNT;

/* Display glyphs for one dit and one dah. */
#define GLYPH_DIT "\xE2\x80\xA2" /* U+2022 bullet */
#define GLYPH_DAH "\xE2\x88\x92" /* U+2212 minus  */

/* Pattern -> entry used for decoding (NULL if unknown). */
const morse_entry *morse_decode(const char *code);

/* Text token ("A", "<AR>") -> entry (case-insensitive, NULL if unknown). */
const morse_entry *morse_lookup(const char *token);

/*
 * Reads the next token of s into tok: a single character, a "<XX>"
 * prosign or " " for any run of whitespace. Returns the number of bytes
 * consumed, 0 at the end of the string.
 */
int morse_next_token(const char *s, char *tok, size_t cap);

/* ".-" -> "•−" */
void morse_pretty(const char *code, char *out, size_t cap);

/*
 * Normalises text for comparisons: upper case, prosigns as their letters
 * ("<AR>" and "+" both become "AR"), collapsed spaces, trimmed.
 */
void morse_normalize(const char *in, char *out, size_t cap);

/*
 * Sends text at wpm: calls emit with every key-down and key-up segment, in
 * order. eff_wpm < wpm stretches only the gaps (Farnsworth). Every element
 * is followed by its one-dit space.
 */
typedef void (*morse_emit)(void *ctx, int on, double ms);
void morse_timing(const char *text, int wpm, int eff_wpm, morse_emit emit, void *ctx);

#endif
