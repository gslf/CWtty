/*
 * audio.h - Audio engine built on miniaudio.
 *
 * Playback runs the keyer and the text sequencer at sample rate and
 * synthesises a click-free sidetone. Every key transition is reported back
 * to the UI as a timestamped event so it can be decoded and drawn.
 *
 * Capture delivers mono float samples at AUDIO_RX_RATE for the receiver.
 *
 * All functions are called from the UI thread; communication with the
 * real-time callbacks is lock-free (atomics and SPSC ring buffers).
 */
#ifndef CWTTY_AUDIO_H
#define CWTTY_AUDIO_H

#include <stdint.h>

#define AUDIO_RX_RATE 8000
#define AUDIO_NAME_CAP 128

enum { SRC_OPERATOR = 0, SRC_PLAYBACK = 1 };

typedef struct {
    uint64_t t;   /* playback clock, in samples */
    int      on;  /* key down (1) or up (0) */
    int      src; /* SRC_OPERATOR or SRC_PLAYBACK */
} audio_event;

typedef struct {
    int      out_ok;   /* a real output device is running */
    int      out_null; /* playback runs on the silent null backend */
    int      in_ok;    /* the capture device is open and running */
    unsigned out_rate;
    char     out_name[AUDIO_NAME_CAP];
    char     in_name[AUDIO_NAME_CAP];
    char     error[160];
} audio_status;

typedef struct {
    int tone_hz;
    int wpm;
    int keyer_mode;
    int volume;  /* 0..100 */
    int rise_ms; /* envelope rise/fall time */
} audio_params;

int  audio_init(const char *out_dev, const char *in_dev, int latency_ms);
void audio_shutdown(void);
/* Re-opens the devices (e.g. after a change in the settings). */
int  audio_reopen(const char *out_dev, const char *in_dev, int latency_ms);

void audio_get_status(audio_status *st);
void audio_set_params(const audio_params *p);

/* Operator keying. */
void audio_key_dit(int down);
void audio_key_dah(int down);
void audio_key_straight(int down);
void audio_release_keys(void);
void audio_enable_operator(int on);
int  audio_operator_busy(void);
/* Milliseconds since the operator last keyed (0 while keying). */
double audio_operator_idle_ms(void);

/* Text playback (e.g. for receive training). eff_wpm < wpm => Farnsworth. */
void audio_play_text(const char *text, int wpm, int eff_wpm, double lead_ms);
void audio_stop_playback(void);
int  audio_playback_busy(void);

/* Key events and the playback clock. */
int    audio_poll_events(audio_event *ev, int cap);
double audio_clock_ms(void);
double audio_to_ms(uint64_t samples);

/* Capture. */
int  audio_capture_start(void);
void audio_capture_stop(void);
int  audio_capture_read(float *buf, int cap);

/* Device names, "capture" selects inputs. Returns the number found. */
int audio_list_devices(int capture, char (*names)[AUDIO_NAME_CAP], int cap);

#endif
