/*
 * cloud.c - The terminal app's side of the statistics sync with Filen.
 *
 * The worker thread owns the client while a task runs (job set); the main
 * thread touches it only when the worker is idle (job 0), so the only
 * shared variable is the job.
 */
#include "cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "atomics.h"
#include "fcrypto.h"
#include "filen.h"
#include "https.h"
#include "platform.h"
#include "statsync.h"
#include "util.h"

enum { JOB_NONE, JOB_LOGIN, JOB_SYNC };

static struct {
    filen_t    f;
    statsync_t y;
    char       path[1100];
    at_int     job;     /* set by the main thread, cleared by the worker */
    int        running; /* the job last started, until cloud_poll() sees it end */
    int        started;
    char       error[160];
    char       synced[8];
    int        need_code;
} c;

static void run_requests(filen_t *f)
{
    char headers[512], err[160];
    int n = 0, status;
    while (filen_run(f) == FILEN_WAIT) {
        filen_req *r = &f->req;
        headers[0] = 0;
        if (r->auth[0])
            n = snprintf(headers, sizeof headers, "Authorization: %s\r\n", r->auth);
        else
            n = 0;
        if (r->checksum[0])
            n += snprintf(headers + n, sizeof headers - (size_t)n, "Checksum: %s\r\n", r->checksum);
        if (r->method == FILEN_POST)
            snprintf(headers + n, sizeof headers - (size_t)n, "Content-Type: %s\r\n",
                     r->json ? "application/json" : "application/octet-stream");
        status = https_request(r->method == FILEN_POST ? "POST" : "GET", r->url, headers, r->body,
                               r->method == FILEN_POST ? r->body_len : 0, f->resp, FILEN_RESP_CAP + 1, &n, err,
                               sizeof err, 20000);
        filen_reply(f, status, n);
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        int job = at_get(&c.job);
        if (job == JOB_NONE) {
            plat_sleep_ms(20);
            continue;
        }
        c.f.now_ms = (double)time(NULL) * 1000.0;
        if (job == JOB_LOGIN) {
            run_requests(&c.f);
            free(c.f.work); /* version 3 only */
            c.f.work = NULL;
        } else {
            run_requests(&c.f);
        }
        at_set(&c.job, JOB_NONE);
    }
}

static void seed(void)
{
    unsigned char r[32];
    if (plat_random(r, sizeof r) == 0)
        fc_seed(r, sizeof r);
    fc_wipe(r, sizeof r);
}

static void *work_area(void)
{
    return malloc(FC_ARGON2_WORK(FILEN_ARGON2_KIB));
}

int cloud_start(const char *session_path)
{
    char text[4096];
    seed();
    filen_init(&c.f, STATSYNC_FOLDER);
    c.f.get_work = work_area;
    statsync_init(&c.y);
    str_copy(c.path, sizeof c.path, session_path);
    if (plat_read_file(c.path, text, sizeof text) >= 0 && filen_session_parse(&c.f.s, text) != 0)
        memset(&c.f.s, 0, sizeof c.f.s);
    fc_wipe(text, sizeof text);
    if (!c.started && plat_thread(worker, NULL) == 0)
        c.started = 1;
    return filen_signed_in(&c.f);
}

int cloud_state(void)
{
    if (c.running || at_get(&c.job) != JOB_NONE)
        return CLOUD_BUSY;
    return filen_signed_in(&c.f) ? CLOUD_READY : CLOUD_OFF;
}

const char *cloud_email(void)
{
    return c.f.s.email; /* written by this thread alone */
}

const char *cloud_error(void)
{
    return c.error;
}

int cloud_need_code(void)
{
    return c.need_code;
}

const char *cloud_synced(void)
{
    return c.synced;
}

static void start(int job)
{
    c.error[0] = 0;
    c.running = job;
    at_set(&c.job, job);
}

void cloud_login(const char *email, const char *password, const char *code)
{
    if (!c.started || cloud_state() == CLOUD_BUSY)
        return;
    seed();
    filen_login(&c.f, email, password, code);
    start(JOB_LOGIN);
}

void cloud_sync(const statsbook_t *book)
{
    if (!c.started || cloud_state() != CLOUD_READY)
        return;
    statsync_start(&c.y, &c.f, book);
    start(JOB_SYNC);
}

static void save_session(void)
{
    char text[4096];
    int n = filen_session_format(&c.f.s, text, sizeof text);
    if (n > 0)
        plat_write_private(c.path, text, (size_t)n);
    fc_wipe(text, sizeof text);
}

static void stamp(void)
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    snprintf(c.synced, sizeof c.synced, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

int cloud_poll(statsbook_t *book)
{
    int job = c.running, fx = 0;
    if (!job || at_get(&c.job) != JOB_NONE)
        return 0;
    c.running = 0;
    if (job == JOB_LOGIN) {
        c.need_code = c.f.need_code;
        if (c.f.state == FILEN_DONE) {
            save_session();
            statsync_init(&c.y);
            fx |= CLOUD_SIGNED_IN;
            cloud_sync(book);
        } else {
            str_copy(c.error, sizeof c.error, c.f.error);
            memset(&c.f.s, 0, sizeof c.f.s);
            fx |= CLOUD_LOGIN_FAILED;
        }
        return fx;
    }
    if (c.f.state == FILEN_DONE) {
        if (statsync_merge(&c.y, book))
            fx |= CLOUD_CHANGED;
        save_session(); /* folders may have been made again */
        stamp();
        return fx | CLOUD_SYNCED;
    }
    str_copy(c.error, sizeof c.error, c.f.error);
    if (c.f.signed_out) {
        cloud_sign_out();
        str_copy(c.error, sizeof c.error, "Filen ended the session: sign in again");
        return CLOUD_SIGNED_OUT;
    }
    return CLOUD_SYNC_FAILED;
}

void cloud_sign_out(void)
{
    if (cloud_state() == CLOUD_BUSY)
        return;
    filen_sign_out(&c.f);
    statsync_init(&c.y);
    c.synced[0] = 0;
    c.error[0] = 0;
    c.need_code = 0;
    remove(c.path);
}

void cloud_finish(const statsbook_t *book, int ms)
{
    double end = plat_time_ms() + ms;
    while (at_get(&c.job) != JOB_NONE && plat_time_ms() < end) /* a sync already going */
        plat_sleep_ms(20);
    if (at_get(&c.job) != JOB_NONE || !filen_signed_in(&c.f) || !statsync_pending(&c.y, book))
        return;
    c.running = 0;
    cloud_sync(book);
    while (at_get(&c.job) != JOB_NONE && plat_time_ms() < end)
        plat_sleep_ms(20);
}
