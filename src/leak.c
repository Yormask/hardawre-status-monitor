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
/* leak.c - finds processes whose memory keeps growing.
 *
 * Once a minute the resident memory of every process using more than
 * 64 MB is noted, for the last two hours. A process is called out when,
 * over at least half an hour, it grew by 256 MB and by a third, and hardly
 * ever gave memory back: a program that only ever grows is leaking, or
 * caching without limit. Normal programs go up and down. */
#define _GNU_SOURCE
#include "collect.h"

#include <stdlib.h>
#include <string.h>

#define MIN_RSS_MB 64.0   /* smaller processes aren't followed */
#define MIN_GROWTH_MB 256 /* growth that counts */
#define MIN_MINUTES 30    /* history needed before judging */
#define MAX_TRACK 1024

typedef struct {
    pid_t pid;
    unsigned long long start;
    char comm[24];
    float mb[LEAK_MIN];
    int n, head; /* ring: n values, the next one goes at head */
    int growing;
    double flagged_mb; /* size when it was last reported */
    int seen;
} track_t;

static track_t *T;
static int nt, cap;
static double next_t;

static track_t *find(pid_t pid, unsigned long long start)
{
    for (int i = 0; i < nt; i++)
        if (T[i].pid == pid && T[i].start == start)
            return &T[i];
    return NULL;
}

static float at(const track_t *t, int k) /* k = 0 oldest */
{
    return t->mb[(t->head - t->n + k + LEAK_MIN) % LEAK_MIN];
}

/* growing steadily over the history? */
static int judge(const track_t *t, double *grew, int *minutes)
{
    if (t->n < MIN_MINUTES)
        return 0;
    float first = at(t, 0), last = at(t, t->n - 1);
    int downs = 0;
    for (int k = 1; k < t->n; k++)
        if (at(t, k) < at(t, k - 1) * 0.98f) /* gave back more than 2% */
            downs++;
    *grew = last - first;
    *minutes = t->n;
    return *grew >= MIN_GROWTH_MB && *grew >= first / 3.0 && downs <= t->n / 10;
}

void leak_sample(sys_t *s)
{
    double now = now_sec();
    if (now < next_t)
        return;
    next_t = now + 60;
    for (int i = 0; i < nt; i++)
        T[i].seen = 0;
    procs_t *ps = &s->procs;
    for (int i = 0; i < ps->n; i++) {
        proc_t *p = &ps->p[i];
        double mb = (double)p->rss / (1024.0 * 1024.0);
        if (p->kthread)
            continue;
        track_t *t = find(p->pid, p->start);
        if (!t) {
            if (mb < MIN_RSS_MB || nt >= MAX_TRACK)
                continue;
            if (nt == cap) {
                int nc = cap ? cap * 2 : 64;
                track_t *n2 = realloc(T, (size_t)nc * sizeof *n2);
                if (!n2)
                    continue;
                T = n2;
                cap = nc;
            }
            t = &T[nt++];
            memset(t, 0, sizeof *t);
            t->pid = p->pid;
            t->start = p->start;
            str_copy(t->comm, sizeof t->comm, p->comm);
        }
        t->seen = 1;
        t->mb[t->head] = (float)mb;
        t->head = (t->head + 1) % LEAK_MIN;
        if (t->n < LEAK_MIN)
            t->n++;
        double grew;
        int mins;
        if (judge(t, &grew, &mins)) {
            /* report once, again when it grew by half as much again */
            if (!t->growing || mb >= t->flagged_mb * 1.5) {
                char g[16], now_s[16], d[24];
                fmt_bytes(grew * 1024 * 1024, g, sizeof g);
                fmt_bytes(mb * 1024 * 1024, now_s, sizeof now_s);
                fmt_duration(mins * 60.0, d, sizeof d);
                health_event(&s->health, EV_WARN, "leak", "%s (PID %d) keeps growing: +%s in %s, now %s", t->comm,
                             (int)t->pid, g, d, now_s);
                t->flagged_mb = mb;
            }
            t->growing = 1;
        } else if (t->growing && t->n >= MIN_MINUTES && at(t, t->n - 1) < t->flagged_mb * 0.8) {
            t->growing = 0; /* it gave a good part back after all */
        }
    }
    for (int i = 0; i < nt; i++) /* processes that ended */
        if (!T[i].seen)
            T[i--] = T[--nt];
}

int leak_history(pid_t pid, unsigned long long start, float *out, int max, int *growing, double *rate)
{
    const track_t *t = find(pid, start);
    *growing = 0;
    *rate = 0;
    if (!t)
        return 0;
    int n = MIN(t->n, max);
    for (int k = 0; k < n; k++)
        out[k] = at(t, t->n - n + k);
    *growing = t->growing;
    int w = MIN(t->n, 30); /* MB per hour over the last half hour */
    if (w >= 2)
        *rate = (at(t, t->n - 1) - at(t, t->n - w)) * 60.0 / (w - 1);
    return n;
}

int leak_growing(pid_t pid, unsigned long long start)
{
    const track_t *t = find(pid, start);
    return t && t->growing;
}
