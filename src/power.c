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

/* power.c - CPU/DRAM/iGPU power from RAPL (powercap) and battery drain */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void power_init(power_t *p)
{
    char base[PATHBUF], dir[PATHBUF], f[PATHBUF];
    p->ac_online = -1;
    p->pkg_w = p->core_w = p->uncore_w = p->dram_w = p->psys_w = -1;

    /* RAPL zones: intel-rapl:0 = package 0, intel-rapl:0:0 = its cores, ...
     * Recent kernels expose the same interface for AMD Zen CPUs too.
     * (intel-rapl-mmio duplicates the package zone, so it is skipped.) */
    sfmt(base, sizeof base, "%s/class/powercap", g_sysfs);
    DIR *d = opendir(base);
    if (d) {
        static char names[64][64];
        int nn = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL && nn < 64)
            if (!strncmp(e->d_name, "intel-rapl:", 11))
                str_copy(names[nn++], sizeof names[0], e->d_name);
        closedir(d);
        qsort(names, (size_t)nn, sizeof names[0], cmp_name);

        for (int i = 0; i < nn && p->nrapl < MAX_RAPL; i++) {
            rapl_t *z = &p->z[p->nrapl];
            memset(z, 0, sizeof *z);
            sfmt(dir, sizeof dir, "%s/%s", base, names[i]);
            sfmt(f, sizeof f, "%s/name", dir);
            if (read_line(f, z->name, sizeof z->name))
                continue;
            sfmt(z->path, sizeof z->path, "%s/energy_uj", dir);
            if (!path_exists(z->path))
                continue;
            sfmt(f, sizeof f, "%s/max_energy_range_uj", dir);
            if (read_ull(f, &z->max_range))
                z->max_range = 0;
            int colons = 0;
            for (const char *q = names[i]; *q; q++)
                colons += *q == ':';
            z->depth = colons - 1;
            /* since Linux 5.10 energy_uj is root-only (PLATYPUS side channel) */
            int fd = open(z->path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                z->readable = 1;
                close(fd);
            } else {
                p->rapl_denied = 1;
            }
            p->nrapl++;
        }
    }

    /* batteries and AC adapter */
    sfmt(base, sizeof base, "%s/class/power_supply", g_sysfs);
    d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            char type[32], scope[32];
            sfmt(dir, sizeof dir, "%s/%s", base, e->d_name);
            sfmt(f, sizeof f, "%s/type", dir);
            if (read_line(f, type, sizeof type))
                continue;
            sfmt(f, sizeof f, "%s/scope", dir);
            if (read_line(f, scope, sizeof scope))
                scope[0] = '\0';
            if (!strcmp(type, "Battery") && strcmp(scope, "Device") && p->nbat < 4) {
                battery_t *b = &p->bat[p->nbat++];
                str_copy(b->name, sizeof b->name, e->d_name);
                str_copy(b->path, sizeof b->path, dir);
                b->watts = b->capacity = b->time_left = -1;
            } else if (!strcmp(type, "Mains") && !p->ac_path[0]) {
                sfmt(p->ac_path, sizeof p->ac_path, "%s/online", dir);
            }
        }
        closedir(d);
    }
}

static int battr(const battery_t *b, const char *name, long long *v)
{
    char f[PATHBUF];
    sfmt(f, sizeof f, "%s/%s", b->path, name);
    return read_ll(f, v);
}

static void battery_sample(battery_t *b)
{
    char f[PATHBUF];
    long long v, cur, volt, now, full;
    sfmt(f, sizeof f, "%s/status", b->path);
    if (read_line(f, b->status, sizeof b->status))
        str_copy(b->status, sizeof b->status, "Unknown");
    b->capacity = battr(b, "capacity", &v) == 0 ? (double)v : -1;

    b->watts = -1;
    if (battr(b, "power_now", &v) == 0)
        b->watts = fabs((double)v) / 1e6;
    else if (battr(b, "current_now", &cur) == 0 && battr(b, "voltage_now", &volt) == 0)
        b->watts = fabs((double)cur) * (double)volt / 1e12;

    double e_now = -1, e_full = -1; /* Wh */
    if (battr(b, "energy_now", &now) == 0 && battr(b, "energy_full", &full) == 0) {
        e_now = (double)now / 1e6;
        e_full = (double)full / 1e6;
    } else if (battr(b, "charge_now", &now) == 0 && battr(b, "charge_full", &full) == 0 &&
               (battr(b, "voltage_min_design", &volt) == 0 || battr(b, "voltage_now", &volt) == 0)) {
        e_now = (double)now * (double)volt / 1e12;
        e_full = (double)full * (double)volt / 1e12;
    }
    b->time_left = -1;
    if (b->watts > 0.1 && e_now >= 0) {
        if (!strcmp(b->status, "Discharging"))
            b->time_left = e_now / b->watts * 3600.0;
        else if (!strcmp(b->status, "Charging") && e_full > e_now)
            b->time_left = (e_full - e_now) / b->watts * 3600.0;
    }
}

void power_sample(power_t *p, double dt)
{
    double pkg = 0, core = 0, unc = 0, dram = 0, psys = 0;
    int hp = 0, hc = 0, hu = 0, hd = 0, hs = 0;
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        unsigned long long e;
        z->watts = -1;
        if (!z->readable || read_ull(z->path, &e))
            continue;
        if (z->primed && dt > 0) {
            unsigned long long de;
            if (e >= z->prev)
                de = e - z->prev;
            else /* counter wrapped */
                de = z->max_range > z->prev ? z->max_range - z->prev + e : 0;
            z->watts = (double)de / 1e6 / dt;
        }
        z->prev = e;
        z->primed = 1;
        if (z->watts < 0)
            continue;
        if (!strncmp(z->name, "package", 7) && z->depth == 0) {
            pkg += z->watts;
            hp = 1;
        } else if (!strcmp(z->name, "core")) {
            core += z->watts;
            hc = 1;
        } else if (!strcmp(z->name, "uncore")) {
            unc += z->watts;
            hu = 1;
        } else if (!strcmp(z->name, "dram")) {
            dram += z->watts;
            hd = 1;
        } else if (!strcmp(z->name, "psys")) {
            psys += z->watts;
            hs = 1;
        }
    }
    p->pkg_w = hp ? pkg : -1;
    p->core_w = hc ? core : -1;
    p->uncore_w = hu ? unc : -1;
    p->dram_w = hd ? dram : -1;
    p->psys_w = hs ? psys : -1;

    for (int i = 0; i < p->nbat; i++)
        battery_sample(&p->bat[i]);
    if (p->ac_path[0]) {
        long long v;
        p->ac_online = read_ll(p->ac_path, &v) == 0 ? (int)v : -1;
    }
}
