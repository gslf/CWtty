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

/*
 * Writes the full path of the configuration file into buf, creating the
 * parent directory if needed. Returns 0 on success.
 *
 *   Linux   $XDG_CONFIG_HOME/cwtty/cwtty.conf (default ~/.config/...)
 *   macOS   ~/Library/Application Support/cwtty/cwtty.conf
 *   Windows %APPDATA%\cwtty\cwtty.conf
 */
int plat_config_path(char *buf, size_t cap);

#endif
