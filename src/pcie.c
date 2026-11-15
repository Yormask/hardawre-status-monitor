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
/* pcie.c - the PCIe link a device runs at, against what it and its slot can
 * do, from sysfs (no root, and nothing is asked of the device itself: the
 * kernel reads the link registers). A drive in a slot with fewer lanes than
 * it has (a chipset M.2 slot, lanes shared with SATA ports) runs at that by
 * design; one below what both can do has a problem: a riser cable, a bad
 * contact, a BIOS setting. */
#define _GNU_SOURCE
#include "collect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "16.0 GT/s PCIe" -> 4 (generation), 0 = unknown */
static int gen_of(const char *dir, const char *name)
{
    char p[PATHBUF], b[64];
    sfmt(p, sizeof p, "%s/%s", dir, name);
    if (read_line(p, b, sizeof b))
        return 0;
    double gt = atof(b);
    return gt >= 63 ? 6 : gt >= 31 ? 5 : gt >= 15 ? 4 : gt >= 7.9 ? 3 : gt >= 4.9 ? 2 : gt >= 2.4 ? 1 : 0;
}

static int width_of(const char *dir, const char *name)
{
    char p[PATHBUF];
    long long v;
    sfmt(p, sizeof p, "%s/%s", dir, name);
    return read_ll(p, &v) || v <= 0 || v > 32 ? 0 : (int)v;
}

int pcie_link(const char *dev, pcilink_t *l)
{
    memset(l, 0, sizeof *l);
    char real[PATHBUF];
    char *rp = realpath(dev, NULL);
    if (!rp)
        return -1;
    str_copy(real, sizeof real, rp);
    free(rp);
    l->gen = gen_of(real, "current_link_speed");
    l->width = width_of(real, "current_link_width");
    l->dev_gen = gen_of(real, "max_link_speed");
    l->dev_width = width_of(real, "max_link_width");
    if (!l->gen || !l->width || !l->dev_gen || !l->dev_width)
        return -1;
    /* the port above it: the slot, and what it can do */
    char *sl = strrchr(real, '/');
    if (sl) {
        *sl = '\0';
        l->slot_gen = gen_of(real, "max_link_speed");
        l->slot_width = width_of(real, "max_link_width");
    }
    int bg = l->slot_gen ? MIN(l->dev_gen, l->slot_gen) : l->dev_gen;
    int bw = l->slot_width ? MIN(l->dev_width, l->slot_width) : l->dev_width;
    l->best_gen = bg;
    l->best_width = bw;
    l->below = l->gen < bg || l->width < bw;                  /* slower than both can do: a problem */
    l->slot_less = !l->below && (bg < l->dev_gen || bw < l->dev_width); /* the slot can't do what the device can */
    return 0;
}

const char *pcie_text(const pcilink_t *l, char *out, size_t n)
{
    if (l->below)
        sfmt(out, n, "PCIe %d.0 x%d, can do %d.0 x%d: slower than the slot and the device allow", l->gen, l->width,
             l->best_gen, l->best_width);
    else if (l->slot_less)
        sfmt(out, n, "PCIe %d.0 x%d: the slot's most (the device can do %d.0 x%d)", l->gen, l->width, l->dev_gen,
             l->dev_width);
    else
        sfmt(out, n, "PCIe %d.0 x%d (its best)", l->gen, l->width);
    return out;
}
