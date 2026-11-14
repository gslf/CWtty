/*
 * room.h - The protocol of an online CW room, independent of the transport
 * and of the audio backend.
 *
 * The operator's key transitions go out in small packets; each remote
 * station's arrive here, are put back in order, played on a voice of their
 * own (a pitch per station) after a jitter buffer, and decoded to text with
 * the station's callsign. Every packet is published on all the brokers at
 * once: duplicates are dropped by sequence number.
 *
 * Times are milliseconds of the "room clock", which is the audio playback
 * clock: the voices are scheduled on it. Nothing here does I/O; the host
 * takes the packets to publish, the voice key changes and the events.
 *
 * Packets (version 1): [u8 version][u8 flags] then the body, or, in a
 * private room, [nonce 24][mac 16][body encrypted with XChaCha20-Poly1305]
 * with the header and the topic as associated data. Bodies start with
 * [u8 type][u32 station][u16 seq]:
 *   HELLO  [u8 flags: 1 = asks the others to introduce themselves]
 *          [u8 wpm][u8 n][n bytes of callsign]
 *   KEY    [u32 t0][u32 t_send][u8 key state at t_send][u8 n]
 *          [n x u16: bit 15 the new state, bits 0-14 ms since the previous
 *          transition (the first: since t0)]
 *   BYE
 * Times are the sender's clock in ms. Transitions are batched for 120 ms
 * (EMQX allows ten messages a second per client). While its key is held
 * down, a station repeats an empty KEY, so a lost key-up cannot hold a
 * tone.
 */
#ifndef CWTTY_ROOM_H
#define CWTTY_ROOM_H

#include <stdint.h>

#include "decoder.h"

#define ROOM_STATIONS 16
#define ROOM_VOICES 8 /* VOICES of voices.h */
#define ROOM_CALL_CAP 11
#define ROOM_NAME_CAP 25
#define ROOM_TOPIC_CAP 48
#define ROOM_PACKET_CAP 200
#define ROOM_OUT_QUEUE 16
#define ROOM_EVENTS 128
#define ROOM_VKEYS 256
#define ROOM_DUE 128
#define ROOM_OFFSETS 32
#define ROOM_REORDER 8
#define ROOM_KEY_BATCH 40

/* Events for the host. */
enum {
    RE_JOIN,  /* a station appeared */
    RE_LEAVE, /* a station left or went silent for a minute */
    RE_TOKEN, /* a station's decoded character or word space */
    RE_KEY    /* a station's key changed now (for the tapes) */
};

/* room_join() errors. */
enum { ROOM_BAD_CALL = -1, ROOM_BAD_NAME = -2 };

typedef struct {
    int       kind;
    int       station; /* index in room_t.st */
    int       voice, on;
    double    t;
    dec_token tok;
} room_event;

/* A key change of a voice at room clock t, to pass to the audio backend
 * (set the voice to tone_hz first). */
typedef struct {
    int    voice, tone_hz, on;
    double t;
} room_vkey;

typedef struct {
    double ts, tl; /* sender time, room clock */
    int    on;
} room_due;

typedef struct {
    uint16_t seq;
    double   arrived;
    int      len;
    uint8_t  body[ROOM_PACKET_CAP];
} room_pending;

typedef struct {
    int      used;
    uint32_t id;
    char     call[ROOM_CALL_CAP];
    int      wpm, tone_hz, voice;
    double   heard; /* room clock of the last packet */

    uint16_t     next_seq;
    room_pending pend[ROOM_REORDER];
    int          npend;

    int      have_raw;
    uint32_t raw0; /* first sender time seen, for unwrapping */

    /* Jitter buffer: room clock = sender time + offset. */
    double offs[ROOM_OFFSETS];
    int    noffs, offi;
    double offset;
    int    have_offset;
    double active;  /* room clock of the last scheduled transition */
    int    play_on; /* the station's key, as scheduled */
    double covered; /* sender time up to which its keying is known */

    room_due   due[ROOM_DUE];
    int        due_head, due_count;
    double     dec_now;
    cw_decoder dec;
} room_station;

typedef struct {
    uint8_t data[ROOM_PACKET_CAP];
    int     len;
} room_packet;

typedef struct {
    int      joined, priv;
    uint32_t id;
    char     call[ROOM_CALL_CAP];
    char     name[ROOM_NAME_CAP];
    char     topic[ROOM_TOPIC_CAP];
    int      wpm;
    uint8_t  key[32];

    uint8_t  rng_key[32];
    uint64_t rng_ctr;

    /* Sending. */
    uint16_t seq;
    uint16_t batch[ROOM_KEY_BATCH];
    int      nbatch;
    uint32_t batch_t0, batch_last;
    double   batch_at;
    int      key_on;
    double   key_tx_at; /* last KEY sent */
    double   hello_at, reply_at, ask_at;

    room_packet out[ROOM_OUT_QUEUE];
    int         out_head, out_count;

    room_station st[ROOM_STATIONS];
    int          owner[ROOM_VOICES]; /* station using each voice, -1: free */
    double       free_at[ROOM_VOICES];

    room_event ev[ROOM_EVENTS];
    int        ev_head, ev_count;
    room_vkey  vk[ROOM_VKEYS];
    int        vk_head, vk_count;

    int    base_tone, own_tone;
    double margin; /* playout margin, ms: covers the audio period */
} room_t;

/* seed: 32 bytes from a cryptographic random generator. */
void room_init(room_t *r, const uint8_t seed[32]);

/* Clean a callsign ([A-Z0-9/], at most 10) or a room name (printable
 * ASCII, at most 24, upper case, trimmed) in place. Return the length, 0
 * when nothing valid is left. */
int room_clean_call(char *call);
int room_clean_name(char *name);

/* Enters a room: a password makes it private (and takes a moment: the key
 * is derived with Argon2). Returns 0, ROOM_BAD_CALL or ROOM_BAD_NAME. */
int  room_join(room_t *r, const char *call, const char *name, const char *password, int wpm, double now);
void room_leave(room_t *r, double now);

/* Introduces us again and asks the others to: when a broker connects. */
void room_announce(room_t *r, double now);
/* Forgets the measured delays (the room clock stopped for a while). */
void room_resync(room_t *r);

/* Pitches of the remote stations: around base_hz, avoiding own_hz. */
void room_settings(room_t *r, int wpm, int base_hz, int own_hz, double margin_ms);

/* The operator's key changed at room clock t. */
void room_key(room_t *r, int on, double t);

/* A message of the room's topic arrived. */
void room_receive(room_t *r, const uint8_t *data, int len, double now);

/* Timers: playout, decoding, batching, presence. known: the room clock up
 * to which every operator transition has been passed to room_key(). */
void room_update(room_t *r, double now, double known);

/* What the host takes after each call. Packets wait in the room while no
 * broker is connected (the newest are dropped when the queue is full). */
int room_pop_out(room_t *r, uint8_t *buf, int cap); /* a packet to publish */
int room_pop_event(room_t *r, room_event *e);
int room_pop_vkey(room_t *r, room_vkey *k);

#endif
