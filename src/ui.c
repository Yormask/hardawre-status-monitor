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

/* ui.c - ncurses dashboard, process table and hardware view */
#define _GNU_SOURCE
#include "ui.h"

#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static int U = 1;         /* UTF-8 drawing characters available */
static attr_t A_GRAY;      /* dim text */
static const char *SEP;    /* " · " */
static const char *DEG;    /* "°C" */

static const char *const BLK[9] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static const char *const HB[8] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };

enum { C_TITLE = 1, C_BORDER, C_OK, C_WARN, C_CRIT, C_GRAY, C_HDR, C_THEAD, C_SEL, C_ACC, C_INFO, C_KEY };
#define CP(n) COLOR_PAIR(n)

static const char *const SORT_NAMES[] = { "CPU%", "MEM%", "DISK I/O", "PID", "NAME", "TIME" };
static const char *const VIEW_NAMES[] = { "Dashboard", "Processes", "Hardware" };

/* ================================================================ setup */

void ui_init(int utf8)
{
    U = utf8;
    SEP = U ? " · " : " | ";
    DEG = U ? "°C" : "C";
    initscr();
    cbreak();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    if (has_colors()) {
        start_color();
        use_default_colors();
        int gray = COLORS >= 16 ? 8 : COLOR_WHITE;
        init_pair(C_TITLE, COLOR_CYAN, -1);
        init_pair(C_BORDER, COLORS >= 16 ? 8 : COLOR_BLUE, -1);
        init_pair(C_OK, COLOR_GREEN, -1);
        init_pair(C_WARN, COLOR_YELLOW, -1);
        init_pair(C_CRIT, COLOR_RED, -1);
        init_pair(C_GRAY, (short)gray, -1);
        init_pair(C_HDR, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_THEAD, COLOR_BLACK, COLOR_GREEN);
        init_pair(C_SEL, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_ACC, COLOR_MAGENTA, -1);
        init_pair(C_INFO, COLOR_CYAN, -1);
        init_pair(C_KEY, COLOR_BLACK, COLOR_CYAN);
        A_GRAY = CP(C_GRAY) | (COLORS < 16 ? A_DIM : 0);
    } else {
        A_GRAY = A_DIM;
    }
}

void ui_end(void)
{
    endwin();
}

/* ============================================================ primitives */

static int put(int y, int x, int maxw, attr_t a, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static int put(int y, int x, int maxw, attr_t a, const char *fmt, ...)
{
    if (maxw <= 0 || y < 0 || y >= LINES || x < 0 || x >= COLS)
        return 0;
    if (x + maxw > COLS)
        maxw = COLS - x;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[utf8_bytes_for_cols(buf, maxw)] = '\0';
    attrset(a);
    mvaddstr(y, x, buf);
    attrset(A_NORMAL);
    return utf8_cols(buf);
}

static void app(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void app(char *b, size_t n, const char *fmt, ...)
{
    size_t l = strlen(b);
    if (l + 1 >= n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + l, n - l, fmt, ap);
    va_end(ap);
}

static void fill(int y, int x, int w, attr_t a)
{
    if (y < 0 || y >= LINES || x >= COLS)
        return;
    if (x + w > COLS)
        w = COLS - x;
    if (w > 0)
        mvhline(y, x, ' ' | a, w);
}

static attr_t pct_attr(double p)
{
    return CP(p >= 85 ? C_CRIT : p >= 60 ? C_WARN : C_OK);
}

static attr_t temp_attr(double t)
{
    return CP(t >= 85 ? C_CRIT : t >= 70 ? C_WARN : C_OK);
}

static void box_(int y, int x, int h, int w, const char *title, const char *info)
{
    if (h < 2 || w < 4)
        return;
    attrset(CP(C_BORDER));
    if (U) {
        mvaddstr(y, x, "╭");
        for (int i = 1; i < w - 1; i++)
            addstr("─");
        addstr("╮");
        for (int r = 1; r < h - 1; r++) {
            mvaddstr(y + r, x, "│");
            mvaddstr(y + r, x + w - 1, "│");
        }
        mvaddstr(y + h - 1, x, "╰");
        for (int i = 1; i < w - 1; i++)
            addstr("─");
        addstr("╯");
    } else { /* plain ASCII works on every terminal and serial console */
        mvaddch(y, x, '+');
        mvhline(y, x + 1, '-', w - 2);
        mvaddch(y, x + w - 1, '+');
        mvvline(y + 1, x, '|', h - 2);
        mvvline(y + 1, x + w - 1, '|', h - 2);
        mvaddch(y + h - 1, x, '+');
        mvhline(y + h - 1, x + 1, '-', w - 2);
        mvaddch(y + h - 1, x + w - 1, '+');
    }
    attrset(A_NORMAL);
    int tx = x + 2, lim = x + w - 2;
    if (title)
        tx += put(y, tx, lim - tx, CP(C_TITLE) | A_BOLD, " %s ", title);
    if (info && *info)
        put(y, tx, lim - tx, A_GRAY, "%s ", info);
}

/* Horizontal bar with 1/8-cell resolution in UTF-8 mode. */
static void bar(int y, int x, int w, double pct, attr_t a)
{
    if (w <= 0 || y < 0 || y >= LINES || x >= COLS)
        return;
    if (x + w > COLS)
        w = COLS - x;
    pct = CLAMP(pct, 0.0, 100.0);
    if (!a)
        a = pct_attr(pct);
    double cells = pct / 100.0 * w;
    int full = (int)cells, part = U ? (int)((cells - full) * 8) : 0, used = full;
    move(y, x);
    attrset(a);
    for (int i = 0; i < full; i++)
        addstr(U ? "█" : "|");
    if (part > 0 && used < w) {
        addstr(HB[part]);
        used++;
    }
    attrset(A_GRAY);
    for (; used < w; used++)
        addstr(U ? "·" : ".");
    attrset(A_NORMAL);
}

/* "Label [bar...........] value" */
static void meter(int y, int x, int w, const char *label, int lw, double pct, const char *val, int vw, attr_t a)
{
    int vc = utf8_cols(val);
    if (vw < vc)
        vw = vc;
    put(y, x, lw, CP(C_INFO), "%s", label);
    int bw = w - lw - vw - 1;
    if (bw > 0)
        bar(y, x + lw, bw, pct, a);
    put(y, x + w - vw, vw, A_BOLD, "%*s", vw, val);
}

/* History graph, newest sample on the right. fixed==0 -> green/yellow/red
 * by height. flip draws downward from the top (for "out" series). */
static void graph(int y, int x, int h, int w, const hist_t *hs, double maxv, attr_t fixed, int flip)
{
    if (h <= 0 || w <= 0)
        return;
    if (maxv <= 0)
        maxv = 1;
    for (int c = 0; c < w; c++) {
        double v = hist_get(hs, w - 1 - c);
        if (v <= 0)
            continue;
        int lv = (int)(CLAMP(v / maxv, 0.0, 1.0) * h * 8 + 0.5);
        if (lv == 0)
            lv = 1;
        for (int r = 0; r < h && lv > r * 8; r++) {
            int f = MIN(lv - r * 8, 8);
            attr_t a = fixed ? fixed : h <= 2 ? pct_attr(100.0 * v / maxv) : pct_attr(100.0 * (r + 0.5) / h);
            int yy = flip ? y + r : y + h - 1 - r;
            if (!U) {
                attrset(a);
                mvaddstr(yy, x + c, f >= 4 ? "|" : ".");
            } else if (flip) { /* reverse video turns a lower block into an upper one */
                attrset(a | A_REVERSE);
                mvaddstr(yy, x + c, BLK[8 - f]);
            } else {
                attrset(a);
                mvaddstr(yy, x + c, BLK[f]);
            }
        }
    }
    attrset(A_NORMAL);
}

/* In-line "key value" pairs; stops cleanly when the line is full. */
static void kv(int y, int *x, int xmax, attr_t va, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
static void kv(int y, int *x, int xmax, attr_t va, const char *key, const char *fmt, ...)
{
    char v[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    int need = (*key ? utf8_cols(key) + 1 : 0) + utf8_cols(v);
    if (*x + need > xmax) {
        *x = xmax;
        return;
    }
    if (*key)
        *x += put(y, *x, xmax - *x, A_GRAY, "%s ", key);
    *x += put(y, *x, xmax - *x, va, "%s", v);
    *x += 2;
}

/* "R ▄▅▇  / W ▀▀▔" style twin graph (reads up, writes hanging down). */
static void dual_graph(int y, int x, int h, int w, const hist_t *up, const hist_t *dn, attr_t ua, attr_t da,
                       const char *ul, const char *dl)
{
    if (h <= 0 || w <= 3)
        return;
    int gx = x + 2, gw = w - 2;
    double mx = MAX(hist_max(up, gw), hist_max(dn, gw)) * 1.05;
    if (mx <= 0)
        mx = 1;
    if (h == 1) {
        graph(y, gx, 1, gw, up, mx, ua, 0);
        put(y, x, 1, ua | A_BOLD, "%s", ul);
        return;
    }
    int uh = (h + 1) / 2, dh = h - uh;
    graph(y, gx, uh, gw, up, mx, ua, 0);
    graph(y + uh, gx, dh, gw, dn, mx, da, 1);
    put(y, x, 1, ua | A_BOLD, "%s", ul);
    put(y + h - 1, x, 1, da | A_BOLD, "%s", dl);
    if (h >= 3) {
        char m[16];
        fmt_rate(mx, m, sizeof m);
        put(y, gx, gw, A_GRAY, "%s", m);
    }
}

/* ================================================================== CPU */

static int cpu_cols(int iw, int ncpu, int max_rows)
{
    if (ncpu <= 1)
        return 1;
    if (max_rows < 1)
        max_rows = 1;
    int cols = (ncpu + max_rows - 1) / max_rows;
    int pref = MAX(iw / 28, 1);
    if (cols < pref)
        cols = pref;
    if (cols > ncpu)
        cols = ncpu;
    while (cols > 1 && iw / cols < 7)
        cols--;
    return cols;
}

static int cpu_rows(int iw, int ncpu, int max_rows)
{
    int c = cpu_cols(iw, ncpu, max_rows);
    return (ncpu + c - 1) / c;
}

static void cpu_cell(int y, int x, int cw, int id, int idw, double pct, double mhz)
{
    int avail = cw - (cw >= 20 ? 3 : 1); /* gap to the next column */
    int show_pct = avail >= idw + 1 + 4 + 5;
    int show_f = mhz > 0 && avail >= idw + 1 + 8 + 5 + 6;
    int bw = avail - idw - 1 - (show_pct ? 5 : 0) - (show_f ? 6 : 0);
    put(y, x, idw, A_GRAY, "%*d", idw, id);
    if (bw > 0)
        bar(y, x + idw + 1, bw, pct, 0);
    int xx = x + idw + 1 + MAX(bw, 0);
    if (show_pct)
        xx += put(y, xx, 5, pct_attr(pct), "%4.0f%%", pct);
    if (show_f)
        put(y, xx, 6, A_GRAY, " %4.1fG", mhz / 1000.0);
}

static void panel_cpu(sys_t *s, int y, int x, int h, int w, int crows)
{
    cpu_t *c = &s->cpu;
    char info[256] = "", v[32], a[16], b[16], d[16];
    app(info, sizeof info, "%s", c->model);
    if (c->threads)
        app(info, sizeof info, "%s%dC/%dT", SEP, c->cores, c->threads);
    box_(y, x, h, w, "CPU", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0 || iw <= 10)
        return;

    /* row 0: total + temperature + package power */
    int right = 0;
    char tbuf[16] = "", pbuf[16] = "";
    if (c->temp_c >= 0) {
        sfmt(tbuf, sizeof tbuf, "%.0f%s", c->temp_c, DEG);
        right += utf8_cols(tbuf) + 2;
    }
    if (s->power.pkg_w >= 0) {
        sfmt(pbuf, sizeof pbuf, "%.1f W", s->power.pkg_w);
        right += utf8_cols(pbuf) + 2;
    }
    sfmt(v, sizeof v, "%5.1f%%", c->total);
    meter(iy, ix, iw - right, "Total", 6, c->total, v, 6, 0);
    int xx = ix + iw - right;
    if (tbuf[0])
        xx += 2 + put(iy, xx + 2, ix + iw - xx - 2, temp_attr(c->temp_c) | A_BOLD, "%s", tbuf);
    if (pbuf[0])
        put(iy, xx + 2, ix + iw - xx - 2, CP(C_WARN) | A_BOLD, "%s", pbuf);
    r++;

    /* row 1: breakdown */
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_si(c->ctxt_rate, a, sizeof a);
        fmt_si(c->intr_rate, b, sizeof b);
        fmt_si(c->fork_rate, d, sizeof d);
        kv(iy + r, &kx, km, A_BOLD, "usr", "%.1f%%", c->user);
        kv(iy + r, &kx, km, A_BOLD, "sys", "%.1f%%", c->sys);
        kv(iy + r, &kx, km, c->iowait >= 5 ? CP(C_WARN) : A_BOLD, "iowait", "%.1f%%", c->iowait);
        if (c->mhz_avg > 0)
            kv(iy + r, &kx, km, A_BOLD, "clock", "%.2f GHz", c->mhz_avg / 1000.0);
        kv(iy + r, &kx, km, A_BOLD, "load", "%.2f %.2f %.2f", c->load[0], c->load[1], c->load[2]);
        if (c->steal > 0.05)
            kv(iy + r, &kx, km, CP(C_WARN), "steal", "%.1f%%", c->steal);
        kv(iy + r, &kx, km, A_BOLD, "ctx/s", "%s", a);
        kv(iy + r, &kx, km, A_BOLD, "irq/s", "%s", b);
        kv(iy + r, &kx, km, A_BOLD, "forks/s", "%s", d);
        r++;
    }

    /* cores at the bottom, history graph fills what is left */
    int avail_rows = ih - r;
    int rows_total = cpu_rows(iw, c->ncpu, crows);
    int rows_vis = MIN(rows_total, avail_rows);
    int gh = avail_rows - rows_vis;
    if (gh > 0)
        graph(iy + r, ix, gh, iw, &s->h_cpu, 100, 0, 0);
    int cy = iy + r + MAX(gh, 0);
    int cols = cpu_cols(iw, c->ncpu, crows);
    int cw = iw / cols, maxid = 0;
    for (int i = 0; i < c->ncpu; i++)
        maxid = MAX(maxid, c->id[i]);
    int idw = maxid >= 100 ? 3 : maxid >= 10 ? 2 : 1;
    for (int i = 0; i < c->ncpu; i++) {
        int col = i / rows_total, row = i % rows_total;
        if (row >= rows_vis)
            continue;
        cpu_cell(cy + row, ix + col * cw, cw, c->id[i], idw, c->pct[i], c->mhz[i]);
    }
}

/* ================================================================== GPU */

static int gpu_block(const gpu_t *g, int multi)
{
    return multi + 1 + (g->mem_total > 0) + 1;
}

static int gpu_need(sys_t *s)
{
    if (!s->gpus.n)
        return 3;
    int n = 2;
    for (int i = 0; i < s->gpus.n; i++)
        n += gpu_block(&s->gpus.g[i], s->gpus.n > 1);
    return n;
}

static void gpu_details(int y, int x, int w, const gpu_t *g)
{
    int xx = x, xm = x + w;
    if (g->temp_c >= 0)
        kv(y, &xx, xm, temp_attr(g->temp_c) | A_BOLD, "", "%.0f%s", g->temp_c, DEG);
    if (g->power_w >= 0) {
        if (g->power_cap_w > 0)
            kv(y, &xx, xm, CP(C_WARN) | A_BOLD, "", "%.1f/%.0f W", g->power_w, g->power_cap_w);
        else
            kv(y, &xx, xm, CP(C_WARN) | A_BOLD, "", "%.1f W", g->power_w);
    }
    if (g->clk_mhz >= 0) {
        if (g->clk_max_mhz > 0)
            kv(y, &xx, xm, A_BOLD, "clock", "%.0f/%.0f MHz", g->clk_mhz, g->clk_max_mhz);
        else
            kv(y, &xx, xm, A_BOLD, "clock", "%.0f MHz", g->clk_mhz);
    }
    if (g->mem_clk_mhz > 0)
        kv(y, &xx, xm, A_BOLD, "mem", "%.0f MHz", g->mem_clk_mhz);
    if (g->fan_pct >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f%%", g->fan_pct);
    else if (g->fan_rpm >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f rpm", g->fan_rpm);
    for (int i = 0; i < g->neng; i++)
        kv(y, &xx, xm, pct_attr(g->eng[i].pct), g->eng[i].name, "%.0f%%", g->eng[i].pct);
}

static void panel_gpu(sys_t *s, int y, int x, int h, int w)
{
    gpus_t *G = &s->gpus;
    char info[128] = "";
    if (G->n == 1)
        str_copy(info, sizeof info, G->g[0].name);
    else if (G->n > 1)
        sfmt(info, sizeof info, "%d GPUs", G->n);
    box_(y, x, h, w, "GPU", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!G->n) {
        put(iy, ix, iw, A_GRAY, "No GPU found in /sys/class/drm");
        return;
    }
    int multi = G->n > 1, graph_gpu = -1;
    for (int k = 0; k < G->n && r < ih; k++) {
        gpu_t *g = &G->g[k];
        if (multi)
            put(iy + r++, ix, iw, A_BOLD, "%d %s", k, g->name);
        if (r >= ih)
            break;
        if (g->util >= 0) {
            char v[16];
            sfmt(v, sizeof v, "%5.1f%%", g->util);
            meter(iy + r++, ix, iw, g->util_est ? "Load~" : "Load", 6, g->util, v, 6, 0);
            if (graph_gpu < 0)
                graph_gpu = k;
        } else {
            int xx = ix + put(iy + r, ix, iw, CP(C_INFO), "Load  ");
            put(iy + r++, xx, ix + iw - xx, CP(C_WARN), "%s", g->note[0] ? g->note : "n/a");
        }
        if (r < ih && g->mem_total > 0) {
            char a[16], b[16], v[40];
            fmt_bytes(g->mem_used, a, sizeof a);
            fmt_bytes(g->mem_total, b, sizeof b);
            sfmt(v, sizeof v, "%s/%s", a, b);
            meter(iy + r++, ix, iw, "VRAM", 6, 100.0 * g->mem_used / g->mem_total, v, 11, 0);
        }
        if (r < ih)
            gpu_details(iy + r++, ix, iw, g);
    }
    if (graph_gpu >= 0 && r < ih)
        graph(iy + r, ix, ih - r, iw, &s->h_gpu[graph_gpu], 100, 0, 0);
}

/* =============================================================== Memory */

static void panel_mem(sys_t *s, int y, int x, int h, int w)
{
    mem_t *m = &s->mem;
    char a[16], b[16], v[48], info[64];
    fmt_bytes((double)m->total, a, sizeof a);
    sfmt(info, sizeof info, "%s total", a);
    box_(y, x, h, w, "Memory", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;

    double pct = m->total ? 100.0 * (double)m->used / (double)m->total : 0;
    fmt_bytes((double)m->used, a, sizeof a);
    fmt_bytes((double)m->total, b, sizeof b);
    sfmt(v, sizeof v, "%s/%s", a, b);
    meter(iy + r++, ix, iw, "RAM", 5, pct, v, 11, 0);

    if (r < ih) {
        if (m->swap_total) {
            fmt_bytes((double)m->swap_used, a, sizeof a);
            fmt_bytes((double)m->swap_total, b, sizeof b);
            sfmt(v, sizeof v, "%s/%s", a, b);
            meter(iy + r, ix, iw, "Swap", 5, 100.0 * (double)m->swap_used / (double)m->swap_total, v, 11, 0);
        } else {
            put(iy + r, ix, 5, CP(C_INFO), "Swap");
            put(iy + r, ix + 5, iw - 5, A_GRAY, "none");
        }
        r++;
    }
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_bytes((double)m->buffcache, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "cache", "%s", a);
        fmt_bytes((double)m->avail, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "avail", "%s", a);
        fmt_bytes((double)m->shmem, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "shm", "%s", a);
        fmt_bytes((double)m->dirty, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "dirty", "%s", a);
        r++;
    }
    if (r < ih) {
        int kx = ix, km = ix + iw;
        fmt_rate(m->swapin_bps, a, sizeof a);
        fmt_rate(m->swapout_bps, b, sizeof b);
        kv(iy + r, &kx, km, m->swapin_bps > 0 ? CP(C_WARN) : A_BOLD, "swap in", "%s", a);
        kv(iy + r, &kx, km, m->swapout_bps > 0 ? CP(C_WARN) : A_BOLD, "out", "%s", b);
        fmt_si(m->majflt_rate, a, sizeof a);
        kv(iy + r, &kx, km, A_BOLD, "majflt/s", "%s", a);
        r++;
    }
    if (r < ih)
        graph(iy + r, ix, ih - r, iw, &s->h_mem, 100, 0, 0);
}

/* ================================================================ Power */

typedef struct {
    char label[64], val[32];
    attr_t va;
    int sub;
} pline_t;

static int pl_add(pline_t *L, int n, int max, attr_t va, int sub, const char *label, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
static int pl_add(pline_t *L, int n, int max, attr_t va, int sub, const char *label, const char *fmt, ...)
{
    if (n >= max)
        return n;
    str_copy(L[n].label, sizeof L[n].label, label);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(L[n].val, sizeof L[n].val, fmt, ap);
    va_end(ap);
    L[n].va = va;
    L[n].sub = sub;
    return n + 1;
}

static int build_power(sys_t *s, pline_t *L, int max)
{
    power_t *p = &s->power;
    int n = 0;
    if (p->pkg_w >= 0) {
        n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, "CPU package", "%.1f W", p->pkg_w);
        if (p->core_w >= 0)
            n = pl_add(L, n, max, A_BOLD, 1, "cores", "%.1f W", p->core_w);
        if (p->uncore_w >= 0)
            n = pl_add(L, n, max, A_BOLD, 1, "iGPU / uncore", "%.1f W", p->uncore_w);
    } else if (p->rapl_denied) {
        n = pl_add(L, n, max, CP(C_WARN), 0, "CPU (RAPL)", "%s", "needs root");
    }
    if (p->dram_w >= 0)
        n = pl_add(L, n, max, A_BOLD, 0, "DRAM", "%.1f W", p->dram_w);
    if (p->psys_w >= 0)
        n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, "Platform (psys)", "%.1f W", p->psys_w);
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (g->power_w < 0 || (g->integrated && !strcmp(g->vendor, "Intel") && p->uncore_w >= 0))
            continue; /* Intel iGPU already shown as uncore */
        char lab[48];
        sfmt(lab, sizeof lab, "GPU%d %s", i, g->vendor);
        if (g->power_cap_w > 0)
            n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, lab, "%.1f/%.0f W", g->power_w, g->power_cap_w);
        else
            n = pl_add(L, n, max, CP(C_WARN) | A_BOLD, 0, lab, "%.1f W", g->power_w);
    }
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        char lab[64] = "", tl[24] = "";
        app(lab, sizeof lab, "%s", b->name);
        if (b->capacity >= 0)
            app(lab, sizeof lab, " %.0f%%", b->capacity);
        app(lab, sizeof lab, " %s", b->status);
        if (b->time_left > 0) {
            fmt_duration(b->time_left, tl, sizeof tl);
            app(lab, sizeof lab, "%s%s", SEP, tl);
        }
        attr_t va = !strcmp(b->status, "Discharging") ? CP(C_WARN) | A_BOLD : CP(C_OK) | A_BOLD;
        if (b->watts >= 0)
            n = pl_add(L, n, max, va, 0, lab, "%.1f W", b->watts);
        else
            n = pl_add(L, n, max, va, 0, lab, "%s", "");
    }
    if (p->ac_online >= 0 && p->nbat)
        n = pl_add(L, n, max, A_BOLD, 0, "AC adapter", "%s", p->ac_online ? "online" : "offline");
    if (!n)
        n = pl_add(L, n, max, A_GRAY, 0, "No power sensors found", "%s", "");
    return n;
}

static void panel_power(sys_t *s, int y, int x, int h, int w, const pline_t *L, int n)
{
    char info[64] = "";
    if (s->total_w >= 0)
        sfmt(info, sizeof info, "%s%.1f W (%s)", U ? "≈ " : "~", s->total_w, s->total_w_src);
    box_(y, x, h, w, "Power", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    for (int i = 0; i < n && r < ih; i++, r++) {
        int vw = utf8_cols(L[i].val);
        if (L[i].sub)
            put(iy + r, ix, iw - vw - 1, A_GRAY, "  %s", L[i].label);
        else
            put(iy + r, ix, iw - vw - 1, CP(C_INFO), "%s", L[i].label);
        put(iy + r, ix + iw - vw, vw, L[i].va ? L[i].va : A_NORMAL, "%s", L[i].val);
    }
    if (r < ih) {
        double mx = hist_max(&s->h_pw, iw) * 1.1;
        graph(iy + r, ix, ih - r, iw, &s->h_pw, mx, CP(C_WARN), 0);
        if (ih - r >= 2 && mx > 0)
            put(iy + r, ix, iw, A_GRAY, "%.0f W", mx);
    }
}

/* ============================================================ Disk / Net */

static void panel_disk(sys_t *s, int y, int x, int h, int w)
{
    disks_t *D = &s->disks;
    char a[16], b[16], info[64];
    fmt_rate(D->rd_bps, a, sizeof a);
    fmt_rate(D->wr_bps, b, sizeof b);
    sfmt(info, sizeof info, "R %s  W %s", a, b);
    box_(y, x, h, w, "Disk I/O", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!D->n)
        put(iy + r++, ix, iw, A_GRAY, "No physical drives found");
    int nw = iw >= 44 ? 10 : 8;
    for (int i = 0; i < D->n && r < ih && i < 4; i++, r++) {
        disk_t *d = &D->d[i];
        int xx = ix, xm = ix + iw;
        put(iy + r, xx, nw, A_BOLD, "%s", d->name);
        xx += nw;
        fmt_rate(d->rd_bps, a, sizeof a);
        fmt_rate(d->wr_bps, b, sizeof b);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "R ");
        xx += put(iy + r, xx, xm - xx, d->rd_bps > 0 ? CP(C_INFO) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  W ");
        xx += put(iy + r, xx, xm - xx, d->wr_bps > 0 ? CP(C_ACC) | A_BOLD : A_GRAY, "%7s", b);
        if (xm - xx >= 6)
            xx += put(iy + r, xx, xm - xx, pct_attr(d->util), " %4.0f%%", d->util);
        if (xm - xx >= 14) {
            fmt_si(d->rd_iops + d->wr_iops, a, sizeof a);
            put(iy + r, xx, xm - xx, A_GRAY, "  %s IOPS", a);
        }
    }
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_dr, &s->h_dw, CP(C_INFO), CP(C_ACC), "R", "W");
}

static int net_pick(sys_t *s, int *idx, int max)
{
    int n = 0;
    for (int pass = 0; pass < 3 && n < max; pass++)
        for (int i = 0; i < s->net.n && n < max; i++) {
            netif_t *f = &s->net.i[i];
            int ok = 0;
            if (pass == 0) /* physical NICs that are up */
                ok = !f->is_virtual && f->up;
            else if (pass == 1) /* VPNs, bridges with traffic (skip veth noise) */
                ok = f->is_virtual && f->up && f->rx_total + f->tx_total > 0 && strncmp(f->name, "veth", 4);
            else if (n == 0) /* nothing yet: show anything */
                ok = 1;
            if (ok)
                idx[n++] = i;
        }
    return n;
}

static void panel_net(sys_t *s, int y, int x, int h, int w, const int *idx, int nn)
{
    char a[16], b[16], info[64];
    fmt_rate(s->net.rx_bps, a, sizeof a);
    fmt_rate(s->net.tx_bps, b, sizeof b);
    sfmt(info, sizeof info, "%s %s  %s %s", U ? "↓" : "in", a, U ? "↑" : "out", b);
    box_(y, x, h, w, "Network", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2, r = 0;
    if (ih <= 0)
        return;
    if (!nn)
        put(iy + r++, ix, iw, A_GRAY, "No network interfaces");
    int nw = iw >= 44 ? 10 : 8;
    for (int k = 0; k < nn && r < ih; k++, r++) {
        netif_t *f = &s->net.i[idx[k]];
        int xx = ix, xm = ix + iw;
        put(iy + r, xx, nw, f->up ? A_BOLD : A_GRAY, "%s", f->name);
        xx += nw;
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "%s ", U ? "↓" : "in");
        xx += put(iy + r, xx, xm - xx, f->rx_bps > 0 ? CP(C_INFO) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  %s ", U ? "↑" : "out");
        xx += put(iy + r, xx, xm - xx, f->tx_bps > 0 ? CP(C_ACC) | A_BOLD : A_GRAY, "%7s", b);
        if (f->addr[0] && xm - xx >= (int)strlen(f->addr) + 2)
            put(iy + r, xx, xm - xx, A_GRAY, "  %s", f->addr);
        else if (f->speed > 0 && xm - xx >= 10)
            put(iy + r, xx, xm - xx, A_GRAY, "  %dM", f->speed);
    }
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_rx, &s->h_tx, CP(C_INFO), CP(C_ACC), U ? "↓" : "v", U ? "↑" : "^");
}

/* ============================================================ Processes */

enum { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
static const struct {
    const char *h;
    int w, right, sort;
} COLDEF[] = {
    [K_PID] = { "PID", 7, 1, SORT_PID },   [K_USER] = { "USER", 9, 0, -1 },    [K_PRI] = { "PRI", 3, 1, -1 },
    [K_NI] = { "NI", 3, 1, -1 },           [K_THR] = { "THR", 4, 1, -1 },      [K_S] = { "S", 1, 0, -1 },
    [K_CPU] = { "CPU%", 5, 1, SORT_CPU },  [K_MEM] = { "MEM%", 5, 1, SORT_MEM }, [K_VIRT] = { "VIRT", 6, 1, -1 },
    [K_RSS] = { "RSS", 6, 1, SORT_MEM },   [K_RD] = { "READ/s", 8, 1, SORT_IO }, [K_WR] = { "WRITE/s", 8, 1, SORT_IO },
    [K_TIME] = { "TIME+", 9, 1, SORT_TIME }, [K_CMD] = { "COMMAND", 0, 0, SORT_NAME },
};

static int g_sort, g_rev;
static proc_t *g_pa;

static int cmp_dbl_desc(double a, double b)
{
    return (a < b) - (a > b);
}

static int cmp_proc(const void *a, const void *b)
{
    const proc_t *p = &g_pa[*(const int *)a], *q = &g_pa[*(const int *)b];
    int r = 0;
    switch (g_sort) {
    case SORT_CPU: r = cmp_dbl_desc(p->cpu, q->cpu); break;
    case SORT_MEM: r = cmp_dbl_desc((double)p->rss, (double)q->rss); break;
    case SORT_IO: r = cmp_dbl_desc(p->rd_bps + p->wr_bps, q->rd_bps + q->wr_bps); break;
    case SORT_PID: r = (p->pid > q->pid) - (p->pid < q->pid); break;
    case SORT_NAME: r = strcasecmp(p->comm, q->comm); break;
    case SORT_TIME: r = cmp_dbl_desc((double)p->ticks, (double)q->ticks); break;
    }
    if (!r && g_sort != SORT_MEM)
        r = cmp_dbl_desc((double)p->rss, (double)q->rss);
    if (!r)
        r = (p->pid > q->pid) - (p->pid < q->pid);
    return g_rev ? -r : r;
}

static int proc_match(proc_t *p, const char *f)
{
    if (!f[0])
        return 1;
    if (strcasestr(p->comm, f) || strcasestr(uid_name(p->uid), f))
        return 1;
    if (!p->kthread && strcasestr(proc_cmdline(p), f))
        return 1;
    char b[16];
    sfmt(b, sizeof b, "%d", (int)p->pid);
    return !strcmp(b, f);
}

static void fmt_time(unsigned long long ticks, long hz, char *out, size_t n)
{
    double s = (double)ticks / (double)hz;
    long t = (long)s;
    if (t < 3600)
        sfmt(out, n, "%ld:%05.2f", t / 60, s - (double)(t / 60 * 60));
    else
        sfmt(out, n, "%ldh%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

static void cell_text(proc_t *p, int k, long hz, char *o, size_t n, attr_t *a)
{
    *a = A_NORMAL;
    switch (k) {
    case K_PID: sfmt(o, n, "%d", (int)p->pid); break;
    case K_USER:
        str_copy(o, n, uid_name(p->uid));
        if (p->uid == 0)
            *a = A_GRAY;
        break;
    case K_PRI: sfmt(o, n, "%d", p->prio); break;
    case K_NI:
        sfmt(o, n, "%d", p->nice);
        *a = p->nice < 0 ? CP(C_CRIT) : p->nice > 0 ? CP(C_OK) : A_GRAY;
        break;
    case K_THR: sfmt(o, n, "%d", p->nthreads); break;
    case K_S:
        sfmt(o, n, "%c", p->state);
        *a = p->state == 'R' ? CP(C_OK) | A_BOLD
           : p->state == 'D' || p->state == 'Z' ? CP(C_CRIT) | A_BOLD
           : p->state == 'T' ? CP(C_WARN) : A_GRAY;
        break;
    case K_CPU:
        sfmt(o, n, "%.1f", p->cpu);
        *a = p->cpu < 0.05 ? A_GRAY : p->cpu >= 85 ? CP(C_CRIT) | A_BOLD : p->cpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD;
        break;
    case K_MEM:
        sfmt(o, n, "%.1f", p->mem);
        *a = p->mem < 0.05 ? A_GRAY : p->mem >= 25 ? CP(C_WARN) | A_BOLD : A_NORMAL;
        break;
    case K_VIRT: fmt_bytes((double)p->vsize, o, n); *a = A_GRAY; break;
    case K_RSS: fmt_bytes((double)p->rss, o, n); break;
    case K_RD:
    case K_WR: {
        double v = k == K_RD ? p->rd_bps : p->wr_bps;
        if (!p->io_ok) {
            str_copy(o, n, p->kthread ? "" : "-");
            *a = A_GRAY;
        } else if (v <= 0) {
            str_copy(o, n, "0");
            *a = A_GRAY;
        } else {
            fmt_rate(v, o, n);
            *a = (k == K_RD ? CP(C_INFO) : CP(C_ACC)) | A_BOLD;
        }
        break;
    }
    case K_TIME: fmt_time(p->ticks, hz, o, n); *a = A_GRAY; break;
    default: o[0] = '\0';
    }
}

static void draw_cmd(int y, int x, int w, proc_t *p, int full, attr_t base, int sel)
{
    if (w <= 0)
        return;
    const char *cmd = p->kthread ? "" : proc_cmdline(p);
    if (!*cmd) {
        put(y, x, w, sel ? base : A_GRAY, "[%s]", p->comm);
        return;
    }
    size_t a0 = strcspn(cmd, " ");
    const char *slash = memrchr(cmd, '/', a0);
    const char *bn = slash ? slash + 1 : cmd;
    int xx = x;
    if (full && bn > cmd)
        xx += put(y, xx, x + w - xx, sel ? base : A_GRAY, "%.*s", (int)(bn - cmd), cmd);
    xx += put(y, xx, x + w - xx, base | A_BOLD, "%.*s", (int)(cmd + a0 - bn), bn);
    put(y, xx, x + w - xx, sel ? base : A_NORMAL, "%s", cmd + a0);
}

static void draw_procs(sys_t *s, ui_state_t *u, int y, int x, int h, int w, int full)
{
    procs_t *ps = &s->procs;
    static int *ord;
    static int ocap;
    if (h < 3)
        return;
    if (ocap < ps->n) {
        int nc = ps->n + 256;
        int *no = realloc(ord, (size_t)nc * sizeof *no);
        if (!no)
            return;
        ord = no;
        ocap = nc;
    }
    int n = 0;
    for (int i = 0; i < ps->n; i++) {
        proc_t *p = &ps->p[i];
        if (p->kthread && !u->show_kthreads)
            continue;
        if (!proc_match(p, u->filter))
            continue;
        ord[n++] = i;
    }
    g_sort = u->sort;
    g_rev = u->reverse;
    g_pa = ps->p;
    qsort(ord, (size_t)n, sizeof *ord, cmp_proc);

    char info[256] = "";
    app(info, sizeof info, "%d tasks, %d thr, %d running", ps->n, ps->threads, ps->running);
    if (ps->diskwait)
        app(info, sizeof info, ", %d D", ps->diskwait);
    if (ps->zombie)
        app(info, sizeof info, ", %d zombie", ps->zombie);
    app(info, sizeof info, "%ssort %s%s", SEP, SORT_NAMES[u->sort], u->reverse ? " (rev)" : "");
    if (u->follow)
        app(info, sizeof info, "%sfollow", SEP);
    if (u->filter[0])
        app(info, sizeof info, "%sfilter \"%s\": %d", SEP, u->filter, n);
    if (ps->io_denied && geteuid() != 0)
        app(info, sizeof info, "%sI/O of other users' processes needs root", SEP);
    box_(y, x, h, w, "Processes", info);

    int ix = x + 1, iw = w - 2, iy = y + 1, ih = h - 2, rows = ih - 1;
    if (rows < 1)
        return;
    u->page_rows = rows;

    /* selection: by row position, or glued to the process with follow on */
    int sel = -1;
    if (u->follow && u->sel_pid)
        for (int k = 0; k < n; k++)
            if (ps->p[ord[k]].pid == u->sel_pid) {
                sel = k;
                break;
            }
    if (sel < 0)
        sel = u->sel_idx;
    sel += u->sel_delta;
    u->sel_delta = 0;
    if (sel >= n)
        sel = n - 1;
    if (sel < 0)
        sel = 0;
    u->sel_idx = sel;
    u->sel_pid = n ? ps->p[ord[sel]].pid : 0;
    if (sel < u->scroll)
        u->scroll = sel;
    if (sel >= u->scroll + rows)
        u->scroll = sel - rows + 1;
    u->scroll = CLAMP(u->scroll, 0, MAX(0, n - rows));

    /* choose columns, dropping the least important ones on narrow screens */
    int cols[16], nc = 0;
    if (full) {
        static const int fc[] = { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof fc / sizeof fc[0]; i++)
            cols[nc++] = fc[i];
    } else {
        static const int dc[] = { K_PID, K_USER, K_S, K_CPU, K_MEM, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof dc / sizeof dc[0]; i++)
            cols[nc++] = dc[i];
    }
    static const int drop[] = { K_PRI, K_VIRT, K_NI, K_TIME, K_THR, K_WR, K_RD, K_USER };
    for (size_t d = 0; d < sizeof drop / sizeof drop[0]; d++) {
        int used = 0;
        for (int i = 0; i < nc; i++)
            used += COLDEF[cols[i]].w + 1;
        if (iw - used >= (iw >= 100 ? 40 : 24)) /* room left for COMMAND */
            break;
        for (int i = 0; i < nc; i++)
            if (cols[i] == drop[d]) {
                memmove(&cols[i], &cols[i + 1], (size_t)(nc - i - 1) * sizeof cols[0]);
                nc--;
                break;
            }
    }

    /* header */
    fill(iy, ix, iw, CP(C_THEAD));
    int cx = ix;
    for (int i = 0; i < nc; i++) {
        int k = cols[i], cw = k == K_CMD ? ix + iw - cx : COLDEF[k].w;
        attr_t a = CP(C_THEAD) | (COLDEF[k].sort == u->sort ? A_BOLD | A_UNDERLINE : 0);
        if (COLDEF[k].right)
            put(iy, cx, cw, a, "%*s", cw, COLDEF[k].h);
        else
            put(iy, cx, cw, a, "%-*s", cw, COLDEF[k].h);
        cx += cw + 1;
    }

    /* rows */
    for (int r = 0; r < rows; r++) {
        int k = u->scroll + r;
        if (k >= n)
            break;
        proc_t *p = &ps->p[ord[k]];
        int yy = iy + 1 + r, issel = k == sel;
        attr_t base = issel ? CP(C_SEL) : A_NORMAL;
        if (issel)
            fill(yy, ix, iw, base);
        cx = ix;
        for (int i = 0; i < nc; i++) {
            int c = cols[i];
            if (c == K_CMD) {
                draw_cmd(yy, cx, ix + iw - cx, p, full, base, issel);
                break;
            }
            char t[48];
            attr_t a;
            cell_text(p, c, ps->hz, t, sizeof t, &a);
            if (issel)
                a = base | (a & A_BOLD);
            int cw = COLDEF[c].w;
            if (COLDEF[c].right)
                put(yy, cx, cw, a, "%*s", cw, t);
            else
                put(yy, cx, cw, a, "%-*.*s", cw, cw, t);
            cx += cw + 1;
        }
    }
    if (n > rows) {
        char m[48];
        sfmt(m, sizeof m, " %d-%d of %d ", u->scroll + 1, MIN(n, u->scroll + rows), n);
        put(y + h - 1, x + w - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}

/* ============================================================= Hardware */

typedef struct {
    int y0, x, w, h, line, scroll;
} lc_t;

static int lc_row(lc_t *c)
{
    int r = c->line - c->scroll;
    c->line++;
    return (r >= 0 && r < c->h) ? c->y0 + r : -1;
}

static void lc_text(lc_t *c, attr_t a, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void lc_text(lc_t *c, attr_t a, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(y, c->x, c->w, a, "%s", b);
}

static void lc_title(lc_t *c, const char *t)
{
    if (c->line)
        lc_row(c);
    int y = lc_row(c);
    if (y >= 0) {
        int n = put(y, c->x, c->w, CP(C_TITLE) | A_BOLD, "%s ", t);
        attrset(CP(C_BORDER));
        for (int i = n; i < c->w; i++)
            mvaddstr(y, c->x + i, U ? "─" : "-");
        attrset(A_NORMAL);
    }
}

static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    int kw = MIN(16, c->w / 3);
    put(y, c->x, kw, A_GRAY, "%s", k);
    put(y, c->x + kw, c->w - kw, va, "%s", b);
}

static void hw_sensors(sys_t *s, lc_t *c)
{
    sensors_t *S = &s->sens;
    lc_title(c, "Sensors");
    if (!S->n) {
        lc_text(c, A_GRAY, "No hwmon sensors found in /sys/class/hwmon");
        return;
    }
    int last = -1;
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (t->chip_idx != last) {
            lc_text(c, CP(C_INFO) | A_BOLD, "%s", t->chip);
            last = t->chip_idx;
        }
        int y = lc_row(c);
        if (y < 0)
            continue;
        int lw = MIN(22, c->w / 2), xx = c->x + 2 + lw;
        put(y, c->x + 2, lw, A_NORMAL, "%s", t->label);
        if (!t->ok) {
            put(y, xx, c->w - lw - 2, A_GRAY, "n/a");
            continue;
        }
        if (t->type == SENS_TEMP) {
            xx += put(y, xx, 9, temp_attr(t->value) | A_BOLD, "%5.1f%s", t->value, DEG);
            double ref = t->crit > 0 ? t->crit : 100.0;
            int bw = MIN(20, c->x + c->w - xx - 14);
            if (bw > 3) {
                bar(y, xx + 1, bw, 100.0 * t->value / ref, temp_attr(t->value));
                xx += bw + 1;
            }
            if (t->crit > 0)
                put(y, xx + 1, c->x + c->w - xx - 1, A_GRAY, "crit %.0f%s", t->crit, DEG);
        } else if (t->type == SENS_FAN) {
            put(y, xx, c->w - lw - 2, A_BOLD, "%5.0f RPM", t->value);
        } else {
            put(y, xx, c->w - lw - 2, CP(C_WARN) | A_BOLD, "%6.2f W", t->value);
        }
    }
}

static void hw_gpus(sys_t *s, lc_t *c)
{
    lc_title(c, "Graphics");
    if (!s->gpus.n) {
        lc_text(c, A_GRAY, "No GPU found in /sys/class/drm");
        return;
    }
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        char b[256] = "";
        lc_text(c, A_BOLD, "%s", g->name);
        app(b, sizeof b, "%s", g->driver[0] ? g->driver : "?");
        if (g->pci[0])
            app(b, sizeof b, "%sPCI %s", SEP, g->pci);
        app(b, sizeof b, "%s%s%s", SEP, g->card, g->integrated ? " (integrated)" : "");
        lc_kv(c, "  Driver", A_NORMAL, "%s", b);
        if (g->util >= 0) {
            b[0] = '\0';
            for (int e = 0; e < g->neng; e++)
                app(b, sizeof b, "%s%s %.0f%%", e ? "  " : "   (", g->eng[e].name, g->eng[e].pct);
            if (g->neng)
                app(b, sizeof b, ")");
            lc_kv(c, "  Load", pct_attr(g->util) | A_BOLD, "%s%.1f%%%s", g->util_est ? "~" : "", g->util, b);
        } else {
            lc_kv(c, "  Load", CP(C_WARN), "%s", g->note[0] ? g->note : "n/a");
        }
        if (g->mem_total > 0) {
            char a1[16], a2[16];
            fmt_bytes(g->mem_used, a1, sizeof a1);
            fmt_bytes(g->mem_total, a2, sizeof a2);
            lc_kv(c, "  VRAM", A_BOLD, "%s / %s (%.0f%%)", a1, a2, 100.0 * g->mem_used / g->mem_total);
        }
        if (g->clk_mhz >= 0)
            lc_kv(c, "  Clock", A_BOLD, "%.0f MHz%s", g->clk_mhz, g->clk_max_mhz > 0 ? "" : "");
        if (g->clk_max_mhz > 0)
            lc_kv(c, "  Max clock", A_NORMAL, "%.0f MHz", g->clk_max_mhz);
        if (g->mem_clk_mhz > 0)
            lc_kv(c, "  Memory clock", A_NORMAL, "%.0f MHz", g->mem_clk_mhz);
        if (g->temp_c >= 0)
            lc_kv(c, "  Temperature", temp_attr(g->temp_c) | A_BOLD, "%.1f%s", g->temp_c, DEG);
        if (g->power_w >= 0) {
            if (g->power_cap_w > 0)
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W of %.0f W limit", g->power_w, g->power_cap_w);
            else
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W%s", g->power_w,
                      g->integrated && !strcmp(g->vendor, "Intel") ? "  (RAPL uncore)" : "");
        }
        if (g->fan_pct >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f%%", g->fan_pct);
        else if (g->fan_rpm >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f RPM", g->fan_rpm);
        if (g->util_est)
            lc_kv(c, "  Note", A_GRAY, "%s", "load estimated from RC6 sleep time; run as root for exact per-engine load");
        else if (g->util >= 0 && g->note[0])
            lc_kv(c, "  Note", A_GRAY, "%s", g->note);
    }
}

static void hw_power(sys_t *s, lc_t *c)
{
    power_t *p = &s->power;
    lc_title(c, "Power");
    if (s->total_w >= 0)
        lc_kv(c, "Estimated draw", CP(C_WARN) | A_BOLD, "%.1f W  (%s)", s->total_w, s->total_w_src);
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        char k[48];
        sfmt(k, sizeof k, "%sRAPL %s", z->depth ? "  " : "", z->name);
        if (!z->readable)
            lc_kv(c, k, CP(C_WARN), "%s", "no permission");
        else if (z->watts >= 0)
            lc_kv(c, k, A_BOLD, "%.2f W", z->watts);
        else
            lc_kv(c, k, A_GRAY, "%s", "...");
    }
    if (p->rapl_denied && geteuid() != 0) {
        lc_text(c, CP(C_WARN), "RAPL energy counters are root-only since Linux 5.10.");
        lc_text(c, CP(C_WARN), "Run 'sudo hardware-stats' to see CPU / DRAM / iGPU power.");
    }
    if (!p->nrapl)
        lc_text(c, A_GRAY, "No RAPL power domains (/sys/class/powercap)");
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        char tl[24] = "";
        if (b->time_left > 0)
            fmt_duration(b->time_left, tl, sizeof tl);
        lc_kv(c, b->name, A_BOLD, "%s, %.0f%%%s%s%s", b->status, b->capacity, b->watts >= 0 ? "" : "",
              tl[0] ? ", " : "", tl);
        if (b->watts >= 0)
            lc_kv(c, "  Rate", CP(C_WARN) | A_BOLD, "%.2f W", b->watts);
    }
    if (p->ac_online >= 0)
        lc_kv(c, "AC adapter", A_BOLD, "%s", p->ac_online ? "online" : "offline");
}

static void hw_system(sys_t *s, lc_t *c)
{
    cpu_t *cp = &s->cpu;
    mem_t *m = &s->mem;
    char a[24], b[24], d[24];
    lc_title(c, "System");
    lc_kv(c, "Host", A_BOLD, "%s", s->host);
    lc_kv(c, "OS", A_NORMAL, "%s", s->os);
    lc_kv(c, "Kernel", A_NORMAL, "%s (%s)", s->kernel, s->arch);
    fmt_duration(cp->uptime, a, sizeof a);
    lc_kv(c, "Uptime", A_NORMAL, "%s", a);
    lc_kv(c, "Load average", A_BOLD, "%.2f  %.2f  %.2f", cp->load[0], cp->load[1], cp->load[2]);
    lc_kv(c, "Tasks", A_NORMAL, "%d processes, %d threads, %d running, %d blocked", s->procs.n, s->procs.threads,
          s->procs.running, s->procs.diskwait);
    fmt_si(cp->ctxt_rate, a, sizeof a);
    fmt_si(cp->intr_rate, b, sizeof b);
    fmt_si(cp->fork_rate, d, sizeof d);
    lc_kv(c, "Activity", A_NORMAL, "%s ctx switches/s, %s interrupts/s, %s forks/s", a, b, d);

    lc_title(c, "CPU");
    lc_kv(c, "Model", A_BOLD, "%s", cp->model);
    lc_kv(c, "Topology", A_NORMAL, "%d cores, %d threads (%d online)", cp->cores, cp->threads, cp->ncpu);
    lc_kv(c, "Usage", A_NORMAL, "%.1f%%  (user %.1f%%, system %.1f%%, iowait %.1f%%, steal %.1f%%)", cp->total,
          cp->user, cp->sys, cp->iowait, cp->steal);
    if (cp->mhz_avg > 0)
        lc_kv(c, "Clock", A_NORMAL, "avg %.0f MHz, max %.0f MHz", cp->mhz_avg, cp->mhz_max);
    if (cp->temp_c >= 0)
        lc_kv(c, "Temperature", temp_attr(cp->temp_c) | A_BOLD, "%.1f%s", cp->temp_c, DEG);
    if (cp->mhz_avg > 0) {
        int per = MAX(1, (c->w - 2) / 13);
        char line[512] = "";
        for (int i = 0; i < cp->ncpu; i++) {
            app(line, sizeof line, "cpu%-3d%5.0f  ", cp->id[i], cp->mhz[i]);
            if ((i + 1) % per == 0 || i == cp->ncpu - 1) {
                lc_text(c, A_GRAY, "  %s", line);
                line[0] = '\0';
            }
        }
    }

    lc_title(c, "Memory");
    fmt_bytes((double)m->total, a, sizeof a);
    lc_kv(c, "Total", A_BOLD, "%s", a);
    fmt_bytes((double)m->used, a, sizeof a);
    lc_kv(c, "Used", A_BOLD, "%s (%.1f%%)", a, m->total ? 100.0 * (double)m->used / (double)m->total : 0);
    fmt_bytes((double)m->avail, a, sizeof a);
    lc_kv(c, "Available", A_NORMAL, "%s", a);
    fmt_bytes((double)m->buffcache, a, sizeof a);
    fmt_bytes((double)m->shmem, b, sizeof b);
    lc_kv(c, "Buff/cache", A_NORMAL, "%s  (shared %s)", a, b);
    fmt_bytes((double)m->dirty, a, sizeof a);
    fmt_bytes((double)m->writeback, b, sizeof b);
    lc_kv(c, "Dirty/writeback", A_NORMAL, "%s / %s", a, b);
    fmt_bytes((double)m->swap_used, a, sizeof a);
    fmt_bytes((double)m->swap_total, b, sizeof b);
    lc_kv(c, "Swap", A_NORMAL, "%s / %s", a, b);
    fmt_rate(m->swapin_bps, a, sizeof a);
    fmt_rate(m->swapout_bps, b, sizeof b);
    lc_kv(c, "Swap I/O", A_NORMAL, "in %s, out %s, %.0f major faults/s", a, b, m->majflt_rate);
}

static void hw_storage(sys_t *s, lc_t *c)
{
    disks_t *D = &s->disks;
    char a[24], b[24], d[24];
    lc_title(c, "Drives");
    if (!D->n)
        lc_text(c, A_GRAY, "No physical drives found");
    for (int i = 0; i < D->n; i++) {
        disk_t *k = &D->d[i];
        fmt_bytes((double)k->size, a, sizeof a);
        lc_text(c, A_BOLD, "%-10s %-5s %6s  %s", k->name, k->kind, a, k->model);
        fmt_rate(k->rd_bps, a, sizeof a);
        fmt_rate(k->wr_bps, b, sizeof b);
        fmt_si(k->rd_iops + k->wr_iops, d, sizeof d);
        lc_text(c, A_NORMAL, "  now   read %s  write %s  %s IOPS  busy %.0f%%", a, b, d, k->util);
        fmt_bytes((double)k->rd_total, a, sizeof a);
        fmt_bytes((double)k->wr_total, b, sizeof b);
        lc_text(c, A_GRAY, "  since boot   read %s  written %s", a, b);
    }
    lc_title(c, "Filesystems");
    if (!D->nfs)
        lc_text(c, A_GRAY, "No local filesystems found");
    for (int i = 0; i < D->nfs; i++) {
        fs_t *f = &D->fs[i];
        int y = lc_row(c);
        if (y < 0)
            continue;
        char v[64];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        sfmt(v, sizeof v, "%s/%s %-5s", a, b, f->type);
        double pct = f->total ? 100.0 * (double)f->used / (double)f->total : 0;
        int lw = MIN(18, c->w / 3);
        put(y, c->x, lw - 1, A_BOLD, "%s", f->mount);
        meter(y, c->x + lw, c->w - lw, "", 0, pct, v, 18, 0);
    }
}

static void hw_network(sys_t *s, lc_t *c)
{
    char a[24], b[24];
    lc_title(c, "Network");
    if (!s->net.n)
        lc_text(c, A_GRAY, "No interfaces (besides loopback)");
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        char sp[24] = "";
        if (f->speed > 0)
            sfmt(sp, sizeof sp, "%d Mb/s", f->speed);
        lc_text(c, f->up ? A_BOLD : A_GRAY, "%-12s %-8s %-8s %-11s %s", f->name,
                f->wireless ? "wifi" : f->is_virtual ? "virtual" : "ethernet", f->state, sp, f->addr);
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        char ta[24], tb[24];
        fmt_bytes((double)f->rx_total, ta, sizeof ta);
        fmt_bytes((double)f->tx_total, tb, sizeof tb);
        lc_text(c, A_GRAY, "  %s %s  %s %s   total rx %s  tx %s", U ? "↓" : "in", a, U ? "↑" : "out", b, ta, tb);
    }
}

static void hardware(sys_t *s, ui_state_t *u)
{
    int top = 1, h = LINES - 2, inner = h - 2;
    u->hw_scroll = CLAMP(u->hw_scroll, 0, u->hw_max);
    if (COLS >= 110) {
        int lw = COLS / 2;
        box_(top, 0, h, lw, "Sensors · Graphics · Power", NULL);
        box_(top, lw, h, COLS - lw, "System · Storage · Network", NULL);
        lc_t L = { top + 1, 2, lw - 4, inner, 0, u->hw_scroll };
        lc_t R = { top + 1, lw + 2, COLS - lw - 4, inner, 0, u->hw_scroll };
        hw_sensors(s, &L);
        hw_gpus(s, &L);
        hw_power(s, &L);
        hw_system(s, &R);
        hw_storage(s, &R);
        hw_network(s, &R);
        u->hw_max = MAX(0, MAX(L.line, R.line) - inner);
    } else {
        box_(top, 0, h, COLS, "Hardware", NULL);
        lc_t C = { top + 1, 2, COLS - 4, inner, 0, u->hw_scroll };
        hw_system(s, &C);
        hw_sensors(s, &C);
        hw_gpus(s, &C);
        hw_power(s, &C);
        hw_storage(s, &C);
        hw_network(s, &C);
        u->hw_max = MAX(0, C.line - inner);
    }
    if (u->hw_max > 0) {
        char m[64];
        sfmt(m, sizeof m, " %s scroll %d/%d ", U ? "↑↓" : "up/down", u->hw_scroll, u->hw_max);
        put(top + h - 1, COLS - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}

/* ============================================================ Dashboard */

static void dashboard(sys_t *s, ui_state_t *u)
{
    int top = 1, avail = LINES - 2, W = COLS;
    int lw = W >= 120 ? W * 55 / 100 : W / 2 + 2, rw = W - lw;
    /* the process list always keeps at least ~35% of the screen */
    int proc_min = MAX(avail >= 34 ? 10 : avail >= 24 ? 7 : 5, avail * 35 / 100);
    int top_max = avail - proc_min;

    int core_max = CLAMP(top_max / 3, 2, 12);
    int crows = MIN(cpu_rows(lw - 4, s->cpu.ncpu, core_max), core_max);
    int cpu_fixed = 2 + 2 + crows, gpu_fixed = gpu_need(s);
    int left_fixed = cpu_fixed + gpu_fixed;

    pline_t pl[16];
    int npl = build_power(s, pl, 16);
    int nidx[8], nn = net_pick(s, nidx, 4);
    int nd = MIN(s->disks.n, 4);
    int hr[4] = { 2 + 4, 2 + npl, 2 + MAX(nd, 1), 2 + MAX(nn, 1) }; /* mem, power, disk, net */
    int right_fixed = hr[0] + hr[1] + hr[2] + hr[3];

    int top_h = MIN(MAX(left_fixed, right_fixed) + 6, top_max);
    if (top_h < 8)
        top_h = MIN(8, avail);

    /* left column: GPU gets a few graph rows, CPU takes the rest */
    int gpu_h = gpu_fixed, cpu_h;
    int extra = top_h - left_fixed;
    if (extra >= 0) {
        int has_load = 0;
        for (int i = 0; i < s->gpus.n; i++)
            has_load |= s->gpus.g[i].util >= 0;
        if (has_load)
            gpu_h += MIN(extra / 3, 3);
        cpu_h = top_h - gpu_h;
    } else {
        cpu_h = MIN(cpu_fixed, top_h);
        gpu_h = top_h - cpu_h;
        if (gpu_h < 3) {
            cpu_h = top_h;
            gpu_h = 0;
        }
    }

    /* right column: spare rows become graph space; shrink when short */
    int sum = right_fixed, k = 0;
    static const int grow_order[4] = { 0, 2, 3, 1 };
    while (sum < top_h) {
        hr[grow_order[k++ % 4]]++;
        sum++;
    }
    while (sum > top_h) {
        int done = 0;
        for (int i = 3; i >= 0 && !done; i--)
            if (hr[i] > 3) {
                hr[i]--;
                sum--;
                done = 1;
            }
        if (!done)
            for (int i = 3; i >= 0; i--)
                if (hr[i] > 0) {
                    sum -= hr[i];
                    hr[i] = 0;
                    break;
                }
    }
    for (int i = 3; i >= 0 && sum < top_h; i--)
        if (hr[i] > 0) {
            hr[i] += top_h - sum;
            sum = top_h;
        }

    panel_cpu(s, top, 0, cpu_h, lw, crows);
    if (gpu_h >= 3)
        panel_gpu(s, top + cpu_h, 0, gpu_h, lw);
    int ry = top;
    if (hr[0])
        panel_mem(s, ry, lw, hr[0], rw);
    ry += hr[0];
    if (hr[1])
        panel_power(s, ry, lw, hr[1], rw, pl, npl);
    ry += hr[1];
    if (hr[2])
        panel_disk(s, ry, lw, hr[2], rw);
    ry += hr[2];
    if (hr[3])
        panel_net(s, ry, lw, hr[3], rw, nidx, nn);
    draw_procs(s, u, top + top_h, 0, avail - top_h, W, 0);
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

    static const char *const keys[][2] = { { "q", "Quit" },       { "1-3", "View" },   { "c/m/d/p/n/t", "Sort" },
                                           { "/", "Filter" },     { "k", "Kthreads" }, { "f", "Follow" },
                                           { "+/-", "Interval" }, { "Spc", "Pause" },  { "?", "Help" } };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0] && x < xm; i++) {
        int need = (int)strlen(keys[i][0]) + (int)strlen(keys[i][1]) + 3;
        if (x + need > xm)
            break;
        x += put(y, x, xm - x, CP(C_KEY), "%s", keys[i][0]);
        x += put(y, x, xm - x, A_NORMAL, " %s  ", keys[i][1]);
    }
}

static void help_overlay(void)
{
    static const char *const L[][2] = {
        { "1 2 3, Tab", "Dashboard / Processes / Hardware view" },
        { "Up Down PgUp PgDn", "move the selection or scroll" },
        { "Home End", "jump to top / bottom" },
        { "c m d p n t", "sort: CPU, memory, disk I/O, PID, name, time" },
        { "", "(press the same key again to reverse)" },
        { "r", "reverse sort order" },
        { "/", "filter by name, command, user or PID" },
        { "Esc", "clear the filter" },
        { "f", "selection sticks to the process, not the row" },
        { "k", "show / hide kernel threads" },
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
    put(y + h - 4, x + 3, w - 6, A_GRAY, "disk I/O of other users' processes.");
    put(y + h - 3, x + 3, w - 6, A_BOLD, "%s  %s", APP_NAME, APP_COPYRIGHT);
    put(y + h - 2, x + 3, w - 6, A_GRAY, "ABSOLUTELY NO WARRANTY. Free software: GPL v3 or later (see LICENSE).");
}

/* ================================================================= main */

void ui_draw(sys_t *s, ui_state_t *u)
{
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
    }
    footer(u);
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
    if (u->help) {
        u->help = 0;
        return 1;
    }
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
    case '\t': u->view = (u->view + 1) % VIEW_COUNT; break;
    case KEY_BTAB: u->view = (u->view + VIEW_COUNT - 1) % VIEW_COUNT; break;
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
        if (u->view == VIEW_HW)
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
