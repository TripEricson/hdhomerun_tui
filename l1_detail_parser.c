/*
 * l1_detail_parser.c
 *
 * ATSC 3.0 L1 Detail and PLP Information Parser Implementation
 * Abstracted from hdhomerun_tui.c for reuse in other applications
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * The L1 bitstream walk and the PLP bitrate model follow l1dump.c from
 * dtv-utils (Copyright 2021-2025 Ron Economos): https://github.com/drmpeg/dtv-utils
 * Cell-count tables are in atsc3_tables.h, copied verbatim from l1dump.h.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "l1_detail_parser.h"
#include "atsc3_tables.h"

/* ------------------------------------------------------------------------ */
/* SNR lookup table                                                          */
/* ------------------------------------------------------------------------ */

static const struct modcod_snr_complete {
    char mod[16];
    char cod[8];
    float awgn_long;
    float rayleigh_long;
    float awgn_short;
    float rayleigh_short;
} snr_table_complete[] = {
    // QPSK
    {"QPSK", "2/15", -6.23, -5.72, -5.55, -5.06}, {"QPSK", "3/15", -4.32, -3.62, -3.73, -2.97},
    {"QPSK", "4/15", -2.89, -1.97, -2.32, -1.36}, {"QPSK", "5/15", -1.70, -0.55, -1.30, -0.08},
    {"QPSK", "6/15", -0.54, 0.86, -0.33, 1.15}, {"QPSK", "7/15", 0.30, 1.95, 0.56, 2.30},
    {"QPSK", "8/15", 1.16, 3.16, 1.38, 3.44}, {"QPSK", "9/15", 1.97, 4.35, 2.20, 4.70},
    {"QPSK", "10/15", 2.77, 5.62, 2.94, 5.97}, {"QPSK", "11/15", 3.60, 7.05, 3.82, 7.46},
    {"QPSK", "12/15", 4.49, 8.76, 4.70, 9.15}, {"QPSK", "13/15", 5.53, 10.97, 5.76, 11.56},

    // 16QAM
    {"16QAM", "2/15", -2.73, -1.84, -2.15, -1.14}, {"16QAM", "3/15", -0.25, 0.81, 0.35, 1.45},
    {"16QAM", "4/15", 1.46, 2.69, 1.99, 3.41}, {"16QAM", "5/15", 2.82, 4.32, 3.16, 4.78},
    {"16QAM", "6/15", 4.21, 5.98, 4.45, 6.27}, {"16QAM", "7/15", 5.21, 7.21, 5.51, 7.58},
    {"16QAM", "8/15", 6.30, 8.63, 6.51, 8.96}, {"16QAM", "9/15", 7.32, 9.94, 7.58, 10.28},
    {"16QAM", "10/15", 8.36, 11.40, 8.59, 11.73}, {"16QAM", "11/15", 9.50, 12.78, 9.74, 13.22},
    {"16QAM", "12/15", 10.57, 14.60, 10.81, 14.97}, {"16QAM", "13/15", 11.83, 16.85, 12.09, 17.44},

    // 64QAM
    {"64QAM", "2/15", -0.26, 0.86, 0.35, 1.60}, {"64QAM", "3/15", 2.27, 3.61, 2.85, 4.30},
    {"64QAM", "4/15", 4.15, 5.88, 4.65, 6.55}, {"64QAM", "5/15", 5.96, 7.74, 6.30, 8.29},
    {"64QAM", "6/15", 7.66, 9.72, 7.93, 10.05}, {"64QAM", "7/15", 8.92, 11.10, 9.29, 11.54},
    {"64QAM", "8/15", 10.31, 12.75, 10.56, 13.09}, {"64QAM", "9/15", 11.55, 14.25, 11.83, 14.62},
    {"64QAM", "10/15", 12.88, 15.81, 13.13, 16.20}, {"64QAM", "11/15", 14.28, 17.44, 14.52, 17.87},
    {"64QAM", "12/15", 15.57, 19.39, 15.86, 19.82}, {"64QAM", "13/15", 17.03, 21.82, 17.33, 22.44},

    // 256QAM
    {"256QAM", "2/15", 1.60, 2.89, 2.27, 3.60}, {"256QAM", "3/15", 4.30, 5.97, 4.78, 6.79},
    {"256QAM", "4/15", 6.57, 8.46, 7.19, 9.32}, {"256QAM", "5/15", 8.53, 10.59, 8.93, 11.16},
    {"256QAM", "6/15", 10.61, 12.92, 10.91, 13.29}, {"256QAM", "7/15", 12.10, 14.58, 12.57, 15.15},
    {"256QAM", "8/15", 13.91, 16.54, 14.25, 16.95}, {"256QAM", "9/15", 15.55, 18.23, 15.80, 18.64},
    {"256QAM", "10/15", 17.13, 20.06, 17.45, 20.50}, {"256QAM", "11/15", 18.76, 21.94, 19.08, 22.40},
    {"256QAM", "12/15", 20.44, 24.01, 20.78, 24.54}, {"256QAM", "13/15", 22.22, 26.62, 22.55, 27.23},

    // 1024QAM
    {"1024QAM", "2/15", 3.23, 4.65, 0.0, 0.0}, {"1024QAM", "3/15", 6.17, 8.04, 0.0, 0.0},
    {"1024QAM", "4/15", 8.77, 10.85, 0.0, 0.0}, {"1024QAM", "5/15", 11.07, 13.25, 0.0, 0.0},
    {"1024QAM", "6/15", 13.46, 15.91, 0.0, 0.0}, {"1024QAM", "7/15", 15.30, 17.84, 0.0, 0.0},
    {"1024QAM", "8/15", 17.46, 20.13, 0.0, 0.0}, {"1024QAM", "9/15", 19.45, 22.34, 0.0, 0.0},
    {"1024QAM", "10/15", 21.35, 24.47, 0.0, 0.0}, {"1024QAM", "11/15", 23.43, 26.61, 0.0, 0.0},
    {"1024QAM", "12/15", 25.52, 28.82, 0.0, 0.0}, {"1024QAM", "13/15", 27.62, 31.59, 0.0, 0.0},

    // 4096QAM
    {"4096QAM", "2/15", 4.58, 6.23, 0.0, 0.0}, {"4096QAM", "3/15", 7.85, 9.83, 0.0, 0.0},
    {"4096QAM", "4/15", 10.73, 12.95, 0.0, 0.0}, {"4096QAM", "5/15", 13.45, 15.75, 0.0, 0.0},
    {"4096QAM", "6/15", 16.04, 18.79, 0.0, 0.0}, {"4096QAM", "7/15", 18.22, 21.03, 0.0, 0.0},
    {"4096QAM", "8/15", 20.69, 23.67, 0.0, 0.0}, {"4096QAM", "9/15", 23.05, 26.37, 0.0, 0.0},
    {"4096QAM", "10/15", 25.55, 28.64, 0.0, 0.0}, {"4096QAM", "11/15", 28.11, 31.18, 0.0, 0.0},
    {"4096QAM", "12/15", 30.34, 33.82, 0.0, 0.0}, {"4096QAM", "13/15", 32.83, 36.54, 0.0, 0.0},

    {{0}} // Sentinel
};

/* ------------------------------------------------------------------------ */
/* Bitrate model (follows atsc3rate() in dtv-utils l1dump.c)                 */
/* ------------------------------------------------------------------------ */

static int fft_samples(int fft_enum)
{
    switch (fft_enum) {
        case FFTSIZE_8K:  return 8192;
        case FFTSIZE_16K: return 16384;
        case FFTSIZE_32K: return 32768;
        default:          return 8192;   // upstream default
    }
}

static int gi_samples(int gi_enum)
{
    static const int samples[] = { 0, 192, 384, 512, 768, 1024, 1536,
                                   2048, 2432, 3072, 3648, 4096, 4864 };
    if (gi_enum < 0 || gi_enum > GI_12_4864) return 0;
    return samples[gi_enum];
}

/* Row in preamble_cells_table, same mapping as upstream's switch statements. */
static int atsc3_preamble_row(int fft_enum, int gi, int pilotpattern)
{
    switch (fft_enum) {
        case FFTSIZE_16K:
            if (gi >= GI_1_192 && gi <= GI_11_4096) return 7 + (gi - GI_1_192);   // rows 7-17
            return 7;
        case FFTSIZE_32K:
            if (gi >= GI_1_192 && gi <= GI_8_2432) return 18 + (gi - GI_1_192);   // rows 18-25
            if (gi == GI_9_3072)
                return (pilotpattern == PILOT_SP8_2 || pilotpattern == PILOT_SP8_4) ? 26 : 27;
            if (gi == GI_10_3648)
                return (pilotpattern == PILOT_SP8_2 || pilotpattern == PILOT_SP8_4) ? 28 : 29;
            if (gi == GI_11_4096) return 30;
            if (gi == GI_12_4864) return 31;
            return 18;
        case FFTSIZE_8K:
        default:
            if (gi >= GI_1_192 && gi <= GI_7_2048) return gi - GI_1_192;          // rows 0-6
            return 0;
    }
}

/* Frame duration in ms, as computed by upstream. */
static double atsc3_frame_time_ms(int num_subframes, const struct subframe_info_t *sf,
                                  int frame_length_mode, int frame_length)
{
    const double T  = 1.0 / (384000.0 * 18.0);   // 6.912 Msps baseband
    const double TB = 1.0 / 6144000.0;           // bootstrap sample rate
    double TF = 0.0;

    if (frame_length_mode == 0) {
        // Time-aligned: L1B_frame_length is in units of 5 ms
        return frame_length * 5.0;
    }

    // Symbol-aligned: bootstrap + preamble + all subframe symbols
    for (int n = 0; n < num_subframes; n++) {
        double TS = T * (fft_samples(sf[n].fft_size) + gi_samples(sf[n].guard_interval)) * 1000.0;
        if (n == 0) {
            TF += (sf[n].num_ofdm_symbols + sf[n].num_preamble_symbols) * TS;
            TF += 3072.0 * 4 * TB * 1000.0;
        } else {
            TF += sf[n].num_ofdm_symbols * TS;
        }
    }
    return TF;
}

double calculate_atsc3_bitrate_l1(int fft_size_enum, int guardinterval, int numpayloadsyms,
                                 int numpreamblesyms, int rate, int constellation, int framesize,
                                 int pilotpattern, int firstsbs, int cred, int pilotboost,
                                 int paprmode, int l1_detail_cells,
                                 int num_subframes, const struct subframe_info_t *subframe_info,
                                 int frame_length_mode, int frame_length,
                                 long plp_size_cells, struct atsc3_rate_info *info_out)
{
    static const int kbch_long[12]  = { 8448, 12768, 17088, 21408, 25728, 30048,
                                        34368, 38688, 43008, 47328, 51648, 55968 };
    static const int kbch_short[12] = { 1992, 3072, 4152, 5232, 6312, 7392,
                                        8472, 9552, 10632, 11712, 12792, 13872 };
    int mod, row;
    int l1cells, totalcells, total_preamble_cells, availablecells;
    int first_preamble_cells, preamble_cells, data_cells, sbs_cells, sbs_data_cells;
    int sbsnullcells, papr_cells;
    long plpsize;
    double kbch, fecsize, TF;

    if (info_out) memset(info_out, 0, sizeof(*info_out));

    if (rate < C2_15 || rate > C13_15) return 0.0;
    if (framesize == FECFRAME_NORMAL) {
        fecsize = 64800.0;
        kbch = kbch_long[rate];
    } else if (framesize == FECFRAME_SHORT) {
        fecsize = 16200.0;
        kbch = kbch_short[rate];
    } else {
        return 0.0;
    }

    switch (constellation) {
        case MOD_QPSK:    mod = 2;  break;
        case MOD_16QAM:   mod = 4;  break;
        case MOD_64QAM:   mod = 6;  break;
        case MOD_256QAM:  mod = 8;  break;
        case MOD_1024QAM: mod = 10; break;
        case MOD_4096QAM: mod = 12; break;
        default: return 0.0;
    }

    // Upstream indexes the tables unchecked; clamp so bad signaling can't read out of bounds.
    if (pilotpattern < 0 || pilotpattern > PILOT_SP32_4) pilotpattern = PILOT_SP3_2;
    if (cred < 0 || cred > 4) cred = 0;
    if (pilotboost < 0 || pilotboost > 4) pilotboost = 0;

    row = atsc3_preamble_row(fft_size_enum, guardinterval, pilotpattern);
    first_preamble_cells = preamble_cells_table[row][4];
    preamble_cells = preamble_cells_table[row][cred];

    switch (fft_size_enum) {
        case FFTSIZE_16K:
            papr_cells = 144;
            data_cells = data_cells_table_16K[pilotpattern][cred];
            sbs_cells = sbs_cells_table_16K[pilotpattern][cred];
            sbs_data_cells = sbs_data_cells_table_16K[pilotpattern][cred][pilotboost];
            break;
        case FFTSIZE_32K:
            papr_cells = 288;
            data_cells = data_cells_table_32K[pilotpattern][cred];
            sbs_cells = sbs_cells_table_32K[pilotpattern][cred];
            sbs_data_cells = sbs_data_cells_table_32K[pilotpattern][cred][pilotboost];
            break;
        case FFTSIZE_8K:
        default:
            papr_cells = 72;
            data_cells = data_cells_table_8K[pilotpattern][cred];
            sbs_cells = sbs_cells_table_8K[pilotpattern][cred];
            sbs_data_cells = sbs_data_cells_table_8K[pilotpattern][cred][pilotboost];
            break;
    }

    // L1-Basic cells: its FEC mode is signaled in the bootstrap, not in L1,
    // so upstream assumes Mode 1 (3820 cells). Only affects available_cells.
    l1cells = 3820 + l1_detail_cells;

    if (paprmode != 1) {
        papr_cells = 0;
    }

    TF = atsc3_frame_time_ms(num_subframes, subframe_info, frame_length_mode, frame_length);
    if (TF <= 0.0) return 0.0;

    // Whole-subframe data cells (upstream prints this as "Available PLP cells")
    total_preamble_cells = 0;
    for (int n = 1; n < numpreamblesyms; n++) {
        total_preamble_cells += preamble_cells - papr_cells;
    }
    if (numpreamblesyms == 0) {
        first_preamble_cells = 0;
        l1cells = 0;
    }
    if (firstsbs) {
        totalcells = first_preamble_cells + total_preamble_cells
                   + ((numpayloadsyms - 2) * (data_cells - papr_cells))
                   + ((sbs_cells - papr_cells) * 2);
    } else {
        totalcells = first_preamble_cells + total_preamble_cells
                   + ((numpayloadsyms - 1) * (data_cells - papr_cells))
                   + (sbs_cells - papr_cells);
    }
    sbsnullcells = sbs_cells - sbs_data_cells;
    availablecells = firstsbs ? totalcells - l1cells - (sbsnullcells * 2)
                              : totalcells - l1cells - sbsnullcells;

    // Upstream uses the signaled L1D_plp_size. Fall back to the whole
    // subframe only if no size was signaled.
    plpsize = (plp_size_cells > 0) ? plp_size_cells : availablecells;
    if (plpsize <= 0) return 0.0;

    double plp_bps = (1000.0 / TF) * ((double)plpsize * mod * (kbch / fecsize));
    double ts_bps  = (1000.0 / TF) * ((double)plpsize * mod * ((kbch - 16) / fecsize));

    if (info_out) {
        info_out->plp_bps = plp_bps;
        info_out->ts_bps = ts_bps;
        info_out->frame_time_ms = TF;
        info_out->available_cells = availablecells;
        info_out->plp_cells = plpsize;
    }
    return plp_bps;
}

/* ------------------------------------------------------------------------ */
/* Base64                                                                    */
/* ------------------------------------------------------------------------ */

static const int b64invs[] = { 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
    29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51 };

size_t b64_decoded_size_l1(const char *in) {
    size_t len, ret, i;

    if (in == NULL) return 0;

    len = strlen(in);
    ret = len / 4 * 3;

    for (i = len; i-- > 0;) {
        if (in[i] == '=') {
            ret--;
        } else {
            break;
        }
    }
    return ret;
}

int b64_isvalidchar_l1(char c) {
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c == '+' || c == '/' || c == '=') return 1;
    return 0;
}

int b64_decode_l1(const char *in, unsigned char *out, size_t outlen) {
    size_t len, i, j;
    int v;

    if (in == NULL || out == NULL) return 0;

    len = strlen(in);
    if (outlen < b64_decoded_size_l1(in) || len % 4 != 0) return 0;

    for (i = 0; i < len; i++) {
        if (!b64_isvalidchar_l1(in[i])) {
            return 0;
        }
    }

    for (i = 0, j = 0; i < len; i += 4, j += 3) {
        v = b64invs[in[i] - 43];
        v = (v << 6) | b64invs[in[i + 1] - 43];
        v = in[i + 2] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 2] - 43];
        v = in[i + 3] == '=' ? v << 6 : (v << 6) | b64invs[in[i + 3] - 43];

        out[j] = (v >> 16) & 0xFF;
        if (in[i + 2] != '=') out[j + 1] = (v >> 8) & 0xFF;
        if (in[i + 3] != '=') out[j + 2] = v & 0xFF;
    }

    return 1;
}

/* ------------------------------------------------------------------------ */
/* Misc helpers                                                              */
/* ------------------------------------------------------------------------ */

struct l1_detail_info* create_l1_detail_info(int max_lines) {
    struct l1_detail_info* info = malloc(sizeof(struct l1_detail_info));
    if (!info) return NULL;

    info->display_lines = malloc(max_lines * sizeof(char*));
    if (!info->display_lines) {
        free(info);
        return NULL;
    }

    info->line_count = 0;
    info->max_lines = max_lines;
    info->context.ldpc_info_available = false;
    info->context.ldpc_length = -1;

    return info;
}

void free_l1_detail_info(struct l1_detail_info* info) {
    if (!info) return;

    for (int i = 0; i < info->line_count; i++) {
        free(info->display_lines[i]);
    }
    free(info->display_lines);
    free(info);
}

long parse_status_value_l1(const char *status_str, const char *key) {
    const char *found = strstr(status_str, key);
    if (found) {
        return strtol(found + strlen(key), NULL, 0);
    }
    return -999;
}

void normalize_mod_str_l1(const char *in, char *out, size_t out_size) {
    char digits[8] = {0};
    char alphas[8] = {0};
    int d_idx = 0;
    int a_idx = 0;

    for (int i = 0; in[i] != '\0' && i < 15; i++) {
        if (isdigit((unsigned char)in[i])) {
            if (d_idx < 7) digits[d_idx++] = in[i];
        } else {
            if (a_idx < 7) alphas[a_idx++] = toupper((unsigned char)in[i]);
        }
    }

    if (d_idx > 0) {
        snprintf(out, out_size, "%s%s", digits, alphas);
    } else {
        snprintf(out, out_size, "%s", alphas);
    }
}

struct snr_pair_result get_snr_pair_for_modcod_l1(const char* mod, const char* cod, int ldpc_length) {
    struct snr_pair_result result = {false, false, 0.0, 0.0, 0.0, 0.0, ""};

    const struct modcod_snr_complete* entry = NULL;
    for (int i = 0; snr_table_complete[i].mod[0] != 0; i++) {
        if (strcmp(snr_table_complete[i].mod, mod) == 0 && strcmp(snr_table_complete[i].cod, cod) == 0) {
            entry = &snr_table_complete[i];
            break;
        }
    }

    if (!entry) return result;

    result.found = true;

    if (ldpc_length == 0) {
        if (entry->awgn_short != 0.0 && entry->rayleigh_short != 0.0) {
            result.ldpc_length_known = true;
            result.awgn_min = result.awgn_max = entry->awgn_short;
            result.rayleigh_min = result.rayleigh_max = entry->rayleigh_short;
            strcpy(result.description, "Short LDPC (16200)");
        } else {
            result.ldpc_length_known = true;
            result.awgn_min = result.awgn_max = entry->awgn_long;
            result.rayleigh_min = result.rayleigh_max = entry->rayleigh_long;
            strcpy(result.description, "Long LDPC (64800) - Short unavailable");
        }
    } else if (ldpc_length == 1) {
        result.ldpc_length_known = true;
        result.awgn_min = result.awgn_max = entry->awgn_long;
        result.rayleigh_min = result.rayleigh_max = entry->rayleigh_long;
        strcpy(result.description, "Long LDPC (64800)");
    } else {
        result.ldpc_length_known = false;

        result.awgn_min = entry->awgn_long;
        result.awgn_max = entry->awgn_long;
        if (entry->awgn_short != 0.0) {
            if (entry->awgn_short < result.awgn_min) result.awgn_min = entry->awgn_short;
            if (entry->awgn_short > result.awgn_max) result.awgn_max = entry->awgn_short;
        }

        result.rayleigh_min = entry->rayleigh_long;
        result.rayleigh_max = entry->rayleigh_long;
        if (entry->rayleigh_short != 0.0) {
            if (entry->rayleigh_short < result.rayleigh_min) result.rayleigh_min = entry->rayleigh_short;
            if (entry->rayleigh_short > result.rayleigh_max) result.rayleigh_max = entry->rayleigh_short;
        }

        strcpy(result.description, "LDPC length unknown");
    }

    return result;
}

/* ------------------------------------------------------------------------ */
/* LDM required SNR                                                          */
/* ------------------------------------------------------------------------ */

double ldm_core_required_snr_db(double core_threshold_db, double injection_db)
{
    double g = pow(10.0, -injection_db / 10.0);        // enhanced / core power
    double inv_t = pow(10.0, -core_threshold_db / 10.0);
    if (inv_t <= g) return NAN;                         // enhanced layer too strong for this core modcod
    return 10.0 * log10((1.0 + g) / (inv_t - g));
}

double ldm_enhanced_required_snr_db(double enh_threshold_db, double core_required_db,
                                    double injection_db)
{
    if (isnan(core_required_db)) return NAN;            // core can't be cancelled
    double g = pow(10.0, -injection_db / 10.0);
    double e = enh_threshold_db + 10.0 * log10((1.0 + g) / g);
    return (core_required_db > e) ? core_required_db : e;
}

/* ------------------------------------------------------------------------ */
/* L1 bitstream parser                                                       */
/* ------------------------------------------------------------------------ */

/* Reads straight from the decoded bytes; no fixed-size bit buffer, re-entrant. */
struct l1_bitreader {
    const unsigned char *data;
    size_t nbits;
    size_t pos;
};

static unsigned long br_get(struct l1_bitreader *br, int count)
{
    unsigned long v = 0;
    for (int i = 0; i < count; i++) {
        v <<= 1;
        if (br->pos < br->nbits) {
            v |= (br->data[br->pos >> 3] >> (7 - (br->pos & 7))) & 1;
        }
        br->pos++;   // past the end reads as zeros, like upstream
    }
    return v;
}

/* Appends a formatted line; returns its index, or -1 if the buffer is full. */
static int add_linef(struct l1_detail_info *info, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (info->line_count >= info->max_lines) return -1;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char *copy = strdup(buf);
    if (!copy) return -1;
    info->display_lines[info->line_count] = copy;
    return info->line_count++;
}

static const char *fft_name(unsigned long v)
{
    switch (v) {
        case FFTSIZE_8K:  return "8K";
        case FFTSIZE_16K: return "16K";
        case FFTSIZE_32K: return "32K";
        default:          return "Reserved";
    }
}

static void add_gi_line(struct l1_detail_info *info, const char *label, unsigned long v)
{
    static const char *names[] = { NULL, "GI_1_192", "GI_2_384", "GI_3_512", "GI_4_768",
        "GI_5_1024", "GI_6_1536", "GI_7_2048", "GI_8_2432", "GI_9_3072",
        "GI_10_3648", "GI_11_4096", "GI_12_4864" };
    if (v >= GI_1_192 && v <= GI_12_4864) {
        add_linef(info, "%s: %s", label, names[v]);
    } else {
        add_linef(info, "%s: Reserved (%lu)", label, v);
    }
}

static const char *mod_name(unsigned long v)
{
    static const char *names[] = { "QPSK", "16QAM", "64QAM", "256QAM", "1024QAM", "4096QAM" };
    return (v <= MOD_4096QAM) ? names[v] : "Reserved";
}

/* A/322 Table 9.x: 0..10 -> 0.0..5.0 dB in 0.5 dB steps, 11..30 -> 6..25 dB.
 * (Upstream l1dump prints 0.05 dB for code 1, which is a typo for 0.5 dB.) */
static double add_ldm_line(struct l1_detail_info *info, unsigned long v)
{
    double db = -1.0;
    if (v <= 10) {
        db = v * 0.5;
    } else if (v <= 30) {
        db = (double)v - 5.0;
    }
    if (db >= 0.0) {
        add_linef(info, "      L1D_plp_ldm_injection_level: %.1f dB", db);
    } else {
        add_linef(info, "      L1D_plp_ldm_injection_level: Reserved (%lu)", v);
    }
    return db;
}

/* Non-LDM threshold for a PLP's own modcod and LDPC length. Returns 0 if unknown. */
static int plp_threshold_db(const struct plp_info_t *p, double *awgn, double *rayleigh)
{
    char cod[16];
    struct snr_pair_result r;

    if (p->mod < MOD_QPSK || p->mod > MOD_4096QAM) return 0;
    if (p->cod < C2_15 || p->cod > C13_15) return 0;
    if (p->fec_type < 0) return 0;
    snprintf(cod, sizeof(cod), "%d/15", p->cod + 2);
    // get_snr_pair_for_modcod_l1: 0 = short (16K), 1 = long (64K)
    r = get_snr_pair_for_modcod_l1(mod_name(p->mod), cod, (p->fec_type == FECFRAME_SHORT) ? 0 : 1);
    if (!r.found) return 0;
    *awgn = r.awgn_min;
    *rayleigh = r.rayleigh_min;
    return 1;
}

static int plps_overlap(const struct plp_info_t *a, const struct plp_info_t *b)
{
    return a->subframe_index == b->subframe_index &&
           a->start < b->start + b->size && b->start < a->start + a->size;
}

static void fmt_snr_value(char *out, size_t n, double db)
{
    if (isnan(db)) snprintf(out, n, "not decodable");
    else snprintf(out, n, "%.2f dB", db);
}

void parse_l1_data_l1(const unsigned char* data, size_t len, char** display_lines,
                     int* line_count, int max_lines, struct l1_parse_context* context)
{
    struct l1_bitreader br = { data, len * 8, 0 };
    struct l1_detail_info out = { display_lines, *line_count, max_lines, { false, -1 } };
    struct l1_detail_info *info = &out;
    unsigned long value;
    int i, j, k;

    // L1B parameters
    int l1b_version, l1b_time_info_flag, l1b_papr_reduction, l1b_frame_length_mode;
    int l1b_frame_length = 0;
    int l1b_num_subframes, l1b_l1_detail_size_bytes, l1b_l1_detail_total_cells;
    int l1b_first_sub_mimo, l1b_first_sub_mimo_mixed = 0;

    // L1D parameters
    int l1d_version, l1d_num_rf, l1d_mimo = 0;
    int l1d_plp_layer, l1d_plp_mod, l1d_plp_TI_mode;
    int l1d_plp_num_channel_bonded, l1d_plp_HTI_inter_subframe, l1d_plp_HTI_num_ti_blocks;
    int l1d_mimo_mixed = 0;

    static struct subframe_info_t subframe_info[MAX_SUBFRAMES];   // ~10 KB, keep off the stack
    struct plp_info_t plp_info[MAX_PLPS];
    int plp_count = 0;

    memset(subframe_info, 0, sizeof(subframe_info));
    for (i = 0; i < MAX_PLPS; i++) {
        plp_info[i].plp_id = -1;
        plp_info[i].layer = 0;
        plp_info[i].fec_type = -1;
        plp_info[i].mod = -1;
        plp_info[i].cod = -1;
        plp_info[i].ti_mode = 0;
        plp_info[i].HTI_num_fec_blocks = 0;
        plp_info[i].start = 0;
        plp_info[i].size = 0;
        plp_info[i].ldm_injection_db = -1.0;
        plp_info[i].subframe_index = -1;
        plp_info[i].display_line = -1;
        plp_info[i].snr_line = -1;
    }

    /* ---------------- L1-Basic (200 bits) ---------------- */
    add_linef(info, "--- L1-Basic Signaling ---");

    value = br_get(&br, 3); add_linef(info, "L1B_version: %lu", value); l1b_version = value;
    value = br_get(&br, 1); add_linef(info, "L1B_mimo_scattered_pilot_encoding: %s", value == 0 ? "Walsh-Hadamard" : "Null pilots");
    value = br_get(&br, 1); add_linef(info, "L1B_lls_flag: %s", value == 0 ? "No LLS" : "LLS present");
    value = br_get(&br, 2); l1b_time_info_flag = value;
    switch (value) {
        case 0: add_linef(info, "L1B_time_info_flag: Not included"); break;
        case 1: add_linef(info, "L1B_time_info_flag: ms precision"); break;
        case 2: add_linef(info, "L1B_time_info_flag: us precision"); break;
        default: add_linef(info, "L1B_time_info_flag: ns precision"); break;
    }
    value = br_get(&br, 1); add_linef(info, "L1B_return_channel_flag: %lu", value);
    value = br_get(&br, 2); l1b_papr_reduction = value & 1;   // tone reservation present
    switch (value) {
        case 0: add_linef(info, "L1B_papr_reduction: None"); break;
        case 1: add_linef(info, "L1B_papr_reduction: Tone reservation only"); break;
        case 2: add_linef(info, "L1B_papr_reduction: ACE only"); break;
        default: add_linef(info, "L1B_papr_reduction: Both TR and ACE"); break;
    }
    value = br_get(&br, 1); l1b_frame_length_mode = value;
    if (value == 0) {
        add_linef(info, "L1B_frame_length_mode: Time-aligned");
        value = br_get(&br, 10); add_linef(info, "  L1B_frame_length: %lu", value); l1b_frame_length = value;
        value = br_get(&br, 13); add_linef(info, "  L1B_excess_samples_per_symbol: %lu", value);
    } else {
        add_linef(info, "L1B_frame_length_mode: Symbol-aligned");
        value = br_get(&br, 16); add_linef(info, "  L1B_time_offset: %lu", value);
        value = br_get(&br, 7);  add_linef(info, "  L1B_additional_samples: %lu", value);
    }
    value = br_get(&br, 8); add_linef(info, "L1B_num_subframes: %lu", value + 1); l1b_num_subframes = value;
    value = br_get(&br, 3); add_linef(info, "L1B_preamble_num_symbols: %lu", value + 1); subframe_info[0].num_preamble_symbols = value + 1;
    value = br_get(&br, 3); add_linef(info, "L1B_preamble_reduced_carriers: %lu", value);
    value = br_get(&br, 2); add_linef(info, "L1B_L1_Detail_content_tag: %lu", value);
    value = br_get(&br, 13); add_linef(info, "L1B_L1_Detail_size_bytes: %lu", value); l1b_l1_detail_size_bytes = value;
    value = br_get(&br, 3); add_linef(info, "L1B_L1_Detail_fec_type: Mode %lu", value + 1);
    value = br_get(&br, 2); add_linef(info, "L1B_L1_additional_parity_mode: K=%lu", value);
    value = br_get(&br, 19); add_linef(info, "L1B_L1_Detail_total_cells: %lu", value); l1b_l1_detail_total_cells = value;
    value = br_get(&br, 1); add_linef(info, "L1B_first_sub_mimo: %s", value == 0 ? "No MIMO" : "MIMO"); l1b_first_sub_mimo = value;
    value = br_get(&br, 2); add_linef(info, "L1B_first_sub_miso: %lu", value);
    value = br_get(&br, 2); subframe_info[0].fft_size = value; add_linef(info, "L1B_first_sub_fft_size: %s", fft_name(value));
    value = br_get(&br, 3); subframe_info[0].reduced_carriers = value; add_linef(info, "L1B_first_sub_reduced_carriers: %lu", value);
    value = br_get(&br, 4); subframe_info[0].guard_interval = value; add_gi_line(info, "L1B_first_sub_guard_interval", value);
    value = br_get(&br, 11); add_linef(info, "L1B_first_sub_num_ofdm_symbols: %lu", value + 1); subframe_info[0].num_ofdm_symbols = value + 1;
    value = br_get(&br, 5); add_linef(info, "L1B_first_sub_scattered_pilot_pattern: %lu", value); subframe_info[0].scattered_pilot_pattern = value;
    value = br_get(&br, 3); add_linef(info, "L1B_first_sub_scattered_pilot_boost: %lu", value); subframe_info[0].scattered_pilot_boost = value;
    value = br_get(&br, 1); add_linef(info, "L1B_first_sub_sbs_first: %lu", value); subframe_info[0].sbs_first = value;
    value = br_get(&br, 1); add_linef(info, "L1B_first_sub_sbs_last: %lu", value); subframe_info[0].sbs_last = value;
    if (l1b_version >= 1) {
        value = br_get(&br, 1); add_linef(info, "L1B_first_sub_mimo_mixed: %lu", value); l1b_first_sub_mimo_mixed = value;
        br_get(&br, 47);
    } else {
        br_get(&br, 48);
    }
    value = br_get(&br, 32); add_linef(info, "L1B_crc: 0x%08lx", value);

    /* ---------------- L1-Detail ---------------- */
    add_linef(info, " ");
    add_linef(info, "--- L1-Detail Signaling ---");

    value = br_get(&br, 4); add_linef(info, "L1D_version: %lu", value); l1d_version = value;
    value = br_get(&br, 3); add_linef(info, "L1D_num_rf: %lu", value); l1d_num_rf = value;
    for (i = 1; i <= l1d_num_rf; i++) {
        value = br_get(&br, 16); add_linef(info, "  L1D_bonded_bsid: 0x%04lx", value);
        br_get(&br, 3);
    }
    if (l1b_time_info_flag != 0) {
        value = br_get(&br, 32); add_linef(info, "L1D_time_sec: %lu", value);
        value = br_get(&br, 10); add_linef(info, "L1D_time_msec: %lu", value);
        if (l1b_time_info_flag > 1) {
            value = br_get(&br, 10); add_linef(info, "L1D_time_usec: %lu", value);
            if (l1b_time_info_flag > 2) {
                value = br_get(&br, 10); add_linef(info, "L1D_time_nsec: %lu", value);
            }
        }
    }

    for (i = 0; i <= l1b_num_subframes && i < MAX_SUBFRAMES; i++) {
        struct subframe_info_t *sf = &subframe_info[i];

        add_linef(info, " ");
        add_linef(info, "Subframe #%d:", i);
        if (i > 0) {
            value = br_get(&br, 1); add_linef(info, "  L1D_mimo: %s", value == 0 ? "No MIMO" : "MIMO"); l1d_mimo = value;
            value = br_get(&br, 2); add_linef(info, "  L1D_miso: %lu", value);
            value = br_get(&br, 2); sf->fft_size = value; add_linef(info, "  L1D_fft_size: %s", fft_name(value));
            value = br_get(&br, 3); sf->reduced_carriers = value; add_linef(info, "  L1D_reduced_carriers: %lu", value);
            value = br_get(&br, 4); sf->guard_interval = value; add_gi_line(info, "  L1D_guard_interval", value);
            value = br_get(&br, 11); add_linef(info, "  L1D_num_ofdm_symbols: %lu", value + 1); sf->num_ofdm_symbols = value + 1;
            value = br_get(&br, 5); add_linef(info, "  L1D_scattered_pilot_pattern: %lu", value); sf->scattered_pilot_pattern = value;
            value = br_get(&br, 3); add_linef(info, "  L1D_scattered_pilot_boost: %lu", value); sf->scattered_pilot_boost = value;
            value = br_get(&br, 1); add_linef(info, "  L1D_sbs_first: %lu", value); sf->sbs_first = value;
            value = br_get(&br, 1); add_linef(info, "  L1D_sbs_last: %lu", value); sf->sbs_last = value;   // upstream mis-assigns this to sbs_first
        }
        if (l1b_num_subframes > 0) {
            value = br_get(&br, 1); add_linef(info, "  L1D_subframe_multiplex: %lu", value);
        }
        value = br_get(&br, 1); add_linef(info, "  L1D_frequency_interleaver: %s", value == 0 ? "Preamble Only" : "All Symbols");
        if (sf->sbs_first == 1 || sf->sbs_last == 1) {
            value = br_get(&br, 13); add_linef(info, "  L1D_sbs_null_cells: %lu", value);
        }
        value = br_get(&br, 6); add_linef(info, "  L1D_num_plp: %lu", value + 1); sf->num_plp = value + 1;

        for (j = 0; j < sf->num_plp; j++) {
            // Excess PLPs (malformed signaling) are still parsed to keep the bit position right
            struct plp_info_t dummy;
            struct plp_info_t *plp = (plp_count < MAX_PLPS) ? &plp_info[plp_count] : &dummy;
            int fec_raw;

            plp->subframe_index = i;
            plp->mod = -1;
            plp->cod = -1;
            plp->ldm_injection_db = -1.0;
            plp->display_line = -1;
            plp->snr_line = -1;

            add_linef(info, "    PLP #%d:", j);
            value = br_get(&br, 6); add_linef(info, "      L1D_plp_id: %lu", value); plp->plp_id = value;
            value = br_get(&br, 1); add_linef(info, "      L1D_plp_lls_flag: %lu", value);
            value = br_get(&br, 2); l1d_plp_layer = value; plp->layer = value;
            add_linef(info, "      L1D_plp_layer: %s", (value == 0) ? "Core" : (value == 1 ? "Enhanced" : "Reserved"));
            value = br_get(&br, 24); add_linef(info, "      L1D_plp_start: %lu", value); plp->start = value;
            value = br_get(&br, 24); add_linef(info, "      L1D_plp_size: %lu", value); plp->size = value;
            value = br_get(&br, 2); add_linef(info, "      L1D_plp_scrambler_type: %s", (value == 0) ? "PRBS" : "Reserved");
            value = br_get(&br, 4); fec_raw = value;
            switch (value) {
                case 0: add_linef(info, "      L1D_plp_fec_type: BCH + 16K LDPC"); break;
                case 1: add_linef(info, "      L1D_plp_fec_type: BCH + 64K LDPC"); break;
                case 2: add_linef(info, "      L1D_plp_fec_type: CRC + 16K LDPC"); break;
                case 3: add_linef(info, "      L1D_plp_fec_type: CRC + 64K LDPC"); break;
                case 4: add_linef(info, "      L1D_plp_fec_type: 16K LDPC only"); break;
                case 5: add_linef(info, "      L1D_plp_fec_type: 64K LDPC only"); break;
                default: add_linef(info, "      L1D_plp_fec_type: Reserved"); break;
            }
            if (fec_raw <= 5) {
                plp->fec_type = (fec_raw & 1) ? FECFRAME_NORMAL : FECFRAME_SHORT;
                if (context && !context->ldpc_info_available) {
                    context->ldpc_info_available = true;
                    context->ldpc_length = (fec_raw & 1) ? 1 : 0;   // 1 = 64K (long), 0 = 16K (short)
                }

                value = br_get(&br, 4); l1d_plp_mod = value; plp->mod = value;
                add_linef(info, "      L1D_plp_mod: %s", mod_name(value));
                value = br_get(&br, 4); plp->cod = value;
                if (value <= C13_15) {
                    add_linef(info, "      L1D_plp_cod: %lu/15", value + 2);
                } else {
                    add_linef(info, "      L1D_plp_cod: Reserved");
                }
            } else {
                plp->fec_type = -1;
                l1d_plp_mod = -1;
            }

            value = br_get(&br, 2); l1d_plp_TI_mode = value; plp->ti_mode = value;
            switch (value) {
                case 0: add_linef(info, "      L1D_plp_TI_mode: No TI"); break;
                case 1: add_linef(info, "      L1D_plp_TI_mode: CTI"); break;
                case 2: add_linef(info, "      L1D_plp_TI_mode: HTI"); break;
                default: add_linef(info, "      L1D_plp_TI_mode: Reserved"); break;
            }
            if (l1d_plp_TI_mode == 0) {
                value = br_get(&br, 15); add_linef(info, "      L1D_plp_fec_block_start: %lu", value);
            } else if (l1d_plp_TI_mode == 1) {
                value = br_get(&br, 22); add_linef(info, "      L1D_plp_CTI_fec_block_start: %lu", value);
            }
            if (l1d_num_rf > 0) {
                value = br_get(&br, 3); add_linef(info, "      L1D_plp_num_channel_bonded: %lu", value); l1d_plp_num_channel_bonded = value;
                if (l1d_plp_num_channel_bonded > 0) {
                    value = br_get(&br, 2); add_linef(info, "      L1D_plp_channel_bonding_format: %lu", value);
                    for (k = 0; k < l1d_plp_num_channel_bonded; k++) {
                        value = br_get(&br, 3); add_linef(info, "        L1D_plp_bonded_rf_id: %lu", value);
                    }
                }
            }
            if ((i == 0 && l1b_first_sub_mimo == 1) || (i > 0 && l1d_mimo)) {
                value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_stream_combining: %lu", value);
                value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_IQ_interleaving: %lu", value);
                value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_PH: %lu", value);
            }
            if (l1d_plp_layer == 0) {
                value = br_get(&br, 1);
                if (value == 0) {
                    add_linef(info, "      L1D_plp_type: non-dispersed");
                } else {
                    add_linef(info, "      L1D_plp_type: dispersed");
                    value = br_get(&br, 14); add_linef(info, "      L1D_plp_num_subslices: %lu", value + 1);
                    value = br_get(&br, 24); add_linef(info, "      L1D_plp_subslice_interval: %lu", value);
                }
                if ((l1d_plp_TI_mode == 1 || l1d_plp_TI_mode == 2) && l1d_plp_mod == MOD_QPSK) {
                    value = br_get(&br, 1); add_linef(info, "      L1D_plp_TI_extended_interleaving: %lu", value);
                }
                if (l1d_plp_TI_mode == 1) {
                    value = br_get(&br, 3); add_linef(info, "      L1D_plp_CTI_depth: %lu", value);
                    value = br_get(&br, 11); add_linef(info, "      L1D_plp_CTI_start_row: %lu", value);
                } else if (l1d_plp_TI_mode == 2) {
                    value = br_get(&br, 1); add_linef(info, "      L1D_plp_HTI_inter_subframe: %lu", value); l1d_plp_HTI_inter_subframe = value;
                    value = br_get(&br, 4); add_linef(info, "      L1D_plp_HTI_num_ti_blocks: %lu", value + 1); l1d_plp_HTI_num_ti_blocks = value;
                    value = br_get(&br, 12); add_linef(info, "      L1D_plp_HTI_num_fec_blocks_max: %lu", value + 1);
                    if (l1d_plp_HTI_inter_subframe == 0) {
                        value = br_get(&br, 12); add_linef(info, "      L1D_plp_HTI_num_fec_blocks: %lu", value + 1);
                        plp->HTI_num_fec_blocks = value + 1;
                    } else {
                        for (k = 0; k <= l1d_plp_HTI_num_ti_blocks; k++) {
                            value = br_get(&br, 12); add_linef(info, "        L1D_plp_HTI_num_fec_blocks: %lu", value + 1);
                        }
                    }
                    value = br_get(&br, 1); add_linef(info, "      L1D_plp_HTI_cell_interleaver: %lu", value);
                }
            } else {
                value = br_get(&br, 5); plp->ldm_injection_db = add_ldm_line(info, value);
            }

            // Placeholder lines; filled in once the whole frame structure is known
            plp->display_line = add_linef(info, " ");
            plp->snr_line = add_linef(info, " ");
            if (plp_count < MAX_PLPS) plp_count++;
        }
    }

    add_linef(info, " ");

    if (l1d_version >= 1) {
        value = br_get(&br, 16); add_linef(info, "L1D_bsid: 0x%04lx", value);
    }
    if (l1d_version >= 2) {
        for (i = 0; i <= l1b_num_subframes && i < MAX_SUBFRAMES; i++) {
            if (i > 0) {
                value = br_get(&br, 1); add_linef(info, "  Subframe #%d L1D_mimo_mixed: %lu", i, value); l1d_mimo_mixed = value;
            }
            if ((i == 0 && l1b_first_sub_mimo_mixed == 1) || (i > 0 && l1d_mimo_mixed == 1)) {
                // Uses this subframe's own PLP count (upstream uses the last subframe's)
                for (j = 0; j < subframe_info[i].num_plp; j++) {
                    value = br_get(&br, 1); add_linef(info, "    PLP #%d L1D_plp_mimo: %lu", j, value);
                    if (value == 1) {
                        value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_stream_combining: %lu", value);
                        value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_IQ_interleaving: %lu", value);
                        value = br_get(&br, 1); add_linef(info, "      L1D_plp_mimo_PH: %lu", value);
                    }
                }
            }
        }
    }

    // Skip any reserved bits before the CRC (L1-Basic is 200 bits)
    {
        long remaining = ((long)l1b_l1_detail_size_bytes * 8 - 32) - ((long)br.pos - 200);
        if (remaining > 0) br.pos += remaining;
    }
    value = br_get(&br, 32); add_linef(info, "L1D_crc: 0x%08lx", value);

    // Bitrates, now that every subframe's parameters are known
    int num_subframes = (l1b_num_subframes + 1 < MAX_SUBFRAMES) ? l1b_num_subframes + 1 : MAX_SUBFRAMES;
    for (int p = 0; p < plp_count; p++) {
        struct plp_info_t *plp = &plp_info[p];
        const struct subframe_info_t *sf;
        double bitrate = 0.0;

        if (plp->display_line < 0 || plp->subframe_index < 0) continue;
        sf = &subframe_info[plp->subframe_index];

        if (plp->mod >= 0 && plp->cod >= 0 && plp->fec_type >= 0) {
            bitrate = calculate_atsc3_bitrate_l1(
                sf->fft_size, sf->guard_interval, sf->num_ofdm_symbols,
                (plp->subframe_index == 0) ? sf->num_preamble_symbols : 0,
                plp->cod, plp->mod, plp->fec_type,
                sf->scattered_pilot_pattern, sf->sbs_first,
                sf->reduced_carriers, sf->scattered_pilot_boost,
                l1b_papr_reduction, l1b_l1_detail_total_cells,
                num_subframes, subframe_info,
                l1b_frame_length_mode, l1b_frame_length,
                plp->size, NULL);
        }

        if (bitrate > 0.0) {
            char line[128];
            snprintf(line, sizeof(line), "      -> PLP Bitrate: %.3f Mbps", bitrate / 1000000.0);
            char *copy = strdup(line);
            if (copy) {
                free(display_lines[plp->display_line]);
                display_lines[plp->display_line] = copy;
            }
        }
    }

    // Required SNR per PLP, LDM-adjusted where a core and enhanced PLP share cells.
    // Line text "-> PLP Required SNR:" is deliberately different from the
    // "-> Required SNR:" lines that follow the tuner's plpinfo summary.
    for (int p = 0; p < plp_count; p++) {
        struct plp_info_t *plp = &plp_info[p];
        double awgn, ray, inj = -1.0;
        char line[192], a_txt[32], r_txt[32], suffix[48] = "";

        if (plp->snr_line < 0 || !plp_threshold_db(plp, &awgn, &ray)) continue;

        if (plp->layer == 0) {
            // Strongest enhanced layer on top of this core PLP (lowest injection level)
            for (int q = 0; q < plp_count; q++) {
                const struct plp_info_t *e = &plp_info[q];
                if (e->layer == 1 && e->ldm_injection_db >= 0.0 && plps_overlap(plp, e)) {
                    if (inj < 0.0 || e->ldm_injection_db < inj) inj = e->ldm_injection_db;
                }
            }
            if (inj >= 0.0) {
                awgn = ldm_core_required_snr_db(awgn, inj);
                ray = ldm_core_required_snr_db(ray, inj);
            }
        } else if (plp->layer == 1 && plp->ldm_injection_db >= 0.0) {
            // Must first decode the core PLP(s) underneath; take the hardest one
            double core_a = -INFINITY, core_r = -INFINITY;
            inj = plp->ldm_injection_db;
            for (int q = 0; q < plp_count; q++) {
                const struct plp_info_t *c = &plp_info[q];
                double ca, cr;
                if (c->layer == 0 && plps_overlap(plp, c) && plp_threshold_db(c, &ca, &cr)) {
                    ca = ldm_core_required_snr_db(ca, inj);
                    cr = ldm_core_required_snr_db(cr, inj);
                    if (isnan(ca) || ca > core_a) core_a = ca;
                    if (isnan(cr) || cr > core_r) core_r = cr;
                    if (isnan(core_a) && isnan(core_r)) break;
                }
            }
            awgn = ldm_enhanced_required_snr_db(awgn, core_a, inj);
            ray = ldm_enhanced_required_snr_db(ray, core_r, inj);
        }

        if (inj >= 0.0) snprintf(suffix, sizeof(suffix), " (LDM, injection %.1f dB)", inj);
        fmt_snr_value(a_txt, sizeof(a_txt), awgn);
        fmt_snr_value(r_txt, sizeof(r_txt), ray);
        snprintf(line, sizeof(line), "      -> PLP Required SNR: AWGN %s, Rayleigh %s%s", a_txt, r_txt, suffix);
        char *copy = strdup(line);
        if (copy) {
            free(display_lines[plp->snr_line]);
            display_lines[plp->snr_line] = copy;
        }
    }

    *line_count = out.line_count;
}

void update_plp_snr_info_l1(char** display_lines, int line_count, int ldpc_length) {
    for (int i = 0; i < line_count; i++) {
        char* line = display_lines[i];
        if (strstr(line, "mod=") && strstr(line, "cod=")) {
            char *mod_ptr = strstr(line, "mod=");
            char *cod_ptr = strstr(line, "cod=");

            if (mod_ptr && cod_ptr) {
                char raw_mod_str[16] = {0}, normalized_mod_str[16] = {0}, cod_str[8] = {0};

                const char *mod_val_start = mod_ptr + 4;
                const char *mod_val_end = strchr(mod_val_start, ' ');
                size_t mod_len = mod_val_end ? (size_t)(mod_val_end - mod_val_start) : strlen(mod_val_start);
                if (mod_len < sizeof(raw_mod_str)) {
                    strncpy(raw_mod_str, mod_val_start, mod_len);
                    raw_mod_str[mod_len] = '\0';
                    normalize_mod_str_l1(raw_mod_str, normalized_mod_str, sizeof(normalized_mod_str));
                }

                const char *cod_val_start = cod_ptr + 4;
                const char *cod_val_end = strchr(cod_val_start, ' ');
                size_t cod_len = cod_val_end ? (size_t)(cod_val_end - cod_val_start) : strlen(cod_val_start);
                if (cod_len < sizeof(cod_str)) {
                    strncpy(cod_str, cod_val_start, cod_len);
                    cod_str[cod_len] = '\0';
                }

                if (i + 1 < line_count && strstr(display_lines[i + 1], "-> Required SNR:")) {
                    struct snr_pair_result snr_result = get_snr_pair_for_modcod_l1(normalized_mod_str, cod_str, ldpc_length);
                    if (snr_result.found) {
                        char snr_line[256] = {0};
                        if (snr_result.ldpc_length_known) {
                            snprintf(snr_line, sizeof(snr_line), "  -> Required SNR: AWGN %.2f dB, Rayleigh %.2f dB",
                                     snr_result.awgn_min, snr_result.rayleigh_min);
                        } else {
                            snprintf(snr_line, sizeof(snr_line), "  -> Required SNR: AWGN %.2f to %.2f dB, Rayleigh %.2f to %.2f dB",
                                     snr_result.awgn_min, snr_result.awgn_max,
                                     snr_result.rayleigh_min, snr_result.rayleigh_max);
                        }
                        char *copy = strdup(snr_line);
                        if (copy) {
                            free(display_lines[i + 1]);
                            display_lines[i + 1] = copy;
                        }
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Device-independent helpers                                               */
/* ------------------------------------------------------------------------ */

/*
 * Decode an L1 string (base64 L1-Basic + L1-Detail, as the HDHomeRun's
 * /tunerN/l1detail and alp-capture's .txt both hold) and append its full
 * parse to detail_info. Returns 0, or -1 if the string can't be decoded.
 */
int append_l1_detail_from_base64(struct l1_detail_info *detail_info, const char *l1_base64)
{
    if (!detail_info || !l1_base64) return -1;

    size_t decoded_len = b64_decoded_size_l1(l1_base64);
    unsigned char *decoded_data = malloc(decoded_len ? decoded_len : 1);
    if (!decoded_data) return -1;
    if (!b64_decode_l1(l1_base64, decoded_data, decoded_len)) {
        free(decoded_data);
        return -1;
    }
    parse_l1_data_l1(decoded_data, decoded_len, detail_info->display_lines,
                     &detail_info->line_count, detail_info->max_lines, &detail_info->context);
    free(decoded_data);
    return 0;
}

int save_atsc3_details_to_file(const char* filename, struct l1_detail_info* detail_info, const char* l1_detail_base64) {
    if (!filename || !detail_info) return -1;

    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    for (int i = 0; i < detail_info->line_count; i++) {
        if (strcmp(detail_info->display_lines[i], "__HLINE__") == 0) {
            fprintf(f, "================================================================================\n");
        } else {
            fprintf(f, "%s\n", detail_info->display_lines[i]);
        }
    }

    // Add base64 L1 detail string at the end (for saved files only)
    if (l1_detail_base64 && strlen(l1_detail_base64) > 0) {
        fprintf(f, "\n");
        fprintf(f, "================================================================================\n\n");
        fprintf(f, "Raw L1 Detail (Base64):\n");
        fprintf(f, "%s\n", l1_detail_base64);
    }

    fclose(f);
    return 0;
}
