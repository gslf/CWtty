/*
 * transcript.h - Scrollback of decoded text, split in lines tagged with a
 * direction (received, transmitted, system), optionally a station (the
 * online rooms), and per-character styles.
 */
#ifndef CWTTY_TRANSCRIPT_H
#define CWTTY_TRANSCRIPT_H

#define TR_LINE_CAP 512
#define TR_LINES 256
#define TR_WHO_CAP 12

enum { DIR_RX, DIR_TX, DIR_SYS };
/* Character styles. A word's characters become STY_ABBR or STY_SIGN once
 * the word is complete (a space or the end of its line), see abbr.h. */
enum { STY_NORMAL, STY_PROSIGN, STY_UNKNOWN, STY_ABBR, STY_SIGN };

typedef struct {
    int           dir;
    char          who[TR_WHO_CAP]; /* the station, "" when not tagged */
    int           closed;          /* the next append starts a new line */
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
/* Appends to the open line of station who among the last few lines, so
 * that stations keying at the same time keep a line each; else starts one. */
void transcript_append_who(transcript *t, int dir, const char *who, const char *text, int style);
/* Forces the next append to start a new line. */
void transcript_break(transcript *t);
/* Closes the open line of station who. */
void transcript_break_who(transcript *t, const char *who);
/* Station who has an open line. */
int transcript_open_who(transcript *t, const char *who);
/* Deletes the last character (or prosign) of the last line. */
void transcript_backspace(transcript *t);

int            transcript_count(const transcript *t);
const tr_line *transcript_line(const transcript *t, int i); /* 0 = oldest */

#endif
