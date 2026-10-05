/*
 * term.h - Terminal platform layer.
 *
 * Puts the terminal in raw mode, switches to the alternate screen, reports
 * the window size and restores everything on exit (including on fatal
 * signals). Raw input is read here; decoding it into key events is the job
 * of input.c.
 */
#ifndef CWTTY_TERM_H
#define CWTTY_TERM_H

#include <stddef.h>

int  term_init(void);
void term_shutdown(void);

void term_get_size(int *w, int *h);

/* Returns 1 (once) when the window was resized since the last call. */
int term_take_resize(void);

/* Returns 1 when the OS asked us to quit (SIGTERM, console close...). */
int term_quit_requested(void);

/* Writes raw bytes to the terminal. */
void term_write(const char *buf, size_t len);
void term_write_str(const char *s);

/* Tells the shutdown code to pop the kitty keyboard flags we pushed. */
void term_set_kitty_pushed(int pushed);

#ifndef _WIN32
/* Reads up to cap bytes, waiting at most timeout_ms. Returns bytes read. */
int term_read(unsigned char *buf, int cap, int timeout_ms);
#else
/* Native console input handle (a HANDLE) used by input.c. */
void *term_input_handle(void);
void  term_flag_resize(void);
#endif

#endif
