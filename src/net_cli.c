/*
 * net_cli.c - The rooms without the interface: listen to a room and print
 * what is keyed there, or key a text into it, from the command line. Handy
 * for scripts and for testing the terminal app against the web app.
 */
#include "net_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "net.h"
#include "platform.h"
#include "settings.h"
#include "util.h"

#define JOIN_SETTLE_MS 1500.0 /* let the others introduce themselves first */

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

typedef struct {
    int cur; /* station of the line being printed, -1: none */
} printer;

static void print_event(net_t *n, printer *p, const room_event *e)
{
    const room_station *st = &n->room.st[e->station];
    switch (e->kind) {
    case RE_JOIN:
        printf("%s* %s is in the room (%d Hz)\n", p->cur >= 0 ? "\n" : "", st->call, st->tone_hz);
        p->cur = -1;
        break;
    case RE_LEAVE:
        printf("%s* %s left\n", p->cur >= 0 ? "\n" : "", st->call);
        p->cur = -1;
        break;
    case RE_TOKEN:
        if (p->cur != e->station) {
            printf("%s%s: ", p->cur >= 0 ? "\n" : "", st->call);
            p->cur = e->station;
        }
        fputs(e->tok.text, stdout);
        break;
    default:
        break;
    }
    fflush(stdout);
}

static void print_links(net_t *n, int *last)
{
    char buf[128];
    int i;
    for (i = 0; i < BROKERS; i++) {
        int s = net_link_status(n, i, buf, sizeof buf);
        if (s >= 0 && s != last[i]) {
            fprintf(stderr, "[%s]\n", buf);
            last[i] = s;
        }
    }
}

int net_cli(const net_cli_options *o)
{
    static net_t net;
    settings_t cfg;
    char path[1024], call[SETTINGS_NAME_CAP], room[SETTINGS_NAME_CAP];
    printer pr;
    audio_params ap;
    audio_event ev[256];
    room_event re;
    int last[BROKERS], i, err, phase = 0;
    double up_at = 0.0, done_at = 0.0;

    settings_defaults(&cfg);
    if (o->config[0])
        str_copy(path, sizeof path, o->config);
    else if (plat_config_path(path, sizeof path) != 0)
        path[0] = 0;
    if (path[0])
        settings_load(&cfg, path);

    str_copy(call, sizeof call, o->call[0] ? o->call : cfg.callsign);
    if (!call[0] && !o->send)
        str_copy(call, sizeof call, "SWL");
    str_copy(room, sizeof room, o->room[0] ? o->room : cfg.net_room);

    netio_startup();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (audio_init(cfg.out_dev, "", cfg.latency_ms) != 0) {
        fprintf(stderr, "cwtty: cannot start the audio engine\n");
        return 1;
    }
    ap.tone_hz = cfg.tone_hz;
    ap.wpm = cfg.wpm;
    ap.keyer_mode = cfg.keyer_mode;
    ap.volume = cfg.volume;
    ap.rise_ms = cfg.rise_ms;
    audio_set_params(&ap);
    audio_voice_volume(cfg.volume);

    if (net_init(&net) != 0) {
        fprintf(stderr, "cwtty: cannot initialise the network\n");
        audio_shutdown();
        return 1;
    }
    net_settings(&net, cfg.wpm, cfg.net_tone, cfg.tone_hz);
    err = net_join(&net, call, room, o->password, cfg.wpm);
    if (err) {
        fprintf(stderr, "cwtty: %s\n",
                err == ROOM_BAD_CALL ? "a callsign is needed (--call, letters, digits and /)" : "invalid room name");
        net_free(&net);
        audio_shutdown();
        return 2;
    }
    fprintf(stderr, "[%s room %s as %s]\n", net.room.priv ? "private" : "public", net.room.name, net.room.call);

    for (i = 0; i < BROKERS; i++)
        last[i] = -2;
    pr.cur = -1;
    while (!stop) {
        double clock = audio_clock_ms();
        int n = audio_poll_events(ev, 256), k;
        for (k = 0; k < n; k++)
            if (ev[k].src == SRC_PLAYBACK)
                net_key(&net, ev[k].on, audio_to_ms(ev[k].t));
        net_update(&net, clock);
        while (net_pop_event(&net, &re))
            print_event(&net, &pr, &re);
        print_links(&net, last);

        if (o->send) {
            double now = plat_time_ms();
            if (phase == 0 && net_links_up(&net) > 0) {
                up_at = now;
                phase = 1;
            } else if (phase == 1 && now - up_at > JOIN_SETTLE_MS) {
                audio_play_text(o->text, cfg.wpm, 0, 50.0);
                phase = 2;
            } else if (phase == 2 && !audio_playback_busy()) {
                done_at = now;
                phase = 3;
            } else if (phase == 3 && now - done_at > 1000.0) {
                break;
            }
        }
        plat_sleep_ms(5);
    }
    if (pr.cur >= 0)
        printf("\n");
    net_flush(&net, 1500);
    net_free(&net);
    audio_shutdown();
    return 0;
}
