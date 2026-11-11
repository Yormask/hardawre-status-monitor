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
/* ui_hw.c - Hardware view: every sensor, GPU detail, drive, filesystem and interface */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

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
        lc_text(c, CP(C_WARN), "Run 'sudo hsm' to see CPU / DRAM / iGPU power.");
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

void hardware(sys_t *s, ui_state_t *u)
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
