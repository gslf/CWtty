/*
 * ui.h - Reusable widgets drawn with the cell renderer.
 */
#ifndef CWTTY_UI_H
#define CWTTY_UI_H

#include "dsp.h"
#include "render.h"
#include "transcript.h"

/* ------------------------------------------------------------------------ */
/* Key timeline: transitions over time, drawn as a scrolling tape.           */
/* ------------------------------------------------------------------------ */
#define TL_CAP 1024

typedef struct {
    double        t[TL_CAP];
    unsigned char on[TL_CAP];
    int           head, count;
} timeline;

void tl_push(timeline *tl, double t_ms, int on);
int  tl_state(const timeline *tl);

/* ------------------------------------------------------------------------ */
/* Layout and chrome                                                         */
/* ------------------------------------------------------------------------ */

rect_t ui_center(int w, int h);

/* Rounded panel with a title on the top border and optional info on the
 * right. The interior is filled. */
void ui_panel(rect_t r, const char *title, const char *info, int highlight);

/* Modal window: drop shadow, raised body and a header bar. Returns the
 * body rectangle (inside the borders, below the header). */
rect_t ui_modal(rect_t r, const char *title, const char *hint);

/* Darkens everything drawn so far (modal backdrop). */
void ui_backdrop(float k);

/* A key chip like " C-x " followed by a muted label. Returns the width. */
int ui_hint(int x, int y, const char *key, const char *label, uint32_t bg);

/* A keycap with a label inside, lit when active (3 rows tall). */
void ui_keycap(rect_t r, const char *label, int active);

/* Horizontal bar with 1/8 cell resolution. */
void ui_hbar(int x, int y, int w, double frac, uint32_t fg, uint32_t bg);

/* Element pattern as timing bars: dit "━", dah "━━━", gaps " ". */
int ui_pattern_width(const char *code);
int ui_pattern(int x, int y, const char *code, uint32_t fg, uint32_t bg);

/* Scrolling key tape with 1/8 cell horizontal resolution. */
void ui_ribbon(rect_t r, const timeline *tl, double now_ms, double ms_per_col, uint32_t on, uint32_t bg);

/* Envelope sparkline (one row) from the receiver history. */
void ui_sparkline(rect_t r, const rx_dsp *d, double ms_per_col, uint32_t bg);

/* Waterfall between fmin and fmax, newest line on top. */
void ui_waterfall(rect_t r, const rx_dsp *d, float fmin, float fmax);

/* Frequency axis with the receiver passband highlighted (2 rows). */
void ui_freq_axis(rect_t r, float fmin, float fmax, float tuned, float bw, uint32_t bg);

/* Word-wrapped transcript, bottom aligned. labels: RX/TX gutter. */
void ui_transcript(rect_t r, const transcript *t, int labels, int cursor_dir, int cursor_on);

#endif
