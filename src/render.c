/*
 * render.c - Double-buffered cell renderer with 24-bit colour.
 */
#include "render.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term.h"
#include "util.h"

static int sw, sh;
static cell_t *back, *front;
static int full_redraw = 1;
static int use_truecolor = 1;
static rect_t clip;

static char *out;
static size_t out_len, out_cap;

void scr_init(int truecolor)
{
    use_truecolor = truecolor;
}

void scr_shutdown(void)
{
    free(back);
    free(front);
    free(out);
    back = front = NULL;
    out = NULL;
    out_len = out_cap = 0;
    sw = sh = 0;
}

void scr_resize(int w, int h)
{
    size_t n;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w == sw && h == sh && back)
        return;
    n = (size_t)w * (size_t)h;
    free(back);
    free(front);
    back = (cell_t *)calloc(n, sizeof(cell_t));
    front = (cell_t *)calloc(n, sizeof(cell_t));
    sw = w;
    sh = h;
    full_redraw = 1;
    scr_unclip();
}

int scr_w(void) { return sw; }
int scr_h(void) { return sh; }

void scr_clip(rect_t r)
{
    int x0 = maxi(r.x, 0), y0 = maxi(r.y, 0);
    int x1 = mini(r.x + r.w, sw), y1 = mini(r.y + r.h, sh);
    clip = rect_make(x0, y0, maxi(0, x1 - x0), maxi(0, y1 - y0));
}

void scr_unclip(void)
{
    clip = rect_make(0, 0, sw, sh);
}

void scr_begin(uint32_t bg)
{
    int i, n = sw * sh;
    for (i = 0; i < n; i++) {
        back[i].ch = ' ';
        back[i].fg = 0xFFFFFF;
        back[i].bg = bg;
        back[i].attr = 0;
    }
    scr_unclip();
}

cell_t *scr_cell(int x, int y)
{
    if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h)
        return NULL;
    return &back[y * sw + x];
}

void scr_put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int attr)
{
    cell_t *c = scr_cell(x, y);
    if (!c)
        return;
    c->ch = ch;
    if (fg != COLOR_KEEP)
        c->fg = fg;
    if (bg != COLOR_KEEP)
        c->bg = bg;
    c->attr = (uint16_t)attr;
}

int scr_text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int attr)
{
    int x0 = x;
    uint32_t cp;
    if (!s)
        return 0;
    while (*s) {
        int n = utf8_decode(s, &cp);
        if (n <= 0)
            break;
        s += n;
        scr_put(x++, y, cp, fg, bg, attr);
    }
    return x - x0;
}

int scr_textf(int x, int y, uint32_t fg, uint32_t bg, int attr, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return scr_text(x, y, buf, fg, bg, attr);
}

void scr_fill(rect_t r, uint32_t ch, uint32_t fg, uint32_t bg)
{
    int x, y;
    for (y = r.y; y < r.y + r.h; y++)
        for (x = r.x; x < r.x + r.w; x++)
            scr_put(x, y, ch, fg, bg, 0);
}

void scr_hline(int x, int y, int w, uint32_t ch, uint32_t fg, uint32_t bg)
{
    int i;
    for (i = 0; i < w; i++)
        scr_put(x + i, y, ch, fg, bg, 0);
}

void scr_vline(int x, int y, int h, uint32_t ch, uint32_t fg, uint32_t bg)
{
    int i;
    for (i = 0; i < h; i++)
        scr_put(x, y + i, ch, fg, bg, 0);
}

void scr_box(rect_t r, box_style style, uint32_t fg, uint32_t bg)
{
    /* tl, tr, bl, br, horizontal, vertical */
    static const uint32_t glyphs[4][6] = {
        {0x256D, 0x256E, 0x2570, 0x256F, 0x2500, 0x2502}, /* round  */
        {0x250C, 0x2510, 0x2514, 0x2518, 0x2500, 0x2502}, /* light  */
        {0x250F, 0x2513, 0x2517, 0x251B, 0x2501, 0x2503}, /* heavy  */
        {0x2554, 0x2557, 0x255A, 0x255D, 0x2550, 0x2551}, /* double */
    };
    const uint32_t *g = glyphs[style];
    if (r.w < 2 || r.h < 2)
        return;
    scr_hline(r.x + 1, r.y, r.w - 2, g[4], fg, bg);
    scr_hline(r.x + 1, r.y + r.h - 1, r.w - 2, g[4], fg, bg);
    scr_vline(r.x, r.y + 1, r.h - 2, g[5], fg, bg);
    scr_vline(r.x + r.w - 1, r.y + 1, r.h - 2, g[5], fg, bg);
    scr_put(r.x, r.y, g[0], fg, bg, 0);
    scr_put(r.x + r.w - 1, r.y, g[1], fg, bg, 0);
    scr_put(r.x, r.y + r.h - 1, g[2], fg, bg, 0);
    scr_put(r.x + r.w - 1, r.y + r.h - 1, g[3], fg, bg, 0);
}

uint32_t rgb_mix(uint32_t a, uint32_t b, float t)
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r, g, bl;
    t = clampf(t, 0.0f, 1.0f);
    r = (int)(ar + (br - ar) * t + 0.5f);
    g = (int)(ag + (bg - ag) * t + 0.5f);
    bl = (int)(ab + (bb - ab) * t + 0.5f);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

uint32_t rgb_scale(uint32_t c, float k)
{
    return rgb_mix(0x000000, c, k);
}

void scr_shade(rect_t r, float k)
{
    int x, y;
    for (y = r.y; y < r.y + r.h; y++) {
        for (x = r.x; x < r.x + r.w; x++) {
            cell_t *c = scr_cell(x, y);
            if (!c)
                continue;
            c->fg = rgb_scale(c->fg, k);
            c->bg = rgb_scale(c->bg, k);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

static void out_reserve(size_t extra)
{
    if (out_len + extra <= out_cap)
        return;
    out_cap = (out_len + extra) * 2 + 4096;
    out = (char *)realloc(out, out_cap);
}

static void out_mem(const char *s, size_t n)
{
    out_reserve(n);
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void out_str(const char *s)
{
    out_mem(s, strlen(s));
}

static void out_int(int v)
{
    char buf[16];
    int n = 0, i;
    if (v < 0)
        v = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    out_reserve((size_t)n);
    for (i = n - 1; i >= 0; i--)
        out[out_len++] = buf[i];
}

/* Nearest xterm-256 colour for terminals without 24-bit support. */
static int rgb_to_256(uint32_t c)
{
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    int ri, gi, bi, gray, gi2, cube_d, gray_d, cr, cg, cb, gv;
#define IDX(v) ((v) < 48 ? 0 : (v) < 115 ? 1 : ((v) - 35) / 40)
    ri = IDX(r);
    gi = IDX(g);
    bi = IDX(b);
#undef IDX
    cr = levels[ri];
    cg = levels[gi];
    cb = levels[bi];
    cube_d = (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);
    gray = (r + g + b) / 3;
    gi2 = gray < 8 ? 0 : (gray > 238 ? 23 : (gray - 8) / 10);
    gv = 8 + gi2 * 10;
    gray_d = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    if (gray_d < cube_d)
        return 232 + gi2;
    return 16 + 36 * ri + 6 * gi + bi;
}

static void out_color(int is_bg, uint32_t c)
{
    out_str(is_bg ? ";48;" : ";38;");
    if (use_truecolor) {
        out_str("2;");
        out_int((int)((c >> 16) & 255));
        out_mem(";", 1);
        out_int((int)((c >> 8) & 255));
        out_mem(";", 1);
        out_int((int)(c & 255));
    } else {
        out_str("5;");
        out_int(rgb_to_256(c));
    }
}

static int same_cell(const cell_t *a, const cell_t *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

void scr_present(void)
{
    int x, y, cx = -1, cy = -1, cattr = -1;
    uint32_t cfg = 0, cbg = 0;
    int have_style = 0;

    if (!back)
        return;
    out_len = 0;
    out_str("\x1b[?2026h");
    if (full_redraw)
        out_str("\x1b[0m\x1b[2J");

    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            cell_t *b = &back[y * sw + x];
            cell_t *f = &front[y * sw + x];
            char u[4];
            int n;
            uint32_t ch;

            if (!full_redraw && same_cell(b, f))
                continue;
            if (cx != x || cy != y) {
                out_str("\x1b[");
                out_int(y + 1);
                out_mem(";", 1);
                out_int(x + 1);
                out_mem("H", 1);
            }
            if (b->attr != cattr || !have_style || b->fg != cfg || b->bg != cbg) {
                /* Always a full SGR: reset, attributes, both colours. */
                out_str("\x1b[0");
                if (b->attr & A_BOLD)
                    out_str(";1");
                if (b->attr & A_ITALIC)
                    out_str(";3");
                if (b->attr & A_UNDERLINE)
                    out_str(";4");
                out_color(0, b->fg);
                out_color(1, b->bg);
                out_mem("m", 1);
                cattr = b->attr;
                cfg = b->fg;
                cbg = b->bg;
                have_style = 1;
            }
            ch = b->ch;
            if (ch < 0x20 || ch == 0x7F)
                ch = ' ';
            n = utf8_encode(ch, u);
            out_mem(u, (size_t)n);
            *f = *b;
            cx = x + 1;
            cy = y;
        }
    }
    out_str("\x1b[0m\x1b[?2026l");
    full_redraw = 0;
    term_write(out, out_len);
}
