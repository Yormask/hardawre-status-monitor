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

/* report.c - one-shot plain-text snapshot (hsm --once), handy for scripts,
 * bug reports and ssh sessions without a full terminal */
#define _GNU_SOURCE
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int by_cpu(const void *a, const void *b)
{
    const proc_t *p = *(proc_t *const *)a, *q = *(proc_t *const *)b;
    int r = (p->cpu < q->cpu) - (p->cpu > q->cpu);
    return r ? r : (p->rss < q->rss) - (p->rss > q->rss);
}

void report_print(sys_t *s, int nproc)
{
    char a[32], b[32], c[32];
    cpu_t *cp = &s->cpu;
    mem_t *m = &s->mem;

    fmt_duration(cp->uptime, a, sizeof a);
    printf("%s %s  %s | %s | kernel %s (%s) | up %s\n", APP_NAME, APP_VERSION, s->host, s->os, s->kernel, s->arch, a);

    printf("\nCPU         %s, %d cores / %d threads\n", cp->model, cp->cores, cp->threads);
    printf("  usage     %.1f%%  (user %.1f%%, system %.1f%%, iowait %.1f%%, steal %.1f%%)\n", cp->total, cp->user,
           cp->sys, cp->iowait, cp->steal);
    if (cp->mhz_avg > 0)
        printf("  clock     avg %.0f MHz, max %.0f MHz\n", cp->mhz_avg, cp->mhz_max);
    if (cp->temp_c >= 0)
        printf("  temp      %.1f C\n", cp->temp_c);
    printf("  load      %.2f %.2f %.2f\n", cp->load[0], cp->load[1], cp->load[2]);
    printf("  per CPU  ");
    for (int i = 0; i < cp->ncpu; i++) {
        printf(" %d:%.0f%%", cp->id[i], cp->pct[i]);
        if (i % 12 == 11 && i + 1 < cp->ncpu)
            printf("\n           ");
    }
    printf("\n");

    fmt_bytes((double)m->used, a, sizeof a);
    fmt_bytes((double)m->total, b, sizeof b);
    fmt_bytes((double)m->avail, c, sizeof c);
    printf("\nMemory      %s / %s used (%.1f%%), %s available\n", a, b,
           m->total ? 100.0 * (double)m->used / (double)m->total : 0, c);
    fmt_bytes((double)m->swap_used, a, sizeof a);
    fmt_bytes((double)m->swap_total, b, sizeof b);
    printf("  swap      %s / %s\n", a, b);

    printf("\nGPU\n");
    if (!s->gpus.n)
        printf("  none found\n");
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        printf("  %d %s [%s%s%s]\n", i, g->name, g->driver, g->pci[0] ? " " : "", g->pci);
        if (g->util >= 0) {
            printf("    load    %s%.1f%%%s", g->util_est ? "~" : "", g->util, g->util_est ? " (estimated from RC6 sleep time)" : "");
            for (int e = 0; e < g->neng; e++)
                printf("  %s %.0f%%", g->eng[e].name, g->eng[e].pct);
            printf("\n");
        } else if (g->note[0]) {
            printf("    load    %s\n", g->note);
        }
        if (g->mem_total > 0) {
            fmt_bytes(g->mem_used, a, sizeof a);
            fmt_bytes(g->mem_total, b, sizeof b);
            printf("    vram    %s / %s\n", a, b);
        }
        if (g->clk_mhz >= 0)
            printf("    clock   %.0f MHz\n", g->clk_mhz);
        if (g->temp_c >= 0)
            printf("    temp    %.1f C\n", g->temp_c);
        if (g->power_w >= 0)
            printf("    power   %.1f W\n", g->power_w);
    }

    power_t *p = &s->power;
    printf("\nPower\n");
    if (s->total_w >= 0)
        printf("  estimate  %.1f W (%s)\n", s->total_w, s->total_w_src);
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        if (z->readable && z->watts >= 0)
            printf("  %s%-10s%.2f W\n", z->depth ? "  " : "", z->name, z->watts);
        else if (!z->readable)
            printf("  %s%-10sno permission (run as root)\n", z->depth ? "  " : "", z->name);
    }
    for (int i = 0; i < p->nbat; i++) {
        battery_t *bt = &p->bat[i];
        printf("  %-10s%s, %.0f%%", bt->name, bt->status, bt->capacity);
        if (bt->watts >= 0)
            printf(", %.2f W", bt->watts);
        printf("\n");
    }
    if (!p->nrapl && !p->nbat)
        printf("  no power sensors found\n");

    printf("\nDisks\n");
    for (int i = 0; i < s->disks.n; i++) {
        disk_t *d = &s->disks.d[i];
        fmt_rate(d->rd_bps, a, sizeof a);
        fmt_rate(d->wr_bps, b, sizeof b);
        fmt_bytes((double)d->size, c, sizeof c);
        printf("  %-10s %-5s %6s  read %8s  write %8s  busy %3.0f%%  %s\n", d->name, d->kind, c, a, b, d->util,
               d->model);
    }
    for (int i = 0; i < s->disks.nfs; i++) {
        fs_t *f = &s->disks.fs[i];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        printf("  fs %-20s %-6s %s / %s (%.0f%%)\n", f->mount, f->type, a, b,
               f->total ? 100.0 * (double)f->used / (double)f->total : 0);
    }

    printf("\nNetwork\n");
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        printf("  %-14s %-8s rx %8s  tx %8s  %s\n", f->name, f->state, a, b, f->addr);
    }

    if (s->sens.n) {
        printf("\nSensors\n");
        for (int i = 0; i < s->sens.n; i++) {
            sensor_t *t = &s->sens.s[i];
            if (!t->ok)
                continue;
            static const char *const unit[] = { "C", "RPM", "W" };
            printf("  %-14s %-22s %8.1f %s\n", t->chip, t->label, t->value, unit[t->type]);
        }
    }

    if (nproc > 0 && s->procs.n) {
        proc_t **v = malloc((size_t)s->procs.n * sizeof *v);
        if (v) {
            for (int i = 0; i < s->procs.n; i++)
                v[i] = &s->procs.p[i];
            qsort(v, (size_t)s->procs.n, sizeof *v, by_cpu);
            printf("\nTop processes by CPU  (%d processes, %d threads)\n", s->procs.n, s->procs.threads);
            printf("  %7s %-10s %6s %6s %8s %9s %9s  %s\n", "PID", "USER", "CPU%", "MEM%", "RSS", "READ/s", "WRITE/s",
                   "COMMAND");
            for (int i = 0; i < nproc && i < s->procs.n; i++) {
                proc_t *q = v[i];
                fmt_bytes((double)q->rss, a, sizeof a);
                if (q->io_ok) {
                    fmt_rate(q->rd_bps, b, sizeof b);
                    fmt_rate(q->wr_bps, c, sizeof c);
                } else {
                    str_copy(b, sizeof b, "-");
                    str_copy(c, sizeof c, "-");
                }
                const char *cmd = q->kthread ? "" : proc_cmdline(q);
                printf("  %7d %-10.10s %6.1f %6.1f %8s %9s %9s  %.80s%s%s%s\n", (int)q->pid, uid_name(q->uid), q->cpu,
                       q->mem, a, b, c, cmd, *cmd ? "" : "[", *cmd ? "" : q->comm, *cmd ? "" : "]");
            }
            free(v);
        }
    }
}
