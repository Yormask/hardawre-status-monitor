// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hsm (hardware status monitor) - live monitor for CPU, GPU, memory,
 * power, disks, network and processes on Linux
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
/* ui.h - terminal interface, settings and plain-text report */
#ifndef HSM_UI_H
#define HSM_UI_H

#include "collect.h"

enum { VIEW_DASH, VIEW_PROCS, VIEW_HW, VIEW_SETUP, VIEW_COUNT };
enum { SORT_CPU, SORT_MEM, SORT_IO, SORT_PID, SORT_NAME, SORT_TIME, SORT_COUNT };

/* dashboard panels */
enum { P_CPU, P_GPU, P_MEM, P_POWER, P_DISK, P_NET, P_TEMPS, P_FANS, P_FS, P_COUNT };
/* appearance choices */
enum { BAR_BLOCKS, BAR_SQUARES, BAR_LINE, BAR_CLASSIC, BAR_COUNT };
enum { GRAPH_BLOCKS, GRAPH_BRAILLE, GRAPH_OFF, GRAPH_COUNT };
enum { COLOR_VALUE, COLOR_GRADIENT, COLOR_SINGLE, COLOR_MONO, COLOR_COUNT };
enum { CPUBAR_USAGE, CPUBAR_CLOCK, CPUBAR_TEMP, CPUBAR_COUNT };

extern const char *const PANEL_KEY[P_COUNT];  /* names used in the config file */
extern const char *const PANEL_NAME[P_COUNT]; /* names shown on screen */
extern const char *const BAR_NAME[BAR_COUNT];
extern const char *const GRAPH_NAME[GRAPH_COUNT];
extern const char *const COLOR_NAME[COLOR_COUNT];
extern const char *const CPUBAR_NAME[CPUBAR_COUNT];

typedef struct {
    int col[2][P_COUNT], ncol[2]; /* panel order in the left [0] / right [1] column */
    int on[P_COUNT];              /* shown on the dashboard */
    int show_procs;               /* process list below the panels */
    int left_pct;                 /* left column width in %, 0 = automatic */
    int bar_style, graph_style, color_mode, cpu_bars;
} layout_t;

typedef struct {
    int view, sort, reverse, show_kthreads, paused, help, follow;
    double interval;
    char filter[64];
    int filter_edit;

    /* process selection (kept up to date by the process table) */
    pid_t sel_pid;
    unsigned long long sel_start;
    int sel_kthread;
    char sel_desc[160], sel_user[32];
    int sel_idx, sel_delta, scroll, page_rows;

    int hw_scroll, hw_max;

    layout_t lay;
    int setup_sel;

    /* signal dialog */
    int kill_open, kill_sel, kill_kthread;
    pid_t kill_pid;
    unsigned long long kill_start;
    char kill_desc[160], kill_user[32];

    /* one-line status message in the footer */
    char msg[160];
    double msg_until;
    int msg_err;
} ui_state_t;

void ui_init(int utf8);
void ui_end(void);
void ui_draw(sys_t *s, ui_state_t *u);
int ui_key(ui_state_t *u, int ch); /* returns 0 when the user wants to quit */
void ui_message(ui_state_t *u, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* config.c */
void layout_defaults(layout_t *l);
void config_defaults(ui_state_t *u);
int config_load(ui_state_t *u); /* 0 = loaded, -1 = no file (defaults kept) */
int config_save(const ui_state_t *u);
int config_changed(const ui_state_t *u); /* differs from what was loaded / last saved */
const char *config_path(void);

void report_print(sys_t *s, int nproc);

#endif
