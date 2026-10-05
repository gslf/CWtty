/*
 * main.c - Entry point: command line, terminal setup and the main loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "app.h"
#include "audio.h"
#include "input.h"
#include "platform.h"
#include "render.h"
#include "term.h"
#include "util.h"

#ifndef CWTTY_VERSION
#define CWTTY_VERSION "dev"
#endif

#define FRAME_MS (1000.0 / 60.0)

static void usage(void)
{
    printf("CWtty %s - Morse telegraphy in your terminal\n\n"
           "Usage: cwtty [options]\n\n"
           "Options:\n"
           "  -m, --mode MODE     start in tx, rx, trx or train\n"
           "  -c, --config FILE   use FILE for the settings\n"
           "      --legacy-keys   do not use the kitty keyboard protocol\n"
           "      --demo          feed the receiver with a simulated band\n"
           "      --list-devices  list the audio devices and exit\n"
           "  -v, --version       print the version and exit\n"
           "  -h, --help          show this help\n\n"
           "Environment:\n"
           "  CWTTY_COLOR=256|truecolor   force the colour mode\n\n"
           "Press C-x inside the program for the command menu.\n",
           CWTTY_VERSION);
}

static int parse_mode(const char *s)
{
    if (strcmp(s, "tx") == 0)
        return MODE_TX;
    if (strcmp(s, "rx") == 0)
        return MODE_RX;
    if (strcmp(s, "trx") == 0 || strcmp(s, "txrx") == 0)
        return MODE_TRX;
    if (strcmp(s, "train") == 0 || strcmp(s, "training") == 0)
        return MODE_TRAIN;
    return -1;
}

static int list_devices(void)
{
    char names[MAX_DEVICES][AUDIO_NAME_CAP];
    int n, i;
    audio_init("", "", 10);
    n = audio_list_devices(0, names, MAX_DEVICES);
    printf("Output devices:\n");
    for (i = 0; i < n; i++)
        printf("  %s\n", names[i]);
    n = audio_list_devices(1, names, MAX_DEVICES);
    printf("Input devices:\n");
    for (i = 0; i < n; i++)
        printf("  %s\n", names[i]);
    audio_shutdown();
    return 0;
}

static int detect_truecolor(void)
{
    const char *force = getenv("CWTTY_COLOR");
    if (force && *force)
        return strcmp(force, "256") != 0;
#ifdef _WIN32
    return 1; /* Windows 10+ consoles all support 24-bit colour */
#else
    const char *ct = getenv("COLORTERM");
    const char *term = getenv("TERM");
    const char *prog = getenv("TERM_PROGRAM");
    static const char *const known[] = {"kitty", "alacritty", "foot", "ghostty", "wezterm", "direct", "iterm"};
    int i;

    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
        return 1;
    if (prog && strcmp(prog, "Apple_Terminal") == 0)
        return 0;
    if (prog && (strcmp(prog, "iTerm.app") == 0 || strcmp(prog, "WezTerm") == 0 || strcmp(prog, "ghostty") == 0))
        return 1;
    if (term)
        for (i = 0; i < ARRAY_LEN(known); i++)
            if (strstr(term, known[i]))
                return 1;
    return 0;
#endif
}

int main(int argc, char **argv)
{
    app_options opt;
    app_t *app;
    int i, w, h;
    double next_frame = 0;
#ifndef _WIN32
    int saved_stderr = -1;
#endif

    memset(&opt, 0, sizeof opt);
    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (strcmp(s, "-h") == 0 || strcmp(s, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(s, "-v") == 0 || strcmp(s, "--version") == 0) {
            printf("cwtty %s\n", CWTTY_VERSION);
            return 0;
        } else if (strcmp(s, "--legacy-keys") == 0) {
            opt.force_legacy_keys = 1;
        } else if (strcmp(s, "--demo") == 0) {
            opt.demo = 1;
        } else if (strcmp(s, "--list-devices") == 0) {
            return list_devices();
        } else if ((strcmp(s, "-m") == 0 || strcmp(s, "--mode") == 0) && i + 1 < argc) {
            opt.start_mode = parse_mode(argv[++i]);
            if (opt.start_mode < 0) {
                fprintf(stderr, "cwtty: unknown mode '%s' (use tx, rx, trx or train)\n", argv[i]);
                return 2;
            }
        } else if ((strcmp(s, "-c") == 0 || strcmp(s, "--config") == 0) && i + 1 < argc) {
            str_copy(opt.config_override, sizeof opt.config_override, argv[++i]);
        } else {
            fprintf(stderr, "cwtty: unknown option '%s'\n\n", s);
            usage();
            return 2;
        }
    }
    {
        const char *keys = getenv("CWTTY_KEYS");
        if (keys && strcmp(keys, "legacy") == 0)
            opt.force_legacy_keys = 1;
    }

    rng_seed(plat_entropy());

    if (term_init() != 0) {
        fprintf(stderr, "cwtty: this program needs an interactive terminal%s\n",
#ifdef _WIN32
                " with virtual terminal support (Windows 10 or later)"
#else
                ""
#endif
        );
        return 1;
    }
    scr_init(detect_truecolor());
    input_init(opt.force_legacy_keys);

#ifndef _WIN32
    /* Audio libraries may print diagnostics on stderr: keep them off the
     * screen while the interface is up. */
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            saved_stderr = dup(2);
            dup2(devnull, 2);
            close(devnull);
        }
    }
#endif

    app = app_create(&opt);
    if (!app) {
        scr_shutdown();
        term_shutdown();
#ifndef _WIN32
        if (saved_stderr >= 0) {
            dup2(saved_stderr, 2);
            close(saved_stderr);
        }
#endif
        fprintf(stderr, "cwtty: out of memory\n");
        return 1;
    }

    term_get_size(&w, &h);
    scr_resize(w, h);

    while (!app->quit && !term_quit_requested()) {
        key_event ev[64];
        double now = plat_time_ms();
        int timeout = (int)clampd(next_frame - now, 0.0, 16.0), n, k;
        n = input_poll(ev, 64, timeout);
        for (k = 0; k < n; k++)
            app_handle_key(app, &ev[k]);

        term_take_resize();
        term_get_size(&w, &h);
        if (w != scr_w() || h != scr_h())
            scr_resize(w, h);

        app_update(app);

        now = plat_time_ms();
        if (now >= next_frame || n > 0) {
            app_render(app);
            scr_present();
            next_frame = now + FRAME_MS;
        }
    }

    app_destroy(app);
    scr_shutdown();
    term_shutdown();
#ifndef _WIN32
    if (saved_stderr >= 0) {
        dup2(saved_stderr, 2);
        close(saved_stderr);
    }
#endif
    return 0;
}
