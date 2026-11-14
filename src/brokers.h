/*
 * brokers.h - The MQTT brokers the rooms use, shared by the terminal and
 * the web app. Every packet goes to all of them; a room works as long as
 * one is reachable.
 */
#ifndef CWTTY_BROKERS_H
#define CWTTY_BROKERS_H

#define BROKERS 2

typedef struct {
    const char *name;   /* for the status line */
    const char *host;   /* empty: not configured, skipped */
    int         port;   /* terminal app */
    int         tls;    /* terminal app: TLS on port */
    const char *ws_url; /* web app: MQTT over a secure WebSocket */
    const char *user, *pass;
    const char *ca_pem; /* roots trusted besides the system's (TLS), or NULL */
} broker_info;

extern const broker_info brokers[BROKERS];

int broker_configured(int i);

#endif
