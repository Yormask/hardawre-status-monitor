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
/* cooling.h - the cooling check (cooling.c) */
#ifndef HSM_COOLING_H
#define HSM_COOLING_H

#include "ui.h"
#include <signal.h>

enum { COOL_IDLE, COOL_BASE, COOL_LOAD, COOL_REST, COOL_DONE, COOL_CANCELLED };

typedef struct {
    int phase;
    double phase_t, started; /* monotonic phase start; wall clock of the start */
    unsigned long last_sample;
    int ncpu;
    double limit, stop_at; /* the CPU's limit; where the load stops early */
    double base;           /* °C before the load */
    int nbase;
    double temp, peak, peak_t; /* now; highest under load, seconds into the load */
    double load_s;             /* how long the load ran */
    double mhz_first, mhz_last;
    int nfirst, nlast;
    double fan_base, fan_peak; /* the fastest fan before / under load, -1 = no fans */
    unsigned long long thr0;
    int throttled, stopped_early;
    double end_temp; /* after the rest */
    float curve[512]; /* °C at every sample */
    int ncurve, load_mark, rest_mark;
    char verdict[320], compare[160];
    int level; /* 0 fine, 1 runs hot, 2 can't keep up */
    double before_t, before_base, before_peak; /* the previous check */
    int told; /* a stop was reported */
} cool_t;

int cooling_start(sys_t *s); /* 0, -1 without a CPU temperature */
void cooling_tick(sys_t *s); /* after every sample */
void cooling_stop(void);     /* cancel (the load stops at once) */
void cooling_stopped_event(health_t *h); /* "stopped before the end", once */
const cool_t *cooling_state(void);
double cooling_phase_left(void);                       /* seconds until it's done */
int cooling_cli(sys_t *s, volatile sig_atomic_t *quit); /* hsm --cooling-check; exit status */

#endif
