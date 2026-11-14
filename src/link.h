/*
 * link.h - One broker: the MQTT session of a room over a transport that the
 * host opens and closes, with reconnection.
 *
 * The transport is the host's (a TCP or TLS socket natively, a WebSocket in
 * the browser). link_update() says when to open or close it; the host
 * reports what happened with link_opened(), link_closed() and
 * link_input(), and sends what link_output() returns.
 */
#ifndef CWTTY_LINK_H
#define CWTTY_LINK_H

#include <stdint.h>

#include "mqtt.h"

#define LINK_TOPIC_CAP 64
#define LINK_REASON_CAP 64

enum { LINK_IDLE, LINK_WAIT, LINK_OPENING, LINK_CONNECTING, LINK_UP, LINK_CLOSING };

/* What link_update() asks of the host. */
enum { LINK_NOTHING, LINK_OPEN, LINK_CLOSE };

typedef void (*link_message_fn)(void *ctx, const uint8_t *payload, int len);

typedef struct {
    int    state;
    int    tries;  /* failures in a row: the backoff grows */
    double at;     /* when the current state began (or the retry is due) */
    int    subscribed;
    char   topic[LINK_TOPIC_CAP];
    char   reason[LINK_REASON_CAP]; /* why it is down, for the status line */
    char   client_id[32], user[64], pass[64];

    mqtt_client     mqtt;
    link_message_fn on_message;
    void           *ctx;
} link_t;

void link_init(link_t *l, const char *client_id, const char *user, const char *pass, link_message_fn fn, void *ctx);

/* Joins a topic: connects if needed, or moves the subscription. */
void link_start(link_t *l, const char *topic, double now);
/* Leaves: sends what is queued, then DISCONNECT, then asks to close. */
void link_stop(link_t *l, double now);

int link_publish(link_t *l, const uint8_t *payload, int len);

/* Timers; returns LINK_OPEN or LINK_CLOSE when the host must act. After a
 * LINK_CLOSE the host closes the transport and calls link_closed(). */
int link_update(link_t *l, double now);

/* Transport events. why: a short reason, or NULL for a normal close. */
void link_opened(link_t *l, double now);
void link_closed(link_t *l, double now, const char *why);
void link_input(link_t *l, const uint8_t *data, int n, double now);

const uint8_t *link_output(const link_t *l, int *len);
void           link_consume(link_t *l, int n);

/* The room is reachable through this link. */
int link_up(const link_t *l);
/* Seconds before the next attempt, when waiting (else 0). */
int link_retry_in(const link_t *l, double now);

#endif
