/*
 * debug_log.c - see debug_log.h
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "debug_log.h"

static FILE *log_file = NULL;

bool debug_log_open(const char *path)
{
    if (log_file) return true;
    log_file = fopen(path, "a");
    return log_file != NULL;
}

void debug_log_close(void)
{
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

bool debug_log_enabled(void)
{
    return log_file != NULL;
}

void log_debug(const char *fmt, ...)
{
    if (!log_file) return;

    char timestamp[32];
    time_t now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_file, "[%s] ", timestamp);

    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    va_end(args);

    fputc('\n', log_file);
    fflush(log_file);
}
