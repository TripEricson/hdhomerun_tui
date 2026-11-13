/*
 * l1_detail_parser.h
 *
 * ATSC 3.0 L1 Detail and PLP Information Parser
 * Abstracted from hdhomerun_tui.c for reuse in other applications.
 * Device-independent: no libhdhomerun needed (see l1_details_hdhr.h for
 * the HDHomeRun collection functions).
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * PLP bitrate calculation follows atsc3rate() in l1dump.c from dtv-utils
 * (Copyright 2021-2025 Ron Economos): https://github.com/drmpeg/dtv-utils
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef L1_DETAIL_PARSER_H
#define L1_DETAIL_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_DISPLAY_LINES (64 * 20 + 400) // Increased buffer for bitrate info
#define MAX_PLPS 64                       // PLP IDs are 6 bits, so 64 per frame max
#define MAX_SUBFRAMES 257                 // L1B_num_subframes is 8 bits (+1)

// Enums from ATSC 3.0 standard (A/322), same values as dtv-utils l1dump.h
enum atsc3_fftsize_t {
    FFTSIZE_8K = 0,
    FFTSIZE_16K,
    FFTSIZE_32K,
};

enum atsc3_code_rate_t {
    C2_15 = 0, C3_15, C4_15, C5_15, C6_15, C7_15,
    C8_15, C9_15, C10_15, C11_15, C12_15, C13_15,
};

enum atsc3_framesize_t {
    FECFRAME_NORMAL = 0,   // 64K LDPC
    FECFRAME_SHORT,        // 16K LDPC
};

enum atsc3_constellation_t {
    MOD_QPSK = 0, MOD_16QAM, MOD_64QAM,
    MOD_256QAM, MOD_1024QAM, MOD_4096QAM,
};

enum atsc3_guardinterval_t {
    GI_RESERVED = 0, GI_1_192, GI_2_384, GI_3_512, GI_4_768,
    GI_5_1024, GI_6_1536, GI_7_2048, GI_8_2432, GI_9_3072,
    GI_10_3648, GI_11_4096, GI_12_4864,
};

enum atsc3_pilotpattern_t {
    PILOT_SP3_2 = 0, PILOT_SP3_4, PILOT_SP4_2, PILOT_SP4_4,
    PILOT_SP6_2, PILOT_SP6_4, PILOT_SP8_2, PILOT_SP8_4,
    PILOT_SP12_2, PILOT_SP12_4, PILOT_SP16_2, PILOT_SP16_4,
    PILOT_SP24_2, PILOT_SP24_4, PILOT_SP32_2, PILOT_SP32_4,
};

// Structures for L1 parsing context
struct l1_parse_context {
    bool ldpc_info_available;
    int ldpc_length;  // 0=short (16200), 1=long (64800)
};

// Per-subframe parameters. fft_size and guard_interval hold the enum values
// (FFTSIZE_*, GI_*), not sample counts.
struct subframe_info_t {
    int num_preamble_symbols;   // non-zero only for subframe 0
    int num_ofdm_symbols;
    int fft_size;
    int guard_interval;
    int reduced_carriers;
    int scattered_pilot_pattern;
    int scattered_pilot_boost;
    int sbs_first;
    int sbs_last;
    int num_plp;                // number of PLPs in this subframe (not minus 1)
};

struct plp_info_t {
    int plp_id;
    int layer;                  // 0 = core, 1 = enhanced (LDM)
    int fec_type;               // FECFRAME_NORMAL / FECFRAME_SHORT, -1 if unknown
    int mod;                    // MOD_*, -1 if not signaled
    int cod;                    // C*_15, -1 if not signaled
    int ti_mode;
    int HTI_num_fec_blocks;     // informational only; not used for bitrate
    long start;                 // L1D_plp_start (first cell index)
    long size;                  // L1D_plp_size (cells per subframe)
    double ldm_injection_db;    // enhanced layer only; -1 if not LDM / reserved
    int subframe_index;
    int display_line;           // index of this PLP's bitrate line, -1 if none
    int snr_line;               // index of this PLP's required-SNR line, -1 if none
};

// Extra results from the bitrate calculation
struct atsc3_rate_info {
    double plp_bps;             // PLP bitrate, Kbch/Nldpc
    double ts_bps;              // TS-over-ALP bitrate, (Kbch-16)/Nldpc, as upstream
    double frame_time_ms;
    int available_cells;        // whole-subframe data cells (upstream "Available PLP cells")
    long plp_cells;             // cells actually used for the calculation
};

// SNR lookup result structure
struct snr_pair_result {
    bool found;
    bool ldpc_length_known;
    float awgn_min, awgn_max;
    float rayleigh_min, rayleigh_max;
    char description[64];
};

// Structure to hold complete L1 detail information
struct l1_detail_info {
    char **display_lines;
    int line_count;
    int max_lines;
    struct l1_parse_context context;
};

// Function prototypes
struct l1_detail_info* create_l1_detail_info(int max_lines);
void free_l1_detail_info(struct l1_detail_info* info);

int save_atsc3_details_to_file(const char* filename,
                              struct l1_detail_info* detail_info,
                              const char* l1_detail_base64);

// Decode an L1 string (base64) and append its full parse to detail_info.
// Returns 0, or -1 if it can't be decoded. Device-independent.
int append_l1_detail_from_base64(struct l1_detail_info *detail_info, const char *l1_base64);

// HDHomeRun collection (collect_atsc3_details, save_atsc3_details_auto)
// is in l1_details_hdhr.h.

// Helper functions
long parse_status_value_l1(const char *status_str, const char *key);
void normalize_mod_str_l1(const char *in, char *out, size_t out_size);
struct snr_pair_result get_snr_pair_for_modcod_l1(const char* mod, const char* cod, int ldpc_length);

/*
 * LDM (layered division multiplexing) required SNR, in dB of total received
 * power, using the ideal successive-cancellation model:
 *   g = 10^(-injection_db/10)   (enhanced power relative to core)
 *   core:     needs SINR >= T_core with the enhanced layer as noise
 *             -> SNR_req = (1+g) / (1/T_core - g)
 *   enhanced: core decoded and cancelled first, then SNR * g/(1+g) >= T_enh
 *             -> SNR_req = max(core requirement, T_enh * (1+g)/g)
 * Thresholds are the non-LDM modcod values from the SNR table.
 *
 * ldm_core_required_snr_db() returns NAN when the core cannot be decoded at
 * any SNR (its threshold is above the core-to-enhanced power ratio).
 * For ldm_enhanced_required_snr_db(), pass core_required_db = -INFINITY when
 * no core PLP was found; NAN propagates.
 */
double ldm_core_required_snr_db(double core_threshold_db, double injection_db);
double ldm_enhanced_required_snr_db(double enh_threshold_db, double core_required_db,
                                    double injection_db);

/*
 * PLP bitrate in bits/s, following atsc3rate() in dtv-utils l1dump.c.
 *
 * plp_size_cells is L1D_plp_size, which is signaled for every PLP (core and
 * enhanced, any TI mode), so this works for LDM and multi-PLP subframes.
 * If plp_size_cells <= 0 it falls back to the whole-subframe data cell count
 * (only meaningful for a single-PLP subframe).
 *
 * subframe_info must hold num_subframes entries. info_out may be NULL.
 * Returns 0.0 for invalid / unsupported parameters.
 */
double calculate_atsc3_bitrate_l1(int fft_size_enum, int guardinterval, int numpayloadsyms,
                                 int numpreamblesyms, int rate, int constellation, int framesize,
                                 int pilotpattern, int firstsbs, int cred, int pilotboost,
                                 int paprmode, int l1_detail_cells,
                                 int num_subframes, const struct subframe_info_t *subframe_info,
                                 int frame_length_mode, int frame_length,
                                 long plp_size_cells, struct atsc3_rate_info *info_out);

// Base64 decoding functions
size_t b64_decoded_size_l1(const char *in);
int b64_decode_l1(const char *in, unsigned char *out, size_t outlen);
int b64_isvalidchar_l1(char c);

// L1 parsing functions
void parse_l1_data_l1(const unsigned char* data, size_t len, char** display_lines,
                     int* line_count, int max_lines, struct l1_parse_context* context);
void update_plp_snr_info_l1(char** display_lines, int line_count, int ldpc_length);

#endif // L1_DETAIL_PARSER_H
