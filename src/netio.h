/*
 * netio.h - The terminal app's transport to the MQTT brokers: a TCP
 * connection, optionally with TLS (mbedTLS).
 *
 * Resolving the name, connecting and the TLS handshake can take seconds,
 * so they run on a worker thread; the connection is then used without
 * blocking from the main loop. TLS checks the broker's certificate against
 * the system's root certificates (plus any given ones) and its host name.
 */
#ifndef CWTTY_NETIO_H
#define CWTTY_NETIO_H

#include <stdint.h>

enum { NETIO_CLOSED, NETIO_OPENING, NETIO_OPEN, NETIO_FAILED };

typedef struct netio netio;

/* Once per process: sockets on Windows, SIGPIPE elsewhere. */
void netio_startup(void);

netio *netio_new(void);
void   netio_free(netio *c);

/* Starts connecting. tls: verify and encrypt; extra_ca: PEM roots trusted
 * besides the system's (may be NULL). */
void netio_open(netio *c, const char *host, int port, int tls, const char *extra_ca);
/* The state, once the worker is done: NETIO_OPEN or NETIO_FAILED. */
int netio_poll(netio *c);
/* Bytes read (0: nothing yet), or -1 when the connection is over. */
int netio_read(netio *c, uint8_t *buf, int cap);
/* Bytes written (0: try later), or -1 when the connection is over. */
int netio_write(netio *c, const uint8_t *buf, int n);
void netio_close(netio *c);

/* Why it failed. */
const char *netio_error(const netio *c);

#endif
