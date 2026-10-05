/*
 * keyer.c - Sample-accurate electronic keyer.
 *
 * Semantics:
 *   - Both iambic modes alternate dits and dahs while squeezing, and have
 *     dit/dah memory so that a quick tap during an element is not lost.
 *   - Iambic A stops right after the current element when the squeeze is
 *     released; memories latched only because of the squeeze are dropped.
 *   - Iambic B samples the opposite paddle during each element and sends
 *     one extra opposite element after a squeeze release.
 *   - Ultimatic: when squeezing, the paddle pressed last wins and repeats.
 *   - Bug: the dit paddle makes automatic dits, the dah paddle is manual
 *     (handled outside the keyer, as a straight key).
 */
#include "keyer.h"

#include <string.h>

enum { ST_IDLE, ST_MARK, ST_GAP };
enum { E_DIT = 0, E_DAH = 1 };

const char *keyer_mode_name(int mode)
{
    switch (mode) {
    case KEYER_IAMBIC_A: return "Iambic A";
    case KEYER_IAMBIC_B: return "Iambic B";
    case KEYER_ULTIMATIC: return "Ultimatic";
    case KEYER_BUG: return "Bug";
    default: return "?";
    }
}

void keyer_init(keyer_t *k, int mode, uint32_t dit_len)
{
    memset(k, 0, sizeof *k);
    keyer_set(k, mode, dit_len);
}

void keyer_set(keyer_t *k, int mode, uint32_t dit_len)
{
    k->mode = mode;
    k->dit_len = dit_len > 0 ? dit_len : 1;
}

void keyer_reset(keyer_t *k)
{
    keyer_init(k, k->mode, k->dit_len);
}

int keyer_busy(const keyer_t *k)
{
    return k->state != ST_IDLE;
}

static void start_element(keyer_t *k, int e)
{
    k->elem = e;
    k->state = ST_MARK;
    k->remain = k->dit_len * (e == E_DAH ? 3u : 1u);
    if (e == E_DIT)
        k->dit_mem = k->dit_mem_sq = 0;
    else
        k->dah_mem = k->dah_mem_sq = 0;
}

static void latch(keyer_t *k, int dit, int dah, int dit_edge, int dah_edge)
{
    if (dit_edge) {
        k->dit_mem = 1;
        k->dit_mem_sq = dah;
    }
    if (dah_edge) {
        k->dah_mem = 1;
        k->dah_mem_sq = dit;
    }
    if (k->mode == KEYER_IAMBIC_B && k->state == ST_MARK) {
        if (k->elem == E_DIT && dah) {
            k->dah_mem = 1;
            k->dah_mem_sq = 0;
        } else if (k->elem == E_DAH && dit) {
            k->dit_mem = 1;
            k->dit_mem_sq = 0;
        }
    }
}

static void decide_next(keyer_t *k, int dit, int dah)
{
    int want_dit, want_dah, next = -1;

    if (k->mode == KEYER_IAMBIC_A && !dit && !dah) {
        if (k->dit_mem_sq)
            k->dit_mem = 0;
        if (k->dah_mem_sq)
            k->dah_mem = 0;
    }
    want_dit = dit || k->dit_mem;
    want_dah = dah || k->dah_mem;

    if (want_dit && want_dah) {
        if (k->mode == KEYER_ULTIMATIC) {
            if (dit && dah)
                next = k->last_pressed;
            else
                next = k->elem == E_DIT ? E_DAH : E_DIT;
        } else {
            next = k->elem == E_DIT ? E_DAH : E_DIT;
        }
    } else if (want_dit) {
        next = E_DIT;
    } else if (want_dah) {
        next = E_DAH;
    }

    if (next < 0) {
        k->state = ST_IDLE;
        k->dit_mem = k->dah_mem = 0;
        k->dit_mem_sq = k->dah_mem_sq = 0;
    } else {
        start_element(k, next);
    }
}

int keyer_tick(keyer_t *k, int dit, int dah, int dit_edge, int dah_edge)
{
    if (k->mode == KEYER_BUG) {
        dah = 0;
        dah_edge = 0;
    }
    if (dit && !k->prev_dit)
        dit_edge = 1;
    if (dah && !k->prev_dah)
        dah_edge = 1;
    k->prev_dit = dit;
    k->prev_dah = dah;
    if (dah_edge)
        k->last_pressed = E_DAH;
    if (dit_edge)
        k->last_pressed = E_DIT;

    switch (k->state) {
    case ST_IDLE:
        if (dit || dah || dit_edge || dah_edge) {
            int wd = dit || dit_edge, wa = dah || dah_edge;
            int e;
            if (wd && wa) {
                if (dit_edge && !dah_edge)
                    e = E_DIT;
                else if (dah_edge && !dit_edge)
                    e = E_DAH;
                else
                    e = k->mode == KEYER_ULTIMATIC ? k->last_pressed : E_DIT;
            } else {
                e = wd ? E_DIT : E_DAH;
            }
            start_element(k, e);
            /* Both paddles hit at once: remember the other one too. */
            if (dit_edge && dah_edge && k->mode != KEYER_ULTIMATIC) {
                if (e == E_DIT)
                    k->dah_mem = 1;
                else
                    k->dit_mem = 1;
            }
        }
        break;
    case ST_MARK:
        latch(k, dit, dah, dit_edge, dah_edge);
        if (--k->remain == 0) {
            k->state = ST_GAP;
            k->remain = k->dit_len;
        }
        break;
    case ST_GAP:
        latch(k, dit, dah, dit_edge, dah_edge);
        if (--k->remain == 0)
            decide_next(k, dit, dah);
        break;
    }
    return k->state == ST_MARK;
}
