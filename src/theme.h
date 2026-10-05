/*
 * theme.h - The colour palette: deep charcoal surfaces and a warm yellow
 * accent.
 */
#ifndef CWTTY_THEME_H
#define CWTTY_THEME_H

/* Surfaces, from the background up. */
#define TH_BG        0x0B0C0Eu
#define TH_SURFACE   0x111316u
#define TH_SURFACE_2 0x181B1Fu
#define TH_SURFACE_3 0x22262Cu

/* Lines and borders. */
#define TH_LINE    0x2A2F36u
#define TH_LINE_HI 0x3D444Du

/* Text. */
#define TH_FG       0xE8E6E3u
#define TH_FG_MUTED 0x9AA1ABu
#define TH_FG_FAINT 0x5B626Cu
#define TH_FG_GHOST 0x343940u

/* The accent: warm yellow. */
#define TH_ACCENT     0xFFC61Au
#define TH_ACCENT_HI  0xFFE27Au
#define TH_ACCENT_MID 0xB88A00u
#define TH_ACCENT_LO  0x5C4500u
#define TH_ACCENT_BG  0x2A2208u
#define TH_ON_ACCENT  0x15120Au

/* Status. */
#define TH_OK   0x8FD694u
#define TH_WARN 0xFFA94Du
#define TH_ERR  0xFF6B5Eu

/* Received text is cool white, transmitted text is yellow. */
#define TH_RX 0xD9E2EBu
#define TH_TX TH_ACCENT

#endif
