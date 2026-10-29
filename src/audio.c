/*
 * audio.c - Audio engine built on miniaudio.
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "miniaudio.h"

#include "atomics.h"
#include "engine.h"
#include "morse.h"
#include "util.h"

#define EVENT_RING_BYTES (1024 * sizeof(audio_event))
#define SEQ_RING_BYTES (8192 * sizeof(seq_seg))
#define CAPTURE_FRAMES (AUDIO_RX_RATE * 4)

typedef struct {
    uint32_t samples;
    uint16_t on;
    uint16_t gen;
} seq_seg;

static struct {
    /* --- devices (UI thread only) --- */
    ma_context ctx;
    int        ctx_ok;
    ma_context null_ctx;
    int        null_ctx_ok;
    ma_device  play;
    int        play_ok;
    int        play_null;
    ma_device  cap;
    int        cap_ok;
    int        cap_running;
    char       in_dev[AUDIO_NAME_CAP];
    int        latency_ms;
    char       error[160];

    ma_rb     events; /* audio -> UI */
    ma_rb     seq;    /* UI -> audio */
    ma_pcm_rb capbuf; /* audio -> UI */
    int       rings_ok;

    /* --- shared --- */
    at_int tone_hz, wpm, keyer_mode, volume, rise_ms;
    at_int dit_level, dah_level, straight_level;
    at_int dit_count, dah_count, straight_count;
    at_int ack_presses; /* all press counters seen by the callback */
    at_int op_enabled;
    at_int seq_gen;
    at_int seq_consumed;
    at_u64 clock;
    at_u64 op_last_active;
    at_int op_busy;
    at_int rate;

    /* --- UI thread private --- */
    int      seq_submitted;
    int      ui_gen;
    double   base_ms; /* clock rebasing after a device change */
    uint64_t base_samples;

    /* --- audio thread private --- */
    engine_t eng;
    int      seen_dit, seen_dah, seen_straight;
    int      cur_gen;
    seq_seg  cur_seg;
    uint32_t seg_left;
    int      seg_active;
} A;

/* ======================================================================== */
/* Real-time side                                                           */
/* ======================================================================== */

static void push_event(uint64_t t, int on, int src)
{
    size_t size = sizeof(audio_event);
    void *p;
    if (ma_rb_acquire_write(&A.events, &size, &p) == MA_SUCCESS && size == sizeof(audio_event)) {
        audio_event *e = (audio_event *)p;
        e->t = t;
        e->on = on;
        e->src = src;
        ma_rb_commit_write(&A.events, size);
    }
}

/* Returns the playback key state for this sample. */
static int seq_tick(void)
{
    while (A.seg_left == 0) {
        size_t size = sizeof(seq_seg);
        void *p;
        A.seg_active = 0;
        if (ma_rb_acquire_read(&A.seq, &size, &p) != MA_SUCCESS || size != sizeof(seq_seg))
            return 0;
        memcpy(&A.cur_seg, p, sizeof(seq_seg));
        ma_rb_commit_read(&A.seq, size);
        /* Compare with the latest generation, not the one sampled at the
         * start of the callback: the UI may have flushed and queued new
         * text in the meantime. */
        if (A.cur_seg.gen != (uint16_t)at_get(&A.seq_gen) || A.cur_seg.samples == 0) {
            at_add(&A.seq_consumed, 1); /* stale (flushed) or empty */
            continue;
        }
        A.seg_left = A.cur_seg.samples;
        A.seg_active = 1;
    }
    if (--A.seg_left == 0) {
        at_add(&A.seq_consumed, 1);
        A.seg_active = 0;
    }
    return A.cur_seg.on;
}

static int playback_tick(void *ctx)
{
    (void)ctx;
    return seq_tick();
}

static void playback_event(void *ctx, uint64_t t, int on, int src)
{
    (void)ctx;
    push_event(t, on, src);
}

static void playback_callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    static const engine_io io = {playback_tick, playback_event, NULL};
    engine_t *e = &A.eng;
    int dc = at_get(&A.dit_count), ac = at_get(&A.dah_count), sc = at_get(&A.straight_count);
    int gen;
    (void)input;

    e->tone_hz = at_get(&A.tone_hz);
    e->wpm = at_get(&A.wpm);
    e->keyer_mode = at_get(&A.keyer_mode);
    e->volume = at_get(&A.volume);
    e->rise_ms = at_get(&A.rise_ms);
    if (at_get(&A.op_enabled)) {
        e->dit = at_get(&A.dit_level);
        e->dah = at_get(&A.dah_level);
        e->straight = at_get(&A.straight_level);
        e->dit_edge = dc != A.seen_dit;
        e->dah_edge = ac != A.seen_dah;
        e->straight_edge = sc != A.seen_straight;
    } else {
        e->dit = e->dah = e->straight = 0;
        e->dit_edge = e->dah_edge = e->straight_edge = 0;
    }
    A.seen_dit = dc;
    A.seen_dah = ac;
    A.seen_straight = sc;
    at_set(&A.ack_presses, dc + ac + sc);

    gen = at_get(&A.seq_gen);
    if (gen != A.cur_gen) {
        /* Abandon the segment being played only if it belongs to the old
         * text: seq_tick() may already have started the new one. */
        if (A.seg_left > 0 && A.cur_seg.gen != (uint16_t)gen) {
            at_add(&A.seq_consumed, 1);
            A.seg_left = 0;
            A.seg_active = 0;
            A.cur_seg.on = 0;
        }
        A.cur_gen = gen;
    }

    e->clock = at_get64(&A.clock);
    e->last_active = at_get64(&A.op_last_active);
    engine_render(e, &io, (float *)output, (int)frames, (int)dev->playback.channels, (double)dev->sampleRate);
    at_set64(&A.clock, e->clock);
    at_set64(&A.op_last_active, e->last_active);
    at_set(&A.op_busy, engine_busy(e));
}

static void capture_callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    const float *in = (const float *)input;
    (void)dev;
    (void)output;
    while (frames > 0) {
        ma_uint32 n = frames;
        void *p;
        if (ma_pcm_rb_acquire_write(&A.capbuf, &n, &p) != MA_SUCCESS || n == 0)
            return; /* UI is not keeping up: drop */
        memcpy(p, in, n * sizeof(float));
        ma_pcm_rb_commit_write(&A.capbuf, n);
        in += n;
        frames -= n;
    }
}

/* ======================================================================== */
/* Device management                                                        */
/* ======================================================================== */

static int find_device(ma_device_type type, const char *name, ma_device_id *id)
{
    ma_device_info *play, *capt, *list;
    ma_uint32 np, nc, n, i;
    if (!A.ctx_ok || !name || !*name)
        return 0;
    if (ma_context_get_devices(&A.ctx, &play, &np, &capt, &nc) != MA_SUCCESS)
        return 0;
    list = type == ma_device_type_playback ? play : capt;
    n = type == ma_device_type_playback ? np : nc;
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].name, name) == 0) {
            *id = list[i].id;
            return 1;
        }
    }
    return 0;
}

static int open_playback(const char *name)
{
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    ma_device_id id;

    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 0; /* native */
    cfg.sampleRate = 0;        /* native */
    cfg.periodSizeInMilliseconds = (ma_uint32)A.latency_ms;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback = playback_callback;
    cfg.pulse.pStreamNamePlayback = "CW sidetone";
    if (find_device(ma_device_type_playback, name, &id))
        cfg.playback.pDeviceID = &id;

    A.play_null = 0;
    if (A.ctx_ok && ma_device_init(&A.ctx, &cfg, &A.play) == MA_SUCCESS &&
        ma_device_start(&A.play) == MA_SUCCESS) {
        A.play_ok = 1;
        return 1;
    }
    if (A.ctx_ok)
        ma_device_uninit(&A.play);
    snprintf(A.error, sizeof A.error, "No audio output: running silently");

    /* Keep the keyer clock running on the null backend. */
    if (!A.null_ctx_ok) {
        ma_backend nb = ma_backend_null;
        A.null_ctx_ok = ma_context_init(&nb, 1, NULL, &A.null_ctx) == MA_SUCCESS;
    }
    if (A.null_ctx_ok) {
        cfg.playback.pDeviceID = NULL;
        cfg.playback.channels = 1;
        cfg.sampleRate = 48000;
        if (ma_device_init(&A.null_ctx, &cfg, &A.play) == MA_SUCCESS &&
            ma_device_start(&A.play) == MA_SUCCESS) {
            A.play_ok = 1;
            A.play_null = 1;
            return 1;
        }
        ma_device_uninit(&A.play);
    }
    A.play_ok = 0;
    return 0;
}

static void close_playback(void)
{
    if (A.play_ok) {
        ma_device_uninit(&A.play);
        A.play_ok = 0;
    }
}

int audio_capture_start(void)
{
    ma_device_config cfg;
    ma_device_id id;
    if (A.cap_running)
        return 1;
    if (!A.ctx_ok)
        return 0;
    cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.sampleRate = AUDIO_RX_RATE;
    cfg.periodSizeInMilliseconds = 10;
    cfg.dataCallback = capture_callback;
    cfg.resampling.linear.lpfOrder = 8; /* better anti-aliasing when downsampling */
    cfg.pulse.pStreamNameCapture = "CW receiver";
    if (find_device(ma_device_type_capture, A.in_dev, &id))
        cfg.capture.pDeviceID = &id;

    ma_pcm_rb_reset(&A.capbuf);
    if (ma_device_init(&A.ctx, &cfg, &A.cap) != MA_SUCCESS) {
        snprintf(A.error, sizeof A.error, "No audio input device available");
        return 0;
    }
    if (ma_device_start(&A.cap) != MA_SUCCESS) {
        ma_device_uninit(&A.cap);
        snprintf(A.error, sizeof A.error, "Could not start the audio input");
        return 0;
    }
    A.cap_ok = 1;
    A.cap_running = 1;
    return 1;
}

void audio_capture_stop(void)
{
    if (A.cap_running) {
        ma_device_uninit(&A.cap);
        A.cap_running = 0;
        A.cap_ok = 0;
    }
}

int audio_init(const char *out_dev, const char *in_dev, int latency_ms)
{
    ma_context_config cc = ma_context_config_init();

    memset(&A, 0, sizeof A);
    A.latency_ms = clampi(latency_ms, 2, 100);
    str_copy(A.in_dev, sizeof A.in_dev, in_dev);
    at_set(&A.tone_hz, 600);
    at_set(&A.wpm, 20);
    at_set(&A.volume, 70);
    at_set(&A.rise_ms, 5);
    at_set(&A.op_enabled, 1);
    engine_init(&A.eng);

    if (ma_rb_init(EVENT_RING_BYTES, NULL, NULL, &A.events) != MA_SUCCESS)
        return -1;
    if (ma_rb_init(SEQ_RING_BYTES, NULL, NULL, &A.seq) != MA_SUCCESS) {
        ma_rb_uninit(&A.events);
        return -1;
    }
    if (ma_pcm_rb_init(ma_format_f32, 1, CAPTURE_FRAMES, NULL, NULL, &A.capbuf) != MA_SUCCESS) {
        ma_rb_uninit(&A.seq);
        ma_rb_uninit(&A.events);
        return -1;
    }
    A.rings_ok = 1;

    cc.pulse.pApplicationName = "CWtty";
    cc.jack.pClientName = "CWtty";
    A.ctx_ok = ma_context_init(NULL, 0, &cc, &A.ctx) == MA_SUCCESS;
    if (!A.ctx_ok)
        snprintf(A.error, sizeof A.error, "Audio system unavailable");
    open_playback(out_dev);
    return 0;
}

int audio_reopen(const char *out_dev, const char *in_dev, int latency_ms)
{
    int had_capture = A.cap_running;
    unsigned old_rate = A.play_ok && A.play.sampleRate ? A.play.sampleRate : 48000;
    /* The clock keeps counting samples; rebase the ms conversion (with the
     * old rate, before closing) so a different sample rate on the new
     * device does not make time jump. */
    uint64_t now_samples;
    double now_ms;

    audio_capture_stop();
    close_playback();
    /* Stopping the device can run one last callback. Snapshot only once
     * it has stopped, converting with the previous device's sample rate. */
    now_samples = at_get64(&A.clock);
    now_ms = A.base_ms + ((double)now_samples - (double)A.base_samples) * 1000.0 / old_rate;
    A.base_ms = now_ms;
    A.base_samples = now_samples;
    A.error[0] = 0;
    A.latency_ms = clampi(latency_ms, 2, 100);
    str_copy(A.in_dev, sizeof A.in_dev, in_dev);
    /* No callback is running now, so the UI may produce these events. */
    if (A.eng.op_state)
        push_event(now_samples, 0, SRC_OPERATOR);
    if (A.eng.pb_state)
        push_event(now_samples, 0, SRC_PLAYBACK);
    A.eng.op_state = A.eng.pb_state = 0;
    at_set(&A.op_busy, 0);
    at_set64(&A.op_last_active, now_samples);
    A.eng.env = 0.0f;
    keyer_reset(&A.eng.keyer);
    open_playback(out_dev);
    if (had_capture)
        audio_capture_start();
    return A.play_ok;
}

void audio_shutdown(void)
{
    audio_capture_stop();
    close_playback();
    if (A.ctx_ok)
        ma_context_uninit(&A.ctx);
    if (A.null_ctx_ok)
        ma_context_uninit(&A.null_ctx);
    if (A.rings_ok) {
        ma_rb_uninit(&A.events);
        ma_rb_uninit(&A.seq);
        ma_pcm_rb_uninit(&A.capbuf);
    }
    memset(&A, 0, sizeof A);
}

void audio_get_status(audio_status *st)
{
    memset(st, 0, sizeof *st);
    st->out_ok = A.play_ok && !A.play_null;
    st->out_null = A.play_ok && A.play_null;
    st->in_ok = A.cap_running;
    if (A.play_ok) {
        st->out_rate = A.play.sampleRate;
        if (!A.play_null)
            str_copy(st->out_name, sizeof st->out_name, A.play.playback.name);
    }
    if (A.cap_running)
        str_copy(st->in_name, sizeof st->in_name, A.cap.capture.name);
    str_copy(st->error, sizeof st->error, A.error);
}

int audio_list_devices(int capture, char (*names)[AUDIO_NAME_CAP], int cap)
{
    ma_device_info *play, *capt, *list;
    ma_uint32 np, nc, n, i;
    if (!A.ctx_ok)
        return 0;
    if (ma_context_get_devices(&A.ctx, &play, &np, &capt, &nc) != MA_SUCCESS)
        return 0;
    list = capture ? capt : play;
    n = capture ? nc : np;
    for (i = 0; i < n && (int)i < cap; i++)
        str_copy(names[i], AUDIO_NAME_CAP, list[i].name);
    return (int)i;
}

/* ======================================================================== */
/* UI side                                                                  */
/* ======================================================================== */

void audio_set_params(const audio_params *p)
{
    at_set(&A.tone_hz, clampi(p->tone_hz, 100, 3000));
    at_set(&A.wpm, clampi(p->wpm, 5, 80));
    at_set(&A.keyer_mode, clampi(p->keyer_mode, 0, KEYER_MODE_COUNT - 1));
    at_set(&A.volume, clampi(p->volume, 0, 100));
    at_set(&A.rise_ms, clampi(p->rise_ms, 1, 20));
}

void audio_key_dit(int down)
{
    if (down && !at_get(&A.dit_level))
        at_add(&A.dit_count, 1);
    at_set(&A.dit_level, down);
}

void audio_key_dah(int down)
{
    if (down && !at_get(&A.dah_level))
        at_add(&A.dah_count, 1);
    at_set(&A.dah_level, down);
}

void audio_key_straight(int down)
{
    if (down && !at_get(&A.straight_level))
        at_add(&A.straight_count, 1);
    at_set(&A.straight_level, down);
}

void audio_release_keys(void)
{
    at_set(&A.dit_level, 0);
    at_set(&A.dah_level, 0);
    at_set(&A.straight_level, 0);
}

void audio_enable_operator(int on)
{
    if (!on)
        audio_release_keys();
    at_set(&A.op_enabled, on);
}

int audio_operator_busy(void)
{
    /* Presses the callback has not picked up yet count as busy too. */
    int presses = at_get(&A.dit_count) + at_get(&A.dah_count) + at_get(&A.straight_count);
    return at_get(&A.op_busy) || at_get(&A.straight_level) || at_get(&A.dit_level) ||
           at_get(&A.dah_level) || (A.play_ok && presses != at_get(&A.ack_presses));
}

double audio_operator_idle_ms(void)
{
    uint64_t now = at_get64(&A.clock), last = at_get64(&A.op_last_active);
    if (audio_operator_busy())
        return 0.0;
    return now > last ? audio_to_ms(now) - audio_to_ms(last) : 0.0;
}

double audio_to_ms(uint64_t samples)
{
    unsigned rate = A.play_ok ? A.play.sampleRate : 48000;
    if (rate == 0)
        rate = 48000;
    return A.base_ms + ((double)samples - (double)A.base_samples) * 1000.0 / (double)rate;
}

double audio_clock_ms(void)
{
    return audio_to_ms(at_get64(&A.clock));
}

int audio_poll_events(audio_event *ev, int cap)
{
    int n = 0;
    while (n < cap) {
        size_t size = sizeof(audio_event);
        void *p;
        if (ma_rb_acquire_read(&A.events, &size, &p) != MA_SUCCESS || size != sizeof(audio_event))
            break;
        memcpy(&ev[n++], p, sizeof(audio_event));
        ma_rb_commit_read(&A.events, size);
    }
    return n;
}

int audio_capture_read(float *buf, int cap)
{
    int total = 0;
    if (!A.cap_running)
        return 0;
    while (total < cap) {
        ma_uint32 n = (ma_uint32)(cap - total);
        void *p;
        if (ma_pcm_rb_acquire_read(&A.capbuf, &n, &p) != MA_SUCCESS || n == 0)
            break;
        memcpy(buf + total, p, n * sizeof(float));
        ma_pcm_rb_commit_read(&A.capbuf, n);
        total += (int)n;
    }
    return total;
}

/* ------------------------------------------------------------------------ */
/* Text playback                                                             */
/* ------------------------------------------------------------------------ */

static seq_seg pending;
static int pending_valid;

static void seq_flush_pending(void)
{
    size_t size = sizeof(seq_seg);
    void *p;
    if (!pending_valid)
        return;
    pending_valid = 0;
    if (ma_rb_acquire_write(&A.seq, &size, &p) != MA_SUCCESS || size != sizeof(seq_seg))
        return;
    memcpy(p, &pending, sizeof(seq_seg));
    ma_rb_commit_write(&A.seq, size);
    A.seq_submitted++;
}

/* Queues a segment, merging consecutive segments with the same state. */
static void seq_add(int on, double ms, double rate)
{
    uint32_t samples = (uint32_t)(ms * rate / 1000.0 + 0.5);
    if (samples == 0)
        return;
    if (pending_valid && pending.on == on) {
        pending.samples += samples;
        return;
    }
    seq_flush_pending();
    pending.samples = samples;
    pending.on = (uint16_t)on;
    pending.gen = (uint16_t)A.ui_gen;
    pending_valid = 1;
}

static void seq_emit(void *ctx, int on, double ms)
{
    seq_add(on, ms, *(const double *)ctx);
}

void audio_play_text(const char *text, int wpm, int eff_wpm, double lead_ms)
{
    double rate = A.play_ok ? (double)A.play.sampleRate : 48000.0;

    if (!A.play_ok)
        return; /* nothing would ever consume the queue */
    seq_add(0, lead_ms > 0 ? lead_ms : 1, rate);
    morse_timing(text, wpm, eff_wpm, seq_emit, &rate);
    seq_flush_pending();
}

void audio_stop_playback(void)
{
    pending_valid = 0;
    A.ui_gen = (A.ui_gen + 1) & 0xFFFF;
    at_set(&A.seq_gen, A.ui_gen);
}

int audio_playback_busy(void)
{
    return at_get(&A.seq_consumed) != A.seq_submitted;
}
