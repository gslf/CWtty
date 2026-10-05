/*
 * mode_home.c - The welcome screen.
 */
#include "app.h"
#include "bigfont.h"
#include "theme.h"
#include "util.h"

void home_render(app_t *a, rect_t r)
{
    int logo_w = bigfont_width("CW") + 1 + bigfont_width("tty");
    int logo_h = BIGFONT_ROWS + 2;
    int y = r.y + maxi(0, (r.h - logo_h) / 2), x;

    /* Logo: "CW" in yellow, "tty" in white, the name in Morse below. */
    x = r.x + (r.w - logo_w) / 2;
    bigfont_draw(x, y, "CW", TH_ACCENT, COLOR_KEEP);
    bigfont_draw(x + bigfont_width("CW") + 1, y, "tty", TH_FG, COLOR_KEEP);
    y += BIGFONT_ROWS + 1;
    {
        int pw = ui_pattern_width("-.-.") + 3 + ui_pattern_width(".--");
        int px = r.x + (r.w - pw) / 2;
        px += ui_pattern(px, y, "-.-.", TH_ACCENT_MID, COLOR_KEEP) + 3;
        ui_pattern(px, y, ".--", TH_ACCENT_MID, COLOR_KEEP);
    }
}

int home_key(app_t *a, const key_event *ev)
{
    /* On the home screen single letters work without the C-x prefix. */
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    switch (ev->key) {
    case 't': app_set_mode(a, MODE_TX); return 1;
    case 'r': app_set_mode(a, MODE_RX); return 1;
    case 'x': app_set_mode(a, MODE_TRX); return 1;
    case 'p': app_set_mode(a, MODE_TRAIN); return 1;
    case 'm': app_open_modal(a, MODAL_TABLE); return 1;
    case 's': app_open_modal(a, MODAL_SETTINGS); return 1;
    case 'h':
    case '?': app_open_modal(a, MODAL_HELP); return 1;
    case KEY_ENTER: app_open_modal(a, MODAL_MENU); return 1;
    default: return 0;
    }
}
