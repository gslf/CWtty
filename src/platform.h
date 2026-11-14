/*
 * platform.h - Thin OS abstraction: clocks, sleeping and file locations.
 */
#ifndef CWTTY_PLATFORM_H
#define CWTTY_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic time in milliseconds (arbitrary origin). */
double plat_time_ms(void);

void plat_sleep_ms(int ms);

/* A value that differs between runs, used to seed the PRNG. */
uint32_t plat_entropy(void);

/* Fills buf with bytes from the operating system's cryptographic random
 * generator. Returns 0 on success. */
int plat_random(void *buf, size_t len);

/* Runs fn(arg) on a new detached thread. Returns 0 on success. */
int plat_thread(void (*fn)(void *), void *arg);

/*
 * Writes the full path of the configuration file into buf, creating the
 * parent directory if needed. Returns 0 on success.
 *
 *   Linux   $XDG_CONFIG_HOME/cwtty/cwtty.conf (default ~/.config/...)
 *   macOS   ~/Library/Application Support/cwtty/cwtty.conf
 *   Windows %APPDATA%\cwtty\cwtty.conf
 */
int plat_config_path(char *buf, size_t cap);

/* A file in the user's home directory (name "" for the directory itself):
 * $HOME, %USERPROFILE% on Windows. Returns 0 on success. */
int plat_home_path(const char *name, char *buf, size_t cap);

/* Today's local date. */
void plat_local_date(int *year, int *month, int *mday);

/* Reads a whole file into buf (NUL-terminated). Returns its length or -1. */
int plat_read_file(const char *path, char *buf, size_t cap);

/* Replaces a file with data: written aside, then renamed over it, so that
 * a crash never leaves it half written. Returns 0 on success. */
int plat_write_file(const char *path, const char *data, size_t len);

/* The same, readable by the user alone (0600): for secrets. */
int plat_write_private(const char *path, const char *data, size_t len);

#endif
