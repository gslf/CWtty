/*
 * page.c - The application core of the web page (UI thread): the settings,
 * the decoders of both directions, the receiver, the transceiver's
 * break-in and the training sessions. It mirrors src/app.c and
 * src/mode_trx.c without the terminal; the page feeds it with what the
 * worklet reports and draws what it exposes.
 */
#include <string.h>

#include "decoder.h"
#include "demo.h"
#include "dsp.h"
#include "keyer.h"
#include "morse.h"
#include "settings.h"
#include "training.h"
#include "util.h"
#include "wordlists.h"

#define EXPORT __attribute__((visibility("default")))

#define RX_CAP 2048
#define OUT_CAP 256
#define RX_RECOVERY_MS 150.0 /* deaf after transmitting: the sidetone echoes */
#define LINE_GAP_MS 4000.0   /* a long silence ends the receive line */
#define WF_FMIN 250.0f       /* zero-beat range, as shown by the waterfall */
#define WF_FMAX 1250.0f

enum { MODE_TX, MODE_RX, MODE_TRX, MODE_TRAIN };
enum { DIR_RX, DIR_TX };
enum { SOURCE_OFF, SOURCE_MIC, SOURCE_DEMO };
enum { CONTACT_DIT, CONTACT_DAH, CONTACT_STRAIGHT };
/* After dec_kind: the receive line ends, the receiver's key goes up or down. */
enum { TOK_LINE = TOK_SPACE + 1, TOK_KEY_UP, TOK_KEY_DOWN };

/* What the decoders and the receiver produced, in order, for the page. */
typedef struct {
    int    dir, kind;
    char   text[8];
    double t_ms; /* key transitions: the receiver clock */
} out_token;

static settings_t cfg, applied;
static int        applied_valid;
static cw_decoder txdec, rxdec;
static rx_dsp     dsp;
static training_t tr;

static int    mode, source, ptt;
static double rx_last_char_ms, demo_at;
static int    rx_line_open;

static out_token out[OUT_CAP], popped;
static int       out_head, out_count;

static float rx[RX_CAP];
static char  buf[1024], scratch[512];

/* ------------------------------------------------------------------------ */
/* Internals                                                                 */
/* ------------------------------------------------------------------------ */

static void apply(void)
{
    settings_clamp(&cfg);
    if (!applied_valid || applied.wpm != cfg.wpm)
        dec_set_speed(&txdec, cfg.wpm, 0);
    if (!applied_valid || applied.rx_wpm != cfg.rx_wpm || applied.rx_lock != cfg.rx_lock)
        dec_set_speed(&rxdec, cfg.rx_wpm, cfg.rx_lock);
    if (!applied_valid || applied.rx_freq != cfg.rx_freq)
        dsp_set_freq(&dsp, (float)cfg.rx_freq);
    if (!applied_valid || applied.rx_bw != cfg.rx_bw)
        dsp_set_bw(&dsp, (float)cfg.rx_bw);
    dsp.sens = cfg.rx_sens;
    dsp.afc = cfg.rx_afc;
    applied = cfg;
    applied_valid = 1;
}

static void emit(int dir, int kind, const char *text, double t_ms)
{
    out_token *t;
    if (out_count == OUT_CAP) {
        out_head = (out_head + 1) % OUT_CAP; /* the page is not reading: drop */
        out_count--;
    }
    t = &out[(out_head + out_count++) % OUT_CAP];
    t->dir = dir;
    t->kind = kind;
    str_copy(t->text, sizeof t->text, text);
    t->t_ms = t_ms;
}

static void ptt_on(void)
{
    dec_token tok;
    ptt = 1;
    /* Close the character the receiver was decoding, then go deaf. */
    dec_update(&rxdec, dsp_time_ms(&dsp) + 1.0e6);
    while (dec_pop(&rxdec, &tok))
        if (tok.kind != TOK_SPACE)
            emit(DIR_RX, tok.kind, tok.text, 0);
    dec_clear(&rxdec);
    dsp_mute(&dsp, 1, 0);
}

static void ptt_off(void)
{
    ptt = 0;
    dsp_mute(&dsp, 0, RX_RECOVERY_MS);
    dec_clear(&rxdec);
}

/* Does what the training asks of the decoder; the page gets the rest. */
static int train_do(int fx)
{
    if (fx & TRAIN_RESEND)
        dec_clear(&txdec);
    return fx & ~TRAIN_RESEND;
}

static int keying_allowed(void)
{
    return mode == MODE_TX || mode == MODE_TRX || (mode == MODE_TRAIN && training_keying(&tr, &cfg));
}

static void receive(int n)
{
    dsp_event ev[DSP_MAX_EVENTS];
    int k, i;
    dsp_process(&dsp, rx, n);
    k = dsp_take_events(&dsp, ev, DSP_MAX_EVENTS);
    for (i = 0; i < k; i++) {
        dec_key(&rxdec, ev[i].on, ev[i].t_ms);
        emit(DIR_RX, ev[i].on ? TOK_KEY_DOWN : TOK_KEY_UP, "", ev[i].t_ms);
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle, settings and modes                                             */
/* ------------------------------------------------------------------------ */

EXPORT void cw_init(unsigned seed)
{
    rng_seed(seed);
    settings_defaults(&cfg);
    dec_init(&txdec, cfg.wpm);
    dec_init(&rxdec, cfg.rx_wpm);
    dsp_init(&dsp, (float)cfg.rx_freq, (float)cfg.rx_bw, cfg.rx_sens);
    demo_init();
    applied_valid = 0;
    apply();
}

/* A buffer for strings from the page. */
EXPORT char *cw_buf(void)
{
    return buf;
}

/* Integer settings by name, -1 for an unknown name. */
EXPORT int cw_get(const char *key)
{
    int *p = settings_field(&cfg, key);
    return p ? *p : -1;
}

/* Returns the value actually stored, after clamping. */
EXPORT int cw_set(const char *key, int value)
{
    int *p = settings_field(&cfg, key);
    if (!p)
        return -1;
    *p = value;
    apply();
    return *p;
}

EXPORT void cw_mode(int m)
{
    if (mode == MODE_TRX && ptt)
        ptt_off();
    /* Entering training (again) shows its setup. */
    if (mode == MODE_TRAIN || m == MODE_TRAIN)
        tr.state = TS_SETUP;
    mode = m;
    dec_clear(&txdec);
    if (mode == MODE_TRX)
        dsp_mute(&dsp, 0, 0);
}

/* What feeds the receiver: nothing, the microphone or the demo band. */
EXPORT void cw_source(int s)
{
    if (s != source && s != SOURCE_OFF) {
        dec_clear(&rxdec);
        rx_last_char_ms = dsp_time_ms(&dsp);
        demo_at = -1;
    }
    source = s;
}

/* The sidetone level, silent in the transceiver if so configured. */
EXPORT int cw_sidetone(void)
{
    return mode == MODE_TRX && !cfg.trx_sidetone ? 0 : cfg.volume;
}

/* ------------------------------------------------------------------------ */
/* Keying and audio                                                          */
/* ------------------------------------------------------------------------ */

EXPORT int cw_keying(void)
{
    return keying_allowed();
}

/*
 * The operator's inputs: 0 left paddle, 1 right paddle, 2 straight key.
 * Returns the contact to key in the worklet (CONTACT_*), or -1 when
 * keying is not accepted. Releases always pass: the transmitter must
 * never be left keyed.
 */
EXPORT int cw_press(int input, int down)
{
    int contact = input == 2 ? CONTACT_STRAIGHT : ((input == 0) ^ cfg.swap_paddles ? CONTACT_DIT : CONTACT_DAH);
    if (!down)
        return contact;
    if (!keying_allowed())
        return -1;
    if (mode == MODE_TRX && !ptt)
        ptt_on();
    else if (mode == MODE_TRAIN)
        train_do(training_pressed(&tr, &cfg));
    return contact;
}

/* A transition of the operator's key, at the worklet's clock. */
EXPORT void cw_tx_event(double t_ms, int on)
{
    dec_key(&txdec, on, t_ms);
}

/* Where the page writes captured audio at 8 kHz for cw_rx(), and how much. */
EXPORT float *cw_rx_buf(void)
{
    return rx;
}

EXPORT int cw_rx_cap(void)
{
    return RX_CAP;
}

EXPORT void cw_rx(int n)
{
    if (source == SOURCE_MIC)
        receive(clampi(n, 0, RX_CAP));
}

/* Generates the demo band up to now_ms of the worklet's clock. */
EXPORT void cw_demo(double now_ms)
{
    int n;
    if (source != SOURCE_DEMO)
        return;
    if (demo_at < 0 || now_ms - demo_at > 500.0)
        demo_at = now_ms; /* first call, or stalled: do not catch up */
    while ((n = (int)clampd((now_ms - demo_at) * DSP_RATE / 1000.0, 0, RX_CAP)) > 0) {
        demo_generate(rx, n);
        demo_at += n * 1000.0 / DSP_RATE;
        receive(n);
    }
}

/*
 * Runs the decoders up to the worklet's clock and the break-in, from the
 * worklet's operator state. Returns the training flags for the page.
 */
EXPORT int cw_update(double clock_ms, int op_busy, double idle_ms)
{
    dec_token tok;
    int fx = 0;

    dec_update(&txdec, clock_ms);
    while (dec_pop(&txdec, &tok)) {
        emit(DIR_TX, tok.kind, tok.text, 0);
        if (mode == MODE_TRAIN)
            fx |= train_do(training_sent(&tr, &cfg, &tok, &txdec));
    }

    if (source != SOURCE_OFF) {
        dec_update(&rxdec, dsp_time_ms(&dsp));
        while (dec_pop(&rxdec, &tok)) {
            emit(DIR_RX, tok.kind, tok.text, 0);
            if (tok.kind != TOK_SPACE) {
                rx_last_char_ms = dsp_time_ms(&dsp);
                rx_line_open = 1;
            }
        }
        /* Speed feedback for the detector, bounded so that noise can
         * never talk it into absurd speeds. */
        dsp.dit_ms = (float)clampd(dec_unit_ms(&rxdec), 24.0, 240.0);
        if (dsp.afc)
            applied.rx_freq = cfg.rx_freq = (int)(dsp.freq + 0.5f);
        /* One transmission per line. */
        if (mode == MODE_RX && rx_line_open && dsp_time_ms(&dsp) - rx_last_char_ms > LINE_GAP_MS) {
            emit(DIR_RX, TOK_LINE, "", 0);
            rx_line_open = 0;
        }
    }

    if (mode == MODE_TRX && ptt && !op_busy && idle_ms >= cfg.breakin_ms)
        ptt_off();
    return fx;
}

/* The next output: its text, or NULL. Then cw_pop_dir/kind/time. */
EXPORT const char *cw_pop(void)
{
    if (out_count == 0)
        return NULL;
    popped = out[out_head];
    out_head = (out_head + 1) % OUT_CAP;
    out_count--;
    return popped.text;
}

EXPORT int cw_pop_dir(void)
{
    return popped.dir;
}

EXPORT int cw_pop_kind(void)
{
    return popped.kind;
}

EXPORT double cw_pop_time(void)
{
    return popped.t_ms;
}

/* The character being keyed (DIR_TX) or received (DIR_RX). */
EXPORT const char *cw_pattern(int dir)
{
    return dec_pattern(dir == DIR_TX ? &txdec : &rxdec);
}

/* What that pattern reads as so far, "" if nothing. */
EXPORT const char *cw_live(int dir)
{
    const morse_entry *e = morse_decode(cw_pattern(dir));
    return e ? e->text : "";
}

EXPORT double cw_wpm(int dir)
{
    return dec_wpm(dir == DIR_TX ? &txdec : &rxdec);
}

EXPORT int cw_ptt(void)
{
    return ptt;
}

/* ------------------------------------------------------------------------ */
/* Receiver                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT double cw_rx_freq(void) { return dsp.freq; }
EXPORT double cw_rx_level(void) { return dsp.env_db; }
EXPORT double cw_rx_noise(void) { return dsp.nf_db; }
EXPORT double cw_rx_peak(void) { return dsp.pk_db; }
EXPORT double cw_rx_threshold(void) { return dsp.thr_db; }
EXPORT double cw_rx_snr(void) { return dsp.snr_db; }
EXPORT int cw_rx_open(void) { return dsp.open; }
EXPORT int cw_rx_key(void) { return dsp.state; }
EXPORT double cw_rx_time(void) { return dsp_time_ms(&dsp); }

/* The loudest envelope (dB) between t0 and t1 of the receiver clock. */
EXPORT double cw_rx_env(double t0, double t1)
{
    float best = -200.0f;
    double t;
    for (t = t0; t < t1; t += 1.0) {
        float e = dsp_env_at(&dsp, t);
        if (e > best)
            best = e;
    }
    return best;
}


/* Waterfall: DSP_BINS levels in dB, "age" rows ago; cw_rx_rows() counts
 * the rows produced so far. */
EXPORT const float *cw_rx_row(int age) { return dsp_wf_row(&dsp, age); }
EXPORT unsigned cw_rx_rows(void) { return dsp.wf_serial; }
EXPORT double cw_rx_floor(void) { return dsp.spec_floor; }
EXPORT int cw_rx_bins(void) { return DSP_BINS; }
EXPORT double cw_rx_bin_hz(void) { return DSP_BIN_HZ; }

/* The offered filter bandwidths, 0 past the last. */
EXPORT int cw_bandwidth(int i)
{
    return i >= 0 && i < RX_BANDWIDTH_COUNT ? RX_BANDWIDTHS[i] : 0;
}

/* Tunes to the strongest signal; 0 if there is none to tune to. */
EXPORT int cw_zero_beat(void)
{
    float snr, f = dsp_find_peak(&dsp, WF_FMIN, WF_FMAX, &snr);
    if (snr <= 8.0f)
        return 0;
    cfg.rx_freq = (int)(f + 0.5f);
    apply();
    return cfg.rx_freq;
}

/* Locks the receive speed at the measured one, or releases it. */
EXPORT int cw_lock_speed(void)
{
    cfg.rx_lock = !cfg.rx_lock;
    if (cfg.rx_lock)
        cfg.rx_wpm = clampi((int)(dec_wpm(&rxdec) + 0.5), 5, 60);
    apply();
    return cfg.rx_lock;
}

/* ------------------------------------------------------------------------ */
/* Training                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT int cw_train_enter(void) { return train_do(training_enter(&tr, &cfg, &txdec)); }
EXPORT int cw_train_type(int ch) { return train_do(training_type(&tr, &cfg, ch)); }
EXPORT int cw_train_backspace(void) { return train_do(training_backspace(&tr, &cfg)); }
EXPORT void cw_train_setup(void) { tr.state = TS_SETUP; }

EXPORT int cw_train_state(void) { return tr.state; }
EXPORT int cw_train_ok(void) { return tr.ok; }
EXPORT const char *cw_train_target(void) { return tr.target; }
EXPORT const char *cw_train_typed(void) { return tr.typed; }
EXPORT const char *cw_train_sent(void) { return tr.sent; }
EXPORT const char *cw_train_meaning(void) { return tr.meaning ? tr.meaning : ""; }
EXPORT int cw_train_total(void) { return tr.total; }
EXPORT int cw_train_correct(void) { return tr.correct; }
EXPORT int cw_train_streak(void) { return tr.streak; }
EXPORT int cw_train_best(void) { return tr.best; }
EXPORT double cw_train_send_wpm(void) { return tr.send_wpm; }
EXPORT double cw_train_send_ratio(void) { return tr.send_ratio; }

/* Recent results, newest first: 1 correct, 0 wrong, -1 past the history. */
EXPORT int cw_train_hist(int i)
{
    return i >= 0 && i < tr.nhist ? tr.hist[i] : -1;
}

EXPORT const char *cw_train_weak(void)
{
    scratch[training_weak(&tr, scratch, 5)] = 0;
    return scratch;
}

EXPORT const char *cw_train_charset(void)
{
    training_charset(&cfg, scratch, sizeof scratch);
    return scratch;
}

EXPORT int cw_koch_max(void)
{
    return (int)strlen(KOCH_ORDER);
}

/* ------------------------------------------------------------------------ */
/* Names and the Morse table                                                 */
/* ------------------------------------------------------------------------ */

/* Names for the settings, NULL past the last. */
EXPORT const char *cw_keyer_name(int i) { return i >= 0 && i < KEYER_MODE_COUNT ? keyer_mode_name(i) : NULL; }
EXPORT const char *cw_charset_name(int i) { return i >= 0 && i <= CHARSET_ALL ? TRAIN_CHARSET_NAMES[i] : NULL; }
EXPORT const char *cw_content_name(int i) { return i >= 0 && i <= TRAIN_CODES ? TRAIN_CONTENT_NAMES[i] : NULL; }

EXPORT int cw_table_count(void) { return MORSE_TABLE_COUNT; }
EXPORT int cw_table_group(int i) { return MORSE_TABLE[i].group; }
EXPORT const char *cw_table_text(int i) { return MORSE_TABLE[i].text; }
EXPORT const char *cw_table_code(int i) { return MORSE_TABLE[i].code; }
EXPORT const char *cw_table_name(int i) { return MORSE_TABLE[i].name ? MORSE_TABLE[i].name : ""; }

/* The codes of the characters of text, separated by spaces: ".- -...". */
EXPORT const char *cw_codes(const char *text)
{
    char tok[16];
    size_t len = 0;
    int n;
    scratch[0] = 0;
    while ((n = morse_next_token(text, tok, sizeof tok)) > 0) {
        const morse_entry *e = morse_lookup(tok);
        text += n;
        if (!e || len + strlen(e->code) + 2 > sizeof scratch)
            continue;
        if (len > 0)
            scratch[len++] = ' ';
        str_copy(scratch + len, sizeof scratch - len, e->code);
        len += strlen(e->code);
    }
    return scratch;
}
