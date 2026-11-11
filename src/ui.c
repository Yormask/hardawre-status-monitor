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
/* ui.c - header, footer, help, the signal dialog, key handling and view dispatch */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void ui_message(ui_state_t *u, int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->msg, sizeof u->msg, fmt, ap);
    va_end(ap);
    u->msg_err = err;
    u->msg_until = now_sec() + 5.0;
}

/* ======================================================== header/footer */

static void header(sys_t *s, ui_state_t *u)
{
    int x = 0;
    x += put(0, x, COLS - x, CP(C_HDR) | A_BOLD, " %s ", APP_NAME);
    x += 1;
    for (int v = 0; v < VIEW_COUNT; v++) {
        attr_t a = v == u->view ? CP(C_TITLE) | A_BOLD | A_REVERSE : A_GRAY;
        x += put(0, x, COLS - x, a, " %d %s ", v + 1, VIEW_NAMES[v]);
    }
    x += 2;
    char clock[16], up[24];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(clock, sizeof clock, "%H:%M:%S", &tm);
    int cw = (int)strlen(clock), xm = COLS - cw - 2;
    fmt_duration(s->cpu.uptime, up, sizeof up);
    kv(0, &x, xm, A_BOLD, "", "%s", s->host);
    kv(0, &x, xm, A_NORMAL, "", "%s", s->os);
    kv(0, &x, xm, A_NORMAL, "kernel", "%s", s->kernel);
    kv(0, &x, xm, A_NORMAL, "up", "%s", up);
    put(0, COLS - cw - 1, cw, A_BOLD, "%s", clock);
}

static void footer(ui_state_t *u)
{
    int y = LINES - 1, x = 0;
    if (u->filter_edit) {
        x += put(y, x, COLS, CP(C_KEY), " Filter ");
        x += put(y, x, COLS - x, A_BOLD, " %s", u->filter);
        x += put(y, x, 1, A_REVERSE, " ");
        put(y, x, COLS - x, A_GRAY, "   Enter: keep   Esc: clear");
        return;
    }
    char st[64] = "";
    if (u->paused)
        app(st, sizeof st, "PAUSED ");
    app(st, sizeof st, "%.2gs ", u->interval);
    int sw = (int)strlen(st), xm = COLS - sw - 1;
    put(y, COLS - sw - 1, sw, u->paused ? CP(C_CRIT) | A_BOLD | A_REVERSE : A_GRAY, "%s", st);

    if (u->msg[0] && now_sec() < u->msg_until) {
        put(y, 0, xm, u->msg_err ? CP(C_CRIT) | A_BOLD : CP(C_OK) | A_BOLD, " %s", u->msg);
        return;
    }

    static const char *const keys[][2] = {
        { "q", "Quit" },  { "?", "Help" },        { "s", "Setup" },    { "x", "Kill" },
        { "1-4", "View" }, { "c/m/d/p/n/t", "Sort" }, { "/", "Filter" }, { "b", "CPU bars" },
        { "k", "Kthreads" }, { "f", "Follow" },  { "+/-", "Interval" }, { "Spc", "Pause" },
    };
    static const char *const skeys[][2] = {
        { "Up/Dn", "Select" }, { "Space", "Show/hide" }, { "Left/Right", "Column / value" },
        { "[ ]", "Move" },     { "R", "Defaults" },      { "Esc", "Back" },  { "q", "Quit" },
    };
    const char *const(*k)[2] = u->view == VIEW_SETUP ? skeys : keys;
    size_t nk = u->view == VIEW_SETUP ? sizeof skeys / sizeof skeys[0] : sizeof keys / sizeof keys[0];
    for (size_t i = 0; i < nk && x < xm; i++) {
        int need = (int)strlen(k[i][0]) + (int)strlen(k[i][1]) + 3;
        if (x + need > xm)
            break;
        x += put(y, x, xm - x, CP(C_KEY), "%s", k[i][0]);
        x += put(y, x, xm - x, A_NORMAL, " %s  ", k[i][1]);
    }
}

static void help_overlay(void)
{
    static const char *const L[][2] = {
        { "1 2 3 4, Tab", "Dashboard / Processes / Hardware / Setup" },
        { "s, F2", "Setup: choose, arrange and style the panels" },
        { "Up Down PgUp PgDn", "move the selection or scroll" },
        { "Home End", "jump to top / bottom" },
        { "x, F9", "send a signal to (kill) the selected process" },
        { "c m d p n t", "sort: CPU, memory, disk I/O, PID, name, time" },
        { "", "(press the same key again to reverse)" },
        { "/", "filter by name, command, user or PID" },
        { "Esc", "clear the filter" },
        { "f", "selection sticks to the process, not the row" },
        { "k", "show / hide kernel threads" },
        { "b", "CPU core bars: usage / clock / temperature" },
        { "+ -", "slower / faster refresh" },
        { "Space", "pause / resume sampling" },
        { "q, F10", "quit" },
    };
    int n = (int)(sizeof L / sizeof L[0]);
    int w = MIN(76, COLS), h = MIN(n + 9, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2);
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Help", APP_NAME " " APP_VERSION "  (any key closes)");
    for (int i = 0; i < n && 2 + i < h - 6; i++) {
        put(y + 2 + i, x + 3, 18, CP(C_INFO) | A_BOLD, "%s", L[i][0]);
        put(y + 2 + i, x + 22, w - 25, A_NORMAL, "%s", L[i][1]);
    }
    put(y + h - 5, x + 3, w - 6, A_GRAY, "Run as root for CPU power (RAPL), exact Intel GPU load and");
    put(y + h - 4, x + 3, w - 6, A_GRAY, "disk I/O or signals for other users' processes.");
    put(y + h - 3, x + 3, w - 6, A_BOLD, "%s (%s)  %s", APP_NAME, APP_LONG_NAME, APP_COPYRIGHT);
    put(y + h - 2, x + 3, w - 6, A_GRAY, "ABSOLUTELY NO WARRANTY. Free software: GPL v3 or later (see LICENSE).");
}

/* ======================================================== signal dialog */

static const struct {
    int sig;
    const char *name, *desc;
} SIGS[] = {
    { SIGTERM, "SIGTERM", "ask it to quit (normal, safe)" },
    { SIGKILL, "SIGKILL", "force kill, can't be ignored" },
    { SIGINT, "SIGINT", "interrupt, like Ctrl+C" },
    { SIGHUP, "SIGHUP", "hang up (many services reload)" },
    { SIGSTOP, "SIGSTOP", "pause / freeze the process" },
    { SIGCONT, "SIGCONT", "resume a paused process" },
};
#define NSIGS ((int)(sizeof SIGS / sizeof SIGS[0]))

static void kill_dialog(ui_state_t *u)
{
    int w = MIN(66, COLS), h = MIN(NSIGS + 9, LINES);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2), iw = w - 6;
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Send signal", NULL);
    int xx = x + 3;
    xx += put(y + 2, xx, iw, A_GRAY, "PID ");
    xx += put(y + 2, xx, x + 3 + iw - xx, A_BOLD, "%d", (int)u->kill_pid);
    put(y + 2, xx, x + 3 + iw - xx, A_GRAY, "  user %s", u->kill_user);
    put(y + 3, x + 3, iw, A_NORMAL, "%s", u->kill_desc);
    if (u->kill_kthread)
        put(y + 4, x + 3, iw, CP(C_WARN), "Kernel thread: it ignores most signals.");
    else if (u->kill_pid == getpid())
        put(y + 4, x + 3, iw, CP(C_WARN), "This is %s itself.", APP_NAME);
    else if (u->kill_pid == 1)
        put(y + 4, x + 3, iw, CP(C_WARN), "PID 1 is the init system: the kernel protects it.");
    for (int i = 0; i < NSIGS && 5 + i < h - 2; i++) {
        int yy = y + 5 + i, sel = i == u->kill_sel;
        attr_t a = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, x + 2, w - 4, a);
        int cx = x + 3;
        cx += put(yy, cx, iw, a | A_BOLD, "%2d %-8s", SIGS[i].sig, SIGS[i].name);
        put(yy, cx + 1, x + 3 + iw - cx - 1, sel ? a : (i == 1 ? CP(C_CRIT) : A_GRAY), "%s", SIGS[i].desc);
    }
    put(y + h - 2, x + 3, iw, A_GRAY, "Up/Down choose   Enter send   Esc cancel");
}

static void kill_open(ui_state_t *u)
{
    if (u->view != VIEW_PROCS && !(u->view == VIEW_DASH && u->lay.show_procs)) {
        ui_message(u, 1, "Open the process list first (press 2)");
        return;
    }
    if (!u->sel_pid) {
        ui_message(u, 1, "No process selected");
        return;
    }
    u->kill_open = 1;
    u->kill_sel = 0;
    u->kill_pid = u->sel_pid;
    u->kill_start = u->sel_start;
    u->kill_kthread = u->sel_kthread;
    str_copy(u->kill_desc, sizeof u->kill_desc, u->sel_desc);
    str_copy(u->kill_user, sizeof u->kill_user, u->sel_user);
}

static void kill_key(ui_state_t *u, int ch)
{
    switch (ch) {
    case KEY_UP: u->kill_sel = (u->kill_sel + NSIGS - 1) % NSIGS; break;
    case KEY_DOWN: u->kill_sel = (u->kill_sel + 1) % NSIGS; break;
    case '\n':
    case '\r':
    case KEY_ENTER: {
        int i = u->kill_sel;
        int rc = proc_send_signal(u->kill_pid, u->kill_start, SIGS[i].sig);
        char who[40];
        str_copy(who, sizeof who, u->kill_desc);
        if (!rc)
            ui_message(u, 0, "Sent %s to %d (%s)", SIGS[i].name, (int)u->kill_pid, who);
        else if (rc == EPERM)
            ui_message(u, 1, "Permission denied: PID %d belongs to %s. Run hsm with sudo to signal it.",
                       (int)u->kill_pid, u->kill_user);
        else if (rc == ESRCH)
            ui_message(u, 1, "PID %d has already exited", (int)u->kill_pid);
        else
            ui_message(u, 1, "Could not signal PID %d: %s", (int)u->kill_pid, strerror(rc));
        u->kill_open = 0;
        break;
    }
    case 27:
    case 'q':
    case 'x':
    case KEY_F(9): u->kill_open = 0; break;
    }
}

/* ================================================================= main */

void ui_draw(sys_t *s, ui_state_t *u)
{
    g_lay = &u->lay;
    erase();
    if (COLS < 60 || LINES < 16) {
        put(0, 0, COLS, A_BOLD, "Terminal too small (%dx%d), need at least 60x16", COLS, LINES);
        refresh();
        return;
    }
    header(s, u);
    switch (u->view) {
    case VIEW_DASH: dashboard(s, u); break;
    case VIEW_PROCS: draw_procs(s, u, 1, 0, LINES - 2, COLS, 1); break;
    case VIEW_HW: hardware(s, u); break;
    case VIEW_SETUP: setup_view(s, u); break;
    }
    footer(u);
    if (u->kill_open)
        kill_dialog(u);
    if (u->help)
        help_overlay();
    refresh();
}

static void set_sort(ui_state_t *u, int s)
{
    if (u->sort == s)
        u->reverse ^= 1;
    else {
        u->sort = s;
        u->reverse = 0;
    }
}

int ui_key(ui_state_t *u, int ch)
{
    if (u->filter_edit) {
        size_t len = strlen(u->filter);
        if (ch == 27) {
            u->filter[0] = '\0';
            u->filter_edit = 0;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            u->filter_edit = 0;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len)
                u->filter[len - 1] = '\0';
        } else if (ch >= 32 && ch < 127 && len + 1 < sizeof u->filter) {
            u->filter[len] = (char)ch;
            u->filter[len + 1] = '\0';
        }
        u->sel_idx = u->scroll = 0;
        return 1;
    }
    if (u->kill_open) {
        kill_key(u, ch);
        return 1;
    }
    if (u->help) {
        u->help = 0;
        return 1;
    }
    if (u->view == VIEW_SETUP && setup_key(u, ch))
        return 1;

    static const double steps[] = { 0.25, 0.5, 1, 2, 3, 5, 10 };
    int nsteps = (int)(sizeof steps / sizeof steps[0]), si = 0;
    for (int i = 0; i < nsteps; i++)
        if (u->interval >= steps[i] - 1e-9)
            si = i;
    int page = MAX(1, u->page_rows - 1);
    switch (ch) {
    case 'q':
    case 'Q':
    case KEY_F(10): return 0;
    case '1': u->view = VIEW_DASH; break;
    case '2': u->view = VIEW_PROCS; break;
    case '3': u->view = VIEW_HW; break;
    case '4':
    case 's':
    case KEY_F(2): u->view = VIEW_SETUP; break;
    case '\t': u->view = (u->view + 1) % VIEW_COUNT; break;
    case KEY_BTAB: u->view = (u->view + VIEW_COUNT - 1) % VIEW_COUNT; break;
    case 'x':
    case KEY_F(9): kill_open(u); break;
    case 'b':
        u->lay.cpu_bars = (u->lay.cpu_bars + 1) % CPUBAR_COUNT;
        ui_message(u, 0, "CPU core bars: %s", CPUBAR_NAME[u->lay.cpu_bars]);
        break;
    case 'c': set_sort(u, SORT_CPU); break;
    case 'm': set_sort(u, SORT_MEM); break;
    case 'd': set_sort(u, SORT_IO); break;
    case 'p': set_sort(u, SORT_PID); break;
    case 'n': set_sort(u, SORT_NAME); break;
    case 't': set_sort(u, SORT_TIME); break;
    case 'r': u->reverse ^= 1; break;
    case 'k': u->show_kthreads ^= 1; break;
    case 'f': u->follow ^= 1; break;
    case '/':
        u->filter_edit = 1;
        if (u->view != VIEW_DASH)
            u->view = VIEW_PROCS;
        break;
    case 27: u->filter[0] = '\0'; break;
    case '+':
    case '=': u->interval = steps[MIN(si + 1, nsteps - 1)]; break;
    case '-':
    case '_': u->interval = steps[MAX(si - 1, 0)]; break;
    case ' ': u->paused ^= 1; break;
    case '?':
    case 'h':
    case KEY_F(1): u->help = 1; break;
    case KEY_UP:
        if (u->view == VIEW_HW) u->hw_scroll--; else u->sel_delta--;
        break;
    case KEY_DOWN:
        if (u->view == VIEW_HW) u->hw_scroll++; else u->sel_delta++;
        break;
    case KEY_PPAGE:
        if (u->view == VIEW_HW) u->hw_scroll -= 10; else u->sel_delta -= page;
        break;
    case KEY_NPAGE:
        if (u->view == VIEW_HW) u->hw_scroll += 10; else u->sel_delta += page;
        break;
    case KEY_HOME:
        if (u->view == VIEW_HW) u->hw_scroll = 0; else u->sel_delta = -1000000;
        break;
    case KEY_END:
        if (u->view == VIEW_HW) u->hw_scroll = 1 << 20; else u->sel_delta = 1000000;
        break;
    }
    if (u->hw_scroll < 0)
        u->hw_scroll = 0;
    return 1;
}
