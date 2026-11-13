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
    s->total_why = "";

    cpu_init(&s->cpu);
    procs_init(&s->procs);
    sensors_init(&s->sens);
    gpus_init(&s->gpus);
    power_init(&s->power);
    health_init(&s->health);
    cpu_power_sample(&s->cpu);
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
    s->total_why = "";
    s->pw_partial = 0;
    if (discharging) { /* battery drain = whole laptop */
        s->total_w = bat;
        s->total_w_src = "battery";
        return;
    }
    /* the platform domain on recent Intel laptops - unless the firmware left
     * it unconnected, which shows as less than the CPU package inside it */
    if (p->psys_w >= 0 && !p->psys_bad && (p->pkg_w < 0 || p->psys_w >= 0.9 * p->pkg_w)) {
        s->total_w = p->psys_w;
        s->total_w_src = "platform";
        return;
    }
    double gpu = 0;
    int ngpu = 0;
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        /* an integrated GPU is already inside the CPU package figure */
        if (g->power_w >= 0 && !g->integrated) {
            gpu += g->power_w;
            ngpu++;
        }
    }
    if (p->pkg_w < 0) {
        /* without the CPU there is no total: the GPUs alone would pass for
         * one (47 W "total" while the CPU drew another 40) */
        s->total_w = -1;
        s->total_w_src = "";
        s->total_why = p->rapl_denied ? "needs root" : "";
        s->pw_partial = ngpu > 0;
        s->part_w = gpu;
        return;
    }
    s->total_w = p->pkg_w + (p->dram_w > 0 ? p->dram_w : 0) + gpu;
    s->total_w_src = ngpu ? "CPU+GPU" : "CPU";
}

/* ------------------------------------------------- readings that hang
 *
 * hsm reads everything in turn, so one slow reading holds up the whole
 * sample and its screen. Each part is timed; the slowest one, if it took a
 * second or more, becomes an event naming it - one sample later, once the
 * freeze watchdog has had its say: if the whole system stood still, that
 * freeze is the news, not the reading that happened to be waiting. */
#define SLOW_READ 1.0

static void timed(char *worst, size_t n, double *wd, double t0, const char *what)
{
    double d = now_sec() - t0;
    if (d > *wd) {
        *wd = d;
        str_copy(worst, n, what);
    }
}

static void slow_report(sys_t *s, const char *worst, double wd)
{
    health_t *h = &s->health;
    if (s->slow_what[0]) { /* last sample's: no freeze or suspend reported since? */
        if (h->freezes == s->slow_frz && h->suspends == s->slow_susp) {
            double now = now_sec();
            int k, old = 0;
            for (k = 0; k < 8 && strcmp(s->slow_seen[k].what, s->slow_what); k++)
                if (s->slow_seen[k].t < s->slow_seen[old].t)
                    old = k;
            if (k == 8) { /* new: the least recent slot */
                k = old;
                str_copy(s->slow_seen[k].what, sizeof s->slow_seen[k].what, s->slow_what);
                s->slow_seen[k].t = -1e9;
            }
            if (now - s->slow_seen[k].t >= 600) {
                s->slow_seen[k].t = now;
                health_event(h, EV_WARN, "slow", "Reading %s took %.1f s; hsm waited for it", s->slow_what,
                             s->slow_dur);
            }
        }
        s->slow_what[0] = '\0';
    }
    if (wd >= SLOW_READ) {
        str_copy(s->slow_what, sizeof s->slow_what, worst);
        s->slow_dur = wd;
        s->slow_frz = h->freezes;
        s->slow_susp = h->suspends;
    }
}

void sys_sample(sys_t *s)
{
    double t = now_sec();
    double dt = s->samples ? t - s->t_last : 0;
    s->t_last = t;
    s->dt = dt;

    char worst[96] = "", what[96];
    double wd = 0, t0 = now_sec();
    cpu_sample(&s->cpu, dt);
    timed(worst, sizeof worst, &wd, t0, "the CPU figures");
    t0 = now_sec();
    mem_sample(&s->mem, dt);
    timed(worst, sizeof worst, &wd, t0, "the memory figures");
    t0 = now_sec();
    disks_sample(&s->disks, dt);
    timed(worst, sizeof worst, &wd, t0, "the disk counters");
    t0 = now_sec();
    net_sample(&s->net, dt);
    timed(worst, sizeof worst, &wd, t0, "the network counters");
    t0 = now_sec();
    procs_sample(&s->procs, dt, s->mem.total);
    if (s->procs.slow_s >= SLOW_READ) {
        const char *comm = "";
        for (int i = 0; i < s->procs.n; i++)
            if (s->procs.p[i].pid == s->procs.slow_pid)
                comm = s->procs.p[i].comm;
        sfmt(what, sizeof what, "process %d%s%s%s", (int)s->procs.slow_pid, comm[0] ? " (" : "", comm, comm[0] ? ")" : "");
    } else {
        str_copy(what, sizeof what, "the process list");
    }
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    sensors_sample(&s->sens);
    if (s->sens.slow[0]) {
        char chip[32];
        sensors_chip_name(s->sens.slow, chip, sizeof chip);
        sfmt(what, sizeof what, "the %s sensors", chip);
    } else {
        str_copy(what, sizeof what, "the sensors");
    }
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    gpus_sample(&s->gpus, dt);
    sfmt(what, sizeof what, "the %s", s->gpus.slow[0] ? s->gpus.slow : "GPUs");
    timed(worst, sizeof worst, &wd, t0, what);
    t0 = now_sec();
    gpus_proc_sample(&s->gpus, &s->procs);
    timed(worst, sizeof worst, &wd, t0, "the GPU use of processes (NVML)");
    t0 = now_sec();
    power_sample(&s->power, dt);
    timed(worst, sizeof worst, &wd, t0, s->power.slow[0] ? s->power.slow : "the power figures");
    s->cpu.temp_c = sensors_cpu_temp(&s->sens);

    if (t >= s->next_slow) { /* things that change slowly */
        t0 = now_sec();
        fs_sample(&s->disks);
        timed(worst, sizeof worst, &wd, t0, "the file systems");
        t0 = now_sec();
        net_addrs(&s->net);
        cpu_power_sample(&s->cpu);
        timed(worst, sizeof worst, &wd, t0, "the network addresses and CPU settings");
        s->next_slow = t + 5.0;
    }
    t0 = now_sec();
    health_sample(s);
    timed(worst, sizeof worst, &wd, t0, s->health.slow[0] ? s->health.slow : "the health checks");
    t0 = now_sec();
    svcs_sample(&s->svc);
    timed(worst, sizeof worst, &wd, t0, "the services (cgroups)");
    slow_report(s, worst, wd);

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
        hist_push(&s->h_pw, s->total_w > 0 ? s->total_w : s->pw_partial ? s->part_w : 0);
        hist_push(&s->h_temp, s->cpu.temp_c > 0 ? s->cpu.temp_c : 0);
    }
    s->samples++;
}

void sys_free(sys_t *s)
{
    procs_free(&s->procs);
    gpus_close(&s->gpus);
    health_close(&s->health);
    svcs_free(&s->svc);
}
