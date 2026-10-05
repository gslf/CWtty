/*
 * term.c - Terminal platform layer (POSIX termios / Windows console).
 */
#include "term.h"

#include <stdlib.h>
#include <string.h>

/*
 * Enter: alternate screen, hide cursor, no auto-wrap, focus reporting,
 * clear. Leave undoes it in reverse order.
 */
static const char SEQ_ENTER[] = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1004h\x1b[0m\x1b[2J";
static const char SEQ_LEAVE[] = "\x1b[?1004l\x1b[?7h\x1b[0m\x1b[2J\x1b[?25h\x1b[?1049l";
static const char SEQ_KITTY_POP[] = "\x1b[<u";

static volatile int g_kitty_pushed;

void term_set_kitty_pushed(int pushed)
{
    g_kitty_pushed = pushed;
}

void term_write_str(const char *s)
{
    term_write(s, strlen(s));
}

#ifdef _WIN32
/* ======================================================================== */
/* Windows console                                                          */
/* ======================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

static HANDLE h_in = INVALID_HANDLE_VALUE;
static HANDLE h_out = INVALID_HANDLE_VALUE;
static DWORD old_in_mode, old_out_mode;
static UINT old_out_cp, old_in_cp;
static int g_active;
static volatile LONG g_quit;
static volatile LONG g_resized;

void term_write(const char *buf, size_t len)
{
    DWORD written;
    while (len > 0) {
        DWORD chunk = (DWORD)(len > 65536 ? 65536 : len);
        if (!WriteFile(h_out, buf, chunk, &written, NULL) || written == 0)
            return;
        buf += written;
        len -= written;
    }
}

static void restore(void)
{
    if (!g_active)
        return;
    g_active = 0;
    if (g_kitty_pushed)
        term_write(SEQ_KITTY_POP, sizeof SEQ_KITTY_POP - 1);
    term_write(SEQ_LEAVE, sizeof SEQ_LEAVE - 1);
    SetConsoleMode(h_in, old_in_mode);
    SetConsoleMode(h_out, old_out_mode);
    SetConsoleOutputCP(old_out_cp);
    SetConsoleCP(old_in_cp);
}

/* Console modes outlive the process: restore them even after a crash. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    (void)info;
    restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_quit, 1);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        restore();
        return FALSE;
    }
    return TRUE;
}

int term_init(void)
{
    h_in = GetStdHandle(STD_INPUT_HANDLE);
    h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetConsoleMode(h_in, &old_in_mode) || !GetConsoleMode(h_out, &old_out_mode))
        return -1;
    old_out_cp = GetConsoleOutputCP();
    old_in_cp = GetConsoleCP();

    /* Raw key records (with key-up events), window and focus events. */
    SetConsoleMode(h_in, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    if (!SetConsoleMode(h_out, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                   DISABLE_NEWLINE_AUTO_RETURN)) {
        SetConsoleMode(h_in, old_in_mode);
        return -2; /* no VT support (pre Windows 10) */
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(crash_filter);
    g_active = 1;
    atexit(restore);
    term_write(SEQ_ENTER, sizeof SEQ_ENTER - 1);
    return 0;
}

void term_shutdown(void)
{
    restore();
}

void term_get_size(int *w, int *h)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(h_out, &info)) {
        *w = info.srWindow.Right - info.srWindow.Left + 1;
        *h = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        *w = 80;
        *h = 24;
    }
}

int term_take_resize(void)
{
    return InterlockedExchange(&g_resized, 0) != 0;
}

void term_flag_resize(void)
{
    InterlockedExchange(&g_resized, 1);
}

int term_quit_requested(void)
{
    return g_quit != 0;
}

void *term_input_handle(void)
{
    return (void *)h_in;
}

#else
/* ======================================================================== */
/* POSIX termios                                                            */
/* ======================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static int tty_fd = -1;
static int own_fd;
static struct termios orig_tio;
static volatile sig_atomic_t g_active;
static volatile sig_atomic_t g_resized;
static volatile sig_atomic_t g_quit;

void term_write(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(tty_fd, buf, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* Async-signal-safe: only write() and tcsetattr(). */
static void restore(void)
{
    if (!g_active)
        return;
    g_active = 0;
    if (g_kitty_pushed)
        term_write(SEQ_KITTY_POP, sizeof SEQ_KITTY_POP - 1);
    term_write(SEQ_LEAVE, sizeof SEQ_LEAVE - 1);
    tcsetattr(tty_fd, TCSAFLUSH, &orig_tio);
}

static void on_signal(int sig)
{
    switch (sig) {
    case SIGWINCH:
        g_resized = 1;
        return;
    case SIGINT:
    case SIGTERM:
    case SIGHUP:
        g_quit = 1;
        return;
    default:
        /* Fatal signal: put the terminal back, then die the normal way. */
        restore();
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void install_signals(void)
{
    static const int sigs[] = {SIGWINCH, SIGINT, SIGTERM, SIGHUP,
                               SIGSEGV,  SIGBUS, SIGFPE,  SIGILL, SIGABRT};
    struct sigaction sa;
    size_t i;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int term_init(void)
{
    struct termios raw;

    tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty_fd >= 0) {
        own_fd = 1;
    } else if (isatty(STDIN_FILENO)) {
        tty_fd = STDIN_FILENO;
    } else {
        return -1;
    }
    if (tcgetattr(tty_fd, &orig_tio) != 0)
        return -1;

    raw = orig_tio;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON | INLCR | IGNCR);
    raw.c_oflag &= ~(tcflag_t)(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(tty_fd, TCSAFLUSH, &raw) != 0)
        return -1;

    g_active = 1;
    install_signals();
    atexit(restore);
    term_write(SEQ_ENTER, sizeof SEQ_ENTER - 1);
    return 0;
}

void term_shutdown(void)
{
    restore();
    if (own_fd && tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
        own_fd = 0;
    }
}

void term_get_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(tty_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

int term_take_resize(void)
{
    if (g_resized) {
        g_resized = 0;
        return 1;
    }
    return 0;
}

int term_quit_requested(void)
{
    return g_quit != 0;
}

int term_read(unsigned char *buf, int cap, int timeout_ms)
{
    fd_set rs;
    struct timeval tv;
    ssize_t n;
    int r;

    FD_ZERO(&rs);
    FD_SET(tty_fd, &rs);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    /* select() rather than poll(): poll() on ttys is broken on macOS. */
    r = select(tty_fd + 1, &rs, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (r <= 0)
        return 0;
    n = read(tty_fd, buf, (size_t)cap);
    return n > 0 ? (int)n : 0;
}

#endif
