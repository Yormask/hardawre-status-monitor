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

/* sys.c - ties all collectors together and keeps the graph history */
#define _GNU_SOURCE
#include "collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static void read_os_name(char *out, size_t n)
{
    char buf[4096];
    str_copy(out, n, "Linux");
    if (read_file("/etc/os-release", buf, sizeof buf) <= 0 && read_file("/usr/lib/os-release", buf, sizeof buf) <= 0)
        return;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (strncmp(line, "PRETTY_NAME=", 12))
            continue;
        char *v = line + 12;
        size_t l = strlen(v);
        if (l >= 2 && (v[0] == '"' || v[0] == '\'') && v[l - 1] == v[0]) {
            v[l - 1] = '\0';
            v++;
        }
        str_copy(out, n, v);
        return;
    }
}

void sys_init(sys_t *s)
{
    struct utsname u;
    memset(s, 0, sizeof *s);
    if (uname(&u) == 0) {
        str_copy(s->host, sizeof s->host, u.nodename);
        str_copy(s->kernel, sizeof s->kernel, u.release);
        str_copy(s->arch, sizeof s->arch, u.machine);
    }
    read_os_name(s->os, sizeof s->os);
    s->total_w = -1;
    s->total_w_src = "";

    cpu_init(&s->cpu);
    procs_init(&s->procs);
    sensors_init(&s->sens);
    gpus_init(&s->gpus);
    power_init(&s->power);
    sys_sample(s); /* primes every counter; rates appear from the next sample */
}

static void estimate_total_power(sys_t *s)
{
    power_t *p = &s->power;
    double bat = 0;
    int discharging = 0;
    for (int i = 0; i < p->nbat; i++)
        if (!strcmp(p->bat[i].status, "Discharging") && p->bat[i].watts > 0) {
            bat += p->bat[i].watts;
            discharging = 1;
        }
    if (discharging) { /* battery drain = whole laptop */
        s->total_w = bat;
        s->total_w_src = "battery";
        return;
    }
    if (p->psys_w >= 0) { /* platform domain on recent Intel laptops */
        s->total_w = p->psys_w;
        s->total_w_src = "platform";
        return;
    }
    double t = 0;
    int any = 0;
    if (p->pkg_w >= 0) {
        t += p->pkg_w + (p->dram_w > 0 ? p->dram_w : 0);
        any = 1;
    }
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        /* an integrated GPU is already inside the CPU package figure */
        if (g->power_w >= 0 && (!g->integrated || p->pkg_w < 0)) {
            t += g->power_w;
            any = 1;
        }
    }
    s->total_w = any ? t : -1;
    s->total_w_src = any ? "CPU+GPU" : "";
}

void sys_sample(sys_t *s)
{
    double t = now_sec();
    double dt = s->samples ? t - s->t_last : 0;
    s->t_last = t;
    s->dt = dt;

    cpu_sample(&s->cpu, dt);
    mem_sample(&s->mem, dt);
    disks_sample(&s->disks, dt);
    net_sample(&s->net, dt);
    procs_sample(&s->procs, dt, s->mem.total);
    sensors_sample(&s->sens);
    gpus_sample(&s->gpus, dt);
    power_sample(&s->power, dt);
    s->cpu.temp_c = sensors_cpu_temp(&s->sens);

    if (t >= s->next_slow) { /* things that change slowly */
        fs_sample(&s->disks);
        net_addrs(&s->net);
        s->next_slow = t + 5.0;
    }

    /* Intel iGPU power = RAPL "uncore" (power plane 1) on client CPUs */
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        if (!strcmp(g->vendor, "Intel") && g->integrated && g->power_w < 0 && s->power.uncore_w >= 0)
            g->power_w = s->power.uncore_w;
    }
    estimate_total_power(s);

    if (s->samples) {
        hist_push(&s->h_cpu, s->cpu.total);
        hist_push(&s->h_mem, s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : 0);
        for (int i = 0; i < s->gpus.n; i++)
            hist_push(&s->h_gpu[i], s->gpus.g[i].util > 0 ? s->gpus.g[i].util : 0);
        hist_push(&s->h_dr, s->disks.rd_bps);
        hist_push(&s->h_dw, s->disks.wr_bps);
        hist_push(&s->h_rx, s->net.rx_bps);
        hist_push(&s->h_tx, s->net.tx_bps);
        hist_push(&s->h_pw, s->total_w > 0 ? s->total_w : 0);
    }
    s->samples++;
}

void sys_free(sys_t *s)
{
    procs_free(&s->procs);
    gpus_close(&s->gpus);
}
