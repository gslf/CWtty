/*
 * keyer.h - Sample-accurate electronic keyer (Iambic A/B, Ultimatic, Bug).
 *
 * keyer_tick() is called once per audio sample by the playback callback,
 * so element and space lengths are exact regardless of UI timing.
 */
#ifndef CWTTY_KEYER_H
#define CWTTY_KEYER_H

#include <stdint.h>

typedef enum {
    KEYER_IAMBIC_A,
    KEYER_IAMBIC_B,
    KEYER_ULTIMATIC,
    KEYER_BUG,
    KEYER_MODE_COUNT
} keyer_mode;

typedef struct {
    int      mode;
    uint32_t dit_len; /* samples */

    int      state;  /* idle / mark / gap */
    int      elem;   /* element being sent: 0 dit, 1 dah */
    uint32_t remain; /* samples left in the current state */

    int dit_mem, dah_mem;       /* paddle memories */
    int dit_mem_sq, dah_mem_sq; /* memory was set while squeezing */
    int prev_dit, prev_dah;     /* paddle levels at the previous tick */
    int last_pressed;           /* most recently pressed paddle (Ultimatic) */
} keyer_t;

const char *keyer_mode_name(int mode);

void keyer_init(keyer_t *k, int mode, uint32_t dit_len);

/* Takes effect from the next element. */
void keyer_set(keyer_t *k, int mode, uint32_t dit_len);

/*
 * Advances one sample. dit/dah are the current paddle levels; the *_edge
 * flags signal presses that happened since the previous tick (a tap can be
 * shorter than an audio period). Returns 1 while the keyer is keying.
 */
int keyer_tick(keyer_t *k, int dit, int dah, int dit_edge, int dah_edge);

int keyer_busy(const keyer_t *k);

void keyer_reset(keyer_t *k);

#endif
