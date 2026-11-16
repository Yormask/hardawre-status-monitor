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

/* The PCI devices and the drivers they use, as lspci -k shows them. A card
 * reader, Wi-Fi card or graphics card without a driver can't work: that's the
 * first thing to look at when one doesn't. Bridges and chipset parts often
 * have none and need none; they're counted, not listed. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int pci_vm_driver(const char *drv)
{
    return !strcmp(drv, "vfio-pci") || !strcmp(drv, "pci-stub");
}

static int has_word(const char *s, const char *w)
{
    return strcasestr(s, w) != NULL;
}

/* what it is, in a word; *shown = 0 for bridges and chipset parts */
static const char *kind_of(unsigned cls, const char *name, int *shown)
{
    unsigned base = cls >> 16, sub = (cls >> 8) & 0xff, pi = cls & 0xff;
    *shown = 1;
    switch (base) {
    case 0x01:
        return sub == 0x08 ? "NVMe"
             : sub == 0x06 ? "SATA"
             : sub == 0x04 ? "RAID"
             : sub == 0x07 ? "SAS"
             : sub == 0x01 ? "IDE"
                           : "storage";
    case 0x02: /* Ethernet cards say 0x0200; Wi-Fi cards nearly all 0x0280, "other" */
        if (has_word(name, "wi-fi") || has_word(name, "wifi") || has_word(name, "wireless") ||
            has_word(name, "802.11") || has_word(name, "wlan"))
            return "Wi-Fi";
        if (sub == 0x00 || has_word(name, "ethernet") || has_word(name, "gigabit") || has_word(name, "GbE"))
            return "Ethernet";
        return sub == 0x07 ? "InfiniBand" : sub == 0x80 ? "Wi-Fi" : "network";
    case 0x03:
        return "graphics";
    case 0x04:
        if (sub == 0x01 || sub == 0x03)
            return "audio";
        if (has_word(name, "imaging") || strstr(name, "IPU") || has_word(name, "camera"))
            return "camera";
        return sub == 0x00 ? "video" : "multimedia";
    case 0x09:
        return "input";
    case 0x0c:
        if (sub == 0x03)
            return pi == 0x40 ? "USB4" : "USB";
        if (sub == 0x00)
            return "FireWire";
        if (sub == 0x04)
            return "Fibre Channel";
        if (has_word(name, "thunderbolt"))
            return "Thunderbolt";
        *shown = 0;
        return sub == 0x05 ? "SMBus" : "serial bus";
    case 0x0d:
        return sub == 0x11 ? "Bluetooth" : "wireless";
    case 0x12:
        return strstr(name, "NPU") ? "NPU" : "accelerator";
    case 0x08:
        if (sub == 0x05)
            return "card reader";
        if (has_word(name, "thunderbolt"))
            return "Thunderbolt";
        *shown = 0;
        return "system";
    case 0x07:
        if (sub == 0x00)
            return "serial port";
        if (sub == 0x03)
            return "modem";
        *shown = 0;
        return "communication";
    case 0x00:
    case 0xff: /* no class given: Realtek's card readers are like this */
        return has_word(name, "card reader") ? "card reader" : "other";
    case 0x06:
        *shown = 0;
        return "bridge";
    default:
        *shown = 0;
        return base == 0x05   ? "memory"
             : base == 0x0b ? "processor"
             : base == 0x10 ? "encryption"
             : base == 0x11 ? "signal processing"
                            : "instrumentation";
    }
}

/* "Realtek Semiconductor Co., Ltd." -> "Realtek"; "Advanced Micro Devices, Inc. [AMD/ATI]" -> "AMD" */
static void vendor_short(const char *v, char *out, size_t n)
{
    const char *lb = strchr(v, '[');
    if (lb) {
        size_t l = strcspn(lb + 1, "/]");
        str_copy(out, MIN(n, l + 1), lb + 1);
        return;
    }
    str_copy(out, n, v);
    static const char *const cut[] = { " Corporation", " Corp", ", Inc", " Inc", " Co.", " Co ", " Co,", " Ltd",
                                       " Limited", " Semiconductor", " Technology", " Technologies", " Electronics",
                                       " GmbH", " AG", " S.A.", " B.V.", " and subsidiaries", "," };
    for (size_t i = 0; i < sizeof cut / sizeof cut[0]; i++) {
        char *p = strstr(out, cut[i]);
        if (p && p > out)
            *p = '\0';
    }
}

/* graphics cards: "GA102 [GeForce RTX 3080]" -> "GeForce RTX 3080"; for the rest the
 * brackets hold a code name or a mode ("[Taylor Peak]", "[AHCI mode]"): kept as they are */
static const char *device_short(char *d, unsigned cls)
{
    if (cls >> 16 != 0x03)
        return d;
    char *lb = strchr(d, '['), *rb = lb ? strrchr(lb, ']') : NULL;
    if (lb && rb && rb > lb + 1) {
        *rb = '\0';
        return lb + 1;
    }
    return d;
}

/* one pass over pci.ids for every device not named yet */
static void name_all(pcibus_t *b)
{
    int todo = 0;
    for (int i = 0; i < b->n; i++)
        todo += !b->d[i].named;
    if (!todo)
        return;
    char vname[MAX_PCI][48];
    memset(vname, 0, sizeof vname);
    FILE *f = pci_ids_open();
    if (f) {
        char line[512];
        unsigned cur = 0x10000;
        int want = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n')
                continue;
            if (line[0] == 'C' && line[1] == ' ') /* the classes: past the vendors */
                break;
            line[strcspn(line, "\n")] = '\0';
            if (line[0] != '\t') {
                unsigned v;
                want = 0;
                if (sscanf(line, "%4x", &v) != 1)
                    continue;
                cur = v;
                const char *nm = line + 4;
                while (*nm == ' ')
                    nm++;
                for (int i = 0; i < b->n; i++)
                    if (!b->d[i].named && b->d[i].ven == cur) {
                        vendor_short(nm, vname[i], sizeof vname[i]);
                        want = 1;
                    }
                continue;
            }
            if (!want || line[1] == '\t') /* subsystems */
                continue;
            unsigned dv;
            if (sscanf(line + 1, "%4x", &dv) != 1)
                continue;
            for (int i = 0; i < b->n; i++) {
                pcid_t *d = &b->d[i];
                if (d->named || d->ven != cur || d->dev != dv)
                    continue;
                char *nm = line + 5;
                while (*nm == ' ')
                    nm++;
                char tmp[256];
                str_copy(tmp, sizeof tmp, nm);
                sfmt(d->name, sizeof d->name, "%s%s%s", vname[i], vname[i][0] ? " " : "", device_short(tmp, d->cls));
                d->named = 1;
            }
        }
        fclose(f);
    }
    static const struct {
        unsigned id;
        const char *name;
    } V[] = { /* the usual makers, for when there's no pci.ids */
              { 0x8086, "Intel" },    { 0x1022, "AMD" },          { 0x1002, "AMD" },       { 0x10de, "NVIDIA" },
              { 0x10ec, "Realtek" },  { 0x14e4, "Broadcom" },     { 0x168c, "Qualcomm Atheros" },
              { 0x17cb, "Qualcomm" }, { 0x14c3, "MediaTek" },     { 0x144d, "Samsung" },   { 0x1c5c, "SK hynix" },
              { 0x1987, "Phison" },   { 0x15b7, "Western Digital" }, { 0x1344, "Micron" }, { 0x2646, "Kingston" },
              { 0x1e0f, "KIOXIA" },   { 0x126f, "Silicon Motion" }, { 0x1b21, "ASMedia" }, { 0x1b4b, "Marvell" },
              { 0x1217, "O2 Micro" }, { 0x1180, "Ricoh" },        { 0x17a0, "Genesys Logic" }, { 0x1912, "Renesas" },
              { 0x1af4, "Virtio" },   { 0x1b36, "QEMU" },         { 0x15ad, "VMware" },    { 0x80ee, "VirtualBox" },
              { 0x1414, "Hyper-V" },  { 0x1d0f, "Amazon" },       { 0x1ae0, "Google" } };
    static const struct {
        unsigned id;
        const char *name;
    } VIO[] = { /* virtio, in virtual machines; 0x10xx the old ids */
                { 0x1000, "network" }, { 0x1001, "block" },     { 0x1002, "memory balloon" }, { 0x1003, "console" },
                { 0x1004, "SCSI" },    { 0x1005, "RNG" },       { 0x1009, "9P file system" },  { 0x1041, "network" },
                { 0x1042, "block" },   { 0x1043, "console" },   { 0x1044, "RNG" },     { 0x1045, "memory balloon" },
                { 0x1048, "SCSI" },    { 0x1049, "9P file system" }, { 0x1050, "GPU" }, { 0x1052, "input" },
                { 0x1053, "socket" },  { 0x105a, "file system" } };
    for (int i = 0; i < b->n; i++) { /* not in pci.ids, or no pci.ids */
        pcid_t *d = &b->d[i];
        if (d->named)
            continue;
        if (!vname[i][0])
            for (size_t k = 0; k < sizeof V / sizeof V[0]; k++)
                if (V[k].id == d->ven)
                    str_copy(vname[i], sizeof vname[i], V[k].name);
        if (d->ven == 0x1af4)
            for (size_t k = 0; k < sizeof VIO / sizeof VIO[0]; k++)
                if (VIO[k].id == d->dev) {
                    sfmt(d->name, sizeof d->name, "Virtio %s", VIO[k].name);
                    d->named = 1;
                }
        if (d->named)
            continue;
        if (vname[i][0])
            sfmt(d->name, sizeof d->name, "%s device %04x", vname[i], d->dev);
        else
            sfmt(d->name, sizeof d->name, "PCI device %04x:%04x", d->ven, d->dev);
        d->named = 1;
    }
}

static int by_slot(const void *a, const void *b)
{
    return strcmp(((const pcid_t *)a)->slot, ((const pcid_t *)b)->slot);
}

void pci_sample(pcibus_t *b)
{
    static pcid_t prev[MAX_PCI];
    int nprev = b->n;
    memcpy(prev, b->d, sizeof(pcid_t) * (size_t)nprev);
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/bus/pci/devices", g_sysfs);
    DIR *dir = opendir(base);
    b->ok = dir != NULL;
    b->n = 0;
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && b->n < MAX_PCI) {
        if (e->d_name[0] == '.')
            continue;
        pcid_t *d = &b->d[b->n];
        memset(d, 0, sizeof *d);
        str_copy(d->slot, sizeof d->slot, e->d_name);
        char p[PATHBUF], lk[PATHBUF];
        sfmt(p, sizeof p, "%s/%s/class", base, e->d_name);
        read_hex(p, &d->cls);
        sfmt(p, sizeof p, "%s/%s/vendor", base, e->d_name);
        read_hex(p, &d->ven);
        sfmt(p, sizeof p, "%s/%s/device", base, e->d_name);
        read_hex(p, &d->dev);
        sfmt(p, sizeof p, "%s/%s/driver", base, e->d_name);
        ssize_t l = readlink(p, lk, sizeof lk - 1);
        if (l > 0) {
            lk[l] = '\0';
            const char *sl = strrchr(lk, '/');
            str_copy(d->driver, sizeof d->driver, sl ? sl + 1 : lk);
        }
        sfmt(p, sizeof p, "%s/%s/physfn", base, e->d_name);
        struct stat st;
        d->vf = lstat(p, &st) == 0;
        for (int k = 0; k < nprev; k++) /* known: keep its name */
            if (!strcmp(prev[k].slot, d->slot) && prev[k].ven == d->ven && prev[k].dev == d->dev && prev[k].named) {
                str_copy(d->name, sizeof d->name, prev[k].name);
                d->named = 1;
                break;
            }
        b->n++;
    }
    closedir(dir);
    qsort(b->d, (size_t)b->n, sizeof b->d[0], by_slot);
    name_all(b);
    b->nodrv = b->hidden = 0;
    for (int i = 0; i < b->n; i++) {
        pcid_t *d = &b->d[i];
        str_copy(d->kind, sizeof d->kind, kind_of(d->cls, d->name, &d->shown));
        d->needs = d->shown && !d->driver[0] && !d->vf && strcmp(d->kind, "other");
        b->nodrv += d->needs;
        b->hidden += !d->shown;
    }
}
