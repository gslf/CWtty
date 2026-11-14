/*
 * modals.c - Modal windows: the C-x command menu, the Morse table (which
 * floats over the running activity without stopping it), settings, help
 * and the window to enter an online room.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "cloud.h"
#include "fcrypto.h"
#include "platform.h"
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
    {'n', "Rooms", "Key with other stations on the internet", MODE_NET},
    {'p', "Training", "Practice copying and sending", MODE_TRAIN},
    {'i', "Statistics", "Training records and progress", MODE_STATS},
    {'m', "Morse table", "Floating reference, keeps running", -1},
    {'s', "Settings", "Tone, speed, keyer, receiver, audio", -1},
    {'h', "Help", "Keys and tips", -1},
    {'q', "Quit", "also C-x C-c", -1},
};
#define MENU_TOOLS 6 /* index of the first tool */

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
        app_quit(a);
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
        app_close_modal(a);
        app_quit(a);
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
static const char *const INPUT_NAMES[] = {"Keyboard", "USB pad (mouse)", "USB straight key"};

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
    HDR("ROOMS");
    INT("Remote pitch", net_tone, 300, 1500, 10, "Hz");
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
    "USB key\tA straight key seen as a mouse button: set",
    "\tKey with to USB straight key in Settings;",
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
    "#ROOMS  (C-x n)",
    "Enter\tEnter a room, or change room or callsign",
    "\tEverybody in a room hears everybody, each",
    "\tstation on a pitch of its own (around Remote",
    "\tpitch in Settings), from a terminal or the web",
    "\tapp. Leaving the screen leaves the room.",
    "K\tPasses the turn and ends your line, as on",
    "\tthe air; so do KN, BK, AR and SK. BT (=)",
    "\tstarts a new paragraph. A line also ends when",
    "\tanother station starts, or after a pause.",
    "Password\tOptional: makes the room private, its keying",
    "\tencrypted end to end. Choose an uncommon one.",
    "",
    "#TRAINING",
    "Enter\tStart a session, check an answer, next item",
    "\tEach typed or decoded character is checked.",
    "\tA wrong character restarts the same item.",
    "Level\tCopy: how many times an item can be heard,",
    "\t4 at level 1 down to 1 at level 4. Out of",
    "\tlistens, a wrong answer shows the item.",
    "Records\tThe best streak of correct answers, one per",
    "\tcopy level and one for send, kept across",
    "\tsessions: beat it for a NEW RECORD.",
    "C-x C-p\tInterrupt the session and open training setup",
    "C-r\tPlay the item again (copy)",
    "Tab\tShow or hide the pattern hint (send)",
    "Bksp\tStart the item again (send)",
    "Esc\tBack to the session setup",
    "",
    "#STATISTICS  (C-x i)",
    "\tRecords, answers, accuracy, time, listens,",
    "\tsending speed, days practised, the last four",
    "\tweeks and how well each character goes. Saved",
    "\tnext to the settings, in cwtty.stats.",
    "↑↓\tScroll",
    "e\tExport to ~/cwtty-stats-<date>.txt",
    "i\tImport an exported file (replaces, asks first)",
    "c\tFilen cloud: sign in (2FA too) to keep every",
    "\tdevice's statistics in sync; the totals are",
    "\ttheir sum. The session is kept in cwtty.filen",
    "\t(private), never the password.",
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
/* Enter a room                                                             */
/* ======================================================================== */

enum { JF_CALL, JF_ROOM, JF_PASS, JF_JOIN, JF_COUNT };

static void join_render(app_t *a)
{
    static const char *const labels[] = {"Callsign", "Room", "Password"};
    rect_t r = ui_center(60, 16), body;
    int i, y, fx, fw, blink = app_blink(a, 530);

    body = ui_modal(r, "Enter a room", "Esc cancel");
    fx = body.x + 15;
    fw = body.w - 18;
    y = body.y + 1;
    for (i = 0; i < 3; i++, y += 2) {
        const char *value = i == JF_CALL ? a->join_call : (i == JF_ROOM ? a->join_room : a->join_pass);
        int sel = a->join_sel == i;
        if (sel)
            scr_put(body.x + 1, y, 0x258C, TH_ACCENT, TH_SURFACE_2, 0);
        scr_text(body.x + 3, y, labels[i], sel ? TH_ACCENT_HI : TH_FG, TH_SURFACE_2, A_BOLD);
        ui_field(fx, y, fw, value, sel, i == JF_PASS, blink);
        if (i == JF_PASS && !value[0] && !sel)
            scr_text(fx + 1, y, "none: a public room", TH_FG_GHOST, TH_SURFACE, A_ITALIC);
    }
    if (a->join_error[0]) {
        scr_text(body.x + 3, y, a->join_error, TH_WARN, TH_SURFACE_2, 0);
    } else {
        scr_text(body.x + 3, y, "A password makes the room private and encrypted:",
                 TH_FG_FAINT, TH_SURFACE_2, 0);
        scr_text(body.x + 3, y + 1, "only who knows it is in. Choose an uncommon one.", TH_FG_FAINT, TH_SURFACE_2, 0);
    }
    y += 3;
    {
        int sel = a->join_sel == JF_JOIN, bx = body.x + body.w - 13;
        scr_text(bx, y, "  Enter  ", sel ? TH_ON_ACCENT : TH_ACCENT, sel ? TH_ACCENT : TH_SURFACE_3, A_BOLD);
    }
}

/* Characters a field takes: callsigns are upper case. */
static int join_char(int which, int c)
{
    if (which == JF_CALL) {
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ? c : 0;
    }
    return c >= 0x20 && c < 0x7F ? c : 0;
}

static int join_key(app_t *a, const key_event *ev)
{
    char *value = a->join_sel == JF_CALL ? a->join_call : (a->join_sel == JF_ROOM ? a->join_room : a->join_pass);
    size_t cap = a->join_sel == JF_CALL ? sizeof a->join_call
                                        : (a->join_sel == JF_ROOM ? sizeof a->join_room : sizeof a->join_pass);
    if (is_cancel(ev)) {
        memset(a->join_pass, 0, sizeof a->join_pass);
        app_close_modal(a);
        return 1;
    }
    switch (ev->key) {
    case KEY_UP:
        a->join_sel = (a->join_sel + JF_COUNT - 1) % JF_COUNT;
        return 1;
    case KEY_DOWN:
    case KEY_TAB:
        a->join_sel = (a->join_sel + 1) % JF_COUNT;
        return 1;
    case KEY_ENTER:
        if (online_join(a) == 0)
            app_close_modal(a);
        return 1;
    case KEY_BACKSPACE:
        if (a->join_sel != JF_JOIN && value[0])
            value[strlen(value) - 1] = 0;
        a->join_error[0] = 0;
        return 1;
    default:
        break;
    }
    if (a->join_sel != JF_JOIN && ev->text && !(ev->mods & (MOD_CTRL | MOD_ALT))) {
        int c = join_char(a->join_sel, ev->text);
        size_t n = strlen(value);
        size_t limit = a->join_sel == JF_CALL ? ROOM_CALL_CAP - 1
                                              : (a->join_sel == JF_ROOM ? ROOM_NAME_CAP - 1 : cap - 1);
        if (c && n < limit && n + 1 < cap) {
            value[n] = (char)c;
            value[n + 1] = 0;
            a->join_error[0] = 0;
        }
    }
    return 1; /* modal: everything stays here */
}

/* ======================================================================== */
/* Leave the room?                                                          */
/* ======================================================================== */

static void leave_render(app_t *a)
{
    rect_t r = ui_center(56, 9), body;
    char line[96];
    int quit = a->leave_to == LEAVE_QUIT;

    body = ui_modal(r, quit ? "Leave the room and quit?" : "Leave the room?", "Esc stay");
    snprintf(line, sizeof line, "You will leave %s and stop hearing", a->net ? a->net->room.name : "the room");
    scr_text(body.x + 3, body.y + 1, line, TH_FG, TH_SURFACE_2, 0);
    scr_text(body.x + 3, body.y + 2, "the other stations.", TH_FG, TH_SURFACE_2, 0);
    {
        int bx = body.x + body.w - 3 - 9 - 2 - 8;
        scr_text(bx, body.y + 4, "  Stay  ", TH_FG, TH_SURFACE_3, 0);
        scr_text(bx + 10, body.y + 4, "  Leave  ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
    }
}

static int leave_key(app_t *a, const key_event *ev)
{
    if (is_cancel(ev) || ev->key == 'n') {
        app_close_modal(a);
        return 1;
    }
    if (ev->key == KEY_ENTER || ev->key == 'y') {
        app_close_modal(a);
        a->leave_ok = 1;
        if (a->leave_to == LEAVE_QUIT)
            app_quit(a);
        else
            app_set_mode(a, a->leave_to);
        a->leave_ok = 0;
        return 1;
    }
    return 1; /* modal */
}

/* ======================================================================== */
/* Import statistics                                                        */
/* ======================================================================== */

static statsbook_t import_book; /* the book as it would be, to confirm */

static int answers_of(const stats_t *s)
{
    return s->dir[STATS_COPY].answers + s->dir[STATS_SEND].answers;
}

static void import_render(app_t *a)
{
    rect_t r = ui_center(66, 11), body;
    char line[128];
    int bx;

    if (a->import_stage == 0) {
        body = ui_modal(r, "Import statistics", "Esc cancel");
        scr_text(body.x + 3, body.y + 1, "File", TH_ACCENT_HI, TH_SURFACE_2, A_BOLD);
        ui_field(body.x + 9, body.y + 1, body.w - 12, a->import_path, 1, 0, app_blink(a, 530));
        if (a->import_error[0]) {
            scr_text(body.x + 3, body.y + 3, a->import_error, TH_WARN, TH_SURFACE_2, 0);
        } else {
            scr_text(body.x + 3, body.y + 3, "A file exported with e. It replaces the statistics", TH_FG_FAINT,
                     TH_SURFACE_2, 0);
            scr_text(body.x + 3, body.y + 4, "here: you are asked before anything changes.", TH_FG_FAINT, TH_SURFACE_2,
                     0);
        }
        bx = body.x + body.w - 3 - 8;
        scr_text(bx, body.y + 6, "  Open  ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
        return;
    }
    body = ui_modal(r, "Replace the statistics?", "Esc cancel");
    {
        const stats_t *t = &import_book.total;
        char date[32] = "";
        int y, m, d, n = import_book.others + (answers_of(&import_book.own.s) > 0);
        static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (t->last_day > 0) {
            stats_date(t->last_day, &y, &m, &d);
            snprintf(date, sizeof date, ", last on %d %s %d", d, MON[clampi(m, 1, 12) - 1], y);
        }
        snprintf(line, sizeof line, "The file: %d answer%s from %d device%s%s.", answers_of(t),
                 answers_of(t) == 1 ? "" : "s", n, n == 1 ? "" : "s", date);
        scr_text(body.x + 3, body.y + 1, line, TH_FG, TH_SURFACE_2, 0);
        if (answers_of(&a->book.total) > 0) {
            snprintf(line, sizeof line, "It replaces the %d answer%s here.", answers_of(&a->book.total),
                     answers_of(&a->book.total) == 1 ? "" : "s");
            scr_text(body.x + 3, body.y + 2, line, TH_FG, TH_SURFACE_2, 0);
            scr_text(body.x + 3, body.y + 4, "Export first (e) to keep a copy of the ones here.", TH_FG_FAINT,
                     TH_SURFACE_2, 0);
        } else {
            scr_text(body.x + 3, body.y + 2, "There are no statistics here yet.", TH_FG, TH_SURFACE_2, 0);
        }
    }
    bx = body.x + body.w - 3 - 11 - 2 - 10;
    scr_text(bx, body.y + 6, "  Cancel  ", TH_FG, TH_SURFACE_3, 0);
    scr_text(bx + 12, body.y + 6, "  Replace  ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
}

/* Reads the file and prepares the book it makes. */
static void import_open(app_t *a)
{
    static char text[BOOK_TEXT_CAP];
    char path[1024];
    int n;
    if (app_expand_path(a->import_path, path, sizeof path) != 0) {
        str_copy(a->import_error, sizeof a->import_error, "Type the path of the file.");
        return;
    }
    n = plat_read_file(path, text, sizeof text);
    if (n < 0) {
        str_copy(a->import_error, sizeof a->import_error, "Could not read that file.");
        return;
    }
    import_book = a->book;
    if (n >= (int)sizeof text - 1 || statsbook_import(&import_book, text) != 0) {
        str_copy(a->import_error, sizeof a->import_error, "That is not a CWtty statistics file.");
        return;
    }
    a->import_stage = 1;
}

static int import_key(app_t *a, const key_event *ev)
{
    if (is_cancel(ev) || (a->import_stage == 1 && ev->key == 'n')) {
        app_close_modal(a);
        return 1;
    }
    if (ev->key == KEY_ENTER || (a->import_stage == 1 && ev->key == 'y')) {
        if (a->import_stage == 0) {
            import_open(a);
            return 1;
        }
        a->book = import_book;
        app_save_stats(a);
        app_close_modal(a);
        app_toast(a, 0, "Statistics imported");
        return 1;
    }
    if (a->import_stage == 0) {
        size_t n = strlen(a->import_path);
        if ((ev->mods & MOD_CTRL) && ev->key == 'u') { /* clear the field */
            a->import_path[0] = 0;
            a->import_error[0] = 0;
        } else if (ev->key == KEY_BACKSPACE) {
            while (n > 0 && (a->import_path[n - 1] & 0xC0) == 0x80)
                n--; /* a whole UTF-8 character */
            if (n)
                a->import_path[n - 1] = 0;
            a->import_error[0] = 0;
        } else if (ev->text >= 0x20 && ev->text != 0x7F && !(ev->mods & (MOD_CTRL | MOD_ALT)) &&
                   n + 1 < sizeof a->import_path) {
            char u[4];
            int k = utf8_encode(ev->text, u);
            if (k > 0 && n + (size_t)k < sizeof a->import_path) {
                memcpy(a->import_path + n, u, (size_t)k);
                a->import_path[n + (size_t)k] = 0;
            }
            a->import_error[0] = 0;
        }
    }
    return 1; /* modal */
}

/* ======================================================================== */
/* Filen cloud                                                              */
/* ======================================================================== */

enum { CF_MAIL, CF_PASS, CF_CODE, CF_BUTTON, CF_COUNT };

static void cloud_wipe(app_t *a)
{
    fc_wipe(a->cloud_pass, sizeof a->cloud_pass);
    a->cloud_code[0] = 0;
}

static void cloud_render(app_t *a)
{
    rect_t r = ui_center(66, 16), body;
    int state = cloud_state(), y, i, blink = app_blink(a, 530);
    char line[160];

    body = ui_modal(r, "Filen cloud", "Esc close");
    y = body.y + 1;
    if (a->cloud_confirm) {
        scr_text(body.x + 3, y, "Disconnect from Filen?", TH_FG, TH_SURFACE_2, A_BOLD);
        scr_text(body.x + 3, y + 2, "The statistics stay here and in your Filen account;", TH_FG_MUTED, TH_SURFACE_2, 0);
        scr_text(body.x + 3, y + 3, "this computer stops keeping them in sync.", TH_FG_MUTED, TH_SURFACE_2, 0);
        i = body.x + body.w - 3 - 14 - 2 - 10;
        scr_text(i, body.y + body.h - 3, "  Cancel  ", TH_FG, TH_SURFACE_3, 0);
        scr_text(i + 12, body.y + body.h - 3, "  Disconnect  ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
        return;
    }
    if (state != CLOUD_OFF && cloud_email()[0]) {
        static const char *const labels[] = {"Account", "Status", "Devices", "Folder"};
        const char *status;
        char devices[48];
        if (state == CLOUD_BUSY)
            status = "syncing…";
        else if (a->cloud_failed)
            status = cloud_error()[0] ? cloud_error() : "the last sync failed";
        else if (cloud_synced()[0])
            status = (snprintf(line, sizeof line, "synced at %s", cloud_synced()), line);
        else
            status = "not synced yet";
        if (a->book.others)
            snprintf(devices, sizeof devices, "%d: this one and %d other%s", 1 + a->book.others, a->book.others,
                     a->book.others == 1 ? "" : "s");
        else
            str_copy(devices, sizeof devices, "this one only, so far");
        for (i = 0; i < 4; i++, y += 2) {
            const char *v = i == 0 ? cloud_email() : (i == 1 ? status : (i == 2 ? devices : "CWtty/stats-v1"));
            scr_text(body.x + 3, y, labels[i], TH_FG_MUTED, TH_SURFACE_2, 0);
            scr_text(body.x + 15, y, v, i == 1 && a->cloud_failed && state != CLOUD_BUSY ? TH_WARN : TH_FG,
                     TH_SURFACE_2, i == 0 ? A_BOLD : 0);
        }
        scr_text(body.x + 3, y, "Every device adds its own; the totals are their sum.", TH_FG_FAINT, TH_SURFACE_2, 0);
        scr_text(body.x + 3, y + 1, "Synced at start, a minute after training and at exit.", TH_FG_FAINT,
                 TH_SURFACE_2, 0);
        i = body.x + body.w - 3 - 14 - 2 - 12;
        scr_text(i, y + 3, " s Sync now ", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
        scr_text(i + 14, y + 3, " d Disconnect ", TH_FG, TH_SURFACE_3, 0);
        return;
    }

    /* Signing in. */
    scr_text(body.x + 3, y, "Keep the statistics of all your devices in sync, end-to-end", TH_FG_MUTED, TH_SURFACE_2,
             0);
    scr_text(body.x + 3, y + 1, "encrypted, in your Filen account (filen.io).", TH_FG_MUTED, TH_SURFACE_2, 0);
    y += 3;
    {
        static const char *const labels[] = {"Email", "Password", "2FA code"};
        for (i = 0; i < 3; i++, y += 2) {
            const char *value = i == CF_MAIL ? a->cloud_mail : (i == CF_PASS ? a->cloud_pass : a->cloud_code);
            int sel = a->cloud_sel == i && state != CLOUD_BUSY;
            int fw = i == CF_CODE ? 10 : body.w - 18;
            if (sel)
                scr_put(body.x + 1, y, 0x258C, TH_ACCENT, TH_SURFACE_2, 0);
            scr_text(body.x + 3, y, labels[i], sel ? TH_ACCENT_HI : TH_FG, TH_SURFACE_2, A_BOLD);
            ui_field(body.x + 15, y, fw, value, sel, i == CF_PASS, blink);
            if (i == CF_CODE)
                scr_text(body.x + 27, y, cloud_need_code() ? "from your authenticator app" : "if your account has 2FA",
                         cloud_need_code() ? TH_ACCENT : TH_FG_FAINT, TH_SURFACE_2, 0);
        }
    }
    if (state == CLOUD_BUSY)
        scr_text(body.x + 3, y, "Signing in… (a few seconds: the keys are derived here)", TH_ACCENT, TH_SURFACE_2, 0);
    else if (cloud_error()[0])
        scr_text(body.x + 3, y, cloud_error(), TH_WARN, TH_SURFACE_2, 0);
    else
        scr_text(body.x + 3, y, "The password stays here: only a key derived from it is sent.", TH_FG_FAINT,
                 TH_SURFACE_2, 0);
    {
        int sel = a->cloud_sel == CF_BUTTON, bx = body.x + body.w - 3 - 11;
        scr_text(bx, y + 2, "  Sign in  ", sel || state == CLOUD_BUSY ? TH_ON_ACCENT : TH_ACCENT,
                 sel || state == CLOUD_BUSY ? TH_ACCENT : TH_SURFACE_3, A_BOLD);
    }
}

static int cloud_key(app_t *a, const key_event *ev)
{
    int state = cloud_state();
    if (is_cancel(ev)) {
        if (a->cloud_confirm) {
            a->cloud_confirm = 0;
            return 1;
        }
        cloud_wipe(a);
        app_close_modal(a);
        return 1;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT)) {
        if ((ev->mods & MOD_CTRL) && ev->key == 'u' && state == CLOUD_OFF && a->cloud_sel < CF_BUTTON) {
            char *v = a->cloud_sel == CF_MAIL ? a->cloud_mail : (a->cloud_sel == CF_PASS ? a->cloud_pass : a->cloud_code);
            fc_wipe(v, a->cloud_sel == CF_MAIL ? sizeof a->cloud_mail
                                               : (a->cloud_sel == CF_PASS ? sizeof a->cloud_pass : sizeof a->cloud_code));
        }
        return 1;
    }
    if (a->cloud_confirm) {
        if (ev->key == KEY_ENTER || ev->key == 'y') {
            cloud_sign_out();
            a->cloud_confirm = 0;
            a->cloud_due = 0;
            app_toast(a, 0, "Disconnected from Filen");
        } else if (ev->key == 'n') {
            a->cloud_confirm = 0;
        }
        return 1;
    }
    if (state == CLOUD_BUSY)
        return 1;
    if (state == CLOUD_READY) {
        if (ev->key == 's') {
            a->cloud_due = a->now;
            a->cloud_failed = 0;
        } else if (ev->key == 'd') {
            a->cloud_confirm = 1;
        }
        return 1;
    }
    switch (ev->key) {
    case KEY_UP:
        a->cloud_sel = (a->cloud_sel + CF_COUNT - 1) % CF_COUNT;
        return 1;
    case KEY_DOWN:
    case KEY_TAB:
        a->cloud_sel = (a->cloud_sel + 1) % CF_COUNT;
        return 1;
    case KEY_ENTER:
        if (!a->cloud_mail[0] || !strchr(a->cloud_mail, '@')) {
            a->cloud_sel = CF_MAIL;
        } else if (!a->cloud_pass[0]) {
            a->cloud_sel = CF_PASS;
        } else {
            /* The password waits here until the result (cloud_tick()):
             * only a missing code keeps it, for the code. */
            cloud_login(a->cloud_mail, a->cloud_pass, a->cloud_code);
            a->cloud_sel = CF_BUTTON;
        }
        return 1;
    case KEY_BACKSPACE:
        if (a->cloud_sel < CF_BUTTON) {
            char *v = a->cloud_sel == CF_MAIL ? a->cloud_mail : (a->cloud_sel == CF_PASS ? a->cloud_pass : a->cloud_code);
            size_t n = strlen(v);
            while (n > 0 && (v[n - 1] & 0xC0) == 0x80)
                n--;
            if (n)
                v[n - 1] = 0;
        }
        return 1;
    default:
        break;
    }
    if (a->cloud_sel < CF_BUTTON && ev->text >= 0x20 && ev->text != 0x7F) {
        char *v = a->cloud_sel == CF_MAIL ? a->cloud_mail : (a->cloud_sel == CF_PASS ? a->cloud_pass : a->cloud_code);
        size_t cap = a->cloud_sel == CF_MAIL ? sizeof a->cloud_mail
                                             : (a->cloud_sel == CF_PASS ? sizeof a->cloud_pass : 9);
        size_t n = strlen(v);
        char u[4];
        int k;
        if (a->cloud_sel == CF_CODE && !(ev->text >= '0' && ev->text <= '9'))
            return 1;
        if (a->cloud_sel == CF_MAIL && ev->text == ' ')
            return 1;
        k = utf8_encode(ev->text, u);
        if (k > 0 && n + (size_t)k < cap) {
            memcpy(v + n, u, (size_t)k);
            v[n + (size_t)k] = 0;
        }
    }
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
    case MODAL_JOIN: join_render(a); break;
    case MODAL_LEAVE: leave_render(a); break;
    case MODAL_IMPORT: import_render(a); break;
    case MODAL_CLOUD: cloud_render(a); break;
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
    case MODAL_JOIN: return join_key(a, ev);
    case MODAL_LEAVE: return leave_key(a, ev);
    case MODAL_IMPORT: return import_key(a, ev);
    case MODAL_CLOUD: return cloud_key(a, ev);
    default: return 0;
    }
}
