/*
 * render.h - Double-buffered cell renderer with 24-bit colour.
 *
 * Everything is drawn into a back buffer of cells; scr_present() diffs it
 * against what is on screen and emits the minimal escape sequences, wrapped
 * in a synchronized update to avoid tearing.
 */
#ifndef CWTTY_RENDER_H
#define CWTTY_RENDER_H

#include <stdint.h>

/* Colours are 0xRRGGBB. COLOR_KEEP leaves the existing colour untouched. */
#define COLOR_KEEP 0xFF000000u

enum { A_NONE = 0, A_BOLD = 1, A_ITALIC = 2, A_UNDERLINE = 4 };

typedef struct {
    uint32_t ch;
    uint32_t fg;
    uint32_t bg;
    uint16_t attr;
} cell_t;

typedef struct {
    int x, y, w, h;
} rect_t;

typedef enum { BOX_ROUND, BOX_LIGHT, BOX_HEAVY, BOX_DOUBLE } box_style;

void scr_init(int truecolor);
void scr_shutdown(void);
void scr_resize(int w, int h);
int  scr_w(void);
int  scr_h(void);

/* Starts a new frame by clearing the back buffer. */
void scr_begin(uint32_t bg);
/* Writes the frame to the terminal. */
void scr_present(void);

/* Restricts drawing to r (intersected with the screen). */
void scr_clip(rect_t r);
void scr_unclip(void);

cell_t *scr_cell(int x, int y); /* NULL if outside the clip */
void    scr_put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int attr);
/* Draws UTF-8 text; returns the number of columns advanced. */
int  scr_text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int attr);
int  scr_textf(int x, int y, uint32_t fg, uint32_t bg, int attr, const char *fmt, ...);
void scr_fill(rect_t r, uint32_t ch, uint32_t fg, uint32_t bg);
void scr_hline(int x, int y, int w, uint32_t ch, uint32_t fg, uint32_t bg);
void scr_vline(int x, int y, int h, uint32_t ch, uint32_t fg, uint32_t bg);
void scr_box(rect_t r, box_style style, uint32_t fg, uint32_t bg);
/* Multiplies colours in r by k (0..1): used for backdrops and shadows. */
void scr_shade(rect_t r, float k);

/* Colour helpers. */
uint32_t rgb_mix(uint32_t a, uint32_t b, float t); /* a..b */
uint32_t rgb_scale(uint32_t c, float k);

static inline rect_t rect_make(int x, int y, int w, int h)
{
    rect_t r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

#endif
