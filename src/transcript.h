/*
 * transcript.h - Scrollback of decoded text, split in lines tagged with a
 * direction (received, transmitted, system) and per-character styles.
 */
#ifndef CWTTY_TRANSCRIPT_H
#define CWTTY_TRANSCRIPT_H

#define TR_LINE_CAP 512
#define TR_LINES 256

enum { DIR_RX, DIR_TX, DIR_SYS };
enum { STY_NORMAL, STY_PROSIGN, STY_UNKNOWN };

typedef struct {
    int           dir;
    int           closed; /* the next append starts a new line */
    int           len;
    char          text[TR_LINE_CAP];
    unsigned char style[TR_LINE_CAP];
} tr_line;

typedef struct {
    tr_line lines[TR_LINES];
    int     head;  /* index of the oldest line */
    int     count;
} transcript;

void transcript_clear(transcript *t);
/* Appends to the last line if it has the same direction, else starts one. */
void transcript_append(transcript *t, int dir, const char *text, int style);
/* Forces the next append to start a new line. */
void transcript_break(transcript *t);
/* Deletes the last character (or prosign) of the last line. */
void transcript_backspace(transcript *t);

int            transcript_count(const transcript *t);
const tr_line *transcript_line(const transcript *t, int i); /* 0 = oldest */

#endif
