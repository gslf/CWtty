/*
 * engine.h - The real-time core of the transmitter, independent of the
 * audio backend: the keyer, the manual contacts (straight key, bug lever,
 * sideswiper), text playback and a click-free sidetone, sample by sample.
 *
 * The backend sets the inputs and calls engine_render() from its audio
 * callback. Playback comes from a per-sample callback, and every key
 * transition goes back through another, timestamped with the sample clock.
 */
#ifndef CWTTY_ENGINE_H
#define CWTTY_ENGINE_H

#include <stdint.h>

#include "keyer.h"

enum { SRC_OPERATOR = 0, SRC_PLAYBACK = 1 };

typedef struct {
    int  (*playback)(void *ctx); /* playback key state for the next sample */
    void (*event)(void *ctx, uint64_t t, int on, int src);
    void *ctx;
} engine_io;

typedef struct {
    /* Inputs, read at the start of every block. */
    int tone_hz, wpm, keyer_mode, volume, rise_ms;
    int dit, dah, straight;                /* contact levels */
    int dit_edge, dah_edge, straight_edge; /* pressed since the last block */

    /* Clocks, in samples. */
    uint64_t clock;
    uint64_t last_active; /* the operator was last keying */

    /* State. */
    keyer_t  keyer;
    uint32_t straight_hold;
    int      op_state, pb_state; /* key down: operator, playback */
    double   phase;
    float    env;
} engine_t;

void engine_init(engine_t *e);

/* Renders frames of interleaved audio and consumes the edges. */
void engine_render(engine_t *e, const engine_io *io, float *out, int frames, int channels, double rate);

/* The operator is keying, or the keyer is finishing an element. */
int engine_busy(const engine_t *e);

#endif
