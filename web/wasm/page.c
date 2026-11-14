/*
 * page.c - The application core of the web page (UI thread): the settings,
 * the decoders of both directions, the receiver, the transceiver's
 * break-in, the training sessions and the online room. It mirrors
 * src/app.c, src/mode_trx.c and src/net.c without the terminal; the page
 * feeds it with what the worklet and the WebSockets report and draws what
 * it exposes. The training statistics are kept here too; the page stores
 * their text form in the browser.
 */
#include <string.h>

#include "abbr.h"
#include "brokers.h"
#include "decoder.h"
#include "demo.h"
#include "dsp.h"
#include "keyer.h"
#include "link.h"
#include "morse.h"
#include "over.h"
#include "room.h"
#include "settings.h"
#include "statsbook.h"
#include "fcrypto.h"
#include "filen.h"
#include "statsync.h"
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

enum { MODE_TX, MODE_RX, MODE_TRX, MODE_TRAIN, MODE_STATS, MODE_NET };
enum { DIR_RX, DIR_TX, DIR_NET }; /* DIR_NET: a station of the online room */
enum { SOURCE_OFF, SOURCE_MIC, SOURCE_DEMO };
enum { CONTACT_DIT, CONTACT_DAH, CONTACT_STRAIGHT };
/* After dec_kind: the receive line ends, a key goes up or down, a station
 * enters or leaves the room. */
enum { TOK_LINE = TOK_SPACE + 1, TOK_KEY_UP, TOK_KEY_DOWN, TOK_JOIN, TOK_LEAVE };

/* What the decoders, the receiver and the room produced, in order, for the
 * page. */
typedef struct {
    int    dir, kind;
    char   text[8];
    double t_ms;  /* key transitions: the receiver or the room clock */
    int    who;   /* DIR_NET: the station's slot */
    char   call[ROOM_CALL_CAP];
} out_token;

static settings_t cfg, applied;
static int        applied_valid;
static cw_decoder txdec, rxdec;
static rx_dsp     dsp;
static training_t tr;
static statsbook_t book; /* this device's section for the training, the total shown */
static char        stats_text[BOOK_TEXT_CAP];
static int        today;
static int        resume_state = TS_SETUP; /* the session the setup interrupted */

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
    t->who = -1;
    t->call[0] = 0;
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

static room_t room;
static over_t net_over[ROOM_STATIONS], tx_over; /* where transmissions end */

static int keying_allowed(void)
{
    return mode == MODE_TX || mode == MODE_TRX || (mode == MODE_TRAIN && training_keying(&tr, &cfg)) ||
           (mode == MODE_NET && room.joined);
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
    statsbook_init(&book, "", "Web");
    tr.stats = &book.own.s;
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
    /* A different session: the interrupted one cannot be resumed. */
    if (strncmp(key, "train_", 6) == 0 && strcmp(key, "train_hint") != 0 && *p != value)
        resume_state = TS_SETUP;
    *p = value;
    apply();
    return *p;
}

EXPORT void cw_mode(int m)
{
    if (mode == MODE_TRX && ptt)
        ptt_off();
    /* Training keeps its session while another mode is shown; an item
     * being sent starts again (the decoder is cleared below). */
    if (m == MODE_TRAIN && mode != MODE_TRAIN && tr.state == TS_SEND)
        training_retry(&tr, &cfg);
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
    if (room.joined)
        room_key(&room, on, t_ms);
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
        /* In a room, K and the like end the operator's line. */
        if (mode == MODE_NET && room.joined && over_token(&tx_over, &tok) != OVER_NONE)
            emit(DIR_TX, TOK_LINE, "", 0);
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

/* DIR_NET: the station's slot (for its tape) and callsign. */
EXPORT int cw_pop_who(void)
{
    return popped.who;
}

EXPORT const char *cw_pop_call(void)
{
    return popped.call;
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

EXPORT int cw_train_enter(void)
{
    if (tr.state == TS_SETUP)
        resume_state = TS_SETUP; /* a new session */
    return train_do(training_enter(&tr, &cfg, &txdec));
}
EXPORT int cw_train_type(int ch) { return train_do(training_type(&tr, &cfg, ch)); }
EXPORT int cw_train_backspace(void) { return train_do(training_backspace(&tr, &cfg)); }
/* Back to the setup; the session can be resumed until a setting changes. */
EXPORT void cw_train_setup(void)
{
    if (tr.state != TS_SETUP)
        resume_state = tr.state;
    tr.state = TS_SETUP;
}

EXPORT int cw_train_resumable(void) { return resume_state != TS_SETUP; }

EXPORT int cw_train_resume(void)
{
    int fx = 0;
    if (resume_state == TS_SETUP)
        return 0;
    tr.state = resume_state;
    resume_state = TS_SETUP;
    if (tr.state == TS_SEND)
        fx = train_do(training_retry(&tr, &cfg));
    return fx;
}

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

/* The page's clock (ms) and local day number, every frame. */
EXPORT void cw_clock(double now_ms, int day)
{
    today = day;
    training_clock(&tr, now_ms, day);
}

/* Copy: another listen, TRAIN_PLAY or 0 when the level allows no more. */
EXPORT int cw_train_replay(void) { return training_replay(&tr, &cfg); }
EXPORT int cw_train_listens(void) { return tr.listens; }
EXPORT int cw_train_max_listens(void) { return training_max_listens(&cfg); }
EXPORT int cw_train_rec_hit(void) { return tr.rec_hit; }
EXPORT int cw_train_record(void) { return training_record(&tr, &cfg); }

/* ------------------------------------------------------------------------ */
/* Statistics                                                                */
/* ------------------------------------------------------------------------ */

/* Where the page writes the stored text before cw_stats_load() and
 * cw_stats_import(). */
EXPORT char *cw_stats_buf(void) { return stats_text; }
EXPORT int cw_stats_cap(void) { return (int)sizeof stats_text; }

/* This device's id (hex) for a new book, before cw_stats_load(): a stored
 * book keeps its own. */
EXPORT void cw_stats_init(const char *id)
{
    if (statsbook_valid_id(id))
        str_copy(book.own.id, sizeof book.own.id, id);
}

EXPORT void cw_stats_load(void)
{
    stats_text[sizeof stats_text - 1] = 0;
    statsbook_parse(&book, stats_text);
}

/* An exported file in stats_text, read into the book it would make: 0, or
 * -1 if it is not one. cw_stats_import() takes it. */
static statsbook_t preview;

EXPORT int cw_stats_preview(void)
{
    stats_text[sizeof stats_text - 1] = 0;
    preview = book;
    return statsbook_import(&preview, stats_text);
}

EXPORT int cw_preview_answers(void)
{
    return preview.total.dir[STATS_COPY].answers + preview.total.dir[STATS_SEND].answers;
}

/* The devices with answers in the file. */
EXPORT int cw_preview_devices(void)
{
    return preview.others + (preview.own.s.dir[STATS_COPY].answers + preview.own.s.dir[STATS_SEND].answers > 0);
}

EXPORT int cw_preview_last_day(void) { return preview.total.last_day; }

EXPORT void cw_stats_import(void) { book = preview; }

/* The training counted: a new revision of this device's section. */
EXPORT void cw_stats_changed(void) { statsbook_changed(&book); }

/* The text to store or export, NULL if it does not fit. */
EXPORT const char *cw_stats_save(void)
{
    return statsbook_format(&book, stats_text, (int)sizeof stats_text) >= 0 ? stats_text : NULL;
}

/* An integer of the total by its key in the text form, -1 for an unknown
 * key. */
EXPORT int cw_stat(const char *key)
{
    int *p = stats_field(&book.total, key);
    return p ? *p : -1;
}

/* Devices with statistics, this one included. */
EXPORT int cw_stat_devices(void) { return 1 + book.others; }

EXPORT int cw_stat_seen(int dir, int ch) { return book.total.dir[dir & 1].seen[ch & 127]; }
EXPORT int cw_stat_pct(int dir, int ch) { return stats_char_pct(&book.total, dir, ch); }
EXPORT int cw_stat_activity(int ago) { return stats_activity(&book.total, today, ago); }
EXPORT int cw_stat_day_streak(void) { return stats_day_streak(&book.total, today); }

EXPORT const char *cw_stat_weak(int dir)
{
    scratch[stats_weak(&book.total, dir, scratch, 6)] = 0;
    return scratch;
}

/* ------------------------------------------------------------------------ */
/* The statistics on Filen                                                   */
/* ------------------------------------------------------------------------ */

/* The client (src/filen.c, statsync.c): web/cloud.js performs each request
 * it prepares with fetch() and hands back the reply. */
static filen_t    cloud;
static statsync_t ysync;
static char       cloud_in[1024]; /* text from the page: email, password, code */
static char       cloud_out[4096];

/* Argon2's 64 MiB for version 3 accounts, only when one signs in: the
 * memory grows once and stays. */
static void *argon_area(void)
{
    static void *area;
    if (!area) {
        int old = __builtin_wasm_memory_grow(0, FC_ARGON2_WORK(FILEN_ARGON2_KIB) / 65536 + 1);
        if (old < 0)
            return NULL;
        area = (void *)(((unsigned long)old * 65536 + 7) & ~7UL);
    }
    return area;
}

EXPORT char *cw_cloud_in(void) { return cloud_in; }
EXPORT int cw_cloud_in_cap(void) { return (int)sizeof cloud_in; }

/* 32 random bytes in cloud_in (crypto.getRandomValues), before any work. */
EXPORT void cw_cloud_seed(void)
{
    fc_seed(cloud_in, 32);
    fc_wipe(cloud_in, sizeof cloud_in);
    if (!cloud.data_name[0]) {
        filen_init(&cloud, STATSYNC_FOLDER);
        cloud.get_work = argon_area;
        statsync_init(&ysync);
    }
}

/* A kept session (its text in cloud_in): 1 if signed in. */
EXPORT int cw_cloud_session_load(void)
{
    cloud_in[sizeof cloud_in - 1] = 0;
    if (filen_session_parse(&cloud.s, cloud_in) != 0)
        memset(&cloud.s, 0, sizeof cloud.s);
    fc_wipe(cloud_in, sizeof cloud_in);
    return filen_signed_in(&cloud);
}

/* The session to keep, NULL if none. */
EXPORT const char *cw_cloud_session(void)
{
    return filen_signed_in(&cloud) && filen_session_format(&cloud.s, cloud_out, sizeof cloud_out) > 0 ? cloud_out
                                                                                                       : NULL;
}

/* Signs in with "email\0password\0code\0" in cloud_in. */
EXPORT void cw_cloud_login(void)
{
    const char *email = cloud_in, *pw = email + strlen(email) + 1, *code = pw + strlen(pw) + 1;
    cloud_in[sizeof cloud_in - 1] = 0;
    filen_login(&cloud, email, pw, code);
    fc_wipe(cloud_in, sizeof cloud_in);
}

/* A sync of the statistics, now_ms the wall clock. 0 if not signed in. */
EXPORT int cw_cloud_sync(double now_ms)
{
    if (!filen_signed_in(&cloud))
        return 0;
    cloud.now_ms = now_ms;
    statsync_start(&ysync, &cloud, &book);
    return 1;
}

/* After a sync: the newer sections into the book. 1 if it changed. */
EXPORT int cw_cloud_merge(void) { return statsync_merge(&ysync, &book); }
EXPORT int cw_cloud_pending(void) { return filen_signed_in(&cloud) && statsync_pending(&ysync, &book); }

EXPORT void cw_cloud_sign_out(void)
{
    filen_sign_out(&cloud);
    statsync_init(&ysync);
}

/* The task: FILEN_WAIT with a request ready, FILEN_DONE, FILEN_FAIL. */
EXPORT int cw_cloud_run(void) { return filen_run(&cloud); }
EXPORT int cw_cloud_post(void) { return cloud.req.method == FILEN_POST; }
EXPORT const char *cw_cloud_url(void) { return cloud.req.url; }
EXPORT const char *cw_cloud_auth(void) { return cloud.req.auth; }
EXPORT const char *cw_cloud_checksum(void) { return cloud.req.checksum; }
EXPORT int cw_cloud_json(void) { return cloud.req.json; }
EXPORT const unsigned char *cw_cloud_body(void) { return cloud.req.body; }
EXPORT int cw_cloud_body_len(void) { return cloud.req.body_len; }
EXPORT char *cw_cloud_resp(void) { return cloud.resp; }
EXPORT int cw_cloud_resp_cap(void) { return FILEN_RESP_CAP; }
EXPORT void cw_cloud_reply(int status, int len) { filen_reply(&cloud, status, len); }

EXPORT int cw_cloud_signed_in(void) { return filen_signed_in(&cloud); }
EXPORT int cw_cloud_signed_out(void) { return cloud.signed_out; }
EXPORT int cw_cloud_need_code(void) { return cloud.need_code; }
EXPORT const char *cw_cloud_error(void) { return cloud.error; }
EXPORT const char *cw_cloud_email(void) { return cloud.s.email; }
EXPORT int cw_cloud_sent(void) { return ysync.sent; }

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

/* A decoded word: 0 plain text, 1 an abbreviation (QRL, 73...), 2 a sign
 * (K, AR, SK...). See src/abbr.h. */
EXPORT int cw_word(const char *word)
{
    return abbr_word(word, (int)strlen(word));
}

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

/* ------------------------------------------------------------------------ */
/* Online room                                                               */
/* ------------------------------------------------------------------------ */

/* The page's transport is a WebSocket per broker (net.js); the MQTT
 * sessions, the room protocol and the scheduling of the remote voices are
 * the shared C of src/. The room clock is the worklet's; the links run on
 * the page's own clock, which keeps going while audio is suspended. */

#define NET_IN_CAP 4096
#define NET_VKEYS 256
#define NET_MARGIN_MS 60.0 /* reports every 20 ms, postMessage, a quantum */

static link_t  links[BROKERS];
static int     was_up[BROKERS];
static uint8_t net_seed[32];
static uint8_t net_in[NET_IN_CAP];
static uint8_t net_out[MQTT_OUT_CAP];
static double  net_vkeys[4 * NET_VKEYS]; /* voice, ms, on, tone */
static int     net_nvkeys;
static double  room_now, wall_now;

static void on_link_message(void *ctx, const uint8_t *payload, int len)
{
    (void)ctx;
    room_receive(&room, payload, len, room_now);
}

static int web_configured(int i)
{
    return i >= 0 && i < BROKERS && brokers[i].ws_url && brokers[i].ws_url[0];
}

static void net_settings(void)
{
    room_settings(&room, cfg.wpm, cfg.net_tone, cfg.tone_hz, NET_MARGIN_MS);
}

/* The page fills these 32 bytes with crypto.getRandomValues, then calls
 * cw_net_init(). */
EXPORT uint8_t *cw_net_seed(void)
{
    return net_seed;
}

EXPORT void cw_net_init(void)
{
    static const char digits[] = "0123456789abcdef";
    char id[16];
    int i, k;
    room_init(&room, net_seed);
    memset(net_seed, 0, sizeof net_seed);
    for (i = 0; i < BROKERS; i++) {
        uint32_t v = rng_u32();
        memcpy(id, "cwtty-", 6);
        for (k = 0; k < 8; k++)
            id[6 + k] = digits[(v >> (28 - 4 * k)) & 15];
        id[14] = 0;
        link_init(&links[i], id, brokers[i].user, brokers[i].pass, on_link_message, NULL);
    }
}

EXPORT int cw_net_brokers(void) { return BROKERS; }
EXPORT const char *cw_net_broker_name(int i) { return i >= 0 && i < BROKERS ? brokers[i].name : ""; }
EXPORT const char *cw_net_broker_url(int i) { return web_configured(i) ? brokers[i].ws_url : ""; }

/* Enters the room in cw_buf(): "callsign\nroom\npassword". Returns 0 or
 * ROOM_BAD_CALL / ROOM_BAD_NAME. */
EXPORT int cw_net_join(void)
{
    char *call = buf, *name, *pass;
    int err, i;
    buf[sizeof buf - 1] = 0;
    name = strchr(call, '\n');
    if (!name)
        return ROOM_BAD_NAME;
    *name++ = 0;
    pass = strchr(name, '\n');
    if (pass)
        *pass++ = 0;
    net_settings();
    err = room_join(&room, call, name, pass ? pass : "", cfg.wpm, room_now);
    memset(buf, 0, sizeof buf); /* the password */
    if (err)
        return err;
    net_nvkeys = 0;
    over_reset(&tx_over);
    for (i = 0; i < ROOM_STATIONS; i++)
        over_reset(&net_over[i]);
    for (i = 0; i < BROKERS; i++) {
        was_up[i] = 0;
        if (web_configured(i))
            link_start(&links[i], room.topic, wall_now);
    }
    return 0;
}

EXPORT void cw_net_leave(void)
{
    uint8_t p[ROOM_PACKET_CAP];
    int len, i;
    if (!room.joined)
        return;
    room_leave(&room, room_now);
    while ((len = room_pop_out(&room, p, sizeof p)) > 0)
        for (i = 0; i < BROKERS; i++)
            link_publish(&links[i], p, len);
    for (i = 0; i < BROKERS; i++)
        link_stop(&links[i], wall_now);
    net_nvkeys = 0;
}

EXPORT int cw_net_joined(void) { return room.joined; }

static void emit_station(int kind, const char *text, double t_ms, int station, const char *call)
{
    emit(DIR_NET, kind, text, t_ms);
    out[(out_head + out_count - 1) % OUT_CAP].who = station;
    str_copy(out[(out_head + out_count - 1) % OUT_CAP].call, ROOM_CALL_CAP, call);
}

/*
 * Runs the links and the room. room_ms: the worklet's clock now; known_ms:
 * the clock of its last report (every operator transition before it was
 * passed to cw_tx_event); wall_ms: the page's clock. Returns what to do
 * with the WebSockets: bit 2i opens link i's, bit 2i + 1 closes it.
 */
EXPORT int cw_net_update(double room_ms, double known_ms, double wall_ms)
{
    uint8_t p[ROOM_PACKET_CAP];
    room_event e;
    room_vkey k;
    int i, len, acts = 0, up = 0;

    room_now = room_ms;
    wall_now = wall_ms;
    net_settings();
    for (i = 0; i < BROKERS; i++) {
        int a, on;
        if (!web_configured(i))
            continue;
        a = link_update(&links[i], wall_ms);
        if (a == LINK_OPEN) {
            acts |= 1 << (2 * i);
        } else if (a == LINK_CLOSE) {
            acts |= 2 << (2 * i);
            link_closed(&links[i], wall_ms, NULL);
        }
        on = link_up(&links[i]);
        if (on && !was_up[i]) /* those only on this broker must learn of us */
            room_announce(&room, room_ms);
        was_up[i] = on;
        up += on;
    }
    room_update(&room, room_ms, known_ms);
    while (up > 0 && (len = room_pop_out(&room, p, sizeof p)) > 0)
        for (i = 0; i < BROKERS; i++)
            link_publish(&links[i], p, len);
    while (room_pop_event(&room, &e)) {
        const room_station *st = &room.st[e.station];
        int kind = e.kind == RE_JOIN    ? TOK_JOIN
                   : e.kind == RE_LEAVE ? TOK_LEAVE
                   : e.kind == RE_KEY   ? (e.on ? TOK_KEY_DOWN : TOK_KEY_UP)
                                        : e.tok.kind;
        emit_station(kind, e.kind == RE_TOKEN ? e.tok.text : "", e.t, e.station, st->call);
        if (e.kind == RE_JOIN)
            over_reset(&net_over[e.station]);
        else if (e.kind == RE_TOKEN && over_token(&net_over[e.station], &e.tok) != OVER_NONE)
            emit_station(TOK_LINE, "", e.t, e.station, st->call); /* K and the like end its line */
    }
    while (net_nvkeys < NET_VKEYS && room_pop_vkey(&room, &k)) {
        double *v = &net_vkeys[4 * net_nvkeys++];
        v[0] = k.voice;
        v[1] = k.t;
        v[2] = k.on;
        v[3] = k.tone_hz;
    }
    return acts;
}

/* Remote voice key changes for the worklet: (voice, ms, on, tone) each. */
EXPORT double *cw_net_vkeys(void) { return net_vkeys; }

EXPORT int cw_net_take_vkeys(void)
{
    int n = net_nvkeys;
    net_nvkeys = 0;
    return n;
}

/* The room clock stopped (audio suspended): measure the delays again. */
EXPORT void cw_net_resync(void) { room_resync(&room); }

/* The WebSocket of link i. */
EXPORT void cw_net_opened(int i)
{
    if (i >= 0 && i < BROKERS)
        link_opened(&links[i], wall_now);
}

EXPORT void cw_net_closed(int i)
{
    if (i >= 0 && i < BROKERS)
        link_closed(&links[i], wall_now, "connection closed");
}

EXPORT uint8_t *cw_net_in_buf(void) { return net_in; }
EXPORT int cw_net_in_cap(void) { return NET_IN_CAP; }

EXPORT void cw_net_input(int i, int n)
{
    if (i >= 0 && i < BROKERS)
        link_input(&links[i], net_in, clampi(n, 0, NET_IN_CAP), wall_now);
}

/* Copies what link i has to send into cw_net_out_buf(); returns how much. */
EXPORT int cw_net_out(int i)
{
    const uint8_t *o;
    int n;
    if (i < 0 || i >= BROKERS)
        return 0;
    o = link_output(&links[i], &n);
    n = clampi(n, 0, (int)sizeof net_out);
    memcpy(net_out, o, (size_t)n);
    link_consume(&links[i], n);
    return n;
}

EXPORT uint8_t *cw_net_out_buf(void) { return net_out; }

/* A link's state: 2 carrying the room, 1 waiting after a failure (see the
 * reason and the retry), 0 connecting or off, -1 not configured. */
EXPORT int cw_net_link_state(int i)
{
    if (!web_configured(i))
        return -1;
    if (link_up(&links[i]))
        return 2;
    return links[i].state == LINK_WAIT && links[i].reason[0] ? 1 : 0;
}

EXPORT const char *cw_net_link_reason(int i) { return i >= 0 && i < BROKERS ? links[i].reason : ""; }
EXPORT int cw_net_link_retry(int i) { return i >= 0 && i < BROKERS ? link_retry_in(&links[i], wall_now) : 0; }

EXPORT const char *cw_net_room_name(void) { return room.name; }
EXPORT int cw_net_room_private(void) { return room.priv; }
EXPORT const char *cw_net_call(void) { return room.call; }

EXPORT int cw_net_stations(void) { return ROOM_STATIONS; }
EXPORT int cw_net_station_used(int s) { return s >= 0 && s < ROOM_STATIONS && room.st[s].used; }
EXPORT const char *cw_net_station_call(int s) { return s >= 0 && s < ROOM_STATIONS ? room.st[s].call : ""; }
EXPORT int cw_net_station_tone(int s) { return s >= 0 && s < ROOM_STATIONS ? room.st[s].tone_hz : 0; }
EXPORT int cw_net_station_wpm(int s) { return s >= 0 && s < ROOM_STATIONS ? room.st[s].wpm : 0; }

/* The length of a dit as decoded, of station s or (s < 0) the operator. */
EXPORT double cw_net_unit(int s)
{
    return dec_unit_ms(s >= 0 && s < ROOM_STATIONS ? &room.st[s].dec : &txdec);
}
