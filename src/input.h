/*
 * input.h - Keyboard events with press / repeat / release information.
 *
 * Telegraphy needs to know exactly when a key goes down and comes back up,
 * something classic terminals never report. We use, in order of preference:
 *
 *   - the Windows console API (native key-down / key-up records);
 *   - the kitty keyboard protocol (kitty, foot, WezTerm, Ghostty, Alacritty,
 *     iTerm2, ...) with the "report event types" enhancement;
 *   - a legacy fallback that synthesises releases from auto-repeat timing.
 *
 * Mouse buttons are reported too when enabled: USB paddles that emulate the
 * left and right buttons key the transmitter through them. Terminals report
 * mouse presses and releases exactly (SGR mouse mode on POSIX, console
 * records on Windows).
 */
#ifndef CWTTY_INPUT_H
#define CWTTY_INPUT_H

#include <stdint.h>

enum {
    KEY_NONE = 0,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_ESC = 27,
    KEY_SPACE = 32,
    KEY_BACKSPACE = 127,

    KEY_SPECIAL_BASE = 0x110000,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_FOCUS_IN,
    KEY_FOCUS_OUT,
    KEY_MOUSE_LEFT,
    KEY_MOUSE_RIGHT
};

enum {
    MOD_SHIFT = 1,
    MOD_ALT = 2,
    MOD_CTRL = 4,
    MOD_SUPER = 8,
    MOD_TEXT = 16 /* text without press/release information (e.g. pasted) */
};

enum { EV_PRESS = 1, EV_REPEAT = 2, EV_RELEASE = 3 };

typedef struct {
    int      key;  /* KEY_* or a lower-case Unicode codepoint */
    int      mods; /* MOD_* bit set */
    int      type; /* EV_* */
    uint32_t text; /* text produced by the key (0 if none) */
} key_event;

typedef enum { KBD_LEGACY, KBD_KITTY, KBD_WIN32 } kbd_proto;

typedef struct {
    kbd_proto proto;
    int       kitty_flags;
    int       release_text;    /* real release events for text keys (Space) */
    int       release_special; /* real release events for arrows */
    int       mouse;           /* mouse buttons are reported */
} kbd_info;

/* Detects the best keyboard protocol. Call after term_init(). */
void input_init(int force_legacy);

const kbd_info *input_info(void);

/* Collects pending key events, waiting at most timeout_ms for the first. */
int input_poll(key_event *out, int cap, int timeout_ms);

/* Turns the reporting of mouse buttons on or off. */
void input_set_mouse(int on);

/* Legacy fallback: how long a tapped Space is held (one dit). */
void input_set_legacy_dit_ms(double ms);

/* Testing: forces the protocol information and decodes raw terminal bytes
 * as if they had been read (POSIX parser; returns 0 on Windows). */
void input_test_set_info(const kbd_info *ki);
int  input_test_parse(const unsigned char *bytes, int n, key_event *out, int cap);

#endif
