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
 * them in order (and in the left or right column), pick the look of bars,
 * graphs and colours, and reset everything to defaults.
 * Every change is applied and saved at once. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    OPT_PROCS, OPT_BARS, OPT_GRAPHS, OPT_LEFTW, OPT_COLORS, OPT_SINGLE,
    OPT_CPU_USAGE, OPT_CPU_CLOCK, OPT_CPU_TEMP, OPT_INTERVAL, OPT_KTHREADS, OPT_COUNT
};
static const char *const OPT_LABEL[OPT_COUNT] = {
    "Process list", "Bar style",   "Graphs",      "Left column width", "Colours",  "  Colour",
    "Usage",        "Clock speed", "Temperature", "Refresh interval",  "Kernel threads",
};
enum { ACT_RESET_ALL };
static const char *const ACT_LABEL[] = { "Reset ALL settings to defaults" };

static const int LEFTW[] = { 0, 35, 40, 45, 50, 55, 60, 65, 70 };
static const double STEPS[] = { 0.25, 0.5, 1, 2, 3, 5, 10 };
#define NLEFTW ((int)(sizeof LEFTW / sizeof LEFTW[0]))
#define NSTEPS ((int)(sizeof STEPS / sizeof STEPS[0]))

/* rows of the right-hand settings list (headings + selectable rows) */
enum { K_PANEL, K_OPT, K_ACT, K_HEAD, K_BLANK };
typedef struct {
    int kind, id;
    const char *text;
} row_t;

/* every selectable thing: the panels (left), then right-hand rows */
typedef struct {
    int kind, id, col, pos, row;
} item_t;

#define MAXROWS 64

static int build_rows(const ui_state_t *u, row_t *r)
{
    int n = 0;
#define HEAD(t) (r[n++] = (row_t){ K_HEAD, 0, (t) })
#define BLANK() (r[n++] = (row_t){ K_BLANK, 0, NULL })
#define ROW(k, i) (r[n++] = (row_t){ (k), (i), NULL })
    HEAD("Appearance");
    ROW(K_OPT, OPT_PROCS);
    ROW(K_OPT, OPT_BARS);
    ROW(K_OPT, OPT_GRAPHS);
    ROW(K_OPT, OPT_LEFTW);
    ROW(K_OPT, OPT_COLORS);
    if (u->lay.color_mode == COLOR_SINGLE) /* which colour */
        ROW(K_OPT, OPT_SINGLE);
    BLANK();
    HEAD("CPU core bars: any combination");
    ROW(K_OPT, OPT_CPU_USAGE);
    ROW(K_OPT, OPT_CPU_CLOCK);
    ROW(K_OPT, OPT_CPU_TEMP);
    BLANK();
    HEAD("Behaviour");
    ROW(K_OPT, OPT_INTERVAL);
    ROW(K_OPT, OPT_KTHREADS);
    BLANK();
    ROW(K_ACT, ACT_RESET_ALL);
#undef HEAD
#undef BLANK
#undef ROW
    return n;
}

static int build_items(const ui_state_t *u, item_t *it, row_t *rows, int *nrows)
{
    const layout_t *l = &u->lay;
    int n = 0;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < l->ncol[c]; i++)
            it[n++] = (item_t){ K_PANEL, l->col[c][i], c, i, -1 };
    *nrows = build_rows(u, rows);
    for (int i = 0; i < *nrows; i++)
        if (rows[i].kind == K_OPT || rows[i].kind == K_ACT)
            it[n++] = (item_t){ rows[i].kind, rows[i].id, -1, -1, i };
    return n;
}

static int find_item(const ui_state_t *u, int kind, int id)
{
    item_t it[MAXROWS];
    row_t rows[MAXROWS];
    int nr, n = build_items(u, it, rows, &nr);
    for (int i = 0; i < n; i++)
        if (it[i].kind == kind && it[i].id == id)
            return i;
    return 0;
}

static void save(ui_state_t *u)
{
    if (config_save(u) != 0)
        ui_message(u, 1, "Could not save %s: %s", config_path(), strerror(errno));
}

/* ---------------------------------------------------------- options */

static int cpu_bit(int o)
{
    return o == OPT_CPU_USAGE ? CPUBAR_USAGE : o == OPT_CPU_CLOCK ? CPUBAR_CLOCK : CPUBAR_TEMP;
}

static void opt_value(const ui_state_t *u, int o, char *out, size_t n)
{
    const layout_t *l = &u->lay;
    switch (o) {
    case OPT_PROCS: str_copy(out, n, l->show_procs ? "Shown" : "Hidden"); break;
    case OPT_BARS: str_copy(out, n, BAR_NAME[l->bar_style]); break;
    case OPT_GRAPHS: str_copy(out, n, GRAPH_NAME[l->graph_style]); break;
    case OPT_COLORS: str_copy(out, n, COLOR_NAME[l->color_mode]); break;
    case OPT_SINGLE: str_copy(out, n, SINGLE_NAME[l->single_color]); break;
    case OPT_CPU_USAGE:
    case OPT_CPU_CLOCK:
    case OPT_CPU_TEMP: str_copy(out, n, (l->cpu_bars & cpu_bit(o)) ? "On" : "Off"); break;
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
    case OPT_SINGLE: /* bright colours only where the terminal has them */
        l->single_color = cyc(l->single_color, dir, COLORS >= 16 ? SINGLE_COUNT : 7);
        break;
    case OPT_CPU_USAGE:
    case OPT_CPU_CLOCK:
    case OPT_CPU_TEMP:
        if (l->cpu_bars == cpu_bit(o)) {
            ui_message(u, 1, "At least one kind of CPU bar has to stay on");
            return;
        }
        l->cpu_bars ^= cpu_bit(o);
        break;
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

/* "Reset ALL" asks for a second press within 5 seconds. */
static void reset_all(ui_state_t *u)
{
    if (u->confirm != ACT_RESET_ALL || now_sec() > u->confirm_until) {
        u->confirm = ACT_RESET_ALL;
        u->confirm_until = now_sec() + 5.0;
        ui_message(u, 1, "Press Enter (or R) again to reset ALL settings to defaults; any other key cancels");
        return;
    }
    u->confirm = -1;
    config_reset_all(u);
    u->setup_sel = 0;
    u->setup_scroll = 0;
    ui_message(u, 0, "All settings are back to their defaults");
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

/* ------------------------------------------------------------- keys */

int setup_key(ui_state_t *u, int ch)
{
    if (u->confirm == ACT_RESET_ALL && now_sec() <= u->confirm_until) { /* "Reset ALL" is waiting */
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 'R') {
            reset_all(u);
            return 1;
        }
        u->confirm = -1; /* any other key cancels it */
        ui_message(u, 0, "Reset cancelled");
    }
    layout_t *l = &u->lay;
    item_t it[MAXROWS];
    row_t rows[MAXROWS];
    int nr, n = build_items(u, it, rows, &nr);
    u->setup_sel = CLAMP(u->setup_sel, 0, n - 1);
    item_t cur = it[u->setup_sel];

    switch (ch) {
    case KEY_UP: u->setup_sel = cyc(u->setup_sel, -1, n); return 1;
    case KEY_DOWN: u->setup_sel = cyc(u->setup_sel, 1, n); return 1;
    case KEY_PPAGE: u->setup_sel = MAX(0, u->setup_sel - 10); return 1;
    case KEY_NPAGE: u->setup_sel = MIN(n - 1, u->setup_sel + 10); return 1;
    case KEY_HOME: u->setup_sel = 0; return 1;
    case KEY_END: u->setup_sel = n - 1; return 1;
    case 27:
        u->confirm = -1;
        u->view = VIEW_DASH;
        return 1;
    case 'R': reset_all(u); return 1;
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (cur.kind == K_PANEL) {
            l->on[cur.id] ^= 1;
            save(u);
        } else if (cur.kind == K_OPT) {
            opt_change(u, cur.id, 1);
        } else if (cur.kind == K_ACT && ch != ' ') { /* only Enter confirms a reset */
            reset_all(u);
        }
        return 1;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (cur.kind == K_OPT) {
            opt_change(u, cur.id, ch == KEY_RIGHT ? 1 : -1);
        } else if (cur.kind == K_PANEL) {
            panel_to(l, cur.col, cur.pos, ch == KEY_RIGHT ? 1 : 0);
            u->setup_sel = find_item(u, K_PANEL, cur.id);
            save(u);
        }
        return 1;
    case '[':
    case ']':
    case KEY_SR: /* shift + up   */
    case KEY_SF: /* shift + down */
        if (cur.kind == K_PANEL) {
            panel_move(l, cur.col, cur.pos, (ch == '[' || ch == KEY_SR) ? -1 : 1);
            u->setup_sel = find_item(u, K_PANEL, cur.id);
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
    int lw = MIN(21, w / 2);
    if (sel)
        fill(y, x, MIN(w, lw + 22), base);
    char v[40];
    opt_value(u, o, v, sizeof v);
    int cpu = o == OPT_CPU_USAGE || o == OPT_CPU_CLOCK || o == OPT_CPU_TEMP;
    if (cpu) { /* checkbox-style toggles */
        int on = (u->lay.cpu_bars & cpu_bit(o)) != 0;
        int xx = x + 1 + put(y, x + 1, w, sel ? base : (on ? CP(C_OK) | A_BOLD : A_GRAY), "%s ",
                             U ? (on ? "■" : "□") : (on ? "[x]" : "[ ]"));
        put(y, xx, lw - 2, sel ? base | A_BOLD : (on ? A_BOLD : A_GRAY), "%s", OPT_LABEL[o]);
        put(y, x + 1 + lw, w - lw - 1, sel ? base : A_GRAY, "%s", on ? "shown" : "off");
        return;
    }
    put(y, x + 1, lw, sel ? base : CP(C_INFO), "%s", OPT_LABEL[o]);
    int xx = x + 1 + lw;
    put(y, xx, x + w - xx, base | A_BOLD, U ? "◀ %s ▶" : "< %s >", v);
    xx += 22; /* samples line up in one column */
    int room = x + w - xx - 2;
    if (room < 8)
        return;
    int sw = MIN(room, 20);
    if (o == OPT_BARS || o == OPT_COLORS || o == OPT_SINGLE) {
        bar(y, xx, sw, 68, 0);
    } else if (o == OPT_GRAPHS) {
        static hist_t demo;
        sample_hist(&demo);
        graph(y, xx, 1, sw, &demo, 100, 0, 0);
    }
}

static void action_row(ui_state_t *u, int a, int y, int x, int w, int sel)
{
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    if (sel)
        fill(y, x, MIN(w, 41), base);
    put(y, x + 1, w - 2, sel ? base | A_BOLD : CP(C_WARN), "%s %s", U ? "↺" : "*", ACT_LABEL[a]);
    if (u->confirm == a && now_sec() < u->confirm_until)
        put(y, x + 43, w - 44, CP(C_CRIT) | A_BOLD, "press Enter again to confirm");
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
    item_t it[MAXROWS];
    row_t rows[MAXROWS];
    int nr, n = build_items(u, it, rows, &nr);
    u->setup_sel = CLAMP(u->setup_sel, 0, n - 1);
    item_t cur = it[u->setup_sel];
    if (u->confirm >= 0 && now_sec() > u->confirm_until)
        u->confirm = -1;

    int top = 1, H = LINES - 2, W = COLS;
    int two = W >= 76;
    int pw = two ? MIN(40, W * 2 / 5) : W;               /* panels box width  */
    int ph = two ? H : l->ncol[0] + l->ncol[1] + 6;      /* panels box height */
    int ox = two ? pw : 0, oy = two ? top : top + ph, ow = W - ox;

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
                                        "R              reset everything" };
    for (int i = 0; i < 4 && two; i++)
        if (y + i < top + ph - 1)
            put(y + i, 3, pw - 5, A_GRAY, "%s", help[i]);

    /* ---- settings list; scrolls when it doesn't fit */
    int avail = top + H - oy;
    if (avail < 4)
        return;
    int want = nr + 3, prev_room = avail - want;
    int show_prev = two && prev_room >= 9;
    int oh = show_prev ? want : avail, inner = oh - 3;
    box_(oy, ox, oh, ow, "Settings", "Up/Down select  Left/Right change");
    if (cur.row >= 0) { /* keep the selected row visible */
        if (cur.row < u->setup_scroll)
            u->setup_scroll = cur.row;
        if (cur.row >= u->setup_scroll + inner)
            u->setup_scroll = cur.row - inner + 1;
    }
    u->setup_scroll = CLAMP(u->setup_scroll, 0, MAX(0, nr - inner));
    for (int i = 0; i < inner && u->setup_scroll + i < nr; i++) {
        int ri = u->setup_scroll + i, yy = oy + 2 + i;
        row_t *r = &rows[ri];
        int sel = cur.row == ri;
        switch (r->kind) {
        case K_HEAD: put(yy, ox + 2, ow - 4, A_GRAY | A_BOLD, "%s", r->text); break;
        case K_OPT: option_row(u, r->id, yy, ox + 1, ow - 2, sel); break;
        case K_ACT: action_row(u, r->id, yy, ox + 1, ow - 2, sel); break;
        default: break;
        }
    }
    if (nr > inner) {
        char m[32];
        sfmt(m, sizeof m, " %s more ", U ? "↑↓" : "up/down");
        put(oy + oh - 1, ox + ow - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }

    /* ---- live preview of the dashboard layout */
    if (!show_prev)
        return;
    int py = oy + oh, phh = top + H - py;
    box_(py, ox, phh, ow, "Preview", NULL);
    dashboard_preview(l, py + 2, ox + 3, phh - 6, ow - 6, cur.kind == K_PANEL ? cur.id : -1);
    put(py + phh - 3, ox + 3, ow - 6, A_GRAY, "Saved automatically to");
    put(py + phh - 2, ox + 3, ow - 6, A_GRAY, "%s", config_path());
}
