/*
 * input.c - Keyboard decoding: kitty protocol, legacy escape sequences and
 * Windows console records, all mapped to key_event.
 */
#include "input.h"

#include <string.h>

#include "platform.h"
#include "term.h"
#include "util.h"

#define QUEUE_CAP 256

static kbd_info info;
static key_event queue[QUEUE_CAP];
static int q_len;

static void push_event(int key, int mods, int type, uint32_t text);

static int take_events(key_event *out, int cap)
{
    int n;
    if (cap <= 0)
        return 0;
    n = mini(q_len, cap);
    memcpy(out, queue, (size_t)n * sizeof *out);
    q_len -= n;
    memmove(queue, queue + n, (size_t)q_len * sizeof *queue);
    return n;
}

const kbd_info *input_info(void)
{
    return &info;
}

void input_test_set_info(const kbd_info *ki)
{
    info = *ki;
}

/* ------------------------------------------------------------------------ */
/* Legacy release emulation                                                  */
/* ------------------------------------------------------------------------ */
/*
 * Without release events, a held key only produces auto-repeat presses. We
 * consider a key held while presses keep arriving and synthesise a release
 * when they stop. A single tap is held for a short time: one dit for the
 * straight key (Space), a few ms for the paddles (the keyer latches taps).
 */
enum { EMU_SPACE, EMU_LEFT, EMU_RIGHT, EMU_COUNT };

static struct {
    int    held;
    int    repeating;
    double last;
} emu[EMU_COUNT];

static double legacy_dit_ms = 60.0;

#define EMU_REPEAT_HOLD_MS 110.0
#define EMU_PADDLE_TAP_MS  30.0

void input_set_legacy_dit_ms(double ms)
{
    legacy_dit_ms = clampd(ms, 20.0, 250.0);
}

static int emu_slot(int key, int mods)
{
    if (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER | MOD_TEXT))
        return -1;
    if (key == KEY_SPACE && !info.release_text)
        return EMU_SPACE;
    if (key == KEY_LEFT && !info.release_special)
        return EMU_LEFT;
    if (key == KEY_RIGHT && !info.release_special)
        return EMU_RIGHT;
    return -1;
}

#ifndef _WIN32
static void emu_tick(double now)
{
    static const int keys[EMU_COUNT] = {KEY_SPACE, KEY_LEFT, KEY_RIGHT};
    int i;
    for (i = 0; i < EMU_COUNT; i++) {
        double limit;
        if (!emu[i].held)
            continue;
        if (emu[i].repeating)
            limit = EMU_REPEAT_HOLD_MS;
        else
            limit = i == EMU_SPACE ? legacy_dit_ms : EMU_PADDLE_TAP_MS;
        if (now - emu[i].last >= limit) {
            emu[i].held = 0;
            emu[i].repeating = 0;
            push_event(keys[i], 0, EV_RELEASE, 0);
        }
    }
}

static double emu_next_deadline(double now)
{
    double best = 1e30;
    int i;
    for (i = 0; i < EMU_COUNT; i++) {
        double limit, left;
        if (!emu[i].held)
            continue;
        limit = emu[i].repeating ? EMU_REPEAT_HOLD_MS
                                 : (i == EMU_SPACE ? legacy_dit_ms : EMU_PADDLE_TAP_MS);
        left = emu[i].last + limit - now;
        if (left < best)
            best = left;
    }
    return best;
}
#endif

static void push_event(int key, int mods, int type, uint32_t text)
{
    key_event *e;
    if (key == KEY_NONE || q_len >= QUEUE_CAP)
        return;
    if (type != EV_RELEASE) {
        int slot = emu_slot(key, mods);
        if (slot >= 0) {
            if (emu[slot].held) {
                type = EV_REPEAT;
                emu[slot].repeating = 1;
            } else {
                type = EV_PRESS;
                emu[slot].held = 1;
                emu[slot].repeating = 0;
            }
            emu[slot].last = plat_time_ms();
        }
    }
    e = &queue[q_len++];
    e->key = key;
    e->mods = mods;
    e->type = type;
    e->text = type == EV_RELEASE ? 0 : text;
}

#ifdef _WIN32
/* ======================================================================== */
/* Windows console records                                                  */
/* ======================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static unsigned char vk_down[256];
static int vk_key[256];

static int map_vk(WORD vk, WCHAR ch)
{
    switch (vk) {
    case VK_LEFT: return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_UP: return KEY_UP;
    case VK_DOWN: return KEY_DOWN;
    case VK_HOME: return KEY_HOME;
    case VK_END: return KEY_END;
    case VK_PRIOR: return KEY_PGUP;
    case VK_NEXT: return KEY_PGDN;
    case VK_INSERT: return KEY_INSERT;
    case VK_DELETE: return KEY_DELETE;
    case VK_RETURN: return KEY_ENTER;
    case VK_ESCAPE: return KEY_ESC;
    case VK_TAB: return KEY_TAB;
    case VK_BACK: return KEY_BACKSPACE;
    case VK_SPACE: return KEY_SPACE;
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_CAPITAL:
    case VK_LWIN: case VK_RWIN: case VK_NUMLOCK: case VK_SCROLL:
        return KEY_NONE;
    default:
        break;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
        return KEY_F1 + (vk - VK_F1);
    if (vk >= 'A' && vk <= 'Z')
        return 'a' + (vk - 'A');
    if (vk >= '0' && vk <= '9')
        return vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return '0' + (vk - VK_NUMPAD0);
    if (ch >= 0x20) {
        if (ch >= 'A' && ch <= 'Z')
            return ch + 32;
        return ch;
    }
    return KEY_NONE;
}

static void translate_key(const KEY_EVENT_RECORD *k)
{
    WORD vk = k->wVirtualKeyCode;
    WCHAR ch = k->uChar.UnicodeChar;
    DWORD cks = k->dwControlKeyState;
    int mods = 0, key, type;
    uint32_t text = 0;
    int ctrl = (cks & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int alt = (cks & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    if (cks & SHIFT_PRESSED)
        mods |= MOD_SHIFT;
    /* AltGr arrives as Ctrl+Alt together with a printable character. */
    if (ctrl && alt && ch >= 0x20) {
        ctrl = alt = 0;
    }
    if (ctrl)
        mods |= MOD_CTRL;
    if (alt)
        mods |= MOD_ALT;

    if (vk >= 256)
        return;
    if (k->bKeyDown) {
        key = map_vk(vk, ch);
        if (key == KEY_NONE)
            return;
        type = vk_down[vk] ? EV_REPEAT : EV_PRESS;
        vk_down[vk] = 1;
        vk_key[vk] = key;
        if (ch >= 0x20 && !ctrl)
            text = ch;
    } else {
        if (!vk_down[vk])
            return;
        vk_down[vk] = 0;
        key = vk_key[vk];
        type = EV_RELEASE;
    }
    push_event(key, mods, type, text);
}

void input_init(int force_legacy)
{
    (void)force_legacy;
    memset(&info, 0, sizeof info);
    info.proto = KBD_WIN32;
    info.release_text = 1;
    info.release_special = 1;
}

int input_test_parse(const unsigned char *bytes, int n, key_event *out, int cap)
{
    (void)bytes;
    (void)n;
    (void)out;
    (void)cap;
    return 0;
}

int input_poll(key_event *out, int cap, int timeout_ms)
{
    HANDLE h = (HANDLE)term_input_handle();
    INPUT_RECORD recs[64];
    DWORD avail = 0, nread = 0, i;
    if (cap <= 0)
        return 0;
    if (q_len > 0)
        return take_events(out, cap);
    if (WaitForSingleObject(h, (DWORD)(timeout_ms < 0 ? 0 : timeout_ms)) == WAIT_OBJECT_0) {
        if (GetNumberOfConsoleInputEvents(h, &avail) && avail > 0 &&
            ReadConsoleInputW(h, recs, 64, &nread)) {
            for (i = 0; i < nread; i++) {
                switch (recs[i].EventType) {
                case KEY_EVENT:
                    translate_key(&recs[i].Event.KeyEvent);
                    break;
                case FOCUS_EVENT:
                    push_event(recs[i].Event.FocusEvent.bSetFocus ? KEY_FOCUS_IN : KEY_FOCUS_OUT, 0,
                               EV_PRESS, 0);
                    if (!recs[i].Event.FocusEvent.bSetFocus)
                        memset(vk_down, 0, sizeof vk_down);
                    break;
                case WINDOW_BUFFER_SIZE_EVENT:
                    term_flag_resize();
                    break;
                default:
                    break;
                }
            }
        }
    }
    return take_events(out, cap);
}

#else
/* ======================================================================== */
/* POSIX: escape sequence parser                                            */
/* ======================================================================== */

#define IBUF_CAP 1024
#define ESC_TIMEOUT_MS 30.0

static unsigned char ibuf[IBUF_CAP];
static int ilen;
static double last_rx;

/* Replies to our capability queries. */
static int seen_kitty, seen_da;

#define MAX_FIELDS 4
#define MAX_SUBS 4

typedef struct {
    int priv; /* private marker: '<' '=' '>' '?' or 0 */
    int final;
    int nfields;
    int f[MAX_FIELDS][MAX_SUBS]; /* -1 when absent */
} csi_t;

static int field(const csi_t *c, int i, int sub, int def)
{
    if (i >= c->nfields || c->f[i][sub] < 0)
        return def;
    return c->f[i][sub];
}

static int map_kitty_code(int code)
{
    switch (code) {
    case 8:
    case 127: return KEY_BACKSPACE;
    case 9: return KEY_TAB;
    case 13: return KEY_ENTER;
    case 27: return KEY_ESC;
    case 57409: return '.';
    case 57410: return '/';
    case 57411: return '*';
    case 57412: return '-';
    case 57413: return '+';
    case 57414: return KEY_ENTER;
    case 57415: return '=';
    case 57416: return ',';
    case 57417: return KEY_LEFT;
    case 57418: return KEY_RIGHT;
    case 57419: return KEY_UP;
    case 57420: return KEY_DOWN;
    case 57421: return KEY_PGUP;
    case 57422: return KEY_PGDN;
    case 57423: return KEY_HOME;
    case 57424: return KEY_END;
    case 57425: return KEY_INSERT;
    case 57426: return KEY_DELETE;
    default: break;
    }
    if (code >= 57399 && code <= 57408)
        return '0' + (code - 57399);
    /* Other private-use functional keys: modifiers, media, F13+... */
    if (code >= 0xE000 && code <= 0xF8FF)
        return KEY_NONE;
    if (code < 0x20)
        return KEY_NONE;
    if (code >= 'A' && code <= 'Z')
        return code + 32;
    return code;
}

static void handle_csi(const csi_t *c)
{
    int mods = field(c, 1, 0, 1) - 1;
    int type = field(c, 1, 1, EV_PRESS);
    int key = KEY_NONE;

    if (mods < 0)
        mods = 0;
    mods &= (MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_SUPER);
    if (type < EV_PRESS || type > EV_RELEASE)
        type = EV_PRESS;

    if (c->priv == '?') {
        if (c->final == 'u') {
            seen_kitty = 1;
            info.kitty_flags = field(c, 0, 0, 0);
        } else if (c->final == 'c') {
            seen_da = 1;
        }
        return;
    }
    if (c->priv)
        return;

    switch (c->final) {
    case 'u': {
        int code = field(c, 0, 0, 0);
        int shifted = field(c, 0, 1, -1);
        int textcp = field(c, 2, 0, -1);
        uint32_t text = 0;
        key = map_kitty_code(code);
        if (key == KEY_NONE)
            return;
        if (type != EV_RELEASE && !(mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
            if (textcp > 0)
                text = (uint32_t)textcp;
            else if ((mods & MOD_SHIFT) && shifted > 0)
                text = (uint32_t)shifted;
            else if (key >= 0x20 && key < KEY_SPECIAL_BASE && key != KEY_BACKSPACE)
                text = (uint32_t)((mods & MOD_SHIFT) && key >= 'a' && key <= 'z' ? key - 32 : key);
        }
        push_event(key, mods, type, text);
        return;
    }
    case '~':
        switch (field(c, 0, 0, 0)) {
        case 1: case 7: key = KEY_HOME; break;
        case 2: key = KEY_INSERT; break;
        case 3: key = KEY_DELETE; break;
        case 4: case 8: key = KEY_END; break;
        case 5: key = KEY_PGUP; break;
        case 6: key = KEY_PGDN; break;
        case 11: key = KEY_F1; break;
        case 12: key = KEY_F2; break;
        case 13: key = KEY_F3; break;
        case 14: key = KEY_F4; break;
        case 15: key = KEY_F5; break;
        case 17: key = KEY_F6; break;
        case 18: key = KEY_F7; break;
        case 19: key = KEY_F8; break;
        case 20: key = KEY_F9; break;
        case 21: key = KEY_F10; break;
        case 23: key = KEY_F11; break;
        case 24: key = KEY_F12; break;
        default: return; /* bracketed paste markers and friends */
        }
        break;
    case 'A': key = KEY_UP; break;
    case 'B': key = KEY_DOWN; break;
    case 'C': key = KEY_RIGHT; break;
    case 'D': key = KEY_LEFT; break;
    case 'H': key = KEY_HOME; break;
    case 'F': key = KEY_END; break;
    case 'P': key = KEY_F1; break;
    case 'Q': key = KEY_F2; break;
    case 'R': key = KEY_F3; break;
    case 'S': key = KEY_F4; break;
    case 'Z':
        key = KEY_TAB;
        mods |= MOD_SHIFT;
        break;
    case 'I':
        if (c->nfields == 0)
            key = KEY_FOCUS_IN;
        break;
    case 'O':
        if (c->nfields == 0)
            key = KEY_FOCUS_OUT;
        break;
    default:
        return;
    }
    push_event(key, mods, type, 0);
}

/*
 * Parses "ESC [ ..." at p. Returns bytes consumed, 0 if the sequence is not
 * complete yet, or a negative count of garbage bytes to drop.
 */
static int parse_csi(const unsigned char *p, int n)
{
    csi_t c;
    int i = 2, sub = 0, val = -1;

    memset(&c, 0, sizeof c);
    memset(c.f, 0xFF, sizeof c.f); /* all -1 */
    if (i < n && p[i] >= '<' && p[i] <= '?')
        c.priv = p[i++];
    for (; i < n; i++) {
        unsigned char b = p[i];
        if (b >= '0' && b <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (b - '0');
            if (val > 10000000)
                val = 10000000;
        } else if (b == ':' || b == ';') {
            if (c.nfields == 0)
                c.nfields = 1;
            if (c.nfields <= MAX_FIELDS && sub < MAX_SUBS)
                c.f[c.nfields - 1][sub] = val;
            val = -1;
            if (b == ':') {
                sub++;
            } else {
                sub = 0;
                c.nfields++;
            }
        } else if (b >= 0x20 && b <= 0x2F) {
            /* intermediate byte: ignored */
        } else if (b >= 0x40 && b <= 0x7E) {
            if (val >= 0 || c.nfields > 0) {
                if (c.nfields == 0)
                    c.nfields = 1;
                if (c.nfields <= MAX_FIELDS && sub < MAX_SUBS)
                    c.f[c.nfields - 1][sub] = val;
            }
            if (c.nfields > MAX_FIELDS)
                c.nfields = MAX_FIELDS;
            c.final = b;
            handle_csi(&c);
            return i + 1;
        } else {
            return -i; /* malformed: drop what we have */
        }
        if (i > 96)
            return -i;
    }
    return 0;
}

static int utf8_seq_len(unsigned char b)
{
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Decodes a plain (non-escape) key. Returns bytes used or 0 if incomplete. */
static int parse_plain(const unsigned char *p, int n, int mods)
{
    unsigned char c = p[0];
    int len;
    uint32_t cp;
    char tmp[5];

    if (c == 0x0D || c == 0x0A) {
        push_event(KEY_ENTER, mods, EV_PRESS, 0);
        return 1;
    }
    if (c == 0x09) {
        push_event(KEY_TAB, mods, EV_PRESS, 0);
        return 1;
    }
    if (c == 0x7F || c == 0x08) {
        push_event(KEY_BACKSPACE, mods, EV_PRESS, 0);
        return 1;
    }
    if (c == 0x00) {
        push_event(KEY_SPACE, mods | MOD_CTRL, EV_PRESS, 0);
        return 1;
    }
    if (c < 0x1B) {
        push_event('a' + c - 1, mods | MOD_CTRL, EV_PRESS, 0);
        return 1;
    }
    if (c >= 0x1C && c < 0x20) {
        push_event("\\]^_"[c - 0x1C], mods | MOD_CTRL, EV_PRESS, 0);
        return 1;
    }
    /* With "all keys as escape codes" active, plain bytes can only be
     * pasted text: there will be no release for them. */
    if (info.proto == KBD_KITTY && (info.kitty_flags & 8))
        mods |= MOD_TEXT;
    len = utf8_seq_len(c);
    if (len > n)
        return 0;
    memcpy(tmp, p, (size_t)len);
    tmp[len] = 0;
    len = utf8_decode(tmp, &cp);
    if (cp >= 'A' && cp <= 'Z')
        push_event((int)cp + 32, mods | MOD_SHIFT, EV_PRESS, (mods & MOD_ALT) ? 0 : cp);
    else
        push_event((int)cp, mods, EV_PRESS, (mods & MOD_ALT) ? 0 : cp);
    return len;
}

static void parse_buffer(int flush)
{
    int i = 0;
    /* Leave room for the emulated releases and retain unparsed bytes. */
    while (i < ilen && q_len < QUEUE_CAP - EMU_COUNT) {
        unsigned char c = ibuf[i];
        int used;
        if (c == 0x1B) {
            if (i + 1 >= ilen) {
                if (!flush)
                    break;
                push_event(KEY_ESC, 0, EV_PRESS, 0);
                i++;
                continue;
            }
            if (ibuf[i + 1] == '[') {
                used = parse_csi(ibuf + i, ilen - i);
                if (used == 0) {
                    if (!flush)
                        break;
                    used = ilen - i; /* stale partial sequence: drop it */
                }
                i += used < 0 ? -used : used;
                continue;
            }
            if (ibuf[i + 1] == 'O') {
                int key = KEY_NONE;
                if (i + 2 >= ilen) {
                    if (!flush)
                        break;
                    push_event('o', MOD_ALT, EV_PRESS, 0);
                    i += 2;
                    continue;
                }
                switch (ibuf[i + 2]) {
                case 'A': key = KEY_UP; break;
                case 'B': key = KEY_DOWN; break;
                case 'C': key = KEY_RIGHT; break;
                case 'D': key = KEY_LEFT; break;
                case 'H': key = KEY_HOME; break;
                case 'F': key = KEY_END; break;
                case 'M': key = KEY_ENTER; break;
                case 'P': key = KEY_F1; break;
                case 'Q': key = KEY_F2; break;
                case 'R': key = KEY_F3; break;
                case 'S': key = KEY_F4; break;
                default: break;
                }
                push_event(key, 0, EV_PRESS, 0);
                i += 3;
                continue;
            }
            if (ibuf[i + 1] == 0x1B) {
                push_event(KEY_ESC, 0, EV_PRESS, 0);
                i++;
                continue;
            }
            used = parse_plain(ibuf + i + 1, ilen - i - 1, MOD_ALT);
            if (used == 0)
                break;
            i += 1 + used;
            continue;
        }
        used = parse_plain(ibuf + i, ilen - i, 0);
        if (used == 0) {
            if (!flush)
                break;
            used = ilen - i;
        }
        i += used;
    }
    if (i > 0) {
        memmove(ibuf, ibuf + i, (size_t)(ilen - i));
        ilen -= i;
    }
}

static void read_some(int timeout_ms)
{
    int n;
    if (ilen >= IBUF_CAP)
        ilen = 0; /* should never happen; avoid wedging */
    n = term_read(ibuf + ilen, IBUF_CAP - ilen, timeout_ms);
    if (n > 0) {
        ilen += n;
        last_rx = plat_time_ms();
    }
}

static int wait_for_da(double timeout_ms)
{
    double t0 = plat_time_ms();
    seen_da = 0;
    while (!seen_da && plat_time_ms() - t0 < timeout_ms) {
        read_some(20);
        parse_buffer(0);
    }
    q_len = 0; /* anything typed meanwhile is discarded */
    return seen_da;
}

void input_init(int force_legacy)
{
    memset(&info, 0, sizeof info);
    info.proto = KBD_LEGACY;
    if (force_legacy)
        return;

    /* Ask for kitty keyboard flags, then primary device attributes as a
     * sentinel every terminal answers. */
    seen_kitty = 0;
    term_write_str("\x1b[?u\x1b[c");
    wait_for_da(700.0);
    if (!seen_kitty)
        return;

    /* Disambiguate (1) + event types (2) + alternate keys (4) + all keys as
     * escapes (8) + associated text (16). Then read back what stuck. */
    term_write_str("\x1b[>31u");
    term_set_kitty_pushed(1);
    seen_kitty = 0;
    term_write_str("\x1b[?u\x1b[c");
    wait_for_da(700.0);

    info.proto = KBD_KITTY;
    info.release_special = (info.kitty_flags & 2) != 0;
    info.release_text = (info.kitty_flags & 2) && (info.kitty_flags & 8);
}

int input_test_parse(const unsigned char *bytes, int n, key_event *out, int cap)
{
    q_len = 0;
    ilen = 0;
    memset(emu, 0, sizeof emu);
    if (n < 0)
        return 0;
    if (n > IBUF_CAP)
        n = IBUF_CAP;
    memcpy(ibuf, bytes, (size_t)n);
    ilen = n;
    parse_buffer(1);
    return take_events(out, cap);
}

int input_poll(key_event *out, int cap, int timeout_ms)
{
    double now = plat_time_ms();
    double emu_left = emu_next_deadline(now);
    if (cap <= 0)
        return 0;
    if (q_len > 0)
        return take_events(out, cap);
    if (ilen > 0) {
        parse_buffer(now - last_rx >= ESC_TIMEOUT_MS);
        if (q_len > 0) {
            emu_tick(now);
            return take_events(out, cap);
        }
    }

    if (ilen > 0 && ESC_TIMEOUT_MS < timeout_ms)
        timeout_ms = (int)ESC_TIMEOUT_MS;
    if (emu_left < (double)timeout_ms)
        timeout_ms = emu_left < 0 ? 0 : (int)emu_left + 1;

    read_some(timeout_ms);
    now = plat_time_ms();
    parse_buffer(ilen > 0 && now - last_rx >= ESC_TIMEOUT_MS);
    emu_tick(now);

    return take_events(out, cap);
}

#endif
