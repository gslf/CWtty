/*
 * stats.c - Training records and statistics, kept across sessions.
 *
 * No libc beyond the string functions: this file is also compiled to
 * WebAssembly (web/wasm/libc), so numbers are formatted and parsed here.
 */
#include "stats.h"

#include <stddef.h>
#include <string.h>

#include "util.h"

void stats_reset(stats_t *s)
{
    memset(s, 0, sizeof *s);
}

static stats_record *record_of(stats_t *s, int dir, int level)
{
    return dir == STATS_SEND ? &s->send_rec : &s->copy_rec[clampi(level, 1, STATS_LEVELS) - 1];
}

const stats_record *stats_record_of(const stats_t *s, int dir, int level)
{
    return record_of((stats_t *)s, dir, level);
}

/* ------------------------------------------------------------------------ */
/* Counting                                                                  */
/* ------------------------------------------------------------------------ */

static void count_day(stats_t *s, int day)
{
    int shift, i;
    if (day <= 0)
        return;
    if (day > s->last_day) {
        s->day_streak = s->last_day > 0 && s->last_day == day - 1 ? s->day_streak + 1 : 1;
        if (s->day_streak > s->best_day_streak)
            s->best_day_streak = s->day_streak;
        s->days++;
        if (s->first_day <= 0)
            s->first_day = day;
        s->last_day = day;
    }
    /* The activity window follows the newest day. */
    if (day > s->act_day) {
        shift = day - s->act_day;
        if (s->act_day <= 0 || shift >= STATS_DAYS) {
            memset(s->act, 0, sizeof s->act);
        } else {
            memmove(s->act + shift, s->act, sizeof(int) * (size_t)(STATS_DAYS - shift));
            for (i = 0; i < shift; i++)
                s->act[i] = 0;
        }
        s->act_day = day;
    }
    i = s->act_day - day;
    if (i >= 0 && i < STATS_DAYS)
        s->act[i]++;
}

void stats_answer(stats_t *s, int dir, int ok, double ms, int day)
{
    stats_dir *d = &s->dir[dir & 1];
    int sec;
    d->answers++;
    if (ok)
        d->correct++;
    s->ms_rest += clampd(ms, 0, 60000.0);
    sec = (int)(s->ms_rest / 1000.0);
    d->seconds += sec;
    s->ms_rest -= sec * 1000.0;
    count_day(s, day);
}

void stats_char(stats_t *s, int dir, int ch, int missed)
{
    stats_dir *d = &s->dir[dir & 1];
    ch &= 127;
    d->seen[ch]++;
    if (missed)
        d->miss[ch]++;
}

int stats_streak(stats_t *s, int dir, int level, int streak, int wpm, int day)
{
    stats_record *r = record_of(s, dir, level);
    if (streak <= r->value)
        return 0;
    r->value = streak;
    r->wpm = wpm;
    r->day = day;
    return 1;
}

int stats_day_streak(const stats_t *s, int today)
{
    if (today > 0 && s->last_day < today - 1)
        return 0;
    return s->day_streak;
}

int stats_activity(const stats_t *s, int today, int ago)
{
    int i = ago - (today > 0 ? today - s->act_day : 0); /* act[i] is act_day - i */
    return s->act_day > 0 && i >= 0 && i < STATS_DAYS ? s->act[i] : 0;
}

int stats_char_pct(const stats_t *s, int dir, int ch)
{
    const stats_dir *d = &s->dir[dir & 1];
    ch &= 127;
    if (d->seen[ch] <= 0)
        return -1;
    return (d->seen[ch] - d->miss[ch]) * 100 / d->seen[ch];
}

/* Misses over count, smoothed so that one miss in two is not the worst. */
static double weakness(const stats_dir *d, int ch)
{
    return (d->miss[ch] + 0.5) / (d->seen[ch] + 3.0);
}

int stats_weak(const stats_t *s, int dir, char *out, int cap)
{
    const stats_dir *d = &s->dir[dir & 1];
    int n = 0, ch, i;
    for (ch = 33; ch < 127; ch++) {
        if (d->miss[ch] <= 0)
            continue;
        /* Insertion sort, the weakest first; past cap only if weaker. */
        i = n < cap ? n++ : cap;
        while (i > 0 && weakness(d, ch) > weakness(d, (unsigned char)out[i - 1])) {
            if (i < cap)
                out[i] = out[i - 1];
            i--;
        }
        if (i < cap)
            out[i] = (char)ch;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Dates                                                                     */
/* ------------------------------------------------------------------------ */

/* Howard Hinnant's days_from_civil and civil_from_days. */
int stats_day(int year, int month, int mday)
{
    int y = year - (month <= 2), era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + mday - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

void stats_date(int day, int *year, int *month, int *mday)
{
    int z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)m;
    *year = (int)yoe + era * 400 + (m <= 2);
}

/* ------------------------------------------------------------------------ */
/* Text form                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *key;
    int         offset;
} field_t;

#define F(key, member) {key, (int)offsetof(stats_t, member)}

static const field_t FIELDS[] = {
    F("copy_sessions", dir[0].sessions),
    F("copy_answers", dir[0].answers),
    F("copy_correct", dir[0].correct),
    F("copy_seconds", dir[0].seconds),
    F("copy_items", copy_items),
    F("copy_listens", copy_listens),
    F("copy_first", copy_first),
    F("copy_failed", copy_failed),
    F("copy_record_1", copy_rec[0].value),
    F("copy_record_1_wpm", copy_rec[0].wpm),
    F("copy_record_1_day", copy_rec[0].day),
    F("copy_record_2", copy_rec[1].value),
    F("copy_record_2_wpm", copy_rec[1].wpm),
    F("copy_record_2_day", copy_rec[1].day),
    F("copy_record_3", copy_rec[2].value),
    F("copy_record_3_wpm", copy_rec[2].wpm),
    F("copy_record_3_day", copy_rec[2].day),
    F("copy_record_4", copy_rec[3].value),
    F("copy_record_4_wpm", copy_rec[3].wpm),
    F("copy_record_4_day", copy_rec[3].day),
    F("send_sessions", dir[1].sessions),
    F("send_answers", dir[1].answers),
    F("send_correct", dir[1].correct),
    F("send_seconds", dir[1].seconds),
    F("send_record", send_rec.value),
    F("send_record_wpm", send_rec.wpm),
    F("send_record_day", send_rec.day),
    F("send_timed", send_timed),
    F("send_wpm10", send_wpm10),
    F("send_ratio10", send_ratio10),
    F("send_best10", send_best10),
    F("koch", koch),
    F("first_day", first_day),
    F("last_day", last_day),
    F("days", days),
    F("day_streak", day_streak),
    F("best_day_streak", best_day_streak),
    F("activity_day", act_day),
};

int *stats_field(stats_t *s, const char *key)
{
    int i;
    for (i = 0; i < ARRAY_LEN(FIELDS); i++)
        if (strcmp(FIELDS[i].key, key) == 0)
            return (int *)((char *)s + FIELDS[i].offset);
    return NULL;
}

typedef struct {
    char *p;
    int   n, cap;
} sbuf;

static void put_c(sbuf *b, char c)
{
    if (b->n + 1 < b->cap)
        b->p[b->n] = c;
    b->n++;
}

static void put_s(sbuf *b, const char *s)
{
    while (*s)
        put_c(b, *s++);
}

static void put_i(sbuf *b, int v, int base)
{
    char tmp[12];
    int k = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0)
        put_c(b, '-');
    do {
        tmp[k++] = "0123456789ABCDEF"[u % (unsigned)base];
        u /= (unsigned)base;
    } while (u);
    while (k)
        put_c(b, tmp[--k]);
}

static void put_chars(sbuf *b, const char *key, const stats_dir *d)
{
    int ch, first = 1;
    put_s(b, key);
    put_c(b, '=');
    for (ch = 0; ch < 128; ch++) {
        if (d->seen[ch] <= 0 && d->miss[ch] <= 0)
            continue;
        if (!first)
            put_c(b, ' ');
        first = 0;
        put_i(b, ch, 16);
        put_c(b, ':');
        put_i(b, d->seen[ch], 10);
        put_c(b, ':');
        put_i(b, d->miss[ch], 10);
    }
    put_c(b, '\n');
}

int stats_format(const stats_t *s, char *out, int cap)
{
    sbuf b = {out, 0, cap};
    int i;
    put_s(&b, "# CWtty training records and statistics.\n");
    for (i = 0; i < ARRAY_LEN(FIELDS); i++) {
        put_s(&b, FIELDS[i].key);
        put_c(&b, '=');
        put_i(&b, *(const int *)((const char *)s + FIELDS[i].offset), 10);
        put_c(&b, '\n');
    }
    put_s(&b, "activity=");
    for (i = 0; i < STATS_DAYS; i++) {
        if (i)
            put_c(&b, ' ');
        put_i(&b, s->act[i], 10);
    }
    put_c(&b, '\n');
    put_chars(&b, "copy_chars", &s->dir[STATS_COPY]);
    put_chars(&b, "send_chars", &s->dir[STATS_SEND]);
    if (cap > 0)
        out[b.n < cap ? b.n : cap - 1] = 0;
    return b.n < cap ? b.n : -1;
}

static int digit(char c, int base)
{
    int v = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99));
    return v < base ? v : -1;
}

/* Reads an integer at *p, skipping blanks before it. Returns 0 if none. */
static int get_i(const char **p, int base, int *out)
{
    const char *s = *p;
    long long v = 0;
    int neg = 0, any = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (digit(*s, base) >= 0) {
        if (v < 2147483647LL)
            v = v * base + digit(*s, base);
        s++;
        any = 1;
    }
    if (!any)
        return 0;
    if (v > 2147483647LL)
        v = 2147483647LL;
    *out = (int)(neg ? -v : v);
    *p = s;
    return 1;
}

static void parse_chars(stats_dir *d, const char *v)
{
    int ch, seen, miss;
    while (get_i(&v, 16, &ch)) {
        if (*v++ != ':' || !get_i(&v, 10, &seen) || *v++ != ':' || !get_i(&v, 10, &miss))
            return;
        if (ch >= 0 && ch < 128 && seen >= 0 && miss >= 0) {
            d->seen[ch] = seen;
            d->miss[ch] = miss < seen ? miss : seen;
        }
    }
}

/* The value of a line ends at the newline: the readers stop there. */
static void parse_line(stats_t *s, const char *key, int klen, const char *v)
{
    char name[40];
    int *p, i;
    if (klen <= 0 || klen >= (int)sizeof name)
        return;
    memcpy(name, key, (size_t)klen);
    name[klen] = 0;
    if (strcmp(name, "activity") == 0) {
        for (i = 0; i < STATS_DAYS && get_i(&v, 10, &s->act[i]); i++) {
        }
    } else if (strcmp(name, "copy_chars") == 0) {
        parse_chars(&s->dir[STATS_COPY], v);
    } else if (strcmp(name, "send_chars") == 0) {
        parse_chars(&s->dir[STATS_SEND], v);
    } else if ((p = stats_field(s, name)) != NULL) {
        get_i(&v, 10, p);
    }
}

void stats_parse(stats_t *s, const char *text)
{
    int i;
    stats_reset(s);
    while (*text) {
        const char *k = text;
        while (*text && *text != '\n' && *text != '=')
            text++;
        if (*k != '#' && *text == '=')
            parse_line(s, k, (int)(text - k), text + 1);
        while (*text && *text != '\n')
            text++;
        if (*text)
            text++;
    }
    for (i = 0; i < 2; i++)
        s->dir[i].correct = clampi(s->dir[i].correct, 0, s->dir[i].answers);
}
