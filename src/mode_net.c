/*
 * mode_net.c - Rooms: key together with other stations over the
 * internet, from this terminal or the web app. Each station is heard on a
 * pitch of its own and decoded on a line with its callsign.
 *
 * The room itself lives in net.c; this is the screen, the log and the
 * join window's action. Leaving the screen leaves the room.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "theme.h"
#include "util.h"

/*
 * A station's line ends, as a transmission does on the air:
 *   - with K, KN, BK, AR or SK; BT (=) starts a new paragraph (over.c);
 *   - when another station starts transmitting, even without K, unless the
 *     first one is still keying (two at once keep a line each);
 *   - after a pause of five word spaces at its speed, at least 4 s.
 */
#define PAUSE_MS 4000.0
#define PAUSE_WORDS 5.0
#define KEYING_MS 1000.0   /* still keying: the key moved this recently... */
#define KEYING_UNITS 7.0   /* ...or within a word space */

static int token_style(const dec_token *t)
{
    return t->kind == TOK_PROSIGN ? STY_PROSIGN : (t->kind == TOK_UNKNOWN ? STY_UNKNOWN : STY_NORMAL);
}

/* The stations by index, -1 being the operator. */
static const char *who_of(app_t *a, int s)
{
    return s < 0 ? a->net->room.call : a->net->room.st[s].call;
}

static double unit_of(app_t *a, int s)
{
    return dec_unit_ms(s < 0 ? &a->txdec : &a->net->room.st[s].dec);
}

static double *heard_of(app_t *a, int s)
{
    return s < 0 ? &a->net_tx_heard : &a->net_heard[s];
}

static int present(app_t *a, int s)
{
    return s < 0 || a->net->room.st[s].used;
}

static int keying(app_t *a, int s)
{
    double active = s < 0 ? a->net_tx_active : a->net_active[s], idle = KEYING_UNITS * unit_of(a, s);
    if (idle < KEYING_MS)
        idle = KEYING_MS;
    return active > 0.0 && a->now - active < idle;
}

static void end_line(app_t *a, int s)
{
    transcript_break_who(&a->net_log, who_of(a, s));
    *heard_of(a, s) = 0.0;
}

void online_text(app_t *a, int s, const dec_token *tok)
{
    const char *who = who_of(a, s);
    int i;
    if (tok->kind != TOK_SPACE && !transcript_open_who(&a->net_log, who)) {
        /* A new transmission: whoever was on before has finished. */
        for (i = -1; i < ROOM_STATIONS; i++)
            if (i != s && present(a, i) && *heard_of(a, i) > 0.0 && !keying(a, i))
                end_line(a, i);
    }
    transcript_append_who(&a->net_log, s < 0 ? DIR_TX : DIR_RX, who, tok->text, token_style(tok));
    *heard_of(a, s) = a->now;
    if (over_token(s < 0 ? &a->net_tx_over : &a->net_over[s], tok) != OVER_NONE)
        end_line(a, s);
}

static void system_line(app_t *a, const char *text)
{
    transcript_append(&a->net_log, DIR_SYS, text, STY_NORMAL);
    transcript_break(&a->net_log);
}

void online_enter(app_t *a)
{
    if (!a->net) {
        app_toast(a, 1, "The network could not be started");
        return;
    }
    if (!net_joined(a->net))
        app_open_modal(a, MODAL_JOIN);
}

void online_leave(app_t *a)
{
    char buf[64];
    if (!a->net || !net_joined(a->net))
        return;
    snprintf(buf, sizeof buf, "You left %s", a->net->room.name);
    net_leave(a->net);
    system_line(a, buf);
}

int online_join(app_t *a)
{
    char call[ROOM_CALL_CAP], room[ROOM_NAME_CAP], buf[96];
    int err, s;

    if (!a->net) {
        str_copy(a->join_error, sizeof a->join_error, "The network could not be started");
        return -1;
    }
    str_copy(call, sizeof call, a->join_call);
    str_copy(room, sizeof room, a->join_room);
    if (!room_clean_call(call)) {
        str_copy(a->join_error, sizeof a->join_error, "Enter your callsign: letters, digits and /");
        a->join_sel = 0;
        return -1;
    }
    if (!room_clean_name(room)) {
        str_copy(a->join_error, sizeof a->join_error, "Enter the name of a room");
        a->join_sel = 1;
        return -1;
    }
    if (net_joined(a->net))
        online_leave(a);
    err = net_join(a->net, call, room, a->join_pass, a->cfg.wpm);
    memset(a->join_pass, 0, sizeof a->join_pass);
    if (err) {
        str_copy(a->join_error, sizeof a->join_error, "Could not enter the room");
        return -1;
    }
    str_copy(a->cfg.callsign, sizeof a->cfg.callsign, call);
    str_copy(a->cfg.net_room, sizeof a->cfg.net_room, room);
    app_save_settings(a);
    for (s = 0; s < ROOM_STATIONS; s++) {
        a->net_tl[s].count = 0;
        a->net_heard[s] = a->net_active[s] = 0.0;
        over_reset(&a->net_over[s]);
    }
    a->net_tx_heard = a->net_tx_active = 0.0;
    over_reset(&a->net_tx_over);
    a->net_up = 0;
    snprintf(buf, sizeof buf, "You entered %s %s as %s", a->net->room.priv ? "the private room" : "room",
             a->net->room.name, a->net->room.call);
    system_line(a, buf);
    app_refresh_operator(a);
    return 0;
}

void online_update(app_t *a)
{
    net_t *n = a->net;
    room_event e;
    char buf[64];
    int s, up;

    while (net_pop_event(n, &e)) {
        const room_station *st = &n->room.st[e.station];
        switch (e.kind) {
        case RE_JOIN:
            snprintf(buf, sizeof buf, "%s is in the room", st->call);
            system_line(a, buf);
            a->net_tl[e.station].count = 0;
            a->net_heard[e.station] = a->net_active[e.station] = 0.0;
            over_reset(&a->net_over[e.station]);
            break;
        case RE_LEAVE:
            transcript_break_who(&a->net_log, st->call);
            a->net_heard[e.station] = 0.0;
            snprintf(buf, sizeof buf, "%s left", st->call);
            system_line(a, buf);
            break;
        case RE_KEY:
            tl_push(&a->net_tl[e.station], e.t, e.on);
            a->net_active[e.station] = a->now;
            break;
        case RE_TOKEN:
            online_text(a, e.station, &e.tok);
            break;
        default:
            break;
        }
    }
    if (!net_joined(n))
        return;

    /* A pause ends a line too: the next transmission starts a new one. */
    for (s = -1; s < ROOM_STATIONS; s++) {
        double limit;
        if (!present(a, s) || *heard_of(a, s) <= 0.0 || keying(a, s))
            continue;
        limit = PAUSE_WORDS * 7.0 * unit_of(a, s);
        if (limit < PAUSE_MS)
            limit = PAUSE_MS;
        if (a->now - *heard_of(a, s) > limit)
            end_line(a, s);
    }

    up = net_links_up(n);
    if (up == 0 && a->net_up > 0)
        app_toast(a, 1, "Connection to the room lost: reconnecting");
    a->net_up = up;
}

/* ------------------------------------------------------------------------ */
/* Screen                                                                    */
/* ------------------------------------------------------------------------ */

static void stations_panel(app_t *a, rect_t r)
{
    net_t *n = a->net;
    rect_t in = rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    int y = in.y, s, others = 0, i;
    char buf[96], info[32];

    for (s = 0; s < ROOM_STATIONS; s++)
        others += n->room.st[s].used;
    snprintf(info, sizeof info, "%d", others + (net_joined(n) ? 1 : 0));
    ui_panel(r, "Stations", net_joined(n) ? info : "", 0);
    if (in.w < 10 || in.h < 2)
        return;

    if (net_joined(n)) {
        scr_put(in.x, y, 0x25CF, tl_state(&a->tx_tl) ? TH_ACCENT : TH_ACCENT_LO, COLOR_KEEP, 0);
        scr_text(in.x + 2, y, n->room.call, TH_ACCENT, COLOR_KEEP, A_BOLD);
        scr_text(in.x + 2 + utf8_width(n->room.call) + 1, y, "you", TH_FG_FAINT, COLOR_KEEP, 0);
        y++;
        for (s = 0; s < ROOM_STATIONS && y < in.y + in.h - BROKERS - 1; s++) {
            const room_station *st = &n->room.st[s];
            int keying, rx = in.x + 2 + ROOM_CALL_CAP + 5;
            if (!st->used)
                continue;
            keying = tl_state(&a->net_tl[s]);
            scr_put(in.x, y, keying ? 0x25CF : 0x25CB, keying ? TH_RX : TH_FG_FAINT, COLOR_KEEP, 0);
            scr_text(in.x + 2, y, st->call, TH_FG, COLOR_KEEP, A_BOLD);
            snprintf(buf, sizeof buf, "%d", st->tone_hz);
            scr_text(in.x + 2 + ROOM_CALL_CAP, y, buf, TH_FG_FAINT, COLOR_KEEP, 0);
            if (in.x + in.w - rx >= 6)
                ui_ribbon(rect_make(rx, y, in.x + in.w - rx, 1), &a->net_tl[s], audio_clock_ms(),
                          clampd(1200.0 / maxi(st->wpm, 5) / 3.0, 5.0, 60.0), TH_RX, TH_SURFACE);
            y++;
        }
        if (others == 0 && y < in.y + in.h - BROKERS - 1)
            scr_text(in.x + 2, y + 1, "nobody else yet", TH_FG_FAINT, COLOR_KEEP, A_ITALIC);
    }

    /* The brokers, at the bottom. */
    y = in.y + in.h - BROKERS;
    for (i = 0; i < BROKERS; i++, y++) {
        int st = net_link_status(n, i, buf, sizeof buf);
        uint32_t col = st > 0 ? TH_OK : (st < 0 ? TH_FG_GHOST : TH_WARN);
        if (!net_joined(n) && st >= 0)
            snprintf(buf, sizeof buf, "%s", brokers[i].name);
        scr_put(in.x, y, st > 0 ? 0x25CF : 0x25CB, col, COLOR_KEEP, 0);
        scr_clip(rect_make(in.x + 2, y, in.w - 2, 1));
        scr_text(in.x + 2, y, buf, st > 0 ? TH_FG_MUTED : TH_FG_FAINT, COLOR_KEEP, 0);
        scr_unclip();
    }
}

/* The right of the log's top border: privacy and how to pass the turn,
 * shortened to what fits beside the title (ui_panel draws it only then, so
 * it never covers the title). Returns the column of the K, -1 if none. */
static int log_info(app_t *a, rect_t log, const char *title, char *info, size_t cap)
{
    static const char *const pass[] = {"send K to pass the turn", "K passes the turn", "K: over"};
    int room = log.w - 9 - utf8_width(title), i, k;
    const char *priv = a->net->room.priv ? "private \xc2\xb7 " : "";

    if (net_links_up(a->net) == 0) {
        str_copy(info, cap, "connecting");
        return -1;
    }
    for (k = 0; k < 2; k++) {
        for (i = 0; i < ARRAY_LEN(pass); i++) {
            snprintf(info, cap, "%s%s", k == 0 ? priv : "", pass[i]);
            if (utf8_width(info) < room) {
                const char *at = strchr(pass[i], 'K');
                return utf8_width(k == 0 ? priv : "") + (int)(at - pass[i]);
            }
        }
    }
    str_copy(info, cap, priv[0] && utf8_width("private") < room ? "private" : "");
    return -1;
}

void online_render(app_t *a, rect_t r)
{
    int kh = r.h >= 24 ? 10 : (r.h >= 18 ? 8 : 4);
    int side = r.w >= 96 ? 34 : (r.w >= 76 ? 28 : 0);
    rect_t log = rect_make(r.x, r.y, r.w - side, r.h - kh);
    rect_t keys = rect_make(r.x, r.y + r.h - kh, r.w, kh);
    char title[64], info[64] = "";
    int joined = a->net && net_joined(a->net), kcol = -1;

    if (joined) {
        snprintf(title, sizeof title, "Room %s", a->net->room.name);
        kcol = log_info(a, log, title, info, sizeof info);
    } else {
        str_copy(title, sizeof title, "Rooms");
    }
    ui_panel(log, title, info, 0);
    if (kcol >= 0) /* the K stands out */
        scr_put(log.x + log.w - 3 - utf8_width(info) + kcol, log.y, 'K', TH_ACCENT, TH_BG, A_BOLD);
    ui_transcript(rect_make(log.x + 2, log.y + 1, log.w - 4, log.h - 2), &a->net_log, 2, DIR_TX,
                  joined && app_blink(a, 530));
    if (!joined && log.h > 7) {
        static const char *const lines[] = {"Not in a room.",
                                            "Press Enter to join one: everybody there hears everybody,",
                                            "each station on its own pitch, from a terminal or the web app."};
        static const char *const off[] = {"The network could not be started.", "", ""};
        const char *const *l = a->net ? lines : off;
        int i, y = log.y + log.h / 2 - 2;
        for (i = 0; i < 3; i++)
            if (l[i][0] && utf8_width(l[i]) < log.w - 4)
                scr_text(log.x + (log.w - utf8_width(l[i])) / 2, y + i, l[i], i == 0 ? TH_FG_MUTED : TH_FG_FAINT,
                         COLOR_KEEP, 0);
    }
    if (side && a->net)
        stations_panel(a, rect_make(r.x + r.w - side, r.y, side, r.h - kh));
    keying_panel(a, keys);
}

int online_key(app_t *a, const key_event *ev)
{
    if ((ev->mods & MOD_CTRL) && ev->key == 'l') {
        transcript_clear(&a->net_log);
        return 1;
    }
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    if (ev->key == KEY_ENTER && a->net) {
        app_open_modal(a, MODAL_JOIN);
        return 1;
    }
    return 0;
}
