/*
 * mode_stats.c - The statistics screen: the training records and progress
 * kept by stats.c, in two columns (receive, send) when there is room,
 * stacked and scrolling otherwise.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "cloud.h"
#include "theme.h"
#include "util.h"

static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char *const CHAR_ROWS[] = {"ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789.,?/=-()'\":;!@"};

#define LABEL_W 16 /* label column */
#define VALUE_W 7  /* values, right aligned */

static int content_h; /* height of everything, for scrolling */

static void fmt_date(int day, char *out, size_t cap)
{
    int y, m, d;
    if (day <= 0) {
        str_copy(out, cap, "");
        return;
    }
    stats_date(day, &y, &m, &d);
    snprintf(out, cap, "%d %s %d", d, MONTHS[clampi(m, 1, 12) - 1], y);
}

static void fmt_time(int seconds, char *out, size_t cap)
{
    if (seconds < 60)
        snprintf(out, cap, "%d s", seconds);
    else if (seconds < 3600)
        snprintf(out, cap, "%d min", seconds / 60);
    else
        snprintf(out, cap, "%dh %02dm", seconds / 3600, seconds / 60 % 60);
}

static int pct(int part, int whole)
{
    return whole > 0 ? (int)((long long)part * 100 / whole) : 0;
}

/* A section title with a rule to the right edge. */
static int heading(int x, int y, int w, const char *title, const char *note)
{
    int tw = scr_text(x, y, title, TH_ACCENT, COLOR_KEEP, A_BOLD);
    int nw = note && note[0] ? utf8_width(note) : 0;
    int rule = w - tw - nw - (nw ? 3 : 1);
    if (rule > 0)
        scr_hline(x + tw + 1, y, rule, 0x2500, TH_LINE, COLOR_KEEP);
    if (nw && tw + nw + 2 <= w)
        scr_text(x + w - nw, y, note, TH_FG_FAINT, COLOR_KEEP, 0);
    return y + 1;
}

/*
 * label · value · detail. The long detail if it fits, else the short one,
 * else none. A value of NULL is shown as a dash.
 */
static int row(int x, int y, int w, const char *label, const char *value, uint32_t col, const char *lng,
               const char *shrt, int marked)
{
    int room = w - LABEL_W - VALUE_W - 2;
    const char *detail = lng && utf8_width(lng) <= room ? lng : (shrt && utf8_width(shrt) <= room ? shrt : NULL);
    if (marked)
        scr_put(x, y, 0x25B8, TH_ACCENT, COLOR_KEEP, 0);
    scr_text(x + 2, y, label, marked ? TH_FG : TH_FG_MUTED, COLOR_KEEP, 0);
    if (!value) {
        scr_text(x + LABEL_W + VALUE_W - 1, y, "–", TH_FG_GHOST, COLOR_KEEP, 0);
        return y + 1;
    }
    scr_text(x + LABEL_W + VALUE_W - utf8_width(value), y, value, col, COLOR_KEEP, A_BOLD);
    if (detail)
        scr_text(x + LABEL_W + VALUE_W + 2, y, detail, TH_FG_FAINT, COLOR_KEEP, 0);
    return y + 1;
}

static int record_row(app_t *a, int x, int y, int w, const char *label, const stats_record *r, int marked)
{
    char value[16], lng[64], shrt[16], date[32];
    if (r->value <= 0)
        return row(x, y, w, label, NULL, TH_ACCENT, NULL, NULL, marked);
    snprintf(value, sizeof value, "%d", r->value);
    fmt_date(r->day, date, sizeof date);
    snprintf(shrt, sizeof shrt, "%d WPM", r->wpm);
    if (date[0])
        snprintf(lng, sizeof lng, "%d WPM  ·  %s", r->wpm, date);
    else
        str_copy(lng, sizeof lng, shrt);
    return row(x, y, w, label, value, TH_ACCENT, lng, shrt, marked);
}

/* Each character coloured by its accuracy, then the weakest ones. */
static int characters(app_t *a, int x, int y, int w, int dir)
{
    char weak[6], buf[32];
    static const char legend[] = "≥90%  ≥75%  less";
    int gap = w >= 2 + 26 * 2 ? 2 : 1, i, k, n, cx, lx = x + w - utf8_width(legend);
    y = heading(x, y, w, "Characters", legend);
    if (utf8_width("Characters") + utf8_width(legend) + 2 <= w) {
        /* The legend in its colours, over the faint note. */
        scr_text(lx, y - 1, "≥90%", TH_OK, COLOR_KEEP, 0);
        scr_text(lx + 6, y - 1, "≥75%", TH_ACCENT, COLOR_KEEP, 0);
        scr_text(lx + 12, y - 1, "less", TH_ERR, COLOR_KEEP, 0);
    }
    for (i = 0; i < ARRAY_LEN(CHAR_ROWS); i++, y++) {
        for (k = 0, cx = x + 2; CHAR_ROWS[i][k] && cx < x + w; k++, cx += gap) {
            int p = stats_char_pct(&a->book.total, dir, (unsigned char)CHAR_ROWS[i][k]);
            uint32_t col = p < 0 ? TH_FG_GHOST : (p >= 90 ? TH_OK : (p >= 75 ? TH_ACCENT : TH_ERR));
            scr_put(cx, y, (unsigned char)CHAR_ROWS[i][k], col, COLOR_KEEP, p < 0 ? 0 : A_BOLD);
        }
    }
    n = stats_weak(&a->book.total, dir, weak, (int)sizeof weak);
    cx = x + 2 + scr_text(x + 2, y, "Weakest", TH_FG_MUTED, COLOR_KEEP, 0) + 2;
    if (n == 0)
        scr_text(cx, y, "none yet", TH_FG_FAINT, COLOR_KEEP, 0);
    for (i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%d%%", stats_char_pct(&a->book.total, dir, (unsigned char)weak[i]));
        if (cx + 2 + utf8_width(buf) > x + w)
            break;
        scr_put(cx, y, (unsigned char)weak[i], TH_WARN, COLOR_KEEP, A_BOLD);
        cx += 2 + scr_text(cx + 2, y, buf, TH_FG_FAINT, COLOR_KEEP, 0) + 2;
    }
    return y + 1;
}

static int answers_row(int x, int y, int w, const stats_dir *d)
{
    char value[16], lng[32], shrt[16];
    snprintf(value, sizeof value, "%d", d->answers);
    snprintf(lng, sizeof lng, "%d%% correct", pct(d->correct, d->answers));
    snprintf(shrt, sizeof shrt, "%d%%", pct(d->correct, d->answers));
    return row(x, y, w, "Answers", d->answers ? value : NULL, TH_FG, lng, shrt, 0);
}

static int time_row(int x, int y, int w, const stats_dir *d)
{
    char value[24], lng[32];
    fmt_time(d->seconds, value, sizeof value);
    snprintf(lng, sizeof lng, "%d session%s", d->sessions, d->sessions == 1 ? "" : "s");
    return row(x, y, w, "Time", d->answers ? value : NULL, TH_FG, lng, NULL, 0);
}

static int column_copy(app_t *a, int x, int y, int w)
{
    const stats_t *s = &a->book.total;
    const stats_dir *d = &s->dir[STATS_COPY];
    char value[16], lng[48], label[16];
    int lv;

    y = heading(x, y, w, "RECEIVE", "copy by ear");
    scr_text(x + 2, y++, "Records, best streak", TH_FG_FAINT, COLOR_KEEP, 0);
    for (lv = 1; lv <= STATS_LEVELS; lv++) {
        snprintf(label, sizeof label, "Level %d", lv);
        y = record_row(a, x, y, w, label, stats_record_of(s, STATS_COPY, lv), lv == a->cfg.train_level);
    }
    y++;
    y = answers_row(x, y, w, d);
    snprintf(value, sizeof value, "%d%%", pct(s->copy_first, s->copy_items));
    y = row(x, y, w, "First listen", s->copy_items ? value : NULL, TH_FG, "items copied at once", NULL, 0);
    snprintf(value, sizeof value, "%.1f", s->copy_items ? (double)s->copy_listens / s->copy_items : 0.0);
    snprintf(lng, sizeof lng, "over %d item%s", s->copy_items, s->copy_items == 1 ? "" : "s");
    y = row(x, y, w, "Listens / item", s->copy_items ? value : NULL, TH_FG, lng, NULL, 0);
    snprintf(value, sizeof value, "%d", s->copy_failed);
    y = row(x, y, w, "Out of listens", s->copy_items ? value : NULL, TH_FG, "items lost", NULL, 0);
    y = time_row(x, y, w, d);
    y++;
    return characters(a, x, y, w, STATS_COPY);
}

static int column_send(app_t *a, int x, int y, int w, int beside)
{
    const stats_t *s = &a->book.total;
    const stats_dir *d = &s->dir[STATS_SEND];
    char value[16], lng[48];

    y = heading(x, y, w, "SEND", "keying");
    scr_text(x + 2, y++, "Record, best streak", TH_FG_FAINT, COLOR_KEEP, 0);
    y = record_row(a, x, y, w, "Streak", stats_record_of(s, STATS_SEND, 0), 0);
    snprintf(value, sizeof value, "%.1f", s->send_best10 / 10.0);
    y = row(x, y, w, "Best speed", s->send_timed ? value : NULL, TH_FG, "WPM, fastest correct item", "WPM", 0);
    snprintf(value, sizeof value, "%.1f", s->send_timed ? s->send_wpm10 / 10.0 / s->send_timed : 0.0);
    y = row(x, y, w, "Average speed", s->send_timed ? value : NULL, TH_FG, "WPM", NULL, 0);
    snprintf(value, sizeof value, "1:%.1f", s->send_timed ? s->send_ratio10 / 10.0 / s->send_timed : 0.0);
    snprintf(lng, sizeof lng, "1:3 is ideal");
    y = row(x, y, w, "Dah/dit ratio", s->send_timed ? value : NULL, TH_FG, lng, NULL, 0);
    y++;
    y = answers_row(x, y, w, d);
    y = time_row(x, y, w, d);
    y += beside ? 4 : 1; /* level with the receive column */
    return characters(a, x, y, w, STATS_SEND);
}

/* Answers per day over the last four weeks, as two rows of bars. */
static int activity(app_t *a, int x, int y, int w)
{
    const stats_t *s = &a->book.total;
    char note[160], koch[32] = "";
    int i, max = 1, total = 0, bw, step, bx, today = a->today;

    for (i = 0; i < STATS_DAYS; i++) {
        int n = stats_activity(s, today, i);
        max = maxi(max, n);
        total += n;
    }
    if (s->koch > 0)
        snprintf(koch, sizeof koch, "  ·  Koch %d", s->koch);
    snprintf(note, sizeof note, "%d day%s practised  ·  streak %d (best %d)%s", s->days, s->days == 1 ? "" : "s",
             stats_day_streak(s, today), s->best_day_streak, koch);
    if (utf8_width(note) + 10 > w)
        snprintf(note, sizeof note, "%d day%s  ·  streak %d", s->days, s->days == 1 ? "" : "s",
                 stats_day_streak(s, today));
    y = heading(x, y, w, "PRACTICE", note);

    step = clampi((w - 2) / STATS_DAYS, 1, 4);
    bw = step > 1 ? step - 1 : 1;
    bx = x + 2;
    for (i = STATS_DAYS - 1; i >= 0; i--, bx += step) {
        int n = stats_activity(s, today, i), h = n > 0 ? maxi(1, (n * 16 + max - 1) / max) : 0, k;
        uint32_t col = i == 0 ? TH_ACCENT_HI : TH_ACCENT_MID;
        for (k = 0; k < bw; k++) {
            if (h == 0) {
                scr_put(bx + k, y + 1, 0x2581, TH_FG_GHOST, COLOR_KEEP, 0);
                continue;
            }
            if (h > 8)
                scr_put(bx + k, y, 0x2580 + (uint32_t)(h - 8), col, COLOR_KEEP, 0);
            scr_put(bx + k, y + 1, 0x2580 + (uint32_t)mini(h, 8), col, COLOR_KEEP, 0);
        }
    }
    /* The axis: four weeks ago, the answers, today. */
    {
        int right = x + 2 + STATS_DAYS * step - (step - bw);
        char mid[64];
        scr_text(x + 2, y + 2, "4 weeks ago", TH_FG_FAINT, COLOR_KEEP, 0);
        scr_text(right - 5, y + 2, "today", TH_FG_FAINT, COLOR_KEEP, 0);
        snprintf(mid, sizeof mid, "%d answers", total);
        if (right - x - 2 > utf8_width(mid) + 28)
            scr_text(x + 2 + (right - x - 2 - utf8_width(mid)) / 2, y + 2, mid, TH_FG_MUTED, COLOR_KEEP, 0);
    }
    return y + 3;
}

void stats_render(app_t *a, rect_t r)
{
    const stats_t *s = &a->book.total;
    rect_t in = rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    char info[96] = "", date[32], sync[40] = "";
    int y, top, max_scroll;

    /* The cloud, then the first day. */
    if (cloud_state() == CLOUD_BUSY && cloud_email()[0])
        str_copy(sync, sizeof sync, "syncing…");
    else if (cloud_state() == CLOUD_READY && a->cloud_failed)
        str_copy(sync, sizeof sync, "sync failed");
    else if (cloud_state() == CLOUD_READY && cloud_synced()[0])
        snprintf(sync, sizeof sync, "synced %s", cloud_synced());
    fmt_date(s->first_day, date, sizeof date);
    if (date[0])
        snprintf(info, sizeof info, "since %s%s%s", date, sync[0] ? "  ·  " : "", sync);
    else
        str_copy(info, sizeof info, sync);
    ui_panel(r, "Statistics", info, 0);

    if (s->dir[STATS_COPY].answers + s->dir[STATS_SEND].answers == 0) {
        int cy = in.y + in.h / 2 - 2;
        const char *l1 = "No training yet";
        const char *l2 = "Records, accuracy and progress show up here once you answer in Training.";
        scr_text(in.x + (in.w - utf8_width(l1)) / 2, cy, l1, TH_ACCENT, COLOR_KEEP, A_BOLD);
        if (utf8_width(l2) <= in.w)
            scr_text(in.x + (in.w - utf8_width(l2)) / 2, cy + 2, l2, TH_FG_MUTED, COLOR_KEEP, 0);
        scr_text(in.x + (in.w - 22) / 2, cy + 4, " p ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
        scr_text(in.x + (in.w - 22) / 2 + 4, cy + 4, "start training", TH_FG, COLOR_KEEP, 0);
        scr_text(in.x + (in.w - 22) / 2, cy + 6, " i ", TH_FG, TH_SURFACE_3, A_BOLD);
        scr_text(in.x + (in.w - 22) / 2 + 4, cy + 6, "import statistics", TH_FG_MUTED, COLOR_KEEP, 0);
        scr_text(in.x + (in.w - 22) / 2, cy + 7, " c ", TH_FG, TH_SURFACE_3, A_BOLD);
        scr_text(in.x + (in.w - 22) / 2 + 4, cy + 7,
                 cloud_state() == CLOUD_OFF ? "sync with Filen" : "Filen cloud", TH_FG_MUTED, COLOR_KEEP, 0);
        content_h = 0;
        return;
    }

    max_scroll = maxi(0, content_h - in.h);
    a->stats_scroll = clampi(a->stats_scroll, 0, max_scroll);
    top = in.y - a->stats_scroll;

    scr_clip(in);
    y = activity(a, in.x, top, in.w);
    y++;
    if (in.w >= 2 * 34 + 3) {
        int cw = (in.w - 3) / 2;
        int yl = column_copy(a, in.x, y, cw);
        int yr = column_send(a, in.x + cw + 3, y, in.w - cw - 3, 1);
        y = maxi(yl, yr);
    } else {
        y = column_copy(a, in.x, y, in.w);
        y = column_send(a, in.x, y + 1, in.w, 0);
    }
    scr_unclip();
    content_h = y - top;

    max_scroll = maxi(0, content_h - in.h);
    if (a->stats_scroll > 0)
        scr_put(r.x + r.w - 1, r.y + 1, 0x25B4, TH_ACCENT, TH_BG, 0);
    if (a->stats_scroll < max_scroll)
        scr_put(r.x + r.w - 1, r.y + r.h - 2, 0x25BE, TH_ACCENT, TH_BG, 0);
}

int stats_key(app_t *a, const key_event *ev)
{
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    switch (ev->key) {
    case KEY_UP: a->stats_scroll--; break;
    case KEY_DOWN: a->stats_scroll++; break;
    case KEY_PGUP: a->stats_scroll -= 10; break;
    case KEY_PGDN:
    case KEY_SPACE: a->stats_scroll += 10; break;
    case KEY_HOME: a->stats_scroll = 0; break;
    case KEY_END: a->stats_scroll = 1000; break;
    case 'p':
    case KEY_ENTER: app_set_mode(a, MODE_TRAIN); return 1;
    case 'e': app_export_stats(a); return 1;
    case 'c': app_open_modal(a, MODAL_CLOUD); return 1;
    case 'i': app_open_modal(a, MODAL_IMPORT); return 1;
    default: return 0;
    }
    a->stats_scroll = clampi(a->stats_scroll, 0, 1000);
    return 1;
}
