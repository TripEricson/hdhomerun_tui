/*
 * tuner_hdhr.c
 *
 * HDHomeRun backend for the common tuner interface (tuner.h). Everything
 * that talks to libhdhomerun or to the HDHomeRun's HTTP server lives here;
 * the code is moved from hdhomerun_tui.c and l1_detail_parser.c with the
 * same behavior.
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "hdhomerun.h"
#include "hdhomerun_device.h"
#include "tuner.h"

// Firmware from this date on reports /tunerN/l1detail
#define L1DETAIL_MIN_FIRMWARE 20250623

struct hdhr_tuner {
    struct tuner base;                 // must be first
    struct hdhomerun_device_t *hd;
};

struct hdhr_capture {
    struct tuner_capture base;         // must be first
    struct hdhr_tuner *ht;
    enum tuner_capture_format fmt;
    // CAPTURE_TS: libhdhomerun stream, handed out from its buffer
    uint8_t *pend;
    size_t pend_len;
    // CAPTURE_DBG / CAPTURE_ALP_PCAP: HTTP download from the device
    int sock;
    bool headers_done;
    char hdr[4096];                    // response header seen so far
    size_t hdr_len;
};

static struct hdhr_tuner *H(struct tuner *t) { return (struct hdhr_tuner *)t; }

/* ---- Status-string helpers (as in hdhomerun_tui.c) ------------------- */

// Value inside parentheses after a key, e.g. "ss=100(-35dBm)" -> -35
static long parse_db_value(const char *status_str, const char *key)
{
    const char *key_found = strstr(status_str, key);
    if (key_found) {
        const char *paren_open = strchr(key_found, '(');
        if (paren_open) return strtol(paren_open + 1, NULL, 10);
    }
    return TUNER_NO_VALUE;
}

// Numeric value after a key, decimal or 0x hex
static long parse_status_value(const char *status_str, const char *key)
{
    const char *found = strstr(status_str, key);
    if (found) return strtol(found + strlen(key), NULL, 0);
    return TUNER_NO_VALUE;
}

static long firmware_version_number(struct hdhomerun_device_t *hd)
{
    char *version_str;
    char numeric[16] = {0};
    int i = 0;

    if (hdhomerun_device_get_var(hd, "/sys/version", &version_str, NULL) <= 0) return 0;
    while (version_str[i] && isdigit((unsigned char)version_str[i]) && i < 15) {
        numeric[i] = version_str[i];
        i++;
    }
    return atol(numeric);
}

/* ---- Discovery ------------------------------------------------------- */

static void fill_info(struct tuner_info *ti, uint32_t device_id, const char *ip_str,
                      int index, int count, bool is_legacy)
{
    memset(ti, 0, sizeof(*ti));
    ti->kind = TUNER_KIND_HDHR;
    ti->device_id = device_id;
    snprintf(ti->addr, sizeof(ti->addr), "%s", ip_str);
    ti->tuner_index = index;
    ti->total_tuners_on_device = count;
    ti->is_legacy = is_legacy;
    snprintf(ti->name, sizeof(ti->name), "%08X-%d", device_id, index);
    snprintf(ti->title, sizeof(ti->title), "Tuner %08X-%d (%s)", device_id, index, ip_str);
}

static int hdhr_discover(struct tuner_info list[], int max)
{
    struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
    if (!ds) return 0;

    uint32_t device_types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
    if (hdhomerun_discover2_find_devices_broadcast(ds, HDHOMERUN_DISCOVER_FLAGS_IPV4_GENERAL, device_types, 1) < 0) {
        hdhomerun_discover_destroy(ds);
        return 0;
    }

    int n = 0;
    struct hdhomerun_discover2_device_t *device = hdhomerun_discover2_iter_device_first(ds);
    while (device && n < max) {
        uint32_t device_id = hdhomerun_discover2_device_get_device_id(device);
        int tuner_count = hdhomerun_discover2_device_get_tuner_count(device);
        bool is_legacy = hdhomerun_discover2_device_is_legacy(device);
        char ip_str[64];

        struct hdhomerun_discover2_device_if_t *device_if = hdhomerun_discover2_iter_device_if_first(device);
        struct sockaddr_storage ip_address;
        hdhomerun_discover2_device_if_get_ip_addr(device_if, &ip_address);
        hdhomerun_sock_sockaddr_to_ip_str(ip_str, (struct sockaddr *)&ip_address, true);

        for (int i = 0; i < tuner_count && n < max; i++)
            fill_info(&list[n++], device_id, ip_str, i, tuner_count, is_legacy);

        device = hdhomerun_discover2_iter_device_next(device);
    }

    hdhomerun_discover_destroy(ds);
    return n;
}

static enum tuner_add_result hdhr_add_by_address(const char *ip_str, struct tuner_info list[],
                                                 int *count, int max, int *added,
                                                 uint32_t *device_id_out)
{
    *added = 0;
    if (*count >= max) return TUNER_ADD_LIST_FULL;

    struct hdhomerun_device_t *hd = hdhomerun_device_create_from_str(ip_str, NULL);
    if (!hd) return TUNER_ADD_BAD_ADDRESS;

    // libhdhomerun manages the memory for model_str; not freed here
    char *model_str;
    if (hdhomerun_device_get_var(hd, "/sys/model", &model_str, NULL) <= 0) {
        hdhomerun_device_destroy(hd);
        return TUNER_ADD_NO_RESPONSE;
    }

    uint32_t device_id = hdhomerun_device_get_device_id(hd);
    if (device_id_out) *device_id_out = device_id;

    for (int i = 0; i < *count; i++) {
        if (list[i].kind == TUNER_KIND_HDHR && list[i].device_id == device_id) {
            hdhomerun_device_destroy(hd);
            return TUNER_ADD_DUPLICATE;
        }
    }

    // Legacy heuristic based on the model string
    bool is_legacy = (strstr(model_str, "hdhomerun3") != NULL || strstr(model_str, "hdhomerun_") != NULL);

    // Count tuners by probing
    int device_tuner_count = 0;
    for (int i = 0; i < 8; i++) {
        if (hdhomerun_device_set_tuner(hd, i) < 0) break;
        struct hdhomerun_tuner_status_t status;
        char *status_str;
        if (hdhomerun_device_get_tuner_status(hd, &status_str, &status) <= 0) break;
        device_tuner_count++;
    }
    hdhomerun_device_destroy(hd);

    if (device_tuner_count == 0) return TUNER_ADD_NO_TUNERS;
    for (int i = 0; i < device_tuner_count && *count < max; i++) {
        fill_info(&list[(*count)++], device_id, ip_str, i, device_tuner_count, is_legacy);
        (*added)++;
    }
    return TUNER_ADD_OK;
}

/* ---- Open / close / caps --------------------------------------------- */

static struct tuner *hdhr_open(const struct tuner_info *info)
{
    struct hdhr_tuner *ht = calloc(1, sizeof(*ht));
    if (!ht) return NULL;
    // Connect by IP: more reliable than a device-ID lookup, which needs
    // broadcast discovery
    ht->hd = hdhomerun_device_create_from_str(info->addr, NULL);
    if (!ht->hd) {
        free(ht);
        return NULL;
    }
    hdhomerun_device_set_tuner(ht->hd, info->tuner_index);
    return &ht->base;
}

static void hdhr_close(struct tuner *t)
{
    struct hdhr_tuner *ht = H(t);
    hdhomerun_device_destroy(ht->hd);
    free(ht);
}

static unsigned hdhr_caps(struct tuner *t)
{
    unsigned caps = TUNER_CAP_CHANNELMAP | TUNER_CAP_VLC | TUNER_CAP_TS | TUNER_CAP_DBG |
                    TUNER_CAP_PROGRAMS;
    struct hdhomerun_tuner_status_t status;
    char *raw;
    // Models that report dBm values support ALP pcap capture
    if (hdhomerun_device_get_tuner_status(H(t)->hd, &raw, &status) > 0 &&
        parse_db_value(raw, "ss=") != TUNER_NO_VALUE)
        caps |= TUNER_CAP_ALP_PCAP;
    return caps;
}

/* ---- Status ---------------------------------------------------------- */

static int hdhr_get_status(struct tuner *t, struct tuner_status *st)
{
    struct hdhomerun_tuner_status_t status;
    char *raw;
    int ret = hdhomerun_device_get_tuner_status(H(t)->hd, &raw, &status);

    memset(st, 0, sizeof(*st));
    st->strength_dbm = st->snr_db = st->bps = st->pps = TUNER_NO_VALUE;
    if (ret <= 0) return ret;

    snprintf(st->channel, sizeof(st->channel), "%s", status.channel);
    snprintf(st->lock_str, sizeof(st->lock_str), "%s", status.lock_str);
    st->signal_strength = status.signal_strength;
    st->signal_to_noise_quality = status.signal_to_noise_quality;
    st->symbol_error_quality = status.symbol_error_quality;
    st->strength_dbm = parse_db_value(raw, "ss=");
    st->snr_db = parse_db_value(raw, "snq=");
    st->bps = parse_status_value(raw, "bps=");
    st->pps = parse_status_value(raw, "pps=");
    return ret;
}

static int hdhr_get_vstatus(struct tuner *t, struct tuner_vstatus *vs)
{
    struct hdhomerun_tuner_vstatus_t vstatus;
    char *vstatus_str;
    memset(vs, 0, sizeof(*vs));
    if (hdhomerun_device_get_tuner_vstatus(H(t)->hd, &vstatus_str, &vstatus) <= 0 ||
        strlen(vstatus.vchannel) == 0)
        return 0;
    snprintf(vs->vchannel, sizeof(vs->vchannel), "%s", vstatus.vchannel);
    snprintf(vs->name, sizeof(vs->name), "%s", vstatus.name);
    return 1;
}

static int hdhr_get_plpinfo(struct tuner *t, const char **text)
{
    char *s;
    int ret = hdhomerun_device_get_tuner_plpinfo(H(t)->hd, &s);
    if (ret > 0) *text = s;
    return ret;
}

static int hdhr_get_streaminfo(struct tuner *t, const char **text)
{
    char *s;
    int ret = hdhomerun_device_get_tuner_streaminfo(H(t)->hd, &s);
    if (ret > 0) *text = s;
    return ret;
}

static int hdhr_get_target(struct tuner *t, const char **text)
{
    char *s;
    int ret = hdhomerun_device_get_tuner_target(H(t)->hd, &s);
    if (ret > 0) *text = s;
    return ret;
}

static int hdhr_get_errors(struct tuner *t, struct tuner_errors *err)
{
    char path[64], *debug_str;
    snprintf(path, sizeof(path), "/tuner%d/debug", t->info.tuner_index);
    int ret = hdhomerun_device_get_var(H(t)->hd, path, &debug_str, NULL);
    if (ret <= 0) return ret;
    err->transport = parse_status_value(debug_str, "te=");
    err->network = parse_status_value(debug_str, "ne=");
    err->sequence = parse_status_value(debug_str, "se=");
    return ret;
}

static int hdhr_get_version(struct tuner *t, const char **text)
{
    char *s;
    int ret = hdhomerun_device_get_var(H(t)->hd, "/sys/version", &s, NULL);
    if (ret > 0) *text = s;
    return ret;
}

static int hdhr_get_l1_string(struct tuner *t, const char **b64)
{
    struct hdhomerun_device_t *hd = H(t)->hd;
    struct hdhomerun_tuner_status_t status;
    char *raw, path[64], *s;

    // As the details screen always did: the status must report ss=, and the
    // firmware must have /tunerN/l1detail
    if (hdhomerun_device_get_tuner_status(hd, &raw, &status) <= 0 ||
        parse_status_value(raw, "ss=") == TUNER_NO_VALUE)
        return 0;
    if (firmware_version_number(hd) <= L1DETAIL_MIN_FIRMWARE) return 0;

    snprintf(path, sizeof(path), "/tuner%d/l1detail", t->info.tuner_index);
    if (hdhomerun_device_get_var(hd, path, &s, NULL) <= 0) return 0;
    *b64 = s;
    return 1;
}

/* ---- Tuning and channel maps ----------------------------------------- */

static int hdhr_set_channel(struct tuner *t, const char *spec)
{
    return hdhomerun_device_set_tuner_channel(H(t)->hd, spec);
}

static int hdhr_wait_for_lock(struct tuner *t)
{
    struct hdhomerun_tuner_status_t lock_status;
    return hdhomerun_device_wait_for_lock(H(t)->hd, &lock_status);
}

static int compare_uint(const void *a, const void *b)
{
    unsigned int x = *(const unsigned int *)a, y = *(const unsigned int *)b;
    return (x > y) - (x < y);
}

static int hdhr_get_channel_list(struct tuner *t, unsigned int *channels, int max)
{
    char *map_str;
    int count = 0;
    if (hdhomerun_device_get_tuner_channelmap(H(t)->hd, &map_str) <= 0) return 0;

    char *map_copy = strdup(map_str);
    if (!map_copy) return 0;

    // First token is the map name, then the channels
    char *token = strtok(map_copy, " ");
    token = strtok(NULL, " ");
    while (token != NULL && count < max) {
        channels[count++] = (unsigned int)strtoul(token, NULL, 10);
        token = strtok(NULL, " ");
    }
    free(map_copy);
    if (count > 0) qsort(channels, count, sizeof(unsigned int), compare_uint);
    return count;
}

static int hdhr_get_channelmap(struct tuner *t, char *name, size_t len)
{
    char *map_str;
    if (hdhomerun_device_get_tuner_channelmap(H(t)->hd, &map_str) <= 0) return 0;
    size_t n = strcspn(map_str, " ");
    snprintf(name, len, "%.*s", (int)n, map_str);
    return 1;
}

static int hdhr_get_channelmap_names(struct tuner *t, char **names, int max)
{
    char *features_str;
    int count = 0;
    if (hdhomerun_device_get_var(H(t)->hd, "/sys/features", &features_str, NULL) <= 0) return 0;

    char *features_copy = strdup(features_str);
    if (!features_copy) return 0;
    char *map_line_start = strstr(features_copy, "channelmap:");
    if (!map_line_start) {
        free(features_copy);
        return 0;
    }
    char *map_line_end = strchr(map_line_start, '\n');
    if (map_line_end) *map_line_end = '\0';

    char *token = strtok(map_line_start, " ");
    while (token != NULL && count < max) {
        if (strcmp(token, "channelmap:") != 0) names[count++] = strdup(token);
        token = strtok(NULL, " ");
    }
    free(features_copy);
    return count;
}

static int hdhr_set_channelmap(struct tuner *t, const char *name)
{
    return hdhomerun_device_set_tuner_channelmap(H(t)->hd, name);
}

/* ---- Captures -------------------------------------------------------- */

// HTTP GET of /tunerN/chNNN<plps>?format=...&duration=N from port 5004
static int http_open(struct hdhr_tuner *ht, const char *path, char *err, size_t errlen)
{
    const char *ip_addr = ht->base.info.addr;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(err, errlen, "Error: Could not create socket.");
        return -1;
    }
    int rcvbuf_size = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5004);
    if (inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr) <= 0) {
        snprintf(err, errlen, "Error: Invalid IP address.");
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        snprintf(err, errlen, "Error: Could not connect to device.");
        close(sock);
        return -1;
    }

    char request[512];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip_addr);
    if (send(sock, request, strlen(request), 0) < 0) {
        snprintf(err, errlen, "Error: Failed to send request.");
        close(sock);
        return -1;
    }
    // The device ends the stream after duration=; the caller polls
    fcntl(sock, F_SETFL, O_NONBLOCK);
    return sock;
}

static struct tuner_capture *hdhr_capture_start(struct tuner *t, enum tuner_capture_format fmt,
                                                unsigned rf_channel, const char *spec_suffix,
                                                int duration_s, char *debug_url, size_t debug_len,
                                                char *err, size_t errlen)
{
    struct hdhr_tuner *ht = H(t);
    struct hdhr_capture *c = calloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "Out of memory.");
        return NULL;
    }
    c->ht = ht;
    c->fmt = fmt;
    c->sock = -1;

    if (fmt == CAPTURE_TS) {
        if (hdhomerun_device_stream_start(ht->hd) <= 0) {
            snprintf(err, errlen, "Failed to start stream.");
            free(c);
            return NULL;
        }
        return &c->base;
    }

    char path[256];
    snprintf(path, sizeof(path), "/tuner%d/ch%u%s?format=%s&duration=%d",
             t->info.tuner_index, rf_channel, spec_suffix ? spec_suffix : "",
             fmt == CAPTURE_ALP_PCAP ? "alp-pcap" : "dbg", duration_s);
    if (debug_url && debug_len)
        snprintf(debug_url, debug_len, "http://%s:5004%s", t->info.addr, path);
    c->sock = http_open(ht, path, err, errlen);
    if (c->sock < 0) {
        free(c);
        return NULL;
    }
    return &c->base;
}

static long hdhr_capture_read(struct tuner_capture *base, uint8_t *buf, size_t len)
{
    struct hdhr_capture *c = (struct hdhr_capture *)base;

    if (c->fmt == CAPTURE_TS) {
        if (c->pend_len == 0) {
            size_t actual_size = 0;
            c->pend = hdhomerun_device_stream_recv(c->ht->hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);
            c->pend_len = c->pend ? actual_size : 0;
            if (c->pend_len == 0) return 0;
        }
        size_t n = c->pend_len < len ? c->pend_len : len;
        memcpy(buf, c->pend, n);
        c->pend += n;
        c->pend_len -= n;
        return (long)n;
    }

    for (;;) {
        ssize_t got = recv(c->sock, buf, len, 0);
        if (got == 0) return -1;                       // server closed: finished
        if (got < 0)
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -2;
        if (c->headers_done) return (long)got;

        // Skip the HTTP response header, which may span several reads
        size_t keep = sizeof(c->hdr) - c->hdr_len;
        size_t take = (size_t)got < keep ? (size_t)got : keep;
        memcpy(c->hdr + c->hdr_len, buf, take);
        size_t before = c->hdr_len;
        c->hdr_len += take;
        for (size_t i = before >= 3 ? before - 3 : 0; i + 4 <= c->hdr_len; i++) {
            if (memcmp(c->hdr + i, "\r\n\r\n", 4) == 0) {
                size_t body_off = i + 4 - before;      // offset of the body in buf
                c->headers_done = true;
                if ((size_t)got > body_off) {
                    memmove(buf, buf + body_off, got - body_off);
                    return (long)(got - body_off);
                }
                return 0;
            }
        }
        if (c->hdr_len == sizeof(c->hdr)) return -2;   // no end of header
        // header continues in the next read
    }
}

static void hdhr_capture_stop(struct tuner_capture *base)
{
    struct hdhr_capture *c = (struct hdhr_capture *)base;
    if (c->fmt == CAPTURE_TS)
        hdhomerun_device_stream_stop(c->ht->hd);
    else if (c->sock >= 0)
        close(c->sock);
    free(c);
}

/* ---- Live viewing ---------------------------------------------------- */

static int hdhr_set_program(struct tuner *t, const char *program)
{
    char path[64];
    snprintf(path, sizeof(path), "/tuner%d/program", t->info.tuner_index);
    return hdhomerun_device_set_var(H(t)->hd, path, program, NULL, NULL) < 0 ? -1 : 1;
}

static int hdhr_set_target_rtp(struct tuner *t, int port, char *err, size_t errlen)
{
    // Find the local address on the interface that reaches the HDHomeRun
    char local_ip[INET_ADDRSTRLEN];
    int temp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (temp_sock < 0) {
        snprintf(err, errlen, "Socket error determining local IP.");
        return -1;
    }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(65001);
    if (inet_pton(AF_INET, t->info.addr, &serv.sin_addr) <= 0) {
        close(temp_sock);
        snprintf(err, errlen, "Invalid device IP address.");
        return -1;
    }
    if (connect(temp_sock, (const struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(temp_sock);
        snprintf(err, errlen, "Connect error determining local IP.");
        return -1;
    }

    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(temp_sock, (struct sockaddr *)&name, &namelen) < 0) {
        close(temp_sock);
        snprintf(err, errlen, "getsockname error determining local IP.");
        return -1;
    }
    close(temp_sock);

    if (inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip)) == NULL) {
        snprintf(err, errlen, "inet_ntop error determining local IP.");
        return -1;
    }

    char path[64], target_str[128];
    snprintf(path, sizeof(path), "/tuner%d/target", t->info.tuner_index);
    snprintf(target_str, sizeof(target_str), "rtp://%s:%d", local_ip, port);
    if (hdhomerun_device_set_var(H(t)->hd, path, target_str, NULL, NULL) < 0) {
        snprintf(err, errlen, "Failed to set stream target.");
        return -1;
    }
    return 1;
}

static int hdhr_clear_target(struct tuner *t)
{
    return hdhomerun_device_set_tuner_target(H(t)->hd, "none");
}

const struct tuner_backend tuner_backend_hdhr = {
    .name = "HDHomeRun",
    .discover = hdhr_discover,
    .add_by_address = hdhr_add_by_address,
    .open = hdhr_open,
    .close = hdhr_close,
    .caps = hdhr_caps,
    .get_status = hdhr_get_status,
    .get_vstatus = hdhr_get_vstatus,
    .get_plpinfo = hdhr_get_plpinfo,
    .get_streaminfo = hdhr_get_streaminfo,
    .get_target = hdhr_get_target,
    .get_errors = hdhr_get_errors,
    .get_version = hdhr_get_version,
    .get_l1_string = hdhr_get_l1_string,
    .set_channel = hdhr_set_channel,
    .wait_for_lock = hdhr_wait_for_lock,
    .get_channel_list = hdhr_get_channel_list,
    .get_channelmap = hdhr_get_channelmap,
    .get_channelmap_names = hdhr_get_channelmap_names,
    .set_channelmap = hdhr_set_channelmap,
    .capture_start = hdhr_capture_start,
    .capture_read = hdhr_capture_read,
    .capture_stop = hdhr_capture_stop,
    .set_program = hdhr_set_program,
    .set_target_rtp = hdhr_set_target_rtp,
    .clear_target = hdhr_clear_target,
};
