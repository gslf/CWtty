/*
 * decoder.c - Adaptive CW timing decoder.
 */
#include "decoder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "morse.h"
#include "util.h"

#define MIN_MARK_MS 4.0     /* shorter marks are glitches */
#define MAX_MARK_MS 2500.0  /* longer marks are carriers / stuck keys */
#define CHAR_GAP_UNITS 2.0  /* between 1 (element) and 3 (character) */
#define WORD_GAP_UNITS 5.0  /* between 3 (character) and 7 (word) */
#define MIN_RATIO 1.8       /* dah/dit ratio that separates two clusters */

static double ema(double avg, double v, double a)
{
    return avg + (v - avg) * a;
}

void dec_init(cw_decoder *d, int wpm)
{
    memset(d, 0, sizeof *d);
    dec_set_speed(d, wpm, 0);
}

void dec_set_speed(cw_decoder *d, int wpm, int locked)
{
    double dit = 1200.0 / clampi(wpm, 5, 80);
    d->locked = locked;
    d->lock_dit_ms = dit;
    if (locked || !d->started) {
        d->dit_ms = dit;
        d->dah_ms = 3.0 * dit;
        d->ispace_ms = dit;
        d->have_ispace = 0;
    }
}

void dec_clear(cw_decoder *d)
{
    /* The next transition re-synchronises: a mark cut in half (e.g. by
     * muting the receiver) must not be classified. */
    d->started = 0;
    d->down = 0;
    d->plen = 0;
    d->pat[0] = 0;
    d->after_char = 0;
    d->space_sent = 0;
    d->qlen = 0;
}

double dec_unit_ms(const cw_decoder *d)
{
    double u;
    if (d->locked)
        return d->lock_dit_ms;
    /* Average of mark and space units cancels weighting errors. */
    if (d->have_ispace && d->ispace_ms > 0.5 * d->dit_ms && d->ispace_ms < 2.0 * d->dit_ms)
        u = 0.5 * (d->dit_ms + d->ispace_ms);
    else
        u = d->dit_ms;
    return clampd(u, 15.0, 400.0);
}

/* How much longer marks are than they should be (subtract from marks, add
 * to spaces). */
static double weight_bias(const cw_decoder *d)
{
    double u = dec_unit_ms(d);
    if (d->locked || !d->have_ispace || u == d->dit_ms)
        return 0.0;
    return clampd(0.5 * (d->dit_ms - d->ispace_ms), -0.4 * u, 0.4 * u);
}

double dec_wpm(const cw_decoder *d)
{
    return 1200.0 / dec_unit_ms(d);
}

double dec_ratio(const cw_decoder *d)
{
    return d->dit_ms > 0 ? d->dah_ms / d->dit_ms : 3.0;
}

const char *dec_pattern(const cw_decoder *d)
{
    return d->pat;
}

static void push(cw_decoder *d, int kind, const char *text, const char *code)
{
    dec_token *t;
    if (d->qlen >= DEC_QUEUE) {
        d->qhead = (d->qhead + 1) % DEC_QUEUE; /* drop the oldest */
        d->qlen--;
    }
    t = &d->q[(d->qhead + d->qlen) % DEC_QUEUE];
    t->kind = kind;
    str_copy(t->text, sizeof t->text, text);
    str_copy(t->code, sizeof t->code, code);
    d->qlen++;
}

int dec_pop(cw_decoder *d, dec_token *out)
{
    if (d->qlen == 0)
        return 0;
    *out = d->q[d->qhead];
    d->qhead = (d->qhead + 1) % DEC_QUEUE;
    d->qlen--;
    return 1;
}

static void emit_char(cw_decoder *d)
{
    const morse_entry *e;
    if (d->plen == 0)
        return;
    d->pat[d->plen] = 0;
    e = morse_decode(d->pat);
    if (e)
        push(d, e->group == MG_PROSIGN ? TOK_PROSIGN : TOK_CHAR, e->text, d->pat);
    else
        push(d, TOK_UNKNOWN, "*", d->pat);
    d->plen = 0;
    d->pat[0] = 0;
    d->after_char = 1;
    d->space_sent = 0;
}

static void ring_add(double *ring, int *n, int *head, double v)
{
    ring[*head] = v;
    *head = (*head + 1) % DEC_HIST;
    if (*n < DEC_HIST)
        (*n)++;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Sorted copy of a ring; returns the count. */
static int sorted(const double *ring, int n, double *out)
{
    memcpy(out, ring, sizeof(double) * (size_t)n);
    qsort(out, (size_t)n, sizeof(double), cmp_double);
    return n;
}

/*
 * Re-estimates dit and dah lengths from the recent marks: a two-cluster
 * k-means in the log domain. When all marks look alike, the recent short
 * spaces (element spaces, about one dit) tell dits from dahs.
 */
static void estimate_speed(cw_decoder *d)
{
    double v[DEC_HIST], lo, hi;
    int n = sorted(d->marks, d->nmarks, v), i, iter;

    if (n < 4)
        return;
    lo = log(v[n / 10]);
    hi = log(v[n - 1 - n / 10]);
    if (hi - lo >= log(MIN_RATIO)) {
        for (iter = 0; iter < 8; iter++) {
            double s1 = 0, s2 = 0, mid = 0.5 * (lo + hi);
            int n1 = 0, n2 = 0;
            for (i = 0; i < n; i++) {
                double l = log(v[i]);
                if (l < mid) {
                    s1 += l;
                    n1++;
                } else {
                    s2 += l;
                    n2++;
                }
            }
            if (n1 == 0 || n2 == 0)
                break;
            lo = s1 / n1;
            hi = s2 / n2;
        }
        if (hi - lo >= log(MIN_RATIO)) {
            d->dit_ms = ema(d->dit_ms, exp(lo), 0.5);
            d->dah_ms = ema(d->dah_ms, exp(hi), 0.5);
            return;
        }
    }

    /* Unimodal: all dits or all dahs. */
    {
        double m = v[n / 2], short_space = 0;
        int is_dit;
        if (d->nspaces >= 4) {
            double sp[DEC_HIST];
            int ns = sorted(d->spaces, d->nspaces, sp);
            short_space = sp[ns / 4];
        }
        if (short_space > 0)
            is_dit = m < 2.0 * short_space;
        else
            is_dit = fabs(log(m / d->dit_ms)) < fabs(log(m / d->dah_ms));
        if (is_dit) {
            d->dit_ms = ema(d->dit_ms, m, 0.5);
            d->dah_ms = ema(d->dah_ms, 3.0 * m, 0.5);
        } else {
            d->dah_ms = ema(d->dah_ms, m, 0.5);
            d->dit_ms = ema(d->dit_ms, m / 3.0, 0.5);
        }
    }
}

static void classify_mark(cw_decoder *d, double mark)
{
    double thr;
    int dah;

    if (mark < MIN_MARK_MS || mark > MAX_MARK_MS)
        return;
    /* Very short marks (noise bursts, contact bounce) still count as dits
     * but must not drag the speed estimate up. */
    if (!d->locked && mark > 0.3 * d->dit_ms) {
        ring_add(d->marks, &d->nmarks, &d->mhead, mark);
        estimate_speed(d);
        d->dit_ms = clampd(d->dit_ms, 12.0, 500.0);
        d->dah_ms = clampd(d->dah_ms, MIN_RATIO * d->dit_ms, 6.0 * d->dit_ms);
    }
    thr = d->locked ? 2.0 * d->lock_dit_ms : 0.5 * (d->dit_ms + d->dah_ms);
    dah = mark >= thr;

    if (d->plen >= DEC_MAX_ELEMS) {
        /* Too long for any character: flush it as unknown. */
        d->pat[d->plen] = 0;
        push(d, TOK_UNKNOWN, "*", d->pat);
        d->plen = 0;
    }
    d->pat[d->plen++] = dah ? '-' : '.';
    d->pat[d->plen] = 0;
}

void dec_update(cw_decoder *d, double now_ms)
{
    double gap, u;
    if (!d->started || d->down)
        return;
    u = dec_unit_ms(d);
    gap = now_ms - d->edge_ms + weight_bias(d);
    if (d->plen > 0 && gap > CHAR_GAP_UNITS * u)
        emit_char(d);
    if (d->plen == 0 && d->after_char && !d->space_sent && gap > WORD_GAP_UNITS * u) {
        push(d, TOK_SPACE, " ", "");
        d->space_sent = 1;
        d->after_char = 0;
    }
}

void dec_key(cw_decoder *d, int down, double t_ms)
{
    double dur;
    down = down ? 1 : 0;
    if (!d->started) {
        d->started = 1;
        d->down = down;
        d->edge_ms = t_ms;
        return;
    }
    if (down == d->down)
        return;

    dec_update(d, t_ms);
    dur = t_ms - d->edge_ms;
    if (down) {
        /* A space just ended. Element spaces (inside a character, about one
         * dit long) measure the weighting of the signal. */
        if (!d->locked && dur > MIN_MARK_MS && dur < 20.0 * d->dah_ms)
            ring_add(d->spaces, &d->nspaces, &d->shead, dur);
        if (d->plen > 0 && !d->locked && dur > 0.3 * d->dit_ms && dur < 2.0 * d->dit_ms) {
            d->ispace_ms = d->have_ispace ? ema(d->ispace_ms, dur, 0.3) : dur;
            d->have_ispace = 1;
        }
    } else {
        classify_mark(d, dur);
    }
    d->down = down;
    d->edge_ms = t_ms;
}
