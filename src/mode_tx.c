/*
 * mode_tx.c - Transmit: key with Space (straight key) or the arrows (paddles)
 * and watch the text being decoded as you send.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "bigfont.h"
#include "keyer.h"
#include "morse.h"
#include "theme.h"
#include "util.h"

/* "<AR>" -> "AR" for the big font. */
static void glyph_text(const char *tok, char *out, size_t cap)
{
    size_t n = strlen(tok);
    if (n > 2 && tok[0] == '<' && tok[n - 1] == '>')
        str_copy(out, cap < n - 1 ? cap : n - 1, tok + 1);
    else
        str_copy(out, cap, tok);
}

void keying_panel(app_t *a, rect_t r)
{
    char info[64], glyph[8] = "";
    const char *live = dec_pattern(&a->txdec), *code = "", *name = NULL;
    const morse_entry *e = NULL;
    double ms_per_col = clampd(1200.0 / a->cfg.wpm / 3.0, 5.0, 60.0);
    int tone = tl_state(&a->tx_tl);
    rect_t in;
    uint32_t gcol = TH_FG, pcol = TH_ACCENT;
    int rows, y;

    snprintf(info, sizeof info, "%d WPM  %s%s", a->cfg.wpm, keyer_mode_name(a->cfg.keyer_mode),
             a->cfg.swap_paddles ? "  swapped" : "");
    ui_panel(r, "Keying", info, tone);
    in = rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    if (in.w < 20 || in.h < 1)
        return;

    /* The tape: newest on the right, one dit is three columns. */
    rows = in.h >= 7 ? 2 : 1;
    ui_ribbon(rect_make(in.x, in.y, in.w, rows), &a->tx_tl, audio_clock_ms(), ms_per_col, TH_ACCENT, TH_SURFACE);
    if (in.h < 6)
        return;

    /* What is being keyed right now, or what was decoded last. */
    if (live[0]) {
        e = morse_decode(live);
        code = live;
        pcol = TH_ACCENT;
        if (e) {
            glyph_text(e->text, glyph, sizeof glyph);
            gcol = TH_ACCENT;
            name = e->name;
        }
    } else if (a->tx_last[0]) {
        double age = a->now - a->tx_last_at;
        glyph_text(a->tx_last, glyph, sizeof glyph);
        code = a->tx_last_code;
        gcol = rgb_mix(TH_FG, TH_FG_GHOST, (float)clampd(age / 4000.0, 0.0, 0.85));
        pcol = rgb_mix(TH_ACCENT_MID, TH_FG_GHOST, (float)clampd(age / 4000.0, 0.0, 0.85));
        e = morse_lookup(a->tx_last);
        name = e ? e->name : NULL;
    }

    y = in.y + rows + 1;
    if (glyph[0])
        bigfont_draw(in.x + 1, y, glyph, gcol, COLOR_KEEP);
    {
        int mx = in.x + 21;
        scr_text(mx, y, live[0] ? "KEYING" : "LAST", TH_FG_FAINT, COLOR_KEEP, 0);
        if (code[0])
            ui_pattern(mx, y + 1, code, pcol, COLOR_KEEP);
        else
            scr_text(mx, y + 1, "—", TH_FG_GHOST, COLOR_KEEP, 0);
        if (name)
            scr_text(mx, y + 2, name, TH_FG_MUTED, COLOR_KEEP, 0);
        if (a->txdec.nmarks >= 4) {
            char buf[64];
            snprintf(buf, sizeof buf, "you %.1f WPM  1:%.1f", dec_wpm(&a->txdec), dec_ratio(&a->txdec));
            scr_text(mx, y + 3, buf, TH_FG_FAINT, COLOR_KEEP, 0);
        }
    }

    /* Keycaps, lit while pressed. The pad has no straight key. */
    if (in.w >= 21 + 24 + 36 && in.h >= rows + 4) {
        int kx = in.x + in.w - 35, ky = y;
        const char *left = a->cfg.swap_paddles ? "◂ DAH" : "◂ DIT";
        const char *right = a->cfg.swap_paddles ? "DIT ▸" : "DAH ▸";
        ui_keycap(rect_make(kx, ky, 10, 3), left, a->key_left);
        if (a->cfg.key_input == INPUT_PAD)
            scr_text(kx + 14, ky + 1, "USB PAD", TH_FG_FAINT, COLOR_KEEP, A_BOLD);
        else
            ui_keycap(rect_make(kx + 11, ky, 13, 3), "STRAIGHT", a->key_space);
        ui_keycap(rect_make(kx + 25, ky, 10, 3), right, a->key_right);
        scr_put(kx - 3, ky + 1, tone ? 0x25CF : 0x25CB, tone ? TH_ACCENT : TH_FG_GHOST, COLOR_KEEP, 0);
    }
}

void tx_render(app_t *a, rect_t r)
{
    int kh = r.h >= 24 ? 10 : (r.h >= 18 ? 8 : 4);
    rect_t text = rect_make(r.x, r.y, r.w, r.h - kh);
    rect_t keys = rect_make(r.x, r.y + r.h - kh, r.w, kh);
    char info[64] = "";

    if (transcript_count(&a->tx_log) == 0 && a->txdec.plen == 0)
        snprintf(info, sizeof info, "start keying");
    ui_panel(text, "Transmit", info, 0);
    ui_transcript(rect_make(text.x + 2, text.y + 1, text.w - 4, text.h - 2), &a->tx_log, 0, DIR_TX,
                  app_blink(a, 530));
    if (transcript_count(&a->tx_log) == 0 && a->txdec.plen == 0 && text.h > 6) {
        static const char *const keys[] = {"Space is a straight key: the tone lasts as long as you hold it.",
                                           "← and → are the paddles of an electronic keyer.",
                                           "What you send is decoded here."};
        static const char *const pad[] = {"The levers of the USB pad are the paddles of the keyer.",
                                          "Keep the mouse pointer over this window.",
                                          "What you send is decoded here."};
        const char *const *lines = a->cfg.key_input == INPUT_PAD ? pad : keys;
        int i, y = text.y + text.h / 2 - 2;
        for (i = 0; i < 3; i++)
            if (utf8_width(lines[i]) < text.w - 4)
                scr_text(text.x + (text.w - utf8_width(lines[i])) / 2, y + i, lines[i],
                         i == 2 ? TH_FG_FAINT : TH_FG_MUTED, COLOR_KEEP, 0);
    }
    keying_panel(a, keys);
}

int tx_key(app_t *a, const key_event *ev)
{
    if ((ev->mods & MOD_CTRL) && ev->key == 'l') {
        transcript_clear(&a->tx_log);
        dec_clear(&a->txdec);
        a->tx_last[0] = 0;
        return 1;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    if (ev->key == KEY_BACKSPACE) {
        transcript_backspace(&a->tx_log);
        return 1;
    }
    if (ev->key == KEY_ENTER) {
        transcript_break(&a->tx_log);
        return 1;
    }
    return 0;
}
