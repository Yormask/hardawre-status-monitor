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
/* ui_panels.c - dashboard panels and the configurable two-column layout */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ================================================================== CPU */

static int nbits(int m)
{
    return !!(m & CPUBAR_USAGE) + !!(m & CPUBAR_CLOCK) + !!(m & CPUBAR_TEMP);
}

/* columns of per-core cells; k = bars per core (1-3) */
static int cpu_cols(int iw, int ncpu, int max_rows, int k)
{
    if (ncpu <= 1)
        return 1;
    if (max_rows < 1)
        max_rows = 1;
    int cols = (ncpu + max_rows - 1) / max_rows;
    int pref = MAX(iw / (k <= 1 ? 28 : 4 + 16 * k), 1);
    if (cols < pref)
        cols = pref;
    if (cols > ncpu)
        cols = ncpu;
    while (cols > 1 && iw / cols < 7 * MAX(k, 1))
        cols--;
    return cols;
}

static int cpu_rows(int iw, int ncpu, int max_rows, int k)
{
    int c = cpu_cols(iw, ncpu, max_rows, k);
    return (ncpu + c - 1) / c;
}

static int g_core_max = 8; /* rows allowed for per-core bars, set every frame */

/* One metric of one CPU: bar fill (0-100), value text and colours. */
static void cpu_metric(sys_t *s, int metric, int i, double *bpct, char *txt, size_t n, attr_t *ba, attr_t *ta)
{
    cpu_t *c = &s->cpu;
    int id = c->id[i], grad = gradient_on();
    double pct = c->pct[i], mhz = c->mhz[i];
    if (metric == CPUBAR_CLOCK) {
        double mx = c->mhz_hw[id] > 0 ? c->mhz_hw[id] : c->mhz_max;
        *bpct = (mhz > 0 && mx > 0) ? 100.0 * mhz / mx : 0;
        if (mhz > 0)
            sfmt(txt, n, "%.1fG", mhz / 1000.0);
        else
            str_copy(txt, n, "n/a");
        *ba = grad ? 0 : CP(C_IN);
        *ta = mhz > 0 ? A_BOLD : A_GRAY;
    } else if (metric == CPUBAR_TEMP) {
        int core = c->core_id[id] >= 0 ? c->core_id[id] : id;
        double t = sensors_core_temp(&s->sens, c->pkg_id[id], core);
        if (t < 0)
            t = c->temp_c; /* no per-core sensor (e.g. AMD): package temperature */
        *bpct = t > 0 ? t : 0; /* bar spans 0-100 °C */
        if (t >= 0)
            sfmt(txt, n, "%.0f%s", t, U ? "°" : "C");
        else
            str_copy(txt, n, "n/a");
        *ba = grad ? 0 : bar_temp_attr(t);
        *ta = t >= 0 ? temp_attr(t) | A_BOLD : A_GRAY;
    } else {
        *bpct = pct;
        sfmt(txt, n, "%.0f%%", pct);
        *ba = 0;
        *ta = pct_attr(pct);
    }
}

/* One per-core cell showing the bars picked in Setup (any combination of
 * usage, clock speed and temperature), side by side. */
static void cpu_cell(sys_t *s, int mask, int i, int y, int x, int cw, int idw)
{
    cpu_t *c = &s->cpu;
    int ms[3], k = 0;
    if (mask & CPUBAR_USAGE)
        ms[k++] = CPUBAR_USAGE;
    if (mask & CPUBAR_CLOCK)
        ms[k++] = CPUBAR_CLOCK;
    if (mask & CPUBAR_TEMP)
        ms[k++] = CPUBAR_TEMP;
    if (!k)
        ms[k++] = CPUBAR_USAGE;

    int avail = cw - (cw >= 20 ? 3 : 1); /* gap to the next column */
    put(y, x, idw, A_GRAY, "%*d", idw, c->id[i]);
    int bx = x + idw + 1, rem = avail - idw - 1;
    double bp;
    char txt[16];
    attr_t ba, ta;

    if (k == 1) { /* one bar + its value, and a grey hint (clock or usage) */
        cpu_metric(s, ms[0], i, &bp, txt, sizeof txt, &ba, &ta);
        char sec[16] = "";
        if (ms[0] == CPUBAR_USAGE) {
            if (c->mhz[i] > 0)
                sfmt(sec, sizeof sec, "%.1fG", c->mhz[i] / 1000.0);
        } else {
            sfmt(sec, sizeof sec, "%.0f%%", c->pct[i]);
        }
        int show_t = rem >= 4 + 5, show_s = sec[0] && rem >= 8 + 5 + 6;
        int bw = rem - (show_t ? 5 : 0) - (show_s ? 6 : 0);
        if (bw > 0)
            bar(y, bx, bw, bp, ba);
        int xx = bx + MAX(bw, 0);
        if (show_t) { /* right-aligned by display columns ("54°" is 3 columns, 4 bytes) */
            put(y, xx + 5 - utf8_cols(txt), 5, ta, "%s", txt);
            xx += 5;
        }
        if (show_s)
            put(y, xx + 6 - utf8_cols(sec), 6, A_GRAY, "%s", sec);
        return;
    }
    int sw = rem / k; /* one slot per metric: bar + 5-column value */
    for (int j = 0; j < k; j++) {
        cpu_metric(s, ms[j], i, &bp, txt, sizeof txt, &ba, &ta);
        int sx = bx + j * sw, gap = j < k - 1 ? 2 : 0;
        int bw = sw - 5 - gap;
        if (bw >= 2)
            bar(y, sx, bw, bp, ba);
        else
            bw = 0;
        put(y, sx + bw + 5 - utf8_cols(txt), 5, ta, "%s", txt);
    }
}

static int cpu_need(sys_t *s, ui_state_t *u, int w)
{
    return 4 + MIN(cpu_rows(w - 4, s->cpu.ncpu, g_core_max, nbits(u->lay.cpu_bars)), g_core_max);
}

static void panel_cpu(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    cpu_t *c = &s->cpu;
    int crows = g_core_max;
    char info[256] = "", v[32], a[16], b[16], d[16];
    app(info, sizeof info, "%s", c->model);
    if (c->threads)
        app(info, sizeof info, "%s%dC/%dT", SEP, c->cores, c->threads);
    if (u->lay.cpu_bars != CPUBAR_USAGE) {
        char bd[48];
        cpubar_desc(u->lay.cpu_bars, bd, sizeof bd);
        app(info, sizeof info, "%sbars: %s", SEP, bd);
    }
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
    int k = nbits(u->lay.cpu_bars);
    int rows_total = cpu_rows(iw, c->ncpu, crows, k);
    int rows_vis = MIN(rows_total, avail_rows);
    int gh = avail_rows - rows_vis;
    if (gh > 0)
        graph(iy + r, ix, gh, iw, &s->h_cpu, 100, 0, 0);
    int cy = iy + r + MAX(gh, 0);
    int cols = cpu_cols(iw, c->ncpu, crows, k);
    int cw = iw / cols, maxid = 0;
    for (int i = 0; i < c->ncpu; i++)
        maxid = MAX(maxid, c->id[i]);
    int idw = maxid >= 100 ? 3 : maxid >= 10 ? 2 : 1;
    for (int i = 0; i < c->ncpu; i++) {
        int col = i / rows_total, row = i % rows_total;
        if (row >= rows_vis)
            continue;
        cpu_cell(s, u->lay.cpu_bars, i, cy + row, ix + col * cw, cw, idw);
    }
}

/* ================================================================== GPU */

static int gpu_block(const gpu_t *g, int multi)
{
    return multi + 1 + (g->mem_total > 0) + (g->clk_max_mhz > 0) + 1;
}

static int gpu_need(sys_t *s, ui_state_t *u, int w)
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
    if (g->clk_mhz >= 0 && g->clk_max_mhz <= 0) /* with a known max it gets its own bar */
        kv(y, &xx, xm, A_BOLD, "clock", "%.0f MHz", g->clk_mhz);
    if (g->mem_clk_mhz > 0)
        kv(y, &xx, xm, A_BOLD, "mem", "%.0f MHz", g->mem_clk_mhz);
    if (g->fan_pct >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f%%", g->fan_pct);
    else if (g->fan_rpm >= 0)
        kv(y, &xx, xm, A_BOLD, "fan", "%.0f rpm", g->fan_rpm);
    for (int i = 0; i < g->neng; i++)
        kv(y, &xx, xm, pct_attr(g->eng[i].pct), g->eng[i].name, "%.0f%%", g->eng[i].pct);
}

static void panel_gpu(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
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
        if (r < ih && g->clk_max_mhz > 0) {
            char v[40];
            double cpct = g->clk_mhz > 0 ? 100.0 * g->clk_mhz / g->clk_max_mhz : 0;
            if (g->clk_mhz >= 0)
                sfmt(v, sizeof v, "%.0f/%.0f MHz", g->clk_mhz, g->clk_max_mhz);
            else
                sfmt(v, sizeof v, "?/%.0f MHz", g->clk_max_mhz);
            meter(iy + r++, ix, iw, "Clock", 6, cpct, v, 15, gradient_on() ? 0 : CP(C_IN));
        }
        if (r < ih)
            gpu_details(iy + r++, ix, iw, g);
    }
    if (graph_gpu >= 0 && r < ih)
        graph(iy + r, ix, ih - r, iw, &s->h_gpu[graph_gpu], 100, 0, 0);
}

/* =============================================================== Memory */

static int mem_need(sys_t *s, ui_state_t *u, int w)
{
    return 6;
}

static void panel_mem(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
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

static int power_need(sys_t *s, ui_state_t *u, int w)
{
    pline_t L[16];
    return 2 + build_power(s, L, 16);
}

static void panel_power(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    pline_t L[16];
    int n = build_power(s, L, 16);
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
        double mx = hist_max(&s->h_pw, graph_span(iw)) * 1.1;
        graph(iy + r, ix, ih - r, iw, &s->h_pw, mx, CP(C_WARN), 0);
        if (ih - r >= 2 && mx > 0 && g_lay->graph_style != GRAPH_OFF)
            put(iy + r, ix, iw, A_GRAY, "%.0f W", mx);
    }
}

/* ============================================================ Disk / Net */

static int disk_need(sys_t *s, ui_state_t *u, int w)
{
    return 2 + MAX(MIN(s->disks.n, 4), 1);
}

static void panel_disk(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
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
        xx += put(iy + r, xx, xm - xx, d->rd_bps > 0 ? CP(C_IN) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  W ");
        xx += put(iy + r, xx, xm - xx, d->wr_bps > 0 ? CP(C_OUT) | A_BOLD : A_GRAY, "%7s", b);
        if (xm - xx >= 6)
            xx += put(iy + r, xx, xm - xx, pct_attr(d->util), " %4.0f%%", d->util);
        if (xm - xx >= 14) {
            fmt_si(d->rd_iops + d->wr_iops, a, sizeof a);
            put(iy + r, xx, xm - xx, A_GRAY, "  %s IOPS", a);
        }
    }
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_dr, &s->h_dw, CP(C_IN), CP(C_OUT), "R", "W");
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

static int net_need(sys_t *s, ui_state_t *u, int w)
{
    int idx[8];
    return 2 + MAX(net_pick(s, idx, 4), 1);
}

static void panel_net(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    int idx[8], nn = net_pick(s, idx, 4);
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
        xx += put(iy + r, xx, xm - xx, f->rx_bps > 0 ? CP(C_IN) | A_BOLD : A_GRAY, "%7s", a);
        xx += put(iy + r, xx, xm - xx, A_GRAY, "  %s ", U ? "↑" : "out");
        xx += put(iy + r, xx, xm - xx, f->tx_bps > 0 ? CP(C_OUT) | A_BOLD : A_GRAY, "%7s", b);
        if (f->addr[0] && xm - xx >= (int)strlen(f->addr) + 2)
            put(iy + r, xx, xm - xx, A_GRAY, "  %s", f->addr);
        else if (f->speed > 0 && xm - xx >= 10)
            put(iy + r, xx, xm - xx, A_GRAY, "  %dM", f->speed);
    }
    if (r < ih)
        dual_graph(iy + r, ix, ih - r, iw, &s->h_rx, &s->h_tx, CP(C_IN), CP(C_OUT), U ? "↓" : "v", U ? "↑" : "^");
}

/* ========================================================= Temperatures */

typedef struct {
    char name[40];
    double v, crit;
} tent_t;


static int generic_label(const char *l)
{
    return (!strncmp(l, "temp", 4) || !strncmp(l, "fan", 3)) && l[strcspn(l, "0123456789")] != '\0' &&
           strlen(l) <= 6;
}

static int temps_list(sys_t *s, tent_t *T, int max)
{
    sensors_t *S = &s->sens;
    int n = 0;
    if (S->cpu_idx >= 0 && S->s[S->cpu_idx].ok && n < max) { /* CPU first */
        str_copy(T[n].name, sizeof T[n].name, "CPU");
        T[n].v = S->s[S->cpu_idx].value;
        T[n].crit = S->s[S->cpu_idx].crit;
        n++;
    }
    for (int i = 0; i < S->n && n < max; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_TEMP || !t->ok || i == S->cpu_idx)
            continue;
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        if (!strncmp(t->chip, "coretemp", 8))
            str_copy(T[n].name, sizeof T[n].name, t->label); /* "Core 3" */
        else if (generic_label(t->label) || !strcmp(t->label, "Composite"))
            str_copy(T[n].name, sizeof T[n].name, chip);
        else
            sfmt(T[n].name, sizeof T[n].name, "%s %s", chip, t->label);
        T[n].v = t->value;
        T[n].crit = t->crit;
        n++;
    }
    return n;
}

static int temps_cols(int iw)
{
    return MAX(1, iw / 30);
}

static int temps_need(sys_t *s, ui_state_t *u, int w)
{
    tent_t T[64];
    int n = temps_list(s, T, 64), c = temps_cols(w - 4);
    return n ? 2 + MIN((n + c - 1) / c, 8) : 3;
}

static void panel_temps(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    tent_t T[64];
    int n = temps_list(s, T, 64), hot = -1;
    for (int i = 0; i < n; i++)
        if (hot < 0 || T[i].v > T[hot].v)
            hot = i;
    char info[64] = "";
    if (hot >= 0)
        sfmt(info, sizeof info, "hottest: %s %.0f%s", T[hot].name, T[hot].v, DEG);
    box_(y, x, h, w, "Temperatures", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "No temperature sensors found in /sys/class/hwmon");
        return;
    }
    int cols = temps_cols(iw), rows = MIN((n + cols - 1) / cols, MIN(ih, 8)), cw = iw / cols;
    int shown = MIN(n, rows * cols);
    for (int i = 0; i < shown; i++) {
        int cy = iy + i % rows, cx = ix + (i / rows) * cw, aw = cw - (cols > 1 ? 3 : 0);
        int nw = MIN(14, aw / 2), vw = 5, bw = aw - nw - vw - 1;
        if (i == shown - 1 && shown < n) {
            put(cy, cx, aw, A_GRAY, "+%d more (Hardware view)", n - shown + 1);
            break;
        }
        put(cy, cx, nw, A_NORMAL, "%s", T[i].name);
        if (bw > 2)
            bar(cy, cx + nw, bw, 100.0 * T[i].v / (T[i].crit > 0 ? T[i].crit : 100.0),
                gradient_on() ? 0 : bar_temp_attr(T[i].v));
        char v[16];
        sfmt(v, sizeof v, "%.0f%s", T[i].v, DEG);
        put(cy, cx + aw - utf8_cols(v), vw, temp_attr(T[i].v) | A_BOLD, "%s", v);
    }
    if (ih > rows && s->cpu.temp_c >= 0) /* CPU temperature history */
        graph(iy + rows, ix, ih - rows, iw, &s->h_temp, 100, 0, 0);
}

/* ================================================================= Fans */

typedef struct {
    char name[40];
    double v, ref; /* RPM (or % when ref == 100 and pct set) */
    int pct;
} fent_t;

static int fans_list(sys_t *s, fent_t *F, int max)
{
    sensors_t *S = &s->sens;
    int n = 0;
    for (int i = 0; i < S->n && n < max; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_FAN || !t->ok)
            continue;
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        sfmt(F[n].name, sizeof F[n].name, "%s %s", chip, t->label);
        F[n].v = t->value;
        /* bar scale: the fan's rated max if the driver exposes it,
         * otherwise the fastest it has spun since start (min 2500 RPM) */
        F[n].ref = t->fmax > 0 ? t->fmax : MAX(t->peak, 2500.0);
        F[n].pct = 0;
        n++;
    }
    for (int i = 0; i < s->gpus.n && n < max; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (!g->nvml || (g->fan_rpm < 0 && g->fan_pct < 0))
            continue; /* sysfs GPUs' fans already come from their hwmon chip */
        sfmt(F[n].name, sizeof F[n].name, "GPU%d %s", i, g->vendor);
        if (g->fan_pct >= 0) {
            F[n].v = g->fan_pct;
            F[n].ref = 100;
            F[n].pct = 1;
        } else {
            F[n].v = g->fan_rpm;
            F[n].ref = 3500;
            F[n].pct = 0;
        }
        n++;
    }
    return n;
}

static int fans_need(sys_t *s, ui_state_t *u, int w)
{
    fent_t F[32];
    int n = fans_list(s, F, 32);
    return 2 + (n ? MIN(n, 8) : 2);
}

static void panel_fans(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    fent_t F[32];
    int n = fans_list(s, F, 32);
    char info[32] = "";
    if (n)
        sfmt(info, sizeof info, "%d fan%s", n, n == 1 ? "" : "s");
    box_(y, x, h, w, "Fans", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "No fan speed sensors found.");
        if (ih > 1)
            put(iy + 1, ix, iw, A_GRAY, "Desktop boards often need a driver: try 'sudo sensors-detect'");
        return;
    }
    int nw = MIN(18, iw / 3);
    for (int i = 0; i < n && i < ih; i++) {
        char v[24];
        if (F[i].pct)
            sfmt(v, sizeof v, "%.0f%%", F[i].v);
        else
            sfmt(v, sizeof v, "%.0f RPM", F[i].v);
        put(iy + i, ix, nw, A_NORMAL, "%s", F[i].name);
        meter(iy + i, ix + nw, iw - nw, "", 0, 100.0 * F[i].v / F[i].ref, v, 9,
              F[i].v <= 0 ? A_GRAY : gradient_on() ? 0 : CP(C_IN));
    }
}

/* ========================================================== Filesystems */

static int fs_need(sys_t *s, ui_state_t *u, int w)
{
    return 2 + MAX(MIN(s->disks.nfs, 6), 1);
}

static void panel_fs(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    disks_t *D = &s->disks;
    char info[32];
    sfmt(info, sizeof info, "%d mounted", D->nfs);
    box_(y, x, h, w, "Filesystems", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (!D->nfs)
        put(iy, ix, iw, A_GRAY, "No local filesystems found");
    int nw = MIN(16, iw / 3);
    for (int i = 0; i < D->nfs && i < ih; i++) {
        fs_t *f = &D->fs[i];
        char a[16], b[16], v[40];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        sfmt(v, sizeof v, "%s/%s", a, b);
        put(iy + i, ix, nw - 1, A_BOLD, "%s", f->mount);
        meter(iy + i, ix + nw, iw - nw, "", 0, f->total ? 100.0 * (double)f->used / (double)f->total : 0, v, 11, 0);
    }
}

/* ============================================================ Dashboard */

typedef struct {
    int (*need)(sys_t *, ui_state_t *, int w);                  /* rows incl. borders */
    void (*draw)(sys_t *, ui_state_t *, int y, int x, int h, int w);
    int weight; /* share of spare rows; spare rows become history graphs */
} pdef_t;

static const pdef_t PANELS[P_COUNT] = {
    [P_CPU] = { cpu_need, panel_cpu, 3 },       [P_GPU] = { gpu_need, panel_gpu, 1 },
    [P_MEM] = { mem_need, panel_mem, 1 },       [P_POWER] = { power_need, panel_power, 1 },
    [P_DISK] = { disk_need, panel_disk, 1 },    [P_NET] = { net_need, panel_net, 1 },
    [P_TEMPS] = { temps_need, panel_temps, 1 }, [P_FANS] = { fans_need, panel_fans, 0 },
    [P_FS] = { fs_need, panel_fs, 0 },
};

static int pweight(sys_t *s, int p)
{
    if (g_lay->graph_style == GRAPH_OFF)
        return 0;
    if (p == P_GPU) {
        for (int i = 0; i < s->gpus.n; i++)
            if (s->gpus.g[i].util >= 0)
                return 1;
        return 0;
    }
    if (p == P_TEMPS && s->cpu.temp_c < 0)
        return 0;
    return PANELS[p].weight;
}

/* left column width for a screen (or preview) W columns wide */
static int left_width(const layout_t *l, int W)
{
    int lw = l->left_pct ? W * l->left_pct / 100 : W >= 120 ? W * 55 / 100 : W / 2 + 2;
    return CLAMP(lw, MIN(30, W / 2), MAX(W - 30, W / 2));
}

/* Stack one column's panels into top_h rows: everyone gets what it needs,
 * spare rows go to panels with graphs (by weight); when short, panels
 * shrink from the bottom up and finally drop out. */
static void layout_column(sys_t *s, ui_state_t *u, const int *ids, int n, int y, int x, int w, int top_h)
{
    int h[P_COUNT], wt[P_COUNT], sum = 0, tw = 0;
    for (int i = 0; i < n; i++) {
        h[i] = PANELS[ids[i]].need(s, u, w);
        wt[i] = pweight(s, ids[i]);
        sum += h[i];
        tw += wt[i];
    }
    if (sum < top_h) {
        int extra = top_h - sum;
        if (!tw)
            h[n - 1] += extra;
        while (tw && extra > 0)
            for (int i = 0; i < n && extra > 0; i++)
                for (int k = 0; k < wt[i] && extra > 0; k++, extra--)
                    h[i]++;
        sum = top_h;
    }
    while (sum > top_h) {
        int done = 0;
        for (int i = n - 1; i >= 0 && !done; i--)
            if (h[i] > 3) {
                h[i]--;
                sum--;
                done = 1;
            }
        if (!done)
            for (int i = n - 1; i >= 0; i--)
                if (h[i] > 0) {
                    sum -= h[i];
                    h[i] = 0;
                    break;
                }
    }
    if (sum < top_h)
        for (int i = n - 1; i >= 0; i--)
            if (h[i] > 0) {
                h[i] += top_h - sum;
                break;
            }
    for (int i = 0, yy = y; i < n; yy += h[i], i++)
        if (h[i] >= 3)
            PANELS[ids[i]].draw(s, u, yy, x, h[i], w);
}

static int visible(const layout_t *l, int c, int *ids)
{
    int n = 0;
    for (int i = 0; i < l->ncol[c]; i++)
        if (l->on[l->col[c][i]])
            ids[n++] = l->col[c][i];
    return n;
}

void dashboard(sys_t *s, ui_state_t *u)
{
    const layout_t *l = &u->lay;
    int top = 1, avail = LINES - 2, W = COLS;
    int ids[2][P_COUNT], n[2] = { visible(l, 0, ids[0]), visible(l, 1, ids[1]) };
    int both = n[0] && n[1];
    int lw = both ? left_width(l, W) : W;
    int xs[2] = { 0, both ? lw : 0 }, ws[2] = { lw, both ? W - lw : W };

    if (!n[0] && !n[1]) {
        if (l->show_procs) {
            draw_procs(s, u, top, 0, avail, W, 0);
        } else {
            box_(top, 0, avail, W, "Dashboard", NULL);
            put(top + 2, 3, W - 6, A_BOLD, "All dashboard panels are hidden.");
            put(top + 3, 3, W - 6, A_GRAY, "Press s to open Setup and turn some on.");
        }
        return;
    }

    /* the process list keeps at least ~35% of the screen */
    int proc_min = l->show_procs ? MAX(avail >= 34 ? 10 : avail >= 24 ? 7 : 5, avail * 35 / 100) : 0;
    int top_max = avail - proc_min;
    g_core_max = CLAMP(top_max / 3, 2, 12);

    int top_h = avail;
    if (l->show_procs) {
        int want = 0;
        for (int c = 0; c < 2; c++) {
            int sum = 0, wt = 0;
            for (int i = 0; i < n[c]; i++) {
                sum += PANELS[ids[c][i]].need(s, u, ws[c]);
                wt += pweight(s, ids[c][i]);
            }
            want = MAX(want, sum + (wt ? 6 : 0));
        }
        top_h = MIN(want, top_max);
        if (top_h < 8)
            top_h = MIN(8, avail);
    }
    for (int c = 0; c < 2; c++)
        if (n[c])
            layout_column(s, u, ids[c], n[c], top, xs[c], ws[c], top_h);
    if (l->show_procs)
        draw_procs(s, u, top + top_h, 0, avail - top_h, W, 0);
}

/* Miniature of the dashboard for the Setup screen; panel hl is highlighted. */
void dashboard_preview(const layout_t *l, int y, int x, int h, int w, int hl)
{
    int ids[2][P_COUNT], n[2] = { visible(l, 0, ids[0]), visible(l, 1, ids[1]) };
    int both = n[0] && n[1], rows = MAX(MAX(n[0], n[1]), 1);
    int ph = l->show_procs ? 3 : 0;
    int per = CLAMP((h - ph) / rows, 1, 4), sh = per > 1 ? per - 1 : 1; /* block height */
    int lw = both ? left_width(l, w) : w;
    int xs[2] = { x, both ? x + lw : x }, ws[2] = { lw, both ? w - lw : w };
    if (!n[0] && !n[1]) {
        put(y, x, w, A_GRAY, "(no panels shown)");
    }
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n[c] && i * per < h - ph; i++) {
            int p = ids[c][i];
            attr_t a = p == hl ? CP(C_THEAD) | A_BOLD : CP(C_BORDER) | A_REVERSE;
            for (int r = 0; r < sh; r++)
                fill(y + i * per + r, xs[c], ws[c] - 1, a);
            put(y + i * per, xs[c] + 1, ws[c] - 2, a, "%s", PANEL_NAME[p]);
        }
    if (ph) {
        int py = y + MIN(rows * per, h - ph);
        fill(py, x, w, CP(C_SEL));
        fill(py + 1, x, w, CP(C_SEL));
        put(py, x + 1, w - 2, CP(C_SEL), "Processes");
    }
}
