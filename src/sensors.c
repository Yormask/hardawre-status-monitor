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

/* sensors.c - temperatures, fans and power readings from /sys/class/hwmon */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int type, num, pref;
    char file[64];
} cand_t;

static int match_attr(const char *nm, const char *prefix, const char *suffix, int *num)
{
    size_t pl = strlen(prefix), sl = strlen(suffix), nl = strlen(nm);
    if (nl <= pl + sl || strncmp(nm, prefix, pl) || strcmp(nm + nl - sl, suffix))
        return 0;
    char *end;
    long v = strtol(nm + pl, &end, 10);
    if (end == nm + pl || end != nm + nl - sl)
        return 0;
    *num = (int)v;
    return 1;
}

static int cand_cmp(const void *a, const void *b)
{
    const cand_t *x = a, *y = b;
    if (x->type != y->type)
        return x->type - y->type;
    if (x->num != y->num)
        return x->num - y->num;
    return x->pref - y->pref;
}

static int scan_chip(sensors_t *S, const char *dir, const char *chip, int chip_idx)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    static cand_t c[256];
    int nc = 0, num;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nc < 256) {
        const char *nm = e->d_name;
        int type = -1, pref = 0;
        if (match_attr(nm, "temp", "_input", &num))
            type = SENS_TEMP;
        else if (match_attr(nm, "fan", "_input", &num))
            type = SENS_FAN;
        else if (match_attr(nm, "power", "_average", &num))
            type = SENS_POWER;
        else if (match_attr(nm, "power", "_input", &num)) {
            type = SENS_POWER;
            pref = 1;
        }
        if (type < 0)
            continue;
        c[nc].type = type;
        c[nc].num = num;
        c[nc].pref = pref;
        str_copy(c[nc].file, sizeof c[nc].file, nm);
        nc++;
    }
    closedir(d);
    qsort(c, (size_t)nc, sizeof *c, cand_cmp);

    static const char *const prefix[] = { "temp", "fan", "power" };
    int added = 0;
    for (int i = 0; i < nc && S->n < MAX_SENSORS; i++) {
        if (i && c[i].type == c[i - 1].type && c[i].num == c[i - 1].num)
            continue; /* power*_average preferred over power*_input */
        sensor_t *s = &S->s[S->n];
        memset(s, 0, sizeof *s);
        s->type = c[i].type;
        s->chip_idx = chip_idx;
        s->crit = -1;
        str_copy(s->chip, sizeof s->chip, chip);
        sfmt(s->path, sizeof s->path, "%s/%s", dir, c[i].file);

        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%s%d_label", dir, prefix[s->type], c[i].num);
        if (read_line(p, s->label, sizeof s->label) || !s->label[0])
            sfmt(s->label, sizeof s->label, "%s%d", prefix[s->type], c[i].num);
        if (s->type == SENS_TEMP) {
            long long v;
            sfmt(p, sizeof p, "%s/temp%d_crit", dir, c[i].num);
            if (read_ll(p, &v) || v <= 0) {
                sfmt(p, sizeof p, "%s/temp%d_max", dir, c[i].num);
                if (read_ll(p, &v))
                    v = 0;
            }
            if (v > 0 && v < 200000)
                s->crit = (double)v / 1000.0;
        }
        S->n++;
        added++;
    }
    return added;
}

static int cpu_score(const sensor_t *s)
{
    if (s->type != SENS_TEMP)
        return 0;
    const char *c = s->chip, *l = s->label;
    if (!strncmp(c, "coretemp", 8))
        return !strncmp(l, "Package", 7) ? 100 : 50;
    if (!strncmp(c, "k10temp", 7) || !strncmp(c, "zenpower", 8))
        return !strcmp(l, "Tctl") ? 100 : !strcmp(l, "Tdie") ? 95 : 60;
    if (!strncmp(c, "cpu", 3) || strstr(c, "cpu_thermal") || strstr(c, "soc_thermal"))
        return 40;
    if (!strncmp(c, "thinkpad", 8) && !strcmp(l, "CPU"))
        return 30;
    if (!strncmp(c, "acpitz", 6))
        return 10;
    return 0;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

void sensors_init(sensors_t *S)
{
    char base[PATHBUF];
    S->n = 0;
    S->cpu_idx = -1;
    sfmt(base, sizeof base, "%s/class/hwmon", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    int ids[128], nid = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nid < 128)
        if (!strncmp(e->d_name, "hwmon", 5) && e->d_name[5])
            ids[nid++] = atoi(e->d_name + 5);
    closedir(d);
    qsort(ids, (size_t)nid, sizeof ids[0], cmp_int);

    char names[128][32];
    for (int i = 0; i < nid; i++) {
        char dir[PATHBUF], p[PATHBUF], name[32];
        sfmt(dir, sizeof dir, "%s/hwmon%d", base, ids[i]);
        sfmt(p, sizeof p, "%s/name", dir);
        if (read_line(p, name, sizeof name))
            sfmt(name, sizeof name, "hwmon%d", ids[i]);
        str_copy(names[i], sizeof names[i], name);
        int same = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(names[j], name))
                same++;
        char chip[32];
        if (same) {
            char shortname[24];
            str_copy(shortname, sizeof shortname, name);
            sfmt(chip, sizeof chip, "%s#%d", shortname, same + 1);
        } else {
            str_copy(chip, sizeof chip, name);
        }
        if (!scan_chip(S, dir, chip, i)) { /* very old kernels keep attributes in device/ */
            sfmt(p, sizeof p, "%s/device", dir);
            scan_chip(S, p, chip, i);
        }
    }

    int best = 0;
    for (int i = 0; i < S->n; i++) {
        int sc = cpu_score(&S->s[i]);
        if (sc > best) {
            best = sc;
            S->cpu_idx = i;
        }
    }
}

void sensors_sample(sensors_t *S)
{
    for (int i = 0; i < S->n; i++) {
        sensor_t *s = &S->s[i];
        long long v;
        s->ok = read_ll(s->path, &v) == 0;
        if (!s->ok)
            continue;
        switch (s->type) {
        case SENS_TEMP: s->value = (double)v / 1000.0; break; /* millidegree C */
        case SENS_FAN: s->value = (double)v; break;           /* RPM */
        case SENS_POWER: s->value = (double)v / 1e6; break;   /* microwatt */
        }
    }
}

double sensors_cpu_temp(const sensors_t *S)
{
    if (S->cpu_idx < 0 || !S->s[S->cpu_idx].ok)
        return -1;
    return S->s[S->cpu_idx].value;
}
