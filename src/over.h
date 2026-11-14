/*
 * over.h - Where a transmission ends, read from the decoded text of an
 * online room, as on the air:
 *
 *   K, KN, BK   over (to anyone, to the named station, back to you)
 *   AR, SK      end of the message, end of the contact
 *   BT (=)      a new paragraph
 *
 * Letters count only as a word of their own, so a callsign like K1ABC or
 * the word OK end nothing. The prosigns AR, KN and SK end the transmission
 * wherever they are.
 */
#ifndef CWTTY_OVER_H
#define CWTTY_OVER_H

#include "decoder.h"

enum { OVER_NONE, OVER_END, OVER_BREAK };

typedef struct {
    char word[8];
    int  len, bad;
} over_t;

void over_reset(over_t *o);

/* Feed each decoded token of a station; returns OVER_END when its turn
 * passes, OVER_BREAK when a new paragraph starts, else OVER_NONE. */
int over_token(over_t *o, const dec_token *t);

#endif
