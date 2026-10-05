/*
 * morse.c - The Morse code table and text <-> code helpers.
 */
#include "morse.h"

#include <ctype.h>
#include <string.h>

#include "util.h"

const morse_entry MORSE_TABLE[] = {
    /* Letters */
    {"A", ".-", MG_LETTER, 0, NULL},
    {"B", "-...", MG_LETTER, 0, NULL},
    {"C", "-.-.", MG_LETTER, 0, NULL},
    {"D", "-..", MG_LETTER, 0, NULL},
    {"E", ".", MG_LETTER, 0, NULL},
    {"F", "..-.", MG_LETTER, 0, NULL},
    {"G", "--.", MG_LETTER, 0, NULL},
    {"H", "....", MG_LETTER, 0, NULL},
    {"I", "..", MG_LETTER, 0, NULL},
    {"J", ".---", MG_LETTER, 0, NULL},
    {"K", "-.-", MG_LETTER, 0, NULL},
    {"L", ".-..", MG_LETTER, 0, NULL},
    {"M", "--", MG_LETTER, 0, NULL},
    {"N", "-.", MG_LETTER, 0, NULL},
    {"O", "---", MG_LETTER, 0, NULL},
    {"P", ".--.", MG_LETTER, 0, NULL},
    {"Q", "--.-", MG_LETTER, 0, NULL},
    {"R", ".-.", MG_LETTER, 0, NULL},
    {"S", "...", MG_LETTER, 0, NULL},
    {"T", "-", MG_LETTER, 0, NULL},
    {"U", "..-", MG_LETTER, 0, NULL},
    {"V", "...-", MG_LETTER, 0, NULL},
    {"W", ".--", MG_LETTER, 0, NULL},
    {"X", "-..-", MG_LETTER, 0, NULL},
    {"Y", "-.--", MG_LETTER, 0, NULL},
    {"Z", "--..", MG_LETTER, 0, NULL},
    /* Digits */
    {"0", "-----", MG_DIGIT, 0, NULL},
    {"1", ".----", MG_DIGIT, 0, NULL},
    {"2", "..---", MG_DIGIT, 0, NULL},
    {"3", "...--", MG_DIGIT, 0, NULL},
    {"4", "....-", MG_DIGIT, 0, NULL},
    {"5", ".....", MG_DIGIT, 0, NULL},
    {"6", "-....", MG_DIGIT, 0, NULL},
    {"7", "--...", MG_DIGIT, 0, NULL},
    {"8", "---..", MG_DIGIT, 0, NULL},
    {"9", "----.", MG_DIGIT, 0, NULL},
    /* Punctuation */
    {".", ".-.-.-", MG_PUNCT, 0, "Period"},
    {",", "--..--", MG_PUNCT, 0, "Comma"},
    {"?", "..--..", MG_PUNCT, 0, "Question"},
    {"/", "-..-.", MG_PUNCT, 0, "Slash"},
    {"=", "-...-", MG_PUNCT, 0, "Equals, BT"},
    {"-", "-....-", MG_PUNCT, 0, "Hyphen"},
    {"'", ".----.", MG_PUNCT, 0, "Apostrophe"},
    {"\"", ".-..-.", MG_PUNCT, 0, "Quote"},
    {":", "---...", MG_PUNCT, 0, "Colon"},
    {";", "-.-.-.", MG_PUNCT, 0, "Semicolon"},
    {"!", "-.-.--", MG_PUNCT, 0, "Exclamation"},
    {"(", "-.--.", MG_PUNCT, MF_NODECODE, "Open paren"},
    {")", "-.--.-", MG_PUNCT, 0, "Close paren"},
    {"+", ".-.-.", MG_PUNCT, MF_NODECODE, "Plus, AR"},
    {"&", ".-...", MG_PUNCT, MF_NODECODE, "Ampersand"},
    {"@", ".--.-.", MG_PUNCT, 0, "At sign"},
    {"_", "..--.-", MG_PUNCT, 0, "Underscore"},
    {"$", "...-..-", MG_PUNCT, 0, "Dollar"},
    /* Prosigns */
    {"<AR>", ".-.-.", MG_PROSIGN, 0, "End of message"},
    {"<KN>", "-.--.", MG_PROSIGN, 0, "Over, named station"},
    {"<SK>", "...-.-", MG_PROSIGN, 0, "End of contact"},
    {"<BT>", "-...-", MG_PROSIGN, MF_NODECODE, "Break, new section"},
    {"<AS>", ".-...", MG_PROSIGN, 0, "Wait"},
    {"<KA>", "-.-.-", MG_PROSIGN, 0, "Attention, start"},
    {"<VE>", "...-.", MG_PROSIGN, 0, "Understood"},
    {"<HH>", "........", MG_PROSIGN, 0, "Error"},
    {"<SOS>", "...---...", MG_PROSIGN, 0, "Distress"},
};

const int MORSE_TABLE_COUNT = ARRAY_LEN(MORSE_TABLE);

const morse_entry *morse_decode(const char *code)
{
    int i;
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        if (!(MORSE_TABLE[i].flags & MF_NODECODE) && strcmp(MORSE_TABLE[i].code, code) == 0)
            return &MORSE_TABLE[i];
    }
    return NULL;
}

const morse_entry *morse_lookup(const char *token)
{
    int i;
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        if (str_ieq(MORSE_TABLE[i].text, token))
            return &MORSE_TABLE[i];
    }
    return NULL;
}

int morse_next_token(const char *s, char *tok, size_t cap)
{
    const char *p = s;
    uint32_t cp;
    int n;

    if (cap < 2 || !*s) {
        if (cap)
            tok[0] = 0;
        return 0;
    }
    if (isspace((unsigned char)*p)) {
        while (isspace((unsigned char)*p))
            p++;
        tok[0] = ' ';
        tok[1] = 0;
        return (int)(p - s);
    }
    if (*p == '<') {
        const char *end = strchr(p, '>');
        if (end && (size_t)(end - p + 2) <= cap && end - p <= 8) {
            size_t len = (size_t)(end - p + 1);
            memcpy(tok, p, len);
            tok[len] = 0;
            str_upper(tok);
            if (morse_lookup(tok))
                return (int)len;
        }
    }
    n = utf8_decode(p, &cp);
    if ((size_t)n + 1 > cap)
        n = (int)cap - 1;
    memcpy(tok, p, (size_t)n);
    tok[n] = 0;
    str_upper(tok);
    return n;
}

void morse_pretty(const char *code, char *out, size_t cap)
{
    size_t len = 0;
    if (cap == 0)
        return;
    out[0] = 0;
    for (; *code; code++) {
        const char *g = *code == '.' ? GLYPH_DIT : GLYPH_DAH;
        size_t gl = strlen(g);
        if (len + gl + 1 > cap)
            break;
        memcpy(out + len, g, gl);
        len += gl;
        out[len] = 0;
    }
}

void morse_normalize(const char *in, char *out, size_t cap)
{
    char tok[16];
    size_t len = 0;
    int n;

    if (cap == 0)
        return;
    out[0] = 0;
    while ((n = morse_next_token(in, tok, sizeof tok)) > 0) {
        const char *piece = tok;
        size_t pl;
        in += n;
        if (tok[0] == ' ') {
            if (len == 0 || out[len - 1] == ' ')
                continue;
        } else if (tok[0] == '<' && strlen(tok) > 2 && tok[strlen(tok) - 1] == '>') {
            /* "<AR>" -> "AR" */
            piece = tok + 1;
            tok[strlen(tok) - 1] = 0;
        } else if (strcmp(tok, "+") == 0) {
            piece = "AR";
        } else if (strcmp(tok, "(") == 0) {
            piece = "KN";
        } else if (strcmp(tok, "&") == 0) {
            piece = "AS";
        } else if (strcmp(tok, "=") == 0) {
            piece = "BT";
        }
        pl = strlen(piece);
        if (len + pl + 1 > cap)
            break;
        memcpy(out + len, piece, pl);
        len += pl;
        out[len] = 0;
    }
    while (len > 0 && out[len - 1] == ' ')
        out[--len] = 0;
}
