/*
 * mode_rx.c - Receive: decode CW from the microphone, with a waterfall,
 * signal meters and live controls for tuning and sensitivity.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "theme.h"
#include "util.h"

#define WF_FMIN 250.0f
#define WF_FMAX 1250.0f

/* ------------------------------------------------------------------------ */
/* Shared receiver widgets (also used by the transceiver)                    */
/* ------------------------------------------------------------------------ */

/* Signal level on a 50 dB scale above the noise, with the threshold. */
void rx_meter(app_t *a, int x, int y, int w, uint32_t bg)
{
    const rx_dsp *d = a->dsp;
    float lo = d->nf_db - 6.0f, span = 50.0f;
    int bw = w - 12, i, thr_col, nf_col;
    double frac;
    char buf[32];
    if (bw < 8)
        return;
    frac = (d->env_db - lo) / span;
    thr_col = (int)((d->thr_db - lo) / span * bw);
    nf_col = (int)((d->nf_db - lo) / span * bw);
    ui_hbar(x, y, bw, frac, d->env_db >= d->thr_db && d->open ? TH_ACCENT : TH_ACCENT_LO, bg);
    for (i = 0; i < bw; i++) {
        cell_t *c = scr_cell(x + i, y);
        if (c && c->ch == ' ')
            scr_put(x + i, y, 0x2500, TH_FG_GHOST, bg, 0);
    }
    if (nf_col >= 0 && nf_col < bw)
        scr_put(x + nf_col, y, 0x2502, TH_FG_FAINT, COLOR_KEEP, 0);
    if (thr_col >= 0 && thr_col < bw)
        scr_put(x + thr_col, y, 0x2503, TH_ACCENT_HI, COLOR_KEEP, 0);
    if (a->rx_on)
        snprintf(buf, sizeof buf, "SNR %3.0f dB", d->snr_db);
    else
        snprintf(buf, sizeof buf, "no input");
    scr_text(x + bw + 2, y, buf, d->open ? TH_FG : TH_FG_FAINT, bg, 0);
}

static int seg(int x, int y, const char *label, const char *value, uint32_t vcol, uint32_t bg)
{
    int x0 = x;
    x += scr_text(x, y, label, TH_FG_FAINT, bg, 0) + 1;
    x += scr_text(x, y, value, vcol, bg, A_BOLD);
    return x - x0;
}

void rx_status_line(app_t *a, int x, int y, int w, uint32_t bg)
{
    char buf[48];
    int x0 = x, i;
    const rx_dsp *d = a->dsp;

    snprintf(buf, sizeof buf, "%.0f Hz", d->freq);
    x += seg(x, y, "TUNE", buf, TH_FG, bg);
    if (a->cfg.rx_afc)
        x += scr_text(x + 1, y, "AFC", TH_ACCENT, bg, A_BOLD) + 1;
    x += 3;
    snprintf(buf, sizeof buf, "%d Hz", a->cfg.rx_bw);
    x += seg(x, y, "FILTER", buf, TH_FG, bg) + 3;

    x += scr_text(x, y, "SENS", TH_FG_FAINT, bg, 0) + 1;
    for (i = 1; i <= 10; i++)
        scr_put(x++, y, i <= a->cfg.rx_sens ? 0x2501 : 0x2500, i <= a->cfg.rx_sens ? TH_ACCENT : TH_FG_GHOST,
                bg, 0);
    x += scr_textf(x + 1, y, TH_FG, bg, A_BOLD, "%d", a->cfg.rx_sens) + 4;

    if (x - x0 + 22 < w) {
        snprintf(buf, sizeof buf, "%.0f WPM", a->cfg.rx_lock ? (double)a->cfg.rx_wpm : dec_wpm(&a->rxdec));
        x += seg(x, y, "SPEED", buf, TH_FG, bg);
        x += scr_text(x + 1, y, a->cfg.rx_lock ? "locked" : "auto", a->cfg.rx_lock ? TH_ACCENT : TH_FG_FAINT,
                      bg, 0) + 4;
    }
    if (x - x0 + 10 < w) {
        scr_put(x, y, d->open ? 0x25CF : 0x25CB, d->open ? TH_OK : TH_FG_FAINT, bg, 0);
        scr_text(x + 2, y, d->open ? "signal" : "squelch", d->open ? TH_FG : TH_FG_FAINT, bg, 0);
    }
}

int rx_controls_key(app_t *a, const key_event *ev)
{
    int ch = ev->text ? (int)ev->text : ev->key;
    settings_t *c = &a->cfg;
    int changed = 1;

    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    switch (ch) {
    case '+':
    case '=':
        c->rx_sens = mini(10, c->rx_sens + 1);
        app_toast(a, 0, "Sensitivity %d", c->rx_sens);
        break;
    case '-':
    case '_':
        c->rx_sens = maxi(1, c->rx_sens - 1);
        app_toast(a, 0, "Sensitivity %d", c->rx_sens);
        break;
    case '<':
    case ',':
        c->rx_freq -= (ev->mods & MOD_SHIFT) && ch == ',' ? 50 : 10;
        break;
    case '>':
    case '.':
        c->rx_freq += (ev->mods & MOD_SHIFT) && ch == '.' ? 50 : 10;
        break;
    case 'b':
    case ']':
        c->rx_bw = RX_BANDWIDTHS[(rx_bw_index(c->rx_bw) + 1) % RX_BANDWIDTH_COUNT];
        app_toast(a, 0, "Filter %d Hz", c->rx_bw);
        break;
    case 'B':
    case '[':
        c->rx_bw = RX_BANDWIDTHS[(rx_bw_index(c->rx_bw) + RX_BANDWIDTH_COUNT - 1) % RX_BANDWIDTH_COUNT];
        app_toast(a, 0, "Filter %d Hz", c->rx_bw);
        break;
    case 'a':
        c->rx_afc = !c->rx_afc;
        app_toast(a, 0, "AFC %s", c->rx_afc ? "on" : "off");
        break;
    case 's':
        c->rx_lock = !c->rx_lock;
        if (c->rx_lock)
            c->rx_wpm = clampi((int)(dec_wpm(&a->rxdec) + 0.5), 5, 60);
        app_toast(a, 0, c->rx_lock ? "Speed locked at %d WPM" : "Speed tracking on", c->rx_wpm);
        break;
    case 'z': {
        float snr, f = dsp_find_peak(a->dsp, WF_FMIN, WF_FMAX, &snr);
        if (snr > 8.0f) {
            c->rx_freq = (int)(f + 0.5f);
            app_toast(a, 0, "Tuned to %d Hz", c->rx_freq);
        } else {
            app_toast(a, 1, "No clear signal to tune to");
            changed = 0;
        }
        break;
    }
    default:
        return 0;
    }
    if (changed) {
        app_apply_settings(a);
        app_save_settings(a);
    }
    return 1;
}

void rx_waterfall(app_t *a, rect_t r)
{
    ui_waterfall(rect_make(r.x, r.y, r.w, r.h - 2), a->dsp, WF_FMIN, WF_FMAX);
    ui_freq_axis(rect_make(r.x, r.y + r.h - 2, r.w, 2), WF_FMIN, WF_FMAX, a->dsp->freq, (float)a->cfg.rx_bw,
                 TH_SURFACE);
}

/* Waterfall, axis, envelope, tape and meter: the "Signal" panel body. */
static void signal_body(app_t *a, rect_t in, int wf_h)
{
    int y = in.y, lw = 5;
    rect_t body = rect_make(in.x + lw, in.y, in.w - lw, in.h);
    double ms_per_col = clampd(dec_unit_ms(&a->rxdec) / 3.0, 5.0, 60.0);

    if (wf_h > 0) {
        rx_waterfall(a, rect_make(body.x, y, body.w, wf_h + 2));
        scr_text(in.x, y, "WF", TH_FG_FAINT, COLOR_KEEP, 0);
        y += wf_h + 3;
    }
    scr_text(in.x, y, "ENV", TH_FG_FAINT, COLOR_KEEP, 0);
    ui_sparkline(rect_make(body.x, y, body.w, 1), a->dsp, ms_per_col, TH_SURFACE);
    y++;
    scr_text(in.x, y, "KEY", TH_FG_FAINT, COLOR_KEEP, 0);
    ui_ribbon(rect_make(body.x, y, body.w, 1), &a->rx_tl, dsp_time_ms(a->dsp), ms_per_col, TH_RX, TH_SURFACE);
    y++;
    scr_text(in.x, y, "LVL", TH_FG_FAINT, COLOR_KEEP, 0);
    rx_meter(a, body.x, y, body.w, TH_SURFACE);
}

/* ------------------------------------------------------------------------ */
/* Screen                                                                    */
/* ------------------------------------------------------------------------ */

void rx_render(app_t *a, rect_t r)
{
    int wf_h = clampi(r.h - 20, 0, r.h > 40 ? 12 : 8);
    int sig_h = (wf_h > 0 ? wf_h + 3 : 0) + 3 + 2 + 2;
    rect_t text = rect_make(r.x, r.y, r.w, r.h - sig_h);
    rect_t sig = rect_make(r.x, r.y + r.h - sig_h, r.w, sig_h);
    char info[64];

    if (!a->rx_on)
        snprintf(info, sizeof info, "no audio input");
    else if (a->dsp->open)
        snprintf(info, sizeof info, "%.0f WPM", dec_wpm(&a->rxdec));
    else
        snprintf(info, sizeof info, "listening");
    ui_panel(text, "Receive", info, 0);
    ui_transcript(rect_make(text.x + 2, text.y + 1, text.w - 4, text.h - 2), &a->rx_log, 0, DIR_RX,
                  a->rx_on && app_blink(a, 530));

    snprintf(info, sizeof info, "%.0f Hz  %d Hz filter", a->dsp->freq, a->cfg.rx_bw);
    ui_panel(sig, "Signal", info, a->dsp->state);
    {
        rect_t in = rect_make(sig.x + 2, sig.y + 1, sig.w - 4, sig.h - 2);
        signal_body(a, rect_make(in.x, in.y, in.w, in.h - 2), wf_h);
        rx_status_line(a, in.x, in.y + in.h - 1, in.w, TH_SURFACE);
    }
}

int rx_key(app_t *a, const key_event *ev)
{
    if ((ev->mods & MOD_CTRL) && ev->key == 'l') {
        transcript_clear(&a->rx_log);
        a->rx_last[0] = 0;
        return 1;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    if (ev->key == KEY_UP || ev->key == KEY_RIGHT) {
        a->cfg.rx_freq += 10;
        app_apply_settings(a);
        return 1;
    }
    if (ev->key == KEY_DOWN || ev->key == KEY_LEFT) {
        a->cfg.rx_freq -= 10;
        app_apply_settings(a);
        return 1;
    }
    if (ev->key == KEY_ENTER) {
        transcript_break(&a->rx_log);
        return 1;
    }
    return rx_controls_key(a, ev);
}
