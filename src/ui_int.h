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
/* ui_int.h - internal interface shared by the ui_*.c files */
#ifndef HSM_UI_INT_H
#define HSM_UI_INT_H

#include "ui.h"
#include <ncurses.h>

/* ------------------------------------------------------------ globals */
extern int U;                 /* UTF-8 drawing characters available */
extern const char *SEP;       /* " · " */
extern const char *DEG;       /* "°C" */
extern const layout_t *g_lay; /* active appearance + layout settings */

extern const char *const SORT_NAMES[];
extern const char *const VIEW_NAMES[];

/* colour pairs (the terminal's standard colours); use CP(), which also
 * handles monochrome mode. C_INFO = labels, C_IN / C_OUT = read+download /
 * write+upload, C_B* = bars, C_G* = graphs */
enum {
    C_TITLE = 1, C_BORDER, C_OK, C_WARN, C_CRIT, C_GRAY, C_HDR, C_THEAD, C_SEL, C_ACC, C_INFO, C_KEY,
    C_IN, C_OUT, C_BLO, C_BMID, C_BHI, C_TRACK, C_GLO, C_GMID, C_GHI, C_SINGLE
};
attr_t cp(int pair);
attr_t gray_attr(void);
void colors_sync(const layout_t *l); /* apply the chosen "Single colour" */
int gradient_on(void);
attr_t bar_attr(double pct);    /* bar colour for a 0-100 value  */
attr_t bar_temp_attr(double c); /* bar colour for a temperature   */
attr_t graph_attr(double pct);  /* graph colour for a 0-100 height */
#define CP(n) cp(n)
#define A_GRAY gray_attr()

/* --------------------------------------------------------- primitives */
int put(int y, int x, int maxw, attr_t a, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void app(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void fill(int y, int x, int w, attr_t a);
attr_t pct_attr(double p);
attr_t temp_attr(double t);
void box_(int y, int x, int h, int w, const char *title, const char *info);
void bar(int y, int x, int w, double pct, attr_t a);
void meter(int y, int x, int w, const char *label, int lw, double pct, const char *val, int vw, attr_t a);
int graph_span(int w); /* samples visible in a graph w cells wide */
void graph(int y, int x, int h, int w, const hist_t *hs, double maxv, attr_t fixed, int flip);
void kv(int y, int *x, int xmax, attr_t va, const char *key, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
void dual_graph(int y, int x, int h, int w, const hist_t *up, const hist_t *dn, attr_t ua, attr_t da, const char *ul,
                const char *dl);

/* -------------------------------------------------------------- views */
void dashboard(sys_t *s, ui_state_t *u);
void dashboard_preview(const layout_t *l, int y, int x, int h, int w, int hl);
void draw_procs(sys_t *s, ui_state_t *u, int y, int x, int h, int w, int full);
void hardware(sys_t *s, ui_state_t *u);
void setup_view(sys_t *s, ui_state_t *u);
int setup_key(ui_state_t *u, int ch); /* 1 when the key was used */
void log_view(sys_t *s, ui_state_t *u);
int log_view_key(sys_t *s, ui_state_t *u, int ch);
void log_toggle_system(sys_t *s, ui_state_t *u);  /* key l */
void log_toggle_process(sys_t *s, ui_state_t *u); /* key L */

#endif
