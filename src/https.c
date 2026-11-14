/*
 * https.c - One HTTPS/1.1 request at a time.
 */
#include "https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netio.h"
#include "platform.h"
#include "util.h"

#define HEAD_CAP 16384

static int fail(char *err, int errcap, const char *why)
{
    snprintf(err, (size_t)errcap, "%s", why);
    return 0;
}

static int lower_eq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        if (x != b[i])
            return 0;
    }
    return 1;
}

/* A header's value in the head (up to the blank line), or NULL. */
static const char *header(const char *head, const char *name)
{
    int n = (int)strlen(name);
    const char *p = strstr(head, "\r\n");
    while (p && p[2] != '\r') {
        p += 2;
        if (lower_eq(p, name, n) && p[n] == ':') {
            p += n + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1));
}

/* Joins the chunks of a chunked body in place: its length, or -1. */
static int unchunk(char *b, int n)
{
    int in = 0, out = 0;
    for (;;) {
        long size = 0;
        int d;
        while (in < n && ((d = hex_digit(b[in])) >= 0)) {
            size = size * 16 + d;
            if (size > n)
                return -1;
            in++;
        }
        while (in < n && b[in] != '\n') /* extensions, CR */
            in++;
        in++;
        if (size == 0)
            return out;
        if (in + size > n)
            return -1;
        memmove(b + out, b + in, (size_t)size);
        out += (int)size;
        in += (int)size + 2; /* CRLF */
    }
}

int https_request(const char *method, const char *url, const char *headers, const void *body, int len, char *resp,
                  int cap, int *resp_len, char *err, int errcap, int timeout_ms)
{
    char host[256], *req, *buf;
    const char *p, *path, *colon;
    int port = 443, hn, n, sent = 0, got = 0, bufcap, status = 0, total, body_at = -1, need = -1, chunked = 0;
    double start = plat_time_ms();
    netio *c;

    *resp_len = 0;
    if (cap > 0)
        resp[0] = 0;
    if (errcap > 0)
        err[0] = 0;
    if (strncmp(url, "https://", 8) != 0)
        return fail(err, errcap, "not an https address");
    p = url + 8;
    path = strchr(p, '/');
    if (!path)
        path = p + strlen(p);
    colon = memchr(p, ':', (size_t)(path - p));
    hn = (int)((colon ? colon : path) - p);
    if (hn <= 0 || hn >= (int)sizeof host)
        return fail(err, errcap, "bad address");
    memcpy(host, p, (size_t)hn);
    host[hn] = 0;
    if (colon)
        port = atoi(colon + 1);

    total = (int)strlen(path) + (int)strlen(headers) + 512 + (len > 0 ? len : 0);
    req = (char *)malloc((size_t)total);
    bufcap = cap + HEAD_CAP;
    buf = (char *)malloc((size_t)bufcap + 1);
    if (!req || !buf) {
        free(req);
        free(buf);
        return fail(err, errcap, "out of memory");
    }
    n = snprintf(req, (size_t)total,
                 "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CWtty\r\nAccept: application/json, */*\r\n"
                 "Connection: close\r\n%sContent-Length: %d\r\n\r\n",
                 method, *path ? path : "/", host, headers, len > 0 ? len : 0);
    if (len > 0) {
        memcpy(req + n, body, (size_t)len);
        n += len;
    }

    c = netio_new();
    netio_open(c, host, port, 1, NULL);
    for (;;) {
        int st = netio_poll(c), r;
        if (plat_time_ms() - start > timeout_ms) {
            fail(err, errcap, "Filen did not answer in time");
            break;
        }
        if (st == NETIO_FAILED) {
            fail(err, errcap, netio_error(c));
            break;
        }
        if (st != NETIO_OPEN) {
            plat_sleep_ms(5);
            continue;
        }
        if (sent < n) {
            r = netio_write(c, (const uint8_t *)req + sent, n - sent);
            if (r < 0) {
                fail(err, errcap, "the connection was lost");
                break;
            }
            sent += r;
            if (r == 0)
                plat_sleep_ms(2);
            continue;
        }
        if (got >= bufcap) {
            fail(err, errcap, "the reply is too long");
            break;
        }
        r = netio_read(c, (uint8_t *)buf + got, bufcap - got);
        if (r > 0) {
            got += r;
            buf[got] = 0;
            if (body_at < 0) {
                char *end = strstr(buf, "\r\n\r\n");
                const char *v;
                if (end) {
                    body_at = (int)(end - buf) + 4;
                    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) {
                        status = 0;
                        fail(err, errcap, "not an HTTP reply");
                        break;
                    }
                    end[2] = 0; /* the head alone, for header() */
                    if ((v = header(buf, "content-length")) != NULL)
                        need = atoi(v);
                    if ((v = header(buf, "transfer-encoding")) != NULL && lower_eq(v, "chunked", 7))
                        chunked = 1;
                    end[2] = '\r';
                }
            }
            if (body_at >= 0 && !chunked && need >= 0 && got - body_at >= need)
                break; /* all of it */
            if (body_at >= 0 && chunked && got - body_at >= 5 && memcmp(buf + got - 5, "0\r\n\r\n", 5) == 0)
                break;
            continue;
        }
        if (r < 0) /* closed: the reply ends here */
            break;
        plat_sleep_ms(2);
    }
    netio_close(c);
    netio_free(c);
    free(req);

    if (status && body_at >= 0) {
        int bl = got - body_at;
        if (chunked)
            bl = unchunk(buf + body_at, bl);
        else if (need >= 0 && bl > need)
            bl = need;
        if (bl < 0 || (!chunked && need >= 0 && bl < need)) {
            status = 0;
            fail(err, errcap, "the reply was cut short");
        } else if (bl >= cap) {
            status = 0;
            fail(err, errcap, "the reply is too long");
        } else {
            memcpy(resp, buf + body_at, (size_t)bl);
            resp[bl] = 0;
            *resp_len = bl;
        }
    } else if (!err[0]) {
        fail(err, errcap, "no reply");
        status = 0;
    }
    free(buf);
    return status;
}
