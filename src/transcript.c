/*
 * transcript.c - Scrollback of decoded text.
 */
#include "transcript.h"

#include <string.h>

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

static tr_line *new_line(transcript *t, int dir)
{
    tr_line *l;
    if (t->count == TR_LINES) {
        t->head = (t->head + 1) % TR_LINES;
        t->count--;
    }
    l = &t->lines[(t->head + t->count) % TR_LINES];
    t->count++;
    l->dir = dir;
    l->closed = 0;
    l->len = 0;
    l->text[0] = 0;
    return l;
}

void transcript_append(transcript *t, int dir, const char *text, int style)
{
    tr_line *l = last_line(t);
    size_t n = strlen(text);
    if (n == 0)
        return;
    if (!l || l->dir != dir || l->closed || n > TR_LINE_CAP - 1 - (size_t)l->len) {
        if (n == 1 && text[0] == ' ')
            return; /* never start a line with a space */
        l = new_line(t, dir);
    }
    /* No leading spaces on a fresh line, no double spaces. */
    if (n == 1 && text[0] == ' ' && (l->len == 0 || l->text[l->len - 1] == ' '))
        return;
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
            l = new_line(t, dir);
    }
}

void transcript_break(transcript *t)
{
    tr_line *l = last_line(t);
    if (l && l->len > 0)
        l->closed = 1;
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
