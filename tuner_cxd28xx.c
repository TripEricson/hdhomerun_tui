/*
 * tuner_cxd28xx.c
 *
 * Backend for USB ATSC 1.0 / 3.0 tuners on the cxd28xx Linux driver (Sony
 * CXD2878 demod; e.g. the GTMedia / Zenview HDTV Mate). Uses only what a
 * normal user in the "video" group can open:
 *
 *   /dev/dvb/adapterN/frontend0   tuning, lock, signal level, SNR, errors
 *   /dev/dvb/adapterN/demux0+dvr0 the MPEG-TS (ATSC 1.0 captures)
 *   /dev/alprawN                  L1 signalling (ioctl), USB transfer size
 *                                 (ioctl), and the ALP packets (read)
 *
 * What it reports in the common formats (tuner.h):
 *   status channel   ATSC 3.0: "atsc3:<RF channel>:<PLPs being decoded>",
 *                    e.g. "atsc3:25:0+1"; ATSC 1.0: "auto:33" (or "8vsb:33"
 *                    when tuned as 8VSB explicitly); lock "atsc3" / "8vsb"
 *   plpinfo          every PLP in L1-Detail, in the HDHomeRun's format, with
 *                    lock=1 on the ones being decoded
 *   L1 string        rebuilt from the demod's registers, as alp-capture does
 *   captures         ALP pcap (ATSC 3.0) and MPEG-TS (ATSC 1.0); the TS
 *                    error counters are counted from the stream
 *
 * Not available from this tuner yet: program list and TSID (from the
 * broadcast's tables, not the demod), live viewing, .dbg captures and
 * channel maps.
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include "alp_raw.h"
#include "tuner.h"
#include "l1_detail_parser.h"

#ifndef SYS_ATSC3
#define SYS_ATSC3 35                   // not in older kernel headers
#endif

#define MAX_ADAPTERS 16
#define NO_STREAM_ID_FILTER (~0U)      // no PLP selection given
#define DEFAULT_STREAM_ID 0xFFFFFF00u  // PLP 0 alone, as the HDHomeRun defaults
#define STATUS_CACHE_MS 250            // status is polled several times a frame
#define L1_CACHE_MS 1000               // L1 changes rarely and costs I2C reads
#define LINKTYPE_ATSC_ALP 289
#define REC_BUF (sizeof(struct alp_raw_hdr) + 65536)
#define ATSC1_BPS 19392658             // 8VSB payload rate, as the HDHomeRun reports it
// "auto" tuning tries each standard for this long before switching
#define AUTO_TRY_8VSB_MS 700
#define AUTO_TRY_ATSC3_MS 1500

struct cx_tuner {
    struct tuner base;                 // must be first
    int fe;                            // frontend, O_RDWR (or O_RDONLY if in use)
    int raw;                           // /dev/alprawN for ioctls (never read)
    bool read_only;
    int adapter;
    char fe_name[128];

    // what we last tuned to
    bool tuned;
    unsigned freq_hz;
    unsigned stream_id;
    int delsys;                        // SYS_ATSC3 or SYS_ATSC (8VSB)
    bool auto_mode;                    // tuned with "auto:": trying both standards
    bool auto_found;                   // ... and one has locked
    struct timespec attempt_at;        // start of the current auto attempt
    char spec[64];

    // TS error counters (HDHomeRun te/ne/se), counted during TS captures
    long ts_te, ts_ne, ts_se;
    uint8_t cc[8192];                  // last continuity counter per PID, 0xFF = none

    // caches
    struct timespec status_at, l1_at;
    struct tuner_status status;
    int status_ret;
    char plpinfo[4096];
    int plpinfo_ok;
    char *l1_b64;
    int decoded[4], n_decoded;
    double decoded_bps;                // bitrate of the decoded PLPs, from L1
    char version[160];
};

struct cx_capture {
    struct tuner_capture base;         // must be first
    struct cx_tuner *ct;
    enum tuner_capture_format fmt;
    int dmx;                           // CAPTURE_TS: demux0 (all-PIDs filter) ...
    int dvr;                           // ... and dvr0, read
    uint8_t carry[188];                // partial TS packet, for counting errors
    size_t carry_len;
    int fd;                            // CAPTURE_ALP_PCAP: our own /dev/alprawN: reading claims it
    struct timespec start;
    int duration_s;
    bool header_done;
    uint8_t rec[REC_BUF];
    uint8_t out[16 + 65536];           // one pcap record, being handed out
    size_t out_len, out_pos;
};

static struct cx_tuner *C(struct tuner *t) { return (struct cx_tuner *)t; }

static long ms_since(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!t0->tv_sec && !t0->tv_nsec) return 1L << 30;
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

/* ---- US channel plan ------------------------------------------------- */

// RF channel -> centre frequency (Hz), post-repack plan (TV ends at 36)
static unsigned us_channel_hz(long ch)
{
    if (ch >= 2 && ch <= 4) return (57 + 6 * (ch - 2)) * 1000000u;
    if (ch >= 5 && ch <= 6) return (79 + 6 * (ch - 5)) * 1000000u;
    if (ch >= 7 && ch <= 13) return (177 + 6 * (ch - 7)) * 1000000u;
    if (ch >= 14 && ch <= 36) return (473 + 6 * (ch - 14)) * 1000000u;
    return 0;
}

static long us_hz_channel(unsigned hz)
{
    for (long ch = 2; ch <= 36; ch++) {
        unsigned c = us_channel_hz(ch);
        if (c && (hz > c ? hz - c : c - hz) < 1000000) return ch;
    }
    return 0;
}

/* ---- /dev/alpraw L1 access ------------------------------------------- */

static int l1_get(int fd, unsigned what, uint8_t *buf, unsigned len)
{
    struct alp_raw_l1 req = { .what = what, .len = len, .buf = (uint64_t)(uintptr_t)buf };
    if (ioctl(fd, ALP_RAW_IOC_GET_L1, &req) < 0) return -errno;
    return (int)req.len;
}

// Bank 93h from the demod register snapshot. 0 or -errno.
static int read_bank93(int fd, uint8_t *r93, uint8_t *have)
{
    uint8_t snap[1024];
    int sn = l1_get(fd, ALP_RAW_L1_SNAPSHOT, snap, sizeof(snap));
    if (sn < 0) return sn;
    if (sn < 5 || memcmp(snap, "CXA3", 4)) return -EINVAL;
    memset(have, 0, 256);
    for (int pos = 5; pos + 4 <= sn; pos += 4 + snap[pos + 2]) {
        uint8_t bank = snap[pos], addr = snap[pos + 1], n = snap[pos + 2];
        if (bank != 0x93 || snap[pos + 3] || pos + 4 + n > sn) continue;
        for (int i = 0; i < n; i++) {
            r93[(uint8_t)(addr + i)] = snap[pos + 4 + i];
            have[(uint8_t)(addr + i)] = 1;
        }
    }
    return 0;
}

struct bitw { uint8_t *p; int pos; };

static void put_bits(struct bitw *w, unsigned v, int n)
{
    while (n--) {
        if ((v >> n) & 1) w->p[w->pos >> 3] |= 0x80 >> (w->pos & 7);
        w->pos++;
    }
}

static char *b64(const uint8_t *in, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((n + 2) / 3 * 4 + 1), *o = out;
    size_t i;
    if (!out) return NULL;
    for (i = 0; i + 2 < n; i += 3) {
        *o++ = t[in[i] >> 2];
        *o++ = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *o++ = t[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *o++ = t[in[i + 2] & 63];
    }
    if (n - i == 1) {
        *o++ = t[in[i] >> 2]; *o++ = t[(in[i] & 3) << 4]; *o++ = '='; *o++ = '=';
    } else if (n - i == 2) {
        *o++ = t[in[i] >> 2]; *o++ = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *o++ = t[(in[i + 1] & 15) << 2]; *o++ = '=';
    }
    *o = 0;
    return out;
}

/*
 * base64(L1-Basic + L1-Detail), the HDHomeRun /tunerN/l1detail format.
 * L1-Basic is rebuilt from bank 93h, 60h-78h (see cxd2878_atsc3_regs.h in the
 * driver): A/322's bit fields in order, plus a zero CRC as the HDHomeRun
 * writes it. Also returns the PLPs being decoded (slots 93h:80h-83h).
 */
static char *build_l1_string(int fd, int decoded[4], int *n_decoded)
{
    static uint8_t l1d[8192 + 25];
    uint8_t r93[256], have[256];
    struct bitw w = { l1d, 0 };

    *n_decoded = 0;
    if (read_bank93(fd, r93, have) || !have[0x10] || !have[0x78] || !have[0x83]) return NULL;
    for (int i = 0; i < 4; i++)
        if (r93[0x80 + i] & 0x80) decoded[(*n_decoded)++] = r93[0x80 + i] & 0x3F;
    if ((r93[0x10] & 3) != 3) return NULL;   // L1 not decoded yet

    memset(l1d, 0, 25);
    put_bits(&w, r93[0x60], 8);
    put_bits(&w, r93[0x61] & 7, 3);
    if (!(r93[0x61] & 1)) {
        put_bits(&w, r93[0x62], 8);
        put_bits(&w, r93[0x63] >> 6, 2);
        put_bits(&w, r93[0x63] & 0x3F, 6);
        put_bits(&w, r93[0x64] & 0x7F, 7);
    } else {
        put_bits(&w, r93[0x65], 8);
        put_bits(&w, r93[0x66], 8);
        put_bits(&w, r93[0x67] & 0x7F, 7);
    }
    for (int i = 0x68; i <= 0x71; i++) put_bits(&w, r93[i], 8);
    put_bits(&w, r93[0x72] & 0x3F, 6);
    for (int i = 0x73; i <= 0x78; i++) put_bits(&w, r93[i], 8);
    put_bits(&w, 0, 32);
    if (w.pos != 200) return NULL;

    unsigned l1d_size = (r93[0x6A] << 5) | (r93[0x6B] >> 3);
    int dn = l1_get(fd, ALP_RAW_L1_DETAIL, l1d + 25, sizeof(l1d) - 25);
    if (dn < 0 || (unsigned)dn != l1d_size) return NULL;
    return b64(l1d, 25 + dn);
}

/*
 * Refresh L1 (at most once a second): the L1 string, the decoded PLPs, and
 * plpinfo text built from the shared L1 parser's output, so there is one
 * L1-Detail parser for the TUI, RENDER and this backend.
 */
static void refresh_l1(struct cx_tuner *ct)
{
    if (ms_since(&ct->l1_at) < L1_CACHE_MS) return;
    clock_gettime(CLOCK_MONOTONIC, &ct->l1_at);

    free(ct->l1_b64);
    ct->l1_b64 = build_l1_string(ct->raw, ct->decoded, &ct->n_decoded);
    ct->plpinfo_ok = 0;
    ct->decoded_bps = 0;
    if (!ct->l1_b64) return;

    struct l1_detail_info *info = create_l1_detail_info(MAX_DISPLAY_LINES);
    if (!info) return;
    if (append_l1_detail_from_base64(info, ct->l1_b64) == 0) {
        struct { int id, sfi, lls; char layer[16], mod[32], cod[16], ti[8]; } plps[MAX_PLPS];
        int n = 0, cur = -1, rate_id = -1, subframe = 0;
        long bsid = -1;
        bool rate_seen[64] = { false };
        for (int i = 0; i < info->line_count; i++) {
            const char *l = info->display_lines[i], *v;
            int id;
            if ((v = strstr(l, "L1D_bsid: ")) && l == strstr(l, "L1D_bsid")) {
                bsid = strtol(v + 10, NULL, 0);
            } else if (!strncmp(l, "Subframe #", 10)) {
                subframe = atoi(l + 10);
            } else if ((v = strstr(l, "-> PLP Bitrate: "))) {
                // per subframe; a PLP is counted once
                for (int d = 0; d < ct->n_decoded; d++)
                    if (rate_id >= 0 && rate_id < 64 && ct->decoded[d] == rate_id && !rate_seen[rate_id]) {
                        ct->decoded_bps += strtod(v + 16, NULL) * 1e6;   // "x.xxx Mbps"
                        rate_seen[rate_id] = true;
                    }
            } else if ((v = strstr(l, "L1D_plp_id: "))) {
                id = atoi(v + 12);
                rate_id = id;
                cur = -1;
                for (int k = 0; k < n; k++) if (plps[k].id == id) cur = -2;   // seen in an earlier subframe
                if (cur == -1 && n < MAX_PLPS) {
                    cur = n++;
                    plps[cur].id = id;
                    plps[cur].sfi = subframe;
                    plps[cur].lls = 0;
                    strcpy(plps[cur].layer, "core");
                    plps[cur].mod[0] = plps[cur].cod[0] = plps[cur].ti[0] = '\0';
                }
            } else if (cur >= 0 && (v = strstr(l, "L1D_plp_lls_flag: "))) {
                plps[cur].lls = atoi(v + 18) ? 1 : 0;
            } else if (cur >= 0 && (v = strstr(l, "L1D_plp_TI_mode: "))) {
                snprintf(plps[cur].ti, sizeof(plps[cur].ti), "%s",
                         strstr(v, "HTI") ? "hti" : strstr(v, "CTI") ? "cti" : strstr(v, "No TI") ? "none" : "");
            } else if (cur >= 0 && (v = strstr(l, "L1D_plp_layer: "))) {
                snprintf(plps[cur].layer, sizeof(plps[cur].layer), "%s",
                         strstr(v, "Enhanced") ? "enhanced" : "core");
            } else if (cur >= 0 && (v = strstr(l, "L1D_plp_mod: "))) {
                // "256QAM" -> "qam256", "QPSK" -> "qpsk", like the HDHomeRun
                const char *m = v + 13;
                char digits[8] = "", alpha[16] = "";
                int di = 0, ai = 0;
                for (; *m && *m != ' '; m++) {
                    if (*m >= '0' && *m <= '9') { if (di < 7) digits[di++] = *m; }
                    else if (ai < 15) alpha[ai++] = (char)(*m | 0x20);
                }
                snprintf(plps[cur].mod, sizeof(plps[cur].mod), "%s%s", alpha, digits);
            } else if (cur >= 0 && (v = strstr(l, "L1D_plp_cod: "))) {
                snprintf(plps[cur].cod, sizeof(plps[cur].cod), "%.*s",
                         (int)strcspn(v + 13, " "), v + 13);
            }
        }
        size_t o = 0;
        if (bsid >= 0)
            o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, "bsid=0x%04lx\n", bsid);
        // Same fields and order as the HDHomeRun's plpinfo, e.g.
        //   0: sfi=0 mod=qam64 cod=11/15 layer=core ti=hti lls=1 lock=1
        // with lock=1 only on the PLPs being decoded
        for (int k = 0; k < n && o < sizeof(ct->plpinfo); k++) {
            bool locked = false;
            for (int d = 0; d < ct->n_decoded; d++) if (ct->decoded[d] == plps[k].id) locked = true;
            o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, "%d: sfi=%d", plps[k].id, plps[k].sfi);
            if (plps[k].mod[0] && o < sizeof(ct->plpinfo))
                o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, " mod=%s cod=%s",
                              plps[k].mod, plps[k].cod);
            if (o < sizeof(ct->plpinfo))
                o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, " layer=%s", plps[k].layer);
            if (plps[k].ti[0] && o < sizeof(ct->plpinfo))
                o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, " ti=%s", plps[k].ti);
            if (o < sizeof(ct->plpinfo))
                o += snprintf(ct->plpinfo + o, sizeof(ct->plpinfo) - o, " lls=%d%s\n",
                              plps[k].lls, locked ? " lock=1" : "");
        }
        ct->plpinfo_ok = n > 0;
    }
    free_l1_detail_info(info);
}

/* ---- Discovery / open ------------------------------------------------ */

static bool fe_supports_atsc3(int fe)
{
    struct dtv_property p = { .cmd = DTV_ENUM_DELSYS };
    struct dtv_properties cmd = { .num = 1, .props = &p };
    if (ioctl(fe, FE_GET_PROPERTY, &cmd) < 0) return false;
    for (unsigned i = 0; i < p.u.buffer.len; i++)
        if (p.u.buffer.data[i] == SYS_ATSC3) return true;
    return false;
}

static int cx_discover(struct tuner_info list[], int max)
{
    int n = 0;
    for (int a = 0; a < MAX_ADAPTERS && n < max; a++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/alpraw%d", a);
        int raw = open(path, O_RDONLY | O_NONBLOCK);
        if (raw < 0) continue;
        close(raw);

        snprintf(path, sizeof(path), "/dev/dvb/adapter%d/frontend0", a);
        int fe = open(path, O_RDONLY | O_NONBLOCK);
        if (fe < 0) continue;
        struct dvb_frontend_info fi;
        char fe_name[sizeof(fi.name) + 1] = "";
        if (ioctl(fe, FE_GET_INFO, &fi) == 0) snprintf(fe_name, sizeof(fe_name), "%s", fi.name);
        bool ok = fe_supports_atsc3(fe);
        close(fe);
        if (!ok) continue;

        struct tuner_info *ti = &list[n++];
        memset(ti, 0, sizeof(*ti));
        ti->kind = TUNER_KIND_CXD28XX;
        ti->tuner_index = a;
        ti->total_tuners_on_device = 1;
        snprintf(ti->addr, sizeof(ti->addr), "/dev/dvb/adapter%d", a);
        snprintf(ti->name, sizeof(ti->name), "GTMedia-%d", a);
        snprintf(ti->title, sizeof(ti->title), "Tuner GTMedia-%d (%.24s%s%.60s)", a, ti->addr,
                 fe_name[0] ? ", " : "", fe_name);
    }
    return n;
}

static struct tuner *cx_open(const struct tuner_info *info)
{
    struct cx_tuner *ct = calloc(1, sizeof(*ct));
    char path[64];
    if (!ct) return NULL;
    ct->adapter = info->tuner_index;

    snprintf(path, sizeof(path), "/dev/dvb/adapter%d/frontend0", ct->adapter);
    ct->fe = open(path, O_RDWR | O_NONBLOCK);
    if (ct->fe < 0 && errno == EBUSY) {
        // Another program (alp-capture, atsc3-zap) is tuning: watch only
        ct->fe = open(path, O_RDONLY | O_NONBLOCK);
        ct->read_only = true;
    }
    snprintf(path, sizeof(path), "/dev/alpraw%d", ct->adapter);
    ct->raw = open(path, O_RDONLY | O_NONBLOCK);
    if (ct->fe < 0 || ct->raw < 0) {
        if (ct->fe >= 0) close(ct->fe);
        if (ct->raw >= 0) close(ct->raw);
        free(ct);
        return NULL;
    }

    struct dvb_frontend_info fi;
    if (ioctl(ct->fe, FE_GET_INFO, &fi) == 0) snprintf(ct->fe_name, sizeof(ct->fe_name), "%s", fi.name);
    snprintf(ct->version, sizeof(ct->version), "cxd28xx driver, %s%s",
             ct->fe_name[0] ? ct->fe_name : "frontend",
             ct->read_only ? " (in use by another program: read-only)" : "");
    snprintf(ct->spec, sizeof(ct->spec), "none");
    ct->delsys = SYS_ATSC3;
    memset(ct->cc, 0xFF, sizeof(ct->cc));
    return &ct->base;
}

static void cx_close(struct tuner *t)
{
    struct cx_tuner *ct = C(t);
    close(ct->fe);
    close(ct->raw);
    free(ct->l1_b64);
    free(ct);
}

static unsigned cx_caps(struct tuner *t)
{
    (void)t;
    return TUNER_CAP_ALP_PCAP | TUNER_CAP_TS;
}

static void drop_caches(struct cx_tuner *ct)
{
    memset(&ct->status_at, 0, sizeof(ct->status_at));
    memset(&ct->l1_at, 0, sizeof(ct->l1_at));
}

// Tune the frontend: ATSC 3.0 with a PLP selection, or 8VSB
static int tune_hw(struct cx_tuner *ct, int delsys, unsigned hz, unsigned stream_id)
{
    struct dtv_property p[5];
    struct dtv_properties cmd = { .props = p };
    memset(p, 0, sizeof(p));
    p[0].cmd = DTV_CLEAR;
    cmd.num = 1;
    if (ioctl(ct->fe, FE_SET_PROPERTY, &cmd) < 0) return -1;

    int n = 0;
    memset(p, 0, sizeof(p));
    p[n].cmd = DTV_DELIVERY_SYSTEM; p[n++].u.data = delsys;
    p[n].cmd = DTV_FREQUENCY;       p[n++].u.data = hz;
    if (delsys == SYS_ATSC3) {
        p[n].cmd = DTV_BANDWIDTH_HZ; p[n++].u.data = 6000000;
        p[n].cmd = DTV_STREAM_ID;    p[n++].u.data = stream_id;
    } else {
        p[n].cmd = DTV_MODULATION;   p[n++].u.data = VSB_8;
    }
    p[n++].cmd = DTV_TUNE;
    cmd.num = n;
    if (ioctl(ct->fe, FE_SET_PROPERTY, &cmd) < 0) return -1;

    ct->delsys = delsys;
    clock_gettime(CLOCK_MONOTONIC, &ct->attempt_at);
    drop_caches(ct);
    return 1;
}

/*
 * "auto" tuning, like the HDHomeRun's: try 8VSB, then ATSC 3.0 (PLP 0), and
 * keep alternating until one locks. Called from the status and wait paths,
 * so tuning itself returns at once (the TUI's seek polls the status).
 */
static void auto_step(struct cx_tuner *ct)
{
    enum fe_status fst = 0;
    if (!ct->auto_mode || ct->auto_found) return;
    if (ioctl(ct->fe, FE_READ_STATUS, &fst) == 0 && (fst & FE_HAS_LOCK)) {
        ct->auto_found = true;
        return;
    }
    long window = ct->delsys == SYS_ATSC ? AUTO_TRY_8VSB_MS : AUTO_TRY_ATSC3_MS;
    if (ms_since(&ct->attempt_at) >= window) {
        int next = ct->delsys == SYS_ATSC ? SYS_ATSC3 : SYS_ATSC;
        tune_hw(ct, next, ct->freq_hz, DEFAULT_STREAM_ID);
    }
}

/* ---- Status ---------------------------------------------------------- */

// Pre-BCH BER below this is corrected by BCH (it corrects ~2e-4 of a frame)
#define BER_CLEAN 1e-4

static int cx_get_status(struct tuner *t, struct tuner_status *st)
{
    struct cx_tuner *ct = C(t);

    if (ms_since(&ct->status_at) >= STATUS_CACHE_MS) {
        struct tuner_status *s = &ct->status;
        enum fe_status fst = 0;

        auto_step(ct);
        clock_gettime(CLOCK_MONOTONIC, &ct->status_at);
        memset(s, 0, sizeof(*s));
        s->strength_dbm = s->snr_db = s->bps = s->pps = TUNER_NO_VALUE;

        if (ioctl(ct->fe, FE_READ_STATUS, &fst) < 0) {
            ct->status_ret = -1;
            *st = *s;
            return -1;
        }
        bool locked = fst & FE_HAS_LOCK;

        // Stats from the frontend cache, refreshed by FE_READ_STATUS.
        // cxd2878: strength [0] dBm*1000, [1] relative; CNR likewise;
        // post_bit_error [0] = the PLP's pre-BCH BER * 1e9.
        struct dtv_property p[5];
        struct dtv_properties cmd = { .num = 5, .props = p };
        memset(p, 0, sizeof(p));
        p[0].cmd = DTV_STAT_SIGNAL_STRENGTH;
        p[1].cmd = DTV_STAT_CNR;
        p[2].cmd = DTV_STAT_POST_ERROR_BIT_COUNT;
        p[3].cmd = DTV_FREQUENCY;
        p[4].cmd = DTV_DELIVERY_SYSTEM;
        double ber = -1, snr = NAN;
        if (ioctl(ct->fe, FE_GET_PROPERTY, &cmd) == 0) {
            if (p[0].u.st.len >= 1 && p[0].u.st.stat[0].scale == FE_SCALE_DECIBEL)
                s->strength_dbm = (long)lround(p[0].u.st.stat[0].svalue / 1000.0);
            if (p[0].u.st.len >= 2 && p[0].u.st.stat[1].scale == FE_SCALE_RELATIVE)
                s->signal_strength = (int)(p[0].u.st.stat[1].uvalue * 100 / 65535);
            if (locked && p[1].u.st.len >= 1 && p[1].u.st.stat[0].scale == FE_SCALE_DECIBEL) {
                snr = p[1].u.st.stat[0].svalue / 1000.0;
                s->snr_db = (long)lround(snr);
            }
            if (locked && p[1].u.st.len >= 2 && p[1].u.st.stat[1].scale == FE_SCALE_RELATIVE)
                s->signal_to_noise_quality = (int)(p[1].u.st.stat[1].uvalue * 100 / 65535);
            if (locked && p[2].u.st.len >= 1 && p[2].u.st.stat[0].scale == FE_SCALE_COUNTER)
                ber = p[2].u.st.stat[0].uvalue / 1e9;
            if (!ct->tuned && p[3].u.data) {            // tuned by someone else
                ct->freq_hz = p[3].u.data;
                if (p[4].u.data == SYS_ATSC || p[4].u.data == SYS_ATSC3) ct->delsys = p[4].u.data;
            }
        }
        bool atsc1 = (ct->delsys == SYS_ATSC);
        if (s->signal_strength > 100) s->signal_strength = 100;
        if (s->signal_to_noise_quality > 100) s->signal_to_noise_quality = 100;

        // Symbol quality. ATSC 3.0: 100 while BCH can correct what LDPC
        // leaves, then falling with the BER (25 points per decade).
        // ATSC 1.0: the driver reports no error rate, so from the SNR:
        // 100 at 16 dB and up (8VSB needs about 15), 0 at 12 dB.
        if (locked && !atsc1) {
            if (ber < BER_CLEAN) s->symbol_error_quality = 100;
            else s->symbol_error_quality = (int)fmax(0, 100 + 25 * log10(BER_CLEAN / ber));
        } else if (locked && !isnan(snr)) {
            s->symbol_error_quality = (int)fmax(0, fmin(100, (snr - 12.0) * 25));
        }

        // Channel spec and lock, HDHomeRun style
        unsigned hz = ct->freq_hz;
        long ch = us_hz_channel(hz);
        char where[24];
        if (ch) snprintf(where, sizeof(where), "%ld", ch);
        else snprintf(where, sizeof(where), "%u", hz);
        if (!hz) {
            snprintf(s->channel, sizeof(s->channel), "none");
        } else if (atsc1 || (ct->auto_mode && !locked)) {
            snprintf(s->channel, sizeof(s->channel), "%s:%s", ct->auto_mode ? "auto" : "8vsb", where);
            if (atsc1 && locked) s->bps = ATSC1_BPS;
        } else if (locked) {
            refresh_l1(ct);
            char plps[32] = "";
            size_t o = 0;
            for (int d = 0; d < ct->n_decoded && o < sizeof(plps); d++)
                o += snprintf(plps + o, sizeof(plps) - o, "%s%d", d ? "+" : "", ct->decoded[d]);
            snprintf(s->channel, sizeof(s->channel), "atsc3:%s%s%s", where, plps[0] ? ":" : "", plps);
            if (ct->decoded_bps > 0) s->bps = (long)ct->decoded_bps;   // shown as Network Rate
        } else {
            snprintf(s->channel, sizeof(s->channel), "atsc3:%s", where);
        }
        snprintf(s->lock_str, sizeof(s->lock_str), "%s", !locked ? "none" : atsc1 ? "8vsb" : "atsc3");
        ct->status_ret = 1;
    }
    *st = ct->status;
    return ct->status_ret;
}

static int cx_get_plpinfo(struct tuner *t, const char **text)
{
    struct cx_tuner *ct = C(t);
    struct tuner_status st;
    if (cx_get_status(t, &st) <= 0 || strcmp(st.lock_str, "atsc3")) return 0;
    refresh_l1(ct);
    if (!ct->plpinfo_ok) return 0;
    *text = ct->plpinfo;
    return 1;
}

static int cx_get_errors(struct tuner *t, struct tuner_errors *err)
{
    struct cx_tuner *ct = C(t);
    err->transport = ct->ts_te;
    err->network = ct->ts_ne;
    err->sequence = ct->ts_se;
    return 1;
}

static int cx_get_version(struct tuner *t, const char **text)
{
    *text = C(t)->version;
    return 1;
}

static int cx_get_l1_string(struct tuner *t, const char **b64s)
{
    struct cx_tuner *ct = C(t);
    if (ct->delsys != SYS_ATSC3) return 0;
    refresh_l1(ct);
    if (!ct->l1_b64) return 0;
    *b64s = ct->l1_b64;
    return 1;
}

/* ---- Tuning ---------------------------------------------------------- */

/*
 * Accepts the common channel spec, where <x> is an RF channel (2-69), a
 * frequency in MHz, or in Hz:
 *   "auto:<x>" or "<x>"         ATSC 1.0 or 3.0, whichever locks (auto_step)
 *   "atsc3:<x>[:<p>+<p>...]"    ATSC 3.0 with those PLPs
 *   "8vsb:<x>"                  ATSC 1.0
 * ATSC 3.0 without PLPs decodes PLP 0 alone, as on the HDHomeRun: leaving
 * the choice to the demod decodes every PLP, which some stations' PLP
 * combinations don't allow (it never locks), and the demod then doesn't
 * report which PLPs it's decoding.
 */
static int cx_set_channel(struct tuner *t, const char *spec)
{
    struct cx_tuner *ct = C(t);
    char buf[64], *where, *plps;
    enum { MODE_AUTO, MODE_ATSC3, MODE_8VSB } mode = MODE_AUTO;

    if (ct->read_only) return -1;
    snprintf(buf, sizeof(buf), "%s", spec);
    where = buf;
    if (!strncmp(where, "auto:", 5)) where += 5;
    else if (!strncmp(where, "atsc3:", 6)) { where += 6; mode = MODE_ATSC3; }
    else if (!strncmp(where, "8vsb:", 5)) { where += 5; mode = MODE_8VSB; }
    plps = strchr(where, ':');
    if (plps) *plps++ = '\0';

    unsigned long x = strtoul(where, NULL, 10);
    unsigned hz = x <= 69 ? us_channel_hz((long)x) : x < 1000000 ? (unsigned)(x * 1000000) : (unsigned)x;
    if (!hz) return -1;

    unsigned stream_id = NO_STREAM_ID_FILTER;
    if (plps && *plps) {
        int slot = 0;
        stream_id = 0;
        for (char *tok = strtok(plps, "+,"); tok && slot < 4; tok = strtok(NULL, "+,")) {
            unsigned long id = strtoul(tok, NULL, 10);
            if (id > 63) return -1;
            stream_id |= (unsigned)id << (8 * slot++);
        }
        if (!slot) stream_id = NO_STREAM_ID_FILTER;
        for (; slot < 4 && stream_id != NO_STREAM_ID_FILTER; slot++) stream_id |= 0xFFu << (8 * slot);
        if (stream_id != NO_STREAM_ID_FILTER) mode = MODE_ATSC3;   // PLPs mean ATSC 3.0
    }
    if (stream_id == NO_STREAM_ID_FILTER) stream_id = DEFAULT_STREAM_ID;

    // Retuning to where we already are only costs the lock (e.g. the TUI
    // restores the channel after every capture), so skip it
    enum fe_status fst = 0;
    bool locked_now = ioctl(ct->fe, FE_READ_STATUS, &fst) == 0 && (fst & FE_HAS_LOCK);
    if (ct->tuned && hz == ct->freq_hz && locked_now &&
        ((mode == MODE_AUTO && ct->auto_mode) ||
         (mode == MODE_ATSC3 && ct->delsys == SYS_ATSC3 && stream_id == ct->stream_id) ||
         (mode == MODE_8VSB && ct->delsys == SYS_ATSC)))
        return 1;

    int delsys = (mode == MODE_ATSC3) ? SYS_ATSC3 : SYS_ATSC;   // auto starts with 8VSB
    if (tune_hw(ct, delsys, hz, stream_id) < 0) return -1;

    ct->tuned = true;
    ct->freq_hz = hz;
    ct->stream_id = stream_id;
    ct->auto_mode = (mode == MODE_AUTO);
    ct->auto_found = false;
    snprintf(ct->spec, sizeof(ct->spec), "%s", spec);
    return 1;
}

// Wait for lock (up to 5 s), then for L1 and the PLP selection (up to 3 s)
static int cx_wait_for_lock(struct tuner *t)
{
    struct cx_tuner *ct = C(t);
    enum fe_status fst = 0;
    int i;

    for (i = 0; i < 50; i++) {
        auto_step(ct);
        if (ioctl(ct->fe, FE_READ_STATUS, &fst) == 0 && (fst & FE_HAS_LOCK)) break;
        usleep(100000);
    }
    if (!(fst & FE_HAS_LOCK)) return 0;
    if (ct->auto_mode) ct->auto_found = true;
    for (i = 0; i < 30 && ct->delsys == SYS_ATSC3; i++) {
        memset(&ct->l1_at, 0, sizeof(ct->l1_at));
        refresh_l1(ct);
        if (ct->l1_b64 && ct->n_decoded > 0) break;
        usleep(100000);
    }
    memset(&ct->status_at, 0, sizeof(ct->status_at));
    return 1;
}

// Channels for the arrow keys and seek: the US plan
static int cx_get_channel_list(struct tuner *t, unsigned int *channels, int max)
{
    (void)t;
    int n = 0;
    for (unsigned ch = 2; ch <= 36 && n < max; ch++) channels[n++] = ch;
    return n;
}

/* ---- ALP pcap capture ------------------------------------------------ */

/*
 * The bridge passes data on in whole USB transfers (64 KB by default), so
 * a low-bitrate PLP decoded on its own could take minutes to arrive. Size
 * transfers to fill in about half a second at the decoded PLPs' bitrate
 * (from the shared parser's "PLP Bitrate" lines, see refresh_l1()).
 */
static void set_transfer_size(struct cx_tuner *ct, int fd)
{
    refresh_l1(ct);
    double bps = ct->decoded_bps;
    if (bps <= 0) return;

    uint32_t packets = (uint32_t)(bps / 8 * 0.5 / 188) + 1;
    if (packets >= 348) return;        // fast enough for the default
    ioctl(fd, ALP_RAW_IOC_SET_XFER, &packets);
}

static struct tuner_capture *cx_capture_start(struct tuner *t, enum tuner_capture_format fmt,
                                              unsigned rf_channel, const char *spec_suffix,
                                              int duration_s, char *debug_url, size_t debug_len,
                                              char *err, size_t errlen)
{
    struct cx_tuner *ct = C(t);
    (void)rf_channel; (void)spec_suffix;   // already tuned; the PLPs are what's decoding

    if (fmt == CAPTURE_DBG) {
        snprintf(err, errlen, "Debug capture is not available on this tuner.");
        return NULL;
    }
    if ((fmt == CAPTURE_TS) != (ct->delsys == SYS_ATSC)) {
        snprintf(err, errlen, fmt == CAPTURE_TS ? "TS capture needs an ATSC 1.0 channel."
                                                : "ALP capture needs an ATSC 3.0 channel.");
        return NULL;
    }
    struct cx_capture *c = calloc(1, sizeof(*c));
    if (!c) {
        snprintf(err, errlen, "Out of memory.");
        return NULL;
    }
    c->fmt = fmt;
    c->fd = c->dmx = c->dvr = -1;
    c->ct = ct;

    if (fmt == CAPTURE_TS) {
        // Whole transport stream: an all-PIDs (0x2000) filter on the demux,
        // output to the dvr device
        char path[64];
        snprintf(path, sizeof(path), "/dev/dvb/adapter%d/demux0", ct->adapter);
        c->dmx = open(path, O_RDWR | O_NONBLOCK);
        if (c->dmx < 0) {
            snprintf(err, errlen, "Error: can't open %s: %s", path, strerror(errno));
            free(c);
            return NULL;
        }
        ioctl(c->dmx, DMX_SET_BUFFER_SIZE, 8 * 1024 * 1024);
        struct dmx_pes_filter_params f;
        memset(&f, 0, sizeof(f));
        f.pid = 0x2000;
        f.input = DMX_IN_FRONTEND;
        f.output = DMX_OUT_TS_TAP;
        f.pes_type = DMX_PES_OTHER;
        f.flags = DMX_IMMEDIATE_START;
        if (ioctl(c->dmx, DMX_SET_PES_FILTER, &f) < 0) {
            snprintf(err, errlen, "Error: demux filter: %s", strerror(errno));
            close(c->dmx);
            free(c);
            return NULL;
        }
        snprintf(path, sizeof(path), "/dev/dvb/adapter%d/dvr0", ct->adapter);
        c->dvr = open(path, O_RDONLY | O_NONBLOCK);
        if (c->dvr < 0) {
            snprintf(err, errlen, "Error: can't open %s: %s", path, strerror(errno));
            close(c->dmx);
            free(c);
            return NULL;
        }
        memset(ct->cc, 0xFF, sizeof(ct->cc));
        if (debug_url && debug_len) snprintf(debug_url, debug_len, "%s (all PIDs)", path);
        clock_gettime(CLOCK_MONOTONIC, &c->start);
        return &c->base;
    }
    char path[64];
    snprintf(path, sizeof(path), "/dev/alpraw%d", ct->adapter);
    c->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (c->fd < 0) {
        snprintf(err, errlen, "Error: can't open %s: %s", path, strerror(errno));
        free(c);
        return NULL;
    }
    if (debug_url && debug_len) snprintf(debug_url, debug_len, "%s, %d s", path, duration_s);
    set_transfer_size(ct, c->fd);      // before the first read starts the stream
    c->duration_s = duration_s;
    clock_gettime(CLOCK_MONOTONIC, &c->start);
    return &c->base;
}

/*
 * TS error counters, as the HDHomeRun keeps them: transport errors (TEI
 * set), sequence errors (continuity counter jumps), network errors (here:
 * demux buffer overflows, i.e. data lost before it reached us).
 */
static void count_ts_packet(struct cx_tuner *ct, const uint8_t *pkt)
{
    if (pkt[1] & 0x80) {                         // transport_error_indicator
        ct->ts_te++;
        return;                                  // the rest can't be trusted
    }
    unsigned pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    if (pid == 0x1FFF) return;                   // null packets carry no counter
    unsigned afc = (pkt[3] >> 4) & 3, cc = pkt[3] & 0x0F;
    if (!(afc & 1)) return;                      // no payload: counter doesn't advance
    bool discontinuity = (afc & 2) && pkt[4] > 0 && (pkt[5] & 0x80);
    uint8_t last = ct->cc[pid];
    if (last != 0xFF && !discontinuity && cc != last && cc != ((last + 1) & 0x0F))
        ct->ts_se++;                             // (cc == last: an allowed duplicate)
    ct->cc[pid] = (uint8_t)cc;
}

static void count_ts(struct cx_capture *c, const uint8_t *buf, size_t n)
{
    size_t i = 0;
    if (c->carry_len) {
        size_t need = 188 - c->carry_len, take = n < need ? n : need;
        memcpy(c->carry + c->carry_len, buf, take);
        c->carry_len += take;
        i = take;
        if (c->carry_len < 188) return;
        if (c->carry[0] == 0x47) count_ts_packet(c->ct, c->carry);
        c->carry_len = 0;
    }
    while (i + 188 <= n) {
        if (buf[i] != 0x47) { i++; continue; }  // out of step: find the next sync byte
        count_ts_packet(c->ct, buf + i);
        i += 188;
    }
    if (i < n) {
        c->carry_len = n - i;
        memcpy(c->carry, buf + i, c->carry_len);
    }
}

static long cx_capture_read(struct tuner_capture *base, uint8_t *buf, size_t len)
{
    struct cx_capture *c = (struct cx_capture *)base;

    if (c->fmt == CAPTURE_TS) {
        ssize_t n = read(c->dvr, buf, len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return 0;
            if (errno == EOVERFLOW) {            // demux buffer overran: data lost
                c->ct->ts_ne++;
                return 0;
            }
            return -2;
        }
        count_ts(c, buf, (size_t)n);
        return (long)n;
    }

    if (c->out_pos == c->out_len) {
        c->out_pos = c->out_len = 0;
        if (!c->header_done) {
            // nanosecond pcap, LINKTYPE_ATSC_ALP, as alp-capture writes
            struct __attribute__((packed)) {
                uint32_t magic; uint16_t vmaj, vmin; int32_t zone; uint32_t sigfigs, snaplen, linktype;
            } gh = { 0xa1b23c4d, 2, 4, 0, 0, 262144, LINKTYPE_ATSC_ALP };
            memcpy(c->out, &gh, sizeof(gh));
            c->out_len = sizeof(gh);
            c->header_done = true;
        } else {
            if (ms_since(&c->start) >= c->duration_s * 1000L) return -1;   // done
            ssize_t n = read(c->fd, c->rec, sizeof(c->rec));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) return 0;
                return -2;             // EBUSY (another reader), ENODEV (unplugged), ...
            }
            if ((size_t)n < sizeof(struct alp_raw_hdr)) return 0;
            struct alp_raw_hdr h;
            memcpy(&h, c->rec, sizeof(h));
            if (h.len > (size_t)n - sizeof(h) || h.len > 65536) return 0;
            uint32_t rh[4] = { (uint32_t)(h.tstamp_ns / 1000000000ull),
                               (uint32_t)(h.tstamp_ns % 1000000000ull), h.len, h.len };
            memcpy(c->out, rh, sizeof(rh));
            memcpy(c->out + sizeof(rh), c->rec + sizeof(h), h.len);
            c->out_len = sizeof(rh) + h.len;
        }
    }
    size_t n = c->out_len - c->out_pos;
    if (n > len) n = len;
    memcpy(buf, c->out + c->out_pos, n);
    c->out_pos += n;
    return (long)n;
}

static void cx_capture_stop(struct tuner_capture *base)
{
    struct cx_capture *c = (struct cx_capture *)base;
    if (c->fmt == CAPTURE_TS) {
        close(c->dvr);
        close(c->dmx);
    } else {
        close(c->fd);                  // also drops the transfer-size request
    }
    free(c);
}

const struct tuner_backend tuner_backend_cxd28xx = {
    .name = "cxd28xx",
    .discover = cx_discover,
    .open = cx_open,
    .close = cx_close,
    .caps = cx_caps,
    .get_status = cx_get_status,
    .get_plpinfo = cx_get_plpinfo,
    .get_errors = cx_get_errors,
    .get_version = cx_get_version,
    .get_l1_string = cx_get_l1_string,
    .set_channel = cx_set_channel,
    .wait_for_lock = cx_wait_for_lock,
    .get_channel_list = cx_get_channel_list,
    .capture_start = cx_capture_start,
    .capture_read = cx_capture_read,
    .capture_stop = cx_capture_stop,
};
