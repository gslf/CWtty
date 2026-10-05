/*
 * bigfont.h - A 5x7 dot-matrix font drawn with half blocks: every glyph is
 * 4 terminal rows tall and 6 columns wide (including spacing).
 */
#ifndef CWTTY_BIGFONT_H
#define CWTTY_BIGFONT_H

#include <stdint.h>

#define BIGFONT_ROWS 4
#define BIGFONT_ADVANCE 6

/* Width in columns of text rendered with the big font (no trailing gap). */
int bigfont_width(const char *text);

/* Draws text; bg may be COLOR_KEEP. Returns the width drawn. */
int bigfont_draw(int x, int y, const char *text, uint32_t fg, uint32_t bg);

#endif
