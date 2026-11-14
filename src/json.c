/*
 * json.c - Just enough JSON for the Filen API.
 */
#include "json.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Reader                                                                    */
/* ------------------------------------------------------------------------ */

static const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *skip_string(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\')
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return NULL;
}

/* Past the value at p, or NULL if there is none. */
static const char *skip_value(const char *p, const char *end, int depth)
{
    if (p >= end || depth > 64)
        return NULL;
    if (*p == '"')
        return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1, end);
        if (p < end && *p == close)
            return p + 1;
        for (;;) {
            if (close == '}') {
                if (p >= end || *p != '"' || !(p = skip_string(p, end)))
                    return NULL;
                p = ws(p, end);
                if (p >= end || *p != ':')
                    return NULL;
                p = ws(p + 1, end);
            }
            if (!(p = skip_value(p, end, depth + 1)))
                return NULL;
            p = ws(p, end);
            if (p < end && *p == ',') {
                p = ws(p + 1, end);
                continue;
            }
            return p < end && *p == close ? p + 1 : NULL;
        }
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        p++;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
            p++;
        return p;
    }
    if (end - p >= 4 && (memcmp(p, "true", 4) == 0 || memcmp(p, "null", 4) == 0))
        return p + 4;
    if (end - p >= 5 && memcmp(p, "false", 5) == 0)
        return p + 5;
    return NULL;
}

json_t json_doc(const char *text, int len)
{
    json_t v = {NULL, 0};
    const char *end = text + (len < 0 ? (int)strlen(text) : len), *p = ws(text, end), *q;
    q = skip_value(p, end, 0);
    if (q && ws(q, end) == end) {
        v.p = p;
        v.n = (int)(q - p);
    }
    return v;
}

int json_type(json_t v)
{
    if (!v.p || v.n <= 0)
        return JSON_NONE;
    switch (*v.p) {
    case '{': return JSON_OBJ;
    case '[': return JSON_ARR;
    case '"': return JSON_STR;
    case 't': return JSON_TRUE;
    case 'f': return JSON_FALSE;
    case 'n': return JSON_NULL;
    default: return JSON_NUM;
    }
}

/* Members (or items) one by one: *key gets the member's name (a string
 * value) for objects. */
static int next_member(json_t c, json_t *key, json_t *it)
{
    const char *end = c.p + c.n, *p;
    int obj = json_type(c) == JSON_OBJ;
    if (!obj && json_type(c) != JSON_ARR)
        return 0;
    p = it->p ? ws(it->p + it->n, end) : ws(c.p + 1, end);
    if (it->p) {
        if (p >= end || *p != ',')
            return 0;
        p = ws(p + 1, end);
    }
    if (p >= end || *p == '}' || *p == ']')
        return 0;
    if (obj) {
        const char *k = p;
        if (*p != '"' || !(p = skip_string(p, end)))
            return 0;
        key->p = k;
        key->n = (int)(p - k);
        p = ws(p, end);
        if (p >= end || *p != ':')
            return 0;
        p = ws(p + 1, end);
    }
    it->p = p;
    p = skip_value(p, end, 1);
    if (!p)
        return 0;
    it->n = (int)(p - it->p);
    return 1;
}

int json_next(json_t arr, json_t *it)
{
    json_t key;
    return next_member(arr, &key, it);
}

/* A plain key (no escapes in it) equal to name. */
static int key_is(json_t key, const char *name)
{
    size_t n = strlen(name);
    return key.n == (int)n + 2 && memcmp(key.p + 1, name, n) == 0;
}

int json_get(json_t obj, const char *name, json_t *out)
{
    json_t key, it = {NULL, 0};
    if (json_type(obj) != JSON_OBJ)
        return 0;
    while (next_member(obj, &key, &it))
        if (key_is(key, name)) {
            *out = it;
            return 1;
        }
    return 0;
}

int json_path(json_t v, const char *path, json_t *out)
{
    char name[64];
    while (*path) {
        int k = 0;
        while (*path && *path != '.' && k < (int)sizeof name - 1)
            name[k++] = *path++;
        name[k] = 0;
        if (*path == '.')
            path++;
        if (!json_get(v, name, &v))
            return 0;
    }
    *out = v;
    return 1;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
        if (d < 0)
            return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

static int put_utf8(char *out, int k, int cap, unsigned cp)
{
    char b[4];
    int n, i;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12);
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | cp >> 18);
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (k + n >= cap)
        return -1;
    for (i = 0; i < n; i++)
        out[k + i] = b[i];
    return k + n;
}

int json_str(json_t v, char *out, int cap)
{
    const char *p, *end;
    int k = 0;
    if (cap <= 0)
        return -1;
    out[0] = 0;
    if (json_type(v) != JSON_STR || v.n < 2)
        return -1;
    end = v.p + v.n - 1;
    for (p = v.p + 1; p < end; p++) {
        unsigned cp;
        if (*p != '\\') {
            if (k + 1 >= cap)
                break;
            out[k++] = *p;
            continue;
        }
        if (++p >= end)
            break;
        switch (*p) {
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (end - p < 5 || !hex4(p + 1, &cp)) {
                p = end + 1; /* broken */
                continue;
            }
            p += 4;
            /* A surrogate pair is one character. */
            if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 7 && p[1] == '\\' && p[2] == 'u') {
                unsigned lo;
                if (hex4(p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (cp >= 0xD800 && cp < 0xE000)
                cp = 0xFFFD; /* a lone surrogate */
            break;
        default: cp = (unsigned char)*p; break; /* \" \\ \/ */
        }
        if ((k = put_utf8(out, k, cap, cp)) < 0)
            break;
    }
    if (p != end || k < 0) { /* did not fit, or broken */
        out[0] = 0;
        return -1;
    }
    out[k] = 0;
    return k;
}

int json_get_str(json_t obj, const char *key, char *out, int cap)
{
    json_t v;
    if (cap > 0)
        out[0] = 0;
    if (!json_get(obj, key, &v))
        return cap > 0 ? 0 : -1;
    return json_str(v, out, cap);
}

int json_int(json_t v, long long *out)
{
    const char *p = v.p, *end = v.p + v.n;
    long long x = 0;
    int neg = 0, any = 0;
    if (json_type(v) == JSON_STR) {
        p++;
        end--;
    } else if (json_type(v) != JSON_NUM) {
        return 0;
    }
    if (p < end && *p == '-') {
        neg = 1;
        p++;
    }
    for (; p < end && *p >= '0' && *p <= '9'; p++, any = 1)
        if (x < 922337203685477580LL)
            x = x * 10 + (*p - '0');
    if (!any)
        return 0;
    *out = neg ? -x : x;
    return 1;
}

int json_truthy(json_t v)
{
    long long x;
    if (json_type(v) == JSON_TRUE)
        return 1;
    return json_type(v) == JSON_NUM && json_int(v, &x) && x != 0;
}

/* ------------------------------------------------------------------------ */
/* Writer                                                                    */
/* ------------------------------------------------------------------------ */

static void put_c(jw_t *w, char c)
{
    if (w->n + 1 < w->cap)
        w->p[w->n] = c;
    w->n++;
}

static void put_s(jw_t *w, const char *s)
{
    while (*s)
        put_c(w, *s++);
}

void jw_init(jw_t *w, char *buf, int cap)
{
    w->p = buf;
    w->n = 0;
    w->cap = cap;
    w->comma = 0;
}

static void value_start(jw_t *w)
{
    if (w->comma)
        put_c(w, ',');
    w->comma = 1;
}

void jw_open(jw_t *w, char brace)
{
    value_start(w);
    put_c(w, brace);
    w->comma = 0;
}

void jw_close(jw_t *w, char brace)
{
    put_c(w, brace);
    w->comma = 1;
}

static void quoted(jw_t *w, const char *s, int n)
{
    int i;
    put_c(w, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': put_s(w, "\\\""); break;
        case '\\': put_s(w, "\\\\"); break;
        case '\b': put_s(w, "\\b"); break;
        case '\f': put_s(w, "\\f"); break;
        case '\n': put_s(w, "\\n"); break;
        case '\r': put_s(w, "\\r"); break;
        case '\t': put_s(w, "\\t"); break;
        default:
            if (c < 0x20) {
                put_s(w, "\\u00");
                put_c(w, "0123456789abcdef"[c >> 4]);
                put_c(w, "0123456789abcdef"[c & 15]);
            } else {
                put_c(w, (char)c);
            }
        }
    }
    put_c(w, '"');
}

void jw_key(jw_t *w, const char *key)
{
    value_start(w);
    quoted(w, key, (int)strlen(key));
    put_c(w, ':');
    w->comma = 0;
}

void jw_strn(jw_t *w, const char *s, int n)
{
    value_start(w);
    quoted(w, s, n);
}

void jw_str(jw_t *w, const char *s)
{
    jw_strn(w, s, (int)strlen(s));
}

void jw_int(jw_t *w, long long v)
{
    char tmp[24];
    int k = (int)sizeof tmp - 1;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    value_start(w);
    tmp[k] = 0;
    do {
        tmp[--k] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        tmp[--k] = '-';
    put_s(w, tmp + k);
}

void jw_bool(jw_t *w, int v)
{
    value_start(w);
    put_s(w, v ? "true" : "false");
}

int jw_end(jw_t *w)
{
    if (w->cap > 0)
        w->p[w->n < w->cap ? w->n : w->cap - 1] = 0;
    return w->n < w->cap ? w->n : -1;
}
