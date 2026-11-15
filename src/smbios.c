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
/* smbios.c - the memory modules, from the firmware's SMBIOS tables (type 17,
 * "memory device", as dmidecode shows them): size, kind, speed and slot of
 * each, and whether they run in dual channel. The table is root-only
 * (/sys/firmware/dmi/tables/DMI). Serial numbers are never kept. */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static unsigned w16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static unsigned long w32(const unsigned char *p)
{
    return (unsigned long)w16(p) | (unsigned long)w16(p + 2) << 16;
}

/* string number k of a structure (1-based), "" when none */
static void dmi_str(const unsigned char *s, const unsigned char *end, int k, char *out, size_t n)
{
    out[0] = '\0';
    if (k <= 0)
        return;
    const unsigned char *p = s + s[1];
    for (int i = 1; p < end && *p; i++) {
        size_t l = strnlen((const char *)p, (size_t)(end - p));
        if (i == k) {
            str_copy(out, n, (const char *)p);
            /* trim the padding some firmware leaves */
            size_t m = strlen(out);
            while (m && out[m - 1] == ' ')
                out[--m] = '\0';
            return;
        }
        p += l + 1;
    }
}

static const char *mem_type(int t)
{
    switch (t) {
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x18: return "DDR3";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    default: return "";
    }
}

/* the channel a slot is on, from its labels: "ChannelA-DIMM0", "DIMM_B1",
 * "P0 CHANNEL A", "A1"; 0 = can't tell */
static char channel_of(const char *dev, const char *bank)
{
    const char *sources[2] = { dev, bank };
    for (int s = 0; s < 2; s++) {
        const char *t = sources[s];
        const char *c = strcasestr(t, "channel");
        if (c) {
            c += 7;
            while (*c == ' ' || *c == '_' || *c == '-')
                c++;
            if (isalpha((unsigned char)*c))
                return (char)toupper((unsigned char)*c);
        }
        c = strcasestr(t, "DIMM");
        if (c) { /* "DIMM_A1", "DIMM A2", "DIMMB1" */
            c += 4;
            while (*c == ' ' || *c == '_' || *c == '-')
                c++;
            if (isalpha((unsigned char)*c) && isdigit((unsigned char)c[1]))
                return (char)toupper((unsigned char)*c);
        }
        if (isalpha((unsigned char)t[0]) && isdigit((unsigned char)t[1]) && !t[2]) /* "A1", "B2" */
            return (char)toupper((unsigned char)t[0]);
    }
    return 0;
}

void smbios_memory(machine_t *m)
{
    m->nram = m->ram_slots = 0;
    m->ram_state = 0;
    m->ram_channels = 0;
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/firmware/dmi/tables/DMI", g_sysfs);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        m->ram_state = errno == EACCES || errno == EPERM ? -1 : 0;
        return;
    }
    static unsigned char buf[256 * 1024];
    ssize_t n = 0, r;
    while (n < (ssize_t)sizeof buf && (r = read(fd, buf + n, sizeof buf - (size_t)n)) > 0)
        n += r;
    close(fd);
    const unsigned char *s = buf, *end = buf + (n > 0 ? n : 0);
    char seen[MAX_RAM] = { 0 };
    int soldered = 0;
    while (s + 4 <= end && s[1] >= 4 && s + s[1] <= end) {
        int type = s[0], len = s[1];
        /* the next structure: after the strings, which end with two zero bytes */
        const unsigned char *nx = s + len;
        while (nx + 1 < end && (nx[0] || nx[1]))
            nx++;
        nx += 2;
        if (type == 127) /* end of table */
            break;
        if (type == 17 && len >= 0x15) {
            m->ram_slots++;
            unsigned sz = w16(s + 0x0C);
            unsigned long mb = 0;
            if (sz == 0x7FFF && len >= 0x20)
                mb = w32(s + 0x1C) & 0x7FFFFFFF;
            else if (sz && sz != 0xFFFF)
                mb = sz & 0x8000 ? (sz & 0x7FFF) / 1024 : sz;
            if (mb && m->nram < MAX_RAM) {
                ramdev_t *d = &m->ram[m->nram++];
                memset(d, 0, sizeof *d);
                d->mb = mb;
                dmi_str(s, nx, s[0x10], d->slot, sizeof d->slot);
                dmi_str(s, nx, s[0x11], d->bank, sizeof d->bank);
                str_copy(d->type, sizeof d->type, mem_type(s[0x12]));
                int ff = s[0x0E];
                d->form = ff == 0x09 ? 1 : ff == 0x0D ? 2 : ff == 0x0B || ff == 0x0F ? 3 : 0; /* DIMM, SO-DIMM, soldered */
                if (len >= 0x17) {
                    unsigned v = w16(s + 0x15);
                    d->speed = v == 0xFFFF && len >= 0x58 ? (int)w32(s + 0x54) : v == 0xFFFF ? 0 : (int)v;
                }
                if (len >= 0x22) {
                    unsigned v = w16(s + 0x20);
                    d->conf = v == 0xFFFF && len >= 0x5C ? (int)w32(s + 0x58) : v == 0xFFFF ? 0 : (int)v;
                }
                if (len >= 0x1B) {
                    dmi_str(s, nx, s[0x17], d->maker, sizeof d->maker);
                    dmi_str(s, nx, s[0x1A], d->part, sizeof d->part);
                }
                if (!strcasecmp(d->maker, "Unknown") || !strncmp(d->maker, "0000", 4))
                    d->maker[0] = '\0';
                if (!strcasecmp(d->part, "Unknown") || !strncmp(d->part, "0000", 4))
                    d->part[0] = '\0';
                d->channel = channel_of(d->slot, d->bank);
                soldered |= d->form == 3 || !strncmp(d->type, "LP", 2);
            }
        }
        s = nx;
    }
    m->ram_state = m->ram_slots ? 1 : 0;
    /* channels in use: one module is single channel; modules on different
     * channels are dual (or more); all on one channel is single */
    int nch = 0;
    for (int i = 0; i < m->nram; i++) {
        char c = m->ram[i].channel;
        if (!c) {
            nch = -1;
            break;
        }
        int dup = 0;
        for (int k = 0; k < i; k++)
            dup |= seen[k] == c;
        seen[i] = c;
        if (!dup)
            nch++;
    }
    m->ram_soldered = soldered;
    m->ram_channels = m->nram == 1 && !soldered ? 1 : nch > 0 ? nch : 0;
}
