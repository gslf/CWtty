/*
 * modals.c - Modal windows: the C-x command menu, the Morse table (which
 * floats over the running activity without stopping it), settings and
 * help.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "keyer.h"
#include "morse.h"
#include "theme.h"
#include "util.h"

#ifndef CWTTY_VERSION
#define CWTTY_VERSION "dev"
#endif

static int is_cancel(const key_event *ev)
{
    return ev->key == KEY_ESC || ((ev->mods & MOD_CTRL) && ev->key == 'g');
}

/* ======================================================================== */
/* Command menu                                                             */
/* ======================================================================== */

typedef struct {
    char        key;
    const char *name;
    const char *desc;
    int         mode; /* MODE_* for screens, -1 for tools */
} menu_item;

static const menu_item MENU[] = {
    {'t', "Transmit", "Key with Space and the arrow paddles", MODE_TX},
    {'r', "Receive", "Decode CW from the microphone", MODE_RX},
    {'x', "Transceiver", "Listen, break in to transmit", MODE_TRX},
    {'p', "Training", "Practice copying and sending", MODE_TRAIN},
    {'m', "Morse table", "Floating reference, keeps running", -1},
    {'s', "Settings", "Tone, speed, keyer, receiver, audio", -1},
    {'h', "Help", "Keys and tips", -1},
    {'q', "Quit", "also C-x C-c", -1},
};
#define MENU_TOOLS 4 /* index of the first tool */

static void menu_run(app_t *a, int i)
{
    app_close_modal(a); /* the menu itself */
    switch (MENU[i].key) {
    case 'm':
        if (app_has_modal(a, MODAL_TABLE)) {
            while (app_top_modal(a) != MODAL_TABLE)
                app_close_modal(a);
            app_close_modal(a);
        } else {
            app_open_modal(a, MODAL_TABLE);
        }
        break;
    case 's':
        app_open_modal(a, MODAL_SETTINGS);
        break;
    case 'h':
        app_open_modal(a, MODAL_HELP);
        break;
    case 'q':
        a->quit = 1;
        break;
    default:
        app_set_mode(a, MENU[i].mode);
        break;
    }
}

static void menu_render(app_t *a)
{
    int n = ARRAY_LEN(MENU), i, y;
    int name_w = 0, desc_w = 0, desc_x;
    rect_t r, body;

    for (i = 0; i < n; i++) {
        name_w = maxi(name_w, utf8_width(MENU[i].name));
        desc_w = maxi(desc_w, utf8_width(MENU[i].desc));
    }
    desc_x = 8 + name_w + 1;
    r = ui_center(desc_x + desc_w + 6, n + 9);

    body = ui_modal(r, "Commands", "C-g cancel");
    y = body.y + 1;
    for (i = 0; i < n; i++) {
        int sel = i == a->menu_sel, x = body.x + 2;
        uint32_t bg = sel ? TH_ACCENT_BG : TH_SURFACE_2;
        char chip[4] = {' ', MENU[i].key, ' ', 0};
        if (i == 0 || i == MENU_TOOLS) {
            scr_text(body.x + 3, y, i == 0 ? "SCREENS" : "TOOLS", TH_FG_FAINT, TH_SURFACE_2, A_BOLD);
            y++;
        }
        scr_fill(rect_make(body.x + 1, y, body.w - 2, 1), ' ', TH_FG, bg);
        if (sel)
            scr_put(body.x + 1, y, 0x258C, TH_ACCENT, bg, 0);
        scr_text(x + 1, y, chip, sel ? TH_ON_ACCENT : TH_ACCENT, sel ? TH_ACCENT : TH_SURFACE_3, A_BOLD);
        scr_text(x + 6, y, MENU[i].name, sel ? TH_ACCENT_HI : TH_FG, bg, A_BOLD);
        scr_clip(rect_make(body.x + desc_x, y, maxi(0, body.w - desc_x - 4), 1));
        scr_text(body.x + desc_x, y, MENU[i].desc, sel ? TH_FG_MUTED : TH_FG_FAINT, bg, 0);
        scr_unclip();
        if ((MENU[i].mode >= 0 && MENU[i].mode == a->mode) ||
            (MENU[i].key == 'm' && app_has_modal(a, MODAL_TABLE)))
            scr_put(body.x + body.w - 3, y, 0x25CF, TH_ACCENT, bg, 0);
        y++;
    }
}

static int menu_key(app_t *a, const key_event *ev)
{
    int n = ARRAY_LEN(MENU), i;
    if (is_cancel(ev)) {
        app_close_modal(a);
        return 1;
    }
    if ((ev->mods & MOD_CTRL) && ev->key == 'c') { /* C-x C-c */
        a->quit = 1;
        return 1;
    }
    switch (ev->key) {
    case KEY_UP:
        a->menu_sel = (a->menu_sel + n - 1) % n;
        return 1;
    case KEY_DOWN:
    case KEY_TAB:
        a->menu_sel = (a->menu_sel + 1) % n;
        return 1;
    case KEY_ENTER:
        menu_run(a, a->menu_sel);
        return 1;
    default:
        break;
    }
    /* A letter, with or without Ctrl (C-x C-t works like C-x t). */
    for (i = 0; i < n; i++) {
        if (ev->key == MENU[i].key) {
            menu_run(a, i);
            return 1;
        }
    }
    return 1; /* the menu is modal: swallow everything else */
}

/* ======================================================================== */
/* Morse table                                                              */
/* ======================================================================== */

typedef struct {
    int group;
    int first, count; /* range in MORSE_TABLE */
    int col_w;
} section;

static int collect_sections(section *s)
{
    int i, n = 0;
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        int g = MORSE_TABLE[i].group;
        if (n == 0 || s[n - 1].group != g) {
            s[n].group = g;
            s[n].first = i;
            s[n].count = 0;
            s[n].col_w = g == MG_PROSIGN ? 17 : (g == MG_PUNCT ? 12 : (g == MG_DIGIT ? 10 : 9));
            n++;
        }
        s[n - 1].count++;
    }
    return n;
}

static void table_render(app_t *a)
{
    static const char *const GROUP_NAMES[MG_COUNT] = {"LETTERS", "NUMBERS", "PUNCTUATION", "PROSIGNS"};
    section sec[MG_COUNT];
    int nsec = collect_sections(sec), rows = 13, i, k;
    int total_w = 0, page_split = nsec, page, start, end, x, y;
    const char *live = app_live_pattern(a);
    const char *last = a->rx_on && a->rx_last_at > a->tx_last_at ? a->rx_last : a->tx_last;
    double last_at = a->rx_on && a->rx_last_at > a->tx_last_at ? a->rx_last_at : a->tx_last_at;
    rect_t r, body;
    char hint[48];

    for (i = 0; i < nsec; i++)
        total_w += sec[i].col_w * ((sec[i].count + rows - 1) / rows) + 2;
    /* Two pages when the screen is too narrow for everything. */
    if (total_w + 4 > scr_w() - 2)
        page_split = 2;
    page = page_split < nsec ? a->table_scroll % 2 : 0;
    start = page == 0 ? 0 : page_split;
    end = page == 0 ? page_split : nsec;

    total_w = 0;
    for (i = start; i < end; i++)
        total_w += sec[i].col_w * ((sec[i].count + rows - 1) / rows) + 2;
    snprintf(hint, sizeof hint, page_split < nsec ? "Tab page %d/2  Esc close" : "Esc close", page + 1);
    r = rect_make((scr_w() - (total_w + 4)) / 2, scr_h() >= 26 ? 2 : 1, total_w + 4, rows + 6);
    body = ui_modal(r, "Morse table", hint);

    x = body.x + 2;
    for (i = start; i < end; i++) {
        y = body.y + 1;
        scr_text(x, y, GROUP_NAMES[sec[i].group], TH_FG_FAINT, TH_SURFACE_2, A_BOLD);
        for (k = 0; k < sec[i].count; k++) {
            const morse_entry *e = &MORSE_TABLE[sec[i].first + k];
            int cx = x + (k / rows) * sec[i].col_w, cy = y + 2 + k % rows;
            int prefix = live[0] && strncmp(e->code, live, strlen(live)) == 0;
            int exact = live[0] && strcmp(e->code, live) == 0;
            int recent = !live[0] && last[0] && strcmp(e->text, last) == 0 && a->now - last_at < 2500.0;
            uint32_t bg = exact || recent ? TH_ACCENT : (prefix ? TH_ACCENT_BG : TH_SURFACE_2);
            uint32_t tfg = exact || recent ? TH_ON_ACCENT : (prefix ? TH_ACCENT_HI : TH_FG);
            uint32_t pfg = exact || recent ? TH_ON_ACCENT : (prefix ? TH_ACCENT : TH_FG_MUTED);
            char pretty[48];
            if (live[0] && !prefix) {
                tfg = TH_FG_FAINT;
                pfg = TH_FG_GHOST;
            }
            morse_pretty(e->code, pretty, sizeof pretty);
            scr_fill(rect_make(cx - 1, cy, sec[i].col_w - 1, 1), ' ', TH_FG, bg);
            scr_text(cx, cy, e->text, tfg, bg, A_BOLD);
            scr_text(cx + (sec[i].group == MG_PROSIGN ? 6 : 3), cy, pretty, pfg, bg, 0);
        }
        x += sec[i].col_w * ((sec[i].count + rows - 1) / rows) + 2;
    }
}

static int table_key(app_t *a, const key_event *ev)
{
    if (is_cancel(ev)) {
        app_close_modal(a);
        return 1;
    }
    if (ev->key == KEY_PGDN || ev->key == KEY_PGUP) {
        a->table_scroll++;
        return 1;
    }
    if (ev->key == KEY_TAB && !(a->mode == MODE_TRAIN && a->cfg.train_dir == TRAIN_SEND)) {
        a->table_scroll++;
        return 1;
    }
    return 0; /* everything else reaches the screen below */
}

/* ======================================================================== */
/* Settings                                                                 */
/* ======================================================================== */

enum { F_HEADER, F_INT, F_ENUM, F_BOOL, F_DEVICE };

typedef struct {
    int         kind;
    const char *label;
    int        *value;
    int         min, max, step;
    const char *unit;
    const char *const *names; /* F_ENUM */
    int         audio;        /* re-open the audio devices on close */
    int         capture;      /* F_DEVICE: input */
} field;

static const char *const KEYER_NAMES[] = {"Iambic A", "Iambic B", "Ultimatic", "Bug", "Straight key"};
static const char *const INPUT_NAMES[] = {"Keyboard", "USB pad (mouse)"};

static int build_fields(app_t *a, field *f)
{
    settings_t *c = &a->cfg;
    int n = 0;
#define HDR(l) f[n++] = (field){F_HEADER, l, NULL, 0, 0, 0, NULL, NULL, 0, 0}
#define INT(l, v, mn, mx, st, u) f[n++] = (field){F_INT, l, &c->v, mn, mx, st, u, NULL, 0, 0}
#define ENUM(l, v, names, cnt) f[n++] = (field){F_ENUM, l, &c->v, 0, (cnt) - 1, 1, NULL, names, 0, 0}
#define BOOL(l, v) f[n++] = (field){F_BOOL, l, &c->v, 0, 1, 1, NULL, NULL, 0, 0}
    HDR("KEYER");
    INT("Tone", tone_hz, 300, 1200, 10, "Hz");
    INT("Speed", wpm, 5, 60, 1, "WPM");
    INT("Farnsworth", farnsworth, 0, 59, 1, "WPM");
    ENUM("Key with", key_input, INPUT_NAMES, INPUT_COUNT);
    ENUM("Paddle mode", keyer_mode, KEYER_NAMES, KEYER_MODE_COUNT);
    BOOL("Swap paddles", swap_paddles);
    INT("Volume", volume, 0, 100, 5, "%");
    INT("Tone shape", rise_ms, 1, 10, 1, "ms rise");
    HDR("RECEIVER");
    INT("Speed", rx_wpm, 5, 60, 1, "WPM");
    BOOL("Lock speed", rx_lock);
    INT("Pitch", rx_freq, 200, 1500, 10, "Hz");
    INT("Filter", rx_bw, 50, 500, 0, "Hz");
    INT("Sensitivity", rx_sens, 1, 10, 1, "");
    BOOL("AFC", rx_afc);
    HDR("TRANSCEIVER");
    INT("Break-in delay", breakin_ms, 50, 3000, 50, "ms");
    BOOL("Sidetone", trx_sidetone);
    HDR("AUDIO");
    f[n++] = (field){F_DEVICE, "Output", NULL, 0, 0, 0, NULL, NULL, 1, 0};
    f[n++] = (field){F_DEVICE, "Input", NULL, 0, 0, 0, NULL, NULL, 1, 1};
    f[n] = (field){F_INT, "Buffer", &c->latency_ms, 5, 50, 5, "ms", NULL, 1, 0};
    n++;
#undef HDR
#undef INT
#undef ENUM
#undef BOOL
    return n;
}

static void field_value(app_t *a, const field *f, char *out, size_t cap)
{
    switch (f->kind) {
    case F_BOOL:
        str_copy(out, cap, *f->value ? "On" : "Off");
        break;
    case F_ENUM:
        str_copy(out, cap, f->names[clampi(*f->value, f->min, f->max)]);
        break;
    case F_DEVICE: {
        const char *name = f->capture ? a->cfg.in_dev : a->cfg.out_dev;
        str_copy(out, cap, name[0] ? name : "System default");
        break;
    }
    default:
        if (f->value == &a->cfg.farnsworth && *f->value == 0)
            str_copy(out, cap, "Off");
        else if (f->unit[0])
            snprintf(out, cap, "%d %s", *f->value, f->unit);
        else
            snprintf(out, cap, "%d", *f->value);
        break;
    }
}

static void field_change(app_t *a, const field *f, int dir, int big)
{
    settings_t *c = &a->cfg;
    if (f->kind == F_HEADER)
        return;
    if (f->kind == F_DEVICE) {
        char(*list)[AUDIO_NAME_CAP] = f->capture ? a->dev_in : a->dev_out;
        int n = f->capture ? a->n_in : a->n_out, cur = -1, i;
        char *dst = f->capture ? c->in_dev : c->out_dev;
        for (i = 0; i < n; i++)
            if (strcmp(list[i], dst) == 0)
                cur = i;
        cur += dir;
        if (cur < -1)
            cur = n - 1;
        if (cur >= n)
            cur = -1;
        str_copy(dst, SETTINGS_NAME_CAP, cur < 0 ? "" : list[cur]);
        a->set_audio_dirty = 1;
        return;
    }
    if (f->value == &c->rx_bw) {
        int idx = rx_bw_index(c->rx_bw) + dir;
        c->rx_bw = RX_BANDWIDTHS[clampi(idx, 0, RX_BANDWIDTH_COUNT - 1)];
    } else if (f->value == &c->farnsworth) {
        /* Off, then from 5 WPM up to one below the character speed. */
        int v = c->farnsworth + dir * (big ? 5 : 1);
        if (c->farnsworth == 0 && dir > 0)
            v = 5;
        if (v < 5)
            v = 0;
        c->farnsworth = v >= c->wpm ? (dir > 0 ? c->farnsworth : 0) : v;
    } else if (f->kind == F_BOOL) {
        *f->value = !*f->value;
    } else if (f->kind == F_ENUM) {
        *f->value = (*f->value - f->min + dir + (f->max - f->min + 1)) % (f->max - f->min + 1) + f->min;
    } else {
        *f->value = clampi(*f->value + dir * f->step * (big ? 5 : 1), f->min, f->max);
    }
    if (f->audio)
        a->set_audio_dirty = 1;
    else
        a->set_dirty = 1;
    app_apply_settings(a);
}

static void settings_render(app_t *a)
{
    field f[32];
    int n = build_fields(a, f), i, y, visible, first;
    rect_t r = ui_center(66, n + 6), body;
    char val[160];

    body = ui_modal(r, "Settings", a->have_cfg_path ? "saved on close" : "not saved");
    visible = body.h - 2;
    if (a->set_sel < 0 || a->set_sel >= n || f[a->set_sel].kind == F_HEADER)
        a->set_sel = 1;
    /* Keep the selection in view. */
    first = a->set_scroll;
    if (a->set_sel < first + 1)
        first = maxi(0, a->set_sel - 1);
    if (a->set_sel >= first + visible)
        first = a->set_sel - visible + 1;
    a->set_scroll = first;

    y = body.y + 1;
    for (i = first; i < n && y < body.y + 1 + visible; i++, y++) {
        int sel = i == a->set_sel;
        uint32_t bg = sel ? TH_ACCENT_BG : TH_SURFACE_2;
        int vx = body.x + 22;
        if (f[i].kind == F_HEADER) {
            scr_text(body.x + 3, y, f[i].label, TH_FG_FAINT, TH_SURFACE_2, A_BOLD);
            scr_hline(body.x + 4 + utf8_width(f[i].label), y, body.w - 7 - utf8_width(f[i].label), 0x2500,
                      TH_LINE, TH_SURFACE_2);
            continue;
        }
        scr_fill(rect_make(body.x + 1, y, body.w - 2, 1), ' ', TH_FG, bg);
        if (sel)
            scr_put(body.x + 1, y, 0x258C, TH_ACCENT, bg, 0);
        scr_text(body.x + 4, y, f[i].label, sel ? TH_FG : TH_FG_MUTED, bg, 0);
        field_value(a, &f[i], val, sizeof val);
        if (utf8_width(val) > body.w - 26 - 14)
            val[maxi(0, body.w - 26 - 14)] = 0;
        scr_text(vx, y, "◂", sel ? TH_ACCENT : TH_FG_GHOST, bg, 0);
        scr_text(vx + 2, y, val, sel ? TH_ACCENT_HI : TH_FG, bg, A_BOLD);
        scr_text(vx + 3 + utf8_width(val), y, "▸", sel ? TH_ACCENT : TH_FG_GHOST, bg, 0);
        /* A slider for numeric values. */
        if (f[i].kind == F_INT && f[i].max > f[i].min) {
            int sw = 12, sx = body.x + body.w - sw - 3, k;
            int pos = (int)((double)(*f[i].value - f[i].min) / (f[i].max - f[i].min) * (sw - 1) + 0.5);
            for (k = 0; k < sw; k++)
                scr_put(sx + k, y, k < pos ? 0x2501 : (k == pos ? 0x25CF : 0x2500),
                        k <= pos ? (sel ? TH_ACCENT : TH_ACCENT_MID) : TH_FG_GHOST, bg, 0);
        }
    }
    if (first > 0)
        scr_put(body.x + body.w - 2, body.y + 1, 0x25B4, TH_FG_FAINT, TH_SURFACE_2, 0);
    if (first + visible < n)
        scr_put(body.x + body.w - 2, body.y + visible, 0x25BE, TH_FG_FAINT, TH_SURFACE_2, 0);
}

static int settings_key(app_t *a, const key_event *ev)
{
    field f[32];
    int n = build_fields(a, f), big = (ev->mods & MOD_SHIFT) != 0;
    if (is_cancel(ev) || ev->key == KEY_ENTER) {
        app_close_modal(a);
        return 1;
    }
    switch (ev->key) {
    case KEY_UP:
        do {
            a->set_sel = (a->set_sel + n - 1) % n;
        } while (f[a->set_sel].kind == F_HEADER);
        return 1;
    case KEY_DOWN:
    case KEY_TAB:
        do {
            a->set_sel = (a->set_sel + 1) % n;
        } while (f[a->set_sel].kind == F_HEADER);
        return 1;
    case KEY_LEFT:
        field_change(a, &f[a->set_sel], -1, big);
        return 1;
    case KEY_RIGHT:
        field_change(a, &f[a->set_sel], 1, big);
        return 1;
    case KEY_PGDN:
        field_change(a, &f[a->set_sel], -1, 1);
        return 1;
    case KEY_PGUP:
        field_change(a, &f[a->set_sel], 1, 1);
        return 1;
    case KEY_SPACE:
        /* Hear the current tone, speed and shape. */
        audio_stop_playback();
        audio_play_text("VVV", a->cfg.wpm, 0, 50.0);
        return 1;
    default:
        return 1;
    }
}

/* ======================================================================== */
/* Help                                                                     */
/* ======================================================================== */

static const char *const HELP[] = {
    "#GLOBAL",
    "C-x\tOpen the command menu, then press a letter",
    "C-x C-c\tQuit",
    "C-g, Esc\tClose the window on top",
    "C-l\tClear the text of the current screen",
    "",
    "#KEYING  (Transmit, TX/RX, send training)",
    "Space\tStraight key: the tone sounds while it is held",
    "← →\tPaddles: dit and dah (swap them in Settings)",
    "USB pad\tA paddle seen as mouse buttons: set Key with",
    "\tto USB pad in Settings. Left is dit, right dah;",
    "\tkeep the mouse pointer over this window.",
    "Modes\tIambic A, Iambic B, Ultimatic, Bug and",
    "\tStraight key (both paddles key by hand)",
    "Bksp\tDelete the last character (Transmit)",
    "",
    "#RECEIVER  (Receive, TX/RX)",
    "+ -\tSensitivity: higher catches weaker signals,",
    "\tlower rejects noise and false characters",
    "< >\tTune by 10 Hz (arrow keys too in Receive)",
    "z\tZero-beat: tune to the strongest signal",
    "b B\tNarrower or wider filter",
    "a\tAutomatic frequency control on / off",
    "s\tLock or release the receive speed",
    "",
    "#TRANSCEIVER",
    "\tListens by default. Touch the key to transmit:",
    "\tthe receiver goes quiet and comes back after",
    "\tthe break-in delay set in Settings.",
    "",
    "#TRAINING",
    "Enter\tStart a session, check an answer, next item",
    "\tEach typed or decoded character is checked.",
    "\tA wrong character restarts the same item.",
    "C-x C-p\tInterrupt the session and open training setup",
    "C-r\tPlay the item again (copy)",
    "Tab\tShow or hide the pattern hint (send)",
    "Bksp\tStart the item again (send)",
    "Esc\tBack to the session setup",
    "",
    "#TIPS",
    "\tFor exact keying use a terminal that supports the",
    "\tkitty keyboard protocol (kitty, foot, WezTerm,",
    "\tGhostty, Alacritty, iTerm2) or Windows Terminal.",
    "\tThe header shows KEYS legacy when key releases",
    "\thave to be guessed.",
};

static void help_render(app_t *a)
{
    int n = ARRAY_LEN(HELP), i, y, visible;
    rect_t r = ui_center(66, 40), body;
    char title[64];

    snprintf(title, sizeof title, "Help  CWtty %s", CWTTY_VERSION);
    body = ui_modal(r, title, "MIT License");
    visible = body.h - 3;
    a->help_scroll = clampi(a->help_scroll, 0, maxi(0, n - visible));
    y = body.y + 1;
    for (i = a->help_scroll; i < n && y < body.y + 1 + visible; i++, y++) {
        const char *line = HELP[i], *tab = strchr(line, '\t');
        if (line[0] == '#') {
            scr_text(body.x + 3, y, line + 1, TH_ACCENT, TH_SURFACE_2, A_BOLD);
        } else if (tab) {
            char key[32];
            size_t kl = (size_t)(tab - line);
            str_copy(key, kl + 1 < sizeof key ? kl + 1 : sizeof key, line);
            scr_text(body.x + 3, y, key, TH_FG, TH_SURFACE_2, A_BOLD);
            scr_text(body.x + 15, y, tab + 1, TH_FG_MUTED, TH_SURFACE_2, 0);
        }
    }
    if (a->help_scroll > 0)
        scr_put(body.x + body.w - 2, body.y + 1, 0x25B4, TH_FG_FAINT, TH_SURFACE_2, 0);
    if (a->help_scroll + visible < n)
        scr_put(body.x + body.w - 2, body.y + visible, 0x25BE, TH_ACCENT, TH_SURFACE_2, 0);
    if (a->have_cfg_path && y < body.y + body.h) {
        char buf[1100];
        snprintf(buf, sizeof buf, "Settings: %s", a->cfg_path);
        if (utf8_width(buf) > body.w - 6)
            buf[maxi(0, body.w - 6)] = 0;
        scr_text(body.x + 3, body.y + body.h - 1, buf, TH_FG_FAINT, TH_SURFACE_2, 0);
    }
}

static int help_key(app_t *a, const key_event *ev)
{
    if (is_cancel(ev) || ev->key == KEY_ENTER || ev->key == 'q') {
        app_close_modal(a);
        return 1;
    }
    switch (ev->key) {
    case KEY_UP: a->help_scroll--; break;
    case KEY_DOWN: a->help_scroll++; break;
    case KEY_PGUP: a->help_scroll -= 10; break;
    case KEY_PGDN:
    case KEY_SPACE: a->help_scroll += 10; break;
    case KEY_HOME: a->help_scroll = 0; break;
    case KEY_END: a->help_scroll = 1000; break;
    default: break;
    }
    if (a->help_scroll < 0)
        a->help_scroll = 0;
    return 1;
}

/* ======================================================================== */

void modal_render(app_t *a, int kind)
{
    switch (kind) {
    case MODAL_MENU: menu_render(a); break;
    case MODAL_TABLE: table_render(a); break;
    case MODAL_SETTINGS: settings_render(a); break;
    case MODAL_HELP: help_render(a); break;
    default: break;
    }
}

int modal_key(app_t *a, int kind, const key_event *ev)
{
    switch (kind) {
    case MODAL_MENU: return menu_key(a, ev);
    case MODAL_TABLE: return table_key(a, ev);
    case MODAL_SETTINGS: return settings_key(a, ev);
    case MODAL_HELP: return help_key(a, ev);
    default: return 0;
    }
}
