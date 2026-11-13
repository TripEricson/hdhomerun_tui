/*
 * tuner.h
 *
 * Common tuner interface for the TUI. The screens talk only to this; each
 * kind of receiver is a backend (tuner_hdhr.c for HDHomeRun; a cxd28xx /
 * GTMedia backend can be added the same way).
 *
 * Interchange formats: where the TUI already worked with HDHomeRun text
 * (channel specs, plpinfo and streaminfo), those formats are kept as the
 * common format, and other backends produce / accept the same text:
 *
 *   channel spec   "auto:33", "auto:539000000", "atsc3:539000000:0+16"
 *   plpinfo        "bsid=..." line, then one line per PLP starting
 *                  "<id>: ... lock=0|1 mod=<mod> cod=<cod> ..."
 *   streaminfo     "tsid=..." line and program lines, as an HDHomeRun
 *                  reports them ("program=..." for ATSC 3.0,
 *                  "<num>: <vchannel> <name> [(encrypted)]" for ATSC 1.0)
 *
 * Strings returned by the backend (const char *) stay valid until the next
 * call on the same tuner, like libhdhomerun's.
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef TUNER_H
#define TUNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUNER_NO_VALUE (-999)          // the TUI's existing "not available" sentinel

enum tuner_kind {
    TUNER_KIND_HDHR = 0,
    TUNER_KIND_CXD28XX,                // USB ATSC 3.0 tuner on the cxd28xx driver (GTMedia)
};

// One selectable tuner in the list (replaces struct unified_tuner)
struct tuner_info {
    enum tuner_kind kind;
    char name[32];                     // list label, e.g. "1234ABCD-0"
    char title[128];                   // status pane title, e.g. "Tuner 1234ABCD-0 (192.168.1.5)"
    char addr[64];                     // device address (HDHomeRun: IP)
    uint32_t device_id;
    int tuner_index;
    int total_tuners_on_device;
    bool is_legacy;                    // slower polling
};

struct tuner_status {
    char channel[64];                  // channel spec as tuned, e.g. "atsc3:33:0+16"
    char lock_str[64];                 // "none", "8vsb", "atsc3", ...
    int signal_strength;               // percent
    int signal_to_noise_quality;       // percent
    int symbol_error_quality;          // percent
    long strength_dbm;                 // TUNER_NO_VALUE if not reported
    long snr_db;                       // TUNER_NO_VALUE if not reported
    long bps;                          // TUNER_NO_VALUE if not reported
    long pps;                          // TUNER_NO_VALUE if not reported
};

struct tuner_vstatus {
    char vchannel[32];
    char name[64];
};

// TS-level error counters (HDHomeRun /tunerN/debug te/ne/se)
struct tuner_errors {
    long transport, network, sequence; // TUNER_NO_VALUE if not reported
};

enum tuner_capture_format {
    CAPTURE_TS,                        // MPEG-TS (ATSC 1.0)
    CAPTURE_DBG,                       // HDHomeRun .dbg (ATSC 3.0)
    CAPTURE_ALP_PCAP,                  // ALP pcap (ATSC 3.0)
};

// Capability bits
#define TUNER_CAP_CHANNELMAP   (1u << 0)   // channel maps can be listed / changed
#define TUNER_CAP_VLC          (1u << 1)   // can stream a program to VLC
#define TUNER_CAP_ALP_PCAP     (1u << 2)   // ALP pcap capture
#define TUNER_CAP_DBG          (1u << 3)   // .dbg capture
#define TUNER_CAP_TS           (1u << 4)   // MPEG-TS capture
#define TUNER_CAP_PROGRAMS     (1u << 5)   // program list (streaminfo)

struct tuner;                          // an open tuner (defined below, for backends)
struct tuner_capture;                  // a running capture

// Results of adding a device by address
enum tuner_add_result {
    TUNER_ADD_OK = 0,                  // *added > 0
    TUNER_ADD_BAD_ADDRESS,
    TUNER_ADD_NO_RESPONSE,
    TUNER_ADD_DUPLICATE,
    TUNER_ADD_NO_TUNERS,
    TUNER_ADD_LIST_FULL,
};

/* ---- Discovery ------------------------------------------------------- */

// All tuners: local ones (USB) first, then network ones (HDHomeRun)
int tuner_discover(struct tuner_info list[], int max);
// Local tuners only (no network discovery)
int tuner_discover_local(struct tuner_info list[], int max);
enum tuner_add_result tuner_add_by_address(const char *addr, struct tuner_info list[],
                                           int *count, int max, int *added,
                                           uint32_t *device_id);

/* ---- Open / close ---------------------------------------------------- */

struct tuner *tuner_open(const struct tuner_info *info);
void tuner_close(struct tuner *t);
const struct tuner_info *tuner_get_info(const struct tuner *t);
unsigned tuner_caps(struct tuner *t);

/* ---- Status ---------------------------------------------------------- */

// 1 on success, <= 0 on failure (like libhdhomerun)
int tuner_get_status(struct tuner *t, struct tuner_status *st);
int tuner_get_vstatus(struct tuner *t, struct tuner_vstatus *vs);    // 1 if a vchannel is set
int tuner_get_plpinfo(struct tuner *t, const char **text);          // plpinfo format
int tuner_get_streaminfo(struct tuner *t, const char **text);       // streaminfo format
int tuner_get_target(struct tuner *t, const char **text);           // where the stream is sent
int tuner_get_errors(struct tuner *t, struct tuner_errors *err);
int tuner_get_version(struct tuner *t, const char **text);          // firmware / driver version

// L1 string (base64 L1-Basic + L1-Detail). 1 if available; 0 if this tuner
// or firmware can't report it.
int tuner_get_l1_string(struct tuner *t, const char **b64);

/* ---- Tuning ---------------------------------------------------------- */

int tuner_set_channel(struct tuner *t, const char *spec);           // channel spec format
int tuner_wait_for_lock(struct tuner *t);

/* ---- Channel maps (TUNER_CAP_CHANNELMAP) ----------------------------- */

// Channels of the current map, sorted; returns the count
int tuner_get_channel_list(struct tuner *t, unsigned int *channels, int max);
// Current map name; 1 on success
int tuner_get_channelmap(struct tuner *t, char *name, size_t len);
// Names of the maps this tuner supports; returns the count (caller frees each)
int tuner_get_channelmap_names(struct tuner *t, char **names, int max);
int tuner_set_channelmap(struct tuner *t, const char *name);

/* ---- Captures -------------------------------------------------------- */

// Start a capture of duration_s seconds (CAPTURE_DBG / CAPTURE_ALP_PCAP: the
// device ends it; CAPTURE_TS: until stopped). spec_suffix is the PLP part
// of the request ("p0p16"), "" for none. On failure returns NULL with a
// message in err. debug_url, if not NULL, receives a description of the
// request for the debug line.
struct tuner_capture *tuner_capture_start(struct tuner *t, enum tuner_capture_format fmt,
                                          unsigned rf_channel, const char *spec_suffix,
                                          int duration_s, char *debug_url, size_t debug_len,
                                          char *err, size_t errlen);
// Returns bytes read (> 0), 0 if nothing is ready now, -1 when the capture
// has ended normally, -2 on error.
long tuner_capture_read(struct tuner_capture *c, uint8_t *buf, size_t len);
void tuner_capture_stop(struct tuner_capture *c);

/* ---- Live viewing (TUNER_CAP_VLC) ------------------------------------ */

int tuner_set_program(struct tuner *t, const char *program);
// Send the selected program as RTP to this computer, UDP port 'port'.
// On failure returns < 0 with a message in err.
int tuner_set_target_rtp(struct tuner *t, int port, char *err, size_t errlen);
int tuner_clear_target(struct tuner *t);

/* ---- Backend interface ----------------------------------------------- */

struct tuner_backend {
    const char *name;
    int (*discover)(struct tuner_info list[], int max);
    enum tuner_add_result (*add_by_address)(const char *addr, struct tuner_info list[],
                                            int *count, int max, int *added,
                                            uint32_t *device_id);
    struct tuner *(*open)(const struct tuner_info *info);
    void (*close)(struct tuner *t);
    unsigned (*caps)(struct tuner *t);
    int (*get_status)(struct tuner *t, struct tuner_status *st);
    int (*get_vstatus)(struct tuner *t, struct tuner_vstatus *vs);
    int (*get_plpinfo)(struct tuner *t, const char **text);
    int (*get_streaminfo)(struct tuner *t, const char **text);
    int (*get_target)(struct tuner *t, const char **text);
    int (*get_errors)(struct tuner *t, struct tuner_errors *err);
    int (*get_version)(struct tuner *t, const char **text);
    int (*get_l1_string)(struct tuner *t, const char **b64);
    int (*set_channel)(struct tuner *t, const char *spec);
    int (*wait_for_lock)(struct tuner *t);
    int (*get_channel_list)(struct tuner *t, unsigned int *channels, int max);
    int (*get_channelmap)(struct tuner *t, char *name, size_t len);
    int (*get_channelmap_names)(struct tuner *t, char **names, int max);
    int (*set_channelmap)(struct tuner *t, const char *name);
    struct tuner_capture *(*capture_start)(struct tuner *t, enum tuner_capture_format fmt,
                                           unsigned rf_channel, const char *spec_suffix,
                                           int duration_s, char *debug_url, size_t debug_len,
                                           char *err, size_t errlen);
    long (*capture_read)(struct tuner_capture *c, uint8_t *buf, size_t len);
    void (*capture_stop)(struct tuner_capture *c);
    int (*set_program)(struct tuner *t, const char *program);
    int (*set_target_rtp)(struct tuner *t, int port, char *err, size_t errlen);
    int (*clear_target)(struct tuner *t);
};

// Backends embed these as the first member of their own structs (see
// tuner_hdhr.c), so tuner.c can dispatch. The TUI doesn't look inside.
struct tuner {
    const struct tuner_backend *backend;
    struct tuner_info info;
};
struct tuner_capture {
    const struct tuner_backend *backend;
};

extern const struct tuner_backend tuner_backend_hdhr;
extern const struct tuner_backend tuner_backend_cxd28xx;

#endif // TUNER_H
