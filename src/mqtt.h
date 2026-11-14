/*
 * mqtt.h - A minimal MQTT 3.1.1 client over a byte stream: CONNECT,
 * SUBSCRIBE, PUBLISH at QoS 0 and the keepalive.
 *
 * There is no I/O here. The transport (a TCP or TLS socket natively, a
 * WebSocket in the browser) feeds the bytes it receives to mqtt_input() and
 * sends what mqtt_output() returns. Buffers are fixed; time is passed in.
 */
#ifndef CWTTY_MQTT_H
#define CWTTY_MQTT_H

#include <stdint.h>

#define MQTT_OUT_CAP 8192  /* bytes waiting for the transport */
#define MQTT_IN_CAP 1024   /* largest packet accepted: larger ones are skipped */
#define MQTT_TOPIC_CAP 128 /* longest topic accepted */

enum { MQTT_CLOSED, MQTT_CONNECTING, MQTT_READY, MQTT_FAILED };

enum {
    MQTT_OK,
    MQTT_ERR_PROTOCOL, /* the broker sent something malformed */
    MQTT_ERR_REFUSED,  /* CONNACK refused the session (see refused) */
    MQTT_ERR_TIMEOUT,  /* no CONNACK, or no answer to a ping */
    MQTT_ERR_OVERFLOW  /* the transport did not keep up */
};

typedef void (*mqtt_message_fn)(void *ctx, const char *topic, const uint8_t *payload, int len);

typedef struct {
    int state, error, refused; /* refused: the CONNACK return code */
    int sub_denied;            /* a SUBSCRIBE was refused (access control) */
    int keepalive_ms;
    double started, last_out, ping_at;
    int ping_pending;
    uint16_t next_id;

    /* Receiving: the fixed header, the remaining length, the body. */
    int      stage, hdr, len_bytes;
    uint32_t rem, mult, skip;
    int      body_len;
    uint8_t  body[MQTT_IN_CAP];

    uint8_t out[MQTT_OUT_CAP];
    int     out_len;

    mqtt_message_fn on_message;
    void           *ctx;
} mqtt_client;

void mqtt_init(mqtt_client *m, mqtt_message_fn on_message, void *ctx);

/* Starts a session on a transport that has just opened: queues CONNECT
 * (clean session, no will). user and pass may be NULL or empty. */
void mqtt_connect(mqtt_client *m, const char *client_id, const char *user, const char *pass, int keepalive_s,
                  double now_ms);

/* Queue a packet; return 0 if it does not fit (or the session is not
 * ready, for publish). */
int mqtt_subscribe(mqtt_client *m, const char *topic);
int mqtt_unsubscribe(mqtt_client *m, const char *topic);
int mqtt_publish(mqtt_client *m, const char *topic, const uint8_t *payload, int len);

/* Queues DISCONNECT and closes the session. */
void mqtt_disconnect(mqtt_client *m);

/* The transport received bytes. */
void mqtt_input(mqtt_client *m, const uint8_t *data, int n, double now_ms);

/* Pings when idle and detects a silent broker. Returns the state. */
int mqtt_tick(mqtt_client *m, double now_ms);

/* Bytes waiting to be sent; the transport consumes what it wrote. */
const uint8_t *mqtt_output(const mqtt_client *m, int *len);
void           mqtt_consume(mqtt_client *m, int n);

#endif
