/*
 * abbr.c - The words of CW procedure.
 */
#include "abbr.h"

#include <string.h>

/* The turn and the shape of a message (see also over.c). */
static const char *const SIGNS[] = {"K", "KN", "BK", "AR", "SK", "BT", "=", "+", "AS", "CL"};

/* The usual abbreviations of a CW contact. Q-codes and signal reports are
 * recognised by their form. */
static const char *const ABBRS[] = {
    "CQ",   "DE",  "TU",   "TNX", "TKS",  "THX", "PSE", "PLS",  "R",    "RR",  "RRR", "FB",  "OM",  "YL",
    "XYL",  "OP",  "UR",   "URS", "RST",  "HR",  "HW",  "WX",   "ES",   "GM",  "GA",  "GE",  "GN",  "GL",
    "GB",   "CU",  "CUL",  "CFM", "AGN",  "ANT", "RIG", "PWR",  "NR",   "SRI", "ABT", "BURO", "BCNU", "DX",
    "FER",  "OB",  "OT",   "RPT", "SIG",  "SIGS", "WKD", "WPM", "NIL",  "NW",  "VY",  "HPE", "BTU", "INFO",
    "73",   "88",  "72",   "55",  "HI",   "GD",  "DR",  "FM",   "WID",  "HV",  "RX",  "TX",  "TRX", "CONDX",
    "MNI",  "TEST", "ENUF", "AA",
};

static int in(const char *const *list, int n, const char *w)
{
    int i;
    for (i = 0; i < n; i++)
        if (strcmp(list[i], w) == 0)
            return 1;
    return 0;
}

static int upper(char c)
{
    return c >= 'A' && c <= 'Z';
}

int abbr_word(const char *word, int n)
{
    char w[16];
    if (n <= 0 || n >= (int)sizeof w)
        return WORD_TEXT;
    memcpy(w, word, (size_t)n);
    while (n > 1 && (w[n - 1] == '?' || w[n - 1] == '.' || w[n - 1] == ',' || w[n - 1] == '!'))
        n--;
    w[n] = 0;
    if (w[0] == '<' && w[n - 1] == '>')
        return WORD_SIGN; /* a prosign: <AR>, <SK>... */
    if (in(SIGNS, (int)(sizeof SIGNS / sizeof SIGNS[0]), w))
        return WORD_SIGN;
    /* Q-codes: Q and two letters, QU... left out (QUI and QUA are words). */
    if (n == 3 && w[0] == 'Q' && upper(w[1]) && w[1] != 'U' && upper(w[2]))
        return WORD_ABBR;
    /* Signal reports: 599, 579, 5NN. */
    if (n == 3 && w[0] >= '1' && w[0] <= '5' && ((w[1] >= '1' && w[1] <= '9') || w[1] == 'N') &&
        ((w[2] >= '1' && w[2] <= '9') || w[2] == 'N'))
        return WORD_ABBR;
    return in(ABBRS, (int)(sizeof ABBRS / sizeof ABBRS[0]), w) ? WORD_ABBR : WORD_TEXT;
}
