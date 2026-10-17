/*
 * mode_train.c - Training: learn to copy by ear (the program sends, you
 * type) and to send (the program shows, you key). Material: single
 * characters (Koch method or fixed sets), common English words, Q-codes and
 * CW abbreviations. Characters you miss come back more often.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "bigfont.h"
#include "morse.h"
#include "theme.h"
#include "util.h"
#include "wordlists.h"

static const char *const CHARSET_NAMES[] = {"Koch method", "Letters", "Numbers", "Punctuation",
                                            "Letters & numbers", "Everything"};
static const char *const CONTENT_NAMES[] = {"Characters", "English words", "Q-codes & abbreviations"};

static const char LETTERS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char DIGITS[] = "0123456789";
static const char PUNCT[] = ".,?/=-()'\":;!@";

/* ------------------------------------------------------------------------ */
/* Material                                                                  */
/* ------------------------------------------------------------------------ */

static void charset_string(const settings_t *c, char *out, size_t cap)
{
    out[0] = 0;
    switch (c->train_charset) {
    case CHARSET_KOCH: {
        int n = clampi(c->train_koch, 2, (int)strlen(KOCH_ORDER));
        str_copy(out, (size_t)mini((int)cap, n + 1), KOCH_ORDER);
        break;
    }
    case CHARSET_LETTERS:
        str_copy(out, cap, LETTERS);
        break;
    case CHARSET_DIGITS:
        str_copy(out, cap, DIGITS);
        break;
    case CHARSET_PUNCT:
        str_copy(out, cap, PUNCT);
        break;
    case CHARSET_ALNUM:
        snprintf(out, cap, "%s%s", LETTERS, DIGITS);
        break;
    default:
        snprintf(out, cap, "%s%s%s", LETTERS, DIGITS, PUNCT);
        break;
    }
}

/* Weighted pick: characters you get wrong come back more often, and in a
 * Koch lesson the newest character is favoured. */
static char pick_char(training_t *t, const char *set, int koch)
{
    double w[64], total = 0, r;
    int n = (int)strlen(set), i;
    if (n > 64)
        n = 64;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)set[i];
        w[i] = 1.0 + 4.0 * (double)t->err[c & 127] / (double)(t->seen[c & 127] + 2);
        if (koch && i == n - 1)
            w[i] *= 2.0;
        total += w[i];
    }
    r = (double)rng_u32() / 4294967296.0 * total;
    for (i = 0; i < n; i++) {
        r -= w[i];
        if (r <= 0)
            return set[i];
    }
    return set[n - 1];
}

static void retry_copy(app_t *a)
{
    a->tr.typed[0] = 0;
    a->tr.ntyped = 0;
    a->tr.state = TS_LISTEN;
    a->tr.item_at = a->now;
    audio_stop_playback();
    audio_play_text(a->tr.target, a->cfg.wpm, a->cfg.farnsworth, 350.0);
}

static void retry_send(app_t *a)
{
    a->tr.sent[0] = 0;
    a->tr.state = TS_SEND;
    a->tr.item_at = a->now;
    dec_clear(&a->txdec);
}

static void new_item(app_t *a)
{
    training_t *t = &a->tr;
    const settings_t *c = &a->cfg;
    int idx, tries;

    t->meaning = NULL;
    switch (c->train_content) {
    case TRAIN_CHARS: {
        char set[96];
        int g = clampi(c->train_group, 1, 5), i;
        charset_string(c, set, sizeof set);
        for (i = 0; i < g; i++)
            t->target[i] = pick_char(t, set, c->train_charset == CHARSET_KOCH);
        t->target[g] = 0;
        break;
    }
    case TRAIN_WORDS:
        for (tries = 0, idx = rng_int(WORDS_EN_COUNT); idx == t->last_pick && tries < 4; tries++)
            idx = rng_int(WORDS_EN_COUNT);
        t->last_pick = idx;
        str_copy(t->target, sizeof t->target, WORDS_EN[idx]);
        break;
    default:
        for (tries = 0, idx = rng_int(CW_CODES_COUNT); idx == t->last_pick && tries < 4; tries++)
            idx = rng_int(CW_CODES_COUNT);
        t->last_pick = idx;
        str_copy(t->target, sizeof t->target, CW_CODES[idx].code);
        t->meaning = CW_CODES[idx].meaning;
        break;
    }
    t->typed[0] = 0;
    t->ntyped = 0;
    t->sent[0] = 0;
    t->item_at = a->now;

    if (c->train_dir == TRAIN_COPY) {
        retry_copy(a);
    } else {
        retry_send(a);
    }
}

/* Splits text into tokens ("A", "<AR>"), skipping spaces. */
static int split_tokens(const char *s, char (*out)[16], int cap)
{
    char tok[16];
    int n, k = 0;
    while (k < cap && (n = morse_next_token(s, tok, sizeof tok)) > 0) {
        s += n;
        if (tok[0] != ' ')
            str_copy(out[k++], 16, tok);
    }
    return k;
}

static void record(app_t *a, const char *expect, const char *got, int ok)
{
    training_t *t = &a->tr;
    char te[48][16], tg[48][16];
    int ne = split_tokens(expect, te, 48), ng = split_tokens(got, tg, 48), i;
    /* An early error says nothing about the characters not attempted. */
    if (!ok && ng < ne)
        ne = ng;
    /* Statistics per character of the target, compared token by token so
     * that "=" counts as "=" (not as the letters B and T). */
    for (i = 0; i < ne; i++) {
        char a1[16], b1[16];
        unsigned char ch = (unsigned char)te[i][0];
        if (te[i][1] != 0)
            continue; /* prosigns and multi-letter tokens: no per-character stats */
        morse_normalize(te[i], a1, sizeof a1);
        b1[0] = 0;
        if (i < ng)
            morse_normalize(tg[i], b1, sizeof b1);
        t->seen[ch & 127]++;
        if (strcmp(a1, b1) != 0)
            t->err[ch & 127]++;
        else if (t->err[ch & 127] > 0 && ok)
            t->err[ch & 127]--; /* forgive slowly when you get it right */
    }
    t->total++;
    if (ok) {
        t->correct++;
        t->streak++;
        if (t->streak > t->best)
            t->best = t->streak;
    } else {
        t->streak = 0;
    }
    memmove(t->hist + 1, t->hist, sizeof t->hist - 1);
    t->hist[0] = (unsigned char)ok;
    if (t->nhist < (int)sizeof t->hist)
        t->nhist++;
    t->ok = ok;

    /* Koch method: suggest the next lesson at 90% over 20+ items. */
    if (a->cfg.train_content == TRAIN_CHARS && a->cfg.train_charset == CHARSET_KOCH && t->total >= 20 &&
        t->correct * 10 >= t->total * 9 && t->total % 10 == 0 && a->cfg.train_koch < (int)strlen(KOCH_ORDER))
        app_toast(a, 0, "%d%% correct: ready for Koch lesson %d", t->correct * 100 / t->total,
                  a->cfg.train_koch + 1);
}

static int same_text(const char *x, const char *y)
{
    char nx[96], ny[96];
    morse_normalize(x, nx, sizeof nx);
    morse_normalize(y, ny, sizeof ny);
    return strcmp(nx, ny) == 0;
}

static void check_copy(app_t *a)
{
    training_t *t = &a->tr;
    record(a, t->target, t->typed, same_text(t->target, t->typed));
    if (t->ok) {
        t->state = TS_RESULT;
    } else {
        retry_copy(a);
        app_toast(a, 1, "Not quite: listen and start again");
    }
}

/* Compare normalized prefixes, allowing a prosign still being typed ("<A"). */
static int copy_prefix_matches(const training_t *t)
{
    char expect[96], got[96], candidate[96];
    const char *partial;
    int i;
    morse_normalize(t->target, expect, sizeof expect);
    morse_normalize(t->typed, got, sizeof got);
    if (strncmp(expect, got, strlen(got)) == 0)
        return 1;
    partial = strrchr(t->typed, '<');
    if (!partial || strchr(partial, '>'))
        return 0;
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        const morse_entry *e = &MORSE_TABLE[i];
        if (e->group != MG_PROSIGN || strncmp(e->text, partial, strlen(partial)) != 0)
            continue;
        snprintf(candidate, sizeof candidate, "%.*s%s", (int)(partial - t->typed), t->typed, e->text);
        morse_normalize(candidate, got, sizeof got);
        if (strncmp(expect, got, strlen(got)) == 0)
            return 1;
    }
    return 0;
}

/* -1: wrong character, 0: correct prefix, 1: complete answer. */
static int compare_send(const app_t *a)
{
    char expect[96], sent[96];
    size_t len;
    morse_normalize(a->tr.target, expect, sizeof expect);
    morse_normalize(a->tr.sent, sent, sizeof sent);
    if (!a->cfg.train_strict) {
        char *read = sent, *write = sent;
        while (*read) {
            if (*read != ' ')
                *write++ = *read;
            read++;
        }
        *write = 0;
    }
    len = strlen(sent);
    if (strncmp(expect, sent, len) != 0)
        return -1;
    return expect[len] == 0 ? 1 : 0;
}

static void check_send(app_t *a)
{
    training_t *t = &a->tr;
    t->send_wpm = dec_wpm(&a->txdec);
    t->send_ratio = dec_ratio(&a->txdec);
    record(a, t->target, t->sent, compare_send(a) == 1);
    if (t->ok) {
        t->state = TS_SEND_RESULT;
    } else {
        app_release_keys(a);
        retry_send(a);
        app_toast(a, 1, "Not quite: start sending again from the beginning");
    }
}

/* ------------------------------------------------------------------------ */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------ */

void train_enter(app_t *a)
{
    a->tr.state = TS_SETUP;
}

void train_leave(app_t *a)
{
    audio_stop_playback();
    a->tr.state = TS_SETUP;
}

int train_wants_keying(const app_t *a)
{
    return a->cfg.train_dir == TRAIN_SEND && (a->tr.state == TS_SEND || a->tr.state == TS_SEND_RESULT);
}

void train_operator_pressed(app_t *a)
{
    /* Keep the target until Enter requests another item. */
    if (a->tr.state == TS_SEND_RESULT)
        retry_send(a);
}

void train_on_sent(app_t *a, const dec_token *tok)
{
    training_t *t = &a->tr;
    size_t len = strlen(t->sent);
    if (t->state != TS_SEND)
        return;
    if (tok->kind == TOK_SPACE) {
        /* Long pauses do not finish an incomplete word. Strict mode keeps
         * them so an inter-word gap inside the target fails the comparison. */
        if (a->cfg.train_strict && len > 0 && t->sent[len - 1] != ' ' && len + 1 < sizeof t->sent) {
            t->sent[len] = ' ';
            t->sent[len + 1] = 0;
        }
        return;
    }
    if (len + strlen(tok->text) + 1 < sizeof t->sent)
        strcat(t->sent, tok->text);
    if (compare_send(a) != 0)
        check_send(a);
}

/* ------------------------------------------------------------------------ */
/* Setup form                                                                */
/* ------------------------------------------------------------------------ */

enum { ROW_DIR, ROW_CONTENT, ROW_CHARSET, ROW_KOCH, ROW_GROUP, ROW_HINT, ROW_SPACING, ROW_START };

static int setup_rows(const app_t *a, int *rows)
{
    int n = 0;
    rows[n++] = ROW_DIR;
    rows[n++] = ROW_CONTENT;
    if (a->cfg.train_content == TRAIN_CHARS) {
        rows[n++] = ROW_CHARSET;
        if (a->cfg.train_charset == CHARSET_KOCH)
            rows[n++] = ROW_KOCH;
        rows[n++] = ROW_GROUP;
    }
    if (a->cfg.train_dir == TRAIN_SEND) {
        rows[n++] = ROW_HINT;
        rows[n++] = ROW_SPACING;
    }
    rows[n++] = ROW_START;
    return n;
}

static void setup_change(app_t *a, int row, int delta)
{
    settings_t *c = &a->cfg;
    switch (row) {
    case ROW_DIR: c->train_dir = !c->train_dir; break;
    case ROW_CONTENT: c->train_content = (c->train_content + 3 + delta) % 3; break;
    case ROW_CHARSET: c->train_charset = (c->train_charset + 6 + delta) % 6; break;
    case ROW_KOCH: c->train_koch = clampi(c->train_koch + delta, 2, (int)strlen(KOCH_ORDER)); break;
    case ROW_GROUP: c->train_group = clampi(c->train_group + delta, 1, 5); break;
    case ROW_HINT: c->train_hint = !c->train_hint; break;
    case ROW_SPACING: c->train_strict = !c->train_strict; break;
    default: return;
    }
    app_save_settings(a);
}

static void start_session(app_t *a)
{
    training_t *t = &a->tr;
    t->total = t->correct = t->streak = t->best = 0;
    t->nhist = 0;
    t->last_pick = -1;
    new_item(a);
    app_refresh_operator(a);
}

static void render_setup(app_t *a, rect_t r)
{
    int rows[8], n = setup_rows(a, rows), i, y;
    int fw = mini(r.w - 4, 76), fx = r.x + (r.w - fw) / 2;
    char val[96], set[96];
    const settings_t *c = &a->cfg;

    ui_panel(r, "Training", "set up a session", 0);
    y = r.y + maxi(2, (r.h - (n + 12)) / 2);
    scr_text(fx, y, c->train_dir == TRAIN_COPY ? "Copy by ear: CWtty sends, you type what you hear."
                                               : "Send: CWtty shows the text, you key it with Space or the paddles.",
             TH_FG_MUTED, COLOR_KEEP, A_ITALIC);
    y += 2;

    if (a->tr.sel >= n)
        a->tr.sel = n - 1;
    for (i = 0; i < n; i++) {
        int sel = i == a->tr.sel, row = rows[i];
        uint32_t bg = sel ? TH_ACCENT_BG : TH_SURFACE;
        const char *label = "";
        val[0] = 0;
        if (row == ROW_START) {
            const char *btn = "  Start session  ";
            int bx = fx + (fw - utf8_width(btn) - 8) / 2;
            y++;
            scr_text(bx, y, " Enter ", sel ? TH_ON_ACCENT : TH_ACCENT, sel ? TH_ACCENT_HI : TH_SURFACE_3, A_BOLD);
            scr_text(bx + 8, y, btn, sel ? TH_ON_ACCENT : TH_FG, sel ? TH_ACCENT : TH_SURFACE_3, A_BOLD);
            y++;
            continue;
        }
        switch (row) {
        case ROW_DIR:
            label = "Direction";
            str_copy(val, sizeof val, c->train_dir == TRAIN_COPY ? "Receive  (copy)" : "Send  (key)");
            break;
        case ROW_CONTENT:
            label = "Material";
            str_copy(val, sizeof val, CONTENT_NAMES[c->train_content]);
            break;
        case ROW_CHARSET:
            label = "Characters";
            str_copy(val, sizeof val, CHARSET_NAMES[c->train_charset]);
            break;
        case ROW_KOCH:
            label = "Koch lesson";
            snprintf(val, sizeof val, "%d characters", c->train_koch);
            break;
        case ROW_GROUP:
            label = "Group size";
            snprintf(val, sizeof val, "%d", c->train_group);
            break;
        case ROW_HINT:
            label = "Pattern hint";
            str_copy(val, sizeof val, c->train_hint ? "Shown" : "Hidden");
            break;
        case ROW_SPACING:
            label = "Spacing";
            str_copy(val, sizeof val, c->train_strict ? "Strict (whole word)" : "Relaxed (any pause)");
            break;
        default:
            break;
        }
        scr_fill(rect_make(fx, y, fw, 1), ' ', TH_FG, bg);
        if (sel)
            scr_put(fx, y, 0x258C, TH_ACCENT, bg, 0);
        scr_text(fx + 2, y, label, sel ? TH_FG : TH_FG_MUTED, bg, 0);
        scr_text(fx + 18, y, "◂", sel ? TH_ACCENT : TH_FG_GHOST, bg, 0);
        scr_text(fx + 20, y, val, sel ? TH_ACCENT : TH_FG, bg, A_BOLD);
        scr_text(fx + 21 + utf8_width(val), y, "▸", sel ? TH_ACCENT : TH_FG_GHOST, bg, 0);
        if (row == ROW_KOCH) {
            /* The lesson's characters, the newest one highlighted. */
            int k, kx = fx + 40;
            charset_string(c, set, sizeof set);
            for (k = 0; set[k] && kx < fx + fw - 1; k++, kx += 2)
                scr_put(kx, y, (unsigned char)set[k], set[k + 1] ? TH_FG_MUTED : TH_ACCENT_HI, bg,
                        set[k + 1] ? 0 : A_BOLD);
        }
        y++;
    }

    y += 1;
    {
        static const char *const about[] = {
            ("The Koch method plays every character at full speed from day one. Start with two and add "
             "one when you score 90%."),
            "Frequent English words, the backbone of every conversation on the air.",
            "Q-codes and abbreviations heard in real contacts, each shown with its meaning.",
        };
        const char *txt = c->train_content == TRAIN_CHARS
                              ? (c->train_charset == CHARSET_KOCH ? about[0] : "Random characters from the chosen set.")
                              : about[c->train_content];
        /* Wrap the description on up to two lines. */
        int len = (int)strlen(txt), cut = len;
        if (len > fw) {
            for (cut = fw; cut > 0 && txt[cut] != ' '; cut--) {
            }
        }
        if (y + 2 < r.y + r.h - 1) {
            char line[256];
            str_copy(line, (size_t)mini(cut + 1, (int)sizeof line), txt);
            scr_text(fx, y, line, TH_FG_FAINT, COLOR_KEEP, 0);
            if (cut < len)
                scr_text(fx, y + 1, txt + cut + 1, TH_FG_FAINT, COLOR_KEEP, 0);
            y += 3;
        }
    }
    {
        char buf[160];
        if (c->farnsworth > 0 && c->farnsworth < c->wpm)
            snprintf(buf, sizeof buf, "Speed %d WPM, spaced as %d WPM (Farnsworth)  ·  tone %d Hz  ·  C-x s to change",
                     c->wpm, c->farnsworth, c->tone_hz);
        else
            snprintf(buf, sizeof buf, "Speed %d WPM  ·  tone %d Hz  ·  C-x s to change", c->wpm, c->tone_hz);
        if (y < r.y + r.h - 1)
            scr_text(fx, y, buf, TH_FG_FAINT, COLOR_KEEP, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Session                                                                   */
/* ------------------------------------------------------------------------ */

/* Big text if it fits, else regular text; centered. */
static void big_or_small(rect_t r, int y, const char *text, uint32_t col)
{
    char g[64];
    int i, j = 0;
    /* Prosigns are shown as their letters. */
    for (i = 0; text[i] && j < (int)sizeof g - 1; i++)
        if (text[i] != '<' && text[i] != '>')
            g[j++] = text[i];
    g[j] = 0;
    if (bigfont_width(g) <= r.w - 4)
        bigfont_draw(r.x + (r.w - bigfont_width(g)) / 2, y, g, col, COLOR_KEEP);
    else
        scr_text(r.x + (r.w - utf8_width(text)) / 2, y + 1, text, col, COLOR_KEEP, A_BOLD);
}

/* Patterns of a whole text: "━ ━━━   ━━━ ━ ━". */
static int text_pattern_width(const char *text)
{
    char tok[16];
    int n, w = 0, first = 1;
    while ((n = morse_next_token(text, tok, sizeof tok)) > 0) {
        const morse_entry *e = morse_lookup(tok);
        text += n;
        if (!e)
            continue;
        w += (first ? 0 : 3) + ui_pattern_width(e->code);
        first = 0;
    }
    return w;
}

static void text_pattern(int x, int y, const char *text, uint32_t col)
{
    char tok[16];
    int n, first = 1;
    while ((n = morse_next_token(text, tok, sizeof tok)) > 0) {
        const morse_entry *e = morse_lookup(tok);
        text += n;
        if (!e)
            continue;
        if (!first)
            x += 3;
        x += ui_pattern(x, y, e->code, col, COLOR_KEEP);
        first = 0;
    }
}

static void centered(rect_t r, int y, const char *s, uint32_t col, int attr)
{
    scr_text(r.x + (r.w - utf8_width(s)) / 2, y, s, col, COLOR_KEEP, attr);
}

static void render_stats(app_t *a, rect_t r)
{
    training_t *t = &a->tr;
    char buf[160];
    int x = r.x, i, worst[5], nw = 0, c;

    /* Recent results as dots, newest on the right. */
    x += scr_text(x, r.y, "RECENT", TH_FG_FAINT, COLOR_KEEP, 0) + 1;
    for (i = mini(t->nhist, 20) - 1; i >= 0; i--)
        scr_put(x++, r.y, t->hist[i] ? 0x25CF : 0x25CB, t->hist[i] ? TH_OK : TH_ERR, COLOR_KEEP, 0);
    x += 3;
    if (t->total > 0) {
        snprintf(buf, sizeof buf, "%d/%d  %d%%  streak %d  best %d", t->correct, t->total,
                 t->correct * 100 / t->total, t->streak, t->best);
        x += scr_text(x, r.y, buf, TH_FG_MUTED, COLOR_KEEP, 0) + 3;
    }
    /* The characters you miss most. */
    for (c = 33; c < 127; c++) {
        if (t->err[c] <= 0)
            continue;
        if (nw < 5) {
            worst[nw++] = c;
        } else {
            int m = 0;
            for (i = 1; i < 5; i++)
                if (t->err[worst[i]] < t->err[worst[m]])
                    m = i;
            if (t->err[c] > t->err[worst[m]])
                worst[m] = c;
        }
    }
    if (nw > 0 && x + 20 < r.x + r.w) {
        x += scr_text(x, r.y, "WEAK", TH_FG_FAINT, COLOR_KEEP, 0) + 1;
        for (i = 0; i < nw; i++) {
            scr_put(x, r.y, (uint32_t)worst[i], TH_WARN, COLOR_KEEP, A_BOLD);
            x += 2;
        }
    }
}

static void render_copy(app_t *a, rect_t in)
{
    training_t *t = &a->tr;
    int y = in.y + maxi(0, (in.h - 12) / 2);
    char buf[160];

    if (t->state == TS_LISTEN) {
        int dots = (int)(a->now / 350.0) % 4;
        snprintf(buf, sizeof buf, "%s%.*s", audio_playback_busy() ? "Listen" : "Type what you heard",
                 audio_playback_busy() ? dots : 0, "...");
        centered(in, y, buf, TH_FG_MUTED, 0);
        y += 2;
        if (t->typed[0])
            big_or_small(in, y, t->typed, TH_ACCENT);
        else
            centered(in, y + 1, app_blink(a, 530) ? "▁" : " ", TH_ACCENT, 0);
        y += BIGFONT_ROWS + 2;
        centered(in, y, "Each character is checked  ·  C-r to hear it again", TH_FG_FAINT, 0);
        return;
    }

    /* Result. */
    centered(in, y, t->ok ? "● Correct" : "▲ Not quite", t->ok ? TH_OK : TH_ERR, A_BOLD);
    y += 2;
    big_or_small(in, y, t->target, t->ok ? TH_OK : TH_ACCENT);
    y += BIGFONT_ROWS + 1;
    if (text_pattern_width(t->target) <= in.w - 2)
        text_pattern(in.x + (in.w - text_pattern_width(t->target)) / 2, y, t->target, TH_ACCENT_MID);
    y += 2;
    if (!t->ok) {
        snprintf(buf, sizeof buf, "you typed \"%s\"", t->typed[0] ? t->typed : "nothing");
        centered(in, y++, buf, TH_FG_MUTED, 0);
    }
    if (t->meaning)
        centered(in, y++, t->meaning, TH_FG, A_ITALIC);
    if (y + 1 < in.y + in.h)
        centered(in, y + 1, "Enter next  ·  C-r to hear it again", TH_FG_FAINT, 0);
}

static void render_send(app_t *a, rect_t in)
{
    training_t *t = &a->tr;
    int y = in.y + maxi(0, (in.h - 11) / 2);
    char buf[160];

    centered(in, y, t->state == TS_SEND ? "Send" : (t->ok ? "● Correct" : "▲ Not quite"),
             t->state == TS_SEND ? TH_FG_MUTED : (t->ok ? TH_OK : TH_ERR), t->state == TS_SEND ? 0 : A_BOLD);
    y += 2;
    big_or_small(in, y, t->target, t->state == TS_SEND_RESULT && t->ok ? TH_OK : TH_FG);
    y += BIGFONT_ROWS + 1;
    if ((a->cfg.train_hint || t->state == TS_SEND_RESULT) && text_pattern_width(t->target) <= in.w - 2)
        text_pattern(in.x + (in.w - text_pattern_width(t->target)) / 2, y, t->target,
                     a->cfg.train_hint ? TH_ACCENT_MID : TH_FG_FAINT);
    y += 2;
    if (t->meaning)
        centered(in, y, t->meaning, TH_FG_MUTED, A_ITALIC);
    y += 2;
    if (y >= in.y + in.h)
        return;
    if (t->state == TS_SEND) {
        const char *live = dec_pattern(&a->txdec);
        snprintf(buf, sizeof buf, "%s%s", t->sent[0] ? t->sent : "", app_blink(a, 530) ? "▁" : " ");
        centered(in, y, buf, TH_ACCENT, A_BOLD);
        if (live[0] && y + 1 < in.y + in.h)
            ui_pattern(in.x + (in.w - ui_pattern_width(live)) / 2, y + 1, live, TH_ACCENT, COLOR_KEEP);
    } else {
        if (t->ok)
            snprintf(buf, sizeof buf, "%.1f WPM  ·  dah/dit ratio 1:%.1f", t->send_wpm, t->send_ratio);
        else
            snprintf(buf, sizeof buf, "you sent \"%s\"", t->sent);
        centered(in, y, buf, t->ok ? TH_FG_MUTED : TH_FG, 0);
        if (y + 1 < in.y + in.h)
            centered(in, y + 1, "Enter next  ·  key or Bksp to retry", TH_FG_FAINT, 0);
    }
}

void train_render(app_t *a, rect_t r)
{
    training_t *t = &a->tr;
    char info[96], title[96];
    rect_t main;

    if (t->state == TS_SETUP) {
        render_setup(a, r);
        return;
    }
    if (a->cfg.train_content == TRAIN_CHARS && a->cfg.train_charset == CHARSET_KOCH)
        snprintf(title, sizeof title, "Training  %s  Koch %d", a->cfg.train_dir == TRAIN_COPY ? "copy" : "send",
                 a->cfg.train_koch);
    else
        snprintf(title, sizeof title, "Training  %s  %s", a->cfg.train_dir == TRAIN_COPY ? "copy" : "send",
                 a->cfg.train_content == TRAIN_CHARS ? CHARSET_NAMES[a->cfg.train_charset]
                                                     : CONTENT_NAMES[a->cfg.train_content]);
    if (a->cfg.train_dir == TRAIN_SEND)
        snprintf(info, sizeof info, "%s  ·  score %d/%d", a->cfg.train_strict ? "strict" : "relaxed",
                 t->correct, t->total);
    else if (t->total > 0)
        snprintf(info, sizeof info, "score %d/%d", t->correct, t->total);
    else
        info[0] = 0;

    if (a->cfg.train_dir == TRAIN_SEND) {
        int kh = r.h >= 26 ? 10 : 8;
        main = rect_make(r.x, r.y, r.w, r.h - kh);
        ui_panel(main, title, info, 0);
        render_send(a, rect_make(main.x + 2, main.y + 1, main.w - 4, main.h - 3));
        render_stats(a, rect_make(main.x + 2, main.y + main.h - 2, main.w - 4, 1));
        keying_panel(a, rect_make(r.x, r.y + r.h - kh, r.w, kh));
    } else {
        main = r;
        ui_panel(main, title, info, 0);
        render_copy(a, rect_make(main.x + 2, main.y + 1, main.w - 4, main.h - 3));
        render_stats(a, rect_make(main.x + 2, main.y + main.h - 2, main.w - 4, 1));
    }
}

int train_key(app_t *a, const key_event *ev)
{
    training_t *t = &a->tr;

    if (t->state == TS_SETUP) {
        int rows[8], n = setup_rows(a, rows);
        t->sel = clampi(t->sel, 0, n - 1);
        if (ev->mods & (MOD_CTRL | MOD_ALT))
            return 0;
        switch (ev->key) {
        case KEY_UP: t->sel = (t->sel + n - 1) % n; return 1;
        case KEY_DOWN:
        case KEY_TAB: t->sel = (t->sel + 1) % n; return 1;
        case KEY_LEFT: setup_change(a, rows[t->sel], -1); return 1;
        case KEY_RIGHT:
        case KEY_SPACE: setup_change(a, rows[t->sel], 1); return 1;
        case KEY_ENTER: start_session(a); return 1;
        default: return 0;
        }
    }

    if (ev->key == KEY_ESC || ((ev->mods & MOD_CTRL) && ev->key == 'g')) {
        audio_stop_playback();
        app_release_keys(a);
        t->state = TS_SETUP;
        app_refresh_operator(a);
        return 1;
    }

    if (a->cfg.train_dir == TRAIN_COPY) {
        if ((ev->mods & MOD_CTRL) && ev->key == 'r') {
            audio_stop_playback();
            audio_play_text(t->target, a->cfg.wpm, a->cfg.farnsworth, 200.0);
            return 1;
        }
        if (ev->mods & (MOD_CTRL | MOD_ALT))
            return 0;
        /* The result stays up, for another listen with C-r, until Enter. */
        if (t->state == TS_RESULT) {
            if (ev->key == KEY_ENTER)
                new_item(a);
            return 1;
        }
        if (ev->key == KEY_ENTER) {
            check_copy(a);
            return 1;
        }
        if (ev->key == KEY_BACKSPACE) {
            if (t->ntyped > 0)
                t->typed[--t->ntyped] = 0;
            return 1;
        }
        if (ev->text >= 0x20 && ev->text < 0x7F && t->ntyped < (int)sizeof t->typed - 1) {
            char ch = (char)ev->text;
            if (ch >= 'a' && ch <= 'z')
                ch = (char)(ch - 32);
            t->typed[t->ntyped++] = ch;
            t->typed[t->ntyped] = 0;
            if (same_text(t->target, t->typed) || !copy_prefix_matches(t))
                check_copy(a);
            return 1;
        }
        return 0;
    }

    /* Send. */
    if (ev->mods & (MOD_CTRL | MOD_ALT))
        return 0;
    switch (ev->key) {
    case KEY_TAB:
        a->cfg.train_hint = !a->cfg.train_hint;
        app_save_settings(a);
        return 1;
    case KEY_BACKSPACE:
        retry_send(a);
        return 1;
    case KEY_ENTER:
        if (t->state == TS_SEND_RESULT)
            new_item(a);
        else
            check_send(a);
        return 1;
    default:
        return 0;
    }
}
