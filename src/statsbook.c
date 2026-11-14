/*
 * statsbook.c - The statistics of every device of a user.
 */
#include "statsbook.h"

#include <string.h>

#include "util.h"

typedef struct {
    char        id[BOOK_ID + 1];
    char        name[BOOK_NAME];
    int         rev;
    const char *body; /* the lines after the header */
    int         len;
} raw_section;

int statsbook_valid_id(const char *id)
{
    int i;
    for (i = 0; id[i]; i++)
        if (i >= BOOK_ID || !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    return i > 0;
}

void statsbook_id(char out[BOOK_ID + 1], const unsigned char rnd[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        out[2 * i] = "0123456789abcdef"[rnd[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[rnd[i] & 15];
    }
    out[BOOK_ID] = 0;
}

/* Printable ASCII without ']', at most BOOK_NAME - 1 bytes, trimmed. */
static void set_name(char *dst, const char *src, int n)
{
    int k = 0, i;
    for (i = 0; i < n && k < BOOK_NAME - 1; i++)
        if (src[i] >= 0x20 && src[i] < 0x7f && src[i] != ']')
            dst[k++] = src[i];
    while (k > 0 && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
}

void statsbook_init(statsbook_t *b, const char *id, const char *name)
{
    memset(b, 0, sizeof *b);
    if (statsbook_valid_id(id))
        str_copy(b->own.id, sizeof b->own.id, id);
    set_name(b->own.name, name, (int)strlen(name));
}

const book_section *statsbook_find(const statsbook_t *b, const char *id)
{
    int i;
    if (strcmp(b->own.id, id) == 0)
        return &b->own;
    for (i = 0; i < b->others; i++)
        if (strcmp(b->other[i].id, id) == 0)
            return &b->other[i];
    return NULL;
}

void statsbook_update(statsbook_t *b)
{
    int i;
    stats_reset(&b->total);
    stats_merge(&b->total, &b->own.s);
    for (i = 0; i < b->others; i++)
        stats_merge(&b->total, &b->other[i].s);
    stats_lift(&b->own.s, &b->total);
}

void statsbook_changed(statsbook_t *b)
{
    b->own.rev++;
    statsbook_update(b);
}

/* ------------------------------------------------------------------------ */
/* Text form                                                                 */
/* ------------------------------------------------------------------------ */

static const char *line_end(const char *p)
{
    while (*p && *p != '\n')
        p++;
    return p;
}

static const char *next_line(const char *p)
{
    p = line_end(p);
    return *p ? p + 1 : p;
}

static int is_header(const char *p)
{
    return strncmp(p, "[device ", 8) == 0;
}

/* The first header at or after p (at the start of a line), or NULL. */
static const char *find_header(const char *p)
{
    for (; *p; p = next_line(p))
        if (is_header(p))
            return p;
    return NULL;
}

/* "[device <id> rev=<n> name=<name>]" and the body up to the next header. */
static int read_section(const char *h, raw_section *r)
{
    const char *p = h + 8, *end = line_end(h), *next;
    int k = 0;
    memset(r, 0, sizeof *r);
    while (p < end && *p != ' ' && *p != ']' && k < BOOK_ID)
        r->id[k++] = *p++;
    r->id[k] = 0;
    if (!statsbook_valid_id(r->id))
        return 0;
    while (p < end && *p != ']') {
        while (p < end && *p == ' ')
            p++;
        if (strncmp(p, "rev=", 4) == 0) {
            long long v = 0;
            for (p += 4; p < end && *p >= '0' && *p <= '9'; p++)
                if (v < 2147483647LL)
                    v = v * 10 + (*p - '0');
            r->rev = (int)(v < 2147483647LL ? v : 2147483647LL);
        } else if (strncmp(p, "name=", 5) == 0) {
            const char *n = p + 5, *e = end;
            while (e > n && e[-1] != ']')
                e--;
            if (e > n)
                e--; /* the closing ']' */
            else
                e = end;
            set_name(r->name, n, (int)(e - n));
            p = end;
        } else {
            while (p < end && *p != ' ' && *p != ']')
                p++;
        }
    }
    r->body = *end ? end + 1 : end;
    next = find_header(r->body);
    r->len = (int)((next ? next : r->body + strlen(r->body)) - r->body);
    return 1;
}

/* A section of another device, when newer than the copy kept. With no room
 * left, it takes the place of the least recently practised one if more
 * recent. Returns 1 if taken. */
static int keep_section(statsbook_t *b, const book_section *in)
{
    int i, slot = -1;
    for (i = 0; i < b->others; i++)
        if (strcmp(b->other[i].id, in->id) == 0) {
            if (in->rev <= b->other[i].rev)
                return 0;
            slot = i;
        }
    if (slot < 0 && b->others < BOOK_OTHERS)
        slot = b->others++;
    if (slot < 0) {
        int stale = 0;
        for (i = 1; i < b->others; i++)
            if (b->other[i].s.last_day < b->other[stale].s.last_day)
                stale = i;
        if (in->s.last_day <= b->other[stale].s.last_day)
            return 0;
        slot = stale;
    }
    b->other[slot] = *in;
    return 1;
}

static int keep_other(statsbook_t *b, const raw_section *r)
{
    static book_section in; /* static: the wasm stack is small */
    memset(&in, 0, sizeof in);
    str_copy(in.id, sizeof in.id, r->id);
    str_copy(in.name, sizeof in.name, r->name);
    in.rev = r->rev;
    stats_parse_n(&in.s, r->body, r->len);
    return keep_section(b, &in);
}

/* Reads text into b (cleared, keeping the own id and name). With use_own,
 * an own= line names this device's section. Returns the number of
 * sections, or of known lines in a text from before the sections. */
static int read_book(statsbook_t *b, const char *text, int use_own)
{
    const char *h = find_header(text), *p;
    raw_section r;
    int n = 0;
    b->others = 0;
    b->own.rev = 0;
    stats_reset(&b->own.s);
    if (!h)
        return stats_parse(&b->own.s, text);
    for (p = text; use_own && p < h; p = next_line(p))
        if (strncmp(p, "own=", 4) == 0) {
            char id[BOOK_ID + 1];
            int k = 0;
            for (p += 4; *p && *p != '\n' && *p != '\r' && *p != ' ' && k < BOOK_ID; p++)
                id[k++] = *p;
            id[k] = 0;
            if (statsbook_valid_id(id))
                str_copy(b->own.id, sizeof b->own.id, id);
        }
    for (; h; h = find_header(r.body + r.len)) {
        if (!read_section(h, &r)) {
            r.body = next_line(h);
            r.len = 0;
            continue;
        }
        n++;
        if (strcmp(r.id, b->own.id) == 0) {
            if (b->own.rev > 0 && r.rev <= b->own.rev)
                continue;
            stats_parse_n(&b->own.s, r.body, r.len);
            b->own.rev = r.rev;
        } else {
            keep_other(b, &r);
        }
    }
    return n;
}

void statsbook_parse(statsbook_t *b, const char *text)
{
    read_book(b, text, 1);
    statsbook_update(b);
}

int statsbook_import(statsbook_t *b, const char *text)
{
    static statsbook_t in;
    statsbook_init(&in, b->own.id, b->own.name);
    if (read_book(&in, text, 0) <= 0)
        return -1;
    in.own.rev = maxi(b->own.rev, in.own.rev) + 1;
    in.own.s.ms_rest = b->own.s.ms_rest;
    *b = in;
    statsbook_update(b);
    return 0;
}

int statsbook_adopt(statsbook_t *b, const char *text)
{
    const char *h = find_header(text);
    raw_section r;
    int taken;
    if (!h || !read_section(h, &r))
        return -1;
    if (strcmp(r.id, b->own.id) == 0) {
        if (r.rev <= b->own.rev)
            return 0;
        stats_parse_n(&b->own.s, r.body, r.len);
        b->own.rev = r.rev;
        taken = 1;
    } else {
        taken = keep_other(b, &r);
    }
    if (taken)
        statsbook_update(b);
    return taken;
}

int statsbook_take(statsbook_t *b, const book_section *sec)
{
    int taken;
    if (strcmp(sec->id, b->own.id) == 0) {
        if (sec->rev <= b->own.rev)
            return 0;
        b->own.s = sec->s;
        b->own.rev = sec->rev;
        taken = 1;
    } else {
        taken = keep_section(b, sec);
    }
    if (taken)
        statsbook_update(b);
    return taken;
}

int statsbook_header(const char *text, char id[BOOK_ID + 1], int *rev)
{
    const char *h = find_header(text);
    raw_section r;
    if (!h || !read_section(h, &r))
        return -1;
    str_copy(id, BOOK_ID + 1, r.id);
    *rev = r.rev;
    return 0;
}

typedef struct {
    char *p;
    int   n, cap;
} sbuf;

static void put_s(sbuf *b, const char *s)
{
    for (; *s; s++, b->n++)
        if (b->n + 1 < b->cap)
            b->p[b->n] = *s;
}

static void put_i(sbuf *b, int v)
{
    char tmp[12];
    int k = (int)sizeof tmp - 1;
    unsigned u = v < 0 ? 0u : (unsigned)v;
    tmp[k] = 0;
    do {
        tmp[--k] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    put_s(b, tmp + k);
}

static void put_section(sbuf *b, const book_section *sec)
{
    char name[BOOK_NAME];
    int room, n;
    put_s(b, "[device ");
    put_s(b, sec->id);
    put_s(b, " rev=");
    put_i(b, sec->rev);
    put_s(b, " name=");
    set_name(name, sec->name, (int)strlen(sec->name));
    put_s(b, name);
    put_s(b, "]\n");
    room = b->cap - b->n;
    n = stats_format(&sec->s, room > 0 ? b->p + b->n : NULL, room > 0 ? room : 0);
    b->n += n >= 0 ? n : STATS_TEXT_CAP; /* too long: reported by the caller */
}

static int finish(sbuf *b, char *out, int cap)
{
    if (cap > 0)
        out[b->n < cap ? b->n : cap - 1] = 0;
    return b->n < cap ? b->n : -1;
}

int statsbook_section_text(const book_section *sec, char *out, int cap)
{
    sbuf b = {out, 0, cap};
    put_section(&b, sec);
    return finish(&b, out, cap);
}

int statsbook_format(const statsbook_t *b, char *out, int cap)
{
    sbuf o = {out, 0, cap};
    int i;
    put_s(&o, "# CWtty training records and statistics, a section per device: the totals\n"
              "# are their sums. The other devices come from the cloud or an import.\n");
    put_s(&o, "own=");
    put_s(&o, b->own.id);
    put_s(&o, "\n");
    put_section(&o, &b->own);
    for (i = 0; i < b->others; i++)
        put_section(&o, &b->other[i]);
    return finish(&o, out, cap);
}
