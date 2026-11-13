/*
 * l1_details.c
 *
 * The ATSC 3.0 details screen and the .txt saved with captures: collects
 * PLP info, IDs and the L1 string from a tuner (any backend, via tuner.h)
 * and formats them with l1_detail_parser. Split out of l1_detail_parser.c
 * so the parser itself needs no tuner code and can be shared with RENDER.
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
#include "l1_details.h"
#include "tuner.h"

/* ------------------------------------------------------------------------ */
/* Collection / file output                                                 */
/* ------------------------------------------------------------------------ */

static void push_line(struct l1_detail_info *info, const char *s)
{
    if (info->line_count < info->max_lines) {
        char *copy = strdup(s);
        if (copy) info->display_lines[info->line_count++] = copy;
    }
}

int collect_atsc3_details(struct tuner *t, struct l1_detail_info* detail_info) {
    if (!t || !detail_info) return -1;

    detail_info->line_count = 0;
    detail_info->context.ldpc_info_available = false;
    detail_info->context.ldpc_length = -1;

    const char *plpinfo_str_orig;
    const char *streaminfo_str_orig;

    if (tuner_get_plpinfo(t, &plpinfo_str_orig) <= 0) {
        return -1; // No PLP info available
    }
    char *plpinfo_copy = strdup(plpinfo_str_orig);

    if (tuner_get_streaminfo(t, &streaminfo_str_orig) <= 0) {
        streaminfo_str_orig = "";
    }
    char *streaminfo_copy = strdup(streaminfo_str_orig);

    push_line(detail_info, " ");

    const char *version_str;
    if (tuner_get_version(t, &version_str) > 0) {
        char version_line[128];
        snprintf(version_line, sizeof(version_line), "Firmware Version: %s", version_str);
        push_line(detail_info, version_line);
        push_line(detail_info, " ");
    }

    long bsid = -999;
    long tsid = -999;

    const char *fresh_plpinfo;
    if (tuner_get_plpinfo(t, &fresh_plpinfo) > 0) {
        bsid = parse_status_value_l1(fresh_plpinfo, "bsid=");
    }

    const char *fresh_streaminfo;
    if (tuner_get_streaminfo(t, &fresh_streaminfo) > 0) {
        tsid = parse_status_value_l1(fresh_streaminfo, "tsid=");
    }

    char id_line[64];
    if (bsid != -999) {
        snprintf(id_line, sizeof(id_line), "L1D BSID: %ld (0x%lX)", bsid, bsid);
        push_line(detail_info, id_line);
    } else {
        push_line(detail_info, "L1D BSID: Not set");
    }
    if (tsid != -999) {
        snprintf(id_line, sizeof(id_line), "SLT TSID: %ld (0x%lX)", tsid, tsid);
        push_line(detail_info, id_line);
    } else {
        push_line(detail_info, "SLT TSID: Not set");
    }
    push_line(detail_info, " ");

    // Map PLP ID -> required-SNR text computed by the L1 parser (per-PLP LDPC
    // length, LDM-adjusted). Text is what follows "-> PLP Required SNR:".
    struct plp_snr_map {
        int plp_id;
        char text[128];
    } plp_snr_map[MAX_PLPS];
    int plp_map_count = 0;

    // The tuner decides whether it can report L1 (HDHomeRun: models with
    // dBm values, on firmware with /tunerN/l1detail)
    {
        const char *l1_detail_str;
        if (tuner_get_l1_string(t, &l1_detail_str) > 0) {
            size_t decoded_len = b64_decoded_size_l1(l1_detail_str);
            unsigned char *decoded_data = malloc(decoded_len ? decoded_len : 1);
            if (decoded_data) {
                if (b64_decode_l1(l1_detail_str, decoded_data, decoded_len)) {
                    char** temp_lines = malloc(MAX_DISPLAY_LINES * sizeof(char*));
                    int temp_line_count = 0;
                    struct l1_parse_context temp_context = {false, -1};

                    if (temp_lines) {
                        parse_l1_data_l1(decoded_data, decoded_len, temp_lines,
                                        &temp_line_count, MAX_DISPLAY_LINES, &temp_context);

                        int current_plp_id = -1;
                        for (int j = 0; j < temp_line_count && plp_map_count < MAX_PLPS; j++) {
                            const char *snr;
                            if (strstr(temp_lines[j], "L1D_plp_id:")) {
                                sscanf(temp_lines[j], "%*[^:]: %d", &current_plp_id);
                            } else if ((snr = strstr(temp_lines[j], "-> PLP Required SNR:")) && current_plp_id >= 0) {
                                plp_snr_map[plp_map_count].plp_id = current_plp_id;
                                snprintf(plp_snr_map[plp_map_count].text, sizeof(plp_snr_map[0].text),
                                         "%s", snr + strlen("-> PLP Required SNR:"));
                                plp_map_count++;
                                current_plp_id = -1;
                            }
                        }

                        for (int j = 0; j < temp_line_count; j++) {
                            free(temp_lines[j]);
                        }
                        free(temp_lines);
                    }
                }
                free(decoded_data);
            }
        }
    }

    // PLP summary lines from the tuner, with required SNR
    if (plpinfo_copy) {
        char *line = strtok(plpinfo_copy, "\n");
        while (line != NULL && detail_info->line_count < detail_info->max_lines) {
            if (strncmp(line, "bsid=", 5) != 0) {
                push_line(detail_info, line);

                int line_plp_id = -1;
                sscanf(line, "%d:", &line_plp_id);

                char *mod_ptr = strstr(line, "mod=");
                char *cod_ptr = strstr(line, "cod=");

                // Prefer the L1-derived value (correct LDPC length, LDM-adjusted)
                const char *l1_snr = NULL;
                for (int i = 0; i < plp_map_count; i++) {
                    if (plp_snr_map[i].plp_id == line_plp_id) {
                        l1_snr = plp_snr_map[i].text;
                        break;
                    }
                }

                if (l1_snr) {
                    char snr_line[192];
                    snprintf(snr_line, sizeof(snr_line), "  -> Required SNR:%s", l1_snr);
                    push_line(detail_info, snr_line);
                } else if (mod_ptr && cod_ptr) {
                    // No L1 detail available: plain modcod lookup, LDPC length unknown
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

                    struct snr_pair_result snr_result = get_snr_pair_for_modcod_l1(normalized_mod_str, cod_str, -1);
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
                        push_line(detail_info, snr_line);
                    }
                }

                push_line(detail_info, " ");
            }
            line = strtok(NULL, "\n");
        }
    }

    // Full L1 detail dump
    {
        const char *l1_detail_str;
        if (tuner_get_l1_string(t, &l1_detail_str) > 0) {
            if (detail_info->line_count < detail_info->max_lines - 3) {
                push_line(detail_info, "__HLINE__");
                push_line(detail_info, " ");
            }

            size_t decoded_len = b64_decoded_size_l1(l1_detail_str);
            unsigned char *decoded_data = malloc(decoded_len ? decoded_len : 1);
            if (decoded_data) {
                if (b64_decode_l1(l1_detail_str, decoded_data, decoded_len)) {
                    // The summary SNR lines above already carry per-PLP values from the
                    // L1 parse, so update_plp_snr_info_l1() (one LDPC length for all
                    // PLPs, no LDM) is no longer called here.
                    parse_l1_data_l1(decoded_data, decoded_len, detail_info->display_lines,
                                    &detail_info->line_count, detail_info->max_lines, &detail_info->context);
                }
                free(decoded_data);
            }
        }
    }

    free(streaminfo_copy);
    free(plpinfo_copy);
    return 0;
}

int save_atsc3_details_auto(struct tuner *t, const char* base_filename) {
    if (!t || !base_filename) return -1;

    struct l1_detail_info* detail_info = create_l1_detail_info(MAX_DISPLAY_LINES);
    if (!detail_info) return -1;

    char *saved_l1_detail_str = NULL;

    int result = collect_atsc3_details(t, detail_info);
    if (result != 0) {
        free_l1_detail_info(detail_info);
        return result;
    }

    {
        const char *l1_detail_str;
        if (tuner_get_l1_string(t, &l1_detail_str) > 0) {
            saved_l1_detail_str = strdup(l1_detail_str);
        }
    }

    // Replace extension with .txt
    char details_filename[512];
    const char* last_dot = strrchr(base_filename, '.');
    if (last_dot) {
        size_t base_len = last_dot - base_filename;
        snprintf(details_filename, sizeof(details_filename), "%.*s.txt", (int)base_len, base_filename);
    } else {
        snprintf(details_filename, sizeof(details_filename), "%s.txt", base_filename);
    }

    result = save_atsc3_details_to_file(details_filename, detail_info, saved_l1_detail_str);

    free(saved_l1_detail_str);
    free_l1_detail_info(detail_info);
    return result;
}
