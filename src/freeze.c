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
/* freeze.c - notices when the whole system stops for a while.
 *
 * A small thread wakes up ten times a second and checks how long it was
 * really away. If the kernel couldn't run it for seconds although it wasn't
 * suspended, nothing else could run either: the system froze (memory
 * thrashing, a driver or the kernel hogging the CPU, a stuck virtual
 * machine host...). Time spent suspended is told apart with the boot-time
 * clock, which keeps counting during suspend while the monotonic one
 * stops. The thread does no I/O, so a slow disk alone doesn't fool it. */
#define _GNU_SOURCE
#include "collect.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICK 0.1      /* seconds between wake-ups */
#define STALL_MIN 2.0 /* away this much longer than asked: a freeze */
#define SLEEP_MIN 0.5 /* lost this much more on the boot-time clock: suspended */
#define QLEN 32

static fz_t g_q[QLEN];
static atomic_uint g_head, g_tail;    /* one writer (the thread), one reader (the main loop) */
static atomic_llong g_cont_ns;        /* when hsm itself got SIGCONT (it was stopped, not the system) */
static pthread_t g_thr;
static int g_started;

/* test hook: HSM_FREEZE_SIM="stall:3.5@4,suspend:3600@8" reports a 3.5 s
 * freeze 4 s after the start and a one-hour suspend after 8 s */
static struct {
    double at, dur;
    int kind, done;
} g_sim[8];
static int g_nsim;

static void sim_parse(void)
{
    const char *e = secure_getenv("HSM_FREEZE_SIM");
    while (e && *e && g_nsim < 8) {
        char kind[16];
        double dur, at;
        int len = 0;
        if (sscanf(e, "%15[a-z]:%lf@%lf%n", kind, &dur, &at, &len) != 3 || len <= 0)
            break;
        g_sim[g_nsim].kind = !strcmp(kind, "suspend") ? FZ_SUSPEND : FZ_STALL;
        g_sim[g_nsim].dur = dur;
        g_sim[g_nsim].at = at;
        g_nsim++;
        e += len;
        if (*e == ',')
            e++;
    }
}

static double clk(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void push(double wall, double dur, int kind)
{
    unsigned h = atomic_load(&g_head), t = atomic_load(&g_tail);
    if (h - t >= QLEN)
        return; /* nobody is reading: drop it */
    g_q[h % QLEN] = (fz_t){ wall, dur, kind };
    atomic_store(&g_head, h + 1);
}

static void on_cont(int sig)
{
    (void)sig;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t); /* async-signal-safe */
    atomic_store(&g_cont_ns, (long long)t.tv_sec * 1000000000LL + t.tv_nsec);
}

static void *watchdog(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL); /* every signal goes to the main thread */
    double m = clk(CLOCK_MONOTONIC), b = clk(CLOCK_BOOTTIME), m0 = m;
    const struct timespec tick = { 0, (long)(TICK * 1e9) };
    for (;;) {
        nanosleep(&tick, NULL);
        double m2 = clk(CLOCK_MONOTONIC), b2 = clk(CLOCK_BOOTTIME), w2 = clk(CLOCK_REALTIME);
        for (int i = 0; i < g_nsim; i++)
            if (!g_sim[i].done && m2 - m0 >= g_sim[i].at) {
                g_sim[i].done = 1;
                push(w2 - g_sim[i].dur, g_sim[i].dur, g_sim[i].kind);
            }
        double awake = m2 - m, asleep = (b2 - b) - awake;
        m = m2;
        b = b2;
        if (asleep > SLEEP_MIN) {
            /* suspended: the seconds around it, while the kernel froze and
             * thawed every program, are part of that, not a freeze */
            push(w2 - awake - asleep, asleep, FZ_SUSPEND);
            continue;
        }
        double late = awake - TICK;
        if (late < STALL_MIN)
            continue;
        /* hsm itself stopped (Ctrl+Z, kill -STOP) and continued? The main
         * thread gets a SIGCONT right then; give it a moment to arrive */
        const struct timespec wait = { 0, 50 * 1000000L };
        nanosleep(&wait, NULL);
        long long cont = atomic_load(&g_cont_ns);
        double since_cont = m2 - (double)cont / 1e9;
        if (!(cont && since_cont > -1.0 && since_cont < 1.0))
            push(w2 - awake, late, FZ_STALL);
        m = clk(CLOCK_MONOTONIC);
        b = clk(CLOCK_BOOTTIME);
    }
    return NULL;
}

void freeze_start(void)
{
    if (g_started)
        return;
    sim_parse();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_cont;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCONT, &sa, NULL);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024); /* it needs almost nothing */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&g_thr, &at, watchdog, NULL) == 0)
        g_started = 1;
    pthread_attr_destroy(&at);
}

int freeze_take(fz_t *out)
{
    unsigned h = atomic_load(&g_head), t = atomic_load(&g_tail);
    if (t == h)
        return 0;
    *out = g_q[t % QLEN];
    atomic_store(&g_tail, t + 1);
    return 1;
}
