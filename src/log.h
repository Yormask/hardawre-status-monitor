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
/* log.h - recording sensors and processes to CSV files */
#ifndef HSM_LOG_H
#define HSM_LOG_H

#include "collect.h"
#include <stdio.h>

#define MAX_CHAN 256
#define MAX_PLOG 16

/* One loggable value ("channel"): CPU usage, a temperature sensor, a
 * disk's read speed... built from the hardware that was detected. */
typedef struct {
    char id[64];    /* stable key, used in the config file: "sensor.coretemp.core_0" */
    char label[48]; /* shown on screen: "Core 0" */
    char unit[8];   /* "%", "C", "MHz", "W", "RPM", "MB", "MB/s" */
    char group[16]; /* heading: "CPU", "Memory", "GPU", ... */
    char name[32];  /* disk / interface name, for lookups by name */
    int kind, a;
    int on;
} chan_t;

/* an open per-process log */
typedef struct {
    pid_t pid;
    unsigned long long start;
    char comm[24], path[PATHBUF];
    FILE *f;
    unsigned long rows;
    double t0, last_row;
} plog_t;

typedef struct {
    int nchan;
    chan_t ch[MAX_CHAN];

    /* system log */
    FILE *f;
    char path[PATHBUF];
    unsigned long rows;
    double t0, last_row, last_sync;

    int nplog;
    plog_t pl[MAX_PLOG];

    double every;       /* seconds between rows, 0 = every sample */
    char dir[PATHBUF];  /* where new files go */

    /* a note for the UI (e.g. "process exited, log closed") */
    char note[200];
    int note_err, note_new;
} logger_t;

extern logger_t g_log;

void log_build_channels(sys_t *s, const char *skip); /* skip: ids switched off */
void log_skip_string(char *out, size_t n);          /* ids switched off, for the config file */
int log_count_on(void);
double log_chan_value(const sys_t *s, const chan_t *c); /* NAN when unknown */
void log_fmt_value(const chan_t *c, double v, char *out, size_t n);

int log_start(sys_t *s, char *err, size_t n); /* system log */
void log_stop(void);
int log_proc_find(pid_t pid, unsigned long long start); /* index or -1 */
int log_proc_start(sys_t *s, pid_t pid, unsigned long long start, char *err, size_t n);
void log_proc_stop(int i);
void log_tick(sys_t *s); /* call after every sample: writes the rows that are due */
void log_close_all(void);
long log_file_size(const char *path);

#endif
