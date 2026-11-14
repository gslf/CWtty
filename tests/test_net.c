/*
 * test_net.c - Self-checking tests for the online rooms: the remote voices,
 * the MQTT client, the room protocol and the broker links. Everything runs
 * offline with explicit clocks; the brokers are simulated in memory.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abbr.h"
#include "link.h"
#include "morse.h"
#include "mqtt.h"
#include "over.h"
#include "room.h"
#include "transcript.h"
#include "util.h"
#include "voices.h"

static int failures;

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                \
            printf(__VA_ARGS__);                                                         \
            printf("\n");                                                                \
        }                                                                                \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Voices                                                                    */
/* ------------------------------------------------------------------------ */

#define RATE 48000.0
#define BLOCK 128

/* Renders n frames of silence plus the voices, block by block. */
static void render(voices_t *vs, float *out, int n, uint64_t t0)
{
    int done;
    memset(out, 0, (size_t)n * sizeof *out);
    for (done = 0; done < n; done += BLOCK) {
        int m = n - done < BLOCK ? n - done : BLOCK;
        voices_mix(vs, out + done, m, 1, RATE, t0 + (uint64_t)done);
    }
}

static void test_voice_timing(void)
{
    static voices_t vs;
    static float out[9600];
    int first = -1, last = -1, i, crossings = 0;
    float peak = 0.0f;

    printf("voices: a key-down and key-up start and end at their samples\n");
    voices_init(&vs);
    voices_volume(&vs, 100, 5);
    voices_tone(&vs, 3, 1000);
    CHECK(voices_push(&vs, 3, 1000, 1), "push down");
    CHECK(voices_push(&vs, 3, 1000 + 4800, 0), "push up");
    render(&vs, out, 9600, 0);
    for (i = 0; i < 9600; i++) {
        if (out[i] != 0.0f) {
            if (first < 0)
                first = i;
            last = i;
        }
        if (fabsf(out[i]) > peak)
            peak = fabsf(out[i]);
        if (i > 0 && (out[i - 1] < 0.0f) != (out[i] < 0.0f) && out[i] != 0.0f && out[i - 1] != 0.0f)
            crossings++;
    }
    CHECK(first >= 1000 && first <= 1002, "sound starts at sample %d, expected 1000", first);
    /* Key up at 5800, then a 5 ms (240 samples) fall. */
    CHECK(last >= 5800 + 230 && last <= 5800 + 241, "sound ends at sample %d, expected about 6040", last);
    CHECK(peak > 0.65f && peak <= 0.71f, "peak %.3f, expected 0.7 at full volume", peak);
    /* 1 kHz for about 0.1 s: two crossings per period. */
    CHECK(crossings > 190 && crossings < 215, "%d zero crossings, expected about 200 for 1 kHz", crossings);
    CHECK(!voices_busy(&vs, 3), "the voice is idle afterwards");
}

static void test_voice_limiter(void)
{
    static voices_t vs;
    static float out[4800];
    float peak = 0.0f;
    int k, i;

    printf("voices: eight voices at full volume never clip\n");
    voices_init(&vs);
    voices_volume(&vs, 100, 1);
    for (k = 0; k < VOICES; k++) {
        voices_tone(&vs, k, 500 + 10 * k);
        voices_push(&vs, k, 0, 1);
    }
    render(&vs, out, 4800, 0);
    for (i = 0; i < 4800; i++)
        if (fabsf(out[i]) > peak)
            peak = fabsf(out[i]);
    CHECK(peak <= 0.9001f, "peak %.3f above the 0.9 ceiling", peak);
    CHECK(peak > 0.5f, "peak %.3f: the limiter is too aggressive", peak);
}

static void test_voice_late_and_reset(void)
{
    static voices_t vs;
    static float out[1024];
    int i, sound = 0;

    printf("voices: a late event plays at once, a reset silences\n");
    voices_init(&vs);
    voices_volume(&vs, 100, 1);
    voices_push(&vs, 0, 10, 1); /* long before t0 */
    render(&vs, out, 1024, 50000);
    CHECK(out[100] != 0.0f, "a late key-down still sounds");
    voices_reset(&vs);
    render(&vs, out, 1024, 51024);
    for (i = 0; i < 1024; i++)
        sound |= out[i] != 0.0f;
    CHECK(!sound, "silent after a reset");
    CHECK(!voices_busy(&vs, 0), "idle after a reset");
}


/* ------------------------------------------------------------------------ */
/* MQTT                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    int     count;
    char    topic[MQTT_TOPIC_CAP];
    uint8_t payload[2048];
    int     len;
} received;

static void on_message(void *ctx, const char *topic, const uint8_t *payload, int len)
{
    received *r = (received *)ctx;
    r->count++;
    strcpy(r->topic, topic);
    r->len = len;
    memcpy(r->payload, payload, (size_t)len);
}

/* Takes everything the client queued. */
static int take(mqtt_client *m, uint8_t *buf)
{
    int n;
    const uint8_t *p = mqtt_output(m, &n);
    memcpy(buf, p, (size_t)n);
    mqtt_consume(m, n);
    return n;
}

static void connected(mqtt_client *m, received *r)
{
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    uint8_t buf[MQTT_OUT_CAP];
    mqtt_init(m, on_message, r);
    mqtt_connect(m, "c1", "u", "p", 30, 0.0);
    take(m, buf);
    mqtt_input(m, connack, sizeof connack, 1.0);
}

/* Builds a broker PUBLISH (QoS 0) of a payload of n bytes. */
static int broker_publish(uint8_t *out, const char *topic, const uint8_t *payload, int n)
{
    int tl = (int)strlen(topic), body = 2 + tl + n, at = 0;
    out[at++] = 0x30;
    do {
        uint8_t b = (uint8_t)(body & 127);
        body >>= 7;
        out[at++] = (uint8_t)(b | (body ? 128 : 0));
    } while (body);
    out[at++] = (uint8_t)(tl >> 8);
    out[at++] = (uint8_t)tl;
    memcpy(out + at, topic, (size_t)tl);
    at += tl;
    memcpy(out + at, payload, (size_t)n);
    return at + n;
}

static void test_mqtt_connect(void)
{
    static mqtt_client m;
    static const uint8_t expect[] = {0x10, 20, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 30,
                                     0,    2,  'c', '1', 0, 1, 'u', 0, 1, 'p'};
    static const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    uint8_t buf[MQTT_OUT_CAP];
    int n;

    printf("mqtt: CONNECT, CONNACK and SUBSCRIBE\n");
    mqtt_init(&m, NULL, NULL);
    mqtt_connect(&m, "c1", "u", "p", 30, 0.0);
    n = take(&m, buf);
    CHECK(n == (int)sizeof expect && memcmp(buf, expect, sizeof expect) == 0, "CONNECT bytes (%d)", n);
    CHECK(m.state == MQTT_CONNECTING, "connecting");
    CHECK(!mqtt_publish(&m, "t", (const uint8_t *)"x", 1), "no publish before CONNACK");
    mqtt_input(&m, connack, 2, 1.0); /* in two pieces */
    mqtt_input(&m, connack + 2, 2, 1.0);
    CHECK(m.state == MQTT_READY, "ready after CONNACK");
    CHECK(mqtt_subscribe(&m, "cwtty/1/ab"), "subscribe queued");
    n = take(&m, buf);
    CHECK(n == 2 + 2 + 2 + 10 + 1 && buf[0] == 0x82 && buf[1] == 15 && buf[3] == 1 && buf[n - 1] == 0,
          "SUBSCRIBE bytes (%d)", n);
}

static void test_mqtt_publish_lengths(void)
{
    static mqtt_client m;
    static received r;
    uint8_t buf[MQTT_OUT_CAP], payload[300];
    int n;

    printf("mqtt: PUBLISH and the remaining length encoding\n");
    connected(&m, &r);
    memset(payload, 0xAB, sizeof payload);
    /* Body = 2 + 1 (topic) + payload. */
    CHECK(mqtt_publish(&m, "t", payload, 124), "publish 127-byte body");
    n = take(&m, buf);
    CHECK(n == 1 + 1 + 127 && buf[0] == 0x30 && buf[1] == 127, "one length byte for 127 (%d)", n);
    CHECK(mqtt_publish(&m, "t", payload, 125), "publish 128-byte body");
    n = take(&m, buf);
    CHECK(n == 1 + 2 + 128 && buf[1] == 0x80 && buf[2] == 0x01, "two length bytes for 128");
    CHECK(mqtt_publish(&m, "t", payload, 297), "publish 300-byte body");
    n = take(&m, buf);
    CHECK(n == 1 + 2 + 300 && buf[1] == (0x80 | (300 & 127)) && buf[2] == 300 >> 7, "300 encoded");
}

static void test_mqtt_parser(void)
{
    static mqtt_client m;
    static received r;
    uint8_t stream[4096], big[MQTT_IN_CAP + 100];
    int n = 0, i;

    printf("mqtt: incoming messages split, merged and oversized\n");
    connected(&m, &r);
    n += broker_publish(stream + n, "cwtty/1/x", (const uint8_t *)"hello", 5);
    n += broker_publish(stream + n, "cwtty/1/y", (const uint8_t *)"world!", 6);
    for (i = 0; i < n; i++) /* one byte at a time */
        mqtt_input(&m, stream + i, 1, 2.0);
    CHECK(r.count == 2, "two messages (%d)", r.count);
    CHECK(strcmp(r.topic, "cwtty/1/y") == 0 && r.len == 6 && memcmp(r.payload, "world!", 6) == 0,
          "the second message intact");

    memset(big, 'z', sizeof big);
    n = broker_publish(stream, "cwtty/1/big", big, (int)sizeof big - 20);
    n += broker_publish(stream + n, "cwtty/1/ok", (const uint8_t *)"ok", 2);
    mqtt_input(&m, stream, n, 3.0); /* all at once */
    CHECK(r.count == 3 && strcmp(r.topic, "cwtty/1/ok") == 0, "oversized message skipped, next one read");
    CHECK(m.state == MQTT_READY, "still connected");
}

static void test_mqtt_qos1_suback(void)
{
    static mqtt_client m;
    static received r;
    static const uint8_t qos1[] = {0x32, 0x07, 0x00, 0x01, 't', 0x12, 0x34, 'h', 'i'};
    static const uint8_t suback[] = {0x90, 0x03, 0x00, 0x01, 0x80};
    uint8_t buf[MQTT_OUT_CAP];
    int n;

    printf("mqtt: QoS 1 delivery is acknowledged, a refused SUBSCRIBE noticed\n");
    connected(&m, &r);
    mqtt_input(&m, qos1, sizeof qos1, 2.0);
    CHECK(r.count == 1 && r.len == 2 && memcmp(r.payload, "hi", 2) == 0, "QoS 1 payload");
    n = take(&m, buf);
    CHECK(n == 4 && buf[0] == 0x40 && buf[2] == 0x12 && buf[3] == 0x34, "PUBACK with the packet id");
    mqtt_input(&m, suback, sizeof suback, 3.0);
    CHECK(m.sub_denied, "denied subscription reported");
}

static void test_mqtt_keepalive(void)
{
    static mqtt_client m;
    static received r;
    static const uint8_t pingresp[] = {0xD0, 0x00};
    static const uint8_t refused[] = {0x20, 0x02, 0x00, 0x05};
    uint8_t buf[MQTT_OUT_CAP];
    int n;

    printf("mqtt: keepalive pings, a silent broker, refusals and timeouts\n");
    connected(&m, &r);
    mqtt_tick(&m, 10000.0);
    CHECK(take(&m, buf) == 0, "no ping before half the keepalive");
    mqtt_tick(&m, 15000.0);
    n = take(&m, buf);
    CHECK(n == 2 && buf[0] == 0xC0 && buf[1] == 0, "PINGREQ at half the keepalive");
    mqtt_input(&m, pingresp, 2, 15100.0);
    CHECK(mqtt_tick(&m, 40000.0) == MQTT_READY, "alive after PINGRESP");
    take(&m, buf);
    CHECK(mqtt_tick(&m, 71000.0) == MQTT_FAILED && m.error == MQTT_ERR_TIMEOUT, "silent broker detected");

    mqtt_init(&m, NULL, NULL);
    mqtt_connect(&m, "c", NULL, NULL, 30, 0.0);
    mqtt_input(&m, refused, sizeof refused, 1.0);
    CHECK(m.state == MQTT_FAILED && m.error == MQTT_ERR_REFUSED && m.refused == 5, "refused session");

    mqtt_init(&m, NULL, NULL);
    mqtt_connect(&m, "c", NULL, NULL, 30, 0.0);
    CHECK(mqtt_tick(&m, 5000.0) == MQTT_CONNECTING, "waiting for CONNACK");
    CHECK(mqtt_tick(&m, 11000.0) == MQTT_FAILED && m.error == MQTT_ERR_TIMEOUT, "CONNACK timeout");
}

/* ------------------------------------------------------------------------ */
/* Room                                                                      */
/* ------------------------------------------------------------------------ */

#define SIM_ROOMS 3
#define SIM_FLIGHTS 4096
#define SIM_KEYS 1024

/* Two simulated brokers deliver every packet to every room (the sender
 * too, as MQTT does), each with its own delay, jitter and losses. */
typedef struct {
    double  at;
    int     to, len;
    uint8_t data[ROOM_PACKET_CAP];
} flight;

typedef struct {
    double delay, jitter, loss;
} broker;

typedef struct {
    room_t   room[SIM_ROOMS];
    int      nrooms;
    broker   brokers[2];
    flight   air[SIM_FLIGHTS];
    int      nair;
    int      deliver; /* 0: packets are lost from now on */
    double   t;
    /* What room 1 heard. */
    char     text[256];
    int      joins, leaves;
    room_vkey keys[SIM_KEYS];
    int      nkeys;
    int      tampered; /* corrupt the next packet of room 0 */
} sim;

static double frand(void)
{
    return (double)rng_u32() / 4294967296.0;
}

static void sim_init(sim *x, int nrooms)
{
    uint8_t seed[32];
    int i, k;
    memset(x, 0, sizeof *x);
    x->nrooms = nrooms;
    x->deliver = 1;
    for (i = 0; i < nrooms; i++) {
        for (k = 0; k < 32; k++)
            seed[k] = (uint8_t)(rng_u32() >> 24);
        room_init(&x->room[i], seed);
        room_settings(&x->room[i], 20, 700, 600, 40.0);
    }
    x->brokers[0].delay = 60.0;
    x->brokers[1].delay = 90.0;
}

static void sim_send(sim *x, int from, const uint8_t *data, int len)
{
    int b, to;
    for (b = 0; b < 2; b++) {
        for (to = 0; to < x->nrooms; to++) {
            flight *f;
            if (!x->deliver || frand() < x->brokers[b].loss || x->nair == SIM_FLIGHTS)
                continue;
            f = &x->air[x->nair++];
            f->at = x->t + x->brokers[b].delay + x->brokers[b].jitter * frand();
            f->to = to;
            f->len = len;
            memcpy(f->data, data, (size_t)len);
            if (from == 0 && x->tampered && to == 1 && b == 0)
                f->data[len / 2] ^= 0x40; /* the other broker's copy is intact */
        }
    }
    if (from == 0)
        x->tampered = 0;
}

/* Advances the simulation by one millisecond. */
static void sim_step(sim *x)
{
    uint8_t buf[ROOM_PACKET_CAP];
    room_event e;
    room_vkey k;
    int i, n;

    for (i = 0; i < x->nair;) {
        if (x->air[i].at <= x->t) {
            room_receive(&x->room[x->air[i].to], x->air[i].data, x->air[i].len, x->t);
            x->air[i] = x->air[--x->nair];
        } else {
            i++;
        }
    }
    for (i = 0; i < x->nrooms; i++) {
        room_update(&x->room[i], x->t, x->t);
        while ((n = room_pop_out(&x->room[i], buf, sizeof buf)) > 0)
            sim_send(x, i, buf, n);
        while (room_pop_event(&x->room[i], &e)) {
            if (i != 1)
                continue;
            if (e.kind == RE_JOIN)
                x->joins++;
            else if (e.kind == RE_LEAVE)
                x->leaves++;
            else if (e.kind == RE_TOKEN && strlen(x->text) + strlen(e.tok.text) < sizeof x->text)
                strcat(x->text, e.tok.text);
        }
        while (room_pop_vkey(&x->room[i], &k))
            if (i == 1 && x->nkeys < SIM_KEYS)
                x->keys[x->nkeys++] = k;
    }
    x->t += 1.0;
}

static void sim_run(sim *x, double until)
{
    while (x->t < until)
        sim_step(x);
}

/* The transitions of a text keyed from time t0. */
typedef struct {
    double t[SIM_KEYS];
    int    on[SIM_KEYS], n;
    double at;
} keying;

static void emit_segment(void *ctx, int on, double ms)
{
    keying *k = (keying *)ctx;
    if (k->n < SIM_KEYS && (k->n == 0 ? on : k->on[k->n - 1] != on)) {
        k->t[k->n] = k->at;
        k->on[k->n++] = on;
    }
    k->at += ms;
}

static void make_keying(keying *k, const char *text, double t0)
{
    memset(k, 0, sizeof *k);
    k->at = t0;
    morse_timing(text, 20, 20, emit_segment, k);
    if (k->n < SIM_KEYS && k->on[k->n - 1]) {
        k->t[k->n] = k->at;
        k->on[k->n++] = 0;
    }
}

/* Runs the simulation while room 0 keys k. */
static void sim_key(sim *x, const keying *k, double until)
{
    int i = 0;
    while (x->t < until) {
        while (i < k->n && k->t[i] <= x->t) {
            room_key(&x->room[0], k->on[i], k->t[i]);
            i++;
        }
        sim_step(x);
    }
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

/* The heard key changes keep the sent intervals. */
static int same_rhythm(const sim *x, const keying *k, double tolerance)
{
    int i;
    if (x->nkeys != k->n)
        return 0;
    for (i = 1; i < k->n; i++) {
        double sent = k->t[i] - k->t[i - 1], heard = x->keys[i].t - x->keys[i - 1].t;
        if (x->keys[i].on != k->on[i] || heard < sent - tolerance || heard > sent + tolerance)
            return 0;
    }
    return 1;
}

static void test_room_names(void)
{
    char a[64], b[64];
    printf("room: callsigns and room names are cleaned\n");
    strcpy(a, "iz0abc/p\x1b[2J");
    CHECK(room_clean_call(a) == 10 && strcmp(a, "IZ0ABC/P2J") == 0, "callsign: '%s'", a);
    strcpy(b, "  qrq club  ");
    CHECK(room_clean_name(b) == 8 && strcmp(b, "QRQ CLUB") == 0, "room name: '%s'", b);
    strcpy(a, "*-*");
    CHECK(room_clean_call(a) == 0, "nothing valid left");
}

static void test_room_loopback(void)
{
    static sim x;
    static keying k;

    printf("room: keyed text is heard and decoded with its rhythm\n");
    rng_seed(42);
    sim_init(&x, 2);
    x.brokers[0].jitter = 30.0;
    x.brokers[1].jitter = 30.0;
    CHECK(room_join(&x.room[0], "test1", "Test", NULL, 20, 0.0) == 0, "join A");
    CHECK(room_join(&x.room[1], "test2", "test", NULL, 20, 0.0) == 0, "join B");
    CHECK(strcmp(x.room[0].topic, x.room[1].topic) == 0 && strncmp(x.room[0].topic, "cwtty/1/", 8) == 0,
          "same topic: %s", x.room[0].topic);
    sim_run(&x, 2000.0);
    CHECK(x.joins == 1, "B sees A once (%d)", x.joins);
    CHECK(x.room[0].st[0].used && strcmp(x.room[0].st[0].call, "TEST2") == 0, "A sees B");
    CHECK(!x.room[0].st[1].used, "A does not see itself");

    make_keying(&k, "CQ CQ DE TEST1 K", 2000.0);
    sim_key(&x, &k, k.at + 3000.0);
    trim(x.text);
    CHECK(strcmp(x.text, "CQ CQ DE TEST1 K") == 0, "decoded '%s'", x.text);
    CHECK(same_rhythm(&x, &k, 1.5), "rhythm kept (%d key changes heard, %d sent)", x.nkeys, k.n);
    CHECK(x.nkeys > 0 && x.keys[0].tone_hz != 600, "a pitch away from the own tone (%d)", x.keys[0].tone_hz);
    CHECK(x.nkeys > 0 && x.keys[0].t - k.t[0] < 400.0, "delay %.0f ms", x.nkeys ? x.keys[0].t - k.t[0] : 0.0);

    room_leave(&x.room[0], x.t);
    sim_run(&x, x.t + 500.0);
    CHECK(x.leaves == 1, "B sees A leave");
}

static void test_room_losses(void)
{
    static sim x;
    static keying k;

    printf("room: losses on one broker, reordering and duplicates\n");
    rng_seed(7);
    sim_init(&x, 2);
    x.brokers[0].loss = 0.3;
    x.brokers[0].jitter = 80.0;
    x.brokers[1].delay = 70.0;
    x.brokers[1].jitter = 80.0;
    room_join(&x.room[0], "test1", "loss", NULL, 20, 0.0);
    room_join(&x.room[1], "test2", "loss", NULL, 20, 0.0);
    sim_run(&x, 2000.0);
    make_keying(&k, "PARIS PARIS 73", 2000.0);
    sim_key(&x, &k, k.at + 3000.0);
    trim(x.text);
    CHECK(strcmp(x.text, "PARIS PARIS 73") == 0, "decoded '%s'", x.text);
    CHECK(x.nkeys == k.n, "every key change heard once (%d of %d)", x.nkeys, k.n);
}

static void test_room_private(void)
{
    static sim x;
    static keying k;

    printf("room: private rooms, wrong passwords and forged packets\n");
    rng_seed(99);
    sim_init(&x, 3);
    room_join(&x.room[0], "test1", "club", "s3cret pass", 20, 0.0);
    room_join(&x.room[1], "test2", "club", "s3cret pass", 20, 0.0);
    room_join(&x.room[2], "test3", "club", "wrong", 20, 0.0);
    CHECK(x.room[0].priv && strcmp(x.room[0].topic, x.room[1].topic) == 0, "same private topic");
    CHECK(strcmp(x.room[0].topic, x.room[2].topic) != 0, "another password, another topic");
    sim_run(&x, 2000.0);
    CHECK(x.joins == 1, "B sees A (%d)", x.joins);
    CHECK(!x.room[2].st[0].used, "C sees nobody");

    /* C publishes on A's topic anyway: A and B must ignore it. */
    strcpy(x.room[2].topic, x.room[0].topic);
    room_key(&x.room[2], 1, 2000.0);
    room_key(&x.room[2], 0, 2100.0);
    x.tampered = 1;
    make_keying(&k, "EE", 2050.0);
    sim_key(&x, &k, 4000.0);
    trim(x.text);
    CHECK(x.joins == 1 && strcmp(x.text, "EE") == 0, "only A heard: '%s'", x.text);
}

static void test_room_stuck_key(void)
{
    static sim x;
    int i, last_on = -1;
    double up = 0.0;

    printf("room: a key held when the packets stop is released\n");
    rng_seed(5);
    sim_init(&x, 2);
    room_join(&x.room[0], "test1", "stuck", NULL, 20, 0.0);
    room_join(&x.room[1], "test2", "stuck", NULL, 20, 0.0);
    sim_run(&x, 2000.0);
    room_key(&x.room[0], 1, 2000.0);
    sim_run(&x, 2500.0);
    x.deliver = 0; /* the network goes away with the key down */
    sim_run(&x, 4000.0);
    for (i = 0; i < x.nkeys; i++) {
        last_on = x.keys[i].on;
        if (!x.keys[i].on)
            up = x.keys[i].t;
    }
    CHECK(last_on == 0, "released");
    CHECK(up > 2500.0 && up < 3300.0, "released at %.0f ms", up);
}

/* ------------------------------------------------------------------------ */
/* Links                                                                     */
/* ------------------------------------------------------------------------ */

/* A tiny in-memory broker: exact topics, QoS 0. */
typedef struct {
    int     open;
    uint8_t in[8192];
    int     in_len;
    char    sub[LINK_TOPIC_CAP];
    uint8_t out[8192];
    int     out_len;
    int     connects, disconnects;
} fake_conn;

typedef struct {
    link_t    link[2];
    fake_conn conn[2];
    int       got[2];
    char      last[2][64];
} fake_net;

static fake_net *the_net;

static void got0(void *ctx, const uint8_t *payload, int len)
{
    (void)ctx;
    the_net->got[0]++;
    memcpy(the_net->last[0], payload, (size_t)(len < 63 ? len : 63));
    the_net->last[0][len < 63 ? len : 63] = 0;
}

static void got1(void *ctx, const uint8_t *payload, int len)
{
    (void)ctx;
    the_net->got[1]++;
    memcpy(the_net->last[1], payload, (size_t)(len < 63 ? len : 63));
    the_net->last[1][len < 63 ? len : 63] = 0;
}

static void fake_broker(fake_net *f, int c)
{
    fake_conn *k = &f->conn[c];
    for (;;) {
        uint32_t rem = 0, mult = 1;
        int at = 1, type, o;
        if (k->in_len < 2)
            return;
        do {
            if (at >= k->in_len)
                return;
            rem += (uint32_t)(k->in[at] & 127) * mult;
            mult *= 128;
        } while (k->in[at++] & 128);
        if (at + (int)rem > k->in_len)
            return;
        type = k->in[0] >> 4;
        if (type == 1) { /* CONNECT */
            static const uint8_t ack[] = {0x20, 2, 0, 0};
            memcpy(k->out + k->out_len, ack, 4);
            k->out_len += 4;
            k->connects++;
        } else if (type == 8) { /* SUBSCRIBE */
            int tl = (k->in[at + 2] << 8) | k->in[at + 3];
            uint8_t ack[5];
            memcpy(k->sub, k->in + at + 4, (size_t)tl);
            k->sub[tl] = 0;
            ack[0] = 0x90;
            ack[1] = 3;
            ack[2] = k->in[at];
            ack[3] = k->in[at + 1];
            ack[4] = 0;
            memcpy(k->out + k->out_len, ack, 5);
            k->out_len += 5;
        } else if (type == 10) { /* UNSUBSCRIBE */
            k->sub[0] = 0;
        } else if (type == 3) { /* PUBLISH: to every subscriber */
            int tl = (k->in[at] << 8) | k->in[at + 1];
            for (o = 0; o < 2; o++) {
                fake_conn *d = &f->conn[o];
                if (d->open && (int)strlen(d->sub) == tl && memcmp(d->sub, k->in + at + 2, (size_t)tl) == 0) {
                    memcpy(d->out + d->out_len, k->in, (size_t)at + rem);
                    d->out_len += at + (int)rem;
                }
            }
        } else if (type == 12) { /* PINGREQ */
            k->out[k->out_len++] = 0xD0;
            k->out[k->out_len++] = 0;
        } else if (type == 14) {
            k->disconnects++;
        }
        memmove(k->in, k->in + at + rem, (size_t)(k->in_len - at - (int)rem));
        k->in_len -= at + (int)rem;
    }
}

static void fake_step(fake_net *f, double t)
{
    int i;
    for (i = 0; i < 2; i++) {
        link_t *l = &f->link[i];
        fake_conn *k = &f->conn[i];
        int act = link_update(l, t), n;
        const uint8_t *p;
        if (act == LINK_OPEN) {
            k->open = 1;
            k->in_len = k->out_len = 0;
            k->sub[0] = 0;
            link_opened(l, t);
        } else if (act == LINK_CLOSE) {
            k->open = 0;
            link_closed(l, t, NULL);
        }
        if (!k->open)
            continue;
        p = link_output(l, &n);
        memcpy(k->in + k->in_len, p, (size_t)n);
        k->in_len += n;
        link_consume(l, n);
        fake_broker(f, i);
    }
    for (i = 0; i < 2; i++) {
        fake_conn *k = &f->conn[i];
        if (k->open && k->out_len) {
            link_input(&f->link[i], k->out, k->out_len, t);
            k->out_len = 0;
        }
    }
}

static void test_links(void)
{
    static fake_net f;
    double t = 0.0;

    printf("link: connect, publish, reconnect after a drop, leave\n");
    memset(&f, 0, sizeof f);
    the_net = &f;
    link_init(&f.link[0], "cwtty-a", "user", "pass", got0, NULL);
    link_init(&f.link[1], "cwtty-b", NULL, NULL, got1, NULL);
    link_start(&f.link[0], "cwtty/1/room", t);
    link_start(&f.link[1], "cwtty/1/room", t);
    for (; t < 200.0; t += 10.0)
        fake_step(&f, t);
    CHECK(link_up(&f.link[0]) && link_up(&f.link[1]), "both up");
    CHECK(link_publish(&f.link[0], (const uint8_t *)"hello", 5), "published");
    for (; t < 300.0; t += 10.0)
        fake_step(&f, t);
    CHECK(f.got[1] == 1 && strcmp(f.last[1], "hello") == 0, "B got it (%d)", f.got[1]);
    CHECK(f.got[0] == 1, "A got its own (%d)", f.got[0]);

    /* B's connection drops. */
    f.conn[1].open = 0;
    link_closed(&f.link[1], t, "reset");
    CHECK(!link_up(&f.link[1]) && link_retry_in(&f.link[1], t) == 1, "B waits a second");
    CHECK(strcmp(f.link[1].reason, "reset") == 0, "reason kept");
    CHECK(!link_publish(&f.link[1], (const uint8_t *)"x", 1), "no publishing while down");
    for (; t < 1500.0; t += 10.0)
        fake_step(&f, t);
    CHECK(link_up(&f.link[1]) && f.conn[1].connects == 2, "B reconnected");
    link_publish(&f.link[1], (const uint8_t *)"again", 5);
    for (; t < 1600.0; t += 10.0)
        fake_step(&f, t);
    CHECK(f.got[0] == 2 && strcmp(f.last[0], "again") == 0, "A got B's message");

    /* Keepalive over a long idle time. */
    for (; t < 100000.0; t += 50.0)
        fake_step(&f, t);
    CHECK(link_up(&f.link[0]) && link_up(&f.link[1]), "alive after 100 s idle");

    /* A moves to another topic, then leaves. */
    link_start(&f.link[0], "cwtty/1/other", t);
    for (; t < 100200.0; t += 10.0)
        fake_step(&f, t);
    CHECK(strcmp(f.conn[0].sub, "cwtty/1/other") == 0, "subscription moved");
    link_stop(&f.link[0], t);
    for (; t < 100400.0; t += 10.0)
        fake_step(&f, t);
    CHECK(f.conn[0].disconnects == 1 && !f.conn[0].open && f.link[0].state == LINK_IDLE, "left cleanly");
}

/* ------------------------------------------------------------------------ */
/* Where a transmission ends                                                 */
/* ------------------------------------------------------------------------ */

/* Feeds a text as decoded tokens ("<SK>" is a prosign, " " a word space);
 * returns what the last token said. */
static int over_text(over_t *o, const char *text)
{
    dec_token t;
    int r = OVER_NONE;
    while (*text) {
        memset(&t, 0, sizeof t);
        if (*text == '<') {
            const char *end = strchr(text, '>');
            size_t n = (size_t)(end - text) + 1;
            t.kind = TOK_PROSIGN;
            memcpy(t.text, text, n);
            text += n;
        } else {
            t.kind = *text == ' ' ? TOK_SPACE : TOK_CHAR;
            t.text[0] = *text++;
        }
        r = over_token(o, &t);
    }
    return r;
}

static void test_over(void)
{
    over_t o;
    printf("over: K, KN, BK, AR and SK pass the turn, BT starts a paragraph\n");
    over_reset(&o);
    CHECK(over_text(&o, "CQ CQ DE IZ0ABC K ") == OVER_END, "K as a word");
    CHECK(over_text(&o, "CQ DE K1ABC ") == OVER_NONE, "a callsign starting with K");
    CHECK(over_text(&o, "OK ") == OVER_NONE, "the word OK");
    CHECK(over_text(&o, "IZ0ABC DE IZ0XYZ KN ") == OVER_END, "KN in letters");
    CHECK(over_text(&o, "BK ") == OVER_END, "BK");
    CHECK(over_text(&o, "TU<SK>") == OVER_END, "the SK prosign, even glued to a word");
    CHECK(over_text(&o, "73<AR>") == OVER_END, "the AR prosign");
    CHECK(over_text(&o, "NAME GIO = ") == OVER_BREAK, "BT as =");
    CHECK(over_text(&o, "K") == OVER_NONE, "K not yet followed by a word space");
    CHECK(over_text(&o, " ") == OVER_END, "then the space ends it");
}

static int word(const char *w)
{
    return abbr_word(w, (int)strlen(w));
}

static void test_abbr(void)
{
    printf("abbr: signs and abbreviations are told apart from text\n");
    CHECK(word("K") == WORD_SIGN && word("KN") == WORD_SIGN && word("SK") == WORD_SIGN, "turn signs");
    CHECK(word("<AR>") == WORD_SIGN && word("=") == WORD_SIGN, "prosigns and BT");
    CHECK(word("QRL?") == WORD_ABBR && word("QTH") == WORD_ABBR && word("QSL.") == WORD_ABBR, "Q-codes");
    CHECK(word("QUI") == WORD_TEXT && word("QUA") == WORD_TEXT, "Italian words are not Q-codes");
    CHECK(word("CQ") == WORD_ABBR && word("DE") == WORD_ABBR && word("73") == WORD_ABBR, "the usual ones");
    CHECK(word("599") == WORD_ABBR && word("5NN") == WORD_ABBR && word("579") == WORD_ABBR, "signal reports");
    CHECK(word("K1ABC") == WORD_TEXT && word("OK") == WORD_TEXT && word("HELLO") == WORD_TEXT, "plain words");
    CHECK(word("?") == WORD_TEXT && word("") == WORD_TEXT, "nothing to classify");
}

static void test_transcript_marks(void)
{
    static transcript t;
    const tr_line *l;
    printf("transcript: a complete word gets the style of its kind\n");
    transcript_clear(&t);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "Q", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "R", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "L", STY_NORMAL);
    l = transcript_line(&t, 0);
    CHECK(l->style[0] == STY_NORMAL, "not before the word is complete");
    transcript_append_who(&t, DIR_RX, "IZ0AAA", " ", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "H", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "I", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", " ", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "K", STY_NORMAL);
    transcript_break_who(&t, "IZ0AAA"); /* the line ends without a space */
    l = transcript_line(&t, 0);
    CHECK(l->style[0] == STY_ABBR && l->style[2] == STY_ABBR, "QRL as an abbreviation");
    CHECK(l->style[4] == STY_ABBR, "HI as an abbreviation");
    CHECK(l->style[7] == STY_SIGN, "K as a sign, at the end of the line");
}

static void test_transcript_lines(void)
{
    static transcript t;
    printf("transcript: a line per station, closed on demand\n");
    transcript_clear(&t);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "CQ", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0BBB", "QRZ", STY_NORMAL);
    transcript_append_who(&t, DIR_RX, "IZ0AAA", " DE", STY_NORMAL);
    CHECK(transcript_count(&t) == 2, "two stations, two lines (%d)", transcript_count(&t));
    CHECK(strcmp(transcript_line(&t, 0)->text, "CQ DE") == 0, "the first station's line grew");
    CHECK(transcript_open_who(&t, "IZ0AAA") && transcript_open_who(&t, "IZ0BBB"), "both open");
    transcript_break_who(&t, "IZ0AAA");
    CHECK(!transcript_open_who(&t, "IZ0AAA") && transcript_open_who(&t, "IZ0BBB"), "one closed");
    transcript_append_who(&t, DIR_RX, "IZ0AAA", "K", STY_NORMAL);
    CHECK(transcript_count(&t) == 3 && strcmp(transcript_line(&t, 2)->who, "IZ0AAA") == 0, "a new line after it");
}

int main(void)
{
    test_voice_timing();
    test_voice_limiter();
    test_voice_late_and_reset();
    test_mqtt_connect();
    test_mqtt_publish_lengths();
    test_mqtt_parser();
    test_mqtt_qos1_suback();
    test_mqtt_keepalive();
    test_room_names();
    test_room_loopback();
    test_room_losses();
    test_room_private();
    test_room_stuck_key();
    test_links();
    test_over();
    test_transcript_lines();
    test_abbr();
    test_transcript_marks();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
