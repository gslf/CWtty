/*
 * test_cwtty.c - Self-checking tests for the non-interactive core:
 * Morse table, keyer timing, timing decoder and the full receive chain
 * (synthetic CW audio with noise -> DSP -> decoder).
 */
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "app.h"
#include "dsp.h"
#include "input.h"
#include "keyer.h"
#include "morse.h"
#include "platform.h"
#include "settings.h"
#include "transcript.h"
#include "util.h"

static int failures;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    double ms;
    int    on;
} seg;

/* Text -> on/off segments at the given speed, with an optional timing
 * jitter (fraction of a dit) to imitate a human with a straight key. */
static int text_to_segments(const char *text, double wpm, double jitter, seg *out, int cap)
{
    double dit = 1200.0 / wpm;
    char tok[16];
    int n, count = 0, first = 1;
#define ADD(on_, ms_)                                                                    \
    do {                                                                                 \
        double j = jitter > 0 ? (((double)rng_u32() / 4294967295.0) * 2 - 1) * jitter * dit : 0; \
        if (count < cap) {                                                               \
            out[count].on = (on_);                                                       \
            out[count].ms = (ms_) + j;                                                   \
            count++;                                                                     \
        }                                                                                \
    } while (0)
    ADD(0, 300);
    while ((n = morse_next_token(text, tok, sizeof tok)) > 0) {
        const morse_entry *e;
        const char *c;
        text += n;
        if (tok[0] == ' ') {
            ADD(0, 4 * dit);
            continue;
        }
        e = morse_lookup(tok);
        if (!e)
            continue;
        if (!first)
            ADD(0, 2 * dit);
        first = 0;
        for (c = e->code; *c; c++) {
            ADD(1, *c == '.' ? dit : 3 * dit);
            ADD(0, dit);
        }
    }
    ADD(0, 1500);
#undef ADD
    return count;
}

static double gauss(void)
{
    double u1 = ((double)rng_u32() + 1.0) / 4294967297.0;
    double u2 = ((double)rng_u32() + 1.0) / 4294967297.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * CW_PI * u2);
}

/* Renders CW audio at 8 kHz: tone amplitude 1, noise with given sigma. */
static float *synth(const seg *s, int ns, double freq, double sigma, int *nsamples)
{
    double total = 0, phase = 0, env = 0;
    double step = 1.0 / (0.005 * DSP_RATE); /* 5 ms raised cosine edges */
    int i, k = 0, len;
    float *buf;
    for (i = 0; i < ns; i++)
        total += s[i].ms;
    len = (int)(total * DSP_RATE / 1000.0) + 1;
    buf = (float *)malloc(sizeof(float) * (size_t)len);
    for (i = 0; i < ns; i++) {
        int m = (int)(s[i].ms * DSP_RATE / 1000.0 + 0.5), j;
        for (j = 0; j < m && k < len; j++, k++) {
            double g;
            env += s[i].on ? step : -step;
            env = clampd(env, 0, 1);
            g = 0.5 - 0.5 * cos(CW_PI * env);
            buf[k] = (float)(sin(phase) * g + sigma * gauss());
            phase += 2 * CW_PI * freq / DSP_RATE;
        }
    }
    *nsamples = k;
    return buf;
}

static void collect(cw_decoder *dec, char *out, size_t cap)
{
    dec_token t;
    while (dec_pop(dec, &t)) {
        size_t len = strlen(out);
        if (len + strlen(t.text) + 1 < cap)
            strcat(out, t.text);
    }
}

/* Runs audio through the receiver and returns the decoded text. */
static void receive(const float *x, int n, float tune, float bw, int sens, int afc, int wpm, char *out,
                    size_t cap)
{
    static rx_dsp dsp;
    cw_decoder dec;
    dsp_event ev[DSP_MAX_EVENTS];
    int i, done = 0;
    dsp_init(&dsp, tune, bw, sens);
    dsp.afc = afc;
    dec_init(&dec, wpm);
    out[0] = 0;
    while (done < n) {
        int chunk = mini(80, n - done), ne;
        dsp_process(&dsp, x + done, chunk);
        done += chunk;
        ne = dsp_take_events(&dsp, ev, DSP_MAX_EVENTS);
        for (i = 0; i < ne; i++)
            dec_key(&dec, ev[i].on, ev[i].t_ms);
        dec_update(&dec, dsp_time_ms(&dsp));
        /* Same speed hint the application feeds back (bounded). */
        dsp.dit_ms = (float)clampd(dec_unit_ms(&dec), 24.0, 240.0);
        collect(&dec, out, cap);
    }
}

/* Character error rate via Levenshtein distance. */
static double cer(const char *ref, const char *hyp)
{
    int n = (int)strlen(ref), m = (int)strlen(hyp), i, j;
    int *prev = (int *)malloc(sizeof(int) * (size_t)(m + 1));
    int *cur = (int *)malloc(sizeof(int) * (size_t)(m + 1));
    double r;
    for (j = 0; j <= m; j++)
        prev[j] = j;
    for (i = 1; i <= n; i++) {
        cur[0] = i;
        for (j = 1; j <= m; j++) {
            int sub = prev[j - 1] + (ref[i - 1] != hyp[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = mini(sub, mini(del, ins));
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(m + 1));
    }
    r = n ? (double)prev[m] / n : 0;
    free(prev);
    free(cur);
    return r;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static void test_input_parser(void)
{
#ifndef _WIN32
    static const struct {
        int         kitty; /* parsed with the kitty protocol active */
        const char *bytes;
        int         key, mods, type;
        uint32_t    text;
    } cases[] = {
        /* kitty keyboard protocol */
        {1, "\x1b[32u", KEY_SPACE, 0, EV_PRESS, ' '},
        {1, "\x1b[32;1:2u", KEY_SPACE, 0, EV_REPEAT, ' '},
        {1, "\x1b[32;1:3u", KEY_SPACE, 0, EV_RELEASE, 0},
        {1, "\x1b[D", KEY_LEFT, 0, EV_PRESS, 0},
        {1, "\x1b[1;1:3D", KEY_LEFT, 0, EV_RELEASE, 0},
        {1, "\x1b[1;5:3D", KEY_LEFT, MOD_CTRL, EV_RELEASE, 0},
        {1, "\x1b[1;1:3C", KEY_RIGHT, 0, EV_RELEASE, 0},
        {1, "\x1b[120;5u", 'x', MOD_CTRL, EV_PRESS, 0},
        {1, "\x1b[97;2;65u", 'a', MOD_SHIFT, EV_PRESS, 'A'},
        {1, "\x1b[61:43;2u", '=', MOD_SHIFT, EV_PRESS, '+'},
        {1, "\x1b[13u", KEY_ENTER, 0, EV_PRESS, 0},
        {1, "\x1b[27u", KEY_ESC, 0, EV_PRESS, 0},
        {1, "\x1b[127u", KEY_BACKSPACE, 0, EV_PRESS, 0},
        {1, "\x1b[5~", KEY_PGUP, 0, EV_PRESS, 0},
        {1, "\x1b[O", KEY_FOCUS_OUT, 0, EV_PRESS, 0},
        /* plain bytes in kitty mode are pasted text: they must never key */
        {1, " ", KEY_SPACE, MOD_TEXT, EV_PRESS, ' '},
        /* legacy */
        {0, "\x18", 'x', MOD_CTRL, EV_PRESS, 0},
        {0, "\x1b[C", KEY_RIGHT, 0, EV_PRESS, 0},
        {0, "\x1bOA", KEY_UP, 0, EV_PRESS, 0},
        {0, "\x1b[1;5D", KEY_LEFT, MOD_CTRL, EV_PRESS, 0},
        {0, "Q", 'q', MOD_SHIFT, EV_PRESS, 'Q'},
        {0, "\x7f", KEY_BACKSPACE, 0, EV_PRESS, 0},
        {0, "\r", KEY_ENTER, 0, EV_PRESS, 0},
        {0, "\x1b", KEY_ESC, 0, EV_PRESS, 0},
        {0, "\xc3\xa8", 0xE8, 0, EV_PRESS, 0xE8},
    };
    kbd_info kitty = {KBD_KITTY, 31, 1, 1, 0}, legacy = {KBD_LEGACY, 0, 0, 0, 0};
    key_event ev[8];
    int i, n;
    printf("input parser\n");
    for (i = 0; i < ARRAY_LEN(cases); i++) {
        input_test_set_info(cases[i].kitty ? &kitty : &legacy);
        n = input_test_parse((const unsigned char *)cases[i].bytes, (int)strlen(cases[i].bytes), ev, 8);
        CHECK(n == 1, "case %d: %d events", i, n);
        if (n == 1) {
            CHECK(ev[0].key == cases[i].key && ev[0].mods == cases[i].mods && ev[0].type == cases[i].type &&
                      ev[0].text == cases[i].text,
                  "case %d: key %d mods %d type %d text %u", i, ev[0].key, ev[0].mods, ev[0].type,
                  (unsigned)ev[0].text);
        }
    }
    input_test_set_info(&kitty);
    /* Capability replies are consumed, not reported as keys. */
    {
        const char *reply = "\x1b[?31u\x1b[?62;22c";
        n = input_test_parse((const unsigned char *)reply, (int)strlen(reply), ev, 8);
    }
    CHECK(n == 0 && input_info()->kitty_flags == 31, "capability replies");
    /* A burst of events in one read. */
    {
        const char *burst = "\x1b[32u\x1b[1;1:3D\x1b[32;1:3u";
        n = input_test_parse((const unsigned char *)burst, (int)strlen(burst), ev, 8);
    }
    CHECK(n == 3 && ev[0].key == KEY_SPACE && ev[1].key == KEY_LEFT && ev[2].type == EV_RELEASE, "burst");
    /* A read may contain more events than the caller's output buffer.
     * Preserve both the queued events and the bytes left to parse. */
    {
        char burst[512];
        int total = 0;
        memset(burst, 'a', 300);
        strcpy(burst + 300, "\x1b[32;1:3u");
        n = input_test_parse((const unsigned char *)burst, (int)strlen(burst), ev, 8);
        while (n > 0 && total < 301) {
            for (i = 0; i < n; i++, total++)
                CHECK(total < 300 ? ev[i].key == 'a' :
                          ev[i].key == KEY_SPACE && ev[i].type == EV_RELEASE,
                      "event %d of a long burst", total);
            if (total < 301)
                n = input_poll(ev, 8, 0);
        }
        CHECK(total == 301, "long burst lost events: %d/301", total);
    }
    /* A malformed multibyte prefix must not consume the following key. */
    {
        const unsigned char bytes[] = {0xC2, 'x'};
        n = input_test_parse(bytes, sizeof bytes, ev, 8);
        CHECK(n == 2 && ev[0].text == 0xFFFD && ev[1].key == 'x', "invalid UTF-8 swallowed a key");
    }
#endif
}

static void test_mouse_parser(void)
{
#ifndef _WIN32
    kbd_info mouse = {KBD_KITTY, 31, 1, 1, 1}, nomouse = {KBD_KITTY, 31, 1, 1, 0};
    key_event ev[8];
    int n;
    printf("input parser: mouse buttons (USB pad)\n");
    input_test_set_info(&mouse);
    /* SGR: left press, right press, left release, right release (a squeeze). */
    {
        const char *sgr = "\x1b[<0;10;5M\x1b[<2;10;5M\x1b[<0;10;5m\x1b[<2;10;5m";
        n = input_test_parse((const unsigned char *)sgr, (int)strlen(sgr), ev, 8);
    }
    CHECK(n == 4 && ev[0].key == KEY_MOUSE_LEFT && ev[0].type == EV_PRESS && ev[1].key == KEY_MOUSE_RIGHT &&
              ev[1].type == EV_PRESS && ev[2].key == KEY_MOUSE_LEFT && ev[2].type == EV_RELEASE &&
              ev[3].key == KEY_MOUSE_RIGHT && ev[3].type == EV_RELEASE && ev[0].mods == 0 && ev[0].text == 0,
          "SGR squeeze: %d events", n);
    /* Modifiers are dropped; wheel, motion and the middle button ignored. */
    {
        const char *other = "\x1b[<16;1;1M\x1b[<64;1;1M\x1b[<65;1;1M\x1b[<1;1;1M\x1b[<35;3;3M";
        n = input_test_parse((const unsigned char *)other, (int)strlen(other), ev, 8);
    }
    CHECK(n == 1 && ev[0].key == KEY_MOUSE_LEFT && ev[0].type == EV_PRESS, "SGR extras: %d events", n);
    /* X10 (no SGR support): the release does not say which button. */
    {
        const char *x10 = "\x1b[M !!\x1b[M\"!!\x1b[M#!!";
        n = input_test_parse((const unsigned char *)x10, (int)strlen(x10), ev, 8);
    }
    CHECK(n == 4 && ev[0].key == KEY_MOUSE_LEFT && ev[0].type == EV_PRESS && ev[1].key == KEY_MOUSE_RIGHT &&
              ev[1].type == EV_PRESS && ev[2].type == EV_RELEASE && ev[3].type == EV_RELEASE &&
              ev[2].key != ev[3].key,
          "X10 squeeze: %d events", n);
    /* A report split across reads waits for the rest. */
    {
        const char *part = "\x1b[M ";
        n = input_test_parse((const unsigned char *)part, (int)strlen(part), ev, 8);
        CHECK(n == 0, "partial X10 report produced %d event(s)", n);
    }
    /* Mouse reporting off: reports never become keys. */
    input_test_set_info(&nomouse);
    {
        const char *sgr = "\x1b[<0;10;5M\x1b[<0;10;5m";
        n = input_test_parse((const unsigned char *)sgr, (int)strlen(sgr), ev, 8);
    }
    CHECK(n == 0, "mouse report with reporting off: %d events", n);
#endif
}

static void test_text_helpers(void)
{
    static const char *const invalid[] = {"\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80",
                                          "\xE0\x80\x80", "\xF0\x80\x80\x80", "\xF5\x80\x80\x80"};
    char out[32];
    uint32_t cp;
    int i, n;
    printf("text helpers: malformed Unicode and prosigns\n");
    for (i = 0; i < ARRAY_LEN(invalid); i++)
        CHECK(utf8_decode(invalid[i], &cp) == 1 && cp == 0xFFFD, "invalid UTF-8 case %d", i);
    n = utf8_encode(0xD800, out);
    out[n] = 0;
    CHECK(n == 3 && utf8_decode(out, &cp) == 3 && cp == 0xFFFD, "encoded a surrogate");
    n = utf8_encode(0x110000, out);
    out[n] = 0;
    CHECK(n == 3 && utf8_decode(out, &cp) == 3 && cp == 0xFFFD, "encoded an invalid scalar");
    CHECK(utf8_decode("\xF0\x9F\x98\x80", &cp) == 4 && cp == 0x1F600, "valid UTF-8");
    morse_normalize("<bad> <", out, sizeof out);
    CHECK(strcmp(out, "<BAD> <") == 0, "malformed prosign changed to '%s'", out);
}

static void test_transcript(void)
{
    static transcript t;
    char text[TR_LINE_CAP * 3 + 1];
    int i, total = 0;
    printf("transcript: long appends and UTF-8 deletion\n");
    transcript_clear(&t);
    memset(text, 'A', sizeof text - 1);
    text[sizeof text - 1] = 0;
    transcript_append(&t, DIR_RX, text, STY_UNKNOWN);
    for (i = 0; i < transcript_count(&t); i++) {
        const tr_line *l = transcript_line(&t, i);
        int j;
        CHECK(l->len < TR_LINE_CAP && strlen(l->text) == (size_t)l->len, "line %d overflow", i);
        for (j = 0; j < l->len; j++)
            CHECK(l->text[j] == 'A' && l->style[j] == STY_UNKNOWN, "line %d lost text/style", i);
        total += l->len;
    }
    CHECK(total == (int)strlen(text), "long append lost text: %d", total);
    transcript_clear(&t);
    transcript_append(&t, DIR_TX, "A\xC3\xA8\xF0\x9F\x98\x80", STY_NORMAL);
    transcript_backspace(&t);
    CHECK(strcmp(transcript_line(&t, 0)->text, "A\xC3\xA8") == 0, "partial four-byte deletion");
    transcript_backspace(&t);
    CHECK(strcmp(transcript_line(&t, 0)->text, "A") == 0, "partial two-byte deletion");
    transcript_append(&t, DIR_TX, "<AR>", STY_PROSIGN);
    transcript_backspace(&t);
    CHECK(strcmp(transcript_line(&t, 0)->text, "A") == 0, "prosign deletion");
    transcript_clear(&t);
    memset(text, 'A', TR_LINE_CAP - 2);
    strcpy(text + TR_LINE_CAP - 2, "\xC3\xA8");
    transcript_append(&t, DIR_RX, text, STY_NORMAL);
    CHECK(transcript_count(&t) == 2 && transcript_line(&t, 0)->len == TR_LINE_CAP - 2 &&
              strcmp(transcript_line(&t, 1)->text, "\xC3\xA8") == 0, "wrap split a UTF-8 character");
}

static void test_settings(void)
{
    settings_t s;
    char path[128];
    FILE *f;
    printf("settings: invalid numbers and extreme bandwidths\n");
    settings_defaults(&s);
    s.rx_bw = INT_MIN;
    settings_clamp(&s);
    CHECK(s.rx_bw == 50, "INT_MIN bandwidth became %d", s.rx_bw);
    s.rx_bw = INT_MAX;
    settings_clamp(&s);
    CHECK(s.rx_bw == 500, "INT_MAX bandwidth became %d", s.rx_bw);
    settings_defaults(&s);
    snprintf(path, sizeof path, "cwtty-test-settings-%u.conf", (unsigned)plat_entropy());
    f = fopen(path, "wx");
    CHECK(f != NULL, "could not create settings fixture");
    if (!f)
        return;
    fprintf(f, "wpm=garbage\nvolume=999999999999999999999999\nrx_freq=700junk\ntone_hz=750\nrx_bw=%d\n", INT_MIN);
    fclose(f);
    CHECK(settings_load(&s, path) == 0, "settings load failed");
    CHECK(s.wpm == 20 && s.volume == 60 && s.rx_freq == 600 && s.tone_hz == 750 && s.rx_bw == 50,
          "invalid settings changed valid defaults");
    s.train_strict = 1;
    s.key_input = INPUT_PAD;
    s.keyer_mode = KEYER_STRAIGHT;
    CHECK(settings_save(&s, path) == 0, "could not save strict training setting");
    settings_defaults(&s);
    CHECK(s.train_strict == 0, "relaxed training should be the default");
    CHECK(s.key_input == INPUT_KEYBOARD, "the keyboard should be the default input");
    CHECK(settings_load(&s, path) == 0 && s.train_strict == 1, "strict training setting not persisted");
    CHECK(s.key_input == INPUT_PAD && s.keyer_mode == KEYER_STRAIGHT, "USB pad settings not persisted");
    s.key_input = 7;
    settings_clamp(&s);
    CHECK(s.key_input == INPUT_COUNT - 1, "key input out of range: %d", s.key_input);
    remove(path);
}

static void test_pad_keying(void)
{
    static app_t a;
    key_event space = {KEY_SPACE, 0, EV_PRESS, ' '}, space_up = {KEY_SPACE, 0, EV_RELEASE, 0};
    key_event left = {KEY_MOUSE_LEFT, 0, EV_PRESS, 0}, left_up = {KEY_MOUSE_LEFT, 0, EV_RELEASE, 0};
    key_event right = {KEY_MOUSE_RIGHT, 0, EV_PRESS, 0}, right_up = {KEY_MOUSE_RIGHT, 0, EV_RELEASE, 0};
    printf("keying: keyboard or USB pad\n");
    memset(&a, 0, sizeof a);
    settings_defaults(&a.cfg);
    a.mode = MODE_TX;
    a.kbd = input_info();

    app_handle_key(&a, &left);
    CHECK(!a.key_left, "the pad keyed while the keyboard is selected");
    app_handle_key(&a, &space);
    CHECK(a.key_space, "Space did not key with the keyboard selected");
    app_handle_key(&a, &space_up);

    a.cfg.key_input = INPUT_PAD;
    app_handle_key(&a, &space);
    CHECK(!a.key_space, "Space keyed while the USB pad is selected");
    app_handle_key(&a, &left);
    app_handle_key(&a, &right);
    CHECK(a.key_left && a.key_right, "the pad levers did not key");
    app_handle_key(&a, &left_up);
    app_handle_key(&a, &right_up);
    CHECK(!a.key_left && !a.key_right, "the pad levers stayed down");
    a.cfg.swap_paddles = 1;
    app_handle_key(&a, &left);
    CHECK(a.key_left, "the swapped left lever did not key");
    app_handle_key(&a, &left_up);
    /* Releases always count, whatever device is selected. */
    a.cfg.key_input = INPUT_KEYBOARD;
    a.cfg.swap_paddles = 0;
    a.key_right = 1;
    app_handle_key(&a, &right_up);
    CHECK(!a.key_right, "a pad release was lost after switching to the keyboard");
    audio_release_keys();
}

static void training_init(app_t *a, const char *target, int strict)
{
    memset(a, 0, sizeof *a);
    settings_defaults(&a->cfg);
    a->cfg.train_dir = TRAIN_SEND;
    a->cfg.train_content = TRAIN_WORDS;
    a->cfg.train_strict = strict;
    a->mode = MODE_TRAIN;
    a->kbd = input_info();
    a->tr.state = TS_SEND;
    str_copy(a->tr.target, sizeof a->tr.target, target);
    dec_init(&a->txdec, 20);
    dec_set_speed(&a->txdec, 20, 1);
}

static void training_collect(app_t *a, double now)
{
    dec_token tok;
    a->now = now;
    dec_update(&a->txdec, now);
    while (dec_pop(&a->txdec, &tok))
        train_on_sent(a, &tok);
}

static void training_character(app_t *a, const char *letter, double *now, double gap)
{
    const morse_entry *e = morse_lookup(letter);
    const char *p;
    for (p = e->code; *p; p++) {
        train_operator_pressed(a);
        dec_key(&a->txdec, 1, *now);
        *now += *p == '.' ? 60.0 : 180.0;
        dec_key(&a->txdec, 0, *now);
        *now += p[1] ? 60.0 : gap;
        training_collect(a, *now);
    }
}

static void test_send_training(void)
{
    static app_t a;
    double now;
    key_event enter = {KEY_ENTER, 0, EV_PRESS, 0};
    key_event backspace = {KEY_BACKSPACE, 0, EV_PRESS, 0};
    key_event right = {KEY_RIGHT, 0, EV_PRESS, 0};
    printf("send training: complete words, strict and relaxed spacing\n");
    training_init(&a, "THE", 0);
    now = 0;
    training_character(&a, "T", &now, 5000);
    CHECK(a.tr.state == TS_SEND && a.tr.total == 0 && strcmp(a.tr.target, "THE") == 0 &&
              strcmp(a.tr.sent, "T") == 0, "pause ended an incomplete relaxed word");
    training_character(&a, "H", &now, 10000);
    CHECK(a.tr.state == TS_SEND && strcmp(a.tr.target, "THE") == 0, "second letter changed the word");
    training_character(&a, "E", &now, 180);
    CHECK(a.tr.state == TS_SEND_RESULT && a.tr.ok && a.tr.correct == 1, "spaced letters should pass in relaxed mode");
    training_collect(&a, now + 10000);
    CHECK(a.tr.state == TS_SEND_RESULT && strcmp(a.tr.target, "THE") == 0, "result advanced without Enter");
    train_operator_pressed(&a);
    CHECK(a.tr.state == TS_SEND && a.tr.sent[0] == 0 && strcmp(a.tr.target, "THE") == 0,
          "keying after a result should retry the same word");

    training_init(&a, "THE", 1);
    now = 0;
    training_character(&a, "T", &now, 5000);
    CHECK(a.tr.state == TS_SEND && a.tr.total == 0 && strcmp(a.tr.target, "THE") == 0,
          "strict spacing error prematurely ended the word");
    training_character(&a, "H", &now, 180);
    training_character(&a, "E", &now, 180);
    CHECK(a.tr.state == TS_SEND_RESULT && !a.tr.ok && strcmp(a.tr.sent, "T HE") == 0,
          "a word with an internal word gap passed strict mode");
    train_key(&a, &backspace);
    CHECK(a.tr.state == TS_SEND && a.tr.sent[0] == 0 && strcmp(a.tr.target, "THE") == 0, "retry changed target");
    now = 0;
    training_character(&a, "T", &now, 180);
    training_character(&a, "H", &now, 180);
    training_character(&a, "E", &now, 180);
    CHECK(a.tr.state == TS_SEND_RESULT && a.tr.ok, "a complete correctly spaced word failed strict mode");
    train_key(&a, &enter);
    CHECK(a.tr.state == TS_SEND && a.tr.sent[0] == 0, "Enter did not start the next item");

    training_init(&a, "THE", 0);
    now = 0;
    training_character(&a, "T", &now, 5000);
    training_character(&a, "H", &now, 5000);
    training_character(&a, "A", &now, 180);
    CHECK(a.tr.state == TS_SEND_RESULT && !a.tr.ok, "relaxed mode accepted a wrong letter");
    training_init(&a, "THE", 1);
    now = 0;
    training_character(&a, "T", &now, 5000);
    train_key(&a, &enter);
    CHECK(a.tr.state == TS_SEND_RESULT && !a.tr.ok, "an incomplete word passed strict mode");
    a.tr.state = TS_SETUP;
    a.tr.sel = 3; /* Direction, material, hint, spacing, start. */
    train_key(&a, &right);
    CHECK(a.cfg.train_strict == 0, "spacing choice did not toggle");
}

static void test_copy_training(void)
{
    static app_t a;
    key_event enter = {KEY_ENTER, 0, EV_PRESS, 0};
    key_event space = {KEY_SPACE, 0, EV_PRESS, ' '};
    key_event letter = {'x', 0, EV_PRESS, 'x'};
    key_event replay = {'r', MOD_CTRL, EV_PRESS, 0};
    int ok;
    printf("copy training: the result stays until Enter\n");
    for (ok = 0; ok <= 1; ok++) {
        training_init(&a, "THE", 0);
        a.cfg.train_dir = TRAIN_COPY;
        a.tr.state = TS_LISTEN;
        str_copy(a.tr.typed, sizeof a.tr.typed, ok ? "THE" : "TEH");
        a.tr.ntyped = 3;
        train_key(&a, &enter);
        CHECK(a.tr.state == TS_RESULT && a.tr.ok == ok, "Enter did not check the copy");
        a.now = 60000;
        train_key(&a, &replay);
        train_key(&a, &space);
        train_key(&a, &letter);
        CHECK(a.tr.state == TS_RESULT && strcmp(a.tr.target, "THE") == 0,
              "%s result went away before Enter", ok ? "correct" : "wrong");
        scr_init(1);
        scr_resize(80, 24);
        app_render(&a);
        {
            char row[81];
            int x, y, hint = 0;
            for (y = 0; y < 24 && !hint; y++) {
                for (x = 0; x < 80; x++) {
                    uint32_t cp = scr_cell(x, y)->ch;
                    row[x] = cp < 128 ? (char)cp : ' ';
                }
                row[80] = 0;
                hint = strstr(row, "Enter next") && strstr(row, "C-r") != NULL;
            }
            CHECK(hint, "the result does not say how to continue");
        }
        scr_shutdown();
        train_key(&a, &enter);
        CHECK(a.tr.state == TS_LISTEN && a.tr.typed[0] == 0, "Enter did not start the next item");
    }
}

static void test_training_table_footer(void)
{
    static app_t a;
    char footer[161];
    static const int widths[] = {64, 100, 160};
    int dir, x, wi;
    key_event escape = {KEY_ESC, 0, EV_PRESS, 0};
    printf("training: Morse table owns the Escape hint and action\n");
    scr_init(1);
    for (wi = 0; wi < ARRAY_LEN(widths); wi++) {
        int width = widths[wi];
        scr_resize(width, 40);
        for (dir = TRAIN_COPY; dir <= TRAIN_SEND; dir++) {
            training_init(&a, "THE", 0);
            a.cfg.train_dir = dir;
            a.tr.state = dir == TRAIN_COPY ? TS_LISTEN : TS_SEND;
            app_open_modal(&a, MODAL_TABLE);
            app_render(&a);
            for (x = 0; x < width; x++) {
                uint32_t cp = scr_cell(x, 39)->ch;
                footer[x] = cp < 128 ? (char)cp : ' ';
            }
            footer[width] = 0;
            {
                const char *esc = strstr(footer, "Esc");
                CHECK(esc && !strstr(esc + 3, "Esc") && strstr(footer, "Close table") && !strstr(footer, "Setup"),
                      "conflicting Escape footer: %s", footer);
            }
            app_handle_key(&a, &escape);
            CHECK(!app_has_modal(&a, MODAL_TABLE) && a.tr.state == (dir == TRAIN_COPY ? TS_LISTEN : TS_SEND),
                  "Escape changed training instead of just closing the table");
        }
    }
    scr_shutdown();
}

static void test_morse_table(void)
{
    int i, j;
    char norm[64];
    printf("morse table\n");
    for (i = 0; i < MORSE_TABLE_COUNT; i++) {
        const morse_entry *e = &MORSE_TABLE[i];
        const morse_entry *d = morse_decode(e->code);
        CHECK(d != NULL, "code %s of %s does not decode", e->code, e->text);
        if (!(e->flags & MF_NODECODE))
            CHECK(d == e, "code %s decodes to %s, expected %s", e->code, d ? d->text : "-", e->text);
        for (j = i + 1; j < MORSE_TABLE_COUNT; j++) {
            if (strcmp(e->code, MORSE_TABLE[j].code) == 0)
                CHECK((e->flags | MORSE_TABLE[j].flags) & MF_NODECODE, "ambiguous code %s", e->code);
        }
    }
    morse_normalize("  cq <ar>  de + ", norm, sizeof norm);
    CHECK(strcmp(norm, "CQ AR DE AR") == 0, "normalize gave '%s'", norm);
}

static void test_decoder_exact(void)
{
    static const char *texts[] = {"PARIS PARIS", "CQ CQ DE IK0ABC K", "73 TU <SK>", "THE QUICK BROWN FOX 1234567890 ?/=,."};
    static const int speeds[] = {5, 12, 20, 30, 45};
    seg s[2048];
    int ti, si;
    printf("decoder: exact timing\n");
    for (ti = 0; ti < ARRAY_LEN(texts); ti++) {
        for (si = 0; si < ARRAY_LEN(speeds); si++) {
            cw_decoder dec;
            char out[256] = "", want[256];
            double t = 0;
            int i, ns = text_to_segments(texts[ti], speeds[si], 0, s, ARRAY_LEN(s));
            dec_init(&dec, 20); /* deliberately wrong initial speed */
            for (i = 0; i < ns; i++) {
                double end = t + s[i].ms, tt;
                dec_key(&dec, s[i].on, t);
                for (tt = t; tt < end; tt += 5)
                    dec_update(&dec, tt);
                t = end;
                collect(&dec, out, sizeof out);
            }
            dec_update(&dec, t + 5000);
            collect(&dec, out, sizeof out);
            trim(out);
            str_copy(want, sizeof want, texts[ti]);
            /* The decoder starts at 20 WPM: far from it, the first word is
             * used to adapt. Then everything must be exact. */
            if (speeds[si] != 20) {
                const char *w = strchr(want, ' ');
                size_t lo = strlen(out), lw = w ? strlen(w) : 0;
                /* (the word space before it may be swallowed too) */
                CHECK(w && lo + 1 >= lw && strcmp(out + lo - (lw - 1), w + 1) == 0, "%d WPM '%s' -> '%s'",
                      speeds[si], want, out);
            } else {
                CHECK(cer(want, out) == 0.0, "%d WPM '%s' -> '%s'", speeds[si], want, out);
            }
        }
    }
}

static void test_decoder_jitter(void)
{
    seg s[2048];
    cw_decoder dec;
    char out[256] = "";
    double t = 0;
    int i, ns;
    printf("decoder: human timing (25%% jitter)\n");
    rng_seed(1234);
    ns = text_to_segments("CQ CQ DE IW0XYZ IW0XYZ PSE K", 15, 0.25, s, ARRAY_LEN(s));
    dec_init(&dec, 15);
    for (i = 0; i < ns; i++) {
        double end = t + s[i].ms, tt;
        dec_key(&dec, s[i].on, t);
        for (tt = t; tt < end; tt += 5)
            dec_update(&dec, tt);
        t = end;
    }
    dec_update(&dec, t + 5000);
    collect(&dec, out, sizeof out);
    trim(out);
    CHECK(cer("CQ CQ DE IW0XYZ IW0XYZ PSE K", out) <= 0.05, "-> '%s'", out);
}

static void test_decoder_resync(void)
{
    cw_decoder dec;
    dec_token t;
    int n = 0;
    printf("decoder: cleared in the middle of a mark (break-in)\n");
    dec_init(&dec, 20);
    dec_key(&dec, 1, 1000.0); /* a mark starts... */
    dec_clear(&dec);          /* ...and the receiver is muted */
    dec_key(&dec, 0, 1030.0); /* the cut mark ends */
    dec_update(&dec, 9000.0);
    while (dec_pop(&dec, &t))
        n++;
    CHECK(n == 0, "a cut mark produced %d token(s)", n);
}

static void test_keyer(void)
{
    keyer_t k;
    int i, on_run = 0, prev = 0, marks[16], nm = 0;
    printf("keyer\n");
    /* Iambic B, squeeze for a while then release: alternating elements. */
    keyer_init(&k, KEYER_IAMBIC_B, 100);
    for (i = 0; i < 3000; i++) {
        int squeeze = i < 650;
        int on = keyer_tick(&k, squeeze, squeeze, i == 0, i == 0);
        if (on) {
            on_run++;
        } else if (prev) {
            if (nm < 16)
                marks[nm++] = on_run;
            on_run = 0;
        }
        prev = on;
    }
    /* dit(100) gap dah(300) gap dit gap -> released at 650 during the
     * second dit... B adds one more element after release. */
    CHECK(nm >= 3, "expected at least 3 elements, got %d", nm);
    if (nm >= 3) {
        CHECK(marks[0] == 100 && marks[1] == 300 && marks[2] == 100, "pattern %d %d %d", marks[0], marks[1], marks[2]);
    }
    CHECK(nm == 4 && marks[3] == 300, "iambic B should add a final dah (got %d elements)", nm);

    /* Iambic A, same input: no extra element. */
    keyer_init(&k, KEYER_IAMBIC_A, 100);
    nm = 0;
    prev = 0;
    on_run = 0;
    for (i = 0; i < 3000; i++) {
        int squeeze = i < 650;
        int on = keyer_tick(&k, squeeze, squeeze, i == 0, i == 0);
        if (on) {
            on_run++;
        } else if (prev) {
            if (nm < 16)
                marks[nm++] = on_run;
            on_run = 0;
        }
        prev = on;
    }
    CHECK(nm == 3, "iambic A should stop after the current element (got %d)", nm);

    /* Straight key: the paddles are manual (keyed outside the keyer). */
    keyer_init(&k, KEYER_STRAIGHT, 100);
    nm = 0;
    for (i = 0; i < 1000; i++)
        nm += keyer_tick(&k, i < 500, i >= 200 && i < 700, i == 0, i == 200);
    CHECK(nm == 0 && !keyer_busy(&k), "the straight key mode ran the keyer for %d samples", nm);
}

static void test_receiver(void)
{
    /*
     * Tone amplitude is 1, noise is white with the given sigma at 8 kHz:
     * SNR in a 100 Hz bandwidth = 20 / sigma^2 (sigma 1.0 -> 13 dB,
     * 1.5 -> 9.5 dB, 2.2 -> 6.2 dB; subtract 14 dB for SNR in 2.5 kHz).
     */
    static const struct {
        const char *text;
        double      wpm;
        double      sigma;
        float       tune, bw;
        double      freq;
        int         sens, afc;
        double      max_cer; /* mean over 4 noise realisations */
    } cases[] = {
        {"CQ CQ DE IK0ABC IK0ABC K", 20, 0.0, 600, 100, 600, 6, 0, 0.0},
        {"CQ CQ DE IK0ABC IK0ABC K", 20, 0.3, 600, 100, 600, 6, 0, 0.0},
        {"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG", 25, 1.0, 700, 100, 700, 6, 0, 0.02},
        {"UR RST 599 5NN ES NAME GIO HW CPY", 20, 1.5, 600, 100, 600, 6, 0, 0.05},
        {"UR RST 599 5NN ES NAME GIO HW CPY", 30, 1.2, 650, 150, 650, 6, 0, 0.10},
        {"TEST DE W1AW W1AW TEST", 16, 2.2, 600, 50, 600, 7, 0, 0.45},
        {"QRL? QRL? DE IZ0ABC", 18, 0.5, 600, 100, 680, 6, 1, 0.15},
        {"PARIS PARIS PARIS", 40, 0.5, 800, 200, 800, 6, 0, 0.0},
    };
    int i;
    printf("receiver: audio -> dsp -> decoder\n");
    for (i = 0; i < ARRAY_LEN(cases); i++) {
        seg s[4096];
        char out[512];
        int ns, nsamp, rep;
        float *x;
        double e = 0;
        ns = text_to_segments(cases[i].text, cases[i].wpm, 0.0, s, ARRAY_LEN(s));
        for (rep = 0; rep < 4; rep++) {
            rng_seed(1000 + (uint32_t)(i * 10 + rep));
            x = synth(s, ns, cases[i].freq, cases[i].sigma, &nsamp);
            receive(x, nsamp, cases[i].tune, cases[i].bw, cases[i].sens, cases[i].afc, (int)cases[i].wpm,
                    out, sizeof out);
            trim(out);
            e += cer(cases[i].text, out) / 4;
            free(x);
        }
        printf("  %2.0f WPM, sigma %.1f, %3.0f Hz BW, sens %d%s: CER %5.1f%%  '%s'\n", cases[i].wpm,
               cases[i].sigma, cases[i].bw, cases[i].sens, cases[i].afc ? ", AFC" : "", e * 100, out);
        CHECK(e <= cases[i].max_cer, "CER %.2f too high", e);
    }
}

static void test_receiver_noise_only(void)
{
    seg s[2] = {{0, 0}, {10000, 0}};
    char out[512];
    int nsamp;
    float *x;
    printf("receiver: noise only, no false characters at default sensitivity\n");
    rng_seed(7);
    x = synth(s, 2, 600, 1.0, &nsamp);
    receive(x, nsamp, 600, 100, 6, 0, 20, out, sizeof out);
    trim(out);
    printf("  decoded from 10 s of noise: '%s'\n", out);
    CHECK(strlen(out) <= 3, "too many false characters: '%s'", out);
    free(x);
}

int main(void)
{
    test_input_parser();
    test_mouse_parser();
    test_text_helpers();
    test_transcript();
    test_settings();
    test_pad_keying();
    test_send_training();
    test_copy_training();
    test_training_table_footer();
    test_morse_table();
    test_keyer();
    test_decoder_exact();
    test_decoder_jitter();
    test_decoder_resync();
    test_receiver();
    test_receiver_noise_only();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
