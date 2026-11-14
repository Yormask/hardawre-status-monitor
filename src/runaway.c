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

/* runaway.c - processes that keep a whole core busy for a long time
 *
 * Once a minute, every process that is busy is noted with the CPU time it
 * used in that minute. One that kept at least a whole core busy for 15
 * minutes in a row is called out: a stuck browser tab, a program left
 * spinning after a game closed, an indexer that never finishes. Anything
 * with the GPU open doesn't count, so games and video don't set it off.
 * Names stay as the program gave them. */
#define _GNU_SOURCE
#include "collect.h"

#include <stdlib.h>
#include <string.h>

#define FULL_CORE 0.9  /* cores of CPU time per second that count as "a whole core" */
#define MAX_TRACK 256

typedef struct {
    pid_t pid;
    unsigned long long start, ticks; /* CPU time at the last look */
    double t;                        /* the last look (monotonic) */
    char comm[24];
    int minutes;                     /* minutes in a row at a whole core or more */
    double cores;                    /* cores used, summed over those minutes */
    int flagged;                     /* reported (once per process) */
    int seen;
} busy_t;

static busy_t B[MAX_TRACK];
static int nb;
static double next_t;

/* the test hook: minutes before it counts (HSM_RUNAWAY_MIN, ignored setuid) */
static int after_min(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = secure_getenv("HSM_RUNAWAY_MIN");
        m = e && atoi(e) > 0 ? atoi(e) : RUNAWAY_MIN;
    }
    return m;
}

static busy_t *find(pid_t pid, unsigned long long start)
{
    for (int i = 0; i < nb; i++)
        if (B[i].pid == pid && B[i].start == start)
            return &B[i];
    return NULL;
}

static const proc_t *proc_find(const procs_t *ps, pid_t pid)
{
    int lo = 0, hi = ps->n - 1; /* sorted by pid */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ps->p[mid].pid == pid)
            return &ps->p[mid];
        if (ps->p[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

void runaway_sample(sys_t *s)
{
    double now = now_sec();
    if (now < next_t)
        return;
    next_t = now + 60;
    procs_t *ps = &s->procs;
    long hz = ps->hz > 0 ? ps->hz : 100;
    for (int i = 0; i < nb; i++)
        B[i].seen = 0;

    /* the minute that passed, for the processes already followed */
    for (int i = 0; i < nb; i++) {
        busy_t *b = &B[i];
        const proc_t *p = proc_find(ps, b->pid);
        if (!p || p->start != b->start)
            continue; /* ended: dropped below */
        b->seen = 1;
        double secs = now - b->t;
        double cores = secs > 0 && p->ticks >= b->ticks ? (double)(p->ticks - b->ticks) / (double)hz / secs : 0;
        int gpu = p->gpu > 0 || p->gpu_mem > 0; /* a game, a video: busy on purpose */
        b->ticks = p->ticks;
        b->t = now;
        if (cores >= FULL_CORE && !gpu) {
            b->minutes++;
            b->cores += cores;
        } else {
            b->minutes = 0;
            b->cores = 0;
            if (!b->flagged)
                b->seen = 0; /* not busy now: stop following it */
            continue;
        }
        if (b->minutes >= after_min() && !b->flagged) {
            b->flagged = 1;
            double avg = b->cores / b->minutes;
            char what[32];
            if (avg >= 1.5)
                sfmt(what, sizeof what, "%.0f cores", avg);
            else
                str_copy(what, sizeof what, "a whole core");
            health_event(&s->health, EV_WARN, "runaway", "%s (PID %d) has kept %s busy for %d minute%s without a break",
                         b->comm, (int)b->pid, what, b->minutes, b->minutes == 1 ? "" : "s");
        }
    }
    for (int i = 0; i < nb; i++) /* ended, or no longer busy */
        if (!B[i].seen)
            B[i--] = B[--nb];

    /* processes busy right now start being followed */
    for (int i = 0; i < ps->n && nb < MAX_TRACK; i++) {
        const proc_t *p = &ps->p[i];
        if (p->cpu < FULL_CORE * 100 || p->state == 'Z' || find(p->pid, p->start))
            continue;
        busy_t *b = &B[nb++];
        memset(b, 0, sizeof *b);
        b->pid = p->pid;
        b->start = p->start;
        b->ticks = p->ticks;
        b->t = now;
        b->seen = 1;
        str_copy(b->comm, sizeof b->comm, p->comm);
    }
}

/* minutes in a row it has kept a whole core busy, 0 = not busy that long;
 * *flagged: it was called out */
int runaway_minutes(pid_t pid, unsigned long long start, int *flagged)
{
    const busy_t *b = find(pid, start);
    if (flagged)
        *flagged = b && b->flagged && b->minutes > 0;
    return b ? b->minutes : 0;
}

int runaway_count(void)
{
    int n = 0;
    for (int i = 0; i < nb; i++)
        n += B[i].flagged && B[i].minutes > 0;
    return n;
}
