/*
 * demo.c - A simulated band for trying the receiver without a radio.
 */
#include "demo.h"

#include <math.h>
#include <string.h>

#include "audio.h"
#include "morse.h"
#include "util.h"

#define MAX_SEGS 2048

static const char *const TEXTS[] = {
    "CQ CQ CQ DE IK0ABC IK0ABC K",
    "IK0ABC DE W1AW W1AW GM OM UR RST 579 5NN NAME JOHN QTH BOSTON HW? <KN>",
    "W1AW DE IK0ABC R R TNX FER CALL UR RST 599 NAME GIO QTH ROMA <BT> RIG 100W ANT DIPOLE <KN>",
    "IK0ABC DE W1AW FB GIO TNX FER QSO 73 ES GUD DX <SK>",
    "CQ TEST DE DL1XYZ DL1XYZ TEST",
    "QRL? QRL? DE G4ABC",
    "CQ DX CQ DX DE JA1ZZZ JA1ZZZ K",
};

typedef struct {
    float ms;
    int   on;
} seg_t;

typedef struct {
    seg_t  segs[MAX_SEGS];
    int    nsegs, idx;
    double left;  /* samples left in the current segment */
    double phase, env;
    float  pitch, amp, wpm;
    int    on;
} station;

static station main_st, weak_st;
static double t_samples;
static int text_idx;

static double gauss(void)
{
    double u1 = ((double)rng_u32() + 1.0) / 4294967297.0;
    double u2 = ((double)rng_u32() + 1.0) / 4294967297.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * CW_PI * u2);
}

static void add(station *s, int on, double ms)
{
    if (s->nsegs < MAX_SEGS) {
        s->segs[s->nsegs].on = on;
        s->segs[s->nsegs].ms = (float)ms;
        s->nsegs++;
    }
}

/* Builds a transmission with a little human timing jitter. */
static void load(station *s, const char *text, double wpm, double pause_ms)
{
    double dit = 1200.0 / wpm;
    char tok[16];
    int n, first = 1;
    s->nsegs = 0;
    s->idx = 0;
    s->left = 0;
    add(s, 0, pause_ms);
    while ((n = morse_next_token(text, tok, sizeof tok)) > 0) {
        const morse_entry *e;
        const char *c;
        text += n;
        if (tok[0] == ' ') {
            add(s, 0, 4.0 * dit);
            continue;
        }
        e = morse_lookup(tok);
        if (!e)
            continue;
        if (!first)
            add(s, 0, 2.0 * dit);
        first = 0;
        for (c = e->code; *c; c++) {
            double j = 1.0 + 0.08 * gauss();
            add(s, 1, (*c == '.' ? dit : 3.0 * dit) * j);
            add(s, 0, dit * (1.0 + 0.08 * gauss()));
        }
    }
}

static void next_main(void)
{
    main_st.wpm = (float)(16 + rng_int(13));
    main_st.pitch = 640.0f + (float)rng_int(30);
    load(&main_st, TEXTS[text_idx], main_st.wpm, 1200.0 + rng_int(1500));
    text_idx = (text_idx + 1) % ARRAY_LEN(TEXTS);
}

static void next_weak(void)
{
    weak_st.wpm = (float)(12 + rng_int(8));
    weak_st.pitch = 930.0f + (float)rng_int(40);
    load(&weak_st, "CQ CQ DE EA3QRP EA3QRP K", weak_st.wpm, 3000.0 + rng_int(6000));
}

void demo_init(void)
{
    memset(&main_st, 0, sizeof main_st);
    memset(&weak_st, 0, sizeof weak_st);
    main_st.amp = 1.0f;
    weak_st.amp = 0.25f;
    text_idx = 0;
    t_samples = 0;
    next_main();
    next_weak();
}

static float station_sample(station *s)
{
    const double rate = AUDIO_RX_RATE, step = 1.0 / (0.005 * rate);
    float v;
    while (s->left <= 0) {
        if (s->idx >= s->nsegs)
            return 0.0f;
        s->on = s->segs[s->idx].on;
        s->left += s->segs[s->idx].ms * rate / 1000.0;
        s->idx++;
    }
    s->left -= 1.0;
    s->env = clampd(s->env + (s->on ? step : -step), 0.0, 1.0);
    v = (float)(sin(s->phase) * (0.5 - 0.5 * cos(CW_PI * s->env)));
    s->phase += 2.0 * CW_PI * s->pitch / rate;
    if (s->phase > 2.0 * CW_PI)
        s->phase -= 2.0 * CW_PI;
    return v;
}

void demo_generate(float *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        double t = t_samples / AUDIO_RX_RATE;
        /* Slow fading on the main station. */
        double qsb = 0.75 + 0.25 * sin(2.0 * CW_PI * t / 9.0);
        float v = (float)(main_st.amp * qsb) * station_sample(&main_st);
        v += weak_st.amp * station_sample(&weak_st);
        v += (float)(0.55 * gauss());
        out[i] = v * 0.1f;
        if (main_st.idx >= main_st.nsegs && main_st.left <= 0)
            next_main();
        if (weak_st.idx >= weak_st.nsegs && weak_st.left <= 0)
            next_weak();
        t_samples += 1.0;
    }
}
