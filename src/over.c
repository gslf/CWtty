/*
 * over.c - Where a transmission ends in the text of an online room.
 */
#include "over.h"

#include <string.h>

void over_reset(over_t *o)
{
    o->len = 0;
    o->bad = 0;
    o->word[0] = 0;
}

int over_token(over_t *o, const dec_token *t)
{
    int r = OVER_NONE;
    switch (t->kind) {
    case TOK_SPACE:
        if (!o->bad && o->len > 0) {
            o->word[o->len] = 0;
            if (strcmp(o->word, "K") == 0 || strcmp(o->word, "KN") == 0 || strcmp(o->word, "BK") == 0 ||
                strcmp(o->word, "AR") == 0 || strcmp(o->word, "SK") == 0)
                r = OVER_END;
            else if (strcmp(o->word, "=") == 0)
                r = OVER_BREAK;
        }
        over_reset(o);
        break;
    case TOK_PROSIGN:
        if (strcmp(t->text, "<AR>") == 0 || strcmp(t->text, "<KN>") == 0 || strcmp(t->text, "<SK>") == 0) {
            over_reset(o);
            r = OVER_END;
        } else if (strcmp(t->text, "<BT>") == 0) {
            over_reset(o);
            r = OVER_BREAK;
        } else {
            o->bad = 1;
        }
        break;
    case TOK_CHAR: {
        size_t n = strlen(t->text);
        if (o->len + (int)n < (int)sizeof o->word) {
            memcpy(o->word + o->len, t->text, n);
            o->len += (int)n;
        } else {
            o->bad = 1;
        }
        break;
    }
    default:
        o->bad = 1;
        break;
    }
    return r;
}
