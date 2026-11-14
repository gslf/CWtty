/*
 * mqtt.c - A minimal MQTT 3.1.1 client over a byte stream.
 *
 * Packets: CONNECT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH (QoS 0 out, QoS 0 or 1
 * in), PINGREQ and DISCONNECT; the broker's CONNACK, SUBACK, UNSUBACK,
 * PUBLISH and PINGRESP. The parser takes the stream in pieces of any size.
 */
#include "mqtt.h"

#include <string.h>

#define CONNACK_TIMEOUT_MS 10000.0

enum { ST_HEADER, ST_LENGTH, ST_BODY, ST_SKIP };

void mqtt_init(mqtt_client *m, mqtt_message_fn on_message, void *ctx)
{
    memset(m, 0, sizeof *m);
    m->on_message = on_message;
    m->ctx = ctx;
    m->next_id = 1;
}

static void fail(mqtt_client *m, int error)
{
    if (m->state != MQTT_FAILED)
        m->error = error;
    m->state = MQTT_FAILED;
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                   */
/* ------------------------------------------------------------------------ */

static int len_size(uint32_t n)
{
    return n < 128 ? 1 : n < 16384 ? 2 : n < 2097152 ? 3 : 4;
}

/* Reserves a packet with a body of body_len bytes: writes the fixed header
 * and returns where the body goes, or NULL if it does not fit. */
static uint8_t *packet(mqtt_client *m, int type, uint32_t body_len)
{
    int need = 1 + len_size(body_len) + (int)body_len;
    uint8_t *p;
    if (body_len > 268435455u || m->out_len + need > MQTT_OUT_CAP)
        return NULL;
    p = m->out + m->out_len;
    *p++ = (uint8_t)type;
    do {
        uint8_t b = (uint8_t)(body_len & 127);
        body_len >>= 7;
        *p++ = (uint8_t)(b | (body_len ? 128 : 0));
    } while (body_len);
    m->out_len += need;
    return p;
}

static uint8_t *put16(uint8_t *p, unsigned v)
{
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static uint8_t *put_str(uint8_t *p, const char *s, int n)
{
    p = put16(p, (unsigned)n);
    memcpy(p, s, (size_t)n);
    return p + n;
}

static int slen(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    return n > 65535 ? 65535 : (int)n;
}

void mqtt_connect(mqtt_client *m, const char *client_id, const char *user, const char *pass, int keepalive_s,
                  double now_ms)
{
    int nid = slen(client_id), nuser = slen(user), npass = slen(pass);
    uint32_t body = 10 + 2 + (uint32_t)nid;
    uint8_t flags = 0x02, *p; /* clean session */

    m->out_len = 0;
    m->stage = ST_HEADER;
    m->state = MQTT_CONNECTING;
    m->error = m->refused = m->sub_denied = 0;
    m->ping_pending = 0;
    m->keepalive_ms = keepalive_s * 1000;
    m->started = m->last_out = now_ms;
    if (nuser) {
        flags |= 0x80;
        body += 2 + (uint32_t)nuser;
        if (npass) {
            flags |= 0x40;
            body += 2 + (uint32_t)npass;
        }
    }
    p = packet(m, 0x10, body);
    if (!p) {
        fail(m, MQTT_ERR_OVERFLOW);
        return;
    }
    p = put_str(p, "MQTT", 4);
    *p++ = 4; /* protocol level: 3.1.1 */
    *p++ = flags;
    p = put16(p, (unsigned)keepalive_s);
    p = put_str(p, client_id ? client_id : "", nid);
    if (nuser) {
        p = put_str(p, user, nuser);
        if (npass)
            put_str(p, pass, npass);
    }
}

static int subscription(mqtt_client *m, int type, const char *topic, int qos_byte)
{
    int n = slen(topic);
    uint8_t *p;
    if (m->state != MQTT_READY || n == 0)
        return 0;
    p = packet(m, type, 2 + 2 + (uint32_t)n + (qos_byte ? 1 : 0));
    if (!p)
        return 0;
    p = put16(p, m->next_id);
    m->next_id = (uint16_t)(m->next_id == 65535 ? 1 : m->next_id + 1);
    p = put_str(p, topic, n);
    if (qos_byte)
        *p = 0; /* requested QoS 0 */
    return 1;
}

int mqtt_subscribe(mqtt_client *m, const char *topic)
{
    return subscription(m, 0x82, topic, 1);
}

int mqtt_unsubscribe(mqtt_client *m, const char *topic)
{
    return subscription(m, 0xA2, topic, 0);
}

int mqtt_publish(mqtt_client *m, const char *topic, const uint8_t *payload, int len)
{
    int n = slen(topic);
    uint8_t *p;
    if (m->state != MQTT_READY || n == 0 || len < 0)
        return 0;
    p = packet(m, 0x30, 2 + (uint32_t)n + (uint32_t)len);
    if (!p)
        return 0;
    p = put_str(p, topic, n);
    if (len)
        memcpy(p, payload, (size_t)len);
    return 1;
}

void mqtt_disconnect(mqtt_client *m)
{
    if (m->state == MQTT_READY)
        packet(m, 0xE0, 0);
    m->state = MQTT_CLOSED;
}

const uint8_t *mqtt_output(const mqtt_client *m, int *len)
{
    *len = m->out_len;
    return m->out;
}

void mqtt_consume(mqtt_client *m, int n)
{
    if (n <= 0)
        return;
    if (n >= m->out_len) {
        m->out_len = 0;
        return;
    }
    memmove(m->out, m->out + n, (size_t)(m->out_len - n));
    m->out_len -= n;
}

int mqtt_tick(mqtt_client *m, double now_ms)
{
    if (m->state == MQTT_CONNECTING && now_ms - m->started > CONNACK_TIMEOUT_MS)
        fail(m, MQTT_ERR_TIMEOUT);
    if (m->state != MQTT_READY || m->keepalive_ms <= 0)
        return m->state;
    if (m->ping_pending) {
        if (now_ms - m->ping_at > m->keepalive_ms)
            fail(m, MQTT_ERR_TIMEOUT);
    } else if (now_ms - m->last_out >= m->keepalive_ms / 2) {
        /* Half the keepalive: the broker allows one and a half. */
        if (packet(m, 0xC0, 0)) {
            m->ping_pending = 1;
            m->ping_at = m->last_out = now_ms;
        }
    }
    return m->state;
}

/* ------------------------------------------------------------------------ */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------ */

static unsigned get16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static void on_publish(mqtt_client *m, int flags, const uint8_t *b, int n)
{
    char topic[MQTT_TOPIC_CAP];
    int qos = (flags >> 1) & 3, tlen, at;
    if (n < 2)
        goto bad;
    tlen = (int)get16(b);
    at = 2 + tlen;
    if (qos == 3 || at > n)
        goto bad;
    if (qos > 0) {
        uint8_t *p;
        if (at + 2 > n)
            goto bad;
        if (qos == 1 && (p = packet(m, 0x40, 2)) != NULL) /* PUBACK */
            put16(p, get16(b + at));
        at += 2;
    }
    if (tlen >= MQTT_TOPIC_CAP)
        return; /* not one of ours */
    memcpy(topic, b + 2, (size_t)tlen);
    topic[tlen] = 0;
    if (m->on_message)
        m->on_message(m->ctx, topic, b + at, n - at);
    return;
bad:
    fail(m, MQTT_ERR_PROTOCOL);
}

static void dispatch(mqtt_client *m)
{
    const uint8_t *b = m->body;
    int n = m->body_len, type = m->hdr >> 4, i;

    m->ping_pending = 0; /* anything from the broker proves it is alive */
    switch (type) {
    case 2: /* CONNACK */
        if (n < 2 || m->state != MQTT_CONNECTING) {
            fail(m, MQTT_ERR_PROTOCOL);
        } else if (b[1] != 0) {
            m->refused = b[1];
            fail(m, MQTT_ERR_REFUSED);
        } else {
            m->state = MQTT_READY;
        }
        break;
    case 3: /* PUBLISH */
        if (m->state == MQTT_READY)
            on_publish(m, m->hdr & 15, b, n);
        break;
    case 9: /* SUBACK */
        for (i = 2; i < n; i++)
            if (b[i] & 0x80)
                m->sub_denied = 1;
        break;
    default: /* UNSUBACK, PINGRESP, PUBACK... */
        break;
    }
}

void mqtt_input(mqtt_client *m, const uint8_t *data, int n, double now_ms)
{
    int i = 0;
    (void)now_ms;
    while (i < n && m->state != MQTT_FAILED && m->state != MQTT_CLOSED) {
        uint8_t c = data[i];
        switch (m->stage) {
        case ST_HEADER:
            m->hdr = c;
            m->rem = 0;
            m->mult = 1;
            m->len_bytes = 0;
            m->stage = ST_LENGTH;
            i++;
            break;
        case ST_LENGTH:
            m->rem += (uint32_t)(c & 127) * m->mult;
            m->mult *= 128;
            i++;
            if (++m->len_bytes > 4) {
                fail(m, MQTT_ERR_PROTOCOL);
                break;
            }
            if (c & 128)
                break;
            m->body_len = 0;
            if (m->rem == 0) {
                dispatch(m);
                m->stage = ST_HEADER;
            } else if (m->rem > MQTT_IN_CAP) {
                m->skip = m->rem;
                m->stage = ST_SKIP;
            } else {
                m->stage = ST_BODY;
            }
            break;
        case ST_BODY: {
            int take = (int)m->rem - m->body_len;
            if (take > n - i)
                take = n - i;
            memcpy(m->body + m->body_len, data + i, (size_t)take);
            m->body_len += take;
            i += take;
            if (m->body_len == (int)m->rem) {
                m->stage = ST_HEADER;
                dispatch(m);
            }
            break;
        }
        case ST_SKIP: {
            uint32_t take = m->skip;
            if (take > (uint32_t)(n - i))
                take = (uint32_t)(n - i);
            m->skip -= take;
            i += (int)take;
            if (m->skip == 0) {
                m->ping_pending = 0;
                m->stage = ST_HEADER;
            }
            break;
        }
        }
    }
}
