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
/* log.c - record sensors and processes to CSV files.
 *
 * The system log has one column per selected "channel" (CPU usage, each
 * temperature / fan / power sensor, GPU, memory, disk and network rates...)
 * and one row per interval. Each logged process gets its own file. Rows are
 * flushed straight away and synced to disk every few seconds, so a freeze,
 * crash or power cut loses at most the last few seconds - which is the point
 * when you are hunting for overheating or overload problems. */
#define _GNU_SOURCE
#include "log.h"
#include "ui.h" /* config_as_user, config_mkdir_p */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

logger_t g_log = { .every = 1.0 };

enum {
    CH_CPU_TOTAL, CH_CPU_USER, CH_CPU_SYS, CH_CPU_IOWAIT, CH_CPU_CLOCK, CH_CPU_TEMP, CH_LOAD1,
    CH_CORE, CH_CORE_CLOCK, CH_MEM_PCT, CH_MEM_USED, CH_SWAP_USED,
    CH_GPU_LOAD, CH_GPU_TEMP, CH_GPU_POWER, CH_GPU_CLOCK, CH_GPU_VRAM,
    CH_POWER_TOTAL, CH_RAPL, CH_BAT_POWER, CH_BAT_CHARGE, CH_SENSOR,
    CH_DISK_R, CH_DISK_W, CH_DISK_BUSY, CH_NET_RX, CH_NET_TX
};

#define MB (1024.0 * 1024.0)
#define SYNC_EVERY 5.0 /* seconds between fsync() calls */

/* ------------------------------------------------------------ channels */

static void sanitize_id(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        *s = isalnum(c) || c == '.' || c == '-' ? (char)tolower(c) : '_';
    }
}

static chan_t *add(const char *group, int kind, int a, const char *name, const char *unit, const char *label,
                   const char *idfmt, ...) __attribute__((format(printf, 7, 8)));
static chan_t *add(const char *group, int kind, int a, const char *name, const char *unit, const char *label,
                   const char *idfmt, ...)
{
    if (g_log.nchan >= MAX_CHAN)
        return NULL;
    chan_t *c = &g_log.ch[g_log.nchan++];
    memset(c, 0, sizeof *c);
    va_list ap;
    va_start(ap, idfmt);
    vsnprintf(c->id, sizeof c->id, idfmt, ap);
    va_end(ap);
    sanitize_id(c->id);
    str_copy(c->group, sizeof c->group, group);
    str_copy(c->label, sizeof c->label, label);
    str_copy(c->unit, sizeof c->unit, unit);
    str_copy(c->name, sizeof c->name, name);
    for (char *q = c->label; *q; q++)
        if (*q == '"')
            *q = '\'';
    c->kind = kind;
    c->a = a;
    c->on = 1;
    return c;
}

static int in_list(const char *list, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        size_t l = strcspn(p, ",");
        size_t t = l;
        while (t && p[t - 1] == ' ')
            t--;
        if (t == n && !strncmp(p, id, n))
            return 1;
        p += l;
    }
    return 0;
}

void log_build_channels(sys_t *s, const char *skip)
{
    char lab[48];
    g_log.nchan = 0;

    add("CPU", CH_CPU_TOTAL, 0, "", "%", "CPU total", "cpu.total");
    add("CPU", CH_CPU_USER, 0, "", "%", "CPU user", "cpu.user");
    add("CPU", CH_CPU_SYS, 0, "", "%", "CPU system", "cpu.system");
    add("CPU", CH_CPU_IOWAIT, 0, "", "%", "CPU iowait", "cpu.iowait");
    if (s->cpu.mhz_avg > 0)
        add("CPU", CH_CPU_CLOCK, 0, "", "MHz", "Average CPU clock", "cpu.clock");
    if (s->sens.cpu_idx >= 0)
        add("CPU", CH_CPU_TEMP, 0, "", "C", "CPU temperature", "cpu.temp");
    add("CPU", CH_LOAD1, 0, "", "", "Load average (1 min)", "cpu.load1");
    for (int i = 0; i < s->cpu.ncpu; i++) {
        sfmt(lab, sizeof lab, "Core %d usage", s->cpu.id[i]);
        add("CPU cores", CH_CORE, s->cpu.id[i], "", "%", lab, "cpu.core%d", s->cpu.id[i]);
    }
    for (int i = 0; i < s->cpu.ncpu; i++)
        if (s->cpu.mhz[i] > 0) {
            sfmt(lab, sizeof lab, "Core %d clock", s->cpu.id[i]);
            add("CPU cores", CH_CORE_CLOCK, s->cpu.id[i], "", "MHz", lab, "cpu.core%d.clock", s->cpu.id[i]);
        }

    add("Memory", CH_MEM_PCT, 0, "", "%", "Memory used", "mem.used_pct");
    add("Memory", CH_MEM_USED, 0, "", "MB", "Memory used", "mem.used");
    if (s->mem.swap_total)
        add("Memory", CH_SWAP_USED, 0, "", "MB", "Swap used", "mem.swap");

    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        char gn[8];
        sfmt(gn, sizeof gn, s->gpus.n > 1 ? "GPU%d" : "GPU", i);
        if (g->util >= 0 || g->neng || !strcmp(g->driver, "i915")) {
            sfmt(lab, sizeof lab, "%s load", gn);
            add("GPU", CH_GPU_LOAD, i, "", "%", lab, "gpu%d.load", i);
        }
        if (g->temp_c >= 0) {
            sfmt(lab, sizeof lab, "%s temperature", gn);
            add("GPU", CH_GPU_TEMP, i, "", "C", lab, "gpu%d.temp", i);
        }
        if (g->power_w >= 0) {
            sfmt(lab, sizeof lab, "%s power", gn);
            add("GPU", CH_GPU_POWER, i, "", "W", lab, "gpu%d.power", i);
        }
        if (g->clk_mhz >= 0) {
            sfmt(lab, sizeof lab, "%s clock", gn);
            add("GPU", CH_GPU_CLOCK, i, "", "MHz", lab, "gpu%d.clock", i);
        }
        if (g->mem_total > 0) {
            sfmt(lab, sizeof lab, "%s VRAM used", gn);
            add("GPU", CH_GPU_VRAM, i, "", "MB", lab, "gpu%d.vram", i);
        }
    }

    if (s->total_w >= 0)
        add("Power", CH_POWER_TOTAL, 0, "", "W", "Estimated total power", "power.total");
    for (int i = 0; i < s->power.nrapl; i++)
        if (s->power.z[i].readable) {
            sfmt(lab, sizeof lab, "RAPL %s", s->power.z[i].name);
            add("Power", CH_RAPL, i, "", "W", lab, "power.rapl.%s.%d", s->power.z[i].name, i);
        }
    for (int i = 0; i < s->power.nbat; i++) {
        sfmt(lab, sizeof lab, "%s drain", s->power.bat[i].name);
        add("Power", CH_BAT_POWER, i, "", "W", lab, "power.%s.drain", s->power.bat[i].name);
        sfmt(lab, sizeof lab, "%s charge", s->power.bat[i].name);
        add("Power", CH_BAT_CHARGE, i, "", "%", lab, "power.%s.charge", s->power.bat[i].name);
    }

    static const char *const unit[] = { "C", "RPM", "W" };
    for (int i = 0; i < s->sens.n; i++) {
        sensor_t *t = &s->sens.s[i];
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        sfmt(lab, sizeof lab, "%s %s", chip, t->label); /* "CPU Core 0", "NVMe Composite" */
        add(t->type == SENS_FAN ? "Fans" : t->type == SENS_POWER ? "Power" : "Temperatures", CH_SENSOR, i, "",
            unit[t->type], lab, "sensor.%s.%s", t->chip, t->label);
    }

    for (int i = 0; i < s->disks.n; i++) {
        disk_t *d = &s->disks.d[i];
        sfmt(lab, sizeof lab, "%s read", d->name);
        add("Disks", CH_DISK_R, 0, d->name, "MB/s", lab, "disk.%s.read", d->name);
        sfmt(lab, sizeof lab, "%s write", d->name);
        add("Disks", CH_DISK_W, 0, d->name, "MB/s", lab, "disk.%s.write", d->name);
        sfmt(lab, sizeof lab, "%s busy", d->name);
        add("Disks", CH_DISK_BUSY, 0, d->name, "%", lab, "disk.%s.busy", d->name);
    }

    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        if (f->is_virtual && !f->up)
            continue;
        if (!strncmp(f->name, "veth", 4))
            continue;
        sfmt(lab, sizeof lab, "%s download", f->name);
        add("Network", CH_NET_RX, 0, f->name, "MB/s", lab, "net.%s.rx", f->name);
        sfmt(lab, sizeof lab, "%s upload", f->name);
        add("Network", CH_NET_TX, 0, f->name, "MB/s", lab, "net.%s.tx", f->name);
    }

    for (int i = 0; i < g_log.nchan; i++)
        g_log.ch[i].on = !(skip && in_list(skip, g_log.ch[i].id));
}

void log_skip_string(char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0, k = 0; i < g_log.nchan; i++)
        if (!g_log.ch[i].on) {
            size_t l = strlen(out);
            if (l + strlen(g_log.ch[i].id) + 3 >= n)
                break;
            sfmt(out + l, n - l, "%s%s", k++ ? ", " : "", g_log.ch[i].id);
        }
}

int log_count_on(void)
{
    int k = 0;
    for (int i = 0; i < g_log.nchan; i++)
        k += g_log.ch[i].on;
    return k;
}

static int cpu_index(const sys_t *s, int id)
{
    for (int i = 0; i < s->cpu.ncpu; i++)
        if (s->cpu.id[i] == id)
            return i;
    return -1;
}

static double nn(double v) /* negative = unknown */
{
    return v < 0 ? NAN : v;
}

double log_chan_value(const sys_t *s, const chan_t *c)
{
    int i;
    switch (c->kind) {
    case CH_CPU_TOTAL: return s->cpu.total;
    case CH_CPU_USER: return s->cpu.user;
    case CH_CPU_SYS: return s->cpu.sys;
    case CH_CPU_IOWAIT: return s->cpu.iowait;
    case CH_CPU_CLOCK: return s->cpu.mhz_avg > 0 ? s->cpu.mhz_avg : NAN;
    case CH_CPU_TEMP: return nn(s->cpu.temp_c);
    case CH_LOAD1: return s->cpu.load[0];
    case CH_CORE: return (i = cpu_index(s, c->a)) >= 0 ? s->cpu.pct[i] : NAN;
    case CH_CORE_CLOCK: return (i = cpu_index(s, c->a)) >= 0 && s->cpu.mhz[i] > 0 ? s->cpu.mhz[i] : NAN;
    case CH_MEM_PCT: return s->mem.total ? 100.0 * (double)s->mem.used / (double)s->mem.total : NAN;
    case CH_MEM_USED: return (double)s->mem.used / MB;
    case CH_SWAP_USED: return (double)s->mem.swap_used / MB;
    case CH_GPU_LOAD: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].util) : NAN;
    case CH_GPU_TEMP: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].temp_c) : NAN;
    case CH_GPU_POWER: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].power_w) : NAN;
    case CH_GPU_CLOCK: return c->a < s->gpus.n ? nn(s->gpus.g[c->a].clk_mhz) : NAN;
    case CH_GPU_VRAM: return c->a < s->gpus.n && s->gpus.g[c->a].mem_used >= 0 ? s->gpus.g[c->a].mem_used / MB : NAN;
    case CH_POWER_TOTAL: return nn(s->total_w);
    case CH_RAPL: return c->a < s->power.nrapl ? nn(s->power.z[c->a].watts) : NAN;
    case CH_BAT_POWER: return c->a < s->power.nbat ? nn(s->power.bat[c->a].watts) : NAN;
    case CH_BAT_CHARGE: return c->a < s->power.nbat ? nn(s->power.bat[c->a].capacity) : NAN;
    case CH_SENSOR: return c->a < s->sens.n && s->sens.s[c->a].ok ? s->sens.s[c->a].value : NAN;
    case CH_DISK_R:
    case CH_DISK_W:
    case CH_DISK_BUSY:
        for (i = 0; i < s->disks.n; i++)
            if (!strcmp(s->disks.d[i].name, c->name))
                return c->kind == CH_DISK_R ? s->disks.d[i].rd_bps / MB
                     : c->kind == CH_DISK_W ? s->disks.d[i].wr_bps / MB : s->disks.d[i].util;
        return NAN;
    case CH_NET_RX:
    case CH_NET_TX:
        for (i = 0; i < s->net.n; i++)
            if (!strcmp(s->net.i[i].name, c->name))
                return (c->kind == CH_NET_RX ? s->net.i[i].rx_bps : s->net.i[i].tx_bps) / MB;
        return NAN;
    }
    return NAN;
}

static int decimals(const char *unit)
{
    if (!strcmp(unit, "MHz") || !strcmp(unit, "RPM"))
        return 0;
    if (!strcmp(unit, "MB/s"))
        return 3;
    if (!strcmp(unit, "W") || !*unit)
        return 2;
    return 1;
}

void log_fmt_value(const chan_t *c, double v, char *out, size_t n)
{
    if (isnan(v)) {
        str_copy(out, n, "-");
        return;
    }
    if (!strcmp(c->unit, "C"))
        sfmt(out, n, "%.1f °C", v);
    else if (!strcmp(c->unit, "%"))
        sfmt(out, n, "%.1f %%", v);
    else if (*c->unit)
        sfmt(out, n, "%.*f %s", decimals(c->unit), v, c->unit);
    else
        sfmt(out, n, "%.2f", v);
}

/* --------------------------------------------------------------- files */

static void stamp(char *out, size_t n, const char *fmt)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, fmt, &tm);
}

/* Create a new file (never overwrite one) as the real user, also when
 * hsm runs under sudo, so the logs belong to whoever ran it. */
static FILE *open_new(const char *base, char *path, size_t pn, char *err, size_t en)
{
    int who = config_as_user();
    if (who < 0) {
        sfmt(err, en, "Could not switch to your user to write the log");
        return NULL;
    }
    FILE *f = NULL;
    if (config_mkdir_p(g_log.dir) != 0) {
        sfmt(err, en, "Could not create %s: %s", g_log.dir, strerror(errno));
    } else {
        for (int k = 1; k < 100 && !f; k++) {
            if (k == 1)
                sfmt(path, pn, "%s/%s.csv", g_log.dir, base);
            else
                sfmt(path, pn, "%s/%s-%d.csv", g_log.dir, base, k);
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd < 0) {
                if (errno == EEXIST)
                    continue;
                sfmt(err, en, "Could not create %s: %s", path, strerror(errno));
                break;
            }
            f = fdopen(fd, "w");
            if (!f)
                close(fd);
        }
    }
    config_as_root(who);
    return f;
}

long log_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void put_num(FILE *f, double v, int dec)
{
    if (isnan(v))
        fputc(',', f);
    else
        fprintf(f, ",%.*f", dec, v);
}

/* ------------------------------------------------------------ system log */

static void note(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log.note, sizeof g_log.note, fmt, ap);
    va_end(ap);
    g_log.note_err = err;
    g_log.note_new = 1;
}

static void sys_row(sys_t *s, double now)
{
    char ts[32];
    stamp(ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    fprintf(g_log.f, "%s,%.1f", ts, now - g_log.t0);
    for (int i = 0; i < g_log.nchan; i++)
        if (g_log.ch[i].on)
            put_num(g_log.f, log_chan_value(s, &g_log.ch[i]), decimals(g_log.ch[i].unit));
    fputc('\n', g_log.f);
    fflush(g_log.f);
    g_log.rows++;
    g_log.last_row = now;
    if (ferror(g_log.f)) {
        note(1, "Logging stopped: could not write %s (disk full?)", g_log.path);
        log_stop();
    }
}

int log_start(sys_t *s, char *err, size_t n)
{
    if (g_log.f)
        return 0;
    if (!log_count_on()) {
        sfmt(err, n, "Nothing selected to log: pick values in the Logging view (5)");
        return -1;
    }
    char ts[32], base[64];
    stamp(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S");
    sfmt(base, sizeof base, "hsm-system-%s", ts);
    g_log.f = open_new(base, g_log.path, sizeof g_log.path, err, n);
    if (!g_log.f)
        return -1;
    fprintf(g_log.f, "time,elapsed_s");
    for (int i = 0; i < g_log.nchan; i++) {
        chan_t *c = &g_log.ch[i];
        if (!c->on)
            continue;
        if (*c->unit)
            fprintf(g_log.f, ",\"%s (%s)\"", c->label, c->unit);
        else
            fprintf(g_log.f, ",\"%s\"", c->label);
    }
    fputc('\n', g_log.f);
    g_log.rows = 0;
    g_log.t0 = g_log.last_sync = now_sec();
    sys_row(s, g_log.t0); /* first row right away */
    return g_log.f ? 0 : -1;
}

void log_stop(void)
{
    if (!g_log.f)
        return;
    fflush(g_log.f);
    fsync(fileno(g_log.f));
    fclose(g_log.f);
    g_log.f = NULL;
}

/* ---------------------------------------------------------- process logs */

static proc_t *find_proc(sys_t *s, pid_t pid, unsigned long long start)
{
    int lo = 0, hi = s->procs.n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        proc_t *p = &s->procs.p[mid];
        if (p->pid == pid)
            return p->start == start ? p : NULL;
        if (p->pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

int log_proc_find(pid_t pid, unsigned long long start)
{
    for (int i = 0; i < g_log.nplog; i++)
        if (g_log.pl[i].pid == pid && g_log.pl[i].start == start)
            return i;
    return -1;
}

static void proc_row(sys_t *s, plog_t *pl, proc_t *p, double now)
{
    char ts[32];
    stamp(ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    fprintf(pl->f, "%s,%.1f,%d,%c", ts, now - pl->t0, (int)p->pid, p->state);
    put_num(pl->f, p->cpu, 1);
    put_num(pl->f, p->mem, 2);
    put_num(pl->f, (double)p->rss / MB, 1);
    fprintf(pl->f, ",%d", p->nthreads);
    put_num(pl->f, p->io_ok ? p->rd_bps / MB : NAN, 3);
    put_num(pl->f, p->io_ok ? p->wr_bps / MB : NAN, 3);
    put_num(pl->f, s->cpu.total, 1);
    put_num(pl->f, nn(s->cpu.temp_c), 1);
    fputc('\n', pl->f);
    fflush(pl->f);
    pl->rows++;
    pl->last_row = now;
}

int log_proc_start(sys_t *s, pid_t pid, unsigned long long start, char *err, size_t n)
{
    if (log_proc_find(pid, start) >= 0)
        return 0;
    if (g_log.nplog >= MAX_PLOG) {
        sfmt(err, n, "At most %d processes can be logged at the same time", MAX_PLOG);
        return -1;
    }
    proc_t *p = find_proc(s, pid, start);
    if (!p) {
        sfmt(err, n, "PID %d has already exited", (int)pid);
        return -1;
    }
    plog_t *pl = &g_log.pl[g_log.nplog];
    memset(pl, 0, sizeof *pl);
    pl->pid = pid;
    pl->start = start;
    str_copy(pl->comm, sizeof pl->comm, p->comm);
    char name[24], ts[32], base[96];
    str_copy(name, sizeof name, p->comm);
    for (char *q = name; *q; q++)
        if (!isalnum((unsigned char)*q) && *q != '-' && *q != '.')
            *q = '_';
    stamp(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S");
    sfmt(base, sizeof base, "hsm-process-%d-%s-%s", (int)pid, name, ts);
    pl->f = open_new(base, pl->path, sizeof pl->path, err, n);
    if (!pl->f)
        return -1;
    fprintf(pl->f, "time,elapsed_s,pid,state,\"CPU (%% of one core)\",\"Memory (%%)\",\"RSS (MB)\",threads,"
                   "\"Disk read (MB/s)\",\"Disk write (MB/s)\",\"System CPU (%%)\",\"CPU temperature (C)\"\n");
    pl->t0 = now_sec();
    g_log.nplog++;
    proc_row(s, pl, p, pl->t0);
    return 0;
}

static void plog_close(int i)
{
    plog_t *pl = &g_log.pl[i];
    if (pl->f) {
        fflush(pl->f);
        fsync(fileno(pl->f));
        fclose(pl->f);
    }
    memmove(&g_log.pl[i], &g_log.pl[i + 1], (size_t)(g_log.nplog - i - 1) * sizeof *pl);
    g_log.nplog--;
}

void log_proc_stop(int i)
{
    if (i >= 0 && i < g_log.nplog)
        plog_close(i);
}

/* ------------------------------------------------------------------ tick */

void log_tick(sys_t *s)
{
    double now = now_sec(), ev = g_log.every;
    if (g_log.f && (ev <= 0 || now - g_log.last_row >= ev - 0.05))
        sys_row(s, now);
    for (int i = 0; i < g_log.nplog;) {
        plog_t *pl = &g_log.pl[i];
        proc_t *p = find_proc(s, pl->pid, pl->start);
        if (!p || p->state == 'Z' || p->state == 'X') { /* gone, or a zombie waiting to be reaped */
            note(0, "PID %d (%s) has exited: its log was closed after %lu rows", (int)pl->pid, pl->comm, pl->rows);
            plog_close(i);
            continue;
        }
        if (ev <= 0 || now - pl->last_row >= ev - 0.05)
            proc_row(s, pl, p, now);
        if (ferror(pl->f)) {
            note(1, "Stopped logging PID %d: could not write %s", (int)pl->pid, pl->path);
            plog_close(i);
            continue;
        }
        i++;
    }
    if (now - g_log.last_sync >= SYNC_EVERY) { /* push the data to the disk itself */
        if (g_log.f)
            fsync(fileno(g_log.f));
        for (int i = 0; i < g_log.nplog; i++)
            fsync(fileno(g_log.pl[i].f));
        g_log.last_sync = now;
    }
}

void log_close_all(void)
{
    log_stop();
    while (g_log.nplog)
        plog_close(g_log.nplog - 1);
}
