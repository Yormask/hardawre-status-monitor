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
/* history.h - events and daily figures kept between runs (history.c) */
#ifndef HSM_HISTORY_H
#define HSM_HISTORY_H

#include "log.h"
#include "ui.h"

/* one day in history.csv; NAN = not known that day */
typedef struct {
    int day; /* YYYYMMDD */
    double hours;                    /* how long some hsm was watching */
    double cpu_avg, cpu_max;         /* % */
    double temp_avg, temp_max;       /* CPU, °C */
    double mem_avg, mem_max;         /* % used */
    double swap_max;                 /* MB */
    double power_avg;                /* W, estimated total */
    double read_gb, written_gb;      /* all drives */
    double rx_gb, tx_gb;             /* all network interfaces */
    double gpu_temp_max, drive_temp_max;
    double throttled_s;
    int freezes, oom, errors, warnings;
    double wear;                     /* most worn drive, % of its rated life */
    double fullest;                  /* fullest file system at the end, % */
    double battery;                  /* battery health, % */
    double temp_h, power_h;          /* while counting: hours with a temperature / power figure */
} hday_t;

typedef struct {
    long long t; /* seconds since 1970 */
    int order;   /* place in the file, to keep it for equal times */
    int level;   /* EV_* */
    char cat[16];
    char text[256];
} hevent_t;

void history_start(sys_t *s); /* from watch_start */
void history_tick(sys_t *s);  /* every sample */
void history_end(void);
int history_writer(void);           /* this hsm writes the files (holds the lock) */
int history_today(hday_t *out);     /* today so far, when this hsm is the writer; 0 = ok */
int history_parse_event(char *line, hevent_t *e); /* a line of events.log; 0 = ok (line is changed) */
int history_parse_day(char *line, hday_t *d);     /* a line of history.csv; 0 = ok (line is changed) */

#endif
