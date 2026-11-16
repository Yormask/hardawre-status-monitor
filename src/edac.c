/*
 * hsm - hardware status monitor
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

/* ECC memory: the errors it found and fixed (corrected) or couldn't fix
 * (uncorrected), counted by the kernel's EDAC drivers since boot, per
 * memory controller and module. Readable without root. Whether ECC is on
 * at all comes from EDAC, or from the firmware's tables (root). */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long long num(const char *dir, const char *name)
{
    char p[PATHBUF], b[32];
    sfmt(p, sizeof p, "%s/%s", dir, name);
    if (read_line(p, b, sizeof b) || !b[0])
        return 0;
    return strtoll(b, NULL, 10);
}

/* the modules (dimmN, or rankN for drivers that count per rank) of one controller */
static void modules(ecc_t *e, const char *mc, const char *mcname)
{
    DIR *d = opendir(mc);
    struct dirent *x;
    while (d && (x = readdir(d)) != NULL) {
        int dimm = !strncmp(x->d_name, "dimm", 4), rank = !strncmp(x->d_name, "rank", 4);
        if ((!dimm && !rank) || x->d_name[4] < '0' || x->d_name[4] > '9')
            continue;
        char dir[PATHBUF], p[PATHBUF], label[48] = "", loc[48] = "", mode[16] = "";
        sfmt(dir, sizeof dir, "%s/%s", mc, x->d_name);
        long long sz = num(dir, "size");
        if (sz <= 0) /* an empty slot */
            continue;
        sfmt(p, sizeof p, "%s/dimm_edac_mode", dir);
        read_line(p, mode, sizeof mode);
        if (mode[0] && strcmp(mode, "Unknown") && strcmp(mode, "None") && strcmp(mode, "Reserved") && !e->mode[0])
            str_copy(e->mode, sizeof e->mode, mode);
        if (e->ndimm >= MAX_EDAC_DIMMS)
            continue;
        sfmt(p, sizeof p, "%s/dimm_label", dir);
        read_line(p, label, sizeof label);
        sfmt(p, sizeof p, "%s/dimm_location", dir);
        read_line(p, loc, sizeof loc);
        /* a label the kernel made up ("mc#0csrow#1channel#0") says less than where it is */
        if (!label[0] || !strncmp(label, "mc#", 3))
            sfmt(label, sizeof label, "%s %s", mcname, loc[0] ? loc : x->d_name);
        str_copy(e->dimm[e->ndimm].label, sizeof e->dimm[0].label, label);
        e->dimm[e->ndimm].ce = num(dir, "dimm_ce_count");
        e->dimm[e->ndimm].ue = num(dir, "dimm_ue_count");
        e->ndimm++;
    }
    if (d)
        closedir(d);
}

void ecc_sample(ecc_t *e)
{
    e->ok = 0;
    e->ce = e->ue = 0;
    e->ndimm = 0;
    e->mode[0] = e->driver[0] = '\0';
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/devices/system/edac/mc", g_sysfs);
    DIR *d = opendir(base);
    struct dirent *x;
    while (d && (x = readdir(d)) != NULL) {
        if (strncmp(x->d_name, "mc", 2) || x->d_name[2] < '0' || x->d_name[2] > '9')
            continue;
        char mc[PATHBUF], p[PATHBUF];
        sfmt(mc, sizeof mc, "%s/%s", base, x->d_name);
        e->ok = 1;
        if (!e->driver[0]) {
            sfmt(p, sizeof p, "%s/mc_name", mc);
            read_line(p, e->driver, sizeof e->driver);
        }
        /* the counts include errors no module could be named for */
        e->ce += num(mc, "ce_count");
        e->ue += num(mc, "ue_count");
        modules(e, mc, x->d_name);
    }
    if (d)
        closedir(d);
}

int ecc_state(const ecc_t *e, const machine_t *m, char *why, size_t n)
{
    why[0] = '\0';
    if (e->ok) {
        sfmt(why, n, "on%s%s%s", e->mode[0] ? " (" : "", e->mode, e->mode[0] ? ")" : "");
        return 1;
    }
    if (m->ram_state != 1 || !m->nram)
        return -1;
    int ecc_mods = 0, plain = 0;
    for (int i = 0; i < m->nram; i++) {
        const ramdev_t *r = &m->ram[i];
        if (r->total_w && r->data_w && r->total_w > r->data_w)
            ecc_mods++;
        else if (r->total_w && r->data_w)
            plain++;
    }
    if (plain && !ecc_mods) {
        sfmt(why, n, "off: the modules have none");
        return 0;
    }
    if (ecc_mods && m->ram_ecc == 3) {
        sfmt(why, n, "off: the modules have it, but the board doesn't use it");
        return 0;
    }
    if (ecc_mods && (m->ram_ecc == 5 || m->ram_ecc == 6)) {
        sfmt(why, n, "on (the firmware says so); no EDAC driver counts the errors");
        return 1;
    }
    return -1;
}
