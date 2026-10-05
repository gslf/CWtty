/*
 * ui.c - Reusable widgets drawn with the cell renderer.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "util.h"

#define BLOCK_FULL 0x2588u
#define LEFT_BLOCK(n) (0x2590u - (uint32_t)(n)) /* n eighths, 1..8 */

/* ------------------------------------------------------------------------ */
/* Timeline                                                                  */
/* ------------------------------------------------------------------------ */

void tl_push(timeline *tl, double t_ms, int on)
{
    int idx;
    if (tl->count > 0) {
        int last = (tl->head + tl->count - 1) % TL_CAP;
        if (tl->on[last] == (on ? 1 : 0))
            return;
    }
    if (tl->count == TL_CAP) {
        tl->head = (tl->head + 1) % TL_CAP;
        tl->count--;
    }
    idx = (tl->head + tl->count) % TL_CAP;
    tl->t[idx] = t_ms;
    tl->on[idx] = on ? 1 : 0;
    tl->count++;
}

int tl_state(const timeline *tl)
{
    if (tl->count == 0)
        return 0;
    return tl->on[(tl->head + tl->count - 1) % TL_CAP];
}

/* ------------------------------------------------------------------------ */
/* Chrome                                                                    */
/* ------------------------------------------------------------------------ */

rect_t ui_center(int w, int h)
{
    w = mini(w, scr_w() - 2);
    h = mini(h, scr_h() - 2);
    return rect_make((scr_w() - w) / 2, (scr_h() - h) / 2, w, h);
}

void ui_panel(rect_t r, const char *title, const char *info, int highlight)
{
    uint32_t line = highlight ? TH_ACCENT_MID : TH_LINE;
    if (r.w < 4 || r.h < 2)
        return;
    scr_fill(rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2), ' ', TH_FG, TH_SURFACE);
    scr_box(r, BOX_ROUND, line, TH_BG);
    if (title && *title) {
        int x = r.x + 2;
        scr_put(x++, r.y, ' ', TH_FG, TH_BG, 0);
        x += scr_text(x, r.y, title, highlight ? TH_ACCENT_HI : TH_ACCENT, TH_BG, A_BOLD);
        scr_put(x, r.y, ' ', TH_FG, TH_BG, 0);
    }
    if (info && *info) {
        int w = utf8_width(info);
        int x = r.x + r.w - 3 - w;
        if (x > r.x + 4 + (title ? utf8_width(title) + 2 : 0)) {
            scr_put(x - 1, r.y, ' ', TH_FG, TH_BG, 0);
            scr_text(x, r.y, info, TH_FG_MUTED, TH_BG, 0);
            scr_put(x + w, r.y, ' ', TH_FG, TH_BG, 0);
        }
    }
}

void ui_backdrop(float k)
{
    scr_shade(rect_make(0, 0, scr_w(), scr_h()), k);
}

rect_t ui_modal(rect_t r, const char *title, const char *hint)
{
    int x;
    /* Soft drop shadow, two columns wide to look square. */
    scr_shade(rect_make(r.x + 2, r.y + r.h, r.w, 1), 0.35f);
    scr_shade(rect_make(r.x + r.w, r.y + 1, 2, r.h - 1), 0.35f);

    scr_fill(r, ' ', TH_FG, TH_SURFACE_2);
    scr_box(r, BOX_ROUND, TH_LINE_HI, TH_SURFACE_2);

    /* Header bar. */
    scr_fill(rect_make(r.x + 1, r.y + 1, r.w - 2, 1), ' ', TH_FG, TH_SURFACE_3);
    scr_put(r.x + 1, r.y + 1, 0x258C, TH_ACCENT, TH_SURFACE_3, 0); /* ▌ */
    x = r.x + 3;
    scr_text(x, r.y + 1, title, TH_FG, TH_SURFACE_3, A_BOLD);
    if (hint && *hint) {
        int w = utf8_width(hint);
        scr_text(r.x + r.w - 2 - w, r.y + 1, hint, TH_FG_FAINT, TH_SURFACE_3, 0);
    }
    return rect_make(r.x + 1, r.y + 2, r.w - 2, r.h - 3);
}

int ui_hint(int x, int y, const char *key, const char *label, uint32_t bg)
{
    int x0 = x;
    scr_put(x++, y, ' ', TH_ACCENT, TH_SURFACE_3, 0);
    x += scr_text(x, y, key, TH_ACCENT, TH_SURFACE_3, A_BOLD);
    scr_put(x++, y, ' ', TH_ACCENT, TH_SURFACE_3, 0);
    if (label && *label) {
        scr_put(x++, y, ' ', TH_FG_MUTED, bg, 0);
        x += scr_text(x, y, label, TH_FG_MUTED, bg, 0);
    }
    return x - x0;
}

void ui_keycap(rect_t r, const char *label, int active)
{
    uint32_t face = active ? TH_ACCENT : TH_SURFACE_2;
    uint32_t edge = active ? TH_ACCENT_HI : TH_LINE_HI;
    uint32_t text = active ? TH_ON_ACCENT : TH_FG_MUTED;
    int w = utf8_width(label);
    scr_box(r, BOX_ROUND, edge, COLOR_KEEP);
    scr_fill(rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2), ' ', text, face);
    scr_text(r.x + (r.w - w) / 2, r.y + r.h / 2, label, text, face, A_BOLD);
}

void ui_hbar(int x, int y, int w, double frac, uint32_t fg, uint32_t bg)
{
    int eighths = (int)(clampd(frac, 0, 1) * w * 8 + 0.5), i;
    for (i = 0; i < w; i++) {
        int n = eighths - i * 8;
        if (n >= 8)
            scr_put(x + i, y, BLOCK_FULL, fg, bg, 0);
        else if (n > 0)
            scr_put(x + i, y, LEFT_BLOCK(n), fg, bg, 0);
        else
            scr_put(x + i, y, ' ', fg, bg, 0);
    }
}

int ui_pattern_width(const char *code)
{
    int w = 0;
    for (; *code; code++)
        w += (*code == '-' ? 3 : 1) + (code[1] ? 1 : 0);
    return w;
}

int ui_pattern(int x, int y, const char *code, uint32_t fg, uint32_t bg)
{
    int x0 = x;
    for (; *code; code++) {
        int n = *code == '-' ? 3 : 1, i;
        for (i = 0; i < n; i++)
            scr_put(x++, y, 0x2501, fg, bg, 0); /* ━ */
        if (code[1])
            scr_put(x++, y, ' ', fg, bg, 0);
    }
    return x - x0;
}

/* ------------------------------------------------------------------------ */
/* Tape                                                                      */
/* ------------------------------------------------------------------------ */

void ui_ribbon(rect_t r, const timeline *tl, double now_ms, double ms_per_col, uint32_t on, uint32_t bg)
{
    int col, row, idx = 0, state = 0, sub;
    double t0 = now_ms - r.w * ms_per_col, step = ms_per_col / 8.0;

    /* State at t0: last transition at or before it. */
    while (idx < tl->count && tl->t[(tl->head + idx) % TL_CAP] <= t0) {
        state = tl->on[(tl->head + idx) % TL_CAP];
        idx++;
    }
    for (col = 0; col < r.w; col++) {
        unsigned bits = 0;
        int count = 0;
        uint32_t ch, fg = on, cbg = bg;
        for (sub = 0; sub < 8; sub++) {
            double ts = t0 + (col * 8 + sub + 0.5) * step;
            while (idx < tl->count && tl->t[(tl->head + idx) % TL_CAP] <= ts) {
                state = tl->on[(tl->head + idx) % TL_CAP];
                idx++;
            }
            if (state) {
                bits |= 1u << sub;
                count++;
            }
        }
        if (count == 8) {
            ch = BLOCK_FULL;
        } else if (count == 0) {
            ch = ' ';
        } else if (bits == (1u << count) - 1u) {
            ch = LEFT_BLOCK(count); /* key released inside the cell */
        } else if (bits == (0xFFu << (8 - count) & 0xFFu)) {
            ch = LEFT_BLOCK(8 - count); /* key pressed inside the cell */
            fg = bg;
            cbg = on;
        } else {
            ch = BLOCK_FULL; /* several edges: show the average */
            fg = rgb_mix(bg, on, (float)count / 8.0f);
        }
        for (row = 0; row < r.h; row++)
            scr_put(r.x + col, r.y + row, ch, fg, cbg, 0);
    }
}

void ui_sparkline(rect_t r, const rx_dsp *d, double ms_per_col, uint32_t bg)
{
    static const uint32_t LEVELS[8] = {0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};
    double now = dsp_time_ms(d);
    float range = d->pk_db - d->nf_db + 4.0f;
    int col;
    if (range < 12.0f)
        range = 12.0f;
    for (col = 0; col < r.w; col++) {
        double ta = now - (r.w - col) * ms_per_col, t;
        float best = -200.0f, v;
        uint32_t fg;
        for (t = ta; t < ta + ms_per_col; t += 1.0) {
            float e = dsp_env_at(d, t);
            if (e > best)
                best = e;
        }
        v = (best - d->nf_db + 2.0f) / range;
        if (best < -150.0f || v <= 0.02f) {
            scr_put(r.x + col, r.y, ' ', TH_FG, bg, 0);
            continue;
        }
        v = clampf(v, 0.0f, 1.0f);
        fg = best >= d->thr_db ? (d->open ? TH_ACCENT : TH_FG_MUTED) : TH_ACCENT_LO;
        scr_put(r.x + col, r.y, LEVELS[(int)(v * 7.0f + 0.5f)], fg, bg, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Waterfall                                                                 */
/* ------------------------------------------------------------------------ */

static uint32_t heat(float v)
{
    static const uint32_t stops[] = {0x0E0F11, 0x2A1F05, 0x6E4E00, 0xD49C00, 0xFFD54A, 0xFFF6D6};
    float x = clampf(v, 0.0f, 1.0f) * 5.0f;
    int i = (int)x;
    if (i >= 5)
        return stops[5];
    return rgb_mix(stops[i], stops[i + 1], x - (float)i);
}

static float column_db(const float *row, float f0, float f1)
{
    int k0 = (int)floorf(f0 / DSP_BIN_HZ), k1 = (int)ceilf(f1 / DSP_BIN_HZ), k;
    float best = -200.0f;
    if (k0 < 0)
        k0 = 0;
    if (k1 >= DSP_BINS)
        k1 = DSP_BINS - 1;
    if (k1 <= k0) {
        /* Narrower than a bin: interpolate. */
        float fk = 0.5f * (f0 + f1) / DSP_BIN_HZ;
        int a = clampi((int)fk, 0, DSP_BINS - 2);
        float t = fk - (float)a;
        return row[a] * (1.0f - t) + row[a + 1] * t;
    }
    for (k = k0; k <= k1; k++)
        if (row[k] > best)
            best = row[k];
    return best;
}

void ui_waterfall(rect_t r, const rx_dsp *d, float fmin, float fmax)
{
    float df = (fmax - fmin) / (float)r.w;
    int row, col, center = (int)((d->freq - fmin) / df);
    for (row = 0; row < r.h; row++) {
        const float *top = dsp_wf_row(d, row * 2);
        const float *bot = dsp_wf_row(d, row * 2 + 1);
        for (col = 0; col < r.w; col++) {
            float f0 = fmin + col * df, f1 = f0 + df;
            float vt = top ? (column_db(top, f0, f1) - d->spec_floor - 3.0f) / 32.0f : 0.0f;
            float vb = bot ? (column_db(bot, f0, f1) - d->spec_floor - 3.0f) / 32.0f : 0.0f;
            uint32_t ct = heat(vt), cb = heat(vb);
            if (col == center && vt < 0.2f && vb < 0.2f)
                scr_put(r.x + col, r.y + row, 0x250A, TH_ACCENT_LO, cb, 0); /* ┊ tuning line */
            else
                scr_put(r.x + col, r.y + row, 0x2580, ct, cb, 0); /* ▀ */
        }
    }
}

void ui_freq_axis(rect_t r, float fmin, float fmax, float tuned, float bw, uint32_t bg)
{
    float df = (fmax - fmin) / (float)r.w;
    int col, f;
    char label[16];
    for (col = 0; col < r.w; col++) {
        float f0 = fmin + col * df, f1 = f0 + df, fc = 0.5f * (f0 + f1);
        int in_band = fc >= tuned - bw * 0.5f && fc <= tuned + bw * 0.5f;
        int is_center = tuned >= f0 && tuned < f1;
        uint32_t ch = in_band ? 0x2501 : 0x2500; /* ━ ─ */
        uint32_t fg = in_band ? TH_ACCENT : TH_LINE;
        int tick = 0;
        for (f = 100; f <= 3000; f += 100)
            if (f >= f0 && f < f1)
                tick = f;
        if (is_center) {
            ch = 0x253B; /* ┻ */
            fg = TH_ACCENT_HI;
        } else if (tick && !in_band) {
            ch = tick % 200 == 0 ? 0x2534 : 0x2500; /* ┴ */
            fg = TH_LINE_HI;
        }
        scr_put(r.x + col, r.y, ch, fg, bg, 0);
    }
    if (r.h < 2)
        return;
    /* Labels every 200 Hz, skipping those that collide with the tuning. */
    {
        int tx = r.x + (int)((tuned - fmin) / df);
        int tw;
        snprintf(label, sizeof label, "%.0f Hz", tuned);
        tw = (int)strlen(label);
        for (f = 200; f <= 3000; f += 200) {
            int x, w;
            if (f < fmin || f > fmax)
                continue;
            snprintf(label, sizeof label, "%d", f);
            w = (int)strlen(label);
            x = r.x + (int)((f - fmin) / df) - w / 2;
            if (x < r.x || x + w > r.x + r.w)
                continue;
            if (x + w >= tx - tw / 2 - 1 && x <= tx + tw / 2 + 1)
                continue;
            scr_text(x, r.y + 1, label, TH_FG_FAINT, bg, 0);
        }
        snprintf(label, sizeof label, "%.0f Hz", tuned);
        scr_text(clampi(tx - tw / 2, r.x, r.x + r.w - tw), r.y + 1, label, TH_ACCENT, bg, A_BOLD);
    }
}

/* ------------------------------------------------------------------------ */
/* Transcript                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    int line;
    int start, len;
    int first; /* first visual row of the line */
} vrow;

/* Splits a line in rows of at most w characters, breaking at spaces. */
static int wrap_line(const tr_line *l, int w, int *starts, int *lens, int cap)
{
    int pos = 0, n = 0;
    if (l->len == 0) {
        starts[0] = 0;
        lens[0] = 0;
        return 1;
    }
    while (pos < l->len && n < cap) {
        int take = mini(w, l->len - pos);
        if (pos + take < l->len) {
            int k;
            for (k = take; k > take / 2; k--) {
                if (l->text[pos + k] == ' ') {
                    take = k;
                    break;
                }
            }
        }
        starts[n] = pos;
        lens[n] = take;
        n++;
        pos += take;
        while (pos < l->len && l->text[pos] == ' ')
            pos++;
    }
    return n;
}

void ui_transcript(rect_t r, const transcript *t, int labels, int cursor_dir, int cursor_on)
{
    enum { MAXROWS = 256 };
    static vrow rows[MAXROWS];
    int gutter = labels ? 5 : 0;
    int tw = r.w - gutter, nrows = 0, li, y, i;
    if (tw < 4 || r.h < 1)
        return;

    /* Collect visual rows from the newest line backwards. */
    for (li = transcript_count(t) - 1; li >= 0 && nrows < r.h; li--) {
        const tr_line *l = transcript_line(t, li);
        int starts[128], lens[128];
        int n = wrap_line(l, tw, starts, lens, 128), k;
        for (k = n - 1; k >= 0 && nrows < MAXROWS; k--) {
            rows[nrows].line = li;
            rows[nrows].start = starts[k];
            rows[nrows].len = lens[k];
            rows[nrows].first = k == 0;
            nrows++;
        }
    }

    for (i = 0; i < nrows && i < r.h; i++) {
        const vrow *v = &rows[i];
        const tr_line *l = transcript_line(t, v->line);
        uint32_t base = l->dir == DIR_TX ? TH_TX : (l->dir == DIR_RX ? TH_RX : TH_FG_FAINT);
        int x = r.x + gutter, k;
        y = r.y + r.h - 1 - i;
        if (labels) {
            if (v->first && l->dir != DIR_SYS) {
                scr_text(r.x, y, l->dir == DIR_TX ? "TX" : "RX", l->dir == DIR_TX ? TH_ACCENT : TH_FG_MUTED,
                         COLOR_KEEP, A_BOLD);
            }
            scr_put(r.x + 3, y, 0x2502, l->dir == DIR_TX ? TH_ACCENT_LO : TH_LINE, COLOR_KEEP, 0);
        }
        for (k = 0; k < v->len; k++) {
            int s = l->style[v->start + k];
            uint32_t fg = base;
            int attr = 0;
            if (s == STY_PROSIGN) {
                fg = rgb_mix(base, TH_SURFACE, 0.35f);
                attr = A_UNDERLINE;
            } else if (s == STY_UNKNOWN) {
                fg = TH_ERR;
            }
            if (l->dir == DIR_SYS)
                attr |= A_ITALIC;
            scr_put(x + k, y, (unsigned char)l->text[v->start + k], fg, COLOR_KEEP, attr);
        }
        /* Cursor after the very last character. */
        if (i == 0 && cursor_on && l->dir == cursor_dir && !l->closed && x + v->len < r.x + r.w)
            scr_put(x + v->len, y, 0x258F, cursor_dir == DIR_TX ? TH_ACCENT : TH_RX, COLOR_KEEP, 0);
    }
    if (nrows == 0 && cursor_on)
        scr_put(r.x + gutter, r.y + r.h - 1, 0x258F, cursor_dir == DIR_TX ? TH_ACCENT : TH_RX, COLOR_KEEP, 0);
}
