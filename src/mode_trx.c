/*
 * mode_trx.c - Transceiver: receive by default, break in to transmit as
 * soon as the operator touches the key, return to receive after the
 * break-in delay.
 *
 * The PTT state machine below is the integration point for a radio: today
 * "transmit" means sidetone and a muted receiver; an SDR backend would key
 * its transmitter in ptt_on() / ptt_off() and take the key events from the
 * same audio event stream used for the sidetone.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "bigfont.h"
#include "theme.h"
#include "util.h"

/* How long the receiver stays deaf after transmitting, so the echo of the
 * sidetone in the room is not decoded as an answer. */
#define RX_RECOVERY_MS 150.0

static void ptt_on(app_t *a)
{
    dec_token tok;
    a->ptt = 1;
    a->ptt_at = a->now;
    /* Close the character the receiver was decoding, then go deaf. */
    dec_update(&a->rxdec, dsp_time_ms(a->dsp) + 1.0e6);
    while (dec_pop(&a->rxdec, &tok))
        if (tok.kind != TOK_SPACE)
            transcript_append(&a->trx_log, DIR_RX, tok.text,
                              tok.kind == TOK_PROSIGN ? STY_PROSIGN : (tok.kind == TOK_UNKNOWN ? STY_UNKNOWN : STY_NORMAL));
    dec_clear(&a->rxdec);
    dsp_mute(a->dsp, 1, 0);
}

static void ptt_off(app_t *a)
{
    a->ptt = 0;
    a->ptt_at = a->now;
    dsp_mute(a->dsp, 0, RX_RECOVERY_MS);
    dec_clear(&a->rxdec);
}

void trx_enter(app_t *a)
{
    a->ptt = 0;
    a->ptt_at = a->now;
    dsp_mute(a->dsp, 0, 0);
}

void trx_leave(app_t *a)
{
    if (a->ptt)
        ptt_off(a);
}

void trx_operator_pressed(app_t *a)
{
    if (!a->ptt)
        ptt_on(a);
}

void trx_update(app_t *a)
{
    if (a->ptt && !audio_operator_busy() && audio_operator_idle_ms() >= a->cfg.breakin_ms)
        ptt_off(a);
}

/* ------------------------------------------------------------------------ */
/* Screen                                                                    */
/* ------------------------------------------------------------------------ */

static void station_panel(app_t *a, rect_t r)
{
    rect_t in = rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    int lw = 22, x0 = in.x + lw + 3, rw = in.w - lw - 3, y, wf_h;
    double ms_per_col = clampd(1200.0 / a->cfg.wpm / 3.0, 5.0, 60.0);
    char info[64];

    snprintf(info, sizeof info, "break-in %d ms", a->cfg.breakin_ms);
    ui_panel(r, "Station", info, a->ptt);
    if (in.h < 6 || rw < 20)
        return;

    /* Left: the big PTT indicator and the break-in countdown. */
    {
        const char *label = a->ptt ? "TX" : "RX";
        int gx = in.x + (lw - bigfont_width(label)) / 2;
        uint32_t col = a->ptt ? TH_ACCENT : (a->dsp->open && a->dsp->state ? TH_RX : TH_FG_FAINT);
        if (a->ptt)
            scr_fill(rect_make(in.x, in.y, lw, BIGFONT_ROWS), ' ', TH_FG, TH_ACCENT_BG);
        bigfont_draw(gx, in.y, label, col, COLOR_KEEP);
        y = in.y + BIGFONT_ROWS + 1;
        if (y < in.y + in.h - 1) {
            double idle = audio_operator_idle_ms();
            double left = a->ptt ? (audio_operator_busy() ? a->cfg.breakin_ms : a->cfg.breakin_ms - idle) : 0;
            char buf[32];
            scr_text(in.x, y, a->ptt ? "BREAK-IN" : "LISTENING", a->ptt ? TH_ACCENT : TH_FG_FAINT, COLOR_KEEP, 0);
            ui_hbar(in.x, y + 1, lw - 8, a->ptt ? clampd(left / a->cfg.breakin_ms, 0, 1) : 0, TH_ACCENT,
                    TH_SURFACE_3);
            if (a->ptt) {
                snprintf(buf, sizeof buf, "%4.0f ms", clampd(left, 0, 9999));
                scr_text(in.x + lw - 7, y + 1, buf, TH_FG_MUTED, COLOR_KEEP, 0);
            }
        }
    }
    scr_vline(in.x + lw + 1, in.y, in.h - 1, 0x2502, TH_LINE, COLOR_KEEP);

    /* Right: waterfall, the two lanes, envelope and level. */
    wf_h = in.h - 1 - 4 - 1;
    y = in.y;
    if (wf_h >= 3) {
        rx_waterfall(a, rect_make(x0 + 4, y, rw - 4, wf_h));
        y += wf_h;
    }
    scr_text(x0, y, "RX", TH_FG_MUTED, COLOR_KEEP, A_BOLD);
    ui_ribbon(rect_make(x0 + 4, y, rw - 4, 1), &a->rx_tl, dsp_time_ms(a->dsp), ms_per_col, TH_RX, TH_SURFACE);
    y++;
    scr_text(x0, y, "TX", TH_ACCENT, COLOR_KEEP, A_BOLD);
    ui_ribbon(rect_make(x0 + 4, y, rw - 4, 1), &a->tx_tl, audio_clock_ms(), ms_per_col, TH_ACCENT, TH_SURFACE);
    y++;
    scr_text(x0, y, "ENV", TH_FG_FAINT, COLOR_KEEP, 0);
    ui_sparkline(rect_make(x0 + 4, y, rw - 4, 1), a->dsp, ms_per_col, TH_SURFACE);
    y++;
    scr_text(x0, y, "LVL", TH_FG_FAINT, COLOR_KEEP, 0);
    rx_meter(a, x0 + 4, y, rw - 4, TH_SURFACE);

    rx_status_line(a, in.x, in.y + in.h - 1, in.w, TH_SURFACE);
}

void trx_render(app_t *a, rect_t r)
{
    int sh = clampi(r.h / 2, 11, 17);
    rect_t log = rect_make(r.x, r.y, r.w, r.h - sh);
    char info[64];
    int cur = a->ptt ? DIR_TX : DIR_RX;

    if (sh > r.h - 5)
        sh = r.h - 5;
    log = rect_make(r.x, r.y, r.w, r.h - sh);
    if (transcript_count(&a->trx_log) == 0)
        snprintf(info, sizeof info, "listening, key to break in");
    else
        snprintf(info, sizeof info, "QSO log");
    ui_panel(log, "Transceiver", info, 0);
    ui_transcript(rect_make(log.x + 2, log.y + 1, log.w - 4, log.h - 2), &a->trx_log, 1, cur,
                  app_blink(a, 530) && (a->ptt || a->rx_on));
    station_panel(a, rect_make(r.x, r.y + r.h - sh, r.w, sh));
}

int trx_key(app_t *a, const key_event *ev)
{
    if ((ev->mods & MOD_CTRL) && ev->key == 'l') {
        transcript_clear(&a->trx_log);
        return 1;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    if (ev->key == KEY_ENTER) {
        transcript_break(&a->trx_log);
        return 1;
    }
    return rx_controls_key(a, ev);
}
