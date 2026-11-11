/* cpu.c - CPU usage (/proc/stat), clocks (cpufreq), load and topology */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_buf;
static size_t g_cap;

static void strip_all(char *s, const char *junk)
{
    size_t jl = strlen(junk);
    char *p;
    while ((p = strstr(s, junk)) != NULL)
        memmove(p, p + jl, strlen(p + jl) + 1);
}

/* "Intel(R) Core(TM) i5-8500 CPU @ 3.00GHz" -> "Intel Core i5-8500 @ 3.00GHz" */
static void clean_model(char *s)
{
    static const char *const junk[] = { "(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor", NULL };
    for (int i = 0; junk[i]; i++)
        strip_all(s, junk[i]);
    char *r = s, *w = s;
    int sp = 1;
    for (; *r; r++) {
        if (isspace((unsigned char)*r)) {
            if (!sp)
                *w++ = ' ';
            sp = 1;
        } else {
            *w++ = *r;
            sp = 0;
        }
    }
    while (w > s && w[-1] == ' ')
        w--;
    *w = '\0';
}

static void add_pair(int pairs[][2], int *n, int a, int b)
{
    for (int i = 0; i < *n; i++)
        if (pairs[i][0] == a && pairs[i][1] == b)
            return;
    if (*n < MAX_CPUS) {
        pairs[*n][0] = a;
        pairs[*n][1] = b;
        (*n)++;
    }
}

void cpu_init(cpu_t *c)
{
    char path[PATHBUF];
    c->temp_c = -1;
    str_copy(c->model, sizeof c->model, "Unknown CPU");

    sfmt(path, sizeof path, "%s/cpuinfo", g_procfs);
    char *buf = NULL;
    size_t cap = 0;
    int threads = 0, npairs = 0;
    static int pairs[MAX_CPUS][2];
    if (read_file_grow(path, &buf, &cap) > 0) {
        int prio = 0, phys = 0, core = -1;
        char *cur = buf, *line;
        while ((line = next_line(&cur)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon) { /* blank line ends one processor block */
                if (core >= 0)
                    add_pair(pairs, &npairs, phys, core);
                phys = 0;
                core = -1;
                continue;
            }
            char *ke = colon;
            while (ke > line && isspace((unsigned char)ke[-1]))
                ke--;
            *ke = '\0';
            char *val = colon + 1;
            while (isspace((unsigned char)*val))
                val++;

            if (!strcmp(line, "processor"))
                threads++;
            else if (!strcmp(line, "physical id"))
                phys = atoi(val);
            else if (!strcmp(line, "core id"))
                core = atoi(val);
            else if (!strcmp(line, "model name") && prio < 3) {
                str_copy(c->model, sizeof c->model, val);
                prio = 3;
            } else if ((!strcmp(line, "Model") || !strcmp(line, "Hardware") || !strcmp(line, "cpu model")) &&
                       prio < 2 && *val) {
                str_copy(c->model, sizeof c->model, val);
                prio = 2;
            }
        }
        if (core >= 0)
            add_pair(pairs, &npairs, phys, core);
    }
    free(buf);
    clean_model(c->model);
    c->threads = threads;
    c->cores = npairs ? npairs : threads;
}

static double jtot(const cpu_jiffies_t *j)
{
    return j->user + j->nice + j->sys + j->idle + j->iowait + j->irq + j->softirq + j->steal;
}

static double busy_pct(const cpu_jiffies_t *a, const cpu_jiffies_t *b)
{
    double d = jtot(b) - jtot(a);
    if (d <= 0)
        return 0;
    double di = (b->idle + b->iowait) - (a->idle + a->iowait);
    return CLAMP(100.0 * (d - di) / d, 0.0, 100.0);
}

static double ull_rate(unsigned long long prev, unsigned long long cur, double dt)
{
    return (dt > 0 && cur >= prev) ? (double)(cur - prev) / dt : 0;
}

/* Fallback when cpufreq is unavailable (VMs, some ARM boards). */
static int freqs_from_cpuinfo(cpu_t *c)
{
    static char *buf;
    static size_t cap;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/cpuinfo", g_procfs);
    if (read_file_grow(path, &buf, &cap) <= 0)
        return 0;
    int cur_id = -1, found = 0;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (!strncmp(line, "processor", 9)) {
            char *col = strchr(line, ':');
            cur_id = col ? atoi(col + 1) : -1;
        } else if (!strncmp(line, "cpu MHz", 7)) {
            char *col = strchr(line, ':');
            if (!col)
                continue;
            double mhz = atof(col + 1);
            for (int i = 0; i < c->ncpu; i++)
                if (c->id[i] == cur_id) {
                    c->mhz[i] = mhz;
                    found = 1;
                }
        }
    }
    return found;
}

void cpu_sample(cpu_t *c, double dt)
{
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/stat", g_procfs);
    if (read_file_grow(path, &g_buf, &g_cap) <= 0)
        return;

    int n = 0;
    unsigned long long ctxt = 0, intr = 0, forks = 0;
    char *cur = g_buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u') {
            char *p = line + 3;
            int id = -1;
            if (isdigit((unsigned char)*p))
                id = (int)strtol(p, &p, 10);
            unsigned long long v[8] = { 0 };
            if (sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                       &v[6], &v[7]) < 4)
                continue;
            cpu_jiffies_t j = { (double)v[0], (double)v[1], (double)v[2], (double)v[3],
                                (double)v[4], (double)v[5], (double)v[6], (double)v[7] };
            if (id < 0) {
                const cpu_jiffies_t *a = &c->prev_all;
                double d = jtot(&j) - jtot(a);
                if (c->primed && d > 0) {
                    c->total = busy_pct(a, &j);
                    c->user = CLAMP(100.0 * ((j.user + j.nice) - (a->user + a->nice)) / d, 0.0, 100.0);
                    c->sys = CLAMP(100.0 * ((j.sys + j.irq + j.softirq) - (a->sys + a->irq + a->softirq)) / d, 0.0,
                                   100.0);
                    c->iowait = CLAMP(100.0 * (j.iowait - a->iowait) / d, 0.0, 100.0);
                    c->steal = CLAMP(100.0 * (j.steal - a->steal) / d, 0.0, 100.0);
                }
                c->prev_all = j;
            } else if (n < MAX_CPUS && id >= 0 && id < MAX_CPUS) {
                c->id[n] = id;
                c->pct[n] = c->primed ? busy_pct(&c->prev[id], &j) : 0;
                c->prev[id] = j;
                n++;
            }
        } else if (!strncmp(line, "ctxt ", 5)) {
            ctxt = strtoull(line + 5, NULL, 10);
        } else if (!strncmp(line, "intr ", 5)) {
            intr = strtoull(line + 5, NULL, 10);
        } else if (!strncmp(line, "processes ", 10)) {
            forks = strtoull(line + 10, NULL, 10);
        } else if (!strncmp(line, "procs_running ", 14)) {
            c->procs_running = (unsigned)strtoul(line + 14, NULL, 10);
        } else if (!strncmp(line, "procs_blocked ", 14)) {
            c->procs_blocked = (unsigned)strtoul(line + 14, NULL, 10);
        }
    }
    c->ncpu = n;
    if (c->primed) {
        c->ctxt_rate = ull_rate(c->prev_ctxt, ctxt, dt);
        c->intr_rate = ull_rate(c->prev_intr, intr, dt);
        c->fork_rate = ull_rate(c->prev_forks, forks, dt);
    }
    c->prev_ctxt = ctxt;
    c->prev_intr = intr;
    c->prev_forks = forks;

    /* clocks: cpufreq in sysfs (kHz), else "cpu MHz" in /proc/cpuinfo */
    if (c->freq_src == 0 || c->freq_src == 1) {
        int got = 0;
        for (int i = 0; i < n; i++) {
            long long khz;
            sfmt(path, sizeof path, "%s/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", g_sysfs, c->id[i]);
            if (read_ll(path, &khz) == 0) {
                c->mhz[i] = (double)khz / 1000.0;
                got = 1;
            } else {
                c->mhz[i] = 0;
            }
        }
        if (got)
            c->freq_src = 1;
        else if (c->freq_src == 0)
            c->freq_src = 2;
    }
    if (c->freq_src == 2 && !freqs_from_cpuinfo(c))
        c->freq_src = -1;

    double sum = 0, mx = 0;
    int cnt = 0;
    for (int i = 0; i < n; i++)
        if (c->mhz[i] > 0) {
            sum += c->mhz[i];
            cnt++;
            if (c->mhz[i] > mx)
                mx = c->mhz[i];
        }
    c->mhz_avg = cnt ? sum / cnt : 0;
    c->mhz_max = mx;

    char b[256];
    sfmt(path, sizeof path, "%s/loadavg", g_procfs);
    if (read_file(path, b, sizeof b) > 0)
        sscanf(b, "%lf %lf %lf", &c->load[0], &c->load[1], &c->load[2]);
    sfmt(path, sizeof path, "%s/uptime", g_procfs);
    if (read_file(path, b, sizeof b) > 0)
        sscanf(b, "%lf", &c->uptime);

    c->primed = 1;
}
