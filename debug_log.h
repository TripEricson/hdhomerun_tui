/*
 * debug_log.h
 *
 * Optional debug log for troubleshooting device discovery and connections.
 * Enabled with -v / --verbose; writes timestamped lines to a file (the
 * terminal belongs to ncurses, so nothing is printed to stdout/stderr).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdbool.h>

// Opens (appends to) the log file. Returns false if it could not be opened.
bool debug_log_open(const char *path);
void debug_log_close(void);
bool debug_log_enabled(void);

// printf-style; a no-op unless the log is open.
void log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
