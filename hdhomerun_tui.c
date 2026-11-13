/*
 * hdhomerun_tui.c
 *
 * An ncurses-based command-line tool for interacting with HDHomeRun devices.
 *
 * To compile, you'll need the libhdhomerun source code in a subdirectory
 * named "libhdhomerun" and the ncurses development library installed.
 *
 * On Debian/Ubuntu: sudo apt-get install libncurses5-dev
 * On RedHat/CentOS: sudo yum install ncurses-devel
 * On macOS (using Homebrew): brew install ncurses
 *
 * Then, you can compile with the provided Makefile:
 * make
 * 
 * *****
 * This code has been written almost entirely with Google Gemini and Claude  
 * as a personal test of how effective LLMs can be with respect to writing 
 * code. The answer, as it turns out, is "very," but everything needs to be 
 * validated and it took many iterations to make this work properly.
 * *****
 * 
 * * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * * *****
 *
 * L1 parsing logic adapted from l1dump.c:
 * https://github.com/drmpeg/dtv-utils/blob/master/l1dump.c
 * */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdint.h>
#include "hdhomerun.h"
#include "hdhomerun_device.h"

// Headers for native TCP/HTTP download
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include "l1_detail_parser.h"

#define MAX_DEVICES 10
#define MAX_TUNERS_TOTAL 32 // Max combined tuners from all devices
#define BAR_WIDTH 30
#define MAX_CHANNELS 256
#define LEFT_PANE_WIDTH 14
#define MAX_PLPS 64
#define MAX_MAPS 20 // Maximum number of channel maps supported
#define MAX_PROGRAMS 128

static const char* TUI_VERSION = "0.9.0";

// A struct to hold information about a single, unique tuner
struct unified_tuner {
    uint32_t device_id;
    char ip_str[64];
    int tuner_index;
    int total_tuners_on_device;
    bool is_legacy;
};

// A struct to hold the parsed channel list for a tuner
struct channel_list {
    unsigned int channels[MAX_CHANNELS];
    int count;
};

// A struct to hold a single line of PLP info for sorting
struct plp_line {
    int id;
    char text[256];
};

// Enum to define the different types of save operations
enum save_mode {
    SAVE_NORMAL_TS,
    SAVE_AUTORESTART_TS,
    SAVE_NORMAL_DBG,
    SAVE_AUTORESTART_DBG,
    SAVE_NORMAL_PCAP,
    SAVE_AUTORESTART_PCAP
};

// Function Prototypes
int discover_and_build_tuner_list(struct unified_tuner tuners[]);
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int db_value, const char* db_unit);
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...);
int draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, int scroll_offset);
int show_help_screen(WINDOW *parent_win);
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, enum save_mode mode, struct unified_tuner *tuner_info, bool debug_enabled);
int main_loop(void);
int compare_channels(const void *a, const void *b);
int compare_plps(const void *a, const void *b);
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list);
long parse_db_value(const char *status_str, const char *key);
long parse_status_value(const char *status_str, const char *key);
char* stream_to_vlc(struct hdhomerun_device_t *hd, WINDOW *win, pid_t *vlc_pid, struct unified_tuner *tuner_info);
int select_program_menu(WINDOW *win, char *streaminfo_str, char *selected_program_str, int *selected_plp);
int get_udp_port();
int show_plp_details_screen(WINDOW *parent_win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info);
void add_manual_device(WINDOW *win, struct unified_tuner tuners[], int *total_tuners);
int http_save_stream(const char *ip_addr, const char *url, const char *filename, WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, bool autorestart_enabled, int save_attempts, int max_save_attempts, bool *out_aborted, bool *out_error_detected, bool debug_enabled);

// Global variable to hold initial IP from command line (-i option)
static char *g_initial_ip = NULL;

/*
 * add_device_by_ip
 * Queries a device by IP and adds its tuners to the list. No UI required.
 * Returns the number of tuners added, or 0 on failure.
 */
int add_device_by_ip(const char *ip_str, struct unified_tuner tuners[], int *total_tuners) {
    if (*total_tuners >= MAX_TUNERS_TOTAL) {
        return 0;
    }

    struct hdhomerun_device_t *hd = hdhomerun_device_create_from_str(ip_str, NULL);
    if (!hd) {
        return 0;
    }

    char *model_str;
    if (hdhomerun_device_get_var(hd, "/sys/model", &model_str, NULL) <= 0) {
        hdhomerun_device_destroy(hd);
        return 0;
    }

    uint32_t device_id = hdhomerun_device_get_device_id(hd);
    
    // Check for duplicates
    for (int i = 0; i < *total_tuners; i++) {
        if (tuners[i].device_id == device_id) {
            hdhomerun_device_destroy(hd);
            return 0;
        }
    }

    // Determine if legacy (heuristic based on model string)
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

    int added = 0;
    if (device_tuner_count > 0) {
        for (int i = 0; i < device_tuner_count && *total_tuners < MAX_TUNERS_TOTAL; i++) {
            tuners[*total_tuners].device_id = device_id;
            strncpy(tuners[*total_tuners].ip_str, ip_str, sizeof(tuners[*total_tuners].ip_str) - 1);
            tuners[*total_tuners].ip_str[sizeof(tuners[*total_tuners].ip_str) - 1] = '\0';
            tuners[*total_tuners].tuner_index = i;
            tuners[*total_tuners].total_tuners_on_device = device_tuner_count;
            tuners[*total_tuners].is_legacy = is_legacy;
            (*total_tuners)++;
            added++;
        }
    }

    hdhomerun_device_destroy(hd);
    return added;
}

/*
 * discover_and_build_tuner_list
 * Finds HDHomeRun devices and populates a flat list of all available tuners.
 */
int discover_and_build_tuner_list(struct unified_tuner tuners[]) {
    clear();
    mvprintw(0, 0, "Discovering HDHomeRun devices...");
    refresh();

    struct hdhomerun_discover_t *ds = hdhomerun_discover_create(NULL);
    if (!ds) return 0;

    uint32_t device_types[1] = { HDHOMERUN_DEVICE_TYPE_TUNER };
    if (hdhomerun_discover2_find_devices_broadcast(ds, HDHOMERUN_DISCOVER_FLAGS_IPV4_GENERAL, device_types, 1) < 0) {
        hdhomerun_discover_destroy(ds);
        return 0;
    }

    int total_tuner_count = 0;
    struct hdhomerun_discover2_device_t *device = hdhomerun_discover2_iter_device_first(ds);
    while (device && total_tuner_count < MAX_TUNERS_TOTAL) {
        uint32_t device_id = hdhomerun_discover2_device_get_device_id(device);
        int tuner_count = hdhomerun_discover2_device_get_tuner_count(device);
        bool is_legacy = hdhomerun_discover2_device_is_legacy(device);
        char ip_str[64];

        struct hdhomerun_discover2_device_if_t *device_if = hdhomerun_discover2_iter_device_if_first(device);
        struct sockaddr_storage ip_address;
        hdhomerun_discover2_device_if_get_ip_addr(device_if, &ip_address);
        hdhomerun_sock_sockaddr_to_ip_str(ip_str, (struct sockaddr *)&ip_address, true);

        for (int i = 0; i < tuner_count && total_tuner_count < MAX_TUNERS_TOTAL; i++) {
            tuners[total_tuner_count].device_id = device_id;
            strcpy(tuners[total_tuner_count].ip_str, ip_str);
            tuners[total_tuner_count].tuner_index = i;
            tuners[total_tuner_count].total_tuners_on_device = tuner_count;
            tuners[total_tuner_count].is_legacy = is_legacy;
            total_tuner_count++;
        }

        device = hdhomerun_discover2_iter_device_next(device);
    }

    hdhomerun_discover_destroy(ds);
    clear();
    refresh();
    return total_tuner_count;
}

/*
 * draw_signal_bar
 * Draws a color-coded bar graph for a signal percentage, with optional dB value.
 */
void draw_signal_bar(WINDOW *win, int y, int x, const char *label, int percentage, int db_value, const char* db_unit) {
    char db_str[16] = "";
    if (db_value != -999) {
        sprintf(db_str, "[%3d %s]", db_value, db_unit);
    }
    
    mvwprintw(win, y, x, "%-18s: [%3d%%] ", label, percentage);
    
    int bar_fill_width = (percentage * BAR_WIDTH) / 100;
    if (bar_fill_width > BAR_WIDTH) bar_fill_width = BAR_WIDTH;

    int color_pair = 1; // Default Red
    if (percentage >= 75) color_pair = 3; // Green
    else if (percentage >= 50) color_pair = 2; // Yellow

    wattron(win, COLOR_PAIR(color_pair));
    for (int i = 0; i < bar_fill_width; ++i) waddch(win, '#');
    wattroff(win, COLOR_PAIR(color_pair));

    for (int i = bar_fill_width; i < BAR_WIDTH; ++i) waddch(win, '-');
    
    wprintw(win, " %s ", db_str);
}

/*
 * print_line_in_box
 * Formats and prints a line, safely truncating it to fit inside the window box.
 */
void print_line_in_box(WINDOW *win, int y, int x, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    int max_len = getmaxx(win) - x - 1;
    if (max_len < 0) return;
    mvwaddnstr(win, y, x, buffer, max_len);
}

/*
 * parse_db_value
 * Helper to find a dB value from a key like "ss=100(-35dBm)".
 */
long parse_db_value(const char *status_str, const char *key) {
    const char *key_found = strstr(status_str, key);
    if (key_found) {
        const char *paren_open = strchr(key_found, '(');
        if (paren_open) {
            return strtol(paren_open + 1, NULL, 10);
        }
    }
    return -999; // Sentinel for not found
}

/*
 * parse_status_value
 * Helper to find a numeric value for a given key in the raw status string.
 * It now uses base 0 for strtol to auto-detect hex (0x) and decimal.
 */
long parse_status_value(const char *status_str, const char *key) {
    const char *found = strstr(status_str, key);
    if (found) {
        // Use base 0 to auto-detect number format (e.g., "0x" for hex)
        return strtol(found + strlen(key), NULL, 0);
    }
    return -999; // Sentinel for not found
}


/*
 * draw_status_pane
 * Fetches and displays the status of a tuner in a dedicated sub-window.
 * Returns the total number of content lines, for scrolling purposes.
 */
int draw_status_pane(WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, int scroll_offset) {
    werase(win);
    box(win, 0, 0);
    
    if (!hd || !tuner_info) {
        mvwprintw(win, 1, 2, "No Tuner Selected");
        return 0;
    }

    char title[128];
    sprintf(title, " Tuner %08X-%d (%s) Status ", tuner_info->device_id, tuner_info->tuner_index, tuner_info->ip_str);
    mvwprintw(win, 0, 2, "%s", title);

    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    bool is_atsc3 = false;
    int total_content_lines = 0;
    int y = 2; // Start drawing at line 2

    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
        long bps = parse_status_value(raw_status_str, "bps=");
        long pps = parse_status_value(raw_status_str, "pps=");
        long rssi = parse_db_value(raw_status_str, "ss=");
        long snr = parse_db_value(raw_status_str, "snq=");

        total_content_lines = 11; // Base number of lines for the top section

        // --- All drawing is now conditional on scroll position ---
        if (y - scroll_offset > 0) {
            char channel_display[64];
            strncpy(channel_display, status.channel, sizeof(channel_display) - 1);
            channel_display[sizeof(channel_display) - 1] = '\0';

            char lock_display[64];
            strncpy(lock_display, status.lock_str, sizeof(lock_display) - 1);
            lock_display[sizeof(lock_display) - 1] = '\0';

            // If channel is atsc3 and has PLPs defined (e.g. "atsc3:33:0+16"), format display.
            if (strncmp(status.channel, "atsc3:", 6) == 0) {
                char *first_colon = strchr(status.channel, ':');
                if (first_colon) {
                    char *second_colon = strchr(first_colon + 1, ':');
                    if (second_colon) {
                        // Create a temporary copy to modify for channel display
                        char temp_channel[64];
                        strncpy(temp_channel, status.channel, sizeof(temp_channel));
                        temp_channel[second_colon - status.channel] = '\0';
                        snprintf(channel_display, sizeof(channel_display), "%s", temp_channel);

                        // Format the lock display to show the PLPs
                        snprintf(lock_display, sizeof(lock_display), "atsc3:%s", second_colon + 1);
                    }
                }
            }
            print_line_in_box(win, y - scroll_offset, 2, "Channel: %-15s", channel_display);
            print_line_in_box(win, y - scroll_offset, 28, "Lock: %s", lock_display);
        }
        y++;

        if (strstr(status.lock_str, "atsc3") != NULL) is_atsc3 = true;
        const char *id_label = is_atsc3 ? "BSID" : "TSID";
        long id_val = -999;
        
        char *streaminfo;
        if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) id_val = parse_status_value(streaminfo, "tsid=");
        char *plpinfo;
        if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
            long bsid = parse_status_value(plpinfo, "bsid=");
            if (bsid != -999) id_val = bsid;
        }
        if (id_val != -999) {
            if (y - scroll_offset > 0) print_line_in_box(win, y - scroll_offset, 2, "%s: %ld (0x%lX)", id_label, id_val, id_val);
        }
        y += 2; // Spacer

        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Signal Strength", status.signal_strength, rssi, "dBm"); } y++;
        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Signal Quality", status.signal_to_noise_quality, snr, "dB "); } y++;
        if (y - scroll_offset > 0) { draw_signal_bar(win, y - scroll_offset, 2, "Symbol Quality", status.symbol_error_quality, -999, ""); } y++;
        
        double mbps = (pps > 0 && bps != -999) ? (double)bps / 1000000.0 : 0.0;
        if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "%-18s: %.3f Mbps", "Network Rate", mbps); } y++;

        char *target_str;
        if (hdhomerun_device_get_tuner_target(hd, &target_str) > 0) {
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "%-18s: %s", "Network Target", target_str); } y++;
        }
        
        if (y - scroll_offset > 0) { mvwhline(win, y - scroll_offset, 2, ACS_HLINE, getmaxx(win) - 4); } y++;

        struct hdhomerun_tuner_vstatus_t vstatus;
        char *vstatus_str;
        if (hdhomerun_device_get_tuner_vstatus(hd, &vstatus_str, &vstatus) > 0 && strlen(vstatus.vchannel) > 0) {
            total_content_lines += 2;
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "Virtual Channel: %s", vstatus.vchannel); } y++;
            if (y - scroll_offset > 0) { print_line_in_box(win, y - scroll_offset, 2, "Name: %s", vstatus.name); } y++;
        }
        
        char *streaminfo_prog;
        if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_prog) > 0) {
            char *programs[MAX_PROGRAMS];
            int program_count = 0;
            char *streaminfo_copy = strdup(streaminfo_prog);
            if(streaminfo_copy) {
                char *line = strtok(streaminfo_copy, "\n");
                while (line != NULL && program_count < MAX_PROGRAMS) {
                    if (strchr(line, ':') || strstr(line, "program=")) programs[program_count++] = strdup(line);
                    line = strtok(NULL, "\n");
                }
                free(streaminfo_copy);
            }

            // If program count > 3, use two columns, provided the window is wide enough.
            bool two_columns = (program_count > 3) && (getmaxx(win) > 70);

            total_content_lines += 1; // For "Programs:" title
            if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                print_line_in_box(win, y - scroll_offset, 2, "Programs:");
            }
            y++;

            if (two_columns) {
                int midpoint = (program_count + 1) / 2;
                total_content_lines += midpoint;
                for (int i = 0; i < midpoint; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        // Print left column
                        if (strstr(programs[i], "(encrypted)")) {
                            wattron(win, COLOR_PAIR(1));
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                            wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                        }
                        
                        // Print right column
                        if (i + midpoint < program_count) {
                            if (strstr(programs[i + midpoint], "(encrypted)")) {
                                wattron(win, COLOR_PAIR(1));
                                print_line_in_box(win, y - scroll_offset, getmaxx(win)/2, programs[i + midpoint]);
                                wattroff(win, COLOR_PAIR(1));
                            } else {
                                print_line_in_box(win, y - scroll_offset, getmaxx(win)/2, programs[i + midpoint]);
                            }
                        }
                    }
                    y++;
                }
            } else {
                total_content_lines += program_count;
                for (int i = 0; i < program_count; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        if (strstr(programs[i], "(encrypted)")) {
                            wattron(win, COLOR_PAIR(1));
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                            wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, programs[i]);
                        }
                    }
                    y++;
                }
            }

            for (int i = 0; i < program_count; i++) free(programs[i]);
        }
        
        char *plpinfo_str;
        if (is_atsc3 && hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str) > 0) {
            struct plp_line plp_lines[MAX_PLPS];
            int plp_count = 0;
            char *plpinfo_copy = strdup(plpinfo_str);
            if(plpinfo_copy) {
                char *line = strtok(plpinfo_copy, "\n");
                while(line != NULL && plp_count < MAX_PLPS) {
                    if (strncmp(line, "bsid=", 5) != 0) {
                        sscanf(line, "%d:", &plp_lines[plp_count].id);
                        strncpy(plp_lines[plp_count].text, line, sizeof(plp_lines[0].text) - 1);
                        plp_lines[plp_count].text[sizeof(plp_lines[0].text) - 1] = '\0';
                        plp_count++;
                    }
                    line = strtok(NULL, "\n");
                }
                free(plpinfo_copy);
            }

            if (plp_count > 0) {
                total_content_lines += 2 + plp_count;
                if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                    mvwhline(win, y - scroll_offset, 2, ACS_HLINE, getmaxx(win) - 4);
                }
                y++;
                if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                    print_line_in_box(win, y - scroll_offset, 2, "PLP Info:");
                }
                y++;
                qsort(plp_lines, plp_count, sizeof(struct plp_line), compare_plps);
                for (int i = 0; i < plp_count; i++) {
                    if (y - scroll_offset > 0 && (y - scroll_offset) < getmaxy(win) - 2) {
                        char* line_to_print = plp_lines[i].text;
                        if (strstr(line_to_print, "lock=1")) {
                            wattron(win, COLOR_PAIR(3)); print_line_in_box(win, y - scroll_offset, 4, line_to_print); wattroff(win, COLOR_PAIR(3));
                        } else if (strstr(line_to_print, "lock=0")) {
                            wattron(win, COLOR_PAIR(1)); print_line_in_box(win, y - scroll_offset, 4, line_to_print); wattroff(win, COLOR_PAIR(1));
                        } else {
                            print_line_in_box(win, y - scroll_offset, 4, line_to_print);
                        }
                    }
                    y++;
                }
            }
        }
    }
    return total_content_lines;
}

/*
 * compare_channels
 * A helper function for qsort to sort the channel list numerically.
 */
int compare_channels(const void *a, const void *b) {
   return (*(int*)a - *(int*)b);
}

/*
 * compare_plps
 * A helper function for qsort to sort the PLP list numerically.
 */
int compare_plps(const void *a, const void *b) {
    struct plp_line *plpA = (struct plp_line *)a;
    struct plp_line *plpB = (struct plp_line *)b;
    return (plpA->id - plpB->id);
}

/*
 * populate_channel_list
 * Gets the tuner's channel map, parses it, and stores a sorted list of channels.
 */
void populate_channel_list(struct hdhomerun_device_t *hd, struct channel_list *list) {
    list->count = 0;
    char *map_str;
    if (hdhomerun_device_get_tuner_channelmap(hd, &map_str) <= 0) return;

    char *map_copy = strdup(map_str);
    if (!map_copy) return;

    char *token = strtok(map_copy, " ");
    token = strtok(NULL, " "); 

    while (token != NULL && list->count < MAX_CHANNELS) {
        list->channels[list->count++] = (unsigned int)strtoul(token, NULL, 10);
        token = strtok(NULL, " ");
    }
    free(map_copy);
    if (list->count > 0) qsort(list->channels, list->count, sizeof(unsigned int), compare_channels);
}

/*
 * show_help_screen
 * Displays a scrollable help screen. Returns 1 if user quits, 0 otherwise.
 */
int show_help_screen(WINDOW *parent_win) {
    char version_title[64];
    sprintf(version_title, "HDHomeRun TUI Help (v%s)", TUI_VERSION);

    const char *help_text[] = {
        version_title,
        "",
        "KEY BINDINGS:",
        "  Up/Dn Arrows : Select tuner to view.",
        "  PgUp/PgDn    : Scroll status panel if content overflows.",
        "  Lf/Rt Arrows : Change channel.",
        "  +/- Keys     : Seek for next/previous active channel.",
        "  v            : View stream in VLC (select program for ATSC 1.0).",
        "  d (ATSC 3.0) : Show detailed PLP information and SNR requirements.",
        "  c            : Manually tune to a channel/frequency.",
        "  m            : Change the tuner's channel map.",
        "  p            : Set the tuned ATSC 3.0 PLPs.",
        "  s (ATSC 1.0) : Save a 30-second transport stream capture.",
        "  s (ATSC 3.0) : Save a 30-second debug capture.",
        "  a (ATSC 1.0) : Save a 30-second TS capture with error checking.",
        "  a (ATSC 3.0) : Save a 30-second DBG capture with error checking.",
        "  x (ATSC 3.0) : Save a 30-second PCAP capture, if supported.",
        "  z (ATSC 3.0) : Save a 30-second PCAP capture with error checking.",
        "  Backspace    : During a save, press Backspace to abort.",
        "  r            : Refresh the device list.",
        "  i            : Manually add a device by IP address.",
        "  h            : Show this help screen.",
        "  q            : Quit the application.",
        NULL
    };

    int num_lines = 0;
    while(help_text[num_lines] != NULL) num_lines++;

    int parent_h, parent_w, parent_y, parent_x;
    getmaxyx(parent_win, parent_h, parent_w);
    getbegyx(parent_win, parent_y, parent_x);

    WINDOW *help_win = newwin(parent_h, parent_w, parent_y, parent_x);
    
    int scroll_pos = 0;
    keypad(help_win, TRUE);
    nodelay(stdscr, FALSE);

    while(1) {
        werase(help_win);
        box(help_win, 0, 0);
        mvwprintw(help_win, 0, 2, " Help ");

        int max_display_lines = getmaxy(help_win) - 3;
        for (int i = 0; i < max_display_lines; i++) {
            if (scroll_pos + i < num_lines) {
                mvwprintw(help_win, i + 1, 2, "%s", help_text[scroll_pos + i]);
            }
        }
        
        mvwprintw(help_win, getmaxy(help_win) - 2, 2, "Scroll: Up/Down/PgUp/PgDn | Close: h or Enter | Quit: q");
        wrefresh(help_win);

        int ch = wgetch(help_win);
        switch(ch) {
            case KEY_UP:
                if (scroll_pos > 0) scroll_pos--;
                break;
            case KEY_DOWN:
                if (num_lines > max_display_lines && scroll_pos < num_lines - max_display_lines) {
                    scroll_pos++;
                }
                break;
            case KEY_PPAGE:
                scroll_pos -= max_display_lines;
                if (scroll_pos < 0) scroll_pos = 0;
                break;
            case KEY_NPAGE:
                if (num_lines > max_display_lines) {
                    scroll_pos += max_display_lines;
                    if (scroll_pos > num_lines - max_display_lines) {
                        scroll_pos = num_lines - max_display_lines;
                    }
                }
                break;
            case 'q':
                delwin(help_win);
                return 1;
            case 'h':
            case '\n':
            case '\r':
                delwin(help_win);
                nodelay(stdscr, TRUE);
                return 0;
        }
    }
}

/*
 * http_save_stream
 * Performs a download of an HTTP stream using native sockets, replacing wget.
 * Returns 0 on success, -1 on failure.
 */
int http_save_stream(const char *ip_addr, const char *url, const char *filename, WINDOW *win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info, bool autorestart_enabled, int save_attempts, int max_save_attempts, bool *out_aborted, bool *out_error_detected, bool debug_enabled) {
    *out_aborted = false;
    *out_error_detected = false;

    // 1. Create and connect socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int rcvbuf_size = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    if (sock < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Could not create socket."); wrefresh(win); sleep(2);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5004);
    if (inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr) <= 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Invalid IP address."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Could not connect to device."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 2. Send HTTP GET request
    const char *path_start = strstr(url, "/tuner");
    if (!path_start) {
        print_line_in_box(win, LINES - 3, 2, "Error: Invalid URL for request."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }
    char request[512];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path_start, ip_addr);
    if (send(sock, request, strlen(request), 0) < 0) {
        print_line_in_box(win, LINES - 3, 2, "Error: Failed to send request."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 3. Open output file
    FILE *f = fopen(filename, "wb");
    if (!f) {
        print_line_in_box(win, LINES - 3, 2, "Error: Failed to open file for writing."); wrefresh(win); sleep(2);
        close(sock);
        return -1;
    }

    // 4. Receive data in a non-blocking loop
    // The server handles the duration via the duration= parameter in the URL.
    // We wait for the server to close the connection, with a safety timeout.
    fcntl(sock, F_SETFL, O_NONBLOCK);
    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    long elapsed_ms = 0;
    const long safety_timeout_ms = 45000; // Safety timeout in case server doesn't close
    const long expected_duration_ms = 30000; // Expected duration for UI display
    bool headers_processed = false;
    char buffer[65536]; // Increased buffer size
    
    while (elapsed_ms < safety_timeout_ms) {
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 + (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
        long remaining_s = (expected_duration_ms - elapsed_ms) / 1000;
        if (remaining_s < 0) remaining_s = 0;

        // Update UI
        draw_status_pane(win, hd, tuner_info, 0);
        mvwhline(win, LINES - 5, 1, ' ', getmaxx(win) - 2);
        mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
        mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
        
        if (debug_enabled) {
            print_line_in_box(win, LINES - 5, 2, "URL: %s", url);
        }
        if (remaining_s > 0) {
            print_line_in_box(win, LINES - 4, 2, "Saving to %s... %lds remaining.", filename, remaining_s);
        } else {
            print_line_in_box(win, LINES - 4, 2, "Saving to %s... finishing.", filename);
        }
        if (autorestart_enabled) {
            if (max_save_attempts > 0) {
                print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop. (Attempt %d/%d)", save_attempts, max_save_attempts);
            } else {
                print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop. (Attempt %d)", save_attempts);
            }
        } else {
            print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop.");
        }
        wrefresh(win);

        // Check for user abort
        int ch = getch();
        if (ch == KEY_BACKSPACE) {
            *out_aborted = true;
            break;
        }

        // Check for signal errors if autorestart is on (only during expected capture time)
        if (autorestart_enabled && (elapsed_ms >= 2000) && (elapsed_ms < expected_duration_ms)) {
            struct hdhomerun_tuner_status_t current_status;
            char *current_raw_status;
            if (hdhomerun_device_get_tuner_status(hd, &current_raw_status, &current_status) > 0) {
                if (current_status.symbol_error_quality < 100) {
                    *out_error_detected = true;
                    break;
                }
            }
        }

        // Receive data from socket
        int bytes_read = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            char *data_to_write = buffer;
            int len_to_write = bytes_read;
            if (!headers_processed) {
                char *body_start = strstr(buffer, "\r\n\r\n");
                if (body_start) {
                    body_start += 4; // Move pointer past the CRLFCRLF
                    len_to_write = bytes_read - (body_start - buffer);
                    data_to_write = body_start;
                    headers_processed = true;
                } else {
                    // Headers not fully received in this chunk, so write nothing yet
                    len_to_write = 0;
                }
            }
            if (len_to_write > 0) {
                fwrite(data_to_write, 1, len_to_write, f);
            }
        } else if (bytes_read == 0) {
            // Connection closed by server, successful completion
            break;
        } else { // bytes_read < 0
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                // A real error occurred
                break;
            }
            // No data right now, just loop again
        }
    }

    fclose(f);
    close(sock);
    return 0;
}


/*
 * save_stream
 * Saves a 30-second transport stream capture to a file.
 */
char* save_stream(struct hdhomerun_device_t *hd, WINDOW *win, enum save_mode mode, struct unified_tuner *tuner_info, bool debug_enabled) {
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0) {
        print_line_in_box(win, LINES - 3, 2, "Failed to get tuner status."); wrefresh(win); sleep(2);
        return NULL;
    }

    char original_channel[128];
    strncpy(original_channel, status.channel, sizeof(original_channel));
    original_channel[sizeof(original_channel) - 1] = '\0';

    if (strstr(status.lock_str, "none") != NULL) {
        print_line_in_box(win, LINES - 3, 2, "No signal lock. Cannot save stream."); wrefresh(win); sleep(2);
        return NULL;
    }

    bool is_pcap = (mode == SAVE_NORMAL_PCAP || mode == SAVE_AUTORESTART_PCAP);
    if (is_pcap) {
        if (parse_db_value(raw_status_str, "ss=") == -999) {
            print_line_in_box(win, LINES - 3, 2, "PCAP capture not available on this device model."); wrefresh(win); sleep(2);
            return NULL;
        }
    }

    unsigned int rf_channel = 0;
    char *p = strchr(status.channel, ':');
    if (!p) p = status.channel; else p++;
    if (isdigit((unsigned char)*p)) rf_channel = strtoul(p, NULL, 10);

    long id_val = 0;
    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        long parsed_id = parse_status_value(streaminfo, "tsid=");
        if (parsed_id != -999) id_val = parsed_id;
    }
    
    bool is_atsc3 = (strstr(status.lock_str, "atsc3") != NULL);
    if (is_atsc3) {
        char *plpinfo;
        if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
            long bsid = parse_status_value(plpinfo, "bsid=");
            if (bsid != -999) id_val = bsid;
        }
    }

    bool autorestart_enabled = (mode == SAVE_AUTORESTART_TS || mode == SAVE_AUTORESTART_DBG || mode == SAVE_AUTORESTART_PCAP);
    char *result_str = NULL;

    // --- ATSC 3.0 Capture Logic ---
    if (is_atsc3) {
        int save_attempts = 0;
        
        // Parse requested PLPs from channel string (e.g., "atsc3:33:0+16" -> PLPs 0 and 16)
        int requested_plps[MAX_PLPS];
        int requested_plp_count = 0;
        char *second_colon = strchr(status.channel, ':');
        if (second_colon) second_colon = strchr(second_colon + 1, ':');
        if (second_colon) {
            char plp_part[64];
            strncpy(plp_part, second_colon + 1, sizeof(plp_part) - 1);
            plp_part[sizeof(plp_part) - 1] = '\0';
            // Parse PLPs separated by '+' (e.g., "0+16")
            char *token = strtok(plp_part, "+");
            while (token != NULL && requested_plp_count < MAX_PLPS) {
                requested_plps[requested_plp_count++] = atoi(token);
                token = strtok(NULL, "+");
            }
        }
        
        while(1) { // Loop to allow for auto-restarting
            const char *format = is_pcap ? "alp-pcap" : "dbg";
            const char *ext = is_pcap ? ".pcap" : ".dbg";
            
            char plp_str[128] = {0};
            bool plps_locked = false;
            for (int retry = 0; retry < 4; retry++) { // Initial attempt + 3 retries
                plp_str[0] = '\0'; // Reset for each retry
                int locked_count = 0;
                bool plp_locked[MAX_PLPS] = {false};
                
                char *plpinfo;
                if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
                    char *plpinfo_copy = strdup(plpinfo);
                    if (plpinfo_copy) {
                        // Parse line by line to check lock status
                        char *line = strtok(plpinfo_copy, "\n");
                        while(line != NULL) {
                            int plp_id;
                            if (sscanf(line, "%d:", &plp_id) == 1) {
                                bool is_locked = (strstr(line, "lock=1") != NULL);
                                
                                if (requested_plp_count > 0) {
                                    // Check if this is one of our requested PLPs
                                    for (int i = 0; i < requested_plp_count; i++) {
                                        if (plp_id == requested_plps[i] && is_locked && !plp_locked[i]) {
                                            plp_locked[i] = true;
                                            char temp[8];
                                            sprintf(temp, "p%d", plp_id);
                                            strcat(plp_str, temp);
                                            locked_count++;
                                        }
                                    }
                                } else {
                                    // No specific PLPs requested, collect any locked PLPs
                                    if (is_locked) {
                                        char temp[8];
                                        sprintf(temp, "p%d", plp_id);
                                        strcat(plp_str, temp);
                                        locked_count++;
                                    }
                                }
                            }
                            line = strtok(NULL, "\n");
                        }
                        free(plpinfo_copy);
                    }
                }
                
                // Check if we have what we need
                if (requested_plp_count > 0) {
                    if (locked_count == requested_plp_count) {
                        plps_locked = true;
                    }
                } else {
                    if (locked_count > 0) {
                        plps_locked = true;
                    }
                }
                
                if (plps_locked) {
                    break; // Success - all requested PLPs are locked
                }
                
                if (retry < 3) {
                    mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                    mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
                    if (requested_plp_count > 0) {
                        print_line_in_box(win, LINES - 4, 2, "Waiting for all PLPs to lock (%d/%d)... (%d/3)", locked_count, requested_plp_count, retry + 1);
                    } else {
                        print_line_in_box(win, LINES - 4, 2, "Could not lock PLPs, retrying... (%d/3)", retry + 1);
                    }
                    wrefresh(win);
                    sleep(1);
                }
            }

            if (!plps_locked) {
                mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                if (requested_plp_count > 0) {
                    print_line_in_box(win, LINES - 3, 2, "Could not lock all requested PLPs. Aborting capture."); wrefresh(win); sleep(2);
                } else {
                    print_line_in_box(win, LINES - 3, 2, "No locked PLPs found for ATSC 3.0 capture."); wrefresh(win); sleep(2);
                }
                goto restore_and_exit;
            }

            save_attempts++;

            char filename[256];
            char time_str[20];
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
            sprintf(filename, "rf%u-bsid%ld-%s-%s%s", rf_channel, id_val, plp_str, time_str, ext);
            
            char url[256];
            sprintf(url, "http://%s:5004/tuner%d/ch%u%s?format=%s&duration=30", tuner_info->ip_str, tuner_info->tuner_index, rf_channel, plp_str, format);
            
            bool aborted = false;
            bool error_detected = false;

            if (!error_detected) {
                save_atsc3_details_auto(hd, tuner_info->tuner_index, filename);
            }
            
            // Call the native HTTP download function instead of fork/wget
            // Pass 0 for max_save_attempts to indicate unlimited retries (like ATSC 1.0)
            http_save_stream(tuner_info->ip_str, url, filename, win, hd, tuner_info, autorestart_enabled, save_attempts, 0, &aborted, &error_detected, debug_enabled);

            if (autorestart_enabled && error_detected) {
                remove(filename);
                // Also remove details file if it was created
                char details_filename[512];
                const char* last_dot = strrchr(filename, '.');
                if (last_dot) {
                    size_t base_len = last_dot - filename;
                    snprintf(details_filename, sizeof(details_filename), "%.*s.txt", (int)base_len, filename);
                    remove(details_filename);
                }
                mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
                mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
                print_line_in_box(win, LINES - 4, 2, "Symbol Quality error. Restarting capture in 1s..."); wrefresh(win);
                
                napms(500);
                hdhomerun_device_set_tuner_channel(hd, original_channel);
                struct hdhomerun_tuner_status_t lock_status;
                hdhomerun_device_wait_for_lock(hd, &lock_status);
                
                sleep(1);
                continue; // Continue the while loop to retry
            }
            
            result_str = (char*)malloc(512);
            if (aborted) {
                sprintf(result_str, "Save aborted. Partial files %s may remain.", filename);
            } else {
                sprintf(result_str, "Saved capture to %s\nDetails saved to %.*s.txt", 
                        filename, (int)(strrchr(filename, '.') - filename), filename);
            }
            break; // Exit the while loop
        }
        goto restore_and_exit; // Jump to restoration code
    }

    // --- ATSC 1.0 Capture Logic ---
    while(1) {
        char filename[128];
        char time_str[20];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
        sprintf(filename, "rf%u-tsid%ld-%s.ts", rf_channel, id_val, time_str);

        print_line_in_box(win, LINES - 4, 2, "Starting capture..."); wrefresh(win);

        char debug_path[64];
        sprintf(debug_path, "/tuner%d/debug", tuner_info->tuner_index);
        char *debug_str;
        hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
        long start_te = parse_status_value(debug_str, "te=");
        long start_ne = parse_status_value(debug_str, "ne=");
        long start_se = parse_status_value(debug_str, "se=");

        if (hdhomerun_device_stream_start(hd) <= 0) {
            print_line_in_box(win, LINES - 3, 2, "Failed to start stream."); wrefresh(win); sleep(2);
            return NULL;
        }

        FILE *f = fopen(filename, "wb");
        if (!f) {
            hdhomerun_device_stream_stop(hd);
            print_line_in_box(win, LINES - 3, 2, "Failed to open file for writing."); wrefresh(win); sleep(2);
            return NULL;
        }

        struct timespec start_time, current_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        long elapsed_ms = 0;
        bool error_detected = false;
        bool aborted = false;
        unsigned long long total_bytes = 0;

        while(elapsed_ms < 30000) {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 + (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
            long remaining_s = (30000 - elapsed_ms) / 1000;
            if (remaining_s < 0) remaining_s = 0;
            
            draw_status_pane(win, hd, tuner_info, 0);

            mvwhline(win, LINES - 4, 1, ' ', getmaxx(win) - 2);
            mvwhline(win, LINES - 3, 1, ' ', getmaxx(win) - 2);
            print_line_in_box(win, LINES - 4, 2, "Saving to %s... %lds remaining.", filename, remaining_s);
            print_line_in_box(win, LINES - 3, 2, "Press Backspace to stop.");
            wrefresh(win);

            size_t actual_size;
            uint8_t *video_data = hdhomerun_device_stream_recv(hd, VIDEO_DATA_BUFFER_SIZE_1S, &actual_size);

            if (video_data && actual_size > 0) {
                fwrite(video_data, 1, actual_size, f);
                total_bytes += actual_size;
            }

            if (autorestart_enabled) {
                hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
                long current_te = parse_status_value(debug_str, "te=");
                long current_ne = parse_status_value(debug_str, "ne=");
                long current_se = parse_status_value(debug_str, "se=");

                if (current_te > start_te || current_ne > start_ne || current_se > start_se) {
                    error_detected = true;
                    break;
                }
            }
            if (getch() == KEY_BACKSPACE) {
                aborted = true;
                break;
            }
        }

        fclose(f);
        hdhomerun_device_stream_stop(hd);
        
        if (aborted) {
            result_str = (char*)malloc(512);
            sprintf(result_str, "Save aborted. Partial file %s may remain.", filename);
            
            // ATSC 1.0 doesn't need restoration - tuner continues running
            return result_str;
        }

        hdhomerun_device_get_var(hd, debug_path, &debug_str, NULL);
        long end_te = parse_status_value(debug_str, "te=");
        long end_ne = parse_status_value(debug_str, "ne=");
        long end_se = parse_status_value(debug_str, "se=");

        if (autorestart_enabled && error_detected) {
            remove(filename);
            print_line_in_box(win, LINES - 4, 2, "Error detected. Restarting capture in 1s..."); wrefresh(win);
            sleep(1);
            continue;
        }
        
        result_str = (char*)malloc(512);
        sprintf(result_str, "Saved %.2f MB to %s\nErrors: %ld transport, %ld network, %ld sequence", 
            (double)total_bytes / (1024*1024), filename,
            end_te - start_te, end_ne - start_ne, end_se - start_se);
        
        // ATSC 1.0 doesn't need restoration - tuner continues running
        return result_str;
    }

    restore_and_exit:
        // Common restoration point for ATSC 3.0
        // Give the tuner a moment to settle after the capture operation
        napms(500); // Half second delay
        
        hdhomerun_device_set_tuner_channel(hd, original_channel);
        struct hdhomerun_tuner_status_t lock_status;
        hdhomerun_device_wait_for_lock(hd, &lock_status);
        
        return result_str; // May be NULL if there was an early error
    }

/*
 * get_udp_port
 * Finds a free ephemeral UDP port for streaming.
 * Returns the port number on success, -1 on failure.
 */
int get_udp_port() {
    struct sockaddr_in sin;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY); 
    sin.sin_port = 0; // Ask OS for a free port
    
    if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(sock);
        return -1;
    }

    socklen_t len = sizeof(sin);
    if (getsockname(sock, (struct sockaddr *)&sin, &len) < 0) {
        close(sock);
        return -1;
    }

    int port = ntohs(sin.sin_port);
    close(sock);
    return port;
}


/*
 * select_program_menu
 * Displays a menu of available programs for the user to select.
 * Returns 1 on selection, 0 on cancellation.
 */
int select_program_menu(WINDOW *win, char *streaminfo_str, char *selected_program_str, int *selected_plp) {
    *selected_plp = -1;
    struct program_info {
        char display_str[256];
        char program_num_str[16];
        int plp;
    } programs[MAX_PROGRAMS];
    int program_count = 0;

    char *streaminfo_copy = strdup(streaminfo_str);
    if (!streaminfo_copy) return 0;

    char *line = strtok(streaminfo_copy, "\n");
    while (line != NULL && program_count < MAX_PROGRAMS) {
        while (isspace((unsigned char)*line)) line++;

        // Check for ATSC 3.0 format: "program=..."
        if (strncmp(line, "program=", 8) == 0) {
            strncpy(programs[program_count].display_str, line, sizeof(programs[0].display_str) - 1);
            sscanf(line, "program=%s", programs[program_count].program_num_str);
            char* plp_start = strstr(line, "plp=");
            if (plp_start) {
                sscanf(plp_start, "plp=%d", &programs[program_count].plp);
            } else {
                programs[program_count].plp = -1;
            }
            program_count++;
        } 
        // Check for ATSC 1.0 format: "program [num]: ..." or "[num]: ..."
        else if (strncmp(line, "program ", 8) == 0 || (isdigit((unsigned char)*line) && strchr(line, ':'))) {
            strncpy(programs[program_count].display_str, line, sizeof(programs[0].display_str) - 1);
            if (strncmp(line, "program ", 8) == 0) {
                sscanf(line, "program %s", programs[program_count].program_num_str);
            } else {
                 sscanf(line, "%s", programs[program_count].program_num_str);
            }
            char *colon = strchr(programs[program_count].program_num_str, ':');
            if (colon) *colon = '\0';
            programs[program_count].plp = -1;
            program_count++;
        }
        line = strtok(NULL, "\n");
    }
    free(streaminfo_copy);

    if (program_count == 0) return 0;

    int highlight = 0;
    int choice = -1;
    nodelay(stdscr, FALSE);

    while(choice == -1) {
        wclear(win);
        box(win, 0, 0);
        mvwprintw(win, 0, 2, " Select Program to View ");
        for (int i = 0; i < program_count; i++) {
            if (i + 2 >= getmaxy(win) - 2) break;
            if (i == highlight) wattron(win, A_REVERSE);
            mvwprintw(win, i + 2, 4, "%s", programs[i].display_str);
            if (i == highlight) wattroff(win, A_REVERSE);
        }
        mvwprintw(win, getmaxy(win) - 2, 2, "Select: Up/Down/Enter | Cancel: q");
        wrefresh(win);

        int key = getch();
        switch(key) {
            case KEY_UP:
                if (highlight > 0) highlight--;
                break;
            case KEY_DOWN:
                if (highlight < program_count - 1) highlight++;
                break;
            case '\n':
            case '\r':
                choice = highlight;
                break;
            case 'q':
                choice = -2; // Cancel
                break;
        }
    }

    nodelay(stdscr, TRUE);

    if (choice >= 0) {
        strncpy(selected_program_str, programs[choice].program_num_str, 15);
        selected_program_str[15] = '\0';
        *selected_plp = programs[choice].plp;
    }

    return (choice >= 0);
}

/*
 * stream_to_vlc
 * Manages the process of starting and stopping a video stream to VLC.
 */
char* stream_to_vlc(struct hdhomerun_device_t *hd, WINDOW *win, pid_t *vlc_pid, struct unified_tuner *tuner_info) {
    char tuner_target_path[64];
    sprintf(tuner_target_path, "/tuner%d/target", tuner_info->tuner_index);

    // If VLC is already running, stop it.
    if (*vlc_pid > 0) {
        kill(*vlc_pid, SIGTERM);
        waitpid(*vlc_pid, NULL, 0);
        hdhomerun_device_set_var(hd, tuner_target_path, "none", NULL, NULL);
        *vlc_pid = 0;
        return strdup("VLC stream stopped.");
    }

    // Check for signal lock before starting
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0 || strstr(status.lock_str, "none") != NULL) {
        return strdup("No signal lock. Cannot start stream.");
    }
    
    // Get the list of available programs
    char *streaminfo_str;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_str) <= 0) {
        return strdup("Failed to get program list.");
    }

    // Show menu for user to select a program
    char selected_program[16] = {0};
    int selected_plp = -1;
    if (!select_program_menu(win, streaminfo_str, selected_program, &selected_plp)) {
        return NULL; // User cancelled the menu
    }
    
    // Set the tuner to the selected program
    char tuner_program_path[64];
    sprintf(tuner_program_path, "/tuner%d/program", tuner_info->tuner_index);
    if (hdhomerun_device_set_var(hd, tuner_program_path, selected_program, NULL, NULL) < 0) {
        char err_msg[128];
        sprintf(err_msg, "Failed to set program to %s.", selected_program);
        return strdup(err_msg);
    }

    // Get a free port for the RTP stream
    int port = get_udp_port();
    if (port < 0) {
        return strdup("Could not find a free UDP port.");
    }

    // Dynamically determine the local IP address that connects to the HDHomeRun
    char local_ip[INET_ADDRSTRLEN];
    int temp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (temp_sock < 0) { return strdup("Socket error determining local IP."); }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(65001); // Connect to a port on the HDHomeRun to find the interface
    if (inet_pton(AF_INET, tuner_info->ip_str, &serv.sin_addr) <= 0) {
        close(temp_sock);
        return strdup("Invalid device IP address.");
    }

    if (connect(temp_sock, (const struct sockaddr *)&serv, sizeof(serv)) < 0) {
        close(temp_sock);
        return strdup("Connect error determining local IP.");
    }

    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(temp_sock, (struct sockaddr *)&name, &namelen) < 0) {
        close(temp_sock);
        return strdup("getsockname error determining local IP.");
    }
    close(temp_sock);

    if (inet_ntop(AF_INET, &name.sin_addr, local_ip, sizeof(local_ip)) == NULL) {
        return strdup("inet_ntop error determining local IP.");
    }

    // Set the stream target on the tuner
    char target_str[128];
    sprintf(target_str, "rtp://%s:%d", local_ip, port);
    if (hdhomerun_device_set_var(hd, tuner_target_path, target_str, NULL, NULL) < 0) {
        return strdup("Failed to set stream target.");
    }

    // Fork and execute VLC to listen for the stream
    *vlc_pid = fork();
    if (*vlc_pid == 0) { // Child process for VLC
        // Redirect stdout and stderr to /dev/null to prevent TUI corruption
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null != -1) {
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
        
        char vlc_url[128];
        sprintf(vlc_url, "rtp://@:%d", port);
        execlp("vlc", "vlc", vlc_url, NULL);
        // If execlp fails, print error and exit child process
        perror("execlp for vlc failed");
        _exit(1);
    } else if (*vlc_pid < 0) {
        return strdup("Failed to fork for VLC.");
    }

    return strdup("Streaming to VLC...");
}

/*
 * main_loop
 * The primary application loop for the unified UI.
 */
int main_loop() {
    struct unified_tuner tuners[MAX_TUNERS_TOTAL];
    int total_tuners = 0;
    int highlight = 0;
    
    struct hdhomerun_device_t *hd = NULL;
    uint32_t current_device_id = 0;

    struct channel_list chan_list;
    chan_list.count = 0;
    
    static char *persistent_message = NULL;
    static int status_scroll_offset = 0;
    int total_content_lines = 0;

    // State for VLC piping
    static pid_t vlc_pid = 0;
    // State for mouse tracking (enabled by default to block scroll wheel)
    static bool mouse_tracking_enabled = true;
    // State for debug mode
    static bool debug_mode_enabled = false;

    WINDOW *tuner_win = newwin(LINES, LEFT_PANE_WIDTH, 0, 0);
    WINDOW *status_win = newwin(LINES, COLS - LEFT_PANE_WIDTH, 0, LEFT_PANE_WIDTH);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    // If an initial IP was provided via -i, use it instead of discovery
    if (g_initial_ip != NULL) {
        clear();
        mvprintw(0, 0, "Connecting to %s...", g_initial_ip);
        refresh();
        total_tuners = add_device_by_ip(g_initial_ip, tuners, &total_tuners);
        g_initial_ip = NULL; // Clear so subsequent refreshes use discovery
        clear();
        refresh();
    } else {
        total_tuners = discover_and_build_tuner_list(tuners);
    }
    if (total_tuners == 0) {
        delwin(tuner_win); delwin(status_win);
        return 1;
    }

    while (1) {
        bool tuner_changed = false;
        struct unified_tuner *selected_tuner = (total_tuners > 0) ? &tuners[highlight] : NULL;

        if (selected_tuner) {
            if (hd == NULL || current_device_id != selected_tuner->device_id) {
                if (hd) hdhomerun_device_destroy(hd);
                // Use IP address to connect - more reliable than device ID lookup
                // which requires broadcast discovery
                hd = hdhomerun_device_create_from_str(selected_tuner->ip_str, NULL);
                current_device_id = selected_tuner->device_id;
                status_scroll_offset = 0;
                tuner_changed = true;
            }
            if (hd) {
                hdhomerun_device_set_tuner(hd, selected_tuner->tuner_index);
                if (chan_list.count == 0 || tuner_changed) {
                    populate_channel_list(hd, &chan_list);
                    status_scroll_offset = 0;
                }
            }
        }
        
        // If tuner changed while VLC was running, stop it.
        if (tuner_changed && vlc_pid > 0) {
            kill(vlc_pid, SIGTERM); waitpid(vlc_pid, NULL, 0); vlc_pid = 0;
            hdhomerun_device_set_tuner_target(hd, "none");
            if (persistent_message) free(persistent_message);
            persistent_message = strdup("VLC stopped due to tuner change.");
        }


        werase(tuner_win);
        box(tuner_win, 0, 0);
        for (int i = 0; i < total_tuners; i++) {
            if (i + 2 >= LINES) break;
            if (i == highlight) wattron(tuner_win, A_REVERSE);
            mvwprintw(tuner_win, i + 1, 2, "%08X-%d", tuners[i].device_id, tuners[i].tuner_index);
            if (i == highlight) wattroff(tuner_win, A_REVERSE);
        }
        mvwprintw(tuner_win, LINES - 2, 2, "r: Refresh");
        
        total_content_lines = draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
        
        // Check if current tuner is ATSC3 to adjust hint text
        bool is_atsc3 = false;
        if(hd) {
            struct hdhomerun_tuner_status_t current_status;
            char *raw_status;
            if(hdhomerun_device_get_tuner_status(hd, &raw_status, &current_status) > 0) {
                if(strstr(current_status.lock_str, "atsc3")) {
                    is_atsc3 = true;
                }
            }
        }

        if (persistent_message) {
            char *line1 = persistent_message;
            char *line2 = strchr(persistent_message, '\n');
            if (line2) {
                *line2 = '\0'; 
                line2++;     
            }
            wattron(status_win, A_REVERSE);
            print_line_in_box(status_win, LINES - 4, 2, "%s", line1);
            if (line2) {
                print_line_in_box(status_win, LINES - 3, 2, "%s", line2);
                *(line2 - 1) = '\n'; 
            }
            print_line_in_box(status_win, LINES - 2, 2, "Press Enter to dismiss...");
            wattroff(status_win, A_REVERSE);
        } else {
            if (vlc_pid > 0) {
                 mvwprintw(status_win, LINES - 2, 2, "v: Stop VLC | h: Help | q: Quit");
            } else if (total_content_lines > LINES - 4) {
                 mvwprintw(status_win, LINES - 2, 2, "PgUp/PgDn: Scroll | v: View | h: Help | q: Quit");
            } else {
                 if(is_atsc3) {
                    mvwprintw(status_win, LINES - 2, 2, "v: View | <-/->: Ch | h: Help | q: Quit");
                 } else {
                    mvwprintw(status_win, LINES - 2, 2, "v: View | <-/->: Ch | +/-: Seek | h: Help | q: Quit");
                 }
            }
        }

        wrefresh(tuner_win);
        wrefresh(status_win);

        int ch = getch();

        if (ch == KEY_MOUSE) {
            // Discard all mouse events (primarily scroll wheel) to prevent
            // accidental input. We capture them just so the terminal doesn't
            // translate them into arrow keys.
            MEVENT event;
            getmouse(&event); // Consume the event
            continue;
        }

        if (persistent_message && ch != ERR) {
            free(persistent_message);
            persistent_message = NULL;
            wmove(status_win, LINES - 4, 2); wclrtoeol(status_win);
            wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
            if (ch == '\n' || ch == '\r') continue;
        }

        if (isdigit(ch)) { ungetch(ch); ch = 'c'; }

        switch(ch) {
            case 'q':
                if (vlc_pid > 0) { 
                    kill(vlc_pid, SIGTERM); 
                    waitpid(vlc_pid, NULL, 0); 
                    hdhomerun_device_set_tuner_target(hd, "none");
                }
                if (hd) hdhomerun_device_destroy(hd);
                delwin(tuner_win); delwin(status_win);
                return 0;

            case 'r':
                if (vlc_pid > 0) { 
                    kill(vlc_pid, SIGTERM); 
                    waitpid(vlc_pid, NULL, 0); 
                    vlc_pid = 0; 
                    hdhomerun_device_set_tuner_target(hd, "none");
                }
                if (hd) { hdhomerun_device_destroy(hd); hd = NULL; current_device_id = 0; }
                chan_list.count = 0; status_scroll_offset = 0;
                total_tuners = discover_and_build_tuner_list(tuners);
                highlight = 0;
                if (total_tuners == 0) {
                    delwin(tuner_win); delwin(status_win);
                    return 1;
                }
                break;

            case KEY_UP: if (highlight > 0) { highlight--; chan_list.count = 0; status_scroll_offset = 0; tuner_changed=true; } break;
            case KEY_DOWN: if (highlight < total_tuners - 1) { highlight++; chan_list.count = 0; status_scroll_offset = 0; tuner_changed=true; } break;
            
            case KEY_PPAGE: if (status_scroll_offset > 0) status_scroll_offset--; break;
            case KEY_NPAGE: if (status_scroll_offset < total_content_lines - (LINES - 4)) status_scroll_offset++; break;

            case 'd':
                if(hd && is_atsc3) {
                    if (show_plp_details_screen(status_win, hd, selected_tuner) == 1) { // Quit requested
                         if (hd) hdhomerun_device_destroy(hd);
                         delwin(tuner_win); delwin(status_win);
                         return 0;
                    }
                }
                break;

            case 'v':
                if (!hd) break;
                if (persistent_message) free(persistent_message);
                persistent_message = stream_to_vlc(hd, status_win, &vlc_pid, &tuners[highlight]);
                break;

            case '+':
            case '=':
            case '-':
            case '_':
                if (!hd) break;
                {
                    int seek_direction = (ch == '+' || ch == '=') ? 1 : -1;
                    unsigned int current_channel = 0;
                    struct hdhomerun_tuner_status_t status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &status) > 0) {
                        char *p = strchr(status.channel, ':');
                        if (!p) p = status.channel; else p++;
                        if (isdigit((unsigned char)*p)) current_channel = strtoul(p, NULL, 10);
                    }

                    int current_idx = -1;
                    if (chan_list.count > 0) {
                        for (int i = 0; i < chan_list.count; i++) {
                            if (chan_list.channels[i] == current_channel) {
                                current_idx = i;
                                break;
                            }
                        }
                    }
                    
                    if (current_idx == -1 && chan_list.count > 0) {
                        current_idx = (seek_direction == 1) ? -1 : chan_list.count;
                    }
                    
                    const unsigned int start_channel = current_channel;
                    bool first_iteration = true;

                    while(1) {
                        unsigned int new_channel;
                        if (chan_list.count > 0) {
                            current_idx = (current_idx + seek_direction + chan_list.count) % chan_list.count;
                            new_channel = chan_list.channels[current_idx];
                        } else {
                            if (current_channel == 0) {
                                current_channel = (seek_direction == 1) ? 1 : 70;
                            }
                            current_channel += seek_direction;
                            if (current_channel > 69) current_channel = 2;
                            if (current_channel < 2) current_channel = 69;
                            new_channel = current_channel;
                        }

                        if (!first_iteration && new_channel == start_channel) {
                            break;
                        }
                        first_iteration = false;

                        char tune_str[64];
                        sprintf(tune_str, "auto:%u", new_channel);
                        hdhomerun_device_set_tuner_channel(hd, tune_str);
                        status_scroll_offset = 0;
                        
                        wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
                        box(status_win, 0, 0);
                        print_line_in_box(status_win, LINES - 3, 2, "Seeking %s on ch %u...", (seek_direction == 1) ? "Up" : "Down", new_channel);
                        draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
                        mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                        wrefresh(status_win);

                        bool lock_found = false;
                        for (int i = 0; i < 25; i++) {
                            struct hdhomerun_tuner_status_t seek_status;
                            char *raw_status;
                            if (hdhomerun_device_get_tuner_status(hd, &raw_status, &seek_status) > 0) {
                                if (seek_status.signal_to_noise_quality > 0) {
                                    lock_found = true;
                                    break;
                                }
                            }
                            
                            draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
                            print_line_in_box(status_win, LINES - 3, 2, "Seeking %s on ch %u... (%2.1fs)", (seek_direction == 1) ? "Up" : "Down", new_channel, (25-i)/10.0);
                            mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                            wrefresh(status_win);
                            
                            napms(100);

                            int abort_ch = getch();
                            if (abort_ch != ERR) {
                                ungetch(abort_ch);
                                goto end_seek;
                            }
                        }

                        if (lock_found) {
                            break;
                        }
                    }
                end_seek:
                    wmove(status_win, LINES - 3, 2); wclrtoeol(status_win);
                    box(status_win, 0, 0);
                    draw_status_pane(status_win, hd, selected_tuner, status_scroll_offset);
                    mvwprintw(status_win, LINES - 2, 2, "<-/->: Ch | +/-: Seek | h: Help | q: Quit");
                    wrefresh(status_win);
                }
                break;

            case KEY_LEFT:
            case KEY_RIGHT:
                if (!hd) break;
                {
                    unsigned int current_channel = 0, new_channel = 0;
                    struct hdhomerun_tuner_status_t current_status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &current_status) > 0) {
                        char *p = strchr(current_status.channel, ':');
                        if (!p) p = current_status.channel; else p++;
                        if (isdigit((unsigned char)*p)) current_channel = strtoul(p, NULL, 10);
                    }

                    if (chan_list.count > 0) {
                        int idx = -1;
                        for (int i = 0; i < chan_list.count; i++) if (chan_list.channels[i] == current_channel) idx = i;
                        if (idx != -1) {
                            if (ch == KEY_RIGHT) idx = (idx + 1) % chan_list.count;
                            else idx = (idx - 1 + chan_list.count) % chan_list.count;
                            new_channel = chan_list.channels[idx];
                        } else {
                            if (ch == KEY_RIGHT) new_channel = chan_list.channels[0];
                            else new_channel = chan_list.channels[chan_list.count - 1];
                        }
                    } else {
                        if (current_channel > 0) {
                            if (ch == KEY_RIGHT) new_channel = (current_channel == 69) ? 2 : current_channel + 1;
                            else new_channel = (current_channel == 2) ? 69 : current_channel - 1;
                        } else {
                            if (ch == KEY_RIGHT) new_channel = 2; else new_channel = 69;
                        }
                    }
                    char tune_str[64];
                    sprintf(tune_str, "auto:%u", new_channel);
                    hdhomerun_device_set_tuner_channel(hd, tune_str);
                    status_scroll_offset = 0;
                }
                break;
            
            case 's':
            case 'a':
            case 'x':
            case 'z':
                if (!hd) break;
                {
                    struct hdhomerun_tuner_status_t status;
                    char *raw_status_str;
                    bool is_atsc3_save = false;
                    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
                        if (strstr(status.lock_str, "atsc3") != NULL) is_atsc3_save = true;
                    }

                    enum save_mode mode;
                    bool action_valid = true;
                    switch(ch) {
                        case 's': mode = is_atsc3_save ? SAVE_NORMAL_DBG : SAVE_NORMAL_TS; break;
                        case 'a': mode = is_atsc3_save ? SAVE_AUTORESTART_DBG : SAVE_AUTORESTART_TS; break;
                        case 'x': if (is_atsc3_save) { mode = SAVE_NORMAL_PCAP; } else { action_valid = false; } break;
                        case 'z': if (is_atsc3_save) { mode = SAVE_AUTORESTART_PCAP; } else { action_valid = false; } break;
                    }
                    
                    if (action_valid) {
                        persistent_message = save_stream(hd, status_win, mode, &tuners[highlight], debug_mode_enabled);
                    }
                }
                break;

            case 'c':
                 if (!hd) break;
                 {
                    char channel_str[20] = {0};
                    nodelay(stdscr, FALSE); echo();
                    wmove(status_win, LINES - 2, 2); wclrtoeol(status_win);
                    mvwprintw(status_win, LINES - 2, 2, "Enter Channel/Freq: "); wrefresh(status_win);
                    wgetnstr(status_win, channel_str, sizeof(channel_str) - 1);
                    noecho(); nodelay(stdscr, TRUE);

                    if (strlen(channel_str) > 0) {
                        char full_tune_str[100];
                        sprintf(full_tune_str, "auto:%s", channel_str);
                        hdhomerun_device_set_tuner_channel(hd, full_tune_str);
                        struct hdhomerun_tuner_status_t lock_status;
                        hdhomerun_device_wait_for_lock(hd, &lock_status);
                        status_scroll_offset = 0;
                    }
                 }
                 break;

            case 'h':
                if (show_help_screen(status_win) == 1) {
                    if (hd) hdhomerun_device_destroy(hd);
                    delwin(tuner_win); delwin(status_win);
                    return 0;
                }
                break;
            
            case 'w': // Toggle mouse tracking (disable to allow text selection)
                mouse_tracking_enabled = !mouse_tracking_enabled;
                if (mouse_tracking_enabled) {
                    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
                } else {
                    mousemask(0, NULL); // Disable to allow terminal text selection
                }
                break;

            case 'g': // Hidden toggle for debug mode
                debug_mode_enabled = !debug_mode_enabled;
                break;
                
            case 'i':
                add_manual_device(status_win, tuners, &total_tuners);
                // Force a refresh of the tuner window logic
                chan_list.count = 0; 
                status_scroll_offset = 0;
                break;

            case 'm': // Change channel map
                 if (!hd) break;
                 {
                    char *features_str;
                    if (hdhomerun_device_get_var(hd, "/sys/features", &features_str, NULL) <= 0) break;

                    char *features_copy = strdup(features_str);
                    char *map_line_start = strstr(features_copy, "channelmap:");
                    if (!map_line_start) { free(features_copy); break; }

                    char *map_line_end = strchr(map_line_start, '\n');
                    if (map_line_end) *map_line_end = '\0';

                    char *map_names[MAX_MAPS];
                    int map_count = 0;
                    
                    char *token = strtok(map_line_start, " ");
                    while (token != NULL && map_count < MAX_MAPS) {
                        if (strcmp(token, "channelmap:") != 0) {
                            map_names[map_count++] = strdup(token);
                        }
                        token = strtok(NULL, " ");
                    }
                    free(features_copy);

                    if (map_count > 0) {
                        int menu_start_y = 2;
                        wclear(status_win);
                        box(status_win, 0, 0);

                        char *current_map_str;
                        if (hdhomerun_device_get_tuner_channelmap(hd, &current_map_str) > 0) {
                            char *current_map_copy = strdup(current_map_str);
                            if(current_map_copy) {
                                char *current_map_token = strtok(current_map_copy, " ");
                                mvwprintw(status_win, menu_start_y, 2, "Current Map: %s", current_map_token);
                                free(current_map_copy);
                            }
                        }
                        
                        mvwprintw(status_win, menu_start_y + 2, 2, "Select New Map:");
                        for (int i = 0; i < map_count; i++) {
                            mvwprintw(status_win, menu_start_y + i + 4, 4, "%d: %s", i + 1, map_names[i]);
                        }
                        
                        char choice_str[5] = {0};
                        nodelay(stdscr, FALSE); echo();
                        mvwprintw(status_win, menu_start_y + map_count + 6, 2, "Enter number (or any other key to cancel): ");
                        wrefresh(status_win);
                        wgetnstr(status_win, choice_str, sizeof(choice_str) - 1);
                        noecho(); nodelay(stdscr, TRUE);

                        int choice = atoi(choice_str);
                        if (choice > 0 && choice <= map_count) {
                            hdhomerun_device_set_tuner_channelmap(hd, map_names[choice - 1]);
                            chan_list.count = 0;
                            status_scroll_offset = 0;
                        }

                        for (int i = 0; i < map_count; i++) free(map_names[i]);
                    }
                 }
                 break;

            case 'p':
                 if (!hd) break;
                 {
                    struct hdhomerun_tuner_status_t current_status;
                    char *s;
                    if (hdhomerun_device_get_tuner_status(hd, &s, &current_status) > 0 && strstr(current_status.lock_str, "atsc3")) {
                        char freq_buffer[20] = {0};
                        const char *start = strchr(current_status.channel, ':');
                        if (start) {
                            start++;
                            int i = 0;
                            while (isdigit((unsigned char)*start) && i < (int)sizeof(freq_buffer) - 1) freq_buffer[i++] = *start++;
                            freq_buffer[i] = '\0';
                        }
                        if (strlen(freq_buffer) > 0) {
                            char plp_str_in[20] = {0};
                            nodelay(stdscr, FALSE); echo();
                            wmove(status_win, LINES - 2, 2); wclrtoeol(status_win);
                            mvwprintw(status_win, LINES - 2, 2, "Enter PLPs (e.g. 0,1, Enter for all): "); wrefresh(status_win);
                            wgetnstr(status_win, plp_str_in, sizeof(plp_str_in) - 1);
                            noecho(); nodelay(stdscr, TRUE);

                            char plp_str_out[40] = {0};
                            if (strlen(plp_str_in) > 0) {
                                int j = 0;
                                for (int i = 0; plp_str_in[i] != '\0'; i++) {
                                    if (plp_str_in[i] == ',') plp_str_out[j++] = '+';
                                    else if (isdigit((unsigned char)plp_str_in[i])) plp_str_out[j++] = plp_str_in[i];
                                }
                                plp_str_out[j] = '\0';
                            } else {
                                char *plpinfo;
                                if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
                                    char *plpinfo_copy = strdup(plpinfo);
                                    if(plpinfo_copy) {
                                        char *line = strtok(plpinfo_copy, "\n");
                                        bool first_plp = true;
                                        while(line != NULL) {
                                            int plp_id;
                                            if (sscanf(line, "%d:", &plp_id) == 1) {
                                                if (!first_plp) strcat(plp_str_out, "+");
                                                char plp_id_str[5];
                                                sprintf(plp_id_str, "%d", plp_id);
                                                strcat(plp_str_out, plp_id_str);
                                                first_plp = false;
                                            }
                                            line = strtok(NULL, "\n");
                                        }
                                        free(plpinfo_copy);
                                    }
                                }
                            }
                            if (strlen(plp_str_out) > 0) {
                                char full_tune_str[100];
                                sprintf(full_tune_str, "atsc3:%s:%s", freq_buffer, plp_str_out);
                                hdhomerun_device_set_tuner_channel(hd, full_tune_str);
                                struct hdhomerun_tuner_status_t lock_status;
                                hdhomerun_device_wait_for_lock(hd, &lock_status);
                                status_scroll_offset = 0;
                            }
                        }
                    }
                 }
                 break;
        }
        
        // Check if VLC has exited
        if (vlc_pid > 0) {
            int status;
            if (waitpid(vlc_pid, &status, WNOHANG) == vlc_pid) {
                // VLC has exited. Clean up.
                hdhomerun_device_set_tuner_target(hd, "none");
                vlc_pid = 0;
                if (persistent_message) free(persistent_message);
                persistent_message = strdup("VLC has been closed.");
            }
        }


        // Apply conditional polling rate based on device type
        if (selected_tuner) {
            if (selected_tuner->is_legacy) {
                napms(500); // Slower polling for legacy devices
            } else {
                napms(100); // Faster polling for modern devices
            }
        } else {
            napms(100); // Default if no tuners
        }
    }
}

/*
 * add_manual_device
 * Prompts user for an IP, queries the device, and adds its tuners to the list.
 */
void add_manual_device(WINDOW *win, struct unified_tuner tuners[], int *total_tuners) {
    if (*total_tuners >= MAX_TUNERS_TOTAL) {
        mvwprintw(win, LINES - 3, 2, "Tuner list full. Cannot add more devices.");
        wrefresh(win);
        sleep(2);
        return;
    }

    char ip_str[64] = {0};
    nodelay(stdscr, FALSE); echo();
    wmove(win, LINES - 2, 2); wclrtoeol(win);
    mvwprintw(win, LINES - 2, 2, "Enter Device IP: "); wrefresh(win);
    wgetnstr(win, ip_str, sizeof(ip_str) - 1);
    noecho(); nodelay(stdscr, TRUE);

    if (strlen(ip_str) == 0) return;

    mvwprintw(win, LINES - 3, 2, "Connecting to %s...", ip_str);
    wrefresh(win);

    struct hdhomerun_device_t *hd = hdhomerun_device_create_from_str(ip_str, NULL);
    if (!hd) {
        mvwprintw(win, LINES - 3, 2, "Error: Invalid IP format or device not found.");
        wrefresh(win); sleep(2);
        return;
    }

    // Use get_var to fetch /sys/model. 
    // note: libhdhomerun manages the memory for model_str, we do NOT free it.
    char *model_str;
    if (hdhomerun_device_get_var(hd, "/sys/model", &model_str, NULL) <= 0) {
        mvwprintw(win, LINES - 3, 2, "Error: Device at %s did not respond.", ip_str);
        hdhomerun_device_destroy(hd);
        wrefresh(win); sleep(2);
        return;
    }

    uint32_t device_id = hdhomerun_device_get_device_id(hd);
    
    // Check for duplicates
    for (int i = 0; i < *total_tuners; i++) {
        if (tuners[i].device_id == device_id) {
            mvwprintw(win, LINES - 3, 2, "Device %08X is already in the list.", device_id);
            hdhomerun_device_destroy(hd);
            wrefresh(win); sleep(2);
            return;
        }
    }

    // Determine if legacy (heuristic based on model string)
    bool is_legacy = (strstr(model_str, "hdhomerun3") != NULL || strstr(model_str, "hdhomerun_") != NULL);
    
    // Count tuners by probing
    int device_tuner_count = 0;
    for (int i = 0; i < 8; i++) { // Max reasonable tuners to probe
        if (hdhomerun_device_set_tuner(hd, i) < 0) break;
        
        // Double check validity by asking for status
        struct hdhomerun_tuner_status_t status;
        char *status_str;
        if (hdhomerun_device_get_tuner_status(hd, &status_str, &status) <= 0) break;
        
        device_tuner_count++;
    }

    if (device_tuner_count > 0) {
        int added = 0;
        for (int i = 0; i < device_tuner_count && *total_tuners < MAX_TUNERS_TOTAL; i++) {
            tuners[*total_tuners].device_id = device_id;
            strcpy(tuners[*total_tuners].ip_str, ip_str);
            tuners[*total_tuners].tuner_index = i;
            tuners[*total_tuners].total_tuners_on_device = device_tuner_count;
            tuners[*total_tuners].is_legacy = is_legacy;
            (*total_tuners)++;
            added++;
        }
        mvwprintw(win, LINES - 3, 2, "Added device %08X with %d tuners.", device_id, added);
    } else {
        mvwprintw(win, LINES - 3, 2, "No accessible tuners found on device.");
    }

    hdhomerun_device_destroy(hd);
    wrefresh(win);
    sleep(1);
}

/*
 * main
 * Entry point of the application.
 */
int main(int argc, char *argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            g_initial_ip = argv[i + 1];
            i++; // Skip the IP argument
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("HDHomeRun TUI v%s\n", TUI_VERSION);
            printf("Usage: %s [-i <ip_address>]\n", argv[0]);
            printf("  -i <ip>  : Connect directly to device at IP, skip discovery\n");
            printf("  -h       : Show this help message\n");
            return 0;
        }
    }

    initscr();
    clear();
    noecho();
    cbreak();
    curs_set(0);
    start_color();
    
    // Enable keypad for function keys, etc.
    keypad(stdscr, TRUE);

    // Capture scroll wheel events so they don't get translated to arrow keys
    // by the terminal. We'll discard them in the event handler to prevent
    // accidental tuner/channel changes from scroll wheel input.
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);

    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);

    while (1) {
        int result = main_loop();
        if (result == 0) { // Quit
            break;
        }
        // If result is 1 (no devices), show message and wait for input
        clear();
        mvprintw(LINES / 2, (COLS - 28) / 2, "No HDHomeRun devices found.");
        mvprintw(LINES / 2 + 2, (COLS - 40) / 2, "Press 'r' to refresh, or 'q' to quit.");
        refresh();
        nodelay(stdscr, FALSE);
        int ch = getch();
        if (ch != 'r' && ch != 'R') {
            break;
        }
    }

    endwin();
    return 0;
}

/*
 * Modified show_plp_details_screen function - much simpler now
 */
int show_plp_details_screen(WINDOW *parent_win, struct hdhomerun_device_t *hd, struct unified_tuner *tuner_info) {
    // Create detail info structure
    struct l1_detail_info* detail_info = create_l1_detail_info(MAX_DISPLAY_LINES);
    if (!detail_info) return 0;
    
    // Collect the details using the abstracted function
    if (collect_atsc3_details(hd, tuner_info->tuner_index, detail_info) != 0) {
        free_l1_detail_info(detail_info);
        return 0;
    }

    int parent_h, parent_w, parent_y, parent_x;
    getmaxyx(parent_win, parent_h, parent_w);
    getbegyx(parent_win, parent_y, parent_x);

    WINDOW *detail_win = newwin(parent_h, parent_w, parent_y, parent_x);
    
    int scroll_pos = 0;
    keypad(detail_win, TRUE);
    nodelay(stdscr, FALSE);
    char message[512] = {0};

    while(1) {
        werase(detail_win);
        box(detail_win, 0, 0);
        mvwprintw(detail_win, 0, 2, " ATSC 3.0 PLP & L1 Details ");

        int max_display_lines = getmaxy(detail_win) - 4;
        for (int i = 0; i < max_display_lines; i++) {
            if (scroll_pos + i < detail_info->line_count) {
                if (strcmp(detail_info->display_lines[scroll_pos + i], "__HLINE__") == 0) {
                    mvwhline(detail_win, i + 1, 2, ACS_HLINE, getmaxx(detail_win) - 4);
                } else {
                    mvwprintw(detail_win, i + 1, 2, "%s", detail_info->display_lines[scroll_pos + i]);
                }
            }
        }
        
        if (message[0] != '\0') {
            mvwprintw(detail_win, getmaxy(detail_win) - 2, 2, "%s", message);
        } else {
            mvwprintw(detail_win, getmaxy(detail_win) - 2, 2, "Scroll: Up/Dn | s: Save | d: Close | q: Quit");
        }
        wrefresh(detail_win);

        int ch = wgetch(detail_win);
        message[0] = '\0';

        switch(ch) {
            case KEY_UP: if (scroll_pos > 0) scroll_pos--; break;
            case KEY_DOWN: if (detail_info->line_count > max_display_lines && scroll_pos < detail_info->line_count - max_display_lines) scroll_pos++; break;
            case KEY_PPAGE: scroll_pos -= max_display_lines; if (scroll_pos < 0) scroll_pos = 0; break;
            case KEY_NPAGE:
                if (detail_info->line_count > max_display_lines) {
                    scroll_pos += max_display_lines;
                    if (scroll_pos > detail_info->line_count - max_display_lines) scroll_pos = detail_info->line_count - max_display_lines;
                }
                break;
            case 's':
                {
                    // Create manual save filename
                    unsigned int rf_channel = 0;
                    long id_val = -999;

                    char *status_str_s, *plpinfo_str_s, *streaminfo_str_s;
                    struct hdhomerun_tuner_status_t status_s;
                    
                    if (hdhomerun_device_get_tuner_status(hd, &status_str_s, &status_s) > 0) {
                        char *p = strchr(status_s.channel, ':');
                        if (!p) p = status_s.channel; else p++;
                        if (isdigit((unsigned char)*p)) rf_channel = strtoul(p, NULL, 10);
                    }
                    
                    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo_str_s) > 0) {
                        char *si_copy = strdup(streaminfo_str_s);
                        if (si_copy) {
                            id_val = parse_status_value_l1(si_copy, "tsid=");
                            free(si_copy);
                        }
                    }
                    if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str_s) > 0) {
                        char *pi_copy = strdup(plpinfo_str_s);
                        if(pi_copy) {
                            long bsid_s = parse_status_value_l1(pi_copy, "bsid=");
                            if (bsid_s != -999) id_val = bsid_s;
                            free(pi_copy);
                        }
                    }
                    if (id_val == -999) id_val = 0;

                    char filename[256];
                    char time_str[20];
                    time_t now = time(NULL);
                    struct tm *t = localtime(&now);
                    strftime(time_str, sizeof(time_str)-1, "%Y%m%d-%H%M%S", t);
                    sprintf(filename, "rf%u-bsid%ld-details-%s.txt", rf_channel, id_val, time_str);
                    
                    if (save_atsc3_details_to_file(filename, detail_info, NULL) == 0) {
                        snprintf(message, sizeof(message), "Saved details to %s", filename);
                    } else {
                        snprintf(message, sizeof(message), "Error: Could not save file.");
                    }
                }
                break;
            case 'q':
                delwin(detail_win);
                free_l1_detail_info(detail_info);
                return 1; // Quit requested
            case 'd':
            case '\n':
            case '\r':
                delwin(detail_win);
                free_l1_detail_info(detail_info);
                nodelay(stdscr, TRUE);
                return 0;
        }
    }
}
