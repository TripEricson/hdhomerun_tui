/*
 * l1_detail_parser.h
 *
 * ATSC 3.0 L1 Detail and PLP Information Parser
 * Abstracted from hdhomerun_tui.c for reuse in other applications
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
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

// Forward declarations to avoid duplicate includes
struct hdhomerun_device_t;

#define MAX_DISPLAY_LINES (64 * 20 + 400) // Increased buffer for bitrate info
#define MAX_PLPS 64

// Enums from ATSC 3.0 standard
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
    FECFRAME_NORMAL = 0,
    FECFRAME_SHORT,
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

// Structures for L1 parsing context
struct l1_parse_context {
    bool ldpc_info_available;
    int ldpc_length;  // 0=short (16200), 1=long (64800)
};

struct subframe_info_t {
    int num_preamble_symbols;
    int num_ofdm_symbols;
    int fft_size;
    int guard_interval;
    int reduced_carriers;
    int scattered_pilot_pattern;
    int scattered_pilot_boost;
    int sbs_first;
    int sbs_last;
};

struct plp_info_t {
    int plp_id;
    int fec_type;
    int mod;
    int cod;
    int ti_mode;
    int HTI_num_fec_blocks;
    long size;
    int subframe_index;
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

int collect_atsc3_details(struct hdhomerun_device_t *hd, int tuner_index, 
                         struct l1_detail_info* detail_info);

int save_atsc3_details_to_file(const char* filename, 
                              struct l1_detail_info* detail_info,
                              const char* l1_detail_base64);

int save_atsc3_details_auto(struct hdhomerun_device_t *hd, int tuner_index,
                           const char* base_filename);

// Helper functions
long parse_status_value_l1(const char *status_str, const char *key);
void normalize_mod_str_l1(const char *in, char *out, size_t out_size);
struct snr_pair_result get_snr_pair_for_modcod_l1(const char* mod, const char* cod, int ldpc_length);
double calculate_atsc3_bitrate_l1(int fft_size_enum, int guardinterval, int numpayloadsyms, 
                                 int numpreamblesyms, int rate, int constellation, int framesize, 
                                 int pilotpattern, int firstsbs, int cred, int pilotboost, 
                                 int paprmode, int ti_mode, int fec_blocks, int l1_detail_cells, 
                                 int subframe, int num_subframes, struct subframe_info_t *subframe_info, 
                                 int frame_length_mode, int frame_length, int excess_samples);

// Base64 decoding functions
size_t b64_decoded_size_l1(const char *in);
int b64_decode_l1(const char *in, unsigned char *out, size_t outlen);
int b64_isvalidchar_l1(char c);

// L1 parsing functions
void parse_l1_data_l1(const unsigned char* data, size_t len, char** display_lines, 
                     int* line_count, int max_lines, struct l1_parse_context* context);
void update_plp_snr_info_l1(char** display_lines, int line_count, int ldpc_length);

#endif // L1_DETAIL_PARSER_H
