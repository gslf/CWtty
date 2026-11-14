/*
 * net.h - The online room of the terminal app: the room protocol, one link
 * per broker over its socket, and the remote voices, driven from the main
 * loop.
 *
 * The room clock is the audio playback clock (audio_clock_ms), so remote
 * keying can be scheduled on the voices with sample accuracy.
 */
#ifndef CWTTY_NET_H
#define CWTTY_NET_H

#include "brokers.h"
#include "link.h"
#include "netio.h"
#include "room.h"

typedef struct {
    room_t  room;
    link_t  link[BROKERS];
    netio  *io[BROKERS];
    int     was_up[BROKERS];
    double  now;
    int     ok;
} net_t;

/* Returns 0 on success. */
int  net_init(net_t *n);
void net_free(net_t *n);

/* Enters a room (leaving the current one). A password makes it private.
 * Returns 0 or ROOM_BAD_CALL / ROOM_BAD_NAME. */
int  net_join(net_t *n, const char *call, const char *name, const char *password, int wpm);
void net_leave(net_t *n);
int  net_joined(const net_t *n);

/* Remote pitches around base_hz, away from the operator's own tone. */
void net_settings(net_t *n, int wpm, int base_hz, int own_hz);

/* The operator's key changed at audio clock t (ms). */
void net_key(net_t *n, int on, double t);

/* Sockets, room timers, remote voices. clock: audio_clock_ms() read before
 * the operator events were polled. */
void net_update(net_t *n, double clock);

int net_pop_event(net_t *n, room_event *e);

/* A broker's state for the status line: 1 up, 0 connecting or waiting,
 * -1 not configured. Writes a short description. */
int net_link_status(const net_t *n, int i, char *buf, int cap);
/* How many brokers carry the room now. */
int net_links_up(const net_t *n);

/* Leaves, then keeps the sockets running briefly so the goodbye goes out. */
void net_flush(net_t *n, int ms);

#endif
