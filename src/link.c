/*
 * link.c - One broker: an MQTT session with reconnection.
 */
#include "link.h"

#include <string.h>

#include "util.h"

#define KEEPALIVE_S 30
#define OPEN_TIMEOUT_MS 15000.0 /* DNS, TCP and TLS */
#define CLOSE_TIMEOUT_MS 1000.0

static const double backoff_ms[] = {1000.0, 2000.0, 5000.0, 10000.0, 30000.0};

static void on_mqtt(void *ctx, const char *topic, const uint8_t *payload, int len)
{
    link_t *l = (link_t *)ctx;
    if (l->on_message && strcmp(topic, l->topic) == 0)
        l->on_message(l->ctx, payload, len);
}

void link_init(link_t *l, const char *client_id, const char *user, const char *pass, link_message_fn fn, void *ctx)
{
    memset(l, 0, sizeof *l);
    str_copy(l->client_id, sizeof l->client_id, client_id);
    str_copy(l->user, sizeof l->user, user ? user : "");
    str_copy(l->pass, sizeof l->pass, pass ? pass : "");
    l->on_message = fn;
    l->ctx = ctx;
    mqtt_init(&l->mqtt, on_mqtt, l);
}

static void wait_retry(link_t *l, double now)
{
    int i = mini(l->tries, (int)ARRAY_LEN(backoff_ms) - 1);
    l->state = LINK_WAIT;
    l->at = now + backoff_ms[i];
    l->tries++;
    l->subscribed = 0;
}

void link_start(link_t *l, const char *topic, double now)
{
    if (l->state == LINK_UP && l->subscribed && strcmp(l->topic, topic) != 0) {
        mqtt_unsubscribe(&l->mqtt, l->topic);
        l->subscribed = 0;
    }
    str_copy(l->topic, sizeof l->topic, topic);
    if (l->state == LINK_IDLE || l->state == LINK_CLOSING) {
        l->state = LINK_WAIT;
        l->at = now;
        l->tries = 0;
    }
}

void link_stop(link_t *l, double now)
{
    l->topic[0] = 0;
    if (l->state == LINK_UP || l->state == LINK_CONNECTING) {
        mqtt_disconnect(&l->mqtt);
        l->state = LINK_CLOSING;
        l->at = now;
    } else if (l->state == LINK_OPENING) {
        l->state = LINK_CLOSING;
        l->at = now;
    } else {
        l->state = LINK_IDLE;
    }
    l->subscribed = 0;
}

int link_publish(link_t *l, const uint8_t *payload, int len)
{
    return l->state == LINK_UP && l->subscribed && mqtt_publish(&l->mqtt, l->topic, payload, len);
}

static void set_reason(link_t *l, const char *why)
{
    str_copy(l->reason, sizeof l->reason, why ? why : "");
}

int link_update(link_t *l, double now)
{
    switch (l->state) {
    case LINK_WAIT:
        if (now >= l->at) {
            l->state = LINK_OPENING;
            l->at = now;
            return LINK_OPEN;
        }
        break;
    case LINK_OPENING:
        if (now - l->at > OPEN_TIMEOUT_MS) {
            set_reason(l, "no answer");
            wait_retry(l, now);
            return LINK_CLOSE;
        }
        break;
    case LINK_CONNECTING:
    case LINK_UP:
        if (mqtt_tick(&l->mqtt, now) == MQTT_FAILED) {
            switch (l->mqtt.error) {
            case MQTT_ERR_REFUSED:
                set_reason(l, l->mqtt.refused == 4 || l->mqtt.refused == 5 ? "not authorized" : "refused");
                break;
            case MQTT_ERR_TIMEOUT:
                set_reason(l, "timed out");
                break;
            default:
                set_reason(l, "protocol error");
                break;
            }
            wait_retry(l, now);
            return LINK_CLOSE;
        }
        if (l->state == LINK_CONNECTING && l->mqtt.state == MQTT_READY) {
            l->state = LINK_UP;
            l->tries = 0;
            set_reason(l, NULL);
        }
        if (l->state == LINK_UP && !l->subscribed && l->topic[0])
            l->subscribed = mqtt_subscribe(&l->mqtt, l->topic);
        if (l->state == LINK_UP && l->mqtt.sub_denied && !l->reason[0])
            set_reason(l, "not allowed on this topic");
        break;
    case LINK_CLOSING: {
        int n;
        mqtt_output(&l->mqtt, &n);
        if (n == 0 || now - l->at > CLOSE_TIMEOUT_MS) {
            l->state = LINK_IDLE;
            return LINK_CLOSE;
        }
        break;
    }
    default:
        break;
    }
    return LINK_NOTHING;
}

void link_opened(link_t *l, double now)
{
    if (l->state != LINK_OPENING)
        return;
    l->state = LINK_CONNECTING;
    l->at = now;
    mqtt_connect(&l->mqtt, l->client_id, l->user, l->pass, KEEPALIVE_S, now);
}

void link_closed(link_t *l, double now, const char *why)
{
    if (l->state == LINK_IDLE || l->state == LINK_WAIT)
        return;
    if (l->state == LINK_CLOSING) {
        l->state = LINK_IDLE;
        return;
    }
    set_reason(l, why ? why : "closed by the broker");
    wait_retry(l, now);
}

void link_input(link_t *l, const uint8_t *data, int n, double now)
{
    if (l->state == LINK_CONNECTING || l->state == LINK_UP)
        mqtt_input(&l->mqtt, data, n, now);
}

const uint8_t *link_output(const link_t *l, int *len)
{
    if (l->state != LINK_CONNECTING && l->state != LINK_UP && l->state != LINK_CLOSING) {
        *len = 0;
        return l->mqtt.out;
    }
    return mqtt_output(&l->mqtt, len);
}

void link_consume(link_t *l, int n)
{
    mqtt_consume(&l->mqtt, n);
}

int link_up(const link_t *l)
{
    return l->state == LINK_UP && l->subscribed;
}

int link_retry_in(const link_t *l, double now)
{
    if (l->state != LINK_WAIT || l->at <= now)
        return 0;
    return (int)((l->at - now) / 1000.0 + 0.999);
}
