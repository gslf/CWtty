/*
 * platform.c - Thin OS abstraction: clocks, sleeping, randomness, threads
 * and file locations.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#ifdef _WIN32

double plat_time_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

void plat_sleep_ms(int ms)
{
    Sleep((DWORD)(ms > 0 ? ms : 0));
}

uint32_t plat_entropy(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint32_t)now.QuadPart ^ (uint32_t)GetCurrentProcessId() ^ (uint32_t)time(NULL);
}

int plat_random(void *buf, size_t len)
{
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_start;

static DWORD WINAPI thread_main(LPVOID p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int plat_thread(void (*fn)(void *), void *arg)
{
    thread_start *s = (thread_start *)malloc(sizeof *s);
    HANDLE h;
    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    h = CreateThread(NULL, 0, thread_main, s, 0, NULL);
    if (!h) {
        free(s);
        return -1;
    }
    CloseHandle(h);
    return 0;
}

int plat_config_path(char *buf, size_t cap)
{
    char dir[1024];
    const char *base = getenv("APPDATA");
    if (!base || !*base)
        base = getenv("USERPROFILE");
    if (!base || !*base)
        return -1;
    snprintf(dir, sizeof dir, "%s\\cwtty", base);
    _mkdir(dir);
    if ((size_t)snprintf(buf, cap, "%s\\cwtty.conf", dir) >= cap)
        return -1;
    return 0;
}

#else

double plat_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

void plat_sleep_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0)
        return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

uint32_t plat_entropy(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_nsec ^ ((uint32_t)getpid() << 16) ^ (uint32_t)time(NULL);
}

int plat_random(void *buf, size_t len)
{
#if defined(__APPLE__)
    arc4random_buf(buf, len);
    return 0;
#else
    unsigned char *p = (unsigned char *)buf;
    int fd;
#if defined(__linux__)
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    if (len == 0)
        return 0;
#endif
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
#endif
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_start;

static void *thread_main(void *p)
{
    thread_start s = *(thread_start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int plat_thread(void (*fn)(void *), void *arg)
{
    thread_start *s = (thread_start *)malloc(sizeof *s);
    pthread_t th;
    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&th, NULL, thread_main, s) != 0) {
        free(s);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

/* mkdir -p for a single, already-absolute path. */
static void make_dirs(char *path)
{
    char *p;
    for (p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    }
    mkdir(path, 0755);
}

int plat_config_path(char *buf, size_t cap)
{
    char dir[1024];
    const char *home = getenv("HOME");
#ifdef __APPLE__
    if (!home || !*home)
        return -1;
    snprintf(dir, sizeof dir, "%s/Library/Application Support/cwtty", home);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/')
        snprintf(dir, sizeof dir, "%s/cwtty", xdg);
    else if (home && *home)
        snprintf(dir, sizeof dir, "%s/.config/cwtty", home);
    else
        return -1;
#endif
    make_dirs(dir);
    if ((size_t)snprintf(buf, cap, "%s/cwtty.conf", dir) >= cap)
        return -1;
    return 0;
}

#endif

int plat_home_path(const char *name, char *buf, size_t cap)
{
#ifdef _WIN32
    const char *home = getenv("USERPROFILE"), *sep = "\\";
#else
    const char *home = getenv("HOME"), *sep = "/";
#endif
    size_t n;
    if (!home || !*home)
        return -1;
    n = strlen(home);
    while (n > 1 && (home[n - 1] == '/' || home[n - 1] == '\\'))
        n--;
    if ((size_t)snprintf(buf, cap, "%.*s%s%s", (int)n, home, *name ? sep : "", name) >= cap)
        return -1;
    return 0;
}

void plat_local_date(int *year, int *month, int *mday)
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    *year = tm.tm_year + 1900;
    *month = tm.tm_mon + 1;
    *mday = tm.tm_mday;
}

int plat_read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f || cap == 0) {
        if (f)
            fclose(f);
        return -1;
    }
    n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return (int)n;
}

int plat_write_private(const char *path, const char *data, size_t len)
{
#ifdef _WIN32
    /* The profile's folders are the user's alone already. */
    return plat_write_file(path, data, len);
#else
    char tmp[1100];
    int fd, ok;
    size_t done = 0;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
        return -1;
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    while (done < len) {
        ssize_t w = write(fd, data + done, len - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    ok = done == len && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    return ok ? 0 : -1;
#endif
}

int plat_write_file(const char *path, const char *data, size_t len)
{
    char tmp[1100];
    FILE *f;
    int ok;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
        return -1;
    f = fopen(tmp, "wb");
    if (!f)
        return -1;
    ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok)
        remove(tmp);
    return ok ? 0 : -1;
}
