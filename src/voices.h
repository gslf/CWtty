/*
 * voices.h - The tones of the remote stations: a small bank of oscillators,
 * each keyed by timestamped events, mixed on top of the sidetone.
 *
 * Every voice has its own pitch and a click-free envelope (the raised
 * cosine of the sidetone). Events carry the sample clock at which the key
 * goes down or up, so a voice plays a remote station's keying with sample
 * accuracy however late its packets were queued, as long as they arrive
 * before their time. Single-threaded: the native audio callback and the web
 * audio worklet each own an instance and feed it from their command queues.
 */
#ifndef CWTTY_VOICES_H
#define CWTTY_VOICES_H

#include <stdint.h>

#define VOICES 8
#define VOICE_QUEUE 256

typedef struct {
    uint64_t t; /* sample clock */
    int      on;
} voice_event;

typedef struct {
    int    tone_hz;
    int    on;
    float  env;
    double c, s;   /* phasor: cos and sin of the phase */
    double rc, rs; /* rotation per sample */
    int    rot_hz; /* the tone the rotation was computed for */
    double rot_rate;

    voice_event q[VOICE_QUEUE];
    int         head, count;
} voice;

typedef struct {
    voice v[VOICES];
    int   volume;  /* 0..100, the same curve as the sidetone */
    int   rise_ms; /* envelope rise and fall */
    float limit;   /* gain of the output limiter */
} voices_t;

void voices_init(voices_t *vs);
/* Silences every voice at once and empties the queues. */
void voices_reset(voices_t *vs);

void voices_tone(voices_t *vs, int k, int tone_hz);
void voices_volume(voices_t *vs, int volume, int rise_ms);

/* Queues a key change of voice k at sample t. Events of a voice must come
 * in time order; one in the past plays at once. Returns 0 if the queue is
 * full. */
int voices_push(voices_t *vs, int k, uint64_t t, int on);

/* Adds the voices to frames of interleaved audio whose first frame is at
 * sample t0, then limits the sum so it never clips. */
void voices_mix(voices_t *vs, float *out, int frames, int channels, double rate, uint64_t t0);

/* The voice is sounding or has queued events. */
int voices_busy(const voices_t *vs, int k);

#endif
