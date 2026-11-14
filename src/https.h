/*
 * https.h - One HTTPS/1.1 request at a time, on the TLS connection of
 * netio.c (the system's root certificates, the host name checked). For the
 * cloud sync of the statistics (cloud.c): blocking, so it runs on a worker
 * thread.
 */
#ifndef CWTTY_HTTPS_H
#define CWTTY_HTTPS_H

/* Performs the request: method "GET" or "POST", url "https://host/path",
 * headers "Name: value\r\n" lines (may be ""), the body (len 0 for none).
 * The reply's body goes into resp (NUL-terminated, at most cap - 1 bytes),
 * its length into *len. Returns the HTTP status, or 0 if there was no reply
 * (err says why). Gives up after timeout_ms. */
int https_request(const char *method, const char *url, const char *headers, const void *body, int len, char *resp,
                  int cap, int *resp_len, char *err, int errcap, int timeout_ms);

#endif
