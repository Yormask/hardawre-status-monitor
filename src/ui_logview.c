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
/* ui_logview.c - saved logs inside hsm (Logging view, key v): a list of the
 * CSV files in the log folder, and a viewer that graphs any column over
 * time, stacks a few of them, and jumps between the events - so after a
 * crash you don't need a spreadsheet to see what happened. */
#define _GNU_SOURCE
#include "log.h"
#include "ui_int.h"
#include "watch.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_VALUES (8 * 1024 * 1024) /* floats kept in memory; bigger logs are thinned out */
#define MAX_PINS 3

/* ============================================================ the list */

enum { LF_SYSTEM, LF_PROCESS, LF_INCIDENT, LF_OTHER };
static const char *const LF_NAME[] = { "system", "process", "incident", "log" };

typedef struct {
    char path[PATHBUF], name[128], detail[64];
    int kind;
    double t; /* when it starts (from the name), or when it was last written */
    long long size;
} lfile_t;

static struct {
    lfile_t *f;
    int n, cap;
    double scanned;
    char dir[PATHBUF];
    long long total;
} L;

/* "2026-11-12_01-23-40" anywhere in a name -> local time */
static double name_time(const char *n, const char **end)
{
    for (const char *p = n; *p; p++) {
        int Y, M, D, h, m, s;
        char sep;
        if (!isdigit((unsigned char)*p))
            continue;
        if (sscanf(p, "%4d-%2d-%2d%c%2d-%2d-%2d", &Y, &M, &D, &sep, &h, &m, &s) == 7 && sep == '_' &&
            strlen(p) >= 19) {
            struct tm tm = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m,
                             .tm_sec = s, .tm_isdst = -1 };
            if (end)
                *end = p + 19;
            return (double)mktime(&tm);
        }
    }
    return -1;
}

static void add_file(const char *dir, const char *name, const struct stat *st)
{
    size_t l = strlen(name);
    if (l < 9 || strncmp(name, "hsm-", 4) || strcmp(name + l - 4, ".csv"))
        return;
    if (L.n == L.cap) {
        int nc = L.cap ? L.cap * 2 : 64;
        lfile_t *nf = realloc(L.f, (size_t)nc * sizeof *nf);
        if (!nf)
            return;
        L.f = nf;
        L.cap = nc;
    }
    lfile_t *f = &L.f[L.n++];
    memset(f, 0, sizeof *f);
    sfmt(f->path, sizeof f->path, "%s/%s", dir, name);
    str_copy(f->name, sizeof f->name, name);
    f->size = (long long)st->st_size;
    L.total += f->size;
    const char *after = NULL;
    double t = name_time(name, &after);
    f->t = t > 0 ? t : (double)st->st_mtime;
    if (!strncmp(name, "hsm-system-", 11)) {
        f->kind = LF_SYSTEM;
    } else if (!strncmp(name, "hsm-incident-", 13)) {
        f->kind = LF_INCIDENT;
        if (after && *after == '-') { /* what set it off: "thermal" */
            str_copy(f->detail, sizeof f->detail, after + 1);
            f->detail[strcspn(f->detail, ".-")] = '\0';
            if (!strcmp(f->detail, "shutdown")) /* saved at the next start, from the rolling files */
                str_copy(f->detail, sizeof f->detail, "the computer went down: the minutes before");
            else if (!strcmp(f->detail, "killed"))
                str_copy(f->detail, sizeof f->detail, "hsm was killed or crashed: the minutes before");
        }
    } else if (!strncmp(name, "hsm-process-", 12)) {
        f->kind = LF_PROCESS;
        int pid = atoi(name + 12);
        const char *nm = strchr(name + 12, '-');
        const char *ts = NULL;
        name_time(name, &ts);
        if (nm && ts && ts - 19 > nm + 1) {
            char comm[40];
            int len = (int)MIN((size_t)(ts - 19 - (nm + 1) - 1), sizeof comm - 1);
            memcpy(comm, nm + 1, (size_t)MAX(len, 0));
            comm[MAX(len, 0)] = '\0';
            sfmt(f->detail, sizeof f->detail, "%s (PID %d)", comm, pid);
        }
    } else {
        f->kind = LF_OTHER;
    }
}

static int cmp_file(const void *a, const void *b)
{
    const lfile_t *x = a, *y = b;
    if (x->t != y->t)
        return x->t < y->t ? 1 : -1; /* newest first */
    return strcmp(y->name, x->name);
}

static void scan(const char *dir)
{
    L.n = 0;
    L.total = 0;
    str_copy(L.dir, sizeof L.dir, dir);
    L.scanned = now_sec();
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char p[PATHBUF];
        struct stat st;
        sfmt(p, sizeof p, "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0)
            continue;
        if (S_ISREG(st.st_mode)) {
            add_file(dir, e->d_name, &st);
        } else if (S_ISDIR(st.st_mode) && strlen(e->d_name) == 10 && e->d_name[4] == '-') { /* a day folder */
            DIR *sd = opendir(p);
            struct dirent *se;
            while (sd && (se = readdir(sd)) != NULL) {
                char q[PATHBUF];
                struct stat sst;
                sfmt(q, sizeof q, "%s/%s", p, se->d_name);
                if (lstat(q, &sst) == 0 && S_ISREG(sst.st_mode))
                    add_file(p, se->d_name, &sst);
            }
            if (sd)
                closedir(sd);
        }
    }
    closedir(d);
    if (L.n > 1)
        qsort(L.f, (size_t)L.n, sizeof *L.f, cmp_file);
}

static time_t midnight(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static int split_csv(char *s, char **f, int max);

/* What set off an incident log: the events in the row of the problem
 * ("thermal: CPU throttled (96 °C) while ..."). Read once per file. */
static void incident_reason(lfile_t *f)
{
    static struct {
        char path[PATHBUF];
        long long size;
        char text[64];
    } cache[64];
    static int next;
    for (int i = 0; i < 64; i++)
        if (cache[i].size == f->size && !strcmp(cache[i].path, f->path)) {
            if (cache[i].text[0])
                str_copy(f->detail, sizeof f->detail, cache[i].text);
            return;
        }
    int slot = next++ % 64;
    str_copy(cache[slot].path, sizeof cache[slot].path, f->path);
    cache[slot].size = f->size;
    cache[slot].text[0] = '\0';
    FILE *in = fopen(f->path, "re");
    if (!in)
        return;
    char *line = NULL;
    size_t cap = 0;
    static char *fld[4096];
    int rel = -1, ev = -1;
    for (int k = 0; k < 4000 && getline(&line, &cap, in) > 0; k++) {
        line[strcspn(line, "\n")] = '\0';
        int n = split_csv(line, fld, 4096);
        if (k == 0) {
            for (int i = 0; i < n; i++) {
                if (!strcmp(fld[i], "from_problem_s"))
                    rel = i;
                if (!strcmp(fld[i], "Events"))
                    ev = i;
            }
            if (ev < 0)
                break;
            continue;
        }
        if (ev < n && fld[ev][0] && (rel < 0 || rel >= n || atof(fld[rel]) >= -0.5)) {
            const char *t = fld[ev];
            if (strlen(t) > 9 && t[2] == ':' && t[5] == ':') /* drop the time: the list shows it */
                t += 9;
            str_copy(cache[slot].text, sizeof cache[slot].text, t);
            char *bar = strstr(cache[slot].text, " | ");
            if (bar)
                *bar = '\0';
            str_copy(f->detail, sizeof f->detail, cache[slot].text);
            break;
        }
    }
    free(line);
    fclose(in);
}

/* "Today 01:23:40", "Yesterday 14:02:11", "Mon 10 Nov 09:00" */
static void when_text(double t, char *out, size_t n)
{
    time_t tt = (time_t)t, now = time(NULL);
    struct tm a;
    localtime_r(&tt, &a);
    char hm[16];
    strftime(hm, sizeof hm, "%H:%M:%S", &a);
    long days = (long)((midnight(now) - midnight(tt) + 43200) / 86400); /* + 12 h: DST days are 23 or 25 h */
    if (days == 0)
        sfmt(out, n, "Today %s", hm);
    else if (days == 1)
        sfmt(out, n, "Yesterday %s", hm);
    else
        strftime(out, n, days < 300 ? "%a %d %b %H:%M" : "%d %b %Y %H:%M", &a);
}

/* ============================================================ the data */

typedef struct {
    double t;
    char *text;
} lev_t;

static struct {
    int loaded;
    char path[PATHBUF], name[128], err[200];
    int ncol;
    char (*label)[64];
    char (*unit)[12];
    unsigned char *whole; /* the column only ever holds whole numbers (counts) */
    int n, cap, stride;
    double *t;
    float *v; /* n x ncol */
    int nev, evcap;
    lev_t *ev;
    double step; /* typical time between rows */
    double trig; /* incident: when the problem happened (0 = n/a) */

    /* view */
    int col, cscroll, pin[MAX_PINS], npin, evsel, evscroll;
    double v0, v1, cur;
} V;

static void data_free(void)
{
    free(V.label);
    free(V.unit);
    free(V.whole);
    V.whole = NULL;
    free(V.t);
    free(V.v);
    for (int i = 0; i < V.nev; i++)
        free(V.ev[i].text);
    free(V.ev);
    V.label = NULL;
    V.unit = NULL;
    V.t = NULL;
    V.v = NULL;
    V.ev = NULL;
    V.n = V.cap = V.ncol = V.nev = V.evcap = 0;
    V.loaded = 0;
}

/* split one CSV line in place; handles "quoted, fields" and "" */
static int split_csv(char *s, char **f, int max)
{
    int n = 0;
    while (n < max) {
        if (*s == '"') {
            char *w = ++s;
            f[n++] = w;
            while (*s) {
                if (*s == '"' && s[1] == '"') {
                    *w++ = '"';
                    s += 2;
                } else if (*s == '"') {
                    s++;
                    break;
                } else {
                    *w++ = *s++;
                }
            }
            *w = '\0';
            while (*s && *s != ',') /* junk after the closing quote */
                s++;
        } else {
            f[n++] = s;
            s += strcspn(s, ",");
        }
        if (*s != ',') {
            *s = '\0';
            break;
        }
        *s++ = '\0';
    }
    return n;
}

static void add_event(double t, const char *text)
{
    if (V.nev == V.evcap) {
        int nc = V.evcap ? V.evcap * 2 : 64;
        lev_t *ne = realloc(V.ev, (size_t)nc * sizeof *ne);
        if (!ne)
            return;
        V.ev = ne;
        V.evcap = nc;
    }
    V.ev[V.nev].t = t;
    V.ev[V.nev].text = strdup(text);
    if (V.ev[V.nev].text)
        V.nev++;
}

/* "12:03:44 disk: nvme0n1: I/O error | 12:03:50 thermal: ..." -> one event each */
static void add_events(double row_t, char *s)
{
    while (s && *s) {
        char *bar = strstr(s, " | ");
        if (bar)
            *bar = '\0';
        double t = row_t;
        int h, m, sec;
        if (sscanf(s, "%2d:%2d:%2d", &h, &m, &sec) == 3) { /* the event's own time, on the row's day */
            time_t rt = (time_t)row_t;
            struct tm tm;
            localtime_r(&rt, &tm);
            double back = (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) - (h * 3600 + m * 60 + sec);
            if (back < -43200)
                back += 86400;
            if (back >= 0 && back < 3600)
                t = row_t - back;
        }
        add_event(t, s);
        s = bar ? bar + 3 : NULL;
    }
}

/* "2026-11-12 01:23:40" (+ "+01:00") -> seconds since the epoch */
static double parse_time(const char *s, const char *off)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return NAN;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int oh, om;
    char sign;
    if (off && sscanf(off, "%c%d:%d", &sign, &oh, &om) == 3 && (sign == '+' || sign == '-')) {
        long o = (oh * 60L + om) * 60L * (sign == '-' ? -1 : 1);
        return (double)(timegm(&tm) - o);
    }
    tm.tm_isdst = -1; /* logs from before 1.6: local time */
    return (double)mktime(&tm);
}

static int load(const char *path)
{
    data_free();
    memset(&V, 0, sizeof V);
    str_copy(V.path, sizeof V.path, path);
    const char *b = strrchr(path, '/');
    str_copy(V.name, sizeof V.name, b ? b + 1 : path);
    FILE *f = fopen(path, "re");
    if (!f) {
        sfmt(V.err, sizeof V.err, "Could not open it: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    long long size = fstat(fileno(f), &st) == 0 ? (long long)st.st_size : 0;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len = getline(&line, &lcap, f);
    if (len > 0)
        line[strcspn(line, "\r\n")] = '\0';
    static char *fld[4096];
    int nf = len > 0 ? split_csv(line, fld, 4096) : 0;
    if (nf < 2 || strcmp(fld[0], "time")) {
        str_copy(V.err, sizeof V.err, "This is not an hsm log (no \"time\" column)");
        free(line);
        fclose(f);
        return -1;
    }
    /* which fields are values */
    static int map[4096]; /* field -> value column, -1 = not a value */
    int off_f = -1, ev_f = -1, rel_f = -1;
    V.label = calloc((size_t)nf, sizeof *V.label);
    V.unit = calloc((size_t)nf, sizeof *V.unit);
    V.whole = calloc((size_t)nf, 1);
    if (!V.label || !V.unit || !V.whole) {
        free(line);
        fclose(f);
        return -1;
    }
    for (int i = 0; i < nf; i++) {
        map[i] = -1;
        if (i == 0)
            continue;
        if (!strcmp(fld[i], "utc_offset"))
            off_f = i;
        else if (!strcmp(fld[i], "Events"))
            ev_f = i;
        else if (!strcmp(fld[i], "from_problem_s"))
            rel_f = i;
        else if (strcmp(fld[i], "elapsed_s") && strcmp(fld[i], "pid") && strcmp(fld[i], "state")) {
            int c = V.ncol++;
            map[i] = c;
            str_copy(V.label[c], sizeof V.label[c], fld[i]);
            char *par = strrchr(V.label[c], '(');
            size_t ll = strlen(V.label[c]);
            /* "CPU total (%)"; but "Load average (1 min)" has no unit */
            if (par && par > V.label[c] && V.label[c][ll - 1] == ')' && ll - (size_t)(par - V.label[c]) <= 8 &&
                !memchr(par, ' ', ll - (size_t)(par - V.label[c]))) {
                str_copy(V.unit[c], sizeof V.unit[c], par + 1);
                V.unit[c][strcspn(V.unit[c], ")")] = '\0';
                par[par[-1] == ' ' ? -1 : 0] = '\0';
            }
        }
    }
    /* thin out big logs: keep the highest value of every few rows, so spikes survive.
     * The number of rows is guessed from the size of the first one. */
    long data = ftell(f);
    ssize_t first = getline(&line, &lcap, f);
    if (data < 0 || fseek(f, data, SEEK_SET) != 0)
        first = -1;
    long long est = first > 0 ? (size - data) / first : 0;
    V.stride = V.ncol && est * V.ncol > MAX_VALUES ? (int)((est * V.ncol + MAX_VALUES - 1) / MAX_VALUES) : 1;
    int k = 0;
    double rel0 = NAN;
    while (getline(&line, &lcap, f) > 0) {
        line[strcspn(line, "\r\n")] = '\0'; /* also files saved by a spreadsheet on Windows */
        int n = split_csv(line, fld, MIN(nf, 4096));
        if (n < 1)
            continue;
        double t = parse_time(fld[0], off_f >= 0 && off_f < n ? fld[off_f] : NULL);
        if (isnan(t))
            continue;
        if (ev_f >= 0 && ev_f < n && fld[ev_f][0])
            add_events(t, fld[ev_f]);
        if (rel_f >= 0 && rel_f < n && isnan(rel0) && atof(fld[rel_f]) >= 0)
            rel0 = t;
        if (k == 0) { /* a new row */
            if (V.n == V.cap) {
                int nc = V.cap ? V.cap * 2 : 1024;
                double *nt = realloc(V.t, (size_t)nc * sizeof *nt);
                if (!nt)
                    break;
                V.t = nt;
                float *nv = realloc(V.v, (size_t)nc * (size_t)MAX(V.ncol, 1) * sizeof *nv);
                if (!nv)
                    break;
                V.v = nv;
                V.cap = nc;
            }
            V.t[V.n] = t;
            for (int c = 0; c < V.ncol; c++)
                V.v[(size_t)V.n * V.ncol + c] = NAN;
            V.n++;
        }
        float *row = &V.v[(size_t)(V.n - 1) * V.ncol];
        for (int i = 1; i < n; i++) {
            int c = map[i];
            if (c < 0 || !fld[i][0])
                continue;
            char *e;
            float v = strtof(fld[i], &e);
            if (e != fld[i] && (isnan(row[c]) || v > row[c]))
                row[c] = v;
        }
        k = (k + 1) % V.stride;
    }
    free(line);
    fclose(f);
    if (!V.n) {
        str_copy(V.err, sizeof V.err, "The log has no rows yet");
        return -1;
    }
    /* typical gap between rows (the median of the first ones) */
    double gaps[128];
    int ng = 0;
    for (int i = 1; i < V.n && ng < 128; i++)
        if (V.t[i] > V.t[i - 1])
            gaps[ng++] = V.t[i] - V.t[i - 1];
    for (int i = 1; i < ng; i++)
        for (int j = i; j > 0 && gaps[j] < gaps[j - 1]; j--) {
            double x = gaps[j];
            gaps[j] = gaps[j - 1];
            gaps[j - 1] = x;
        }
    V.step = ng ? gaps[ng / 2] : 1;
    for (int c = 0; c < V.ncol; c++) { /* counts: shown without decimals */
        int whole = 1;
        for (int r = 0; r < V.n && whole; r++) {
            float v = V.v[(size_t)r * V.ncol + c];
            whole = isnan(v) || v == floorf(v);
        }
        V.whole[c] = (unsigned char)whole;
    }
    V.trig = isnan(rel0) ? 0 : rel0;
    V.loaded = 1;
    return 0;
}

/* first row at or after t */
static int row_at(double t)
{
    int lo = 0, hi = V.n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (V.t[mid] < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int nearest_row(double t)
{
    int r = row_at(t);
    if (r >= V.n)
        return V.n - 1;
    if (r > 0 && t - V.t[r - 1] < V.t[r] - t)
        return r - 1;
    return r;
}

static int find_col(const char *const *words)
{
    for (int w = 0; words[w]; w++)
        for (int c = 0; c < V.ncol; c++)
            if (strcasestr(V.label[c], words[w]))
                return c;
    return 0;
}

static void fit(void)
{
    V.v0 = V.t[0];
    V.v1 = MAX(V.t[V.n - 1], V.t[0] + 10);
}

/* sensible first look: the column the problem was about, cursor on the problem */
static void first_look(void)
{
    fit();
    static const char *const thermal[] = { "CPU temperature", "temperature", "Package", NULL };
    static const char *const memory[] = { "Memory used", "Memory", NULL };
    static const char *const disk[] = { "read time", "write time", "busy", NULL };
    static const char *const cpu[] = { "CPU total", "CPU (%", "CPU", NULL };
    const char *n = V.name;
    V.col = strstr(n, "thermal") ? find_col(thermal)
          : strstr(n, "memory")  ? find_col(memory)
          : strstr(n, "disk")    ? find_col(disk)
                                 : find_col(cpu);
    V.cur = V.trig > 0 ? V.trig : V.nev ? V.ev[0].t : V.t[V.n - 1];
    V.evsel = 0;
    for (int i = 1; i < V.nev; i++)
        if (fabs(V.ev[i].t - V.cur) < fabs(V.ev[V.evsel].t - V.cur))
            V.evsel = i;
}

int lv_open(ui_state_t *u, const char *path)
{
    u->view = VIEW_LOG;
    u->log_mode = 2;
    str_copy(u->view_path, sizeof u->view_path, path);
    if (load(path) != 0) {
        ui_message(u, 1, "%s: %s", V.name, V.err);
        u->log_mode = 1;
        return -1;
    }
    first_look();
    return 0;
}

/* ============================================================ drawing */

static void fmt_val(double v, const char *unit, int whole, char *out, size_t n)
{
    if (isnan(v)) {
        str_copy(out, n, "-");
        return;
    }
    double a = fabs(v);
    int dec = whole || a >= 1000 || !strcmp(unit, "MHz") || !strcmp(unit, "RPM") ? 0 : a >= 10 ? 1 : 2;
    if (!strcmp(unit, "C"))
        sfmt(out, n, "%.1f%s", v, DEG);
    else if (!strcmp(unit, "%"))
        sfmt(out, n, "%.*f%%", a >= 100 || whole ? 0 : 1, v);
    else if (*unit)
        sfmt(out, n, "%.*f %s", dec, v, unit);
    else
        sfmt(out, n, "%.*f", dec, v);
}

static void time_text(double t, double span, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, n, span > 20 * 3600 ? "%d %b %H:%M" : span > 3600 ? "%H:%M" : "%H:%M:%S", &tm);
}

static attr_t g_gcol[4];

static void graph_one(int c, int y, int x, int h, int w, double dt, int curx, char *range, size_t rn)
{
    static float *col;
    static int ccap;
    if (ccap < w) {
        float *nc = realloc(col, (size_t)(w + 64) * sizeof *nc);
        if (!nc)
            return;
        col = nc;
        ccap = w + 64;
    }
    /* each screen column: the highest value in its time slice */
    double lo = INFINITY, hi = -INFINITY;
    int r = row_at(V.v0);
    float last = NAN;
    double last_t = -1e18;
    if (r > 0) {
        last = V.v[(size_t)(r - 1) * V.ncol + c];
        last_t = V.t[r - 1];
    }
    for (int k = 0; k < w; k++) {
        double t1 = V.v0 + (k + 1) * dt;
        float m = NAN;
        for (; r < V.n && V.t[r] < t1; r++) {
            float v = V.v[(size_t)r * V.ncol + c];
            if (!isnan(v) && (isnan(m) || v > m))
                m = v;
            if (!isnan(v)) {
                last = v;
                last_t = V.t[r];
            }
        }
        if (isnan(m) && !isnan(last) && V.v0 + k * dt - last_t < 3 * MAX(V.step, dt))
            m = last; /* zoomed in past the rows: hold the value */
        col[k] = m;
        if (!isnan(m)) {
            lo = MIN(lo, m);
            hi = MAX(hi, m);
        }
    }
    const char *unit = V.unit[c];
    range[0] = '\0';
    if (isinf(lo)) {
        put(y + h / 2, x + w / 2 - 6, 14, A_GRAY, "no data here");
        return;
    }
    /* the scale: percentages from 0, the rest around what is visible */
    if (!strcmp(unit, "%")) {
        lo = 0;
        hi = MAX(hi, 100);
    } else {
        double pad = (hi - lo) * 0.1;
        if (pad <= 0)
            pad = fabs(hi) * 0.1 + 1;
        lo = lo >= 0 && lo - pad < 0 ? 0 : lo - pad;
        hi += pad;
    }
    char a[24], b[24];
    fmt_val(lo, unit, V.whole[c], a, sizeof a);
    fmt_val(hi, unit, V.whole[c], b, sizeof b);
    sfmt(range, rn, "scale %s %s %s", a, U ? "–" : "-", b);
    if (curx >= 0 && curx < w) /* the cursor: a thin line, the bar it crosses stands out */
        for (int k = 0; k < h; k++)
            put(y + k, x + curx, 1, A_GRAY, "%s", U ? "│" : "|");
    graph_cols(y, x, h, w, col, lo, hi, g_gcol[0], curx, CP(C_WARN) | A_BOLD);
}

void lv_draw(sys_t *s, ui_state_t *u)
{
    (void)s;
    int top = 1, H = LINES - 2, W = COLS;
    if (!V.loaded || strcmp(V.path, u->view_path)) {
        if (!u->view_path[0] || load(u->view_path) != 0) {
            box_(top, 0, H, W, "Log", NULL);
            put(top + 2, 3, W - 6, CP(C_CRIT), "%s", V.err[0] ? V.err : "No log open");
            put(top + 3, 3, W - 6, A_GRAY, "Esc goes back to the list of saved logs");
            return;
        }
        first_look();
    }
    g_gcol[0] = CP(C_IN) | A_BOLD;
    g_gcol[1] = CP(C_ACC) | A_BOLD;
    g_gcol[2] = CP(C_WARN) | A_BOLD;
    g_gcol[3] = CP(C_OK) | A_BOLD;

    char info[160], a[32], b[32], dur[24];
    double span = V.t[V.n - 1] - V.t[0];
    time_text(V.t[0], span, a, sizeof a);
    time_text(V.t[V.n - 1], span, b, sizeof b);
    fmt_duration(span, dur, sizeof dur);
    sfmt(info, sizeof info, "%s %s %s%s%s%s%d rows%s", a, U ? "→" : "->", b, SEP, dur, SEP, V.n,
         V.stride > 1 ? " (thinned out)" : "");
    box_(top, 0, H, W, V.name, info);

    /* ---- columns, with the value at the cursor */
    int lw = CLAMP(W / 3, 24, 40), ly = top + 1, lh = H - 2;
    int cr = nearest_row(V.cur);
    V.col = CLAMP(V.col, 0, V.ncol - 1);
    if (V.col < V.cscroll)
        V.cscroll = V.col;
    if (V.col >= V.cscroll + lh)
        V.cscroll = V.col - lh + 1;
    V.cscroll = CLAMP(V.cscroll, 0, MAX(0, V.ncol - lh));
    hit_add(ly, 1, lh, lw - 1, HIT_SCROLL, 1, 0); /* under the rows: the wheel scrolls the list */
    for (int i = 0; i < lh && V.cscroll + i < V.ncol; i++) {
        int c = V.cscroll + i, yy = ly + i, sel = c == V.col, pin = -1;
        for (int p = 0; p < V.npin; p++)
            if (V.pin[p] == c)
                pin = p;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, 1, lw - 1, base);
        char v[24];
        fmt_val(V.v[(size_t)cr * V.ncol + c], V.unit[c], V.whole[c], v, sizeof v);
        int vw = utf8_cols(v);
        put(yy, 2, 2, sel ? base : g_gcol[pin >= 0 ? pin + 1 : 0], "%s", pin >= 0 ? (U ? "■" : "#") : " ");
        put(yy, 4, lw - 6 - vw, sel ? base | A_BOLD : A_NORMAL, "%s", V.label[c]);
        put(yy, lw - 1 - vw, vw, sel ? base : A_GRAY, "%s", v);
        hit_add(yy, 1, 1, lw - 1, HIT_COLUMN, c, 0);
    }
    for (int y = ly; y < ly + lh; y++)
        put(y, lw, 1, CP(C_BORDER), "%s", U ? "│" : "|");

    /* ---- graphs: the selected column, plus the pinned ones */
    int gx = lw + 2, gw = W - gx - 2;
    int list[MAX_PINS + 1], ng = 0;
    list[ng++] = V.col;
    for (int p = 0; p < V.npin; p++)
        if (V.pin[p] != V.col)
            list[ng++] = V.pin[p];
    int evh = V.nev ? CLAMP(lh / 4, 3, 8) : 0;
    int gy = ly + 1;          /* below the cursor line */
    int area = lh - evh - 3; /* minus the cursor line, the time axis and the marker row */
    int each = area / ng;
    while (ng > 1 && each < 4)
        each = area / --ng;
    if (each < 3 || gw < 20)
        return;
    double dt = (V.v1 - V.v0) / gw;
    V.cur = CLAMP(V.cur, V.t[0], V.t[V.n - 1]);
    int curx = (int)((V.cur - V.v0) / dt);

    /* where the cursor is, and how much is shown */
    char ct[64], rel[48] = "", shown[32];
    time_text(V.cur, 0, ct, sizeof ct);
    if (span > 20 * 3600) { /* a log of more than a day: which day, too */
        char d[16];
        time_t tt = (time_t)V.cur;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(d, sizeof d, "%a %d %b ", &tm);
        char both[64];
        sfmt(both, sizeof both, "%s%s", d, ct);
        str_copy(ct, sizeof ct, both);
    }
    if (V.trig > 0) { /* incident logs: time from the problem */
        double d = V.cur - V.trig;
        if (fabs(d) < 0.5)
            str_copy(rel, sizeof rel, "the problem");
        else
            sfmt(rel, sizeof rel, "%.0f s %s the problem", fabs(d), d < 0 ? "before" : "after");
    }
    fmt_duration(V.v1 - V.v0, shown, sizeof shown);
    int cx0 = gx + put(ly, gx, gw, A_BOLD, "%s %s", U ? "▸" : ">", ct);
    if (rel[0])
        cx0 += put(ly, cx0, gx + gw - cx0, V.trig > 0 && fabs(V.cur - V.trig) < 0.5 ? CP(C_CRIT) | A_BOLD : A_GRAY,
                   "  %s", rel);
    char zm[48];
    sfmt(zm, sizeof zm, "%s shown", shown);
    int zw = utf8_cols(zm);
    if (gx + gw - zw > cx0 + 2)
        put(ly, gx + gw - zw, zw, A_GRAY, "%s", zm);
    for (int g = 0; g < ng; g++) {
        int c = list[g], y = gy + g * each;
        char v[24];
        fmt_val(V.v[(size_t)cr * V.ncol + c], V.unit[c], V.whole[c], v, sizeof v);
        int pin = -1;
        for (int p = 0; p < V.npin; p++)
            if (V.pin[p] == c)
                pin = p;
        g_gcol[0] = pin >= 0 ? g_gcol[pin + 1] : CP(C_IN) | A_BOLD;
        int xx = gx + put(y, gx, gw, CP(C_TITLE) | A_BOLD, "%s", V.label[c]);
        if (V.unit[c][0])
            xx += put(y, xx, gx + gw - xx, A_GRAY, " (%s)", V.unit[c]);
        xx += put(y, xx, gx + gw - xx, A_BOLD, "   %s", v);
        char range[64];
        graph_one(c, y + 1, gx, each - 1, gw, dt, curx, range, sizeof range);
        int rw = utf8_cols(range);
        if (gx + gw - rw > xx + 2)
            put(y, gx + gw - rw, rw, A_GRAY, "%s", range);
        hit_add(y + 1, gx, each - 1, gw, HIT_GRAPH, gx, gw);
        g_gcol[0] = CP(C_IN) | A_BOLD;
    }

    /* ---- time axis, event markers */
    int ay = gy + ng * each;
    double step = 1;
    static const double nice[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800,
                                   21600, 43200, 86400, 172800, 604800 };
    double vs = V.v1 - V.v0;
    char sample[24];
    time_text(V.v0, vs, sample, sizeof sample);
    int labw = utf8_cols(sample) + 3; /* room for a label and a gap */
    for (size_t i = 0; i < sizeof nice / sizeof nice[0]; i++) {
        step = nice[i];
        if (gw * step / vs >= labw)
            break;
    }
    time_t t0 = (time_t)V.v0;
    struct tm tm;
    localtime_r(&t0, &tm);
    double first = ceil((V.v0 + tm.tm_gmtoff) / step) * step - tm.tm_gmtoff; /* on local round times */
    for (double t = first; t < V.v1; t += step) {
        int x = (int)((t - V.v0) / dt);
        char lab[24];
        time_text(t, vs, lab, sizeof lab);
        if (x >= 0 && x + utf8_cols(lab) < gw) {
            put(ay, gx + x, 1, A_GRAY, "%s", U ? "╵" : "'");
            put(ay, gx + x + 1, utf8_cols(lab), A_GRAY, "%s", lab);
        }
    }
    for (int i = 0; i < V.nev; i++) {
        int x = (int)((V.ev[i].t - V.v0) / dt);
        if (x >= 0 && x < gw)
            put(ay + 1, gx + x, 1, i == V.evsel ? CP(C_CRIT) | A_BOLD : CP(C_WARN) | A_BOLD, "%s", U ? "▲" : "^");
    }

    /* ---- events */
    if (!evh)
        return;
    int ey = ay + 2;
    put(ey, gx, gw, CP(C_TITLE) | A_BOLD, "Events (%d)", V.nev);
    V.evsel = CLAMP(V.evsel, 0, V.nev - 1);
    int rows = evh - 1;
    if (V.evsel < V.evscroll)
        V.evscroll = V.evsel;
    if (V.evsel >= V.evscroll + rows)
        V.evscroll = V.evsel - rows + 1;
    V.evscroll = CLAMP(V.evscroll, 0, MAX(0, V.nev - rows));
    for (int i = 0; i < rows && V.evscroll + i < V.nev; i++) {
        int e = V.evscroll + i, yy = ey + 1 + i, sel = e == V.evsel;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, gx, gw, base);
        put(yy, gx, gw, sel ? base : A_NORMAL, " %s", V.ev[e].text);
        hit_add(yy, gx, 1, gw, HIT_FILE, -2 - e, 0); /* an event row: jump to it */
    }
}

/* ============================================================ list view */

void lv_list_draw(sys_t *s, ui_state_t *u)
{
    (void)s;
    int top = 1, H = LINES - 2, W = COLS;
    const char *dir = config_log_dir(u);
    if (strcmp(dir, L.dir) || now_sec() - L.scanned > 5)
        scan(dir);
    char p[PATHBUF], info[96], sz[16];
    config_pretty_path(dir, p, sizeof p);
    fmt_bytes((double)L.total, sz, sizeof sz);
    sfmt(info, sizeof info, "%d file%s, %s", L.n, L.n == 1 ? "" : "s", sz);
    char title[PATHBUF + 16];
    sfmt(title, sizeof title, "Saved logs  %s", p);
    box_(top, 0, H, W, title, L.n ? info : NULL);
    if (!L.n) {
        put(top + 2, 3, W - 6, A_NORMAL, "No logs yet.");
        put(top + 3, 3, W - 6, A_GRAY, "Press Esc, then l to start logging; incident logs from the flight recorder "
                                       "appear here by themselves.");
        return;
    }
    int ih = H - 3, y0 = top + 2;
    u->lv_sel = CLAMP(u->lv_sel, 0, L.n - 1);
    if (u->lv_sel < u->lv_scroll)
        u->lv_scroll = u->lv_sel;
    if (u->lv_sel >= u->lv_scroll + ih)
        u->lv_scroll = u->lv_sel - ih + 1;
    u->lv_scroll = CLAMP(u->lv_scroll, 0, MAX(0, L.n - ih));
    hit_add(y0, 1, ih, W - 2, HIT_SCROLL, 2, 0);
    int ww = 22, kw = 10, sw = 8, dw = W - 4 - ww - kw - sw - 3;
    fill(top + 1, 1, W - 2, CP(C_THEAD));
    put(top + 1, 2, W - 4, CP(C_THEAD), "%-*s %-*s %-*s %*s", ww, "STARTED", kw, "KIND", MAX(dw, 1), "DETAILS", sw,
        "SIZE");
    for (int i = 0; i < ih && u->lv_scroll + i < L.n; i++) {
        int k = u->lv_scroll + i, yy = y0 + i, sel = k == u->lv_sel;
        lfile_t *f = &L.f[k];
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, 1, W - 2, base);
        if (f->kind == LF_INCIDENT && !strchr(f->detail, ':'))
            incident_reason(f);
        char w[40];
        when_text(f->t, w, sizeof w);
        fmt_bytes((double)f->size, sz, sizeof sz);
        int x = 2; /* fixed columns: put() counts display columns, printf widths count bytes */
        put(yy, x, ww, base | A_BOLD, "%s", w);
        x += ww + 1;
        put(yy, x, kw, sel ? base : f->kind == LF_INCIDENT ? CP(C_CRIT) | A_BOLD : A_NORMAL, "%s", LF_NAME[f->kind]);
        x += kw + 1;
        put(yy, x, MAX(dw, 1), sel ? base : A_GRAY, "%s", f->detail[0] ? f->detail : f->name);
        x += MAX(dw, 1) + 1;
        put(yy, x, sw, sel ? base : A_NORMAL, "%*s", sw, sz);
        hit_add(yy, 1, 1, W - 2, HIT_FILE, k, 0);
    }
    if (L.n > ih) {
        char m[48];
        sfmt(m, sizeof m, " %d-%d of %d ", u->lv_scroll + 1, MIN(L.n, u->lv_scroll + ih), L.n);
        put(top + H - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}

int lv_list_key(sys_t *s, ui_state_t *u, int ch)
{
    (void)s;
    int page = MAX(1, LINES - 6);
    switch (ch) {
    case KEY_UP: u->lv_sel--; break;
    case KEY_DOWN: u->lv_sel++; break;
    case KEY_PPAGE: u->lv_sel -= page; break;
    case KEY_NPAGE: u->lv_sel += page; break;
    case KEY_HOME: u->lv_sel = 0; break;
    case KEY_END: u->lv_sel = L.n - 1; break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (u->lv_sel >= 0 && u->lv_sel < L.n)
            lv_open(u, L.f[u->lv_sel].path);
        break;
    case 27:
    case 'v': u->log_mode = 0; break;
    default: return 0;
    }
    u->lv_sel = L.n ? CLAMP(u->lv_sel, 0, L.n - 1) : 0;
    return 1;
}

/* a click in the list: select, a second click on the same row opens it */
void lv_list_click(ui_state_t *u, int row, int dbl)
{
    if (row < 0 || row >= L.n)
        return;
    u->lv_sel = row;
    if (dbl)
        lv_open(u, L.f[row].path);
}

/* ============================================================ viewer keys */

/* keep the window on the log where it can */
static void clamp_window(void)
{
    double span = V.v1 - V.v0, a = V.t[0], b = MAX(V.t[V.n - 1], V.t[0] + 10);
    if (span >= b - a) {
        V.v0 = a;
        V.v1 = a + span;
        return;
    }
    if (V.v0 < a) {
        V.v0 = a;
        V.v1 = a + span;
    } else if (V.v1 > b) {
        V.v1 = b;
        V.v0 = b - span;
    }
}

/* zoom with the cursor in the middle: what came before and after it */
static void zoom(double f)
{
    double full = MAX(V.t[V.n - 1] - V.t[0], 10);
    double span = CLAMP((V.v1 - V.v0) * f, MAX(V.step * 10, 10), full);
    V.v0 = V.cur - span / 2;
    V.v1 = V.v0 + span;
    clamp_window();
}

static void keep_cursor_visible(void)
{
    double span = V.v1 - V.v0;
    if (V.cur < V.v0) {
        V.v0 = V.cur - span * 0.1;
        V.v1 = V.v0 + span;
    } else if (V.cur > V.v1) {
        V.v1 = V.cur + span * 0.1;
        V.v0 = V.v1 - span;
    }
}

static int event_near(double t)
{
    int best = 0;
    for (int i = 1; i < V.nev; i++)
        if (fabs(V.ev[i].t - t) < fabs(V.ev[best].t - t))
            best = i;
    return best;
}

/* ================================================================ SVG */

static void xml(FILE *f, const char *s) /* text for an SVG / XML file */
{
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break;
        default:
            if ((unsigned char)*s >= 32 || *s == '\t')
                fputc(*s, f);
        }
    }
}

/* What is on screen - the selected column and the pinned ones, over the
 * shown time - as an SVG picture next to the log, for a report or a
 * forum post. Spikes survive: every pixel column shows its lowest and
 * highest value, the line its average. */
static int save_svg(char *out, size_t on, char *err, size_t en)
{
    int cols[MAX_PINS + 1], nc = 0;
    cols[nc++] = V.col;
    for (int p = 0; p < V.npin; p++)
        if (V.pin[p] != V.col)
            cols[nc++] = V.pin[p];
    const int W = 1000, LM = 70, R = 20, GW = W - LM - R, GH = 150, GAP = 46, TOP = 64;
    int nev = 0;
    for (int i = 0; i < V.nev; i++)
        nev += V.ev[i].t >= V.v0 && V.ev[i].t <= V.v1;
    int H = TOP + nc * (GH + GAP) + 30 + MIN(nev, 20) * 16 + 10;
    char base[PATHBUF];
    str_copy(base, sizeof base, V.path);
    char *dot = strrchr(base, '.');
    if (dot && !strcmp(dot, ".csv"))
        *dot = '\0';
    char id[48] = "";
    for (const char *q = V.label[V.col]; *q && strlen(id) < sizeof id - 1; q++) {
        size_t l = strlen(id);
        id[l] = isalnum((unsigned char)*q) ? (char)tolower((unsigned char)*q) : '_';
        id[l + 1] = '\0';
    }
    sfmt(out, on, "%s-%s.svg", base, id);
    int who = config_as_user(); /* the logs are the user's: so is the picture */
    if (who < 0) {
        sfmt(err, en, "%s", "can't switch to your user");
        return -1;
    }
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    config_as_root(who);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        sfmt(err, en, "%s", strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    static const char *const COL[4] = { "#1f77b4", "#9467bd", "#d98c00", "#2ca02c" };
    double span = V.v1 - V.v0;
    char a[40], b[40], dur[24];
    time_text(V.v0, span > 3600 ? span : 0, a, sizeof a);
    time_text(V.v1, span > 3600 ? span : 0, b, sizeof b);
    fmt_duration(span, dur, sizeof dur);
    time_t t0 = (time_t)V.v0;
    struct tm tm;
    localtime_r(&t0, &tm);
    char day[32];
    strftime(day, sizeof day, "%a %d %b %Y", &tm);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
               "font-family=\"DejaVu Sans, Arial, sans-serif\" font-size=\"12\">\n", W, H, W, H);
    fprintf(f, "<title>");
    xml(f, V.name);
    fprintf(f, "</title>\n<rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>\n");
    fprintf(f, "<text x=\"%d\" y=\"24\" font-size=\"15\" font-weight=\"bold\">", LM);
    xml(f, V.name);
    fprintf(f, "</text>\n<text x=\"%d\" y=\"44\" fill=\"#555555\">%s, %s to %s (%s) - made by hsm %s</text>\n", LM, day,
            a, b, dur, APP_VERSION);
    int r0 = MAX(0, row_at(V.v0) - 1), r1 = MIN(V.n, row_at(V.v1) + 1);
    for (int g = 0; g < nc; g++) {
        int c = cols[g], y0 = TOP + g * (GH + GAP);
        double lo = INFINITY, hi = -INFINITY, sum = 0;
        int cnt = 0;
        for (int r = r0; r < r1; r++) {
            double v = V.v[(size_t)r * V.ncol + c];
            if (isnan(v) || V.t[r] < V.v0 || V.t[r] > V.v1)
                continue;
            lo = MIN(lo, v);
            hi = MAX(hi, v);
            sum += v;
            cnt++;
        }
        fprintf(f, "<text x=\"%d\" y=\"%d\" font-weight=\"bold\" fill=\"%s\">", LM, y0 + 14, COL[g % 4]);
        xml(f, V.label[c]);
        if (V.unit[c][0]) {
            fprintf(f, " (");
            xml(f, !strcmp(V.unit[c], "C") ? "°C" : V.unit[c]);
            fprintf(f, ")");
        }
        fprintf(f, "</text>\n");
        if (!cnt) {
            fprintf(f, "<text x=\"%d\" y=\"%d\" fill=\"#888888\">no values in this time</text>\n", LM, y0 + 40);
            continue;
        }
        char vlo[24], vhi[24], vav[24];
        sfmt(vlo, sizeof vlo, "%.*f", V.whole[c] || hi - lo >= 100 ? 0 : 1, lo);
        sfmt(vhi, sizeof vhi, "%.*f", V.whole[c] || hi - lo >= 100 ? 0 : 1, hi);
        sfmt(vav, sizeof vav, "%.*f", V.whole[c] ? 1 : hi - lo >= 100 ? 0 : 1, sum / cnt);
        fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" fill=\"#555555\">lowest %s, highest %s, average %s</text>\n",
                W - R, y0 + 14, vlo, vhi, vav);
        double glo = lo, ghi = hi;
        if (ghi - glo < 1e-9)
            glo -= 1, ghi += 1;
        int gy = y0 + 22;
        fprintf(f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"#f6f7f9\" stroke=\"#d0d4da\"/>\n", LM, gy, GW, GH);
        fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" fill=\"#555555\">%s</text>\n", LM - 6, gy + 10, vhi);
        fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" fill=\"#555555\">%s</text>\n", LM - 6, gy + GH, vlo);
        /* the events, as dashed lines */
        for (int i = 0; i < V.nev; i++) {
            if (V.ev[i].t < V.v0 || V.ev[i].t > V.v1)
                continue;
            double ex = LM + (V.ev[i].t - V.v0) / span * GW;
            fprintf(f, "<line x1=\"%.1f\" y1=\"%d\" x2=\"%.1f\" y2=\"%d\" stroke=\"#d62728\" stroke-dasharray=\"3,3\"><title>", ex, gy,
                    ex, gy + GH);
            xml(f, V.ev[i].text);
            fprintf(f, "</title></line>\n");
        }
        /* per pixel column: the band from lowest to highest, and the average */
        fprintf(f, "<path fill=\"none\" stroke=\"%s\" stroke-opacity=\"0.35\" d=\"", COL[g % 4]);
        double *avg = malloc((size_t)GW * sizeof *avg);
        int r = r0;
        for (int px = 0; px < GW && avg; px++) {
            double ta = V.v0 + span * px / GW, tb = V.v0 + span * (px + 1) / GW;
            double mn = INFINITY, mx = -INFINITY, s2 = 0;
            int k = 0;
            while (r < r1 && V.t[r] < ta)
                r++;
            for (int q = r; q < r1 && V.t[q] < tb; q++) {
                double v = V.v[(size_t)q * V.ncol + c];
                if (isnan(v))
                    continue;
                mn = MIN(mn, v);
                mx = MAX(mx, v);
                s2 += v;
                k++;
            }
            avg[px] = k ? s2 / k : NAN;
            if (k > 1 && mx > mn) {
                double ya = gy + GH - (mn - glo) / (ghi - glo) * GH, yb = gy + GH - (mx - glo) / (ghi - glo) * GH;
                fprintf(f, "M%.1f %.1fV%.1f", LM + px + 0.5, ya, yb);
            }
        }
        fprintf(f, "\"/>\n");
        if (avg) {
            fprintf(f, "<path fill=\"none\" stroke=\"%s\" stroke-width=\"1.6\" stroke-linejoin=\"round\" d=\"", COL[g % 4]);
            int pen = 0;
            for (int px = 0; px < GW; px++) {
                if (isnan(avg[px])) { /* a gap in the log: the line breaks */
                    continue;
                }
                double yy = gy + GH - (avg[px] - glo) / (ghi - glo) * GH;
                fprintf(f, "%c%.1f %.1f", pen ? 'L' : 'M', LM + px + 0.5, yy);
                pen = 1;
                if (px + 1 < GW && isnan(avg[px + 1])) { /* a long gap (no rows for a while): lift the pen */
                    int k = px + 1;
                    while (k < GW && isnan(avg[k]))
                        k++;
                    if ((k - px) * span / GW > 5 * V.step)
                        pen = 0;
                }
            }
            fprintf(f, "\"/>\n");
            free(avg);
        }
        /* time under the graph */
        for (int k = 0; k <= 4; k++) {
            double tt = V.v0 + span * k / 4;
            char ts[40];
            time_text(tt, span > 3600 ? span : 0, ts, sizeof ts);
            fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"%s\" fill=\"#555555\">%s</text>\n", LM + GW * k / 4, gy + GH + 16,
                    k == 0 ? "start" : k == 4 ? "end" : "middle", ts);
        }
    }
    /* the events in words */
    int ey = TOP + nc * (GH + GAP) + 14, shown = 0;
    if (nev) {
        fprintf(f, "<text x=\"%d\" y=\"%d\" font-weight=\"bold\">Events</text>\n", LM, ey);
        for (int i = 0; i < V.nev && shown < 20; i++) {
            if (V.ev[i].t < V.v0 || V.ev[i].t > V.v1)
                continue;
            char ts[40];
            const char *tx = V.ev[i].text;
            int hh, mm, ss, k = 0;
            if (sscanf(tx, "%2d:%2d:%2d %n", &hh, &mm, &ss, &k) == 3 && k > 0) { /* the log has the time already */
                sfmt(ts, sizeof ts, "%.8s", tx);
                tx += k;
            } else {
                time_text(V.ev[i].t, 0, ts, sizeof ts);
            }
            fprintf(f, "<text x=\"%d\" y=\"%d\"><tspan fill=\"#d62728\">%s</tspan>  ", LM, ey + 18 + shown * 16, ts);
            xml(f, tx);
            fprintf(f, "</text>\n");
            shown++;
        }
        if (nev > shown)
            fprintf(f, "<text x=\"%d\" y=\"%d\" fill=\"#888888\">and %d more</text>\n", LM, ey + 18 + shown * 16, nev - shown);
    }
    fprintf(f, "</svg>\n");
    if (fclose(f) != 0) {
        sfmt(err, en, "%s", strerror(errno));
        return -1;
    }
    return 0;
}

int lv_key(sys_t *s, ui_state_t *u, int ch)
{
    (void)s;
    if (!V.loaded) {
        if (ch == 27) {
            u->log_mode = 1;
            return 1;
        }
        return 0;
    }
    int gw = MAX(COLS - CLAMP(COLS / 3, 24, 40) - 4, 10);
    double dt = (V.v1 - V.v0) / gw;
    switch (ch) {
    case KEY_UP: V.col = (V.col + V.ncol - 1) % V.ncol; break;
    case KEY_DOWN: V.col = (V.col + 1) % V.ncol; break;
    case KEY_PPAGE: V.col = MAX(0, V.col - 10); break;
    case KEY_NPAGE: V.col = MIN(V.ncol - 1, V.col + 10); break;
    case KEY_LEFT: V.cur -= MAX(dt, V.step); break;
    case KEY_RIGHT: V.cur += MAX(dt, V.step); break;
    case '[':
    case ']': {
        double sh = (V.v1 - V.v0) / 2 * (ch == '[' ? -1 : 1), v0 = V.v0;
        V.v0 += sh;
        V.v1 += sh;
        clamp_window();
        V.cur += V.v0 - v0; /* the cursor moves with the window */
        break;
    }
    case KEY_HOME: V.cur = V.t[0]; break;
    case KEY_END: V.cur = V.t[V.n - 1]; break;
    case '+':
    case '=': zoom(0.5); break;
    case '-':
    case '_': zoom(2.0); break;
    case '0': fit(); break;
    case 'n':
    case 'N': {
        if (!V.nev) {
            ui_message(u, 0, "This log has no events");
            break;
        }
        int e = -1;
        if (ch == 'n') {
            for (int i = 0; i < V.nev && e < 0; i++)
                if (V.ev[i].t > V.cur + 0.5)
                    e = i;
        } else {
            for (int i = V.nev - 1; i >= 0 && e < 0; i--)
                if (V.ev[i].t < V.cur - 0.5)
                    e = i;
        }
        if (e < 0) {
            ui_message(u, 0, ch == 'n' ? "No more events after this point" : "No events before this point");
            break;
        }
        V.evsel = e;
        V.cur = V.ev[e].t;
        break;
    }
    case ' ': {
        for (int p = 0; p < V.npin; p++)
            if (V.pin[p] == V.col) { /* unpin */
                memmove(&V.pin[p], &V.pin[p + 1], (size_t)(V.npin - p - 1) * sizeof V.pin[0]);
                V.npin--;
                return 1;
            }
        if (V.npin == MAX_PINS) {
            ui_message(u, 1, "At most %d graphs are kept on screen: Space on a pinned one removes it", MAX_PINS);
            break;
        }
        V.pin[V.npin++] = V.col;
        ui_message(u, 0, "Kept on screen: %s (Space again removes it)", V.label[V.col]);
        break;
    }
    case 27:
    case 'v':
        u->log_mode = 1;
        return 1;
    case 's': {
        char out[PATHBUF + 64], err[200], pp[PATHBUF];
        if (save_svg(out, sizeof out, err, sizeof err) == 0) {
            config_pretty_path(out, pp, sizeof pp);
            ui_message(u, 0, "Saved the graph%s as %s", V.npin ? "s" : "", pp);
        } else {
            ui_message(u, 1, "Could not save the picture: %s", err);
        }
        return 1;
    }
    default: return 0;
    }
    V.cur = CLAMP(V.cur, V.t[0], V.t[V.n - 1]);
    keep_cursor_visible();
    if (V.nev && ch != 'n' && ch != 'N')
        V.evsel = event_near(V.cur);
    return 1;
}

void lv_click(ui_state_t *u, int kind, int a, int b, int x, int dbl)
{
    (void)u;
    (void)dbl;
    if (!V.loaded)
        return;
    if (kind == HIT_COLUMN) {
        V.col = CLAMP(a, 0, V.ncol - 1);
    } else if (kind == HIT_GRAPH && b > 0) {
        V.cur = CLAMP(V.v0 + (x - a + 0.5) * (V.v1 - V.v0) / b, V.t[0], V.t[V.n - 1]);
        if (V.nev)
            V.evsel = event_near(V.cur);
    } else if (kind == HIT_FILE && a <= -2) { /* an event row */
        int e = -2 - a;
        if (e < V.nev) {
            V.evsel = e;
            V.cur = V.ev[e].t;
            keep_cursor_visible();
        }
    }
}

void lv_wheel(int where, int dir)
{
    if (!V.loaded)
        return;
    if (where == 1) /* the column list */
        V.col = CLAMP(V.col + dir, 0, V.ncol - 1);
    else
        zoom(dir < 0 ? 0.8 : 1.25);
}
