/*
 * training.c - Training sessions, independent of the interface.
 */
#include "training.h"

#include <string.h>

#include "morse.h"
#include "util.h"
#include "wordlists.h"

const char *const TRAIN_CHARSET_NAMES[] = {"Koch method", "Letters", "Numbers", "Punctuation",
                                           "Letters & numbers", "Everything"};
const char *const TRAIN_CONTENT_NAMES[] = {"Characters", "English words", "Q-codes & abbreviations"};

static const char LETTERS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char DIGITS[] = "0123456789";
static const char PUNCT[] = ".,?/=-()'\":;!@";

/* ------------------------------------------------------------------------ */
/* Material                                                                  */
/* ------------------------------------------------------------------------ */

static void append(char *out, size_t cap, const char *s)
{
    size_t n = strlen(out);
    str_copy(out + n, cap - n, s);
}

void training_charset(const settings_t *c, char *out, size_t cap)
{
    out[0] = 0;
    switch (c->train_charset) {
    case CHARSET_KOCH: {
        int n = clampi(c->train_koch, 2, (int)strlen(KOCH_ORDER));
        str_copy(out, (size_t)mini((int)cap, n + 1), KOCH_ORDER);
        break;
    }
    case CHARSET_LETTERS:
        str_copy(out, cap, LETTERS);
        break;
    case CHARSET_DIGITS:
        str_copy(out, cap, DIGITS);
        break;
    case CHARSET_PUNCT:
        str_copy(out, cap, PUNCT);
        break;
    case CHARSET_ALNUM:
        str_copy(out, cap, LETTERS);
        append(out, cap, DIGITS);
        break;
    default:
        str_copy(out, cap, LETTERS);
        append(out, cap, DIGITS);
        append(out, cap, PUNCT);
        break;
    }
}

/* Weighted pick: characters you get wrong come back more often, and in a
 * Koch lesson the newest character is favoured. */
static char pick_char(const training_t *t, const char *set, int koch)
{
    double w[64], total = 0, r;
    int n = (int)strlen(set), i;
    if (n > 64)
        n = 64;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)set[i];
        w[i] = 1.0 + 4.0 * (double)t->err[c & 127] / (double)(t->seen[c & 127] + 2);
        if (koch && i == n - 1)
            w[i] *= 2.0;
        total += w[i];
    }
    r = (double)rng_u32() / 4294967296.0 * total;
    for (i = 0; i < n; i++) {
        r -= w[i];
        if (r <= 0)
            return set[i];
    }
    return set[n - 1];
}

int training_retry(training_t *t, const settings_t *c)
{
    if (c->train_dir == TRAIN_COPY) {
        t->typed[0] = 0;
        t->ntyped = 0;
        t->state = TS_LISTEN;
        return TRAIN_PLAY;
    }
    t->sent[0] = 0;
    t->state = TS_SEND;
    return TRAIN_RESEND;
}

int training_next(training_t *t, const settings_t *c)
{
    int idx, tries;

    t->meaning = NULL;
    switch (c->train_content) {
    case TRAIN_CHARS: {
        char set[96];
        int g = clampi(c->train_group, 1, 5), i;
        training_charset(c, set, sizeof set);
        for (i = 0; i < g; i++)
            t->target[i] = pick_char(t, set, c->train_charset == CHARSET_KOCH);
        t->target[g] = 0;
        break;
    }
    case TRAIN_WORDS:
        for (tries = 0, idx = rng_int(WORDS_EN_COUNT); idx == t->last_pick && tries < 4; tries++)
            idx = rng_int(WORDS_EN_COUNT);
        t->last_pick = idx;
        str_copy(t->target, sizeof t->target, WORDS_EN[idx]);
        break;
    default:
        for (tries = 0, idx = rng_int(CW_CODES_COUNT); idx == t->last_pick && tries < 4; tries++)
            idx = rng_int(CW_CODES_COUNT);
        t->last_pick = idx;
        str_copy(t->target, sizeof t->target, CW_CODES[idx].code);
        t->meaning = CW_CODES[idx].meaning;
        break;
    }
    t->typed[0] = 0;
    t->ntyped = 0;
    t->sent[0] = 0;
    return training_retry(t, c);
}

int training_start(training_t *t, const settings_t *c)
{
    t->total = t->correct = t->streak = t->best = 0;
    t->nhist = 0;
    t->last_pick = -1;
    return training_next(t, c);
}

/* ------------------------------------------------------------------------ */
/* Checking                                                                  */
/* ------------------------------------------------------------------------ */

/* Splits text into tokens ("A", "<AR>"), skipping spaces. */
static int split_tokens(const char *s, char (*out)[16], int cap)
{
    char tok[16];
    int n, k = 0;
    while (k < cap && (n = morse_next_token(s, tok, sizeof tok)) > 0) {
        s += n;
        if (tok[0] != ' ')
            str_copy(out[k++], 16, tok);
    }
    return k;
}

static int record(training_t *t, const settings_t *c, const char *expect, const char *got, int ok)
{
    char te[48][16], tg[48][16];
    int ne = split_tokens(expect, te, 48), ng = split_tokens(got, tg, 48), i;
    /* An early error says nothing about the characters not attempted. */
    if (!ok && ng < ne)
        ne = ng;
    /* Statistics per character of the target, compared token by token so
     * that "=" counts as "=" (not as the letters B and T). */
    for (i = 0; i < ne; i++) {
        char a1[16], b1[16];
        unsigned char ch = (unsigned char)te[i][0];
        if (te[i][1] != 0)
            continue; /* prosigns and multi-letter tokens: no per-character stats */
        morse_normalize(te[i], a1, sizeof a1);
        b1[0] = 0;
        if (i < ng)
            morse_normalize(tg[i], b1, sizeof b1);
        t->seen[ch & 127]++;
        if (strcmp(a1, b1) != 0)
            t->err[ch & 127]++;
        else if (t->err[ch & 127] > 0 && ok)
            t->err[ch & 127]--; /* forgive slowly when you get it right */
    }
    t->total++;
    if (ok) {
        t->correct++;
        t->streak++;
        if (t->streak > t->best)
            t->best = t->streak;
    } else {
        t->streak = 0;
    }
    memmove(t->hist + 1, t->hist, sizeof t->hist - 1);
    t->hist[0] = (unsigned char)ok;
    if (t->nhist < (int)sizeof t->hist)
        t->nhist++;
    t->ok = ok;

    /* Koch method: suggest the next lesson at 90% over 20+ items. */
    if (c->train_content == TRAIN_CHARS && c->train_charset == CHARSET_KOCH && t->total >= 20 &&
        t->correct * 10 >= t->total * 9 && t->total % 10 == 0 && c->train_koch < (int)strlen(KOCH_ORDER))
        return TRAIN_KOCH;
    return 0;
}

static int same_text(const char *x, const char *y)
{
    char nx[96], ny[96];
    morse_normalize(x, nx, sizeof nx);
    morse_normalize(y, ny, sizeof ny);
    return strcmp(nx, ny) == 0;
}

static int check_copy(training_t *t, const settings_t *c)
{
    int fx = record(t, c, t->target, t->typed, same_text(t->target, t->typed));
    if (t->ok) {
        t->state = TS_RESULT;
        return fx;
    }
    return fx | TRAIN_WRONG | training_retry(t, c);
}

/* Compare normalized prefixes, allowing a prosign still being typed ("<A"). */
static int copy_prefix_matches(const training_t *t)
{
    char expect[96], got[96], candidate[96];
    const char *partial;
    size_t pre;
    int i;
    morse_normalize(t->target, expect, sizeof expect);
    morse_normalize(t->typed, got, sizeof got);
    if (strncmp(expect, got, strlen(got)) == 0)
        return 1;
    partial = strrchr(t->typed, '<');
    if (!partial || strchr(partial, '>'))
        return 0;
    pre = (size_t)(partial - t->typed);
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        const morse_entry *e = &MORSE_TABLE[i];
        if (e->group != MG_PROSIGN || strncmp(e->text, partial, strlen(partial)) != 0)
            continue;
        memcpy(candidate, t->typed, pre);
        str_copy(candidate + pre, sizeof candidate - pre, e->text);
        morse_normalize(candidate, got, sizeof got);
        if (strncmp(expect, got, strlen(got)) == 0)
            return 1;
    }
    return 0;
}

/* -1: wrong character, 0: correct prefix, 1: complete answer. */
static int compare_send(const training_t *t, const settings_t *c)
{
    char expect[96], sent[96];
    size_t len;
    morse_normalize(t->target, expect, sizeof expect);
    morse_normalize(t->sent, sent, sizeof sent);
    if (!c->train_strict) {
        char *read = sent, *write = sent;
        while (*read) {
            if (*read != ' ')
                *write++ = *read;
            read++;
        }
        *write = 0;
    }
    len = strlen(sent);
    if (strncmp(expect, sent, len) != 0)
        return -1;
    return expect[len] == 0 ? 1 : 0;
}

static int check_send(training_t *t, const settings_t *c, const cw_decoder *dec)
{
    int fx;
    t->send_wpm = dec_wpm(dec);
    t->send_ratio = dec_ratio(dec);
    fx = record(t, c, t->target, t->sent, compare_send(t, c) == 1);
    if (t->ok) {
        t->state = TS_SEND_RESULT;
        return fx;
    }
    return fx | TRAIN_WRONG | training_retry(t, c);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

int training_enter(training_t *t, const settings_t *c, const cw_decoder *dec)
{
    switch (t->state) {
    case TS_SETUP: return training_start(t, c);
    case TS_LISTEN: return check_copy(t, c);
    case TS_SEND: return check_send(t, c, dec);
    default: return training_next(t, c);
    }
}

int training_type(training_t *t, const settings_t *c, int ch)
{
    if (t->state != TS_LISTEN || ch < 0x20 || ch >= 0x7F || t->ntyped >= (int)sizeof t->typed - 1)
        return 0;
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    t->typed[t->ntyped++] = (char)ch;
    t->typed[t->ntyped] = 0;
    if (same_text(t->target, t->typed) || !copy_prefix_matches(t))
        return check_copy(t, c);
    return 0;
}

int training_backspace(training_t *t, const settings_t *c)
{
    if (c->train_dir == TRAIN_SEND)
        return training_retry(t, c);
    if (t->state == TS_LISTEN && t->ntyped > 0)
        t->typed[--t->ntyped] = 0;
    return 0;
}

int training_sent(training_t *t, const settings_t *c, const dec_token *tok, const cw_decoder *dec)
{
    size_t len = strlen(t->sent);
    if (t->state != TS_SEND)
        return 0;
    if (tok->kind == TOK_SPACE) {
        /* Long pauses do not finish an incomplete word. Strict mode keeps
         * them so an inter-word gap inside the target fails the comparison. */
        if (c->train_strict && len > 0 && t->sent[len - 1] != ' ' && len + 1 < sizeof t->sent) {
            t->sent[len] = ' ';
            t->sent[len + 1] = 0;
        }
        return 0;
    }
    if (len + strlen(tok->text) + 1 < sizeof t->sent)
        strcat(t->sent, tok->text);
    return compare_send(t, c) != 0 ? check_send(t, c, dec) : 0;
}

int training_pressed(training_t *t, const settings_t *c)
{
    /* Keep the target until Enter requests another item. */
    return t->state == TS_SEND_RESULT ? training_retry(t, c) : 0;
}

int training_keying(const training_t *t, const settings_t *c)
{
    return c->train_dir == TRAIN_SEND && (t->state == TS_SEND || t->state == TS_SEND_RESULT);
}

int training_weak(const training_t *t, char *out, int cap)
{
    int n = 0, ch, i;
    for (ch = 33; ch < 127 && cap > 0; ch++) {
        int m = 0;
        if (t->err[ch] <= 0)
            continue;
        if (n < cap) {
            out[n++] = (char)ch;
            continue;
        }
        for (i = 1; i < n; i++)
            if (t->err[(int)out[i]] < t->err[(int)out[m]])
                m = i;
        if (t->err[ch] > t->err[(int)out[m]])
            out[m] = (char)ch;
    }
    return n;
}
