/*
 * dsp.c - Receiver signal processing.
 */
#include "dsp.h"

#include <math.h>
#include <string.h>

#include "util.h"

#define ENV_RATE 1000 /* envelope frames per second */
#define SETTLE_FRAMES 30 /* let the filters settle before measuring */
#define SPEC_AVG 3 /* FFT frames per waterfall row (~96 ms) */
#define RESAMPLE_FC 2000.0f /* passes the CW range, rejects what would alias into it */

/* 4th order Butterworth as two biquads. */
static const float BUTTER4_Q[2] = {0.54119610f, 1.3065630f};

static float fft_win[DSP_FFT_N];
static float fft_cos[DSP_FFT_N / 2], fft_sin[DSP_FFT_N / 2];
static int fft_rev[DSP_FFT_N];
static int fft_ready;

static void fft_setup(void)
{
    int i, bits = 0, n = DSP_FFT_N;
    if (fft_ready)
        return;
    while ((1 << bits) < n)
        bits++;
    for (i = 0; i < n; i++) {
        int r = 0, b;
        for (b = 0; b < bits; b++)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        fft_rev[i] = r;
        fft_win[i] = (float)(0.5 - 0.5 * cos(2.0 * CW_PI * i / (n - 1))); /* Hann */
    }
    for (i = 0; i < n / 2; i++) {
        fft_cos[i] = (float)cos(2.0 * CW_PI * i / n);
        fft_sin[i] = (float)-sin(2.0 * CW_PI * i / n);
    }
    fft_ready = 1;
}

/* In-place iterative radix-2 FFT. */
static void fft(float *re, float *im)
{
    int n = DSP_FFT_N, size, i, j, k;
    for (i = 0; i < n; i++) {
        j = fft_rev[i];
        if (j > i) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (size = 2; size <= n; size <<= 1) {
        int half = size >> 1, step = n / size;
        for (i = 0; i < n; i += size) {
            for (k = 0; k < half; k++) {
                float wr = fft_cos[k * step], wi = fft_sin[k * step];
                int a = i + k, b = i + k + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

static void biquad_lowpass(biquad *f, double rate, float fc, float q)
{
    double w0 = 2.0 * CW_PI * fc / rate;
    double alpha = sin(w0) / (2.0 * q);
    double c = cos(w0);
    double a0 = 1.0 + alpha;
    f->b0 = (float)((1.0 - c) / 2.0 / a0);
    f->b1 = (float)((1.0 - c) / a0);
    f->b2 = f->b0;
    f->a1 = (float)(-2.0 * c / a0);
    f->a2 = (float)((1.0 - alpha) / a0);
}

static inline float biquad_run(biquad *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

void dsp_set_bw(rx_dsp *d, float hz)
{
    float fc;
    int i;
    d->bw = clampf(hz, 20.0f, 1000.0f);
    fc = d->bw * 0.5f;
    for (i = 0; i < 2; i++) {
        float zi1 = d->lpi[i].z1, zi2 = d->lpi[i].z2;
        float zq1 = d->lpq[i].z1, zq2 = d->lpq[i].z2;
        biquad_lowpass(&d->lpi[i], DSP_RATE, fc, BUTTER4_Q[i]);
        biquad_lowpass(&d->lpq[i], DSP_RATE, fc, BUTTER4_Q[i]);
        d->lpi[i].z1 = zi1;
        d->lpi[i].z2 = zi2;
        d->lpq[i].z1 = zq1;
        d->lpq[i].z2 = zq2;
    }
}

void dsp_set_freq(rx_dsp *d, float hz)
{
    d->freq = clampf(hz, DSP_MIN_HZ, DSP_MAX_HZ);
}

void dsp_init(rx_dsp *d, float freq, float bw, int sens)
{
    int i, j;
    fft_setup();
    memset(d, 0, sizeof *d);
    d->sens = sens;
    d->dit_ms = 60.0f;
    d->nf_db = -80.0f;
    d->p10_db = -88.0f;
    d->pk_db = -80.0f;
    d->dev_db = 6.0f;
    d->thr_db = -70.0f;
    d->env_db = -120.0f;
    d->spec_floor = -80.0f;
    dsp_set_freq(d, freq);
    dsp_set_bw(d, bw);
    for (i = 0; i < DSP_ENV_HIST; i++)
        d->env_hist[i] = -120.0f;
    for (i = 0; i < DSP_WF_ROWS; i++)
        for (j = 0; j < DSP_BINS; j++)
            d->wf[i][j] = -120.0f;
    for (i = 0; i < DSP_BINS; i++)
        d->spec[i] = -120.0f;
}

double dsp_time_ms(const rx_dsp *d)
{
    return (double)d->frames;
}

int dsp_is_muted(const rx_dsp *d)
{
    return d->muted || d->frames < d->mute_until;
}

void dsp_mute(rx_dsp *d, int on, double tail_ms)
{
    if (on) {
        d->muted = 1;
    } else if (d->muted) {
        d->muted = 0;
        d->mute_until = d->frames + (uint64_t)(tail_ms > 0 ? tail_ms : 0);
    }
}

static void add_event(rx_dsp *d, uint64_t frame, int on)
{
    if (d->nev >= DSP_MAX_EVENTS) {
        memmove(d->ev, d->ev + 1, sizeof(d->ev[0]) * (DSP_MAX_EVENTS - 1));
        d->nev--;
    }
    d->ev[d->nev].t_ms = (double)frame;
    d->ev[d->nev].on = on;
    d->nev++;
}

int dsp_take_events(rx_dsp *d, dsp_event *out, int cap)
{
    int n = d->nev < cap ? d->nev : cap;
    memcpy(out, d->ev, sizeof(dsp_event) * (size_t)n);
    if (n < d->nev)
        memmove(d->ev, d->ev + n, sizeof(dsp_event) * (size_t)(d->nev - n));
    d->nev -= n;
    return n;
}

/*
 * One envelope frame (1 ms).
 *
 * Noise statistics use stochastic quantile trackers instead of averages:
 * every sample moves them by a fixed step, so a strong signal that slips
 * through the gating cannot drag them far. The floor is the 25th
 * percentile of the dB envelope in spaces, the spread is measured on the
 * lower tail (25th to 10th percentile), which a signal - always above the
 * noise - cannot contaminate.
 *
 * Detection has two stages:
 *   - a threshold halfway (in linear amplitude) between the noise floor and
 *     the mean mark level, never closer to the noise than a margin;
 *   - a squelch: marks are let through only while the mean mark level is
 *     far enough above the noise (the SNR). Sensitivity moves both.
 */
static void process_envelope(rx_dsp *d, float mag)
{
    static const float STEP = 0.12f; /* dB per frame */
    float tau, a, db, mid, hyst, margin, squelch;
    int muted, starting, glitch, sens;

    tau = clampf(d->dit_ms * 0.2f, 1.0f, 30.0f);
    a = 1.0f - expf(-1.0f / tau);
    d->smooth += (mag - d->smooth) * a;
    db = 20.0f * log10f(d->smooth + 1e-6f);
    d->env_db = db;
    d->env_hist[d->frames % DSP_ENV_HIST] = db;

    sens = clampi(d->sens, 1, 10);
    muted = dsp_is_muted(d);
    starting = d->frames < SETTLE_FRAMES;
    if (d->frames == SETTLE_FRAMES) {
        /* Filters have settled: start from the current level. */
        d->nf_db = db;
        d->p10_db = db - 2.0f;
        d->pk_db = db;
    }

    if (!muted && !starting) {
        /* Noise: learn in confirmed spaces only, not right after a mark
         * (the filter is still ringing). A "mark" lasting seconds is a
         * carrier or a noise step: let the floor creep up then. */
        int learn = d->state == 0 && d->det == 0 && d->frames >= d->guard_until;
        float step = STEP;
        if (!learn && d->state == 1 && d->frames - d->state_since > 1500) {
            learn = 1;
            step = 0.02f;
        }
        if (learn) {
            float s = db < d->nf_db - d->dev_db ? step * 4.0f : step; /* fast fall */
            float spread;
            d->nf_db += s * (db >= d->nf_db ? 0.25f : -0.75f);
            d->p10_db += step * (db >= d->p10_db ? 0.1f : -0.9f);
            d->p10_db = clampf(d->p10_db, d->nf_db - 12.0f, d->nf_db - 0.3f);
            /* Smoothed noise envelope: q90 - q25 is about 3x q25 - q10.
             * The shape of the noise changes slowly: average it. */
            spread = 3.0f * (d->nf_db - d->p10_db);
            d->dev_db += (spread - d->dev_db) * 0.003f;
            d->dev_db = clampf(d->dev_db, 1.5f, 15.0f);
        }

        /* Mean level of the marks (not their peaks: noise rides on top). */
        if (d->det)
            d->pk_db += (db - d->pk_db) * (db > d->pk_db ? 0.03f : 0.015f);
        else
            d->pk_db += (d->nf_db - d->pk_db) * 0.00025f; /* ~4 s */
        if (d->pk_db < d->nf_db)
            d->pk_db = d->nf_db;

        margin = (1.3f - 0.07f * (float)sens) * d->dev_db;
        mid = 20.0f * log10f(0.5f * (powf(10.0f, d->nf_db / 20.0f) + powf(10.0f, d->pk_db / 20.0f)));
        d->thr_db = mid > d->nf_db + margin ? mid : d->nf_db + margin;

        /* Squelch with 2 dB of hysteresis: 13.8 dB at sensitivity 1,
         * 3 dB at 10. */
        d->snr_db = d->pk_db - d->nf_db;
        squelch = 15.0f - 1.2f * (float)sens;
        if (d->open)
            d->open = d->snr_db >= squelch - 2.0f;
        else
            d->open = d->snr_db >= squelch;
    }

    /* Threshold crossing with hysteresis. */
    hyst = clampf(0.15f * (d->thr_db - d->nf_db), 0.5f, 3.0f);
    if (muted || starting)
        d->det = 0;
    else if (d->det)
        d->det = db > d->thr_db - hyst * 0.5f;
    else
        d->det = db > d->thr_db + hyst * 0.5f;

    {
        int raw = d->det && d->open;
        if (raw != d->raw) {
            d->raw = raw;
            d->raw_since = d->frames;
        }
    }

    /* Deglitch: a new state must persist for a fraction of a dit. */
    glitch = (int)clampf(d->dit_ms * 0.3f, 4.0f, 20.0f);
    if (muted || !d->open)
        glitch = 0;
    if (d->raw != d->state && d->frames - d->raw_since >= (uint64_t)glitch) {
        d->state = d->raw;
        d->state_since = d->raw_since;
        add_event(d, d->raw_since, d->state);
        if (!d->state) /* the filter keeps ringing for a few time constants */
            d->guard_until = d->frames + (uint64_t)clampf(2500.0f / d->bw, 8.0f, 60.0f);
    }
    if (d->state)
        d->last_mark_frame = d->frames;
    d->frames++;
}

static float median_db(const float *v, int lo, int hi)
{
    /* Small fixed-size selection: copy and partially sort. */
    float tmp[DSP_BINS];
    int n = hi - lo, i, j;
    if (n <= 0)
        return -120.0f;
    memcpy(tmp, v + lo, sizeof(float) * (size_t)n);
    for (i = 0; i <= n / 2; i++) {
        int m = i;
        for (j = i + 1; j < n; j++)
            if (tmp[j] < tmp[m])
                m = j;
        if (m != i) {
            float t = tmp[i];
            tmp[i] = tmp[m];
            tmp[m] = t;
        }
    }
    return tmp[n / 2];
}

float dsp_find_peak(const rx_dsp *d, float lo, float hi, float *snr_db)
{
    int k0 = (int)(clampf(lo, DSP_MIN_HZ, DSP_MAX_HZ) / DSP_BIN_HZ);
    int k1 = (int)(clampf(hi, DSP_MIN_HZ, DSP_MAX_HZ) / DSP_BIN_HZ) + 1;
    int k, best = k0;
    float delta = 0.0f;
    if (k0 < 1)
        k0 = 1;
    if (k1 > DSP_BINS - 2)
        k1 = DSP_BINS - 2;
    for (k = k0; k <= k1; k++)
        if (d->spec[k] > d->spec[best])
            best = k;
    {
        /* Parabolic interpolation for sub-bin accuracy. */
        float a = d->spec[best - 1], b = d->spec[best], c = d->spec[best + 1];
        float den = a - 2.0f * b + c;
        if (fabsf(den) > 1e-6f)
            delta = clampf(0.5f * (a - c) / den, -0.5f, 0.5f);
    }
    if (snr_db)
        *snr_db = d->spec[best] - d->spec_floor;
    return ((float)best + delta) * DSP_BIN_HZ;
}

static void spectrum_frame(rx_dsp *d)
{
    float re[DSP_FFT_N], im[DSP_FFT_N];
    int i;
    for (i = 0; i < DSP_FFT_N; i++) {
        re[i] = d->fft_buf[i] * fft_win[i];
        im[i] = 0.0f;
    }
    fft(re, im);
    for (i = 0; i < DSP_BINS; i++)
        d->spec_acc[i] += re[i] * re[i] + im[i] * im[i];
    if (++d->spec_frames < SPEC_AVG)
        return;

    for (i = 0; i < DSP_BINS; i++) {
        d->spec[i] = 10.0f * log10f(d->spec_acc[i] / SPEC_AVG + 1e-12f);
        d->spec_acc[i] = 0.0f;
    }
    d->spec_frames = 0;
    {
        int lo = (int)(DSP_MIN_HZ / DSP_BIN_HZ), hi = (int)(DSP_MAX_HZ / DSP_BIN_HZ);
        float med = median_db(d->spec, lo, hi);
        d->spec_floor += (med - d->spec_floor) * (d->wf_count == 0 ? 1.0f : 0.15f);
    }
    d->wf_head = (d->wf_head + 1) % DSP_WF_ROWS;
    memcpy(d->wf[d->wf_head], d->spec, sizeof d->spec);
    if (d->wf_count < DSP_WF_ROWS)
        d->wf_count++;
    d->wf_serial++;

    /* AFC: follow the signal while it is being received. */
    if (d->afc && !dsp_is_muted(d) && d->frames - d->last_mark_frame < 400) {
        float snr, pf = dsp_find_peak(d, d->freq - 120.0f, d->freq + 120.0f, &snr);
        if (snr > 10.0f && fabsf(pf - d->freq) > 1.5f)
            dsp_set_freq(d, d->freq + (pf - d->freq) * 0.35f);
    }
}

void dsp_process(rx_dsp *d, const float *x, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        float s = x[i];
        float c = cosf((float)d->phase), sn = sinf((float)d->phase);
        float vi = biquad_run(&d->lpi[1], biquad_run(&d->lpi[0], s * c));
        float vq = biquad_run(&d->lpq[1], biquad_run(&d->lpq[0], -s * sn));

        d->phase += 2.0 * CW_PI * d->freq / DSP_RATE;
        if (d->phase >= 2.0 * CW_PI)
            d->phase -= 2.0 * CW_PI;

        if (++d->decim >= DSP_RATE / ENV_RATE) {
            d->decim = 0;
            process_envelope(d, 2.0f * sqrtf(vi * vi + vq * vq));
        }

        d->fft_buf[d->fft_fill++] = s;
        if (d->fft_fill == DSP_FFT_N) {
            spectrum_frame(d);
            memmove(d->fft_buf, d->fft_buf + DSP_FFT_N / 2, sizeof(float) * DSP_FFT_N / 2);
            d->fft_fill = DSP_FFT_N / 2;
        }
        d->samples++;
    }
}

float dsp_env_at(const rx_dsp *d, double t_ms)
{
    uint64_t t;
    if (t_ms < 0 || t_ms >= (double)d->frames || (double)d->frames - t_ms >= DSP_ENV_HIST)
        return -120.0f;
    t = (uint64_t)t_ms;
    return d->env_hist[t % DSP_ENV_HIST];
}

const float *dsp_wf_row(const rx_dsp *d, int age)
{
    if (age < 0 || age >= d->wf_count)
        return NULL;
    return d->wf[(d->wf_head - age + DSP_WF_ROWS) % DSP_WF_ROWS];
}

void dsp_resampler_init(dsp_resampler *r, double in_rate)
{
    int i;
    memset(r, 0, sizeof *r);
    r->step = in_rate / DSP_RATE;
    for (i = 0; i < 2; i++)
        biquad_lowpass(&r->lp[i], in_rate, (float)clampd(RESAMPLE_FC, 1.0, 0.45 * in_rate), BUTTER4_Q[i]);
}

int dsp_resample(dsp_resampler *r, const float *in, int n, float *out)
{
    int i, k = 0;
    for (i = 0; i < n; i++) {
        float y = biquad_run(&r->lp[1], biquad_run(&r->lp[0], in[i]));
        /* The outputs that fall between the previous input and this one. */
        while (r->pos < 1.0) {
            out[k++] = r->last + (y - r->last) * (float)r->pos;
            r->pos += r->step;
        }
        r->pos -= 1.0;
        r->last = y;
    }
    return k;
}
