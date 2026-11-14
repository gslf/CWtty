/*
 * statsbook.h - The statistics of every device of a user: this device's
 * section, written by the training, and copies of the sections of the
 * other devices (from the cloud or from an import). What is shown is the
 * total, their sum (stats_merge()).
 *
 * Every section has a random id, a name ("Terminal", "Web") and a
 * revision that grows with every change: between two copies of a section
 * the higher revision wins. The text form, kept on disk, exported and
 * stored in the cloud (one section per file there):
 *
 *   # comments
 *   own=<id>                                    (local file only)
 *   [device <id> rev=<n> name=<name>]
 *   copy_sessions=...                           (stats_format())
 *   [device <id> rev=<n> name=<name>]
 *   ...
 *
 * A file from before the sections is this device's section.
 *
 * Pure data, no I/O, no allocation: also compiled to WebAssembly.
 */
#ifndef CWTTY_STATSBOOK_H
#define CWTTY_STATSBOOK_H

#include "stats.h"

#define BOOK_OTHERS 8      /* other devices kept, the least recent leave */
#define BOOK_ID 16         /* hex digits of a device id */
#define BOOK_NAME 24
#define BOOK_TEXT_CAP 131072 /* the text form of a full book always fits */

typedef struct {
    char    id[BOOK_ID + 1]; /* "" = unused */
    char    name[BOOK_NAME];
    int     rev;
    stats_t s;
} book_section;

typedef struct {
    book_section own;
    book_section other[BOOK_OTHERS];
    int          others;
    stats_t      total; /* own + others, see statsbook_update() */
} statsbook_t;

/* An empty book for this device. id: BOOK_ID hex digits (statsbook_id()),
 * kept until a stored text names another one. */
void statsbook_init(statsbook_t *b, const char *id, const char *name);

/* A device id from 8 random bytes. */
void statsbook_id(char out[BOOK_ID + 1], const unsigned char rnd[8]);

/* Reads the stored text over b: this device's section is the one named by
 * own= (or the whole text, from before the sections). */
void statsbook_parse(statsbook_t *b, const char *text);

/* The text to store or export: the length, or -1 if cap is too small. */
int statsbook_format(const statsbook_t *b, char *out, int cap);

/* One section with its header, as stored in the cloud. */
int statsbook_section_text(const book_section *sec, char *out, int cap);

/* This device's section changed (training): a new revision and a new
 * total. */
void statsbook_changed(statsbook_t *b);

/* The total again, and this device's records raised to it. */
void statsbook_update(statsbook_t *b);

/* Replaces the book with an exported one: this device takes its own section
 * from it (or starts empty) with a revision above both, the other sections
 * replace the ones kept. 0, or -1 when text is not a statistics file (b is
 * untouched). */
int statsbook_import(statsbook_t *b, const char *text);

/* A section from the cloud: taken when newer than the copy kept. 1 taken,
 * 0 not newer, -1 not a section. */
int statsbook_adopt(statsbook_t *b, const char *text);

/* A section from another book (a sync): taken like statsbook_adopt(). */
int statsbook_take(statsbook_t *b, const book_section *sec);

/* The device id and revision of a section's text: 0, or -1. */
int statsbook_header(const char *text, char id[BOOK_ID + 1], int *rev);

/* The section of a device, NULL if not kept. */
const book_section *statsbook_find(const statsbook_t *b, const char *id);

/* Checks a device id: 1..BOOK_ID lower-case hex digits. */
int statsbook_valid_id(const char *id);

#endif
