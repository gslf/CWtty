/*
 * voices.c - The tones of the remote stations.
 *
 * The oscillators are rotating phasors, so the per-sample cost is a few
 * multiplications (the web build imports sin() from JavaScript, which is
 * too slow to call for eight voices at 48 kHz). The envelope's cosine is
 * only evaluated while it ramps.
 */
#include "voices.h"

#include <math.h>
#include <string.h>

#include "util.h"

#define LIMIT_CEIL 0.9f
#define LIMIT_RELEASE_MS 100.0

void voices_init(voices_t *vs)
{
    int k;
    memset(vs, 0, sizeof *vs);
    vs->volume = 70;
    vs->rise_ms = 5;
    vs->limit = 1.0f;
    for (k = 0; k < VOICES; k++) {
        vs->v[k].tone_hz = 700;
        vs->v[k].c = 1.0;
    }
}

void voices_reset(voices_t *vs)
{
    int k;
    for (k = 0; k < VOICES; k++) {
        voice *v = &vs->v[k];
        v->on = 0;
        v->env = 0.0f;
        v->head = v->count = 0;
        v->c = 1.0;
        v->s = 0.0;
    }
    vs->limit = 1.0f;
}

void voices_tone(voices_t *vs, int k, int tone_hz)
{
    if (k >= 0 && k < VOICES)
        vs->v[k].tone_hz = clampi(tone_hz, 100, 3000);
}

void voices_volume(voices_t *vs, int volume, int rise_ms)
{
    vs->volume = clampi(volume, 0, 100);
    vs->rise_ms = clampi(rise_ms, 1, 20);
}

int voices_push(voices_t *vs, int k, uint64_t t, int on)
{
    voice *v;
    if (k < 0 || k >= VOICES)
        return 0;
    v = &vs->v[k];
    if (v->count == VOICE_QUEUE)
        return 0;
    v->q[(v->head + v->count) % VOICE_QUEUE].t = t;
    v->q[(v->head + v->count) % VOICE_QUEUE].on = on ? 1 : 0;
    v->count++;
    return 1;
}

int voices_busy(const voices_t *vs, int k)
{
    const voice *v;
    if (k < 0 || k >= VOICES)
        return 0;
    v = &vs->v[k];
    return v->on || v->env > 0.0f || v->count > 0;
}

/* Adds one voice to the mix buffer (mono). */
static void render_voice(voice *v, float *mix, int frames, double rate, uint64_t t0, float vol, float step)
{
    int i;
    double c = v->c, s = v->s, n;

    /* Nothing to do: silent, and no event falls in this block. */
    if (!v->on && v->env <= 0.0f && (v->count == 0 || v->q[v->head].t >= t0 + (uint64_t)frames))
        return;

    if (v->rot_hz != v->tone_hz || v->rot_rate != rate) {
        double w = 2.0 * CW_PI * v->tone_hz / rate;
        v->rc = cos(w);
        v->rs = sin(w);
        v->rot_hz = v->tone_hz;
        v->rot_rate = rate;
    }

    for (i = 0; i < frames; i++) {
        uint64_t t = t0 + (uint64_t)i;
        double nc;
        while (v->count > 0 && v->q[v->head].t <= t) {
            v->on = v->q[v->head].on;
            v->head = (v->head + 1) % VOICE_QUEUE;
            v->count--;
        }
        if (v->on) {
            if (v->env < 1.0f)
                v->env = v->env + step < 1.0f ? v->env + step : 1.0f;
        } else if (v->env > 0.0f) {
            v->env = v->env - step > 0.0f ? v->env - step : 0.0f;
        }
        if (v->env > 0.0f) {
            float g = v->env >= 1.0f ? 1.0f : 0.5f - 0.5f * cosf((float)CW_PI * v->env);
            mix[i] += (float)s * g * vol;
        }
        nc = c * v->rc - s * v->rs;
        s = c * v->rs + s * v->rc;
        c = nc;
    }
    /* Keep the phasor on the unit circle (one Newton step is plenty). */
    n = (3.0 - (c * c + s * s)) * 0.5;
    v->c = c * n;
    v->s = s * n;
}

void voices_mix(voices_t *vs, float *out, int frames, int channels, double rate, uint64_t t0)
{
    float mix[1024];
    float vol = (float)vs->volume / 100.0f;
    float step = 1.0f / (float)(rate * vs->rise_ms / 1000.0);
    float release = (float)(1.0 / (rate * LIMIT_RELEASE_MS / 1000.0));
    int done = 0, k, i, c, any;

    vol = vol * vol * 0.7f;
    while (done < frames) {
        int n = mini(frames - done, (int)(sizeof mix / sizeof mix[0]));
        uint64_t t = t0 + (uint64_t)done;
        memset(mix, 0, (size_t)n * sizeof mix[0]);
        any = 0;
        for (k = 0; k < VOICES; k++) {
            if (voices_busy(vs, k)) {
                render_voice(&vs->v[k], mix, n, rate, t, vol, step);
                any = 1;
            }
        }
        if (!any && vs->limit >= 1.0f) {
            done += n;
            continue;
        }
        for (i = 0; i < n; i++) {
            float *frame = out + (size_t)(done + i) * channels;
            float x = fabsf(frame[0] + mix[i]);
            /* Slow release, instant attack: the envelopes already ramp, so
             * the gain changes are smooth enough. */
            if (vs->limit < 1.0f)
                vs->limit = vs->limit + release < 1.0f ? vs->limit + release : 1.0f;
            if (x * vs->limit > LIMIT_CEIL)
                vs->limit = LIMIT_CEIL / x;
            for (c = 0; c < channels; c++)
                frame[c] = (frame[c] + mix[i]) * vs->limit;
        }
        done += n;
    }
}
