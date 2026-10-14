/*
 * settings.c - User settings, persisted as a simple key=value file.
 */
#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyer.h"
#include "util.h"

const int RX_BANDWIDTHS[] = {50, 75, 100, 150, 200, 300, 500};
const int RX_BANDWIDTH_COUNT = ARRAY_LEN(RX_BANDWIDTHS);

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->tone_hz = 600;
    s->wpm = 20;
    s->farnsworth = 0;
    s->key_input = INPUT_KEYBOARD;
    s->keyer_mode = KEYER_IAMBIC_B;
    s->swap_paddles = 0;
    s->volume = 60;
    s->rise_ms = 5;

    s->rx_freq = 600;
    s->rx_bw = 100;
    s->rx_sens = 6;
    s->rx_afc = 1;
    s->rx_wpm = 20;
    s->rx_lock = 0;

    s->breakin_ms = 600;
    s->trx_sidetone = 1;

    s->latency_ms = 10;

    s->train_dir = 0;
    s->train_content = 0;
    s->train_charset = 0;
    s->train_koch = 6;
    s->train_group = 1;
    s->train_hint = 1;
    s->train_strict = 0;
}

int rx_bw_index(int hz)
{
    int i, best = 0;
    for (i = 1; i < RX_BANDWIDTH_COUNT; i++)
        if (llabs((long long)RX_BANDWIDTHS[i] - hz) < llabs((long long)RX_BANDWIDTHS[best] - hz))
            best = i;
    return best;
}

void settings_clamp(settings_t *s)
{
    s->tone_hz = clampi(s->tone_hz, 300, 1200);
    s->wpm = clampi(s->wpm, 5, 60);
    if (s->farnsworth >= s->wpm)
        s->farnsworth = 0;
    s->farnsworth = clampi(s->farnsworth, 0, 59);
    s->key_input = clampi(s->key_input, 0, INPUT_COUNT - 1);
    s->keyer_mode = clampi(s->keyer_mode, 0, KEYER_MODE_COUNT - 1);
    s->swap_paddles = !!s->swap_paddles;
    s->volume = clampi(s->volume, 0, 100);
    s->rise_ms = clampi(s->rise_ms, 1, 10);
    s->rx_freq = clampi(s->rx_freq, 200, 1500);
    s->rx_bw = RX_BANDWIDTHS[rx_bw_index(s->rx_bw)];
    s->rx_sens = clampi(s->rx_sens, 1, 10);
    s->rx_afc = !!s->rx_afc;
    s->rx_wpm = clampi(s->rx_wpm, 5, 60);
    s->rx_lock = !!s->rx_lock;
    s->breakin_ms = clampi(s->breakin_ms, 50, 3000);
    s->trx_sidetone = !!s->trx_sidetone;
    s->latency_ms = clampi(s->latency_ms, 5, 50);
    s->train_dir = clampi(s->train_dir, 0, 1);
    s->train_content = clampi(s->train_content, 0, 2);
    s->train_charset = clampi(s->train_charset, 0, 5);
    s->train_koch = clampi(s->train_koch, 2, 41);
    s->train_group = clampi(s->train_group, 1, 5);
    s->train_hint = !!s->train_hint;
    s->train_strict = !!s->train_strict;
}

typedef struct {
    const char *key;
    int         offset;
    int         is_string;
} field_t;

#define F_INT(name) {#name, (int)offsetof(settings_t, name), 0}
#define F_STR(name) {#name, (int)offsetof(settings_t, name), 1}

static const field_t FIELDS[] = {
    F_INT(tone_hz),       F_INT(wpm),           F_INT(farnsworth),    F_INT(keyer_mode),
    F_INT(swap_paddles),  F_INT(volume),        F_INT(rise_ms),       F_INT(rx_freq),
    F_INT(rx_bw),         F_INT(rx_sens),       F_INT(rx_afc),        F_INT(rx_wpm),
    F_INT(rx_lock),       F_INT(breakin_ms),    F_INT(trx_sidetone),  F_INT(latency_ms),
    F_STR(out_dev),       F_STR(in_dev),        F_INT(train_dir),     F_INT(train_content),
    F_INT(train_charset), F_INT(train_koch),    F_INT(train_group),   F_INT(train_hint),
    F_INT(train_strict),  F_INT(key_input),
};

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int settings_load(settings_t *s, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int i;
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val;
        key = trim(line);
        if (*key == '#' || *key == 0)
            continue;
        eq = strchr(key, '=');
        if (!eq)
            continue;
        *eq = 0;
        key = trim(key);
        val = trim(eq + 1);
        for (i = 0; i < ARRAY_LEN(FIELDS); i++) {
            char *p;
            if (strcmp(FIELDS[i].key, key) != 0)
                continue;
            p = (char *)s + FIELDS[i].offset;
            if (FIELDS[i].is_string)
                str_copy(p, SETTINGS_NAME_CAP, val);
            else {
                char *end;
                long value;
                errno = 0;
                value = strtol(val, &end, 10);
                if (end != val && *end == 0 && errno != ERANGE && value >= INT_MIN && value <= INT_MAX)
                    *(int *)p = (int)value;
            }
        }
    }
    fclose(f);
    settings_clamp(s);
    return 0;
}

int settings_save(const settings_t *s, const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f)
        return -1;
    fprintf(f, "# CWtty settings. Edit while the program is not running.\n");
    for (i = 0; i < ARRAY_LEN(FIELDS); i++) {
        const char *p = (const char *)s + FIELDS[i].offset;
        if (FIELDS[i].is_string)
            fprintf(f, "%s=%s\n", FIELDS[i].key, p);
        else
            fprintf(f, "%s=%d\n", FIELDS[i].key, *(const int *)p);
    }
    return fclose(f) == 0 ? 0 : -1;
}
