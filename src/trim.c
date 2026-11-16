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

/* TRIM: an SSD has to be told which blocks are free, or it slows down as it
 * fills and wears faster. For each file system on an SSD: whether TRIM gets
 * through to the drive (encryption and USB adapters can stop it), and what
 * sends it: the "discard" mount option (on every delete), or fstrim run
 * weekly (systemd's fstrim.timer, or cron), and when that last ran. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* the sysfs folder of a block device by its kernel name ("nvme0n1p2", "dm-0") */
static int blkdir(const char *name, char *out, size_t n)
{
    sfmt(out, n, "%s/class/block/%s", g_sysfs, name);
    if (path_exists(out))
        return 1;
    sfmt(out, n, "%s/block/%s", g_sysfs, name);
    return path_exists(out);
}

/* the whole disk a partition is on ("nvme0n1p2" -> "nvme0n1"); a disk is itself */
static void disk_of(const char *name, char *out, size_t n)
{
    char d[PATHBUF], p[PATHBUF];
    str_copy(out, n, name);
    if (!blkdir(name, d, sizeof d))
        return;
    sfmt(p, sizeof p, "%s/partition", d);
    if (!path_exists(p))
        return;
    char *rp = realpath(d, NULL);
    if (!rp)
        return;
    char *sl = strrchr(rp, '/');
    if (sl) {
        *sl = '\0';
        const char *pp = strrchr(rp, '/');
        if (pp)
            str_copy(out, n, pp + 1);
    }
    free(rp);
}

/* the kernel name of a mounted device: "/dev/nvme0n1p2" -> "nvme0n1p2",
 * "/dev/mapper/root" -> "dm-0"; "" when it isn't a block device */
static void kname(const char *dev, char *out, size_t n)
{
    out[0] = '\0';
    char d[PATHBUF];
    if (!strncmp(dev, "/dev/mapper/", 12)) {
        sfmt(d, sizeof d, "%s/block", g_sysfs);
        DIR *b = opendir(d);
        struct dirent *e;
        while (b && (e = readdir(b)) != NULL && !out[0]) {
            char p[PATHBUF], nm[128];
            if (strncmp(e->d_name, "dm-", 3))
                continue;
            sfmt(p, sizeof p, "%s/block/%s/dm/name", g_sysfs, e->d_name);
            if (!read_line(p, nm, sizeof nm) && !strcmp(nm, dev + 12))
                str_copy(out, n, e->d_name);
        }
        if (b)
            closedir(b);
        return;
    }
    if (strncmp(dev, "/dev/", 5))
        return;
    char nm[64];
    str_copy(nm, sizeof nm, dev + 5);
    if (strchr(nm, '/')) /* /dev/disk/by-...: not how the kernel lists mounts */
        return;
    if (blkdir(nm, d, sizeof d))
        str_copy(out, n, nm);
}

/* the devices at the bottom of a stack (dm-crypt, LVM, md): their names */
static void leaves(const char *name, char leaf[][32], int *nl, int depth)
{
    char d[PATHBUF], p[PATHBUF];
    int any = 0;
    if (depth < 5 && blkdir(name, d, sizeof d)) {
        sfmt(p, sizeof p, "%s/slaves", d);
        DIR *s = opendir(p);
        struct dirent *e;
        while (s && (e = readdir(s)) != NULL)
            if (e->d_name[0] != '.') {
                any = 1;
                leaves(e->d_name, leaf, nl, depth + 1);
            }
        if (s)
            closedir(s);
    }
    if (!any && *nl < 8)
        str_copy(leaf[(*nl)++], 32, name);
}

static long long qnum(const char *disk, const char *what)
{
    char d[PATHBUF], p[PATHBUF], b[32];
    if (!blkdir(disk, d, sizeof d))
        return -1;
    sfmt(p, sizeof p, "%s/queue/%s", d, what);
    if (read_line(p, b, sizeof b) || !b[0])
        return -1;
    return strtoll(b, NULL, 10);
}

static int lexists(const char *rel)
{
    char p[PATHBUF];
    struct stat st;
    sfmt(p, sizeof p, "%s%s", g_rootfs, rel);
    return lstat(p, &st) == 0;
}

/* fstrim run weekly: by systemd's timer (and when it last ran) or by cron */
static int trim_timer(char *how, size_t n, double *ago)
{
    *ago = -1;
    static const char *const SD[] = { "/etc/systemd/system/timers.target.wants/fstrim.timer",
                                      "/usr/lib/systemd/system/timers.target.wants/fstrim.timer",
                                      "/lib/systemd/system/timers.target.wants/fstrim.timer" };
    static const char *const CRON[] = { "/etc/cron.weekly/fstrim", "/etc/cron.weekly/fstrim-all",
                                        "/etc/periodic/weekly/fstrim", "/etc/cron.daily/fstrim" };
    for (size_t i = 0; i < sizeof SD / sizeof SD[0]; i++)
        if (lexists(SD[i])) {
            sfmt(how, n, "fstrim.timer");
            char p[PATHBUF];
            struct stat st;
            sfmt(p, sizeof p, "%s/var/lib/systemd/timers/stamp-fstrim.timer", g_rootfs);
            if (stat(p, &st) == 0 && st.st_mtime > 0)
                *ago = MAX(0, (double)(time(NULL) - st.st_mtime));
            return 1;
        }
    for (size_t i = 0; i < sizeof CRON / sizeof CRON[0]; i++)
        if (lexists(CRON[i])) {
            sfmt(how, n, "cron");
            return 1;
        }
    return 0;
}

void trim_sample(disks_t *ds)
{
    char timer[32] = "";
    double ago = -1;
    int weekly = trim_timer(timer, sizeof timer, &ago);
    for (int i = 0; i < ds->nfs; i++) {
        fs_t *f = &ds->fs[i];
        f->trim = TRIM_NA;
        f->trim_why[0] = '\0';
        f->trim_ago = -1;
        char top[32], leaf[8][32];
        int nl = 0;
        kname(f->dev, top, sizeof top);
        if (!top[0])
            continue;
        leaves(top, leaf, &nl, 0);
        /* only SSDs (and virtual disks that take TRIM); not RAM disks or images */
        int ssd = nl > 0, usb = 0;
        for (int k = 0; k < nl; k++) {
            char disk[32], d[PATHBUF];
            disk_of(leaf[k], disk, sizeof disk);
            if (!strncmp(disk, "loop", 4) || !strncmp(disk, "zram", 4) || !strncmp(disk, "ram", 3) ||
                !strncmp(disk, "nbd", 3) || qnum(disk, "rotational") != 0)
                ssd = 0;
            if (blkdir(disk, d, sizeof d)) {
                char *rp = realpath(d, NULL);
                usb |= rp && strstr(rp, "/usb") != NULL;
                free(rp);
            }
        }
        if (!ssd)
            continue;
        /* does TRIM get through: the top of the stack says (dm-crypt without
         * allow-discards says no, so does a USB adapter that doesn't pass it on) */
        char topdisk[32];
        disk_of(top, topdisk, sizeof topdisk);
        if (qnum(topdisk, "discard_max_bytes") == 0) {
            char p[PATHBUF], uuid[64] = "";
            sfmt(p, sizeof p, "%s/block/%s/dm/uuid", g_sysfs, top);
            read_line(p, uuid, sizeof uuid);
            f->trim = TRIM_BLOCKED;
            if (!strncmp(uuid, "CRYPT-", 6))
                sfmt(f->trim_why, sizeof f->trim_why, "blocked by the encryption: add 'discard' to its line in /etc/crypttab");
            else if (usb)
                sfmt(f->trim_why, sizeof f->trim_why, "blocked by its USB adapter");
            else
                sfmt(f->trim_why, sizeof f->trim_why, "not taken by the drive");
            continue;
        }
        if (f->discard) {
            f->trim = TRIM_DISCARD;
            sfmt(f->trim_why, sizeof f->trim_why, "%s", weekly ? timer : "");
            f->trim_ago = weekly ? ago : -1;
        } else if (weekly) {
            f->trim = TRIM_TIMER;
            str_copy(f->trim_why, sizeof f->trim_why, timer);
            f->trim_ago = ago;
        } else {
            f->trim = TRIM_NONE;
        }
    }
}

const char *trim_text(const fs_t *f, char *out, size_t n, int *warn)
{
    char ago[32] = "";
    *warn = 0;
    if (f->trim_ago >= 0) {
        fmt_duration(f->trim_ago, ago, sizeof ago);
        str_copy(ago + strlen(ago), sizeof ago - strlen(ago), " ago");
    }
    switch (f->trim) {
    case TRIM_DISCARD:
        sfmt(out, n, "on every delete (discard)%s", f->trim_why[0] ? ", and weekly" : "");
        break;
    case TRIM_TIMER:
        if (!strcmp(f->trim_why, "cron"))
            sfmt(out, n, "weekly (cron)");
        else
            sfmt(out, n, "weekly (%s), %s%s", f->trim_why, ago[0] ? "last " : "", ago[0] ? ago : "hasn't run yet");
        break;
    case TRIM_NONE:
        sfmt(out, n, "%s", "never: sudo systemctl enable --now fstrim.timer");
        *warn = 1;
        break;
    case TRIM_BLOCKED:
        sfmt(out, n, "%s", f->trim_why);
        *warn = strstr(f->trim_why, "encryption") != NULL;
        break;
    default:
        out[0] = '\0';
    }
    return out;
}
