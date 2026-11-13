/*
 * l1_details.h
 *
 * ATSC 3.0 details collection for the details screen and the .txt saved
 * with captures; works with any tuner backend. See l1_details.c.
 *
 * HDHomeRun TUI - Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef L1_DETAILS_H
#define L1_DETAILS_H

#include "l1_detail_parser.h"

struct tuner;

int collect_atsc3_details(struct tuner *t, struct l1_detail_info* detail_info);

int save_atsc3_details_auto(struct tuner *t, const char* base_filename);

#endif // L1_DETAILS_H
