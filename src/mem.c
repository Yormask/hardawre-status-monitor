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

/* mem.c - RAM / swap usage (/proc/meminfo) and paging activity (/proc/vmstat) */
#define _GNU_SOURCE
#include "collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void mem_sample(mem_t *m, double dt)
{
    static char buf[16384];
    static char *vbuf;
    static size_t vcap;
    char path[PATHBUF];

    sfmt(path, sizeof path, "%s/meminfo", g_procfs);
    if (read_file(path, buf, sizeof buf) > 0) {
        int have_avail = 0;
        char *cur = buf, *line;
        while ((line = next_line(&cur)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            unsigned long long v = strtoull(colon + 1, NULL, 10) * 1024ULL;
            if (!strcmp(line, "MemTotal"))
                m->total = v;
            else if (!strcmp(line, "MemFree"))
                m->free = v;
            else if (!strcmp(line, "MemAvailable")) {
                m->avail = v;
                have_avail = 1;
            } else if (!strcmp(line, "Buffers"))
                m->buffers = v;
            else if (!strcmp(line, "Cached"))
                m->cached = v;
            else if (!strcmp(line, "SReclaimable"))
                m->sreclaim = v;
            else if (!strcmp(line, "Shmem"))
                m->shmem = v;
            else if (!strcmp(line, "SwapTotal"))
                m->swap_total = v;
            else if (!strcmp(line, "SwapFree"))
                m->swap_free = v;
            else if (!strcmp(line, "Dirty"))
                m->dirty = v;
            else if (!strcmp(line, "Writeback"))
                m->writeback = v;
        }
        if (!have_avail) /* kernels older than 3.14 */
            m->avail = m->free + m->buffers + m->cached;
        m->used = m->total > m->avail ? m->total - m->avail : 0;
        m->buffcache = m->buffers + m->cached + m->sreclaim;
        m->swap_used = m->swap_total > m->swap_free ? m->swap_total - m->swap_free : 0;
    }

    sfmt(path, sizeof path, "%s/vmstat", g_procfs);
    if (read_file_grow(path, &vbuf, &vcap) > 0) {
        unsigned long long in = 0, out = 0, maj = 0;
        char *cur = vbuf, *line;
        while ((line = next_line(&cur)) != NULL) {
            if (!strncmp(line, "pswpin ", 7))
                in = strtoull(line + 7, NULL, 10);
            else if (!strncmp(line, "pswpout ", 8))
                out = strtoull(line + 8, NULL, 10);
            else if (!strncmp(line, "pgmajfault ", 11))
                maj = strtoull(line + 11, NULL, 10);
        }
        long pg = sysconf(_SC_PAGESIZE);
        if (pg <= 0)
            pg = 4096;
        if (m->primed && dt > 0) {
            m->swapin_bps = in >= m->p_swpin ? (double)(in - m->p_swpin) * pg / dt : 0;
            m->swapout_bps = out >= m->p_swpout ? (double)(out - m->p_swpout) * pg / dt : 0;
            m->majflt_rate = maj >= m->p_majflt ? (double)(maj - m->p_majflt) / dt : 0;
        }
        m->p_swpin = in;
        m->p_swpout = out;
        m->p_majflt = maj;
        m->primed = 1;
    }
}
