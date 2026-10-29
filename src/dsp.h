/*
 * dsp.h - Receiver signal processing.
 *
 * Pipeline (8 kHz mono input):
 *
 *   input -> quadrature mixer at the tuned pitch -> 4th order Butterworth
 *   low-pass on I and Q (bandwidth/2) -> magnitude at 1 kHz -> smoothing
 *   -> dB -> noise floor / spread / peak tracking -> adaptive threshold
 *   with hysteresis -> deglitch -> key-down / key-up events
 *
 * In parallel a 512-point FFT feeds the waterfall and the automatic
 * frequency control (AFC).
 *
 * Sensitivity (1..10) sets how far above the measured noise spread the
 * threshold sits: high values catch very weak signals, low values reject
 * noise and avoid false characters.
 */
#ifndef CWTTY_DSP_H
#define CWTTY_DSP_H

#include <stdint.h>

#define DSP_RATE 8000
#define DSP_FFT_N 512
#define DSP_BINS (DSP_FFT_N / 2)
#define DSP_BIN_HZ ((float)DSP_RATE / DSP_FFT_N)
#define DSP_WF_ROWS 96
#define DSP_ENV_HIST 8192 /* ms of envelope history */
#define DSP_MAX_EVENTS 256

#define DSP_MIN_HZ 200.0f
#define DSP_MAX_HZ 1500.0f

typedef struct {
    double t_ms;
    int    on;
} dsp_event;

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} biquad;

typedef struct {
    /* configuration */
    float freq;   /* tuned pitch, Hz */
    float bw;     /* filter bandwidth, Hz */
    int   sens;   /* 1..10 */
    int   afc;    /* automatic frequency control */
    float dit_ms; /* speed hint from the decoder (deglitch, smoothing) */

    /* mixer and filters */
    double phase;
    biquad lpi[2], lpq[2];
    int    decim;
    float  smooth;

    /* levels, dB */
    float env_db;
    float nf_db;  /* noise floor: median of the dB envelope in spaces */
    float p10_db; /* 10th percentile of the noise */
    float dev_db; /* noise spread above the median (from the lower tail) */
    float pk_db;  /* mean level of the marks */
    float thr_db; /* detection threshold */
    float snr_db; /* mark level above the noise floor */

    /* detector */
    int      det;  /* envelope above the threshold */
    int      open; /* squelch open: the signal is strong enough */
    int      raw;  /* det && open, before deglitching */
    int      state;
    uint64_t raw_since, state_since;
    uint64_t frames;  /* 1 kHz envelope frames == ms */
    uint64_t samples; /* input samples processed */
    uint64_t guard_until, mute_until;
    int      muted;
    uint64_t last_mark_frame;

    dsp_event ev[DSP_MAX_EVENTS];
    int       nev;

    float env_hist[DSP_ENV_HIST];

    /* spectrum */
    float fft_buf[DSP_FFT_N];
    int   fft_fill;
    float spec_acc[DSP_BINS];
    int   spec_frames;
    float spec[DSP_BINS]; /* latest averaged spectrum, dB */
    float spec_floor;     /* median level in the display range, dB */
    float wf[DSP_WF_ROWS][DSP_BINS];
    int   wf_head, wf_count;
    unsigned wf_serial;   /* increments with every new waterfall row */
} rx_dsp;

void dsp_init(rx_dsp *d, float freq, float bw, int sens);
void dsp_set_freq(rx_dsp *d, float hz);
void dsp_set_bw(rx_dsp *d, float hz);

void dsp_process(rx_dsp *d, const float *x, int n);
int  dsp_take_events(rx_dsp *d, dsp_event *out, int cap);

/* Current time of the receiver clock (ms of processed audio). */
double dsp_time_ms(const rx_dsp *d);

/* Silences the detector (e.g. while transmitting); tail_ms extends the
 * mute after it is released, to let the sidetone echo die out. */
void dsp_mute(rx_dsp *d, int on, double tail_ms);
int  dsp_is_muted(const rx_dsp *d);

/* Strongest spectral peak in [lo, hi] Hz; *snr_db gets its prominence. */
float dsp_find_peak(const rx_dsp *d, float lo, float hi, float *snr_db);

/* Envelope (dB) at a given receiver time, from the history. */
float dsp_env_at(const rx_dsp *d, double t_ms);

/* Waterfall row "age" rows ago (0 = newest), or NULL. */
const float *dsp_wf_row(const rx_dsp *d, int age);

/*
 * Brings audio from a device rate down to DSP_RATE, for backends that
 * cannot capture at it: a 4th order Butterworth anti-alias filter, then
 * linear interpolation.
 */
typedef struct {
    biquad lp[2];
    double step; /* input samples per output sample */
    double pos;  /* where the next output falls after the last input */
    float  last; /* last filtered input */
} dsp_resampler;

void dsp_resampler_init(dsp_resampler *r, double in_rate);
/* Returns the samples written to out: at most n * DSP_RATE / in_rate + 1. */
int  dsp_resample(dsp_resampler *r, const float *in, int n, float *out);

#endif
