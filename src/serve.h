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
/* serve.h - hsm --serve (serve.c) */
#ifndef HSM_SERVE_H
#define HSM_SERVE_H

#include "ui.h"
#include <signal.h>
#include <stdio.h>

/* "9101", "0.0.0.0:9101", "[::1]:9101" -> a listening socket, or -1 with err;
 * shown: the address it ended up on */
int serve_open(const char *spec, char *err, size_t n, char *shown, size_t sn);
/* answer requests until the time is up (or quit is set) */
void serve_wait(int fd, double seconds, sys_t *s, volatile sig_atomic_t *quit);
void metrics_write(FILE *f, sys_t *s); /* Prometheus text format */

#endif
