/*
 * tuner.c
 *
 * Dispatch for the common tuner interface (tuner.h): discovery across all
 * backends, and each call routed to the open tuner's backend. A backend
 * that doesn't implement a call gets a "not available" result.
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>
#include "tuner.h"

// In list order: local tuners first, above the network ones
static const struct tuner_backend *const backends[] = {
    &tuner_backend_cxd28xx,
    &tuner_backend_hdhr,
};
#define NUM_BACKENDS (int)(sizeof(backends) / sizeof(backends[0]))

static const struct tuner_backend *backend_for(enum tuner_kind kind)
{
    switch (kind) {
    case TUNER_KIND_HDHR: return &tuner_backend_hdhr;
    case TUNER_KIND_CXD28XX: return &tuner_backend_cxd28xx;
    }
    return NULL;
}

int tuner_discover(struct tuner_info list[], int max)
{
    int n = 0;
    for (int i = 0; i < NUM_BACKENDS && n < max; i++)
        if (backends[i]->discover)
            n += backends[i]->discover(list + n, max - n);
    return n;
}

int tuner_discover_local(struct tuner_info list[], int max)
{
    int n = 0;
    for (int i = 0; i < NUM_BACKENDS && n < max; i++)
        if (backends[i] != &tuner_backend_hdhr && backends[i]->discover)
            n += backends[i]->discover(list + n, max - n);
    return n;
}

enum tuner_add_result tuner_add_by_address(const char *addr, struct tuner_info list[],
                                           int *count, int max, int *added,
                                           uint32_t *device_id)
{
    // Only HDHomeRun devices are added by network address
    *added = 0;
    if (*count >= max) return TUNER_ADD_LIST_FULL;
    return tuner_backend_hdhr.add_by_address(addr, list, count, max, added, device_id);
}

struct tuner *tuner_open(const struct tuner_info *info)
{
    const struct tuner_backend *b = info ? backend_for(info->kind) : NULL;
    struct tuner *t = (b && b->open) ? b->open(info) : NULL;
    if (t) {
        t->backend = b;
        t->info = *info;
    }
    return t;
}

void tuner_close(struct tuner *t)
{
    if (t) t->backend->close(t);
}

const struct tuner_info *tuner_get_info(const struct tuner *t)
{
    return t ? &t->info : NULL;
}

unsigned tuner_caps(struct tuner *t)
{
    return (t && t->backend->caps) ? t->backend->caps(t) : 0;
}

// Call an optional backend function, or return 'fallback'
#define CALL(t, fn, fallback, ...) \
    (((t) && (t)->backend->fn) ? (t)->backend->fn((t), __VA_ARGS__) : (fallback))

int tuner_get_status(struct tuner *t, struct tuner_status *st)       { return CALL(t, get_status, -1, st); }
int tuner_get_vstatus(struct tuner *t, struct tuner_vstatus *vs)     { return CALL(t, get_vstatus, 0, vs); }
int tuner_get_plpinfo(struct tuner *t, const char **text)            { return CALL(t, get_plpinfo, 0, text); }
int tuner_get_streaminfo(struct tuner *t, const char **text)         { return CALL(t, get_streaminfo, 0, text); }
int tuner_get_target(struct tuner *t, const char **text)             { return CALL(t, get_target, 0, text); }
int tuner_get_version(struct tuner *t, const char **text)            { return CALL(t, get_version, 0, text); }
int tuner_get_l1_string(struct tuner *t, const char **b64)           { return CALL(t, get_l1_string, 0, b64); }
int tuner_set_channel(struct tuner *t, const char *spec)             { return CALL(t, set_channel, -1, spec); }
int tuner_set_channelmap(struct tuner *t, const char *name)          { return CALL(t, set_channelmap, -1, name); }
int tuner_set_program(struct tuner *t, const char *program)          { return CALL(t, set_program, -1, program); }

int tuner_get_errors(struct tuner *t, struct tuner_errors *err)
{
    err->transport = err->network = err->sequence = TUNER_NO_VALUE;
    return CALL(t, get_errors, 0, err);
}

int tuner_wait_for_lock(struct tuner *t)
{
    return (t && t->backend->wait_for_lock) ? t->backend->wait_for_lock(t) : -1;
}

int tuner_clear_target(struct tuner *t)
{
    return (t && t->backend->clear_target) ? t->backend->clear_target(t) : -1;
}

int tuner_get_channel_list(struct tuner *t, unsigned int *channels, int max)
{
    return CALL(t, get_channel_list, 0, channels, max);
}

int tuner_get_channelmap(struct tuner *t, char *name, size_t len)
{
    return CALL(t, get_channelmap, 0, name, len);
}

int tuner_get_channelmap_names(struct tuner *t, char **names, int max)
{
    return CALL(t, get_channelmap_names, 0, names, max);
}

int tuner_set_target_rtp(struct tuner *t, int port, char *err, size_t errlen)
{
    if (!t || !t->backend->set_target_rtp) {
        snprintf(err, errlen, "Live viewing not supported on this tuner.");
        return -1;
    }
    return t->backend->set_target_rtp(t, port, err, errlen);
}

struct tuner_capture *tuner_capture_start(struct tuner *t, enum tuner_capture_format fmt,
                                          unsigned rf_channel, const char *spec_suffix,
                                          int duration_s, char *debug_url, size_t debug_len,
                                          char *err, size_t errlen)
{
    if (debug_url && debug_len) debug_url[0] = '\0';
    if (!t || !t->backend->capture_start) {
        snprintf(err, errlen, "Capture not supported on this tuner.");
        return NULL;
    }
    struct tuner_capture *c = t->backend->capture_start(t, fmt, rf_channel, spec_suffix,
                                                        duration_s, debug_url, debug_len,
                                                        err, errlen);
    if (c) c->backend = t->backend;
    return c;
}

long tuner_capture_read(struct tuner_capture *c, uint8_t *buf, size_t len)
{
    return c ? c->backend->capture_read(c, buf, len) : -2;
}

void tuner_capture_stop(struct tuner_capture *c)
{
    if (c) c->backend->capture_stop(c);
}
