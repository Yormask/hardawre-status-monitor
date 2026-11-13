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

#define MAX_CHAN 512
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
    int day; /* date of the file (YYYYMMDD): a new file starts at midnight */
} plog_t;

typedef struct {
    int nchan;
    chan_t ch[MAX_CHAN];

    /* system log */
    FILE *f;
    char path[PATHBUF];
    unsigned long rows;
    double t0, last_row, last_sync;
    unsigned long ev_seq; /* events up to this one are already in the log */
    int day;              /* date of the file (YYYYMMDD): a new file starts at midnight */

    int nplog;
    plog_t pl[MAX_PLOG];

    double every;       /* seconds between rows, 0 = every sample */
    char dir[PATHBUF];  /* where new files go */

    /* notes for the UI (e.g. "process exited, log closed"), oldest first */
    char note[4][200];
    int note_err[4], nnote;
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

/* a message for the screen (or stderr without one) */
void log_note(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int log_note_pop(char *out, size_t n, int *err); /* 1 = got one */

/* shared with the flight recorder */
int log_chan_is_events(const chan_t *c);
int log_day_key(double wall);                           /* YYYYMMDD, local time */
FILE *log_create(double wall, const char *base, char *path, size_t pn, char *err, size_t en);
void log_put_header(FILE *f, const char *first, int all); /* all: every channel, not only the selected */
void log_put_time(FILE *f, double wall);                  /* "2026-11-12 01:23:40,+01:00" */
void log_put_value(FILE *f, const chan_t *c, double v);   /* ",12.3" (or "," when unknown) */
void log_put_events(FILE *f, const health_t *h, unsigned long after, unsigned long upto);

/* Delete logs older than keep_days (0 = never) from dir: date folders and,
 * from before 1.6, loose files. Only hsm's own CSV files. Returns how many. */
int log_housekeeping(const char *dir, int keep_days);

#endif
