/*
 * netio.c - TCP and TLS connections to the brokers.
 *
 * netio_open() hands a job to a worker thread: name resolution, a connect
 * with a timeout, TCP_NODELAY (the packets are small and late is bad) and
 * the TLS handshake. The main loop then polls the job; closing a job that
 * is still running abandons it to the worker, which frees it when done.
 */
#include "netio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "atomics.h"
#include "platform.h"
#include "util.h"

#define CONNECT_TIMEOUT_MS 6000
#define HANDSHAKE_TIMEOUT_MS 10000

#ifdef _WIN32
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

enum { JOB_RUNNING, JOB_OPEN, JOB_FAILED, JOB_ABANDONED };

typedef struct {
    at_int state;
    char   host[256];
    int    port, tls;
    char  *extra_ca;
    sock_t fd;
    char   error[128];

    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
} job_t;

struct netio {
    job_t *job;
    int    state;
    char   error[128];
};

/* ------------------------------------------------------------------------ */
/* Sockets                                                                   */
/* ------------------------------------------------------------------------ */

void netio_startup(void)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

static int would_block(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
#endif
}

static void set_nonblocking(sock_t fd)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

/* Waits until fd is readable (or writable), at most ms. */
static int wait_fd(sock_t fd, int write, int ms)
{
    fd_set set, err;
    struct timeval tv;
    FD_ZERO(&set);
    FD_ZERO(&err);
    FD_SET(fd, &set);
    FD_SET(fd, &err);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    /* On Windows a failed connect shows up in the exception set. */
    return select((int)fd + 1, write ? NULL : &set, write ? &set : NULL, &err, &tv) > 0 && !FD_ISSET(fd, &err);
}

static int connect_to(job_t *j)
{
    struct addrinfo hints, *list = NULL, *ai;
    char port[16];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", j->port);
    if (getaddrinfo(j->host, port, &hints, &list) != 0 || !list) {
        snprintf(j->error, sizeof j->error, "cannot find %.100s", j->host);
        return 0;
    }
    snprintf(j->error, sizeof j->error, "cannot reach %.100s:%d", j->host, j->port);
    for (ai = list; ai; ai = ai->ai_next) {
        sock_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int ok = 0;
        if (fd == BAD_SOCK)
            continue;
        set_nonblocking(fd);
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            ok = 1;
        } else if (would_block() && wait_fd(fd, 1, CONNECT_TIMEOUT_MS)) {
            int err = 0;
            socklen_t len = sizeof err;
            ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0;
        }
        if (ok) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
            j->fd = fd;
            freeaddrinfo(list);
            j->error[0] = 0;
            return 1;
        }
        sock_close(fd);
    }
    freeaddrinfo(list);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* TLS                                                                       */
/* ------------------------------------------------------------------------ */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    job_t *j = (job_t *)ctx;
    int n = (int)send(j->fd, (const char *)buf, (int)len, SEND_FLAGS);
    if (n < 0)
        return would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    job_t *j = (job_t *)ctx;
    int n = (int)recv(j->fd, (char *)buf, (int)len, 0);
    if (n < 0)
        return would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return n;
}

/* Adds the system's root certificates. Returns how many. */
static int system_roots(mbedtls_x509_crt *ca)
{
    int n = 0;
#ifdef _WIN32
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    PCCERT_CONTEXT c = NULL;
    if (!store)
        return 0;
    while ((c = CertEnumCertificatesInStore(store, c)) != NULL)
        if (mbedtls_x509_crt_parse_der(ca, c->pbCertEncoded, c->cbCertEncoded) == 0)
            n++;
    CertCloseStore(store, 0);
#else
    static const char *const bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",                /* Debian, Ubuntu, Arch, Gentoo */
        "/etc/pki/tls/certs/ca-bundle.crt",                  /* Fedora, RHEL, AlmaLinux */
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", /* the same, newer */
        "/etc/ssl/ca-bundle.pem",                            /* openSUSE */
        "/etc/ssl/cert.pem",                                 /* macOS, Alpine, BSD */
    };
    int i;
    for (i = 0; i < ARRAY_LEN(bundles) && n == 0; i++) {
        FILE *f = fopen(bundles[i], "rb");
        long size;
        char *buf;
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = size > 0 && size < 16L * 1024 * 1024 ? (char *)malloc((size_t)size + 1) : NULL;
        if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
            const mbedtls_x509_crt *c;
            buf[size] = 0;
            mbedtls_x509_crt_parse(ca, (const unsigned char *)buf, (size_t)size + 1);
            for (c = ca; c && c->version; c = c->next)
                n++;
        }
        free(buf);
        fclose(f);
    }
#endif
    return n;
}

static int handshake(job_t *j)
{
    static const char pers[] = "cwtty";
    double end = plat_time_ms() + HANDSHAKE_TIMEOUT_MS;
    int ret, roots;

    if (mbedtls_ctr_drbg_seed(&j->drbg, mbedtls_entropy_func, &j->entropy, (const unsigned char *)pers,
                              sizeof pers - 1) != 0) {
        snprintf(j->error, sizeof j->error, "no random numbers for TLS");
        return 0;
    }
    roots = system_roots(&j->ca);
    if (j->extra_ca && mbedtls_x509_crt_parse(&j->ca, (const unsigned char *)j->extra_ca, strlen(j->extra_ca) + 1) == 0)
        roots++;
    if (roots == 0) {
        snprintf(j->error, sizeof j->error, "no root certificates found");
        return 0;
    }
    if (mbedtls_ssl_config_defaults(&j->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(j->error, sizeof j->error, "TLS setup failed");
        return 0;
    }
    mbedtls_ssl_conf_authmode(&j->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&j->conf, &j->ca, NULL);
    mbedtls_ssl_conf_rng(&j->conf, mbedtls_ctr_drbg_random, &j->drbg);
    if (mbedtls_ssl_setup(&j->ssl, &j->conf) != 0 || mbedtls_ssl_set_hostname(&j->ssl, j->host) != 0) {
        snprintf(j->error, sizeof j->error, "TLS setup failed");
        return 0;
    }
    mbedtls_ssl_set_bio(&j->ssl, j, bio_send, bio_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&j->ssl)) != 0) {
        int left = (int)(end - plat_time_ms());
        if ((ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) || left <= 0)
            break;
        wait_fd(j->fd, ret == MBEDTLS_ERR_SSL_WANT_WRITE, left);
    }
    if (ret == 0)
        return 1;
    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        snprintf(j->error, sizeof j->error, "%.100s: certificate not trusted", j->host);
    } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        snprintf(j->error, sizeof j->error, "%.100s: TLS timed out", j->host);
    } else {
        char msg[80];
        mbedtls_strerror(ret, msg, sizeof msg);
        snprintf(j->error, sizeof j->error, "TLS: %s", msg);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Jobs                                                                      */
/* ------------------------------------------------------------------------ */

static void job_free(job_t *j)
{
    if (j->fd != BAD_SOCK)
        sock_close(j->fd);
    mbedtls_ssl_free(&j->ssl);
    mbedtls_ssl_config_free(&j->conf);
    mbedtls_x509_crt_free(&j->ca);
    mbedtls_ctr_drbg_free(&j->drbg);
    mbedtls_entropy_free(&j->entropy);
    free(j->extra_ca);
    free(j);
}

static void worker(void *p)
{
    job_t *j = (job_t *)p;
    int ok = connect_to(j) && (!j->tls || handshake(j));
    if (!at_cas(&j->state, JOB_RUNNING, ok ? JOB_OPEN : JOB_FAILED))
        job_free(j); /* abandoned while we worked */
}

netio *netio_new(void)
{
    return (netio *)calloc(1, sizeof(netio));
}

void netio_free(netio *c)
{
    if (!c)
        return;
    netio_close(c);
    free(c);
}

void netio_open(netio *c, const char *host, int port, int tls, const char *extra_ca)
{
    job_t *j;
    netio_close(c);
    j = (job_t *)calloc(1, sizeof *j);
    if (!j) {
        c->state = NETIO_FAILED;
        str_copy(c->error, sizeof c->error, "out of memory");
        return;
    }
    str_copy(j->host, sizeof j->host, host);
    j->port = port;
    j->tls = tls;
    j->fd = BAD_SOCK;
    if (extra_ca && *extra_ca) {
        j->extra_ca = (char *)malloc(strlen(extra_ca) + 1);
        if (j->extra_ca)
            strcpy(j->extra_ca, extra_ca);
    }
    mbedtls_ssl_init(&j->ssl);
    mbedtls_ssl_config_init(&j->conf);
    mbedtls_x509_crt_init(&j->ca);
    mbedtls_ctr_drbg_init(&j->drbg);
    mbedtls_entropy_init(&j->entropy);
    at_set(&j->state, JOB_RUNNING);
    c->job = j;
    c->state = NETIO_OPENING;
    c->error[0] = 0;
    if (plat_thread(worker, j) != 0) {
        c->job = NULL;
        job_free(j);
        c->state = NETIO_FAILED;
        str_copy(c->error, sizeof c->error, "cannot start a thread");
    }
}

int netio_poll(netio *c)
{
    if (c->state == NETIO_OPENING && c->job) {
        int s = at_get(&c->job->state);
        if (s == JOB_OPEN) {
            c->state = NETIO_OPEN;
        } else if (s == JOB_FAILED) {
            str_copy(c->error, sizeof c->error, c->job->error);
            job_free(c->job);
            c->job = NULL;
            c->state = NETIO_FAILED;
        }
    }
    return c->state;
}

static void lost(netio *c, const char *why)
{
    str_copy(c->error, sizeof c->error, why);
    job_free(c->job);
    c->job = NULL;
    c->state = NETIO_FAILED;
}

int netio_read(netio *c, uint8_t *buf, int cap)
{
    job_t *j = c->job;
    int n;
    if (c->state != NETIO_OPEN)
        return c->state == NETIO_OPENING ? 0 : -1;
    if (j->tls) {
        n = mbedtls_ssl_read(&j->ssl, buf, (size_t)cap);
        if (n > 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
    } else {
        n = (int)recv(j->fd, (char *)buf, cap, 0);
        if (n > 0)
            return n;
        if (n < 0 && would_block())
            return 0;
    }
    lost(c, "connection lost");
    return -1;
}

int netio_write(netio *c, const uint8_t *buf, int len)
{
    job_t *j = c->job;
    int n;
    if (c->state != NETIO_OPEN)
        return c->state == NETIO_OPENING ? 0 : -1;
    if (len <= 0)
        return 0;
    if (j->tls) {
        n = mbedtls_ssl_write(&j->ssl, buf, (size_t)len);
        if (n >= 0)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
    } else {
        n = (int)send(j->fd, (const char *)buf, len, SEND_FLAGS);
        if (n >= 0)
            return n;
        if (would_block())
            return 0;
    }
    lost(c, "connection lost");
    return -1;
}

void netio_close(netio *c)
{
    job_t *j = c->job;
    if (j) {
        if (!at_cas(&j->state, JOB_RUNNING, JOB_ABANDONED)) {
            if (at_get(&j->state) == JOB_OPEN && j->tls)
                mbedtls_ssl_close_notify(&j->ssl);
            job_free(j);
        }
        c->job = NULL;
    }
    c->state = NETIO_CLOSED;
}

const char *netio_error(const netio *c)
{
    return c->error;
}
