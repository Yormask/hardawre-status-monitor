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
/* busy.c - what keeps a drive busy ("target is busy" when you unmount it):
 * the processes with a file open on it, their working folder there, or a
 * program or library from it mapped in; and swap files, loop devices and
 * other file systems mounted inside it. Only when asked (it reads every
 * process's open files). Paths are compared as text, never looked up, so a
 * dead network drive can't hang it; a file belongs to the deepest mount
 * point it is under. Other users' processes need root. */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the deepest of the mount points path is under, -1 none */
static int owner(const drvfs_t *fs, int nfs, const char *path)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < nfs; i++) {
        size_t l = strlen(fs[i].mount);
        int under = l == 1 ? path[0] == '/' : !strncmp(path, fs[i].mount, l) && (path[l] == '/' || !path[l]);
        if (under && l > bl) {
            best = i;
            bl = l;
        }
    }
    return best;
}

static int link_of(const char *p, char *out, size_t n)
{
    ssize_t l = readlink(p, out, n - 1);
    if (l <= 0)
        return -1;
    out[l] = '\0';
    char *d = strstr(out, " (deleted)"); /* still open, though gone from the folder */
    if (d && !d[10])
        *d = '\0';
    return out[0] == '/' ? 0 : -1; /* sockets, pipes, anon_inode: not files */
}

/* one process: what it holds on each file system; returns the record for fs k, creating it */
#define PER_FS 40 /* processes listed per file system; past that they're counted */
static drvuse_t *rec(drvfs_t *F, drvuse_t *out, int max, int *n, pid_t pid, int fs, const char *comm, uid_t uid)
{
    int on = 0;
    for (int i = 0; i < *n; i++) {
        if (out[i].pid == pid && out[i].fs == fs)
            return &out[i];
        on += out[i].fs == fs;
    }
    if (*n >= max || on >= PER_FS) {
        if (F[fs].lastp != pid) {
            F[fs].lastp = pid;
            F[fs].more++;
        }
        return NULL;
    }
    drvuse_t *b = &out[(*n)++];
    memset(b, 0, sizeof *b);
    b->pid = pid;
    b->fs = fs;
    b->uid = uid;
    str_copy(b->comm, sizeof b->comm, comm);
    return b;
}

int busy_scan(drvfs_t *fs, int nfs, drvuse_t *out, int max, int *denied)
{
    int n = 0;
    *denied = 0;
    for (int i = 0; i < nfs; i++) {
        fs[i].nproc = fs[i].more = fs[i].swap = fs[i].loops = fs[i].nested = 0;
        fs[i].lastp = 0;
    }
    /* other file systems mounted inside one: it can't go before they do */
    for (int j = 0; j < nfs; j++) {
        if (!strcmp(fs[j].mount, "/"))
            continue;
        char parent[sizeof fs[0].mount];
        str_copy(parent, sizeof parent, fs[j].mount);
        char *sl = strrchr(parent, '/');
        if (!sl)
            continue;
        if (sl == parent)
            sl[1] = '\0';
        else
            *sl = '\0';
        int k = owner(fs, nfs, parent);
        if (k >= 0 && k != j)
            fs[k].nested++;
    }
    /* swap files */
    char path[PATHBUF], buf[4096];
    sfmt(path, sizeof path, "%s/swaps", g_procfs);
    FILE *f = fopen(path, "re");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            char name[PATHBUF];
            if (sscanf(buf, "%511s", name) == 1 && name[0] == '/' && strncmp(name, "/dev/", 5)) {
                int k = owner(fs, nfs, name);
                if (k >= 0)
                    fs[k].swap++;
            }
        }
        fclose(f);
    }
    /* loop devices made from a file on it (a mounted disk image) */
    sfmt(path, sizeof path, "%s/block", g_sysfs);
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "loop", 4))
            continue;
        char p[PATHBUF], bf[PATHBUF];
        sfmt(p, sizeof p, "%s/block/%s/loop/backing_file", g_sysfs, e->d_name);
        if (read_line(p, bf, sizeof bf) == 0 && bf[0] == '/') {
            int k = owner(fs, nfs, bf);
            if (k >= 0)
                fs[k].loops++;
        }
    }
    if (d)
        closedir(d);

    /* every process */
    d = opendir(g_procfs);
    while (d && (e = readdir(d)) != NULL) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0)
            continue;
        char base[PATHBUF], comm[24] = "?", l[PATHBUF];
        sfmt(base, sizeof base, "%s/%ld", g_procfs, pid);
        sfmt(path, sizeof path, "%s/comm", base);
        read_line(path, comm, sizeof comm);
        struct stat st;
        uid_t uid = stat(base, &st) == 0 ? st.st_uid : 0;
        /* working folder, root, program */
        static const char *const what[] = { "cwd", "root", "exe" };
        int seen_any = 0;
        for (int w = 0; w < 3; w++) {
            sfmt(path, sizeof path, "%s/%s", base, what[w]);
            if (link_of(path, l, sizeof l)) {
                if (errno == EACCES && !w)
                    seen_any = -1;
                continue;
            }
            int k = owner(fs, nfs, l);
            if (k < 0 || (w == 1 && !strcmp(l, "/")))
                continue;
            drvuse_t *b = rec(fs, out, max, &n, (pid_t)pid, k, comm, uid);
            if (b)
                *(w == 0 ? &b->cwd : w == 1 ? &b->root : &b->exe) = 1;
        }
        if (seen_any < 0) /* not yours: its folder and files can't be read */
            (*denied)++;
        /* open files */
        sfmt(path, sizeof path, "%s/fd", base);
        DIR *fd = opendir(path);
        struct dirent *g;
        while (fd && (g = readdir(fd)) != NULL) {
            if (g->d_name[0] == '.')
                continue;
            char fp[PATHBUF];
            sfmt(fp, sizeof fp, "%s/fd/%s", base, g->d_name);
            if (link_of(fp, l, sizeof l))
                continue;
            int k = owner(fs, nfs, l);
            if (k < 0)
                continue;
            drvuse_t *b = rec(fs, out, max, &n, (pid_t)pid, k, comm, uid);
            if (!b)
                continue;
            if (!b->nfiles++)
                str_copy(b->file, sizeof b->file, l);
        }
        if (fd)
            closedir(fd);
        /* files mapped in: its program, libraries, a memory-mapped file */
        sfmt(path, sizeof path, "%s/maps", base);
        f = fopen(path, "re");
        char last[PATHBUF] = "";
        while (f && fgets(buf, sizeof buf, f)) {
            char *sl = strchr(buf, '/');
            if (!sl)
                continue;
            sl[strcspn(sl, "\n")] = '\0';
            if (!strcmp(sl, last)) /* the same file in a row */
                continue;
            str_copy(last, sizeof last, sl);
            char *dl = strstr(sl, " (deleted)");
            if (dl && !dl[10])
                *dl = '\0';
            int k = owner(fs, nfs, sl);
            if (k < 0)
                continue;
            drvuse_t *b = rec(fs, out, max, &n, (pid_t)pid, k, comm, uid);
            if (b && !b->maps++ && !b->file[0])
                str_copy(b->file, sizeof b->file, sl);
        }
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    for (int i = 0; i < n; i++)
        fs[out[i].fs].nproc++;
    return n;
}
