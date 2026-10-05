/*
 * settings.h - User settings, persisted as a simple key=value file.
 */
#ifndef CWTTY_SETTINGS_H
#define CWTTY_SETTINGS_H

#define SETTINGS_NAME_CAP 128

typedef struct {
    /* Keyer and sidetone */
    int tone_hz;      /* 300..1200 */
    int wpm;          /* character speed, 5..60 */
    int farnsworth;   /* effective speed for playback (0 = off) */
    int keyer_mode;   /* keyer_mode */
    int swap_paddles; /* left arrow = dah */
    int volume;       /* 0..100 */
    int rise_ms;      /* envelope rise time, 1..10 */

    /* Receiver */
    int rx_freq;  /* tuned pitch, Hz */
    int rx_bw;    /* filter bandwidth, Hz */
    int rx_sens;  /* 1..10 */
    int rx_afc;   /* automatic frequency control */
    int rx_wpm;   /* expected (or locked) speed */
    int rx_lock;  /* lock the receive speed */

    /* Transceiver */
    int breakin_ms; /* hang time before returning to receive */
    int trx_sidetone;

    /* Audio */
    int  latency_ms;
    char out_dev[SETTINGS_NAME_CAP];
    char in_dev[SETTINGS_NAME_CAP];

    /* Training */
    int train_dir;
    int train_content;
    int train_charset;
    int train_koch;
    int train_group;
    int train_hint;
    int train_strict; /* require word spacing; 0 allows pauses between letters */
} settings_t;

/* Receiver filter bandwidths offered in the UI. */
extern const int RX_BANDWIDTHS[];
extern const int RX_BANDWIDTH_COUNT;

void settings_defaults(settings_t *s);
void settings_clamp(settings_t *s);
int  settings_load(settings_t *s, const char *path);
int  settings_save(const settings_t *s, const char *path);

/* Index of the closest offered bandwidth. */
int rx_bw_index(int hz);

#endif
