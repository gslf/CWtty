/*
 * app.c - Application core: event routing between the audio engine, the
 * receiver and the decoders, keying, mode switching and the window chrome
 * (header bar, footer hints, toasts, modal stack).
 */
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo.h"
#include "keyer.h"
#include "morse.h"
#include "platform.h"
#include "theme.h"
#include "util.h"

#define MIN_W 64
#define MIN_H 20

/* The values last pushed to the engines, to apply only what changed. */
static settings_t applied;
static int applied_valid;

int app_blink(const app_t *a, int period_ms)
{
    return ((long long)(a->now / period_ms) & 1) == 0;
}

void app_toast(app_t *a, int warn, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->toast, sizeof a->toast, fmt, ap);
    va_end(ap);
    a->toast_warn = warn;
    a->toast_until = a->now + (warn ? 4000.0 : 2200.0);
}

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

void app_apply_settings(app_t *a)
{
    settings_t *c = &a->cfg;
    audio_params p;

    settings_clamp(c);
    p.tone_hz = c->tone_hz;
    p.wpm = c->wpm;
    p.keyer_mode = c->keyer_mode;
    p.volume = (a->mode == MODE_TRX && !c->trx_sidetone) ? 0 : c->volume;
    p.rise_ms = c->rise_ms;
    audio_set_params(&p);
    input_set_legacy_dit_ms(1200.0 / c->wpm);

    if (!applied_valid || applied.wpm != c->wpm)
        dec_set_speed(&a->txdec, c->wpm, 0);
    if (!applied_valid || applied.rx_wpm != c->rx_wpm || applied.rx_lock != c->rx_lock)
        dec_set_speed(&a->rxdec, c->rx_wpm, c->rx_lock);
    if (!applied_valid || applied.rx_freq != c->rx_freq)
        dsp_set_freq(a->dsp, (float)c->rx_freq);
    if (!applied_valid || applied.rx_bw != c->rx_bw)
        dsp_set_bw(a->dsp, (float)c->rx_bw);
    a->dsp->sens = c->rx_sens;
    a->dsp->afc = c->rx_afc;

    applied = *c;
    applied_valid = 1;
}

void app_save_settings(app_t *a)
{
    if (!a->have_cfg_path)
        return;
    if (settings_save(&a->cfg, a->cfg_path) != 0)
        app_toast(a, 1, "Could not save settings to %s", a->cfg_path);
}

void app_reopen_audio(app_t *a)
{
    audio_reopen(a->cfg.out_dev, a->cfg.in_dev, a->cfg.latency_ms);
    audio_get_status(&a->audio);
    if (a->audio.out_null)
        app_toast(a, 1, "Audio output unavailable: running silently");
    else
        app_toast(a, 0, "Audio: %s", a->audio.out_name[0] ? a->audio.out_name : "default device");
}

/* ------------------------------------------------------------------------ */
/* Modals                                                                    */
/* ------------------------------------------------------------------------ */

int app_top_modal(const app_t *a)
{
    return a->nmodal > 0 ? a->modals[a->nmodal - 1] : MODAL_NONE;
}

int app_has_modal(const app_t *a, int kind)
{
    int i;
    for (i = 0; i < a->nmodal; i++)
        if (a->modals[i] == kind)
            return 1;
    return 0;
}

void app_open_modal(app_t *a, int kind)
{
    int i, reopened = 0;
    /* Re-opening a modal brings it to the top, keeping its state. */
    for (i = 0; i < a->nmodal; i++) {
        if (a->modals[i] == kind) {
            memmove(&a->modals[i], &a->modals[i + 1], sizeof(int) * (size_t)(a->nmodal - i - 1));
            a->nmodal--;
            reopened = 1;
            break;
        }
    }
    if (a->nmodal >= (int)(sizeof a->modals / sizeof a->modals[0]))
        a->nmodal--;
    a->modals[a->nmodal++] = kind;
    if (reopened) {
        app_refresh_operator(a);
        return;
    }

    switch (kind) {
    case MODAL_MENU: {
        static const int mode_rows[MODE_COUNT] = {0, 0, 1, 2, 3};
        a->menu_sel = mode_rows[a->mode];
        break;
    }
    case MODAL_SETTINGS:
        a->n_out = audio_list_devices(0, a->dev_out, MAX_DEVICES);
        a->n_in = audio_list_devices(1, a->dev_in, MAX_DEVICES);
        a->set_dirty = 0;
        a->set_audio_dirty = 0;
        break;
    case MODAL_HELP:
        a->help_scroll = 0;
        break;
    default:
        break;
    }
    app_refresh_operator(a);
}

void app_close_modal(app_t *a)
{
    int kind;
    if (a->nmodal == 0)
        return;
    kind = a->modals[--a->nmodal];
    if (kind == MODAL_SETTINGS) {
        if (a->set_audio_dirty)
            app_reopen_audio(a);
        if (a->set_dirty || a->set_audio_dirty) {
            app_save_settings(a);
            if (!a->set_audio_dirty)
                app_toast(a, 0, "Settings saved");
        }
        a->set_dirty = a->set_audio_dirty = 0;
    }
    app_refresh_operator(a);
}

/* ------------------------------------------------------------------------ */
/* Keying                                                                    */
/* ------------------------------------------------------------------------ */

int app_keying_allowed(const app_t *a)
{
    int top = app_top_modal(a);
    if (top != MODAL_NONE && top != MODAL_TABLE)
        return 0;
    switch (a->mode) {
    case MODE_TX:
    case MODE_TRX:
        return 1;
    case MODE_TRAIN:
        return train_wants_keying(a);
    default:
        return 0;
    }
}

void app_release_keys(app_t *a)
{
    a->key_left = a->key_right = a->key_space = 0;
    audio_release_keys();
}

void app_refresh_operator(app_t *a)
{
    int allowed = app_keying_allowed(a);
    if (!allowed)
        a->key_left = a->key_right = a->key_space = 0;
    audio_enable_operator(allowed);
}

static int is_keying_key(const key_event *ev)
{
    return ev->key == KEY_SPACE || ev->key == KEY_LEFT || ev->key == KEY_RIGHT;
}

static void paddle(app_t *a, int left, int down)
{
    int dit = left ^ a->cfg.swap_paddles;
    if (dit)
        audio_key_dit(down);
    else
        audio_key_dah(down);
}

static void keying_press(app_t *a, int key)
{
    if (key == KEY_SPACE) {
        a->key_space = 1;
        audio_key_straight(1);
    } else if (key == KEY_LEFT) {
        a->key_left = 1;
        paddle(a, 1, 1);
    } else {
        a->key_right = 1;
        paddle(a, 0, 1);
    }
    if (a->mode == MODE_TRX)
        trx_operator_pressed(a);
    else if (a->mode == MODE_TRAIN)
        train_operator_pressed(a);
}

static void keying_release(app_t *a, int key)
{
    if (key == KEY_SPACE) {
        a->key_space = 0;
        audio_key_straight(0);
    } else if (key == KEY_LEFT) {
        a->key_left = 0;
        paddle(a, 1, 0);
    } else {
        a->key_right = 0;
        paddle(a, 0, 0);
    }
}

const char *app_live_pattern(const app_t *a)
{
    if (app_keying_allowed(a) && a->txdec.plen > 0)
        return dec_pattern(&a->txdec);
    if (a->rx_on && a->rxdec.plen > 0)
        return dec_pattern(&a->rxdec);
    return "";
}

/* ------------------------------------------------------------------------ */
/* Modes                                                                     */
/* ------------------------------------------------------------------------ */

static void rx_enable(app_t *a, int on)
{
    if (on && !a->rx_on && a->demo) {
        dec_clear(&a->rxdec);
        a->rx_on = 1;
        a->demo_at = a->now;
        a->rx_last_char_ms = dsp_time_ms(a->dsp);
        return;
    }
    if (!on && a->demo) {
        a->rx_on = 0;
        return;
    }
    if (on && !a->rx_on) {
        dec_clear(&a->rxdec);
        if (!audio_capture_start()) {
            audio_get_status(&a->audio);
            app_toast(a, 1, "%s", a->audio.error[0] ? a->audio.error : "No audio input");
            return;
        }
        a->rx_on = 1;
        a->rx_last_char_ms = dsp_time_ms(a->dsp);
    } else if (!on && a->rx_on) {
        audio_capture_stop();
        a->rx_on = 0;
    }
    audio_get_status(&a->audio);
}

void app_set_mode(app_t *a, int mode)
{
    if (mode == a->mode)
        return;
    app_release_keys(a);
    audio_stop_playback();
    if (a->mode == MODE_TRX)
        trx_leave(a);
    else if (a->mode == MODE_TRAIN)
        train_leave(a);

    a->mode = mode;
    rx_enable(a, mode == MODE_RX || mode == MODE_TRX);
    dec_clear(&a->txdec);
    a->tx_last[0] = a->tx_last_code[0] = 0;
    if (mode == MODE_TRX)
        trx_enter(a);
    else if (mode == MODE_TRAIN)
        train_enter(a);
    app_apply_settings(a); /* sidetone level depends on the mode */
    app_refresh_operator(a);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

app_t *app_create(const app_options *opt)
{
    app_t *a = (app_t *)calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->dsp = (rx_dsp *)calloc(1, sizeof(rx_dsp));
    if (!a->dsp) {
        free(a);
        return NULL;
    }

    settings_defaults(&a->cfg);
    if (opt && opt->config_override[0]) {
        str_copy(a->cfg_path, sizeof a->cfg_path, opt->config_override);
        a->have_cfg_path = 1;
    } else {
        a->have_cfg_path = plat_config_path(a->cfg_path, sizeof a->cfg_path) == 0;
    }
    if (a->have_cfg_path)
        settings_load(&a->cfg, a->cfg_path);

    a->kbd = input_info();
    a->now = a->started = plat_time_ms();
    a->demo = opt && opt->demo;
    if (a->demo)
        demo_init();

    if (audio_init(a->cfg.out_dev, a->cfg.in_dev, a->cfg.latency_ms) != 0) {
        free(a->dsp);
        free(a);
        return NULL;
    }
    dec_init(&a->txdec, a->cfg.wpm);
    dec_init(&a->rxdec, a->cfg.rx_wpm);
    dsp_init(a->dsp, (float)a->cfg.rx_freq, (float)a->cfg.rx_bw, a->cfg.rx_sens);
    applied_valid = 0;
    app_apply_settings(a);
    audio_get_status(&a->audio);

    a->mode = MODE_HOME;
    a->tr.sel = 0;
    app_refresh_operator(a);
    if (opt && opt->start_mode > MODE_HOME && opt->start_mode < MODE_COUNT)
        app_set_mode(a, opt->start_mode);

    if (a->audio.out_null)
        app_toast(a, 1, "No audio output device: running silently");
    return a;
}

void app_destroy(app_t *a)
{
    if (!a)
        return;
    /* Quitting with the settings still open must not lose changes. */
    if (a->set_dirty || a->set_audio_dirty)
        app_save_settings(a);
    app_release_keys(a);
    audio_shutdown();
    free(a->dsp);
    free(a);
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

void app_handle_key(app_t *a, const key_event *ev)
{
    int top = app_top_modal(a);

    a->now = plat_time_ms();
    if (ev->key == KEY_FOCUS_OUT) {
        /* Never leave the transmitter keyed when the window loses focus. */
        app_release_keys(a);
        return;
    }
    if (ev->key == KEY_FOCUS_IN)
        return;

    if (is_keying_key(ev)) {
        /* Releases always count, whatever modifier is held meanwhile:
         * dropping one would leave the transmitter keyed. */
        if (ev->type == EV_RELEASE) {
            keying_release(a, ev->key);
            return;
        }
        /* Text-only keys (e.g. pasted) have no release: never key with them. */
        if (app_keying_allowed(a) && !(ev->mods & (MOD_CTRL | MOD_ALT | MOD_SUPER | MOD_TEXT))) {
            if (ev->type == EV_PRESS)
                keying_press(a, ev->key);
            return;
        }
    }
    if (ev->type == EV_RELEASE)
        return;

    /* C-x: the command menu (Emacs-style prefix). */
    if ((ev->mods & MOD_CTRL) && ev->key == 'x') {
        if (top == MODAL_MENU)
            app_close_modal(a);
        else
            app_open_modal(a, MODAL_MENU);
        return;
    }
    if (top != MODAL_NONE && modal_key(a, top, ev))
        return;

    switch (a->mode) {
    case MODE_HOME:
        if (home_key(a, ev))
            return;
        break;
    case MODE_TX:
        if (tx_key(a, ev))
            return;
        break;
    case MODE_RX:
        if (rx_key(a, ev))
            return;
        break;
    case MODE_TRX:
        if (trx_key(a, ev))
            return;
        break;
    case MODE_TRAIN:
        if (train_key(a, ev))
            return;
        break;
    default:
        break;
    }

    if ((ev->mods & MOD_CTRL) && (ev->key == 'c' || ev->key == 'q'))
        app_toast(a, 0, "Press C-x C-c to quit");
}

/* ------------------------------------------------------------------------ */
/* Update                                                                    */
/* ------------------------------------------------------------------------ */

static int token_style(const dec_token *t)
{
    return t->kind == TOK_PROSIGN ? STY_PROSIGN : (t->kind == TOK_UNKNOWN ? STY_UNKNOWN : STY_NORMAL);
}

static void on_tx_token(app_t *a, const dec_token *t)
{
    switch (a->mode) {
    case MODE_TX:
        transcript_append(&a->tx_log, DIR_TX, t->text, token_style(t));
        break;
    case MODE_TRX:
        transcript_append(&a->trx_log, DIR_TX, t->text, token_style(t));
        break;
    case MODE_TRAIN:
        train_on_sent(a, t);
        break;
    default:
        break;
    }
    if (t->kind != TOK_SPACE) {
        str_copy(a->tx_last, sizeof a->tx_last, t->text);
        str_copy(a->tx_last_code, sizeof a->tx_last_code, t->code);
        a->tx_last_at = a->now;
    }
}

static void on_rx_token(app_t *a, const dec_token *t)
{
    if (a->mode == MODE_RX)
        transcript_append(&a->rx_log, DIR_RX, t->text, token_style(t));
    else if (a->mode == MODE_TRX)
        transcript_append(&a->trx_log, DIR_RX, t->text, token_style(t));
    if (t->kind != TOK_SPACE) {
        str_copy(a->rx_last, sizeof a->rx_last, t->text);
        str_copy(a->rx_last_code, sizeof a->rx_last_code, t->code);
        a->rx_last_at = a->now;
        a->rx_last_char_ms = dsp_time_ms(a->dsp);
    }
}

static void update_receiver(app_t *a)
{
    static float buf[2048];
    dsp_event ev[DSP_MAX_EVENTS];
    dec_token tok;
    int got, n, i;

    for (;;) {
        if (a->demo) {
            /* Real-time pacing of the simulated band. */
            double due = (a->now - a->demo_at) * AUDIO_RX_RATE / 1000.0;
            got = (int)clampd(due, 0, (double)(sizeof buf / sizeof buf[0]));
            if (got <= 0)
                break;
            demo_generate(buf, got);
            a->demo_at += got * 1000.0 / AUDIO_RX_RATE;
            if (a->now - a->demo_at > 500.0)
                a->demo_at = a->now; /* we were stalled: do not catch up */
        } else {
            got = audio_capture_read(buf, (int)(sizeof buf / sizeof buf[0]));
            if (got <= 0)
                break;
        }
        dsp_process(a->dsp, buf, got);
        n = dsp_take_events(a->dsp, ev, DSP_MAX_EVENTS);
        for (i = 0; i < n; i++) {
            tl_push(&a->rx_tl, ev[i].t_ms, ev[i].on);
            dec_key(&a->rxdec, ev[i].on, ev[i].t_ms);
        }
    }
    dec_update(&a->rxdec, dsp_time_ms(a->dsp));
    while (dec_pop(&a->rxdec, &tok))
        on_rx_token(a, &tok);

    /* Speed feedback for the detector (deglitch, smoothing), bounded so
     * that noise can never talk it into absurd speeds. */
    a->dsp->dit_ms = (float)clampd(dec_unit_ms(&a->rxdec), 24.0, 240.0);
    if (a->dsp->afc) {
        a->cfg.rx_freq = (int)(a->dsp->freq + 0.5f);
        applied.rx_freq = a->cfg.rx_freq;
    }
    /* A long silence ends the line: one transmission per line. */
    if (a->mode == MODE_RX && dsp_time_ms(a->dsp) - a->rx_last_char_ms > 4000.0)
        transcript_break(&a->rx_log);
}

void app_update(app_t *a)
{
    audio_event ev[256];
    dec_token tok;
    int n, i;
    double clock;

    a->now = plat_time_ms();
    if (a->now - a->audio_status_at > 500.0) {
        audio_get_status(&a->audio);
        a->audio_status_at = a->now;
    }

    /* Read the clock first: events polled afterwards can only be newer,
     * so a gap is never overestimated. */
    clock = audio_clock_ms();
    while ((n = audio_poll_events(ev, 256)) > 0) {
        for (i = 0; i < n; i++) {
            double t = audio_to_ms(ev[i].t);
            /* Playback (training) is not shown: it would give the answer away. */
            if (ev[i].src == SRC_OPERATOR) {
                tl_push(&a->tx_tl, t, ev[i].on);
                dec_key(&a->txdec, ev[i].on, t);
            }
        }
    }
    dec_update(&a->txdec, clock);
    while (dec_pop(&a->txdec, &tok))
        on_tx_token(a, &tok);

    if (a->rx_on)
        update_receiver(a);

    if (a->mode == MODE_TRX)
        trx_update(a);
}

/* ------------------------------------------------------------------------ */
/* Chrome                                                                    */
/* ------------------------------------------------------------------------ */

static void render_header(app_t *a)
{
    static const struct {
        int         mode;
        const char *label;
    } tabs[] = {{MODE_TX, "TX"}, {MODE_RX, "RX"}, {MODE_TRX, "TX/RX"}, {MODE_TRAIN, "TRAINING"}};
    int w = scr_w(), x = 1, i, rx;
    char buf[96];
    const char *kname;
    uint32_t kcol, ocol;

    scr_fill(rect_make(0, 0, w, 1), ' ', TH_FG, TH_SURFACE_2);
    x += scr_text(x, 0, " CW", TH_ON_ACCENT, TH_ACCENT, A_BOLD);
    x += scr_text(x, 0, "tty ", TH_ON_ACCENT, TH_ACCENT, 0);
    x += 2;
    for (i = 0; i < ARRAY_LEN(tabs); i++) {
        int active = a->mode == tabs[i].mode;
        uint32_t bg = active ? TH_ACCENT_BG : TH_SURFACE_2;
        scr_put(x++, 0, ' ', TH_FG, bg, 0);
        x += scr_text(x, 0, tabs[i].label, active ? TH_ACCENT : TH_FG_FAINT, bg, active ? A_BOLD : 0);
        scr_put(x++, 0, ' ', TH_FG, bg, 0);
        x += 1;
    }

    /* Right side, drawn from the right edge while there is room. */
    rx = w - 2;
    ocol = a->audio.out_ok ? TH_OK : TH_WARN;
    if (a->rx_on && a->demo) {
        rx -= 4;
        scr_text(rx, 0, "DEMO", TH_ACCENT, TH_SURFACE_2, A_BOLD);
        rx -= 2;
    } else if (a->rx_on) {
        uint32_t icol = a->audio.in_ok ? TH_OK : TH_WARN;
        rx -= 4;
        scr_text(rx, 0, "IN", TH_FG_FAINT, TH_SURFACE_2, 0);
        scr_put(rx + 3, 0, a->audio.in_ok ? 0x25CF : 0x25CB, icol, TH_SURFACE_2, 0);
        rx -= 2;
    }
    rx -= 5;
    scr_text(rx, 0, "OUT", TH_FG_FAINT, TH_SURFACE_2, 0);
    scr_put(rx + 4, 0, a->audio.out_ok ? 0x25CF : 0x25CB, ocol, TH_SURFACE_2, 0);
    rx -= 2;

    if (a->kbd->proto == KBD_LEGACY || !a->kbd->release_text) {
        kname = a->kbd->proto == KBD_LEGACY ? "KEYS legacy" : "KEYS partial";
        kcol = TH_WARN;
    } else {
        kname = a->kbd->proto == KBD_WIN32 ? "KEYS win32" : "KEYS kitty";
        kcol = TH_FG_FAINT;
    }
    if (rx - utf8_width(kname) - 2 > x + 1) {
        rx -= utf8_width(kname) + 2;
        scr_text(rx, 0, kname, kcol, TH_SURFACE_2, 0);
        scr_put(rx + utf8_width(kname) + 1, 0, a->kbd->proto == KBD_LEGACY ? 0x25B2 : 0x25CF,
                a->kbd->proto == KBD_LEGACY ? TH_WARN : TH_OK, TH_SURFACE_2, 0);
        rx -= 3;
    }

    snprintf(buf, sizeof buf, "%d WPM  %d Hz  %s", a->cfg.wpm, a->cfg.tone_hz,
             keyer_mode_name(a->cfg.keyer_mode));
    rx -= utf8_width(buf);
    if (rx > x + 2)
        scr_text(rx, 0, buf, TH_FG_MUTED, TH_SURFACE_2, 0);
}

typedef struct {
    const char *key;
    const char *label;
} hint_t;

static int mode_hints(app_t *a, hint_t *h, int cap)
{
    int n = 0;
#define H(k, l)                                                                          \
    do {                                                                                 \
        if (n < cap) {                                                                   \
            h[n].key = (k);                                                              \
            h[n].label = (l);                                                            \
            n++;                                                                         \
        }                                                                                \
    } while (0)
    switch (app_top_modal(a)) {
    case MODAL_MENU:
        H("↑↓", "Select");
        H("Enter", "Open");
        H("Esc", "Close");
        return n;
    case MODAL_SETTINGS:
        H("↑↓", "Select");
        H("←→", "Change");
        H("Space", "Test tone");
        H("Esc", "Save & close");
        return n;
    case MODAL_HELP:
        H("↑↓", "Scroll");
        H("Esc", "Close");
        return n;
    default:
        break;
    }
    H("C-x", "Menu");
    if (app_has_modal(a, MODAL_TABLE))
        H("Esc", "Close table");
    switch (a->mode) {
    case MODE_HOME:
        break;
    case MODE_TX:
        H("Space", "Straight key");
        H("← →", "Paddles");
        H("Bksp", "Delete");
        H("C-l", "Clear");
        break;
    case MODE_RX:
        H("+ -", "Sensitivity");
        H("< >", "Tune");
        H("z", "Zero-beat");
        H("b", "Filter");
        H("a", "AFC");
        H("s", "Speed lock");
        H("C-l", "Clear");
        break;
    case MODE_TRX:
        H("Space ← →", "Transmit");
        H("+ -", "Sensitivity");
        H("< >", "Tune");
        H("z", "Zero-beat");
        H("C-l", "Clear");
        break;
    case MODE_TRAIN:
        if (a->tr.state == TS_SETUP) {
            H("↑↓", "Select");
            H("←→", "Change");
            H("Enter", "Start");
        } else if (a->cfg.train_dir == TRAIN_COPY) {
            H("Type", "Answer");
            H("Enter", "Check / next");
            H("C-r", "Replay");
            if (!app_has_modal(a, MODAL_TABLE))
                H("Esc", "Setup");
        } else {
            H("Space ← →", "Send");
            H("Enter", a->tr.state == TS_SEND_RESULT ? "Next" : "Check / skip");
            H("Tab", "Hint");
            H("Bksp", "Retry");
            if (!app_has_modal(a, MODAL_TABLE))
                H("Esc", "Setup");
        }
        break;
    default:
        break;
    }
#undef H
    return n;
}

static void render_footer(app_t *a)
{
    int w = scr_w(), y = scr_h() - 1, x = 1, i, n, right = w - 1;
    hint_t hints[12];

    scr_fill(rect_make(0, y, w, 1), ' ', TH_FG, TH_BG);

    if (a->toast[0] && a->now < a->toast_until) {
        int tw = utf8_width(a->toast) + 4;
        right = w - tw - 1;
        scr_put(right + 1, y, a->toast_warn ? 0x25B2 : 0x25CF, a->toast_warn ? TH_WARN : TH_ACCENT, TH_BG, 0);
        scr_text(right + 3, y, a->toast, a->toast_warn ? TH_WARN : TH_FG, TH_BG, 0);
    }

    n = mode_hints(a, hints, ARRAY_LEN(hints));
    for (i = 0; i < n; i++) {
        int hw = utf8_width(hints[i].key) + 3 + utf8_width(hints[i].label);
        if (x + hw >= right)
            break;
        x += ui_hint(x, y, hints[i].key, hints[i].label, TH_BG) + 2;
    }
}

static void render_too_small(void)
{
    char msg[64];
    int w = scr_w(), h = scr_h();
    snprintf(msg, sizeof msg, "%dx%d", w, h);
    scr_text(maxi(0, (w - 18) / 2), h / 2 - 1, "Terminal too small", TH_ACCENT, TH_BG, A_BOLD);
    snprintf(msg, sizeof msg, "need %dx%d, have %dx%d", MIN_W, MIN_H, w, h);
    scr_text(maxi(0, (w - utf8_width(msg)) / 2), h / 2 + 1, msg, TH_FG_MUTED, TH_BG, 0);
}

void app_render(app_t *a)
{
    int w = scr_w(), h = scr_h(), i;
    rect_t content;

    scr_begin(TH_BG);
    if (w < MIN_W || h < MIN_H) {
        render_too_small();
        return;
    }
    render_header(a);
    if (h >= 26)
        content = rect_make(1, 2, w - 2, h - 4);
    else
        content = rect_make(1, 1, w - 2, h - 2);

    switch (a->mode) {
    case MODE_HOME:
        home_render(a, content);
        break;
    case MODE_TX:
        tx_render(a, content);
        break;
    case MODE_RX:
        rx_render(a, content);
        break;
    case MODE_TRX:
        trx_render(a, content);
        break;
    case MODE_TRAIN:
        train_render(a, content);
        break;
    default:
        break;
    }
    for (i = 0; i < a->nmodal; i++) {
        int kind = a->modals[i];
        /* The table floats over live activity: dim it only a little. */
        ui_backdrop(kind == MODAL_TABLE ? 0.8f : 0.45f);
        modal_render(a, kind);
    }
    render_footer(a); /* on top: it shows the hints of the active modal */
}
