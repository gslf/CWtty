/*
 * abbr.h - The words of CW procedure, to show them apart from the text in
 * the transcripts:
 *
 *   signs           pass the turn or shape the message: K, KN, BK, AR, SK,
 *                   BT (=) and the prosigns;
 *   abbreviations   Q-codes (QRL, QTH, QSL...), CQ, DE, TU, 73, 599, 5NN
 *                   and the other usual ones.
 *
 * Shared by the terminal and the web app, so both colour the same words.
 */
#ifndef CWTTY_ABBR_H
#define CWTTY_ABBR_H

enum { WORD_TEXT, WORD_ABBR, WORD_SIGN };

/* Classifies a decoded word of n bytes (upper case). A trailing ? . , or !
 * is ignored: QRL? is a Q-code. */
int abbr_word(const char *word, int n);

#endif
