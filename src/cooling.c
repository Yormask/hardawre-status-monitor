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
/* cooling.c - the cooling check: every CPU core busy for a minute, then
 * half a minute of rest, watching the temperature, the clock speed, the
 * fans and throttling. It says whether the cooling keeps up, and compares
 * with the previous check: a machine that gets a few degrees hotter every
 * few months is collecting dust. Results are kept in cooling.log in the
 * state folder. The load stops early near the CPU's temperature limit. */
#define _GNU_SOURCE
#include "cooling.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BASE_S 5.0  /* idle, before */
#define LOAD_S 60.0 /* everything busy */
#define REST_S 30.0 /* cooling down */

static cool_t C;
static atomic_int g_stop;
static pthread_t g_thr[256];
static int g_nthr;

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *burn(void *arg)
{
    (void)arg;
    volatile double x = 1.0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed))
        for (int i = 0; i < 200000; i++)
            x = x * 1.0000001 + 0.5;
    return NULL;
}

static void load_on(int n)
{
    atomic_store(&g_stop, 0);
    g_nthr = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    for (int i = 0; i < n && i < 256; i++)
        if (pthread_create(&g_thr[g_nthr], &at, burn, NULL) == 0)
            g_nthr++;
    pthread_attr_destroy(&at);
}

static void load_off(void)
{
    atomic_store(&g_stop, 1);
    for (int i = 0; i < g_nthr; i++)
        pthread_join(g_thr[i], NULL);
    g_nthr = 0;
}

static double fastest_fan(const sensors_t *S)
{
    double m = -1;
    for (int i = 0; i < S->n; i++)
        if (S->s[i].type == SENS_FAN && S->s[i].ok && !S->s[i].hidden)
            m = MAX(m, S->s[i].value);
    return m;
}

static unsigned long long throttles(const health_t *h)
{
    return h->thr_ok ? h->thr_count + h->core_count : 0;
}

/* the previous check, from cooling.log */
static void read_before(void)
{
    C.before_t = 0;
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/cooling.log", config_state_dir());
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        double t, base, peak;
        if (line[0] != '#' && sscanf(line, "%lf %lf %lf", &t, &base, &peak) == 3) {
            C.before_t = t;
            C.before_base = base;
            C.before_peak = peak;
        }
    }
    fclose(f);
}

static void save_result(void)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/cooling.log", config_state_dir());
    int who = config_as_user();
    if (who < 0)
        return;
    int fd = -1;
    if (config_mkdir_p(config_state_dir()) == 0)
        fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
    config_as_root(who);
    if (fd < 0)
        return;
    FILE *f = fdopen(fd, "a");
    if (!f) {
        close(fd);
        return;
    }
    if (ftell(f) == 0)
        fprintf(f, "# hsm cooling checks: time, idle C, peak C, seconds to the peak, C after the rest, "
                   "MHz at first, MHz at the end, fan RPM idle, fan RPM peak, throttled, verdict 0 ok 1 warm 2 bad\n");
    fprintf(f, "%.0f %.1f %.1f %.0f %.1f %.0f %.0f %.0f %.0f %d %d\n", C.started, C.base, C.peak, C.peak_t, C.end_temp,
            C.mhz_first, C.mhz_last, C.fan_base, C.fan_peak, C.throttled, C.level);
    fclose(f);
}

static void verdict(health_t *h)
{
    char *v = C.verdict;
    size_t n = sizeof C.verdict;
    int lvl = 0;
    double lim = C.limit;
    if (C.stopped_early || C.throttled) {
        lvl = 2;
        if (C.throttled)
            sfmt(v, n, "The CPU slowed itself down to protect itself from heat at %.0f °C: the cooling can't keep up. "
                       "Check that the fan turns and the heatsink isn't clogged with dust.",
                 C.peak);
        else
            sfmt(v, n, "Stopped early at %.0f °C, close to the CPU's %.0f °C limit: the cooling can't keep up. "
                       "Check that the fan turns and the heatsink isn't clogged with dust.",
                 C.peak, lim);
    } else if (C.peak >= lim - 10 || C.peak >= 90) {
        lvl = 1;
        sfmt(v, n, "Gets hot: %.0f °C under full load, %.0f °C below its limit. Cleaning the fan and heatsink, or "
                   "fresh thermal paste, would give it more room.",
             C.peak, lim - C.peak);
    } else {
        sfmt(v, n, "Cooling keeps up: %.0f °C to %.0f °C under full load without slowing down, back to %.0f °C after "
                   "%.0f s of rest.",
             C.base, C.peak, C.end_temp, REST_S);
    }
    if (C.fan_base >= 0 && C.fan_peak >= 0 && C.fan_base > 0 && C.fan_peak < C.fan_base * 1.1 && C.peak - C.base > 15) {
        lvl = MAX(lvl, 1);
        size_t l = strlen(v);
        sfmt(v + l, n - l, " The fans didn't speed up (%.0f RPM).", C.fan_peak);
    }
    C.level = lvl;
    /* against the previous check */
    C.compare[0] = '\0';
    if (C.before_t > 0 && !C.stopped_early) {
        time_t t = (time_t)C.before_t;
        struct tm tm;
        localtime_r(&t, &tm);
        char d[32];
        strftime(d, sizeof d, "%d %b %Y", &tm);
        double dp = C.peak - C.before_peak;
        sfmt(C.compare, sizeof C.compare, "The check of %s peaked at %.0f °C: %+.0f °C now%s", d, C.before_peak, dp,
             dp >= 5 ? ", the cooling got worse (dust?)" : dp <= -5 ? ", better than then" : ", about the same");
    }
    health_event(h, lvl == 2 ? EV_ERR : lvl ? EV_WARN : EV_INFO, "cooling", "Cooling check: %.0f °C to %.0f °C under load%s%s",
                 C.base, C.peak, C.throttled ? ", throttled" : C.stopped_early ? ", stopped early" : "",
                 lvl == 2 ? ": the cooling can't keep up" : lvl ? ": runs hot" : ": fine");
}

int cooling_start(sys_t *s)
{
    if (C.phase == COOL_BASE || C.phase == COOL_LOAD || C.phase == COOL_REST)
        return 0;
    if (s->cpu.temp_c < 0)
        return -1; /* nothing to measure with */
    memset(&C, 0, sizeof C);
    const sensor_t *t = s->sens.cpu_idx >= 0 ? &s->sens.s[s->sens.cpu_idx] : NULL;
    double crit = t && t->crit > 0 ? t->crit : 100.0;
    C.limit = crit;
    C.stop_at = crit - 3;
    C.phase = COOL_BASE;
    C.phase_t = now_sec();
    C.started = wall();
    C.base = 0;
    C.peak = -1;
    C.fan_base = fastest_fan(&s->sens);
    C.fan_peak = -1;
    C.ncpu = s->cpu.ncpu;
    C.last_sample = s->samples;
    read_before();
    health_event(&s->health, EV_INFO, "cooling", "Cooling check started: all %d CPU threads busy for a minute, on purpose",
                 C.ncpu);
    return 0;
}

void cooling_stop(void)
{
    if (C.phase == COOL_LOAD)
        load_off();
    if (C.phase == COOL_BASE || C.phase == COOL_LOAD || C.phase == COOL_REST)
        C.phase = COOL_CANCELLED;
}

/* after a stop: an event, so the logs explain the minute of load */
void cooling_stopped_event(health_t *h)
{
    if (C.phase == COOL_CANCELLED && !C.told) {
        C.told = 1;
        health_event(h, EV_INFO, "cooling", "Cooling check stopped before the end (the CPU load is off)");
    }
}

void cooling_tick(sys_t *s)
{
    if ((C.phase != COOL_BASE && C.phase != COOL_LOAD && C.phase != COOL_REST) || s->samples == C.last_sample)
        return;
    C.last_sample = s->samples;
    double now = now_sec(), in = now - C.phase_t, temp = s->cpu.temp_c;
    C.temp = temp;
    if (C.ncurve < (int)(sizeof C.curve / sizeof C.curve[0]))
        C.curve[C.ncurve++] = (float)temp;
    double fan = fastest_fan(&s->sens);
    switch (C.phase) {
    case COOL_BASE:
        C.base += temp;
        C.nbase++;
        if (in >= BASE_S) {
            C.base /= MAX(C.nbase, 1);
            C.thr0 = throttles(&s->health);
            C.phase = COOL_LOAD;
            C.phase_t = now;
            C.load_mark = C.ncurve;
            load_on(C.ncpu);
        }
        break;
    case COOL_LOAD:
        if (temp > C.peak) {
            C.peak = temp;
            C.peak_t = in;
        }
        C.fan_peak = MAX(C.fan_peak, fan);
        if (s->cpu.mhz_avg > 0) {
            if (in <= 5) {
                C.mhz_first += s->cpu.mhz_avg;
                C.nfirst++;
            } else if (in >= LOAD_S - 10) {
                C.mhz_last += s->cpu.mhz_avg;
                C.nlast++;
            }
        }
        if (throttles(&s->health) > C.thr0)
            C.throttled = 1;
        if (temp >= C.stop_at)
            C.stopped_early = 1;
        if (in >= LOAD_S || C.stopped_early) {
            load_off();
            C.load_s = in;
            C.mhz_first = C.nfirst ? C.mhz_first / C.nfirst : NAN;
            C.mhz_last = C.nlast ? C.mhz_last / C.nlast : NAN;
            C.phase = COOL_REST;
            C.phase_t = now;
            C.rest_mark = C.ncurve;
        }
        break;
    case COOL_REST:
        C.end_temp = temp;
        if (in >= REST_S) {
            C.phase = COOL_DONE;
            verdict(&s->health);
            save_result();
        }
        break;
    }
}

const cool_t *cooling_state(void)
{
    return &C;
}

double cooling_phase_left(void)
{
    double in = now_sec() - C.phase_t;
    switch (C.phase) {
    case COOL_BASE: return BASE_S - in + LOAD_S + REST_S;
    case COOL_LOAD: return LOAD_S - in + REST_S;
    case COOL_REST: return REST_S - in;
    }
    return 0;
}

/* hsm --cooling-check: the same, on the command line */
static void nap(double sec, volatile sig_atomic_t *quit)
{
    struct timespec ts = { (time_t)sec, (long)((sec - (double)(time_t)sec) * 1e9) };
    while (nanosleep(&ts, &ts) == -1 && !*quit)
        ;
}

int cooling_cli(sys_t *s, volatile sig_atomic_t *quit)
{
    if (cooling_start(s) != 0) {
        fprintf(stderr, "%s: no CPU temperature sensor found: nothing to check with\n", APP_NAME);
        return 2;
    }
    printf("Cooling check: %d CPU threads busy for %.0f s, then %.0f s of rest. Ctrl+C stops it.\n", C.ncpu, LOAD_S,
           REST_S);
    fflush(stdout);
    double next_print = 0;
    while (C.phase == COOL_BASE || C.phase == COOL_LOAD || C.phase == COOL_REST) {
        if (*quit) {
            cooling_stop();
            printf("\nStopped.\n");
            return 3;
        }
        nap(1.0, quit);
        if (*quit)
            continue;
        sys_sample(s);
        cooling_tick(s);
        if (now_sec() >= next_print || C.phase == COOL_DONE) {
            next_print = now_sec() + 5;
            static const char *const ph[] = { "", "idle", "full load", "rest", "", "" };
            char fan[32] = "";
            if (fastest_fan(&s->sens) >= 0)
                sfmt(fan, sizeof fan, "  fan %.0f RPM", fastest_fan(&s->sens));
            if (C.phase != COOL_DONE)
                printf("  %-9s %5.1f °C  clock %4.0f MHz%s  (%.0f s left)\n", ph[C.phase], s->cpu.temp_c, s->cpu.mhz_avg,
                       fan, MAX(0, cooling_phase_left()));
            fflush(stdout);
        }
    }
    printf("\nIdle %.0f °C, peak %.0f °C after %.0f s, %.0f °C after %.0f s of rest.\n", C.base, C.peak, C.peak_t,
           C.end_temp, REST_S);
    if (!isnan(C.mhz_first) && !isnan(C.mhz_last))
        printf("Clock under load: %.0f MHz at first, %.0f MHz at the end.\n", C.mhz_first, C.mhz_last);
    if (C.fan_base >= 0)
        printf("Fastest fan: %.0f RPM idle, %.0f RPM under load.\n", C.fan_base, C.fan_peak);
    printf("\n%s\n", C.verdict);
    if (C.compare[0])
        printf("%s\n", C.compare);
    return C.level == 2 ? 1 : 0;
}
