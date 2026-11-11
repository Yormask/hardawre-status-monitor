// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hardware-stats - live monitor for CPU, GPU, memory, power, disks,
 * network and processes on Linux
 * Copyright (C) 2026  Yormask
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* ui.h - terminal interface and plain-text report */
#ifndef HWSTATS_UI_H
#define HWSTATS_UI_H

#include "collect.h"

enum { VIEW_DASH, VIEW_PROCS, VIEW_HW, VIEW_COUNT };
enum { SORT_CPU, SORT_MEM, SORT_IO, SORT_PID, SORT_NAME, SORT_TIME };

typedef struct {
    int view, sort, reverse, show_kthreads, paused, help, follow;
    double interval;
    char filter[64];
    int filter_edit;
    pid_t sel_pid;
    int sel_idx, sel_delta, scroll, page_rows;
    int hw_scroll, hw_max;
} ui_state_t;

void ui_init(int utf8);
void ui_end(void);
void ui_draw(sys_t *s, ui_state_t *u);
int ui_key(ui_state_t *u, int ch); /* returns 0 when the user wants to quit */

void report_print(sys_t *s, int nproc);

#endif
