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
/* ui_log.c - Logging view: choose what to record, start / stop the system
 * log, and see / stop the per-process logs. */
#define _GNU_SOURCE
#include "log.h"
#include "ui_int.h"
#include "watch.h"

#include <stdio.h>
#include <string.h>

static const double EVERY[] = { 0, 1, 2, 5, 10, 30, 60 }; /* 0 = every refresh */
#define NEVERY ((int)(sizeof EVERY / sizeof EVERY[0]))

/* where each channel ended up on screen, for Left / Right and scrolling */
static int g_col[MAX_CHAN], g_row[MAX_CHAN], g_rows_vis = 1;

static void every_text(double e, char *out, size_t n)
{
    if (e <= 0)
        str_copy(out, n, "every refresh");
    else
        sfmt(out, n, "every %g s", e);
}

static void prepare(ui_state_t *u)
{
    str_copy(g_log.dir, sizeof g_log.dir, config_log_dir(u));
    g_log.every = u->log_every;
}

static void save_selection(ui_state_t *u)
{
    log_skip_string(u->log_skip, sizeof u->log_skip);
    config_save(u);
}

/* ------------------------------------------------------- start / stop */

void log_toggle_system(sys_t *s, ui_state_t *u)
{
    char p[PATHBUF], err[PATHBUF + 64];
    if (g_log.f) {
        unsigned long rows = g_log.rows;
        log_stop();
        config_pretty_path(g_log.path, p, sizeof p);
        ui_message(u, 0, "Log saved: %s (%lu rows)", p, rows);
        return;
    }
    prepare(u);
    if (log_start(s, err, sizeof err) != 0) {
        ui_message(u, 1, "%s", err);
        return;
    }
    config_pretty_path(g_log.path, p, sizeof p);
    ui_message(u, 0, "Logging %d values to %s  (l stops)", log_count_on(), p);
}

void log_toggle_process(sys_t *s, ui_state_t *u)
{
    if (u->view != VIEW_PROCS && !(u->view == VIEW_DASH && u->lay.show_procs)) {
        ui_message(u, 1, "Select a process in the process list first (press 2)");
        return;
    }
    if (!u->sel_pid) {
        ui_message(u, 1, "No process selected");
        return;
    }
    char p[PATHBUF], err[PATHBUF + 64];
    int i = log_proc_find(u->sel_pid, u->sel_start);
    if (i >= 0) {
        unsigned long rows = g_log.pl[i].rows;
        config_pretty_path(g_log.pl[i].path, p, sizeof p);
        log_proc_stop(i);
        ui_message(u, 0, "Stopped logging PID %d: %s (%lu rows)", (int)u->sel_pid, p, rows);
        return;
    }
    prepare(u);
    if (log_proc_start(s, u->sel_pid, u->sel_start, err, sizeof err) != 0) {
        ui_message(u, 1, "%s", err);
        return;
    }
    config_pretty_path(g_log.pl[g_log.nplog - 1].path, p, sizeof p);
    ui_message(u, 0, "Logging PID %d to %s  (L again stops)", (int)u->sel_pid, p);
}

/* ---------------------------------------------------------------- keys */

static int nearest_in_col(int col, int row)
{
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < g_log.nchan; i++)
        if (g_col[i] == col) {
            int d = g_row[i] > row ? g_row[i] - row : row - g_row[i];
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
    return best;
}

int log_view_key(sys_t *s, ui_state_t *u, int ch)
{
    if (u->log_mode == 1)
        return lv_list_key(s, u, ch);
    if (u->log_mode == 2)
        return lv_key(s, u, ch);
    switch (ch) { /* recorder, clean-up and saved logs work with or without channels */
    case 'v':
        u->log_mode = 1;
        return 1;
    case 'r':
        u->rec_on ^= 1;
        config_save(u);
        if (u->rec_on)
            ui_message(u, 0, "Flight recorder on: the last %d minutes are kept and saved when something goes wrong",
                       u->rec_minutes);
        else
            ui_message(u, 0, "Flight recorder off");
        return 1;
    case 'd': { /* delete logs after: from the longest to the shortest time, then never */
        int i = 0;
        while (i < NKEEP && KEEP_DAYS[i] != u->log_keep_days)
            i++;
        i = i >= NKEEP || i == 0 ? NKEEP - 1 : i - 1; /* KEEP_DAYS[0] is "never" */
        u->log_keep_days = KEEP_DAYS[i];
        config_save(u);
        keep_changed(u);
        return 1;
    }
    }
    int n = g_log.nchan + g_log.nplog, sel = u->log_sel;
    if (n <= 0)
        return 0;
    sel = CLAMP(sel, 0, n - 1);
    switch (ch) {
    case KEY_UP: sel = sel > 0 ? sel - 1 : n - 1; break;
    case KEY_DOWN: sel = sel < n - 1 ? sel + 1 : 0; break;
    case KEY_PPAGE: sel = MAX(0, sel - g_rows_vis); break;
    case KEY_NPAGE: sel = MIN(n - 1, sel + g_rows_vis); break;
    case KEY_HOME: sel = 0; break;
    case KEY_END: sel = n - 1; break;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (sel < g_log.nchan) {
            int j = nearest_in_col(g_col[sel] + (ch == KEY_RIGHT ? 1 : -1), g_row[sel]);
            if (j >= 0)
                sel = j;
        }
        break;
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (sel >= g_log.nchan) { /* a process log: stop it */
            plog_t *pl = &g_log.pl[sel - g_log.nchan];
            char p[PATHBUF];
            config_pretty_path(pl->path, p, sizeof p);
            ui_message(u, 0, "Stopped logging PID %d: %s (%lu rows)", (int)pl->pid, p, pl->rows);
            log_proc_stop(sel - g_log.nchan);
        } else if (g_log.f) {
            ui_message(u, 1, "Stop the log first (l) to change what is recorded");
        } else {
            g_log.ch[sel].on ^= 1;
            save_selection(u);
        }
        break;
    case 'a':
    case 'n':
        if (g_log.f) {
            ui_message(u, 1, "Stop the log first (l) to change what is recorded");
        } else {
            for (int i = 0; i < g_log.nchan; i++)
                g_log.ch[i].on = ch == 'a';
            save_selection(u);
        }
        break;
    case 'i': {
        int i = 0;
        while (i < NEVERY && EVERY[i] != u->log_every)
            i++;
        u->log_every = EVERY[i < NEVERY ? (i + 1) % NEVERY : 2];
        g_log.every = u->log_every;
        config_save(u);
        char t[32];
        every_text(u->log_every, t, sizeof t);
        ui_message(u, 0, "Logging %s", t);
        break;
    }
    default: return 0;
    }
    u->log_sel = sel;
    (void)s;
    return 1;
}

/* a click selects a row; on the box (or a double click) it toggles, as Space does */
void log_click(sys_t *s, ui_state_t *u, int row, int box, int dbl)
{
    u->log_sel = row;
    if (box || dbl)
        log_view_key(s, u, ' ');
}

/* -------------------------------------------------------------- drawing */

static void chan_row(sys_t *s, int i, int y, int x, int w, int sel)
{
    chan_t *c = &g_log.ch[i];
    attr_t base = sel ? CP(C_SEL) : A_NORMAL;
    if (sel)
        fill(y, x, w, base);
    hit_add(y, x, 1, w, HIT_LOGCH, i, 0);
    hit_add(y, x, 1, U ? 3 : 4, HIT_LOGCH, i, 1); /* the box itself: toggles at once */
    int xx = x + 1;
    if (U)
        xx += put(y, xx, w, sel ? base : (c->on ? CP(C_OK) | A_BOLD : A_GRAY), "%s ", c->on ? "■" : "□");
    else
        xx += put(y, xx, w, base, "%s ", c->on ? "[x]" : "[ ]");
    char v[32];
    log_fmt_value(c, log_chan_value(s, c), v, sizeof v);
    if (!U) { /* "°" needs UTF-8 */
        for (char *q = v; *q;) {
            if ((unsigned char)*q >= 0x80)
                memmove(q, q + 1, strlen(q));
            else
                q++;
        }
    }
    int vw = utf8_cols(v);
    put(y, xx, x + w - xx - vw - 2, sel ? base | A_BOLD : (c->on ? A_NORMAL : A_GRAY), "%s", c->label);
    put(y, x + w - vw - 1, vw, sel ? base : A_GRAY, "%s", v);
}

void log_view(sys_t *s, ui_state_t *u)
{
    if (u->log_mode == 1) {
        lv_list_draw(s, u);
        return;
    }
    if (u->log_mode == 2) {
        lv_draw(s, u);
        return;
    }
    int top = 1, H = LINES - 2, W = COLS;
    int n = g_log.nchan + g_log.nplog;
    u->log_sel = n ? CLAMP(u->log_sel, 0, n - 1) : 0;
    char t[64], p[PATHBUF], sz[16];

    /* ---- status */
    int sh = 6;
    box_(top, 0, sh, W, "System log", NULL);
    int x = 2, y = top + 1;
    every_text(g_log.f ? g_log.every : u->log_every, t, sizeof t);
    if (g_log.f) {
        char dur[24];
        fmt_duration(now_sec() - g_log.t0, dur, sizeof dur);
        fmt_bytes((double)log_file_size(g_log.path), sz, sizeof sz);
        x += put(y, x, W - 4, CP(C_CRIT) | A_BOLD, "%s Recording", U ? "●" : "*");
        put(y, x, W - 2 - x, A_BOLD, "   %s   %lu rows   %s   %d values   %s", dur, g_log.rows, sz, log_count_on(), t);
        config_pretty_path(g_log.path, p, sizeof p);
        put(y + 1, 2, W - 4, A_NORMAL, "%s", p);
    } else {
        x += put(y, x, W - 4, A_BOLD, "Not recording");
        put(y, x, W - 2 - x, A_GRAY, "   %d of %d values selected   %s", log_count_on(), g_log.nchan, t);
        if (g_log.path[0]) {
            config_pretty_path(g_log.path, p, sizeof p);
            put(y + 1, 2, W - 4, A_GRAY, "Last log: %s (%lu rows)", p, g_log.rows);
        } else {
            config_pretty_path(config_log_dir(u), p, sizeof p);
            put(y + 1, 2, W - 4, A_GRAY, "Logs are saved as CSV files in %s", p);
        }
    }
    /* flight recorder and clean-up */
    rec_status_t rs;
    rec_status(&rs);
    char keep[24];
    keep_text(u->log_keep_days, keep, sizeof keep);
    x = 2;
    if (!u->rec_on) {
        x += put(y + 2, x, W - 4, A_GRAY, "Flight recorder: off");
    } else if (rs.busy) {
        x += put(y + 2, x, W - 4, CP(C_CRIT) | A_BOLD, "%s Flight recorder: saving an incident log", U ? "●" : "*");
        x += put(y + 2, x, W - 2 - x, A_NORMAL, " (%s)", rs.reason);
    } else {
        x += put(y + 2, x, W - 4, CP(C_OK) | A_BOLD, "Flight recorder: on");
        x += put(y + 2, x, W - 2 - x, A_NORMAL, ", keeps the last %d min", u->rec_minutes);
        if (rs.saved)
            x += put(y + 2, x, W - 2 - x, A_NORMAL, "%s%d incident log%s saved", SEP, rs.saved,
                     rs.saved == 1 ? "" : "s");
    }
    put(y + 2, x, W - 2 - x, A_GRAY, "%sdelete logs after: %s", SEP, keep);
    static const struct {
        const char *k, *l;
        int code;
    } KH[] = { { "l", "start/stop", 'l' },     { "Space", "select", ' ' }, { "a", "all", 'a' },
               { "n", "none", 'n' },           { "i", "interval", 'i' },   { "r", "recorder", 'r' },
               { "d", "delete after", 'd' },   { "v", "saved logs", 'v' } };
    x = 2;
    for (size_t i = 0; i < sizeof KH / sizeof KH[0]; i++)
        x += key_hint(y + 3, x, W - 2, KH[i].k, KH[i].l, KH[i].code);

    /* ---- what to log: groups laid out in columns, whole groups per column */
    int iw = W - 4, ncols = MAX(1, iw / 44), cw = iw / ncols;
    int total = g_log.nchan; /* entries = group headings + channels */
    for (int i = 0; i < g_log.nchan; i++)
        if (!i || strcmp(g_log.ch[i].group, g_log.ch[i - 1].group))
            total++;
    int target = (total + ncols - 1) / ncols;
    static int hcol[MAX_CHAN], hrow[MAX_CHAN], hidx[MAX_CHAN];
    int nh = 0, col = 0, row = 0, maxrow = 0;
    for (int i = 0; i < g_log.nchan;) {
        int j = i;
        while (j < g_log.nchan && !strcmp(g_log.ch[j].group, g_log.ch[i].group))
            j++;
        int k = j - i + 1; /* heading + its channels */
        if (row > 0 && row + k > target && col < ncols - 1) {
            col++;
            row = 0;
        }
        hcol[nh] = col;
        hrow[nh] = row++;
        hidx[nh++] = i;
        for (; i < j; i++) {
            g_col[i] = col;
            g_row[i] = row++;
        }
        maxrow = MAX(maxrow, row);
    }

    /* ---- process logs, right below the list (or at the bottom when it is long) */
    int ph = 2 + MAX(g_log.nplog, 1), cy = top + sh;
    int chh = MIN(MAX(maxrow, 1) + 2, top + H - ph - cy);
    int py = cy + MAX(chh, 0);
    char info[32];
    sfmt(info, sizeof info, "%d running", g_log.nplog);
    box_(py, 0, ph, W, "Process logs", g_log.nplog ? info : NULL);
    if (!g_log.nplog)
        put(py + 1, 3, W - 6, A_GRAY, "None. Select a process in the Processes view (2) and press L to log its usage.");
    for (int i = 0; i < g_log.nplog; i++) {
        plog_t *pl = &g_log.pl[i];
        int yy = py + 1 + i, sel = u->log_sel == g_log.nchan + i;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, 1, W - 2, base);
        hit_add(yy, 1, 1, W - 2, HIT_LOGCH, g_log.nchan + i, 0);
        fmt_bytes((double)log_file_size(pl->path), sz, sizeof sz);
        config_pretty_path(pl->path, p, sizeof p);
        int xx = 3;
        xx += put(yy, xx, W - 6, sel ? base : CP(C_CRIT) | A_BOLD, "%s ", U ? "●" : "*");
        xx += put(yy, xx, W - 3 - xx, base | A_BOLD, "PID %-7d %-16s", (int)pl->pid, pl->comm);
        xx += put(yy, xx, W - 3 - xx, base, "%6lu rows %7s   ", pl->rows, sz);
        put(yy, xx, W - 3 - xx, sel ? base : A_GRAY, "%s", p);
    }
    if (g_log.nplog)
        put(py + ph - 1, W - 36, 34, A_GRAY, " Space / Enter on a row stops it ");

    if (chh < 3)
        return;
    sfmt(info, sizeof info, "%d of %d selected", log_count_on(), g_log.nchan);
    box_(cy, 0, chh, W, "What to log", info);
    if (!g_log.nchan) {
        put(cy + 1, 3, W - 6, A_GRAY, "Nothing found to log");
        return;
    }
    int rows_vis = chh - 2;
    g_rows_vis = MAX(1, rows_vis);
    /* scroll so the selected channel is visible */
    if (u->log_sel < g_log.nchan) {
        int r = g_row[u->log_sel];
        if (r < u->log_scroll)
            u->log_scroll = r;
        if (r >= u->log_scroll + rows_vis)
            u->log_scroll = r - rows_vis + 1;
    }
    u->log_scroll = CLAMP(u->log_scroll, 0, MAX(0, maxrow - rows_vis));
    int y0 = cy + 1;
    for (int h = 0; h < nh; h++) {
        int r = hrow[h] - u->log_scroll;
        if (r >= 0 && r < rows_vis)
            put(y0 + r, 2 + hcol[h] * cw, cw - 2, CP(C_TITLE) | A_BOLD, "%s", g_log.ch[hidx[h]].group);
    }
    for (int i = 0; i < g_log.nchan; i++) {
        int r = g_row[i] - u->log_scroll;
        if (r >= 0 && r < rows_vis)
            chan_row(s, i, y0 + r, 2 + g_col[i] * cw, cw - 2, i == u->log_sel);
    }
    if (maxrow > rows_vis) {
        char m[32];
        sfmt(m, sizeof m, " %s more ", U ? "↑↓" : "up/down");
        put(cy + chh - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}
