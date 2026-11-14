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

/* programs.c - what ran each day
 *
 * The hsm that keeps the history (history.c) adds up, for every program
 * name, the CPU time, GPU time and bytes written today. The top ten of each
 * go into programs.csv next to history.csv, so the History view can answer
 * "what kept the fans going yesterday?". Programs are grouped by the name
 * the kernel knows them by, unchanged; kernel threads are left out. */
#define _GNU_SOURCE
#include "history.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 1024     /* names followed in a day (a hash table) */
#define KEEP_DAYS 400  /* days kept in the file */

typedef struct {
    char name[24];
    double v[PROG_KINDS]; /* CPU s, GPU s, bytes written */
} prog_t;

static struct {
    prog_t t[SLOTS];
    int n, day;
} P;

static const char *const KIND_KEY[PROG_KINDS] = { "cpu", "gpu", "write" };

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* the entry for a name, made if needed (NULL when the day has too many) */
static prog_t *slot(const char *name)
{
    unsigned i = hash(name) & (SLOTS - 1);
    for (int k = 0; k < SLOTS; k++, i = (i + 1) & (SLOTS - 1)) {
        prog_t *e = &P.t[i];
        if (!e->name[0]) {
            if (P.n >= SLOTS * 3 / 4)
                return NULL;
            str_copy(e->name, sizeof e->name, name);
            P.n++;
            return e;
        }
        if (!strcmp(e->name, name))
            return e;
    }
    return NULL;
}

/* names as the kernel has them, but nothing that would break a line */
static void clean_name(char *dst, size_t n, const char *src)
{
    str_copy(dst, n, src);
    for (char *p = dst; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127)
            *p = '?';
}

void prog_sample(const sys_t *s, double dt)
{
    if (dt <= 0 || dt > 30)
        return;
    const procs_t *ps = &s->procs;
    for (int i = 0; i < ps->n; i++) {
        const proc_t *p = &ps->p[i];
        if (p->kthread)
            continue;
        double v[PROG_KINDS] = { MAX(p->cpu, 0) / 100.0 * dt, p->gpu > 0 ? p->gpu / 100.0 * dt : 0,
                                 p->io_ok && p->wr_bps > 0 ? p->wr_bps * dt : 0 };
        if (v[0] < 1e-3 && v[1] <= 0 && v[2] <= 0)
            continue;
        char nm[24];
        clean_name(nm, sizeof nm, p->comm);
        prog_t *e = slot(nm);
        if (!e)
            continue;
        for (int k = 0; k < PROG_KINDS; k++)
            e->v[k] += v[k];
    }
}

/* -------------------------------------------------------------- the file */

static void file_path(char *out, size_t n)
{
    sfmt(out, n, "%s/programs.csv", config_state_dir());
}

static void day_text(int day, char *out, size_t n)
{
    sfmt(out, n, "%04d-%02d-%02d", day / 10000, day / 100 % 100, day % 100);
}

static void put_name(FILE *f, const char *name)
{
    fputc('"', f);
    for (const char *p = name; *p; p++) {
        if (*p == '"')
            fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

/* "2026-11-13,cpu,\"firefox\",3720.5": 0 = ok */
static int parse_line(char *line, int *day, int *kind, char *name, size_t nn, double *v)
{
    int y, m, d;
    if (sscanf(line, "%4d-%2d-%2d,", &y, &m, &d) != 3)
        return -1;
    *day = y * 10000 + m * 100 + d;
    char *p = line + 11;
    *kind = -1;
    for (int k = 0; k < PROG_KINDS; k++) {
        size_t l = strlen(KIND_KEY[k]);
        if (!strncmp(p, KIND_KEY[k], l) && p[l] == ',') {
            *kind = k;
            p += l + 1;
        }
    }
    if (*kind < 0 || *p != '"')
        return -1;
    size_t o = 0;
    for (p++; *p; p++) {
        if (*p == '"') {
            if (p[1] != '"')
                break;
            p++;
        }
        if (o + 1 < nn)
            name[o++] = *p;
    }
    name[o] = '\0';
    if (*p != '"' || p[1] != ',')
        return -1;
    *v = strtod(p + 2, NULL);
    return isfinite(*v) && *v >= 0 ? 0 : -1;
}

/* the top of a kind, biggest first */
static int top_of(int kind, prog_top_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        const prog_t *e = &P.t[i];
        double v = e->v[kind];
        if (!e->name[0] || v <= (kind == 2 ? 1024.0 * 1024.0 : 1.0)) /* under a megabyte or a second */
            continue;
        if (n == max && v <= out[max - 1].v)
            continue; /* not among the top */
        int k = n < max ? n++ : max - 1; /* a free place, or over the smallest */
        while (k > 0 && out[k - 1].v < v) {
            out[k] = out[k - 1];
            k--;
        }
        str_copy(out[k].name, sizeof out[k].name, e->name);
        out[k].v = v;
    }
    return n;
}

void prog_flush(void)
{
    if (!P.day)
        return;
    char p[PATHBUF], tmp[PATHBUF + 8], today[16];
    file_path(p, sizeof p);
    sfmt(tmp, sizeof tmp, "%s.tmp", p);
    day_text(P.day, today, sizeof today);
    /* the oldest day kept: KEEP_DAYS before today */
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = P.day / 10000 - 1900;
    tm.tm_mon = P.day / 100 % 100 - 1;
    tm.tm_mday = P.day % 100 - KEEP_DAYS;
    tm.tm_hour = 12;
    mktime(&tm);
    int oldest = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;

    int who = config_as_user();
    if (who < 0)
        return;
    FILE *in = fopen(p, "r");
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        if (in)
            fclose(in);
        config_as_root(who);
        return;
    }
    fprintf(f, "date,kind,name,amount\n");
    char line[512];
    while (in && fgets(line, sizeof line, in)) {
        char copy[512], nm[64];
        int day, kind;
        double v;
        str_copy(copy, sizeof copy, line);
        if (parse_line(copy, &day, &kind, nm, sizeof nm, &v) != 0 || day == P.day || day < oldest)
            continue;
        fputs(line, f);
        if (line[strlen(line) - 1] != '\n')
            fputc('\n', f);
    }
    for (int k = 0; k < PROG_KINDS; k++) {
        prog_top_t top[PROG_TOP];
        int n = top_of(k, top, PROG_TOP);
        for (int i = 0; i < n; i++) {
            fprintf(f, "%s,%s,", today, KIND_KEY[k]);
            put_name(f, top[i].name);
            fprintf(f, k == 2 ? ",%.0f\n" : ",%.1f\n", top[i].v);
        }
    }
    if (in)
        fclose(in);
    if (fclose(f) == 0)
        rename(tmp, p);
    else
        unlink(tmp);
    config_as_root(who);
}

/* a new day, or the writer starting: go on from what the file has for it */
void prog_day_start(int day)
{
    memset(&P, 0, sizeof P);
    P.day = day;
    char p[PATHBUF], line[512];
    file_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char nm[64];
        int d, kind;
        double v;
        if (parse_line(line, &d, &kind, nm, sizeof nm, &v) != 0 || d != day)
            continue;
        prog_t *e = slot(nm);
        if (e)
            e->v[kind] = v;
    }
    fclose(f);
}

/* ------------------------------------------------------- the History view */

static struct {
    time_t mtime;
    off_t size;
    double checked;
    int day;            /* the day asked for last */
    prog_day_t got;
} C;

int prog_get_day(int day, prog_day_t *out)
{
    memset(out, 0, sizeof *out);
    if (history_writer() && day == P.day) { /* today, as this hsm counts it */
        for (int k = 0; k < PROG_KINDS; k++)
            out->n[k] = top_of(k, out->top[k], PROG_TOP);
        return out->n[0] + out->n[1] + out->n[2] ? 0 : -1;
    }
    char p[PATHBUF];
    file_path(p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0)
        return -1;
    double now = now_sec();
    if (day == C.day && st.st_mtime == C.mtime && st.st_size == C.size && now - C.checked < 60) {
        *out = C.got;
        return out->n[0] + out->n[1] + out->n[2] ? 0 : -1;
    }
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char nm[64];
        int d, kind;
        double v;
        if (parse_line(line, &d, &kind, nm, sizeof nm, &v) != 0 || d != day || out->n[kind] >= PROG_TOP)
            continue;
        prog_top_t *t = &out->top[kind][out->n[kind]++];
        str_copy(t->name, sizeof t->name, nm);
        t->v = v;
    }
    fclose(f);
    C.day = day;
    C.mtime = st.st_mtime;
    C.size = st.st_size;
    C.checked = now;
    C.got = *out;
    return out->n[0] + out->n[1] + out->n[2] ? 0 : -1;
}
