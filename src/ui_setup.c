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
#include "watch.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    OPT_PROCS, OPT_BARS, OPT_GRAPHS, OPT_LEFTW, OPT_COLORS, OPT_SINGLE, OPT_HIGHLIGHT, OPT_SELTEXT,
    OPT_TITLES, OPT_BACKGROUND, OPT_CORE_BAR, OPT_CORE_VALUES,
    OPT_NOTIFY, OPT_A_TEMP, OPT_A_THROTTLE, OPT_A_GTEMP, OPT_A_GSLOW, OPT_A_DISK, OPT_A_MEM, OPT_A_FULL, OPT_A_PROBLEMS,
    OPT_KEEP, OPT_REC, OPT_REC_MIN,
    OPT_INTERVAL, OPT_KTHREADS, OPT_MOUSE, OPT_NETCHECK, OPT_COUNT
};
static const char *const OPT_LABEL[OPT_COUNT] = {
    "Process list", "Bar style",   "Graphs",      "Left column width", "Bars & values",  "  Colour",
    "Highlight",    "Selected text", "Titles",    "Background",
    "Bars show",    "Other values",
    "Alert by",     "CPU hotter than", "CPU throttling", "GPU hotter than", "GPU slowing", "Disk slower than",
    "Memory left below", "Disk fuller than", "Problems",
    "Delete logs after", "Flight recorder", "  Keeps the last",
    "Refresh interval",  "Kernel threads", "Mouse", "Network check",
};
enum { ACT_RESET_ALL, ACT_TEST_ALERT, ACT_SENSORS };
static const char *const ACT_LABEL[] = { "Reset ALL settings to defaults", "Send a test alert",
                                         "Sensor names and limits" };

static const int LEFTW[] = { 0, 35, 40, 45, 50, 55, 60, 65, 70 };
static const double STEPS[] = { 0.25, 0.5, 1, 2, 3, 5, 10 };
static const int A_TEMPS[] = { 0, 70, 75, 80, 85, 90, 95, 100 };
static const int A_DISKS[] = { 0, 100, 200, 500, 1000, 2000, 5000 };
static const int A_MEMS[] = { 0, 2, 5, 10, 15, 20 };
static const int A_FULLS[] = { 0, 80, 85, 90, 95, 98 };
static const int REC_MINS[] = { 2, 5, 10, 30 };
#define NLEFTW ((int)(sizeof LEFTW / sizeof LEFTW[0]))
#define NSTEPS ((int)(sizeof STEPS / sizeof STEPS[0]))
#define COUNT(a) ((int)(sizeof(a) / sizeof(a)[0]))

const char *const NOTIFY_NAME[ALERT_COUNT] = { "Off", "Desktop", "Terminal bell", "Desktop + bell" };
const int KEEP_DAYS[] = { 0, 7, 14, 30, 60, 90, 180, 365 };
const int NKEEP = COUNT(KEEP_DAYS);

/* the next entry of a list of choices, starting from where v is */
static int step_list(const int *list, int n, int v, int dir)
{
    int i = 0;
    while (i < n && list[i] != v)
        i++;
    if (i == n) /* a value set by hand in the config file: start from the nearest */
        for (i = 0; i < n - 1 && list[i] < v; i++)
            ;
    return list[((i + dir) % n + n) % n];
}

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
    BLANK();
    HEAD("Colours");
    ROW(K_OPT, OPT_COLORS);
    if (u->lay.color_mode == COLOR_SINGLE) /* which colour */
        ROW(K_OPT, OPT_SINGLE);
    ROW(K_OPT, OPT_HIGHLIGHT);
    ROW(K_OPT, OPT_SELTEXT);
    ROW(K_OPT, OPT_TITLES);
    ROW(K_OPT, OPT_BACKGROUND);
    BLANK();
    HEAD("CPU cores");
    ROW(K_OPT, OPT_CORE_BAR);
    ROW(K_OPT, OPT_CORE_VALUES);
    BLANK();
    HEAD("Alerts");
    ROW(K_OPT, OPT_NOTIFY); /* the limits also decide what counts as a problem, so they stay */
    ROW(K_OPT, OPT_A_TEMP);
    ROW(K_OPT, OPT_A_THROTTLE);
    ROW(K_OPT, OPT_A_GTEMP);
    ROW(K_OPT, OPT_A_GSLOW);
    ROW(K_OPT, OPT_A_DISK);
    ROW(K_OPT, OPT_A_MEM);
    ROW(K_OPT, OPT_A_FULL);
    ROW(K_OPT, OPT_A_PROBLEMS);
    ROW(K_ACT, ACT_TEST_ALERT);
    BLANK();
    HEAD("Sensors");
    ROW(K_ACT, ACT_SENSORS);
    BLANK();
    HEAD("Logs");
    ROW(K_OPT, OPT_KEEP);
    ROW(K_OPT, OPT_REC);
    if (u->rec_on)
        ROW(K_OPT, OPT_REC_MIN);
    BLANK();
    HEAD("Behaviour");
    ROW(K_OPT, OPT_INTERVAL);
    ROW(K_OPT, OPT_KTHREADS);
    ROW(K_OPT, OPT_MOUSE);
    ROW(K_OPT, OPT_NETCHECK);
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

static void core_sample(const layout_t *l, int y, int x, int w);

static void off_or(char *out, size_t n, int v, const char *fmt)
{
    if (v > 0)
        sfmt(out, n, fmt, v);
    else
        str_copy(out, n, "Off");
}

void keep_text(int days, char *out, size_t n)
{
    if (days > 0)
        sfmt(out, n, "%d days", days);
    else
        str_copy(out, n, "Never");
}

void keep_changed(ui_state_t *u)
{
    if (u->log_keep_days > 0)
        ui_message(u, 0, "Logs older than %d days will be deleted (the first clean-up runs in 2 minutes)",
                   u->log_keep_days);
    else
        ui_message(u, 0, "Logs are kept: nothing is deleted");
    watch_keep_changed();
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
    case OPT_HIGHLIGHT: str_copy(out, n, SINGLE_NAME[l->highlight]); break;
    case OPT_BACKGROUND: str_copy(out, n, BG_NAME[l->background]); break;
    case OPT_SELTEXT: str_copy(out, n, TXT_NAME[l->sel_text]); break;
    case OPT_TITLES: str_copy(out, n, l->title_color ? TXT_NAME[l->title_color] : "Automatic (cyan)"); break;
    case OPT_CORE_BAR: str_copy(out, n, CORE_NAME[l->core_bar]); break;
    case OPT_CORE_VALUES: str_copy(out, n, l->core_values ? "Shown" : "Hidden"); break;
    case OPT_NOTIFY: str_copy(out, n, NOTIFY_NAME[u->alert_notify]); break;
    case OPT_A_TEMP: off_or(out, n, u->alert_cpu_temp, U ? "%d °C" : "%d C"); break;
    case OPT_A_THROTTLE: str_copy(out, n, u->alert_throttle ? "Alert" : "Off"); break;
    case OPT_A_GTEMP: off_or(out, n, u->alert_gpu_temp, U ? "%d °C" : "%d C"); break;
    case OPT_A_GSLOW: str_copy(out, n, u->alert_gpu_slow ? "Alert" : "Off"); break;
    case OPT_A_DISK: off_or(out, n, u->alert_disk_ms, "%d ms"); break;
    case OPT_A_MEM: off_or(out, n, u->alert_mem_pct, "%d%%"); break;
    case OPT_A_FULL: off_or(out, n, u->alert_disk_full, "%d%%"); break;
    case OPT_A_PROBLEMS: str_copy(out, n, u->alert_problems ? "Alert" : "Off"); break;
    case OPT_KEEP: keep_text(u->log_keep_days, out, n); break;
    case OPT_REC: str_copy(out, n, u->rec_on ? "On" : "Off"); break;
    case OPT_REC_MIN: sfmt(out, n, "%d minutes", u->rec_minutes); break;
    case OPT_MOUSE: str_copy(out, n, l->mouse ? "On" : "Off"); break;
    case OPT_LEFTW:
        if (l->left_pct)
            sfmt(out, n, "%d%%", l->left_pct);
        else
            str_copy(out, n, "Automatic");
        break;
    case OPT_INTERVAL: sfmt(out, n, "%g s", u->interval); break;
    case OPT_KTHREADS: str_copy(out, n, u->show_kthreads ? "Shown" : "Hidden"); break;
    case OPT_NETCHECK: str_copy(out, n, u->net_check ? "On" : "Off"); break;
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
    case OPT_HIGHLIGHT: l->highlight = cyc(l->highlight, dir, COLORS >= 16 ? SINGLE_COUNT : 7); break;
    case OPT_BACKGROUND:
        do /* skip dark gray / bright white on 8-colour terminals */
            l->background = cyc(l->background, dir, BG_COUNT);
        while (COLORS < 16 && BG_COLOR[l->background] >= 8);
        break;
    case OPT_SELTEXT:
    case OPT_TITLES: {
        int *v = o == OPT_SELTEXT ? &l->sel_text : &l->title_color;
        do /* bright colours only where the terminal has them */
            *v = cyc(*v, dir, TXT_COUNT);
        while (COLORS < 16 && TXT_COLOR[*v] >= 8);
        break;
    }
    case OPT_CORE_BAR: l->core_bar = cyc(l->core_bar, dir, CORE_COUNT); break;
    case OPT_CORE_VALUES: l->core_values ^= 1; break;
    case OPT_NOTIFY: u->alert_notify = cyc(u->alert_notify, dir, ALERT_COUNT); break;
    case OPT_A_TEMP: u->alert_cpu_temp = step_list(A_TEMPS, COUNT(A_TEMPS), u->alert_cpu_temp, dir); break;
    case OPT_A_THROTTLE: u->alert_throttle ^= 1; break;
    case OPT_A_GTEMP: u->alert_gpu_temp = step_list(A_TEMPS, COUNT(A_TEMPS), u->alert_gpu_temp, dir); break;
    case OPT_A_GSLOW: u->alert_gpu_slow ^= 1; break;
    case OPT_A_DISK: u->alert_disk_ms = step_list(A_DISKS, COUNT(A_DISKS), u->alert_disk_ms, dir); break;
    case OPT_A_MEM: u->alert_mem_pct = step_list(A_MEMS, COUNT(A_MEMS), u->alert_mem_pct, dir); break;
    case OPT_A_FULL: u->alert_disk_full = step_list(A_FULLS, COUNT(A_FULLS), u->alert_disk_full, dir); break;
    case OPT_A_PROBLEMS: u->alert_problems ^= 1; break;
    case OPT_KEEP:
        u->log_keep_days = step_list(KEEP_DAYS, NKEEP, u->log_keep_days, dir);
        keep_changed(u);
        break;
    case OPT_REC: u->rec_on ^= 1; break;
    case OPT_REC_MIN: u->rec_minutes = step_list(REC_MINS, COUNT(REC_MINS), u->rec_minutes, dir); break;
    case OPT_MOUSE:
        l->mouse ^= 1;
        ui_mouse(l->mouse);
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
    case OPT_NETCHECK:
        u->net_check ^= 1;
        netq_set(u->net_check, u->net_check_host);
        break;
    }
    save(u);
}

/* "Reset ALL" asks for a second press within 5 seconds. */
static void reset_all(ui_state_t *u)
{
    if (u->confirm != ACT_RESET_ALL || now_sec() > u->confirm_until) {
        u->confirm = ACT_RESET_ALL;
        u->confirm_until = now_sec() + 5.0;
        ui_message(u, 1, "Press Enter (or R) again to reset ALL settings to defaults (sensor names stay); any other key cancels");
        return;
    }
    u->confirm = -1;
    config_reset_all(u);
    netq_set(u->net_check, u->net_check_host); /* off again */
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
    if (u->sens_open)
        return sensors_page_key(u, ch);
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
        } else if (cur.kind == K_ACT && cur.id == ACT_TEST_ALERT) {
            alert_test(u, 1);
        } else if (cur.kind == K_ACT && cur.id == ACT_SENSORS) {
            u->sens_open = 1;
            u->sens_edit = 0;
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

/* a click selects; on an arrow it changes the value, on a box (or twice) it
 * does what Enter does */
void setup_click(ui_state_t *u, int item, int arrow, int dbl)
{
    u->setup_sel = item;
    if (arrow == -1 || arrow == 1)
        setup_key(u, arrow < 0 ? KEY_LEFT : KEY_RIGHT);
    else if (arrow == 2 || dbl)
        setup_key(u, '\n');
}

/* ----------------------------------------------------------- drawing */

static void sample_hist(hist_t *h)
{
    if (h->count)
        return;
    for (int i = 0; i < HIST_LEN; i++)
        hist_push(h, 50 + 45 * sin(i / 3.0) * cos(i / 11.0));
}

static void option_row(ui_state_t *u, int o, int y, int x, int w, int sel, int item)
{
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    int lw = MIN(21, w / 2);
    if (sel)
        fill(y, x, MIN(w, lw + 22), base);
    char v[40];
    opt_value(u, o, v, sizeof v);
    put(y, x + 1, lw, sel ? base : CP(C_INFO), "%s", OPT_LABEL[o]);
    int xx = x + 1 + lw;
    int vw = put(y, xx, x + w - xx, base | A_BOLD, U ? "◀ %s ▶" : "< %s >", v);
    hit_add(y, x, 1, w, HIT_SETUP, item, 0);
    hit_add(y, xx, 1, 2, HIT_SETUP, item, -1); /* the arrows change the value */
    hit_add(y, xx + vw - 2, 1, 2, HIT_SETUP, item, 1);
    xx += 22; /* samples line up in one column */
    int room = x + w - xx - 2;
    if (room < 8)
        return;
    int sw = MIN(room, 20);
    if (o == OPT_BARS || o == OPT_COLORS || o == OPT_SINGLE) {
        bar(y, xx, sw, 68, 0);
    } else if (o == OPT_HIGHLIGHT || o == OPT_SELTEXT) {
        put(y, xx, sw, CP(C_SEL) | A_BOLD, " %-*s", sw - 1, "selected row");
    } else if (o == OPT_TITLES) {
        put(y, xx, room, CP(C_TITLE) | A_BOLD, "Dashboard panels  Sensors");
    } else if (o == OPT_BACKGROUND && u->lay.background == 0) {
        put(y, xx, room, A_GRAY, "same as your terminal");
    } else if (o == OPT_GRAPHS) {
        static hist_t demo;
        sample_hist(&demo);
        graph(y, xx, 1, sw, &demo, 100, 0, 0);
    } else if (o == OPT_CORE_BAR) {
        core_sample(&u->lay, y, xx, room);
    } else {
        const char *hint = NULL;
        switch (o) {
        case OPT_NOTIFY:
            hint = u->alert_notify == ALERT_OFF ? "no alerts"
                 : u->alert_notify == ALERT_BELL ? "beeps while hsm is open"
                                                 : "a pop-up on your desktop";
            break;
        case OPT_A_TEMP: hint = "for 5 seconds"; break;
        case OPT_A_THROTTLE: hint = "slowed down by heat"; break;
        case OPT_A_GTEMP: hint = "for 5 seconds"; break;
        case OPT_A_GSLOW: hint = "slowed down by heat for minutes"; break;
        case OPT_A_DISK: hint = "per request, for 10 s"; break;
        case OPT_A_MEM: hint = "of memory available"; break;
        case OPT_A_FULL: hint = "space or inodes used"; break;
        case OPT_A_PROBLEMS: hint = "freezes, kernel errors, OOM, bad drives"; break;
        case OPT_CORE_VALUES: hint = u->lay.core_values ? "the other two, as numbers" : "only the bar's own value"; break;
        case OPT_KEEP: hint = u->log_keep_days ? "only hsm's own CSV files" : "logs are kept"; break;
        case OPT_REC: hint = "saves the lead-up to problems"; break;
        case OPT_REC_MIN: hint = "in memory"; break;
        case OPT_MOUSE: hint = u->lay.mouse ? "Shift + drag selects text" : NULL; break;
        case OPT_NETCHECK: hint = u->net_check ? "pings the router and the internet every 2 s" : "delay and lost packets"; break;
        case OPT_INTERVAL: hint = "+ and - change it anywhere"; break;
        }
        if (hint)
            put(y, xx, room, A_GRAY, "%s", hint);
    }
}

/* an example core row, as the CPU panel would draw it */
static void core_sample(const layout_t *l, int y, int x, int w)
{
    static const char *const val[CORE_COUNT] = { "48%", "54°", "3.6G" };
    static const double pct[CORE_COUNT] = { 48, 54, 88 };
    int m = l->core_bar, bw = MIN(10, w - 20);
    if (bw < 3)
        return;
    int xx = x + put(y, x, w, A_GRAY, "0 ");
    attr_t ba = m == CORE_TEMP ? bar_temp_attr(54) : m == CORE_CLOCK ? (gradient_on() ? 0 : CP(C_IN)) : 0;
    bar(y, xx, bw, pct[m], ba);
    xx += bw + 1;
    const char *t = U || m != CORE_TEMP ? val[m] : "54C";
    attr_t ta = m == CORE_USAGE ? pct_attr(48) : m == CORE_TEMP ? temp_attr(54) : A_NORMAL;
    xx += put(y, xx + 4 - utf8_cols(t), 4, ta | A_BOLD, "%s", t) + (4 - utf8_cols(t));
    static const int order[CORE_COUNT] = { CORE_USAGE, CORE_CLOCK, CORE_TEMP };
    for (int k = 0; k < CORE_COUNT && l->core_values; k++) {
        int o = order[k];
        if (o == m)
            continue;
        t = U || o != CORE_TEMP ? val[o] : "54C";
        ta = o == CORE_USAGE ? pct_attr(48) : o == CORE_TEMP ? temp_attr(54) : A_NORMAL;
        xx += put(y, xx + 6 - utf8_cols(t), MAX(0, x + w - xx), ta, "%s", t) + (6 - utf8_cols(t));
    }
}

static void action_row(ui_state_t *u, int a, int y, int x, int w, int sel, int item)
{
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    if (sel)
        fill(y, x, MIN(w, 41), base);
    hit_add(y, x, 1, MIN(w, 41), HIT_SETUP, item, 0);
    if (a == ACT_TEST_ALERT || a == ACT_SENSORS) {
        put(y, x + 1, w - 2, sel ? base | A_BOLD : CP(C_INFO), "%s %s", U ? "▸" : ">", ACT_LABEL[a]);
        int hx = x + 1 + MIN(21, w / 2) + 22; /* where the other rows show their samples */
        if (x + w - hx > 12)
            put(y, hx, x + w - hx, A_GRAY, "%s",
                a == ACT_TEST_ALERT ? "Enter: check that alerts reach you" : "Enter: your names, limits, hiding");
        return;
    }
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
    if (u->sens_open) {
        sensors_page(s, u);
        return;
    }
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
        for (int i = 0; i < l->ncol[c]; i++, k++) {
            hit_add(y, 1, 1, pw - 2, HIT_SETUP, k, 0);
            hit_add(y, 2, 1, U ? 2 : 4, HIT_SETUP, k, 2); /* the box: show / hide */
            panel_row(l, l->col[c][i], y++, 1, pw - 2, k == u->setup_sel);
        }
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
    /* "Saved automatically to" and the file on one line when they fit */
    char saved[PATHBUF + 32];
    sfmt(saved, sizeof saved, "Saved automatically to %s", config_path());
    int one_line = utf8_cols(saved) <= ow - 6;
    if (show_prev) { /* room in the preview for every panel: the list scrolls a little instead */
        int need = dashboard_preview_rows(l) + (one_line ? 5 : 6);
        int take = MIN(need - prev_room, nr - 16);
        if (take > 0) {
            want -= take;
            prev_room += take;
        }
    }
    int oh = show_prev ? want : avail, inner = oh - 3;
    box_(oy, ox, oh, ow, "Settings", "Up/Down select  Left/Right change");
    if (cur.row >= 0) { /* keep the selected row visible */
        if (cur.row < u->setup_scroll)
            u->setup_scroll = cur.row;
        if (cur.row >= u->setup_scroll + inner)
            u->setup_scroll = cur.row - inner + 1;
    }
    u->setup_scroll = CLAMP(u->setup_scroll, 0, MAX(0, nr - inner));
    hit_add(oy + 2, ox + 1, inner, ow - 2, HIT_SCROLL, 0, 0);
    for (int i = 0; i < inner && u->setup_scroll + i < nr; i++) {
        int ri = u->setup_scroll + i, yy = oy + 2 + i, item = 0;
        row_t *r = &rows[ri];
        int sel = cur.row == ri;
        for (int k2 = 0; k2 < n; k2++)
            if (it[k2].row == ri)
                item = k2;
        switch (r->kind) {
        case K_HEAD: put(yy, ox + 2, ow - 4, A_GRAY | A_BOLD, "%s", r->text); break;
        case K_OPT: option_row(u, r->id, yy, ox + 1, ow - 2, sel, item); break;
        case K_ACT: action_row(u, r->id, yy, ox + 1, ow - 2, sel, item); break;
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
    dashboard_preview(l, py + 2, ox + 3, phh - (one_line ? 5 : 6), ow - 6, cur.kind == K_PANEL ? cur.id : -1);
    if (one_line) {
        put(py + phh - 2, ox + 3, ow - 6, A_GRAY, "%s", saved);
    } else {
        put(py + phh - 3, ox + 3, ow - 6, A_GRAY, "Saved automatically to");
        put(py + phh - 2, ox + 3, ow - 6, A_GRAY, "%s", config_path());
    }
}
