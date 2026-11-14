/*
 * transcript.c - Scrollback of decoded text.
 */
#include "transcript.h"

#include <string.h>

#include "abbr.h"
#include "util.h"

void transcript_clear(transcript *t)
{
    t->head = 0;
    t->count = 0;
}

static tr_line *last_line(transcript *t)
{
    if (t->count == 0)
        return NULL;
    return &t->lines[(t->head + t->count - 1) % TR_LINES];
}

static tr_line *new_line(transcript *t, int dir, const char *who)
{
    tr_line *l;
    if (t->count == TR_LINES) {
        t->head = (t->head + 1) % TR_LINES;
        t->count--;
    }
    l = &t->lines[(t->head + t->count) % TR_LINES];
    t->count++;
    l->dir = dir;
    str_copy(l->who, sizeof l->who, who);
    l->closed = 0;
    l->len = 0;
    l->text[0] = 0;
    return l;
}

/* Gives the last word of a line the style of its kind (abbr.h), once it is
 * complete. Only plain characters change: prosigns and unknowns keep theirs. */
static void mark_word(tr_line *l)
{
    int end = l->len, start, kind, i;
    while (end > 0 && l->text[end - 1] == ' ')
        end--;
    start = end;
    while (start > 0 && l->text[start - 1] != ' ')
        start--;
    if (start == end)
        return;
    kind = abbr_word(l->text + start, end - start);
    if (kind == WORD_TEXT)
        return;
    for (i = start; i < end; i++)
        if (l->style[i] == STY_NORMAL)
            l->style[i] = (unsigned char)(kind == WORD_SIGN ? STY_SIGN : STY_ABBR);
}

/* Appends to line l, or to a new line if l is NULL or full. */
static void append_to(transcript *t, tr_line *l, int dir, const char *who, const char *text, int style)
{
    size_t n = strlen(text);
    if (n == 0)
        return;
    if (!l || n > TR_LINE_CAP - 1 - (size_t)l->len) {
        if (n == 1 && text[0] == ' ')
            return; /* never start a line with a space */
        l = new_line(t, dir, who);
    }
    /* No leading spaces on a fresh line, no double spaces. */
    if (n == 1 && text[0] == ' ' && (l->len == 0 || l->text[l->len - 1] == ' '))
        return;
    if (n == 1 && text[0] == ' ')
        mark_word(l); /* a word is complete */
    while (n > 0) {
        size_t chunk = TR_LINE_CAP - 1 - (size_t)l->len;
        if (chunk > n)
            chunk = n;
        /* Keep a UTF-8 character together when wrapping a long append. */
        if (chunk < n) {
            size_t boundary = chunk;
            while (boundary > 0 && ((unsigned char)text[boundary] & 0xC0) == 0x80)
                boundary--;
            if (boundary > 0)
                chunk = boundary;
        }
        memcpy(l->text + l->len, text, chunk);
        memset(l->style + l->len, style, chunk);
        l->len += (int)chunk;
        l->text[l->len] = 0;
        text += chunk;
        n -= chunk;
        if (n > 0)
            l = new_line(t, dir, who);
    }
}

void transcript_append(transcript *t, int dir, const char *text, int style)
{
    tr_line *l = last_line(t);
    if (l && (l->dir != dir || l->closed || l->who[0])) {
        mark_word(l); /* the line is over */
        l = NULL;
    }
    append_to(t, l, dir, "", text, style);
}

#define WHO_LOOKBACK 8

/* The open line of who among the last lines, or NULL. */
static tr_line *open_line_of(transcript *t, const char *who)
{
    int i;
    for (i = t->count - 1; i >= 0 && i >= t->count - WHO_LOOKBACK; i--) {
        tr_line *l = &t->lines[(t->head + i) % TR_LINES];
        if (strcmp(l->who, who) == 0)
            return l->closed ? NULL : l;
    }
    return NULL;
}

void transcript_append_who(transcript *t, int dir, const char *who, const char *text, int style)
{
    tr_line *l = open_line_of(t, who);
    if (l && l->dir != dir)
        l = NULL;
    append_to(t, l, dir, who, text, style);
}

int transcript_open_who(transcript *t, const char *who)
{
    return open_line_of(t, who) != NULL;
}

void transcript_break_who(transcript *t, const char *who)
{
    tr_line *l = open_line_of(t, who);
    if (l) {
        mark_word(l);
        l->closed = 1;
    }
}

void transcript_break(transcript *t)
{
    tr_line *l = last_line(t);
    if (l && l->len > 0) {
        mark_word(l);
        l->closed = 1;
    }
}

void transcript_backspace(transcript *t)
{
    tr_line *l = last_line(t);
    if (!l || l->len <= 0)
        return;
    if (l->style[l->len - 1] == STY_PROSIGN && l->text[l->len - 1] == '>') {
        while (l->len > 0 && l->text[l->len - 1] != '<')
            l->len--;
    }
    if (l->len > 0) {
        l->len--;
        while (l->len > 0 && ((unsigned char)l->text[l->len] & 0xC0) == 0x80)
            l->len--;
    }
    l->text[l->len] = 0;
}

int transcript_count(const transcript *t)
{
    return t->count;
}

const tr_line *transcript_line(const transcript *t, int i)
{
    if (i < 0 || i >= t->count)
        return NULL;
    return &t->lines[(t->head + i) % TR_LINES];
}
