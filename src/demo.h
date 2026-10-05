/*
 * demo.h - A simulated band for trying the receiver without a radio:
 * stations calling CQ and answering, with noise, fading (QSB) and a weaker
 * station further up the band.
 */
#ifndef CWTTY_DEMO_H
#define CWTTY_DEMO_H

void demo_init(void);
/* Generates n samples at AUDIO_RX_RATE. */
void demo_generate(float *out, int n);

#endif
