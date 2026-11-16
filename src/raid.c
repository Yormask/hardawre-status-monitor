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

/* RAID arrays and pools: Linux software RAID (md) from /sys/block/mdX/md,
 * ZFS pools' state from the SPL kstats, btrfs file systems with their
 * devices' error counters. All of it is readable without root. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<linux/btrfs.h>)
#include <fcntl.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define HAVE_BTRFS_IOC 1
#endif
#endif

/* the array with this kind and key, or a new one */
static array_t *slot(arrays_t *A, int kind, const char *key)
{
    for (int i = 0; i < A->n; i++)
        if (A->a[i].kind == kind && !strcmp(A->a[i].key, key))
            return &A->a[i];
    if (A->n >= MAX_ARRAYS)
        return NULL;
    array_t *a = &A->a[A->n++];
    memset(a, 0, sizeof *a);
    a->kind = kind;
    str_copy(a->key, sizeof a->key, key);
    return a;
}

/* the live figures start over at every scan; health's memory stays */
static void clear_live(array_t *a)
{
    a->level[0] = a->meta[0] = a->state[0] = a->sync[0] = '\0';
    a->ndisks = a->degraded = a->nmem = 0;
    a->sync_pct = a->sync_kbs = a->sync_left_s = -1;
    a->mismatch = a->errs = 0;
    a->seen = 1;
}

static long long num(const char *dir, const char *name, long long dflt)
{
    char p[PATHBUF], b[64];
    sfmt(p, sizeof p, "%s/%s", dir, name);
    if (read_line(p, b, sizeof b) || !b[0])
        return dflt;
    char *end;
    long long v = strtoll(b, &end, 10);
    return end == b ? dflt : v;
}

/* --------------------------------------------------------------- md RAID */

static void md_read(arrays_t *A, const char *blk)
{
    char base[PATHBUF], p[PATHBUF], b[128];
    sfmt(base, sizeof base, "%s/block/%s/md", g_sysfs, blk);
    sfmt(p, sizeof p, "%s/level", base);
    /* an Intel RST / DDF container holds the real arrays, which are listed too */
    if (read_line(p, b, sizeof b) || !b[0] || !strcmp(b, "container"))
        return;
    array_t *a = slot(A, ARR_MD, blk);
    if (!a)
        return;
    clear_live(a);
    str_copy(a->name, sizeof a->name, blk);
    str_copy(a->level, sizeof a->level, b);
    a->ndisks = (int)num(base, "raid_disks", 0);
    a->degraded = (int)num(base, "degraded", 0);
    a->mismatch = num(base, "mismatch_cnt", 0);
    sfmt(p, sizeof p, "%s/array_state", base);
    read_line(p, a->state, sizeof a->state);
    sfmt(p, sizeof p, "%s/sync_action", base);
    read_line(p, a->sync, sizeof a->sync);
    unsigned long long done = 0, total = 0;
    sfmt(p, sizeof p, "%s/sync_completed", base);
    if (strcmp(a->sync, "idle") && strcmp(a->sync, "frozen") && !read_line(p, b, sizeof b) &&
        sscanf(b, "%llu / %llu", &done, &total) == 2 && total > 0) {
        a->sync_pct = 100.0 * (double)done / (double)total;
        long long kbs = num(base, "sync_speed", -1);
        if (kbs > 0) {
            a->sync_kbs = (double)kbs;
            a->sync_left_s = (double)(total - done) * 512.0 / 1024.0 / (double)kbs;
        }
    }
    /* the disks in it: dev-sda1 folders, each with its state */
    DIR *d = opendir(base);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && a->nmem < MAX_MEMBERS) {
        if (strncmp(e->d_name, "dev-", 4))
            continue;
        char dir[PATHBUF], st[96] = "";
        sfmt(dir, sizeof dir, "%s/%s", base, e->d_name);
        sfmt(p, sizeof p, "%s/state", dir);
        read_line(p, st, sizeof st);
        member_t *m = &a->mem[a->nmem++];
        memset(m, 0, sizeof *m);
        str_copy(m->name, sizeof m->name, e->d_name + 4);
        m->id = (int)num(dir, "slot", -1); /* "none" for a spare: -1 */
        m->bad = strstr(st, "faulty") != NULL;
        m->spare = strstr(st, "spare") != NULL || (!strstr(st, "in_sync") && !m->bad);
        m->err[BE_READ] = num(dir, "errors", 0);
    }
    if (d)
        closedir(d);
    for (int i = 1; i < a->nmem; i++) /* by slot, spares last */
        for (int k = i; k > 0; k--) {
            member_t *x = &a->mem[k - 1], *y = &a->mem[k];
            int kx = x->id < 0 ? 1 << 20 : x->id, ky = y->id < 0 ? 1 << 20 : y->id;
            if (kx < ky || (kx == ky && strcmp(x->name, y->name) <= 0))
                break;
            member_t t = *x;
            *x = *y;
            *y = t;
        }
}

/* --------------------------------------------------------------- ZFS */

static void zfs_read(arrays_t *A)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/spl/kstat/zfs", g_procfs);
    DIR *d = opendir(base);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char p[PATHBUF], st[32];
        sfmt(p, sizeof p, "%s/%s/state", base, e->d_name);
        if (read_line(p, st, sizeof st) || !st[0]) /* not a pool (the module's own figures) */
            continue;
        array_t *a = slot(A, ARR_ZFS, e->d_name);
        if (!a)
            break;
        clear_live(a);
        str_copy(a->name, sizeof a->name, e->d_name);
        str_copy(a->state, sizeof a->state, st);
        a->degraded = strcmp(st, "ONLINE") != 0;
    }
    if (d)
        closedir(d);
}

/* --------------------------------------------------------------- btrfs */

/* the mount point of a device ("sda2", "dm-0") in the mount table, or "" */
static void mount_of(const char *mounts, const char *dev, char *out, size_t n)
{
    char want[2][PATHBUF];
    sfmt(want[0], sizeof want[0], "/dev/%s", dev);
    want[1][0] = '\0';
    if (!strncmp(dev, "dm-", 3)) { /* device mapper: mounted by its /dev/mapper name */
        char p[PATHBUF], nm[96];
        sfmt(p, sizeof p, "%s/block/%s/dm/name", g_sysfs, dev);
        if (!read_line(p, nm, sizeof nm) && nm[0])
            sfmt(want[1], sizeof want[1], "/dev/mapper/%s", nm);
    }
    out[0] = '\0';
    const char *l = mounts;
    while (l && *l) {
        const char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        char line[512];
        str_copy(line, sizeof line, l);
        if (len < sizeof line)
            line[len] = '\0';
        char src[256], mnt[256], type[32];
        if (sscanf(line, "%255s %255s %31s", src, mnt, type) == 3 && !strcmp(type, "btrfs") &&
            (!strcmp(src, want[0]) || (want[1][0] && !strcmp(src, want[1])))) {
            /* "\040" stands for a space in the mount table; a shorter path wins (/ before /home/@snapshots) */
            if (!out[0] || strlen(mnt) < strlen(out))
                str_copy(out, n, mnt);
        }
        l = nl ? nl + 1 : NULL;
    }
}

/* the profiles of one kind of chunks: "raid1", "single,raid1" while converting */
static void profiles(const char *dir, char *out, size_t n)
{
    out[0] = '\0';
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        static const char *const P[] = { "single", "dup", "raid0", "raid1", "raid10", "raid5", "raid6", "raid1c3", "raid1c4" };
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            if (!strcmp(e->d_name, P[i]))
                sfmt(out + strlen(out), n - strlen(out), "%s%s", out[0] ? "," : "", P[i]);
    }
    if (d)
        closedir(d);
}

static void btrfs_read(arrays_t *A, const char *mounts)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/fs/btrfs", g_sysfs);
    DIR *d = opendir(base);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (strlen(e->d_name) != 36 || e->d_name[8] != '-') /* the file systems' UUIDs; "features" isn't one */
            continue;
        array_t *a = slot(A, ARR_BTRFS, e->d_name);
        if (!a)
            break;
        clear_live(a);
        char fs[PATHBUF], p[PATHBUF], label[64] = "";
        sfmt(fs, sizeof fs, "%s/%s", base, e->d_name);
        sfmt(p, sizeof p, "%s/label", fs);
        read_line(p, label, sizeof label);
        /* its devices, by name */
        char devs[MAX_MEMBERS][32];
        int ndev = 0;
        sfmt(p, sizeof p, "%s/devices", fs);
        DIR *dd = opendir(p);
        struct dirent *de;
        while (dd && (de = readdir(dd)) != NULL && ndev < MAX_MEMBERS)
            if (de->d_name[0] != '.')
                str_copy(devs[ndev++], sizeof devs[0], de->d_name);
        if (dd)
            closedir(dd);
        char mnt[128] = "";
        for (int i = 0; i < ndev && !mnt[0]; i++)
            mount_of(mounts, devs[i], mnt, sizeof mnt);
        if (mnt[0])
            str_copy(a->name, sizeof a->name, mnt);
        else if (label[0])
            str_copy(a->name, sizeof a->name, label);
        else
            sfmt(a->name, sizeof a->name, "%.8s", e->d_name);
        sfmt(p, sizeof p, "%s/allocation/data", fs);
        profiles(p, a->level, sizeof a->level);
        sfmt(p, sizeof p, "%s/allocation/metadata", fs);
        profiles(p, a->meta, sizeof a->meta);
        /* each device's error counters (Linux 5.14+) and whether it's missing */
        sfmt(p, sizeof p, "%s/devinfo", fs);
        dd = opendir(p);
        while (dd && (de = readdir(dd)) != NULL && a->nmem < MAX_MEMBERS) {
            char *end;
            long id = strtol(de->d_name, &end, 10);
            if (end == de->d_name || *end)
                continue;
            char dir[PATHBUF], es[PATHBUF], b[512];
            sfmt(dir, sizeof dir, "%s/%s", p, de->d_name);
            member_t *m = &a->mem[a->nmem++];
            memset(m, 0, sizeof *m);
            m->id = (int)id;
            m->bad = num(dir, "missing", 0) > 0;
            sfmt(es, sizeof es, "%s/error_stats", dir);
            if (read_file(es, b, sizeof b) > 0) {
                static const char *const K[BE_N] = { "write_errs", "read_errs", "flush_errs", "corruption_errs",
                                                     "generation_errs" };
                for (int k = 0; k < BE_N; k++) {
                    const char *at = strstr(b, K[k]);
                    if (at)
                        m->err[k] = strtoll(at + strlen(K[k]), NULL, 10);
                    a->errs += m->err[k];
                }
            }
            if (ndev == 1 && !m->bad)
                str_copy(m->name, sizeof m->name, devs[0]);
            else
                sfmt(m->name, sizeof m->name, "device %ld", id);
        }
        if (dd)
            closedir(dd);
        a->ndisks = a->nmem;
        for (int i = 0; i < a->nmem; i++)
            a->degraded += a->mem[i].bad;
#ifdef HAVE_BTRFS_IOC
        /* several devices: which is which, from the mounted file system (no root needed) */
        if (a->nmem > 1 && mnt[0] == '/' && !strstr(mnt, "\\0")) {
            int fd = open(mnt, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
            for (int i = 0; fd >= 0 && i < a->nmem; i++) {
                struct btrfs_ioctl_dev_info_args di;
                memset(&di, 0, sizeof di);
                di.devid = (__u64)a->mem[i].id;
                if (!a->mem[i].bad && !ioctl(fd, BTRFS_IOC_DEV_INFO, &di) && di.path[0]) {
                    const char *nm = strrchr((const char *)di.path, '/');
                    str_copy(a->mem[i].name, sizeof a->mem[i].name, nm ? nm + 1 : (const char *)di.path);
                }
            }
            if (fd >= 0)
                close(fd);
        }
#endif
        for (int i = 1; i < a->nmem; i++) /* by devid */
            for (int k = i; k > 0 && a->mem[k - 1].id > a->mem[k].id; k--) {
                member_t t = a->mem[k - 1];
                a->mem[k - 1] = a->mem[k];
                a->mem[k] = t;
            }
    }
    if (d)
        closedir(d);
}

void arrays_sample(arrays_t *A)
{
    for (int i = 0; i < A->n; i++)
        A->a[i].seen = 0;
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/block", g_sysfs);
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (!strncmp(e->d_name, "md", 2) && e->d_name[2] >= '0' && e->d_name[2] <= '9')
            md_read(A, e->d_name);
    if (d)
        closedir(d);
    zfs_read(A);
    char *mounts = NULL;
    size_t cap = 0;
    sfmt(p, sizeof p, "%s/self/mounts", g_procfs);
    if (read_file_grow(p, &mounts, &cap) < 0 && mounts)
        mounts[0] = '\0';
    btrfs_read(A, mounts ? mounts : "");
    free(mounts);
    /* gone (stopped, exported, unmounted): forgotten */
    int k = 0;
    for (int i = 0; i < A->n; i++)
        if (A->a[i].seen)
            A->a[k++] = A->a[i];
    A->n = k;
    for (int i = 1; i < A->n; i++) /* md, then ZFS, then btrfs; by name */
        for (int j = i; j > 0; j--) {
            array_t *x = &A->a[j - 1], *y = &A->a[j];
            if (x->kind < y->kind || (x->kind == y->kind && strcmp(x->name, y->name) <= 0))
                break;
            array_t t = *x;
            *x = *y;
            *y = t;
        }
    A->nbad = 0;
    for (int i = 0; i < A->n; i++)
        A->nbad += array_problem(&A->a[i], p, sizeof p) == 2;
}

const char *array_level(const array_t *a, char *out, size_t n)
{
    if (a->kind == ARR_ZFS)
        sfmt(out, n, "ZFS pool");
    else if (a->kind == ARR_BTRFS)
        sfmt(out, n, "btrfs, %s", a->level[0] ? a->level : "?");
    else if (!strncmp(a->level, "raid", 4))
        sfmt(out, n, "RAID %s", a->level + 4);
    else
        sfmt(out, n, "%s", a->level); /* linear, multipath */
    return out;
}

int array_problem(const array_t *a, char *out, size_t n)
{
    out[0] = '\0';
    if (a->kind == ARR_ZFS) {
        if (a->degraded) {
            sfmt(out, n, "%s", a->state);
            return 2;
        }
        return 0;
    }
    if (a->kind == ARR_MD) {
        if (!strcmp(a->state, "broken") || !strcmp(a->state, "inactive")) {
            sfmt(out, n, "%s", !strcmp(a->state, "broken") ? "failed" : "not running");
            return 2;
        }
        if (a->degraded > 0) {
            sfmt(out, n, "degraded");
            return 2;
        }
        if (!strcmp(a->sync, "recover") || !strcmp(a->sync, "resync") || !strcmp(a->sync, "reshape")) {
            if (a->sync_pct >= 0)
                sfmt(out, n, "%s %.0f%%", !strcmp(a->sync, "recover") ? "rebuild" : a->sync, a->sync_pct);
            else
                sfmt(out, n, "%s", !strcmp(a->sync, "recover") ? "rebuild" : a->sync);
            return 1;
        }
        return 0;
    }
    if (a->degraded > 0) { /* btrfs */
        sfmt(out, n, "a device missing");
        return 2;
    }
    if (a->errs > 0) {
        sfmt(out, n, "%lld error%s", a->errs, a->errs == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
