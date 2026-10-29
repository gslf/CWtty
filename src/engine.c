/*
 * engine.c - The real-time core of the transmitter.
 */
#include "engine.h"

#include <math.h>
#include <string.h>

#include "util.h"

#define STRAIGHT_MIN_MS 12.0 /* a tap is always audible */

void engine_init(engine_t *e)
{
    memset(e, 0, sizeof *e);
    e->tone_hz = 600;
    e->wpm = 20;
    e->keyer_mode = KEYER_IAMBIC_B;
    e->volume = 70;
    e->rise_ms = 5;
    keyer_init(&e->keyer, KEYER_IAMBIC_B, 2880);
}

int engine_busy(const engine_t *e)
{
    return keyer_busy(&e->keyer) || e->op_state;
}

void engine_render(engine_t *e, const engine_io *io, float *out, int frames, int channels, double rate)
{
    double inc = 2.0 * CW_PI * clampi(e->tone_hz, 100, 3000) / rate;
    int wpm = clampi(e->wpm, 5, 80);
    int mode = clampi(e->keyer_mode, 0, KEYER_MODE_COUNT - 1);
    float vol = (float)clampi(e->volume, 0, 100) / 100.0f;
    float step = 1.0f / (float)(rate * clampi(e->rise_ms, 1, 20) / 1000.0);
    int i, c;

    vol = vol * vol * 0.7f; /* perceptual curve, with headroom */
    keyer_set(&e->keyer, mode, (uint32_t)(rate * 1.2 / wpm));

    /* Taps shorter than a block are still heard: the straight key, the
     * manual dah lever of the bug and the paddles used as straight keys. */
    if (e->straight_edge || (mode == KEYER_BUG && e->dah_edge) ||
        (mode == KEYER_STRAIGHT && (e->dit_edge || e->dah_edge)))
        e->straight_hold = (uint32_t)(rate * STRAIGHT_MIN_MS / 1000.0);

    for (i = 0; i < frames; i++) {
        int k, manual, op, pb;
        float s = 0.0f;

        k = keyer_tick(&e->keyer, e->dit, e->dah, i == 0 && e->dit_edge, i == 0 && e->dah_edge);
        manual = e->straight || e->straight_hold > 0 || (mode == KEYER_BUG && e->dah) ||
                 (mode == KEYER_STRAIGHT && (e->dit || e->dah));
        if (e->straight_hold > 0)
            e->straight_hold--;
        op = k || manual;
        pb = io->playback(io->ctx);

        if (op != e->op_state) {
            io->event(io->ctx, e->clock, op, SRC_OPERATOR);
            e->op_state = op;
        }
        if (pb != e->pb_state) {
            io->event(io->ctx, e->clock, pb, SRC_PLAYBACK);
            e->pb_state = pb;
        }
        if (op || keyer_busy(&e->keyer))
            e->last_active = e->clock;

        e->env = clampf(e->env + (op || pb ? step : -step), 0.0f, 1.0f);
        if (e->env > 0.0f) {
            float g = 0.5f - 0.5f * cosf((float)CW_PI * e->env); /* raised cosine */
            s = (float)sin(e->phase) * g * vol;
        }
        e->phase += inc;
        if (e->phase >= 2.0 * CW_PI)
            e->phase -= 2.0 * CW_PI;
        for (c = 0; c < channels; c++)
            out[i * channels + c] = s;
        e->clock++;
    }
    e->dit_edge = e->dah_edge = e->straight_edge = 0;
}
