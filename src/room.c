/*
 * room.c - The protocol of an online CW room.
 */
#include "room.h"

#include <string.h>

#include "monocypher.h"

#include "util.h"

#define VERSION 1
#define FLAG_ENCRYPTED 1

enum { T_HELLO = 1, T_KEY = 2, T_BYE = 3 };

#define BODY_HEAD 7          /* type, station, seq */
#define SEAL (24 + 16)       /* nonce and mac */
#define BATCH_MS 120.0       /* KEY packets: about eight a second at most */
#define HELLO_EVERY_MS 20000.0
#define STATION_TIMEOUT_MS 60000.0
#define GAP_WAIT_MS 300.0    /* a missing packet may come from the other broker */
#define SILENCE_MS 1000.0    /* the jitter buffer adapts in silences */
#define STUCK_MS 300.0       /* key held without news: let it go */
#define WARMUP_MS 100.0      /* extra margin until the delay is known */
#define VOICE_TAIL_MS 50.0   /* the envelope's fall, and some */
#define TONE_STEP 60
#define TONE_GUARD 40

/* Argon2id for the private rooms: 256 KiB, 4 passes. */
#define KDF_BLOCKS 256
#define KDF_PASSES 4
static uint64_t kdf_area[KDF_BLOCKS * 128];

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static unsigned get16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void rng(room_t *r, void *buf, size_t n)
{
    static const uint8_t nonce[8] = {0};
    r->rng_ctr = crypto_chacha20_djb((uint8_t *)buf, NULL, n, r->rng_key, nonce, r->rng_ctr);
}

static uint32_t rng32(room_t *r)
{
    uint8_t b[4];
    rng(r, b, sizeof b);
    return get32(b);
}

static void hex(char *out, const uint8_t *data, int n)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * n] = 0;
}

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

int room_clean_call(char *call)
{
    int n = 0, i;
    for (i = 0; call[i]; i++) {
        int c = upper((unsigned char)call[i]);
        if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/') && n < ROOM_CALL_CAP - 1)
            call[n++] = (char)c;
    }
    call[n] = 0;
    return n;
}

int room_clean_name(char *name)
{
    int n = 0, i, start = 0;
    while (name[start] == ' ')
        start++;
    for (i = start; name[i]; i++) {
        int c = upper((unsigned char)name[i]);
        if (c >= 0x20 && c < 0x7F && n < ROOM_NAME_CAP - 1)
            name[n++] = (char)c;
    }
    while (n > 0 && name[n - 1] == ' ')
        n--;
    name[n] = 0;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Queues                                                                    */
/* ------------------------------------------------------------------------ */

static void event(room_t *r, int kind, int station)
{
    room_event *e;
    if (r->ev_count == ROOM_EVENTS) { /* the host is not reading: drop the oldest */
        r->ev_head = (r->ev_head + 1) % ROOM_EVENTS;
        r->ev_count--;
    }
    e = &r->ev[(r->ev_head + r->ev_count) % ROOM_EVENTS];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->station = station;
    e->voice = r->st[station].voice;
    r->ev_count++;
}

int room_pop_event(room_t *r, room_event *e)
{
    if (r->ev_count == 0)
        return 0;
    *e = r->ev[r->ev_head];
    r->ev_head = (r->ev_head + 1) % ROOM_EVENTS;
    r->ev_count--;
    return 1;
}

static void vkey(room_t *r, int voice, int tone, int on, double t)
{
    room_vkey *k;
    if (voice < 0 || r->vk_count == ROOM_VKEYS)
        return;
    k = &r->vk[(r->vk_head + r->vk_count) % ROOM_VKEYS];
    k->voice = voice;
    k->tone_hz = tone;
    k->on = on;
    k->t = t;
    r->vk_count++;
}

int room_pop_vkey(room_t *r, room_vkey *k)
{
    if (r->vk_count == 0)
        return 0;
    *k = r->vk[r->vk_head];
    r->vk_head = (r->vk_head + 1) % ROOM_VKEYS;
    r->vk_count--;
    return 1;
}

int room_pop_out(room_t *r, uint8_t *buf, int cap)
{
    room_packet *p;
    int n;
    if (r->out_count == 0)
        return 0;
    p = &r->out[r->out_head];
    n = p->len <= cap ? p->len : 0;
    memcpy(buf, p->data, (size_t)n);
    r->out_head = (r->out_head + 1) % ROOM_OUT_QUEUE;
    r->out_count--;
    return n;
}

/* Seals a body into a packet and queues it. */
static void send_body(room_t *r, const uint8_t *body, int len)
{
    room_packet *p;
    if (r->out_count == ROOM_OUT_QUEUE || 2 + SEAL + len > ROOM_PACKET_CAP)
        return;
    p = &r->out[(r->out_head + r->out_count) % ROOM_OUT_QUEUE];
    p->data[0] = VERSION;
    if (r->priv) {
        uint8_t ad[2 + ROOM_TOPIC_CAP];
        size_t tl = strlen(r->topic);
        p->data[1] = FLAG_ENCRYPTED;
        memcpy(ad, p->data, 2);
        memcpy(ad + 2, r->topic, tl);
        rng(r, p->data + 2, 24);
        crypto_aead_lock(p->data + 2 + SEAL, p->data + 2 + 24, r->key, p->data + 2, ad, 2 + tl, body, (size_t)len);
        p->len = 2 + SEAL + len;
    } else {
        p->data[1] = 0;
        memcpy(p->data + 2, body, (size_t)len);
        p->len = 2 + len;
    }
    r->out_count++;
}

static int body_head(room_t *r, uint8_t *b, int type)
{
    b[0] = (uint8_t)type;
    put32(b + 1, r->id);
    put16(b + 5, r->seq);
    r->seq++;
    return BODY_HEAD;
}

static void send_hello(room_t *r, int ask)
{
    uint8_t b[BODY_HEAD + 3 + ROOM_CALL_CAP];
    int n = body_head(r, b, T_HELLO), cl = (int)strlen(r->call);
    b[n++] = (uint8_t)(ask ? 1 : 0);
    b[n++] = (uint8_t)clampi(r->wpm, 5, 80);
    b[n++] = (uint8_t)cl;
    memcpy(b + n, r->call, (size_t)cl);
    send_body(r, b, n + cl);
}

/* Sends the keying batch (possibly empty: the key is still held). */
static void send_key(room_t *r, uint32_t t_send, double now)
{
    uint8_t b[BODY_HEAD + 10 + 2 * ROOM_KEY_BATCH];
    int n = body_head(r, b, T_KEY), i;
    if (r->nbatch == 0)
        r->batch_t0 = t_send;
    put32(b + n, r->batch_t0);
    put32(b + n + 4, t_send);
    b[n + 8] = (uint8_t)r->key_on;
    b[n + 9] = (uint8_t)r->nbatch;
    n += 10;
    for (i = 0; i < r->nbatch; i++, n += 2)
        put16(b + n, r->batch[i]);
    send_body(r, b, n);
    r->nbatch = 0;
    r->key_tx_at = now;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------ */

void room_init(room_t *r, const uint8_t seed[32])
{
    int k;
    memset(r, 0, sizeof *r);
    memcpy(r->rng_key, seed, 32);
    r->wpm = 20;
    r->base_tone = 700;
    r->own_tone = 600;
    r->margin = 40.0;
    for (k = 0; k < ROOM_VOICES; k++)
        r->owner[k] = -1;
}

void room_settings(room_t *r, int wpm, int base_hz, int own_hz, double margin_ms)
{
    r->wpm = clampi(wpm, 5, 80);
    r->base_tone = clampi(base_hz, 300, 1500);
    r->own_tone = own_hz;
    r->margin = clampd(margin_ms, 20.0, 500.0);
}

static void derive_id(uint8_t out[16], const char *label, const char *name)
{
    uint8_t msg[32 + ROOM_NAME_CAP];
    size_t ll = strlen(label) + 1, nl = strlen(name);
    memcpy(msg, label, ll); /* with its terminator: labels never run into names */
    memcpy(msg + ll, name, nl);
    crypto_blake2b(out, 16, msg, ll + nl);
}

int room_join(room_t *r, const char *call, const char *name, const char *password, int wpm, double now)
{
    uint8_t id[16];
    int k;

    if (r->joined)
        room_leave(r, now);
    str_copy(r->call, sizeof r->call, call ? call : "");
    str_copy(r->name, sizeof r->name, name ? name : "");
    if (!room_clean_call(r->call))
        return ROOM_BAD_CALL;
    if (!room_clean_name(r->name))
        return ROOM_BAD_NAME;
    r->wpm = clampi(wpm, 5, 80);
    r->priv = password && *password;
    if (r->priv) {
        crypto_argon2_config cfg;
        crypto_argon2_inputs in;
        uint8_t salt[16], master[32];
        derive_id(salt, "cwtty-salt", r->name);
        cfg.algorithm = CRYPTO_ARGON2_ID;
        cfg.nb_blocks = KDF_BLOCKS;
        cfg.nb_passes = KDF_PASSES;
        cfg.nb_lanes = 1;
        in.pass = (const uint8_t *)password;
        in.pass_size = (uint32_t)strlen(password);
        in.salt = salt;
        in.salt_size = sizeof salt;
        crypto_argon2(master, sizeof master, kdf_area, cfg, in, crypto_argon2_no_extras);
        crypto_blake2b_keyed(r->key, 32, master, 32, (const uint8_t *)"encrypt", 7);
        crypto_blake2b_keyed(id, 16, master, 32, (const uint8_t *)"topic", 5);
        crypto_wipe(master, sizeof master);
        crypto_wipe(kdf_area, sizeof kdf_area);
    } else {
        derive_id(id, "cwtty-public", r->name);
    }
    memcpy(r->topic, "cwtty/1/", 8);
    hex(r->topic + 8, id, 16);

    memset(r->st, 0, sizeof r->st);
    for (k = 0; k < ROOM_VOICES; k++) {
        r->owner[k] = -1;
        r->free_at[k] = 0.0;
    }
    r->ev_count = r->vk_count = r->out_count = 0;
    r->nbatch = 0;
    r->key_on = 0;
    do
        r->id = rng32(r);
    while (r->id == 0);
    r->seq = (uint16_t)rng32(r);
    r->joined = 1;
    r->reply_at = 0.0;
    r->ask_at = now;
    r->hello_at = now + HELLO_EVERY_MS;
    send_hello(r, 1);
    return 0;
}

void room_resync(room_t *r)
{
    int s;
    for (s = 0; s < ROOM_STATIONS; s++) {
        r->st[s].have_offset = 0;
        r->st[s].noffs = r->st[s].offi = 0;
    }
}

void room_announce(room_t *r, double now)
{
    if (!r->joined)
        return;
    r->ask_at = now;
    send_hello(r, 1);
}

static void drop_station(room_t *r, int s, double now, int announce)
{
    room_station *st = &r->st[s];
    if (st->voice >= 0) {
        if (st->play_on)
            vkey(r, st->voice, st->tone_hz, 0, now + 1.0);
        r->owner[st->voice] = -1;
        r->free_at[st->voice] = now;
    }
    if (announce)
        event(r, RE_LEAVE, s);
    st->used = 0;
}

void room_leave(room_t *r, double now)
{
    int s;
    if (!r->joined)
        return;
    if (r->key_on || r->nbatch) {
        r->key_on = 0;
        send_key(r, r->batch_last, now);
    }
    {
        uint8_t b[BODY_HEAD];
        send_body(r, b, body_head(r, b, T_BYE));
    }
    for (s = 0; s < ROOM_STATIONS; s++)
        if (r->st[s].used)
            drop_station(r, s, now, 0);
    r->joined = 0;
    crypto_wipe(r->key, sizeof r->key);
}

/* ------------------------------------------------------------------------ */
/* Sending                                                                   */
/* ------------------------------------------------------------------------ */

void room_key(room_t *r, int on, double t)
{
    uint32_t ms;
    uint32_t delta;
    on = on ? 1 : 0;
    if (!r->joined || on == r->key_on)
        return;
    ms = (uint32_t)(t > 0.0 ? t + 0.5 : 0.0);
    if (r->nbatch == 0) {
        r->batch_t0 = r->batch_last = ms;
        r->batch_at = t;
    }
    delta = ms - r->batch_last;
    if (delta > 0x7FFF)
        delta = 0x7FFF;
    r->batch[r->nbatch++] = (uint16_t)(((uint32_t)on << 15) | delta);
    r->batch_last = ms;
    r->key_on = on;
    if (r->nbatch == ROOM_KEY_BATCH)
        send_key(r, ms, t);
}

/* ------------------------------------------------------------------------ */
/* Receiving                                                                 */
/* ------------------------------------------------------------------------ */

static int find_station(const room_t *r, uint32_t id)
{
    int s;
    for (s = 0; s < ROOM_STATIONS; s++)
        if (r->st[s].used && r->st[s].id == id)
            return s;
    return -1;
}

/* The first pitch of the sequence base, +1, -1, +2, -2... steps that no
 * other station has and that is away from the operator's own tone. */
static int pick_tone(const room_t *r, int self)
{
    int i, s;
    for (i = 0; i < 2 * ROOM_STATIONS + 8; i++) {
        int step = (i + 1) / 2 * (i % 2 ? 1 : -1);
        int hz = r->base_tone + step * TONE_STEP, taken = 0;
        if (hz < 250 || hz > 2000)
            continue;
        if (hz > r->own_tone - TONE_GUARD && hz < r->own_tone + TONE_GUARD)
            continue;
        for (s = 0; s < ROOM_STATIONS; s++)
            if (s != self && r->st[s].used && r->st[s].tone_hz == hz)
                taken = 1;
        if (!taken)
            return hz;
    }
    return r->base_tone;
}

static double unwrap(room_station *st, uint32_t raw)
{
    if (!st->have_raw) {
        st->raw0 = raw;
        st->have_raw = 1;
    }
    return (double)st->raw0 + (double)(int32_t)(raw - st->raw0);
}

/* A voice for a station about to key down. */
static int take_voice(room_t *r, int s, double now)
{
    int k, best = -1;
    for (k = 0; k < ROOM_VOICES; k++) {
        int o = r->owner[k];
        if (o < 0 || (!r->st[o].play_on && r->free_at[k] + VOICE_TAIL_MS < now)) {
            if (best < 0 || r->free_at[k] < r->free_at[best])
                best = k;
        }
    }
    if (best >= 0) {
        if (r->owner[best] >= 0)
            r->st[r->owner[best]].voice = -1;
        r->owner[best] = s;
        r->st[s].voice = best;
    }
    return best;
}

/* Plays a transition of station s at room clock tl (sender time ts). */
static void schedule(room_t *r, int s, double ts, double tl, int on, double now)
{
    room_station *st = &r->st[s];
    if (on == st->play_on)
        return;
    if (on && st->voice < 0)
        take_voice(r, s, now);
    if (st->voice >= 0) {
        vkey(r, st->voice, st->tone_hz, on, tl);
        r->free_at[st->voice] = tl;
    }
    if (st->due_count < ROOM_DUE) {
        room_due *d = &st->due[(st->due_head + st->due_count) % ROOM_DUE];
        d->ts = ts;
        d->tl = tl;
        d->on = on;
        st->due_count++;
    }
    st->active = tl;
    st->play_on = on;
}

/* The playout offset for a station that has been silent: the largest
 * recent delay plus the margin. */
static double fresh_offset(const room_t *r, const room_station *st)
{
    double m = st->offs[0];
    int i;
    for (i = 1; i < st->noffs; i++)
        if (st->offs[i] > m)
            m = st->offs[i];
    return m + r->margin + (st->noffs < 8 ? WARMUP_MS : 0.0);
}

static void on_key(room_t *r, int s, const uint8_t *b, int len, double now)
{
    room_station *st = &r->st[s];
    double t0, tsend, ts, first;
    int n, i, state;
    if (len < BODY_HEAD + 10)
        return;
    t0 = unwrap(st, get32(b + BODY_HEAD));
    tsend = unwrap(st, get32(b + BODY_HEAD + 4));
    state = b[BODY_HEAD + 8] ? 1 : 0;
    n = b[BODY_HEAD + 9];
    if (n > ROOM_KEY_BATCH || len < BODY_HEAD + 10 + 2 * n)
        return;

    first = n > 0 ? t0 + (double)(get16(b + BODY_HEAD + 10) & 0x7FFF) : tsend;
    st->offs[st->offi] = now - first;
    st->offi = (st->offi + 1) % ROOM_OFFSETS;
    if (st->noffs < ROOM_OFFSETS)
        st->noffs++;
    if (!st->have_offset || (!st->play_on && now - st->active > SILENCE_MS)) {
        st->offset = fresh_offset(r, st);
        st->have_offset = 1;
    }

    ts = t0;
    for (i = 0; i < n; i++) {
        unsigned v = get16(b + BODY_HEAD + 10 + 2 * i);
        double tl;
        ts += (double)(v & 0x7FFF);
        tl = ts + st->offset;
        if (tl < now + 2.0) { /* late: stretch rather than lose it */
            st->offset += now + 2.0 - tl;
            tl = now + 2.0;
        }
        if (tl <= st->active)
            tl = st->active + 1.0;
        schedule(r, s, ts, tl, (v >> 15) & 1, now);
    }
    if (tsend > st->covered)
        st->covered = tsend;
    /* A transition lost on every broker: the state at t_send says it. */
    if (state != st->play_on) {
        double tl = tsend + st->offset;
        if (tl < now + 2.0)
            tl = now + 2.0;
        if (tl <= st->active)
            tl = st->active + 1.0;
        schedule(r, s, tl - st->offset, tl, state, now);
    }
}

static void on_hello(room_t *r, int s, const uint8_t *b, int len, double now)
{
    room_station *st = &r->st[s];
    char call[ROOM_CALL_CAP];
    int n;
    if (len < BODY_HEAD + 3)
        return;
    n = b[BODY_HEAD + 2];
    if (n >= ROOM_CALL_CAP || len < BODY_HEAD + 3 + n)
        return;
    memcpy(call, b + BODY_HEAD + 3, (size_t)n);
    call[n] = 0;
    if (room_clean_call(call))
        str_copy(st->call, sizeof st->call, call);
    st->wpm = clampi(b[BODY_HEAD + 1], 5, 80);
    if ((b[BODY_HEAD] & 1) && r->reply_at == 0.0) /* asked: answer, at a random moment */
        r->reply_at = now + 200.0 + (double)(rng32(r) % 1300);
}

/* Handles a body that is next in the station's sequence. */
static void process(room_t *r, int s, const uint8_t *b, int len, double now)
{
    room_station *st = &r->st[s];
    st->next_seq = (uint16_t)(get16(b + 5) + 1);
    if (b[0] == T_KEY)
        on_key(r, s, b, len, now);
    else if (b[0] == T_HELLO)
        on_hello(r, s, b, len, now);
}

/* Handles the packets waiting for a missing one, in order, up to the first
 * gap; with skip, gaps are given up. */
static void drain(room_t *r, int s, double now, int skip)
{
    room_station *st = &r->st[s];
    for (;;) {
        int i, best = -1;
        int16_t bd = 0;
        for (i = 0; i < st->npend; i++) {
            int16_t d = (int16_t)(st->pend[i].seq - st->next_seq);
            if (best < 0 || d < bd) {
                best = i;
                bd = d;
            }
        }
        if (best < 0 || (bd > 0 && !skip))
            return;
        if (bd >= 0)
            process(r, s, st->pend[best].body, st->pend[best].len, now);
        st->pend[best] = st->pend[--st->npend];
        if (!r->st[s].used)
            return;
    }
}

static void on_body(room_t *r, const uint8_t *b, int len, double now)
{
    uint32_t id;
    uint16_t seq;
    int s, type;
    int16_t d;
    room_station *st;

    if (len < BODY_HEAD)
        return;
    type = b[0];
    id = get32(b + 1);
    seq = (uint16_t)get16(b + 5);
    if (id == r->id) /* our own echo */
        return;
    s = find_station(r, id);

    if (s < 0) {
        if (type == T_HELLO) {
            for (s = 0; s < ROOM_STATIONS && r->st[s].used; s++) {
            }
            if (s == ROOM_STATIONS)
                return;
            st = &r->st[s];
            memset(st, 0, sizeof *st);
            st->used = 1;
            st->id = id;
            st->voice = -1;
            st->heard = st->active = now;
            str_copy(st->call, sizeof st->call, "?");
            st->tone_hz = pick_tone(r, s);
            dec_init(&st->dec, clampi(len > BODY_HEAD + 1 ? b[BODY_HEAD + 1] : 20, 5, 80));
            process(r, s, b, len, now);
            if (st->call[0] == '?') { /* no valid callsign */
                st->used = 0;
                return;
            }
            event(r, RE_JOIN, s);
        } else if (type == T_KEY && now - r->ask_at > 2000.0) {
            r->ask_at = now; /* someone we missed: ask everybody */
            send_hello(r, 1);
        }
        return;
    }

    st = &r->st[s];
    st->heard = now;
    if (type == T_BYE) {
        drop_station(r, s, now, 1);
        return;
    }
    d = (int16_t)(seq - st->next_seq);
    if (d < 0) /* a duplicate from the other broker, or given up on */
        return;
    if (d == 0) {
        process(r, s, b, len, now);
        drain(r, s, now, 0);
    } else if (d > 64) { /* far ahead: start over from here */
        st->npend = 0;
        process(r, s, b, len, now);
    } else {
        int i;
        for (i = 0; i < st->npend; i++)
            if (st->pend[i].seq == seq)
                return;
        if (st->npend == ROOM_REORDER) {
            drain(r, s, now, 1);
            if (!r->st[s].used || st->npend == ROOM_REORDER)
                return;
        }
        st->pend[st->npend].seq = seq;
        st->pend[st->npend].arrived = now;
        st->pend[st->npend].len = len;
        memcpy(st->pend[st->npend].body, b, (size_t)len);
        st->npend++;
    }
}

void room_receive(room_t *r, const uint8_t *data, int len, double now)
{
    uint8_t body[ROOM_PACKET_CAP];
    if (!r->joined || len < 2 || len > ROOM_PACKET_CAP || data[0] != VERSION)
        return;
    if (r->priv) {
        uint8_t ad[2 + ROOM_TOPIC_CAP];
        size_t tl = strlen(r->topic);
        int n = len - 2 - SEAL;
        if (data[1] != FLAG_ENCRYPTED || n < BODY_HEAD)
            return;
        memcpy(ad, data, 2);
        memcpy(ad + 2, r->topic, tl);
        if (crypto_aead_unlock(body, data + 2 + 24, r->key, data + 2, ad, 2 + tl, data + 2 + SEAL, (size_t)n) != 0)
            return; /* forged, damaged, or another password */
        on_body(r, body, n, now);
    } else {
        if (data[1] != 0)
            return;
        on_body(r, data + 2, len - 2, now);
    }
}

/* ------------------------------------------------------------------------ */
/* Timers                                                                    */
/* ------------------------------------------------------------------------ */

static void station_update(room_t *r, int s, double now)
{
    room_station *st = &r->st[s];
    dec_token tok;
    int i;

    if (now - st->heard > STATION_TIMEOUT_MS) {
        drop_station(r, s, now, 1);
        return;
    }
    /* Give up waiting for a missing packet. */
    for (i = 0; i < st->npend; i++) {
        if (now - st->pend[i].arrived > GAP_WAIT_MS) {
            drain(r, s, now, 1);
            break;
        }
    }
    if (!st->used)
        return;
    /* The key is held but the station went quiet. */
    if (st->play_on && now > st->covered + st->offset + STUCK_MS) {
        double tl = now + 2.0 > st->active + 1.0 ? now + 2.0 : st->active + 1.0;
        schedule(r, s, tl - st->offset, tl, 0, now);
    }
    /* Transitions due now: the decoder and the tapes. */
    while (st->due_count > 0 && st->due[st->due_head].tl <= now) {
        room_due *d = &st->due[st->due_head];
        dec_key(&st->dec, d->on, d->ts);
        if (d->ts > st->dec_now)
            st->dec_now = d->ts;
        event(r, RE_KEY, s);
        r->ev[(r->ev_head + r->ev_count - 1) % ROOM_EVENTS].on = d->on;
        r->ev[(r->ev_head + r->ev_count - 1) % ROOM_EVENTS].t = d->tl;
        st->due_head = (st->due_head + 1) % ROOM_DUE;
        st->due_count--;
    }
    if (st->have_offset && now - st->offset > st->dec_now)
        st->dec_now = now - st->offset;
    dec_update(&st->dec, st->dec_now);
    while (dec_pop(&st->dec, &tok)) {
        event(r, RE_TOKEN, s);
        r->ev[(r->ev_head + r->ev_count - 1) % ROOM_EVENTS].tok = tok;
    }
}

void room_update(room_t *r, double now, double known)
{
    int s;
    if (!r->joined)
        return;
    for (s = 0; s < ROOM_STATIONS; s++)
        if (r->st[s].used)
            station_update(r, s, now);

    if (r->nbatch > 0 && now - r->batch_at >= BATCH_MS) {
        uint32_t k = (uint32_t)(known > 0.0 ? known + 0.5 : 0.0);
        send_key(r, (int32_t)(k - r->batch_last) > 0 ? k : r->batch_last, now);
    } else if (r->key_on && r->nbatch == 0 && now - r->key_tx_at >= BATCH_MS) {
        uint32_t k = (uint32_t)(known > 0.0 ? known + 0.5 : 0.0);
        send_key(r, (int32_t)(k - r->batch_last) > 0 ? k : r->batch_last, now);
    }
    if (now >= r->hello_at) {
        r->hello_at = now + HELLO_EVERY_MS;
        send_hello(r, 0);
    }
    if (r->reply_at != 0.0 && now >= r->reply_at) {
        r->reply_at = 0.0;
        send_hello(r, 0);
    }
}
