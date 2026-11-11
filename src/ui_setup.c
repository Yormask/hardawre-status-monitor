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
/* ui_setup.c - Setup screen: choose which panels the dashboard shows, put
 * them in order (and in the left or right column) and pick the look of
 * bars, graphs and colours. Every change is applied and saved at once. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { OPT_PROCS, OPT_BARS, OPT_GRAPHS, OPT_COLORS, OPT_CPUBARS, OPT_LEFTW, OPT_INTERVAL, OPT_KTHREADS, OPT_COUNT };
static const char *const OPT_LABEL[OPT_COUNT] = { "Process list",      "Bar style",        "Graphs",
                                                  "Colours",           "CPU core bars",    "Left column width",
                                                  "Refresh interval",  "Kernel threads" };
static const int LEFTW[] = { 0, 35, 40, 45, 50, 55, 60, 65, 70 };
static const double STEPS[] = { 0.25, 0.5, 1, 2, 3, 5, 10 };
#define NLEFTW ((int)(sizeof LEFTW / sizeof LEFTW[0]))
#define NSTEPS ((int)(sizeof STEPS / sizeof STEPS[0]))

/* every selectable row: the panels (left column, then right), then options */
typedef struct {
    int kind; /* 0 panel, 1 option */
    int id, col, pos;
} item_t;

static int build_items(const layout_t *l, item_t *it)
{
    int n = 0;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < l->ncol[c]; i++)
            it[n++] = (item_t){ 0, l->col[c][i], c, i };
    for (int o = 0; o < OPT_COUNT; o++)
        it[n++] = (item_t){ 1, o, -1, -1 };
    return n;
}

static int find_panel(const ui_state_t *u, int p)
{
    item_t it[P_COUNT + OPT_COUNT];
    int n = build_items(&u->lay, it);
    for (int i = 0; i < n; i++)
        if (it[i].kind == 0 && it[i].id == p)
            return i;
    return 0;
}

static void save(ui_state_t *u)
{
    if (config_save(u) != 0)
        ui_message(u, 1, "Could not save %s: %s", config_path(), strerror(errno));
}

/* ---------------------------------------------------------- options */

static void opt_value(const ui_state_t *u, int o, char *out, size_t n)
{
    const layout_t *l = &u->lay;
    switch (o) {
    case OPT_PROCS: str_copy(out, n, l->show_procs ? "Shown" : "Hidden"); break;
    case OPT_BARS: str_copy(out, n, BAR_NAME[l->bar_style]); break;
    case OPT_GRAPHS: str_copy(out, n, GRAPH_NAME[l->graph_style]); break;
    case OPT_COLORS: str_copy(out, n, COLOR_NAME[l->color_mode]); break;
    case OPT_CPUBARS: str_copy(out, n, CPUBAR_NAME[l->cpu_bars]); break;
    case OPT_LEFTW:
        if (l->left_pct)
            sfmt(out, n, "%d%%", l->left_pct);
        else
            str_copy(out, n, "Automatic");
        break;
    case OPT_INTERVAL: sfmt(out, n, "%g s", u->interval); break;
    case OPT_KTHREADS: str_copy(out, n, u->show_kthreads ? "Shown" : "Hidden"); break;
    default: out[0] = '\0';
    }
}

static int cyc(int v, int dir, int n)
{
    return ((v + dir) % n + n) % n;
}

static void opt_change(ui_state_t *u, int o, int dir)
{
    layout_t *l = &u->lay;
    int i;
    switch (o) {
    case OPT_PROCS: l->show_procs ^= 1; break;
    case OPT_BARS: l->bar_style = cyc(l->bar_style, dir, BAR_COUNT); break;
    case OPT_GRAPHS: l->graph_style = cyc(l->graph_style, dir, GRAPH_COUNT); break;
    case OPT_COLORS: l->color_mode = cyc(l->color_mode, dir, COLOR_COUNT); break;
    case OPT_CPUBARS: l->cpu_bars = cyc(l->cpu_bars, dir, CPUBAR_COUNT); break;
    case OPT_LEFTW:
        for (i = 0; i < NLEFTW && LEFTW[i] != l->left_pct; i++)
            ;
        l->left_pct = LEFTW[cyc(i < NLEFTW ? i : 0, dir, NLEFTW)];
        break;
    case OPT_INTERVAL:
        for (i = 0; i < NSTEPS - 1 && STEPS[i] < u->interval - 1e-9; i++)
            ;
        u->interval = STEPS[CLAMP(i + dir, 0, NSTEPS - 1)];
        break;
    case OPT_KTHREADS: u->show_kthreads ^= 1; break;
    }
    save(u);
}

/* ----------------------------------------------------------- panels */

/* Move panel at (c, i) one step up/down the combined list (left column
 * followed by right column), crossing between columns at the ends. */
static void panel_move(layout_t *l, int c, int i, int dir)
{
    int p = l->col[c][i];
    if (dir < 0 && i > 0) {
        l->col[c][i] = l->col[c][i - 1];
        l->col[c][i - 1] = p;
    } else if (dir > 0 && i < l->ncol[c] - 1) {
        l->col[c][i] = l->col[c][i + 1];
        l->col[c][i + 1] = p;
    } else if (dir < 0 && c == 1) { /* top of right column -> bottom of left */
        memmove(&l->col[1][0], &l->col[1][1], (size_t)(l->ncol[1] - 1) * sizeof(int));
        l->ncol[1]--;
        l->col[0][l->ncol[0]++] = p;
    } else if (dir > 0 && c == 0) { /* bottom of left column -> top of right */
        l->ncol[0]--;
        memmove(&l->col[1][1], &l->col[1][0], (size_t)l->ncol[1] * sizeof(int));
        l->col[1][0] = p;
        l->ncol[1]++;
    }
}

/* Move panel at (c, i) into the other column, at the same height. */
static void panel_to(layout_t *l, int c, int i, int nc)
{
    if (c == nc)
        return;
    int p = l->col[c][i];
    memmove(&l->col[c][i], &l->col[c][i + 1], (size_t)(l->ncol[c] - i - 1) * sizeof(int));
    l->ncol[c]--;
    int at = MIN(i, l->ncol[nc]);
    memmove(&l->col[nc][at + 1], &l->col[nc][at], (size_t)(l->ncol[nc] - at) * sizeof(int));
    l->col[nc][at] = p;
    l->ncol[nc]++;
}

int setup_key(ui_state_t *u, int ch)
{
    layout_t *l = &u->lay;
    item_t it[P_COUNT + OPT_COUNT];
    int n = build_items(l, it);
    u->setup_sel = CLAMP(u->setup_sel, 0, n - 1);
    item_t cur = it[u->setup_sel];

    switch (ch) {
    case KEY_UP: u->setup_sel = cyc(u->setup_sel, -1, n); return 1;
    case KEY_DOWN: u->setup_sel = cyc(u->setup_sel, 1, n); return 1;
    case KEY_HOME: u->setup_sel = 0; return 1;
    case KEY_END: u->setup_sel = n - 1; return 1;
    case 27: u->view = VIEW_DASH; return 1;
    case 'R': {
        double iv = u->interval;
        layout_defaults(l);
        u->interval = iv;
        save(u);
        ui_message(u, 0, "Dashboard layout and styles reset to defaults");
        return 1;
    }
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (cur.kind == 0) {
            l->on[cur.id] ^= 1;
            save(u);
        } else {
            opt_change(u, cur.id, 1);
        }
        return 1;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (cur.kind == 1) {
            opt_change(u, cur.id, ch == KEY_RIGHT ? 1 : -1);
        } else {
            panel_to(l, cur.col, cur.pos, ch == KEY_RIGHT ? 1 : 0);
            u->setup_sel = find_panel(u, cur.id);
            save(u);
        }
        return 1;
    case '[':
    case ']':
    case KEY_SR: /* shift + up   */
    case KEY_SF: /* shift + down */
        if (cur.kind == 0) {
            panel_move(l, cur.col, cur.pos, (ch == '[' || ch == KEY_SR) ? -1 : 1);
            u->setup_sel = find_panel(u, cur.id);
            save(u);
        }
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------- drawing */

static void sample_hist(hist_t *h)
{
    if (h->count)
        return;
    for (int i = 0; i < HIST_LEN; i++)
        hist_push(h, 50 + 45 * sin(i / 3.0) * cos(i / 11.0));
}

static void option_row(ui_state_t *u, int o, int y, int x, int w, int sel)
{
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    int lw = MIN(19, w / 2);
    if (sel)
        fill(y, x, MIN(w, lw + 22), base);
    char v[40];
    opt_value(u, o, v, sizeof v);
    put(y, x + 1, lw, sel ? base : CP(C_INFO), "%s", OPT_LABEL[o]);
    int xx = x + 1 + lw;
    put(y, xx, x + w - xx, base | A_BOLD, U ? "◀ %s ▶" : "< %s >", v);
    xx += 22; /* samples line up in one column */
    int room = x + w - xx - 2;
    if (room < 8)
        return;
    int sw = MIN(room, 20);
    if (o == OPT_BARS || o == OPT_COLORS) {
        bar(y, xx, sw, 68, 0);
    } else if (o == OPT_GRAPHS) {
        static hist_t demo;
        sample_hist(&demo);
        graph(y, xx, 1, sw, &demo, 100, 0, 0);
    }
}

static void panel_row(const layout_t *l, int p, int y, int x, int w, int sel)
{
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    if (sel)
        fill(y, x, w, base);
    int xx = x + 2;
    if (U)
        xx += put(y, xx, w, sel ? base : (l->on[p] ? CP(C_OK) | A_BOLD : A_GRAY), "%s ", l->on[p] ? "■" : "□");
    else
        xx += put(y, xx, w, base, "%s ", l->on[p] ? "[x]" : "[ ]");
    put(y, xx, x + w - xx, sel ? base | A_BOLD : (l->on[p] ? A_BOLD : A_GRAY), "%s", PANEL_NAME[p]);
    if (!l->on[p])
        put(y, x + w - 8, 7, sel ? base : A_GRAY, "hidden");
}

void setup_view(sys_t *s, ui_state_t *u)
{
    (void)s;
    layout_t *l = &u->lay;
    item_t it[P_COUNT + OPT_COUNT];
    int n = build_items(l, it);
    u->setup_sel = CLAMP(u->setup_sel, 0, n - 1);
    item_t cur = it[u->setup_sel];

    int top = 1, H = LINES - 2, W = COLS;
    int two = W >= 76;
    int pw = two ? MIN(40, W * 2 / 5) : W;            /* panels box width */
    int ph = two ? H : l->ncol[0] + l->ncol[1] + 6;   /* panels box height */
    int ox = two ? pw : 0, oy = two ? top : top + ph, ow = W - ox;
    int oh = OPT_COUNT + 4;

    /* ---- panels, grouped by column */
    box_(top, 0, ph, pw, "Dashboard panels", NULL);
    int y = top + 2, k = 0;
    for (int c = 0; c < 2; c++) {
        put(y++, 2, pw - 4, A_GRAY | A_BOLD, c ? "Right column" : "Left column");
        for (int i = 0; i < l->ncol[c]; i++, k++)
            panel_row(l, l->col[c][i], y++, 1, pw - 2, k == u->setup_sel);
        y++;
    }
    static const char *const help[] = { "Space / Enter  show or hide",
                                        "[ ]  or Shift+Up/Down  move",
                                        "Left / Right   other column",
                                        "R              reset to defaults" };
    for (int i = 0; i < 4 && two; i++)
        if (y + i < top + ph - 1)
            put(y + i, 3, pw - 5, A_GRAY, "%s", help[i]);

    /* ---- appearance + behaviour */
    if (oy + 3 > top + H)
        return;
    oh = MIN(oh, top + H - oy);
    box_(oy, ox, oh, ow, "Appearance & behaviour", "Left / Right to change");
    for (int o = 0; o < OPT_COUNT && 2 + o < oh - 1; o++)
        option_row(u, o, oy + 2 + o, ox + 1, ow - 2, cur.kind == 1 && cur.id == o);

    /* ---- live preview of the dashboard layout */
    int py = oy + oh, phh = top + H - py;
    if (!two || phh < 6)
        return;
    box_(py, ox, phh, ow, "Preview", NULL);
    dashboard_preview(l, py + 2, ox + 3, phh - 6, ow - 6, cur.kind == 0 ? cur.id : -1);
    put(py + phh - 3, ox + 3, ow - 6, A_GRAY, "Saved automatically to");
    put(py + phh - 2, ox + 3, ow - 6, A_GRAY, "%s", config_path());
}
