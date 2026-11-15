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
/* ui_busy.c - Hardware view > U: what keeps each drive busy, for when it
 * won't unmount ("target is busy") or you want to unplug it. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_USE 512
static drvfs_t g_fs[MAX_FS];
static int g_nfs, g_nuse, g_denied;
static drvuse_t g_use[MAX_USE];
static double g_when;

/* the disk a partition is on: "/dev/sdc1" -> "sdc", "/dev/nvme0n1p2" -> "nvme0n1" */
static void disk_of(const char *dev, char *out, size_t n)
{
    const char *b = strrchr(dev, '/');
    str_copy(out, n, b ? b + 1 : dev);
    size_t l = strlen(out);
    if (!strncmp(out, "nvme", 4) || !strncmp(out, "mmcblk", 6)) {
        char *p = strrchr(out, 'p');
        if (p && p > out + 4 && p[1] >= '0' && p[1] <= '9')
            *p = '\0';
    } else {
        while (l && out[l - 1] >= '0' && out[l - 1] <= '9')
            out[--l] = '\0';
    }
}

static void scan(sys_t *s)
{
    g_nfs = 0;
    /* USB drives first, then by where they are mounted; "/" can't be unmounted anyway */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s->disks.nfs && g_nfs < MAX_FS; i++) {
            const fs_t *f = &s->disks.fs[i];
            char disk[32];
            disk_of(f->dev, disk, sizeof disk);
            int usb = 0;
            for (int k = 0; k < s->usb.n; k++)
                usb |= s->usb.d[k].blk[0] && !strcmp(s->usb.d[k].blk, disk);
            if (usb != !pass)
                continue;
            drvfs_t *d = &g_fs[g_nfs++];
            memset(d, 0, sizeof *d);
            str_copy(d->mount, sizeof d->mount, f->mount);
            str_copy(d->dev, sizeof d->dev, f->dev);
            str_copy(d->type, sizeof d->type, f->type);
            d->usb = usb;
        }
    g_nuse = busy_scan(g_fs, g_nfs, g_use, MAX_USE, &g_denied);
    g_when = now_sec();
}

void busy_open(sys_t *s, ui_state_t *u)
{
    u->busy_open = 1;
    u->busy_scroll = 0;
    scan(s);
}

int busy_key(sys_t *s, ui_state_t *u, int ch)
{
    switch (ch) {
    case 27:
    case KEY_LEFT: u->busy_open = 0; return 1;
    case KEY_UP: u->busy_scroll--; break;
    case KEY_DOWN: u->busy_scroll++; break;
    case KEY_PPAGE: u->busy_scroll -= 10; break;
    case KEY_NPAGE: u->busy_scroll += 10; break;
    case KEY_HOME: u->busy_scroll = 0; break;
    case 'r': scan(s); ui_message(u, 0, "Looked again"); break;
    default: return 0;
    }
    if (u->busy_scroll < 0)
        u->busy_scroll = 0;
    return 1;
}

/* one line of the page, skipping the ones scrolled past */
typedef struct {
    int y, top, ymax, skip, row, x, w;
} pg_t;

static int pg_row(pg_t *p)
{
    int r = p->row++;
    if (r < p->skip || p->y >= p->ymax)
        return -1;
    return p->y++;
}

void busy_view(sys_t *s, ui_state_t *u)
{
    int top = 1, H = LINES - 2, W = COLS;
    box_(top, 0, H, W, "What keeps each drive busy", NULL);
    pg_t p = { top + 1, top + 1, top + H - 1, u->busy_scroll, 0, 2, W - 4 };
    int y;
    /* data the drives haven't been given yet: unplugging now would lose it */
    double pend = (double)(s->mem.dirty + s->mem.writeback);
    char a[24];
    fmt_bytes(pend, a, sizeof a);
    if ((y = pg_row(&p)) >= 0)
        put(y, p.x, p.w, pend >= 1 << 20 ? CP(C_WARN) | A_BOLD : A_GRAY, "%s %s%s",
            pend >= 1 << 20 ? "Not yet written to the drives:" : "Waiting to be written:", a,
            pend >= 1 << 20 ? " (Linux writes in the background: wait before you unplug a drive)" : " (nothing worth waiting for)");
    if (g_denied && (y = pg_row(&p)) >= 0)
        put(y, p.x, p.w, A_GRAY, "%d process%s of other users couldn't be checked: sudo hsm sees them all", g_denied,
            g_denied == 1 ? "" : "es");
    pg_row(&p);
    if (!g_nfs && (y = pg_row(&p)) >= 0)
        put(y, p.x, p.w, A_GRAY, "%s", "No file systems to look at.");
    for (int i = 0; i < g_nfs; i++) {
        const drvfs_t *f = &g_fs[i];
        if (!strcmp(f->mount, "/"))
            continue;
        int users = f->nproc + f->more;
        char tail[64];
        if (users)
            sfmt(tail, sizeof tail, "in use by %d process%s", users, users == 1 ? "" : "es");
        else if (f->swap || f->loops || f->nested)
            sfmt(tail, sizeof tail, "%s", "held (see below)");
        else
            sfmt(tail, sizeof tail, "%s", "not in use: it can be unmounted");
        if ((y = pg_row(&p)) >= 0) {
            int xx = p.x;
            xx += put(y, xx, p.w, CP(C_INFO) | A_BOLD, "%s", f->mount);
            put(y, xx, p.x + p.w - xx, A_GRAY, "  %s%s%s%s", f->dev, SEP, f->type, f->usb ? " · USB" : "");
            put(y, p.x + p.w - utf8_cols(tail), utf8_cols(tail), users || f->swap || f->loops || f->nested ? A_BOLD : CP(C_OK),
                "%s", tail);
        }
        if (f->nested && (y = pg_row(&p)) >= 0)
            put(y, p.x + 3, p.w - 3, A_NORMAL, "%d file system%s mounted inside it: unmount %s first", f->nested,
                f->nested == 1 ? "" : "s", f->nested == 1 ? "that one" : "those");
        if (f->swap && (y = pg_row(&p)) >= 0)
            put(y, p.x + 3, p.w - 3, A_NORMAL, "a swap file on it is in use (swapoff it first)");
        if (f->loops && (y = pg_row(&p)) >= 0)
            put(y, p.x + 3, p.w - 3, A_NORMAL, "%d disk image%s on it attached as a loop device (losetup -d)", f->loops,
                f->loops == 1 ? "" : "s");
        for (int k = 0; k < g_nuse; k++) {
            const drvuse_t *d = &g_use[k];
            if (d->fs != i || (y = pg_row(&p)) < 0)
                continue;
            char what[240] = "";
            if (d->nfiles == 1)
                sfmt(what, sizeof what, "open: %s", d->file);
            else if (d->nfiles > 1)
                sfmt(what, sizeof what, "%d files open (%s, ...)", d->nfiles, d->file);
            else if (d->exe)
                sfmt(what, sizeof what, "%s", "its program comes from here");
            else if (d->maps)
                sfmt(what, sizeof what, "uses a file from here: %s", d->file);
            if (d->cwd)
                app(what, sizeof what, "%sworking folder", what[0] ? ", " : "");
            if (d->root)
                app(what, sizeof what, "%sits root folder (chroot)", what[0] ? ", " : "");
            int xx = p.x + 3;
            xx += put(y, xx, 16, A_BOLD, "%-15s", d->comm);
            xx += put(y, xx, 8, A_GRAY, "%7d ", (int)d->pid);
            xx += put(y, xx, 10, A_GRAY, "%-9.9s ", uid_name(d->uid));
            put(y, xx, p.x + p.w - xx, A_NORMAL, "%s", what);
        }
        if (f->more && (y = pg_row(&p)) >= 0)
            put(y, p.x + 3, p.w - 3, A_GRAY, "and %d more", f->more);
        pg_row(&p);
    }
    u->busy_scroll = MIN(u->busy_scroll, MAX(0, p.row - (p.ymax - p.top)));
    if ((y = p.ymax - 1) > p.top && p.row <= p.ymax - p.top)
        put(y, p.x, p.w, A_GRAY, "%s", "To free a drive: close the files, or the program; a terminal whose folder is on it: cd elsewhere.");
}
