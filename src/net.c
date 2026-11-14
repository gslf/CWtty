/*
 * net.c - The online room of the terminal app.
 */
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "platform.h"
#include "util.h"

static void on_message(void *ctx, const uint8_t *payload, int len)
{
    net_t *n = (net_t *)ctx;
    room_receive(&n->room, payload, len, n->now);
}

int net_init(net_t *n)
{
    uint8_t seed[32], cid[4];
    char id[32];
    int i;

    memset(n, 0, sizeof *n);
    if (plat_random(seed, sizeof seed) != 0)
        return -1;
    room_init(&n->room, seed);
    for (i = 0; i < BROKERS; i++) {
        if (plat_random(cid, sizeof cid) != 0)
            return -1;
        snprintf(id, sizeof id, "cwtty-%02x%02x%02x%02x", cid[0], cid[1], cid[2], cid[3]);
        link_init(&n->link[i], id, brokers[i].user, brokers[i].pass, on_message, n);
        n->io[i] = netio_new();
        if (!n->io[i])
            return -1;
    }
    n->ok = 1;
    return 0;
}

void net_free(net_t *n)
{
    int i;
    for (i = 0; i < BROKERS; i++)
        netio_free(n->io[i]);
    memset(n, 0, sizeof *n);
}

void net_settings(net_t *n, int wpm, int base_hz, int own_hz)
{
    /* The room clock moves one audio period at a time, and the voices take
     * their events once a period: the jitter buffer covers both. */
    double margin = 2.0 * audio_period_ms() + 10.0;
    room_settings(&n->room, wpm, base_hz, own_hz, margin > 30.0 ? margin : 30.0);
}

int net_join(net_t *n, const char *call, const char *name, const char *password, int wpm)
{
    int i, err;
    n->now = audio_clock_ms();
    audio_voice_clear();
    err = room_join(&n->room, call, name, password, wpm, n->now);
    if (err)
        return err;
    for (i = 0; i < BROKERS; i++) {
        n->was_up[i] = 0;
        if (broker_configured(i))
            link_start(&n->link[i], n->room.topic, n->now);
    }
    return 0;
}

void net_leave(net_t *n)
{
    room_t *r = &n->room;
    uint8_t buf[ROOM_PACKET_CAP];
    int i, len;
    if (!r->joined)
        return;
    n->now = audio_clock_ms();
    room_leave(r, n->now);
    while ((len = room_pop_out(r, buf, sizeof buf)) > 0)
        for (i = 0; i < BROKERS; i++)
            link_publish(&n->link[i], buf, len);
    for (i = 0; i < BROKERS; i++)
        link_stop(&n->link[i], n->now);
    audio_voice_clear();
}

int net_joined(const net_t *n)
{
    return n->room.joined;
}

void net_key(net_t *n, int on, double t)
{
    room_key(&n->room, on, t);
}

/* Moves bytes between a link and its socket. */
static void pump(net_t *n, int i)
{
    link_t *l = &n->link[i];
    netio *io = n->io[i];
    uint8_t buf[2048];
    int act = link_update(l, n->now), st, k, len;
    const uint8_t *out;

    if (act == LINK_OPEN) {
        netio_open(io, brokers[i].host, brokers[i].port, brokers[i].tls, brokers[i].ca_pem);
    } else if (act == LINK_CLOSE) {
        netio_close(io);
        link_closed(l, n->now, NULL);
        return;
    }
    st = netio_poll(io);
    if (l->state == LINK_OPENING) {
        if (st == NETIO_OPEN) {
            link_opened(l, n->now);
        } else if (st == NETIO_FAILED) {
            link_closed(l, n->now, netio_error(io));
            netio_close(io);
            return;
        }
    }
    if (st != NETIO_OPEN)
        return;
    out = link_output(l, &len);
    if (len > 0) {
        int w = netio_write(io, out, len);
        if (w > 0)
            link_consume(l, w);
    }
    for (k = 0; k < 16; k++) { /* at most 32 KiB per pass */
        int r = netio_read(io, buf, sizeof buf);
        if (r > 0) {
            link_input(l, buf, r, n->now);
        } else {
            if (r < 0) {
                link_closed(l, n->now, netio_error(io));
                netio_close(io);
            }
            break;
        }
    }
}

void net_update(net_t *n, double clock)
{
    room_t *r = &n->room;
    uint8_t buf[ROOM_PACKET_CAP];
    room_vkey k;
    int i, len;

    n->now = clock;
    for (i = 0; i < BROKERS; i++) {
        int up;
        if (!broker_configured(i))
            continue;
        pump(n, i);
        up = link_up(&n->link[i]);
        if (up && !n->was_up[i]) /* those only on this broker must learn of us */
            room_announce(r, clock);
        n->was_up[i] = up;
    }
    room_update(r, clock, clock);
    while (net_links_up(n) > 0 && (len = room_pop_out(r, buf, sizeof buf)) > 0)
        for (i = 0; i < BROKERS; i++)
            link_publish(&n->link[i], buf, len);
    while (room_pop_vkey(r, &k)) {
        audio_voice_tone(k.voice, k.tone_hz);
        audio_voice_key(k.voice, k.t, k.on);
    }
    /* Send what was just queued without waiting for the next pass. */
    for (i = 0; i < BROKERS; i++) {
        const uint8_t *out;
        if (!broker_configured(i) || netio_poll(n->io[i]) != NETIO_OPEN)
            continue;
        out = link_output(&n->link[i], &len);
        if (len > 0) {
            int w = netio_write(n->io[i], out, len);
            if (w > 0)
                link_consume(&n->link[i], w);
        }
    }
}

int net_pop_event(net_t *n, room_event *e)
{
    return room_pop_event(&n->room, e);
}

int net_link_status(const net_t *n, int i, char *buf, int cap)
{
    const link_t *l = &n->link[i];
    if (!broker_configured(i)) {
        snprintf(buf, (size_t)cap, "%s: not configured", brokers[i].name);
        return -1;
    }
    switch (l->state) {
    case LINK_UP:
        if (link_up(l)) {
            snprintf(buf, (size_t)cap, "%s: connected", brokers[i].name);
            return 1;
        }
        snprintf(buf, (size_t)cap, "%s: %s", brokers[i].name, l->reason[0] ? l->reason : "joining");
        return 0;
    case LINK_WAIT:
        if (l->reason[0])
            snprintf(buf, (size_t)cap, "%s: %s, retry in %d s", brokers[i].name, l->reason,
                     link_retry_in(l, n->now));
        else
            snprintf(buf, (size_t)cap, "%s: connecting", brokers[i].name);
        return 0;
    case LINK_OPENING:
    case LINK_CONNECTING:
        snprintf(buf, (size_t)cap, "%s: connecting", brokers[i].name);
        return 0;
    default:
        snprintf(buf, (size_t)cap, "%s: off", brokers[i].name);
        return 0;
    }
}

int net_links_up(const net_t *n)
{
    int i, up = 0;
    for (i = 0; i < BROKERS; i++)
        up += link_up(&n->link[i]);
    return up;
}

void net_flush(net_t *n, int ms)
{
    double end = plat_time_ms() + ms;
    int i, busy;
    net_leave(n);
    do {
        busy = 0;
        net_update(n, audio_clock_ms());
        for (i = 0; i < BROKERS; i++)
            busy |= n->link[i].state != LINK_IDLE;
        if (busy)
            plat_sleep_ms(5);
    } while (busy && plat_time_ms() < end);
}
