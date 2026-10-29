/*
 * worklet.c - The audio thread of the web app (an AudioWorklet): the
 * shared transmitter engine with a text playback queue, and the capture
 * brought down to the receiver's 8 kHz.
 *
 * worklet.js calls these from the audio thread only, between and during
 * render quanta: no locking is needed.
 */
#include <stdint.h>

#include "dsp.h"
#include "engine.h"
#include "morse.h"
#include "util.h"

#define EXPORT __attribute__((visibility("default")))

#define FRAMES_CAP 1024  /* render quantum: 128 frames today */
#define RX_CAP 4096      /* capture at 8 kHz between two reports */
#define EVENTS_CAP 512
#define SEGS_CAP 8192
#define TEXT_CAP 1024

typedef struct {
    uint32_t samples;
    int      on;
} segment;

static engine_t eng;
static double   rate = 48000.0;
static int      enabled = 1;
static int      level[3], edge[3]; /* contacts: dit, dah, straight */

/* Text playback: key-down / key-up segments, the head is playing. */
static segment segs[SEGS_CAP];
static int     seg_head, seg_count;

static double events[3 * EVENTS_CAP]; /* (ms, on, src) */
static int    nevents;

static dsp_resampler resampler;
static float         out[FRAMES_CAP], in[FRAMES_CAP], rx[RX_CAP + FRAMES_CAP + 1];
static int           nrx;
static char          text[TEXT_CAP];

static int playback(void *ctx)
{
    segment *s = &segs[seg_head];
    int on;
    (void)ctx;
    if (seg_count == 0)
        return 0;
    on = s->on;
    if (--s->samples == 0) {
        seg_head = (seg_head + 1) % SEGS_CAP;
        seg_count--;
    }
    return on;
}

static void event(void *ctx, uint64_t t, int on, int src)
{
    (void)ctx;
    if (nevents == EVENTS_CAP)
        return;
    events[3 * nevents] = (double)t * 1000.0 / rate;
    events[3 * nevents + 1] = on;
    events[3 * nevents + 2] = src;
    nevents++;
}

/* Queues a segment, merging it with the last one in the same state. */
static void queue(void *ctx, int on, double ms)
{
    uint32_t samples = (uint32_t)(ms * rate / 1000.0 + 0.5);
    segment *last = &segs[(seg_head + seg_count - 1 + SEGS_CAP) % SEGS_CAP];
    (void)ctx;
    if (samples == 0)
        return;
    if (seg_count > 0 && last->on == on) {
        last->samples += samples;
    } else if (seg_count < SEGS_CAP) {
        segs[(seg_head + seg_count) % SEGS_CAP] = (segment){samples, on};
        seg_count++;
    }
}

EXPORT void wk_init(double sample_rate)
{
    rate = sample_rate;
    engine_init(&eng);
    dsp_resampler_init(&resampler, sample_rate);
}

EXPORT void wk_params(int tone_hz, int wpm, int keyer_mode, int volume, int rise_ms)
{
    eng.tone_hz = tone_hz;
    eng.wpm = wpm;
    eng.keyer_mode = keyer_mode;
    eng.volume = volume;
    eng.rise_ms = rise_ms;
}

/* Contact 0 is the dit paddle, 1 the dah paddle, 2 the straight key. A
 * press is remembered until the next quantum, so a short tap is heard. */
EXPORT void wk_key(int contact, int down)
{
    int c = clampi(contact, 0, 2);
    if (down && !level[c])
        edge[c] = 1;
    level[c] = down;
}

EXPORT void wk_enable(int on)
{
    if (!on)
        level[0] = level[1] = level[2] = 0;
    enabled = on;
}

EXPORT char *wk_text(void)
{
    return text;
}

/* Plays wk_text() after lead_ms of silence. */
EXPORT void wk_play(int wpm, int eff_wpm, double lead_ms)
{
    text[TEXT_CAP - 1] = 0;
    queue(NULL, 0, lead_ms > 0 ? lead_ms : 1);
    morse_timing(text, wpm, eff_wpm, queue, NULL);
}

EXPORT void wk_stop(void)
{
    seg_head = seg_count = 0;
}

EXPORT float *wk_input(void)
{
    return in;
}

EXPORT float *wk_output(void)
{
    return out;
}

/* Renders frames of mono sidetone into wk_output(). */
EXPORT void wk_render(int frames)
{
    static const engine_io io = {playback, event, NULL};
    eng.dit = enabled && level[0];
    eng.dah = enabled && level[1];
    eng.straight = enabled && level[2];
    eng.dit_edge = enabled && edge[0];
    eng.dah_edge = enabled && edge[1];
    eng.straight_edge = enabled && edge[2];
    edge[0] = edge[1] = edge[2] = 0;
    engine_render(&eng, &io, out, clampi(frames, 0, FRAMES_CAP), 1, rate);
}

/* Resamples frames of wk_input() into the receive buffer. */
EXPORT void wk_capture(int frames)
{
    if (nrx <= RX_CAP)
        nrx += dsp_resample(&resampler, in, clampi(frames, 0, FRAMES_CAP), rx + nrx);
}

/* The buffers since the last take, valid until the next quantum. */
EXPORT float *wk_rx(void)
{
    return rx;
}

EXPORT int wk_take_rx(void)
{
    int n = nrx;
    nrx = 0;
    return n;
}

EXPORT double *wk_events(void)
{
    return events;
}

EXPORT int wk_take_events(void)
{
    int n = nevents;
    nevents = 0;
    return n;
}

EXPORT double wk_clock_ms(void)
{
    return (double)eng.clock * 1000.0 / rate;
}

/* The operator is keying: a contact is held or an element is under way. */
EXPORT int wk_busy(void)
{
    return engine_busy(&eng) || (enabled && (level[0] || level[1] || level[2]));
}

EXPORT double wk_idle_ms(void)
{
    return wk_busy() ? 0.0 : (double)(eng.clock - eng.last_active) * 1000.0 / rate;
}

EXPORT int wk_playing(void)
{
    return seg_count > 0;
}
