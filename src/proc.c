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

/* proc.c - process table: CPU %, memory, per-process disk I/O */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define PF_KTHREAD 0x00200000u
#define FD_EVERY 5.0   /* seconds between open-file counts of a process */
#define DRM_EVERY 30.0 /* ...and between full looks for GPU clients among them */

void procs_init(procs_t *ps)
{
    /* GPU clients can only exist if there is a DRM device */
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/class/drm", g_sysfs);
    DIR *dd = opendir(p);
    if (dd) {
        struct dirent *e;
        while ((e = readdir(dd)) != NULL)
            if (!strncmp(e->d_name, "renderD", 7) || !strncmp(e->d_name, "card", 4))
                ps->drm = 1;
        closedir(dd);
    }
    ps->hz = sysconf(_SC_CLK_TCK);
    if (ps->hz <= 0)
        ps->hz = 100;
    ps->pagesize = sysconf(_SC_PAGESIZE);
    if (ps->pagesize <= 0)
        ps->pagesize = 4096;
    ps->dir = opendir(g_procfs);
}

void procs_free(procs_t *ps)
{
    for (int i = 0; i < ps->n; i++)
        free(ps->p[i].cmdline);
    for (int i = 0; i < ps->nprev; i++)
        free(ps->prev[i].cmdline);
    free(ps->p);
    free(ps->prev);
    if (ps->dir)
        closedir(ps->dir);
    memset(ps, 0, sizeof *ps);
}

/* One read() is enough for stat/io: both are far below a page. */
static int read_at(int dfd, const char *rel, char *buf, size_t n)
{
    int fd = openat(dfd, rel, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    int e = errno;
    close(fd);
    if (r < 0) {
        errno = e;
        return -1;
    }
    buf[r] = '\0';
    return (int)r;
}

static proc_t *find_prev(procs_t *ps, pid_t pid)
{
    int lo = 0, hi = ps->nprev - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        pid_t m = ps->prev[mid].pid;
        if (m == pid)
            return &ps->prev[mid];
        if (m < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ------------------------------------------------- open files, GPU use */

/* Count the open files of a process. Its GPU (DRM) clients are found by
 * looking where each file points, which costs a readlink() per file, so
 * that only happens when the count changed or every DRM_EVERY seconds. */
static void scan_fds(int dfd, const char *pidname, proc_t *p, procs_t *ps, double now)
{
    char rel[64];
    sfmt(rel, sizeof rel, "%s/fd", pidname);
    p->fd_t = now;
    int fd = openat(dfd, rel, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { /* another user's process (needs root) */
        p->nfds = -1;
        p->ndrm = 0;
        return;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        p->nfds = -1;
        return;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    p->nfds = n;
    if (ps->drm && (p->drm_t <= 0 || n != p->drm_nfds || now - p->drm_t >= DRM_EVERY)) {
        rewinddir(d);
        p->ndrm = 0;
        while ((e = readdir(d)) != NULL && p->ndrm < (int)(sizeof p->drm_fd / sizeof p->drm_fd[0])) {
            if (e->d_name[0] == '.')
                continue;
            char tgt[64];
            ssize_t l = readlinkat(dirfd(d), e->d_name, tgt, sizeof tgt - 1);
            if (l <= 0)
                continue;
            tgt[l] = '\0';
            if (!strncmp(tgt, "/dev/dri/", 9))
                p->drm_fd[p->ndrm++] = atoi(e->d_name);
        }
        p->drm_nfds = n;
        p->drm_t = now;
        if (!p->ndrm)
            p->gpu_primed = 0;
    }
    closedir(d);
}

static int engine_idx(procs_t *ps, const char *name, size_t len)
{
    for (int i = 0; i < ps->ngeng; i++)
        if (strlen(ps->geng[i]) == len && !strncmp(ps->geng[i], name, len))
            return i;
    if (ps->ngeng >= 8 || len >= sizeof ps->geng[0])
        return -1;
    memcpy(ps->geng[ps->ngeng], name, len);
    ps->geng[ps->ngeng][len] = '\0';
    ps->gcap[ps->ngeng] = 1;
    return ps->ngeng++;
}

static unsigned long long mem_value(const char *v) /* "1234 KiB" -> bytes */
{
    char *end;
    unsigned long long x = strtoull(v, &end, 10);
    while (*end == ' ' || *end == '\t')
        end++;
    if (!strncmp(end, "KiB", 3))
        x <<= 10;
    else if (!strncmp(end, "MiB", 3))
        x <<= 20;
    else if (!strncmp(end, "GiB", 3))
        x <<= 30;
    return x;
}

/* clients already counted this sample: the same client shows up again in
 * every process that inherited the file (and in dup'ed descriptors) */
static unsigned long long g_seen[512];
static int g_nseen;

static int client_seen(unsigned long long key)
{
    for (int i = 0; i < g_nseen; i++)
        if (g_seen[i] == key)
            return 1;
    if (g_nseen < 512)
        g_seen[g_nseen++] = key;
    return 0;
}

/* GPU use of one process from the DRM "fdinfo" statistics (Linux 5.19+ for
 * Intel, 5.14+ AMD, also xe, nouveau, msm, panfrost, v3d...): busy time per
 * engine in ns, and memory per region. */
static void gpu_clients(int dfd, const char *pidname, proc_t *p, procs_t *ps, double dt)
{
    unsigned long long ns[8] = { 0 }, res = 0, mem = 0;
    int clients = 0, engines = 0;
    for (int k = 0; k < p->ndrm; k++) {
        char rel[64], buf[4096];
        sfmt(rel, sizeof rel, "%s/fdinfo/%d", pidname, p->drm_fd[k]);
        if (read_at(dfd, rel, buf, sizeof buf) <= 0) {
            p->drm_t = 0; /* closed: look again next time */
            p->fd_t = 0;
            continue;
        }
        char *cur = buf, *line, *cid = NULL, *pdev = NULL;
        for (char *q = buf; (q = strstr(q, "drm-")) != NULL; q++) {
            if (!strncmp(q, "drm-client-id:", 14))
                cid = q + 14;
            else if (!strncmp(q, "drm-pdev:", 9))
                pdev = q + 9;
        }
        if (!cid)
            continue; /* not a client (or a kernel without fdinfo stats) */
        unsigned long long key = strtoull(cid, NULL, 10) * 1000003ULL;
        if (pdev) {
            while (*pdev == ' ' || *pdev == '\t')
                pdev++;
            for (; *pdev && *pdev != '\n'; pdev++)
                key = key * 131 + (unsigned char)*pdev;
        }
        if (client_seen(key))
            continue;
        clients++;
        unsigned long long cres = 0, cmem = 0;
        while ((line = next_line(&cur)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon || strncmp(line, "drm-", 4))
                continue;
            const char *v = colon + 1;
            if (!strncmp(line, "drm-engine-capacity-", 20)) {
                int e = engine_idx(ps, line + 20, (size_t)(colon - line - 20));
                if (e >= 0)
                    ps->gcap[e] = MAX(1.0, strtod(v, NULL));
            } else if (!strncmp(line, "drm-engine-", 11)) {
                int e = engine_idx(ps, line + 11, (size_t)(colon - line - 11));
                if (e >= 0) {
                    ns[e] += strtoull(v, NULL, 10);
                    engines = 1;
                }
            } else if (!strncmp(line, "drm-resident-", 13)) {
                cres += mem_value(v);
            } else if (!strncmp(line, "drm-memory-", 11)) {
                cmem += mem_value(v);
            }
        }
        res += cres;
        mem += cmem;
    }
    if (!clients)
        return;
    ps->gpu_any = 1;
    p->gpu_mem = res ? res : mem;
    if (!engines) /* memory statistics only */
        return;
    double best = 0;
    if (p->gpu_primed && dt > 0)
        for (int e = 0; e < ps->ngeng; e++)
            if (ns[e] > p->gpu_ns[e])
                best = MAX(best, (double)(ns[e] - p->gpu_ns[e]) / (dt * 1e9 * ps->gcap[e]) * 100.0);
    p->gpu = CLAMP(best, 0, 100);
    memcpy(p->gpu_ns, ns, sizeof ns);
    p->gpu_primed = 1;
}

static int cmp_pid(const void *a, const void *b)
{
    pid_t x = ((const proc_t *)a)->pid, y = ((const proc_t *)b)->pid;
    return (x > y) - (x < y);
}

void procs_sample(procs_t *ps, double dt, unsigned long long memtotal)
{
    DIR *d = ps->dir;
    if (!d)
        return;
    rewinddir(d);
    int dfd = dirfd(d);

    /* current -> previous; reuse the old previous array as the new current */
    proc_t *t = ps->prev;
    int tc = ps->prevcap;
    ps->prev = ps->p;
    ps->prevcap = ps->cap;
    ps->nprev = ps->n;
    ps->p = t;
    ps->cap = tc;
    ps->n = 0;
    ps->threads = ps->running = ps->sleeping = ps->diskwait = ps->zombie = ps->stopped = ps->io_denied = 0;

    int sorted = 1;
    pid_t last = 0;
    double now = now_sec();
    g_nseen = 0;
    ps->gpu_any = 0;
    char rel[64], buf[2048];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        sfmt(rel, sizeof rel, "%s/stat", e->d_name);
        if (read_at(dfd, rel, buf, sizeof buf) <= 0)
            continue; /* exited meanwhile */

        /* comm may contain spaces and ')' - it ends at the LAST ')' */
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp || !rp[1])
            continue;

        char state = '?';
        int ppid = 0, prio = 0, nice = 0, thr = 0;
        unsigned flags = 0;
        unsigned long long ut = 0, st = 0, start = 0, vsz = 0;
        long long rss = 0;
        if (sscanf(rp + 2, "%c %d %*d %*d %*d %*d %u %*u %*u %*u %*u %llu %llu %*d %*d %d %d %d %*d %llu %llu %lld",
                   &state, &ppid, &flags, &ut, &st, &prio, &nice, &thr, &start, &vsz, &rss) != 11)
            continue;

        if (ps->n >= ps->cap) {
            int ncap = ps->cap ? ps->cap * 2 : 512;
            proc_t *np = realloc(ps->p, (size_t)ncap * sizeof *np);
            if (!np)
                break;
            ps->p = np;
            ps->cap = ncap;
        }
        proc_t *p = &ps->p[ps->n];
        memset(p, 0, sizeof *p);
        p->pid = pid;
        size_t cl = (size_t)(rp - lp - 1);
        if (cl >= sizeof p->comm)
            cl = sizeof p->comm - 1;
        memcpy(p->comm, lp + 1, cl);
        p->comm[cl] = '\0';
        p->state = state;
        p->ppid = ppid;
        p->prio = prio;
        p->nice = nice;
        p->nthreads = thr;
        p->kthread = (flags & PF_KTHREAD) != 0;
        p->ticks = ut + st;
        p->start = start;
        p->vsize = vsz;
        p->rss = rss > 0 ? (unsigned long long)rss * (unsigned long long)ps->pagesize : 0;

        struct stat sb;
        p->uid = fstatat(dfd, e->d_name, &sb, 0) == 0 ? sb.st_uid : 0;

        proc_t *q = find_prev(ps, pid);
        if (q && q->start != start)
            q = NULL; /* pid was reused by a new process */

        p->nfds = -1;
        p->gpu = -1;
        if (q) { /* carry the open-file / GPU tracking over */
            p->nfds = q->nfds;
            p->fd_t = q->fd_t;
            p->drm_t = q->drm_t;
            p->drm_nfds = q->drm_nfds;
            p->ndrm = q->ndrm;
            memcpy(p->drm_fd, q->drm_fd, sizeof p->drm_fd);
            memcpy(p->gpu_ns, q->gpu_ns, sizeof p->gpu_ns);
            p->gpu_primed = q->gpu_primed;
            p->gpu_mem = q->gpu_mem;
        }
        if (!p->kthread) {
            if (!q) /* spread the rescans of processes found together over FD_EVERY */
                scan_fds(dfd, e->d_name, p, ps, now - (double)(pid % 50) * (FD_EVERY / 50.0));
            else if (now - p->fd_t >= FD_EVERY)
                scan_fds(dfd, e->d_name, p, ps, now);
            if (p->ndrm)
                gpu_clients(dfd, e->d_name, p, ps, q ? dt : 0);
        }

        /* /proc/PID/io is only readable for our own processes unless root;
         * once denied, don't keep retrying for the same process */
        if (!p->kthread) {
            if (!q || q->io_ok) {
                sfmt(rel, sizeof rel, "%s/io", e->d_name);
                if (read_at(dfd, rel, buf, sizeof buf) > 0) {
                    char *r = strstr(buf, "read_bytes:"), *w = strstr(buf, "\nwrite_bytes:");
                    if (r)
                        p->io_r = strtoull(r + 11, NULL, 10);
                    if (w)
                        p->io_w = strtoull(w + 13, NULL, 10);
                    p->io_ok = 1;
                } else {
                    ps->io_denied++;
                }
            } else {
                ps->io_denied++;
            }
        }

        if (q && dt > 0) {
            double dticks = p->ticks >= q->ticks ? (double)(p->ticks - q->ticks) : 0;
            p->cpu = dticks / (double)ps->hz / dt * 100.0;
            if (p->io_ok && q->io_ok) {
                p->rd_bps = p->io_r >= q->io_r ? (double)(p->io_r - q->io_r) / dt : 0;
                p->wr_bps = p->io_w >= q->io_w ? (double)(p->io_w - q->io_w) / dt : 0;
            }
        }
        /* keep the cached command line unless the process exec'd */
        if (q && q->cmdline && !strcmp(q->comm, p->comm)) {
            p->cmdline = q->cmdline;
            q->cmdline = NULL;
        }
        p->mem = memtotal ? 100.0 * (double)p->rss / (double)memtotal : 0;

        ps->threads += thr;
        switch (state) {
        case 'R': ps->running++; break;
        case 'D': ps->diskwait++; break;
        case 'Z': ps->zombie++; break;
        case 'T':
        case 't': ps->stopped++; break;
        default: ps->sleeping++; break;
        }
        if (pid < last)
            sorted = 0;
        last = pid;
        ps->n++;
    }
    if (!sorted)
        qsort(ps->p, (size_t)ps->n, sizeof *ps->p, cmp_pid);

    for (int i = 0; i < ps->nprev; i++) {
        free(ps->prev[i].cmdline);
        ps->prev[i].cmdline = NULL;
    }
}

/* Signal a process, but only if it is still the one that was selected:
 * between the user's choice and now it may have exited and its PID been
 * reused by something else. "start" is its start time from /proc/PID/stat. */
/* still the same process (the PID was not reused by another one)? 0 or ESRCH */
static int same_process(pid_t pid, unsigned long long start)
{
    char path[PATHBUF], buf[2048];
    if (pid <= 0)
        return ESRCH;
    sfmt(path, sizeof path, "%s/%d/stat", g_procfs, (int)pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return ESRCH;
    char *rp = strrchr(buf, ')');
    unsigned long long st = 0;
    if (!rp || !rp[1] ||
        sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &st) != 1)
        return ESRCH;
    return start && st != start ? ESRCH : 0;
}

int proc_send_signal(pid_t pid, unsigned long long start, int sig)
{
    if (pid <= 0)
        return EINVAL;
    int r = same_process(pid, start);
    if (r)
        return r;
    return kill(pid, sig) == 0 ? 0 : errno;
}

/* Set the nice value of every thread of a process (setpriority() on a PID
 * changes one thread only, renice(1) style). 0 or an errno. */
int proc_renice(pid_t pid, unsigned long long start, int nice)
{
    if (pid <= 0 || nice < -20 || nice > 19)
        return EINVAL;
    int r = same_process(pid, start);
    if (r)
        return r;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/%d/task", g_procfs, (int)pid);
    DIR *d = opendir(path);
    int done = 0, err = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        char *end;
        long tid = strtol(e->d_name, &end, 10);
        if (*end || tid <= 0)
            continue;
        if (setpriority(PRIO_PROCESS, (id_t)tid, nice) == 0)
            done++;
        else if (errno != ESRCH)
            err = errno;
    }
    if (d)
        closedir(d);
    if (!done && !err && setpriority(PRIO_PROCESS, (id_t)pid, nice) != 0) /* no task list to read */
        err = errno;
    else if (!done && !err)
        done = 1;
    return done ? 0 : err ? err : ESRCH;
}

const char *proc_cmdline(proc_t *p)
{
    if (p->cmdline)
        return p->cmdline;
    char path[PATHBUF], buf[4096];
    sfmt(path, sizeof path, "%s/%d/cmdline", g_procfs, (int)p->pid);
    ssize_t n = read_file(path, buf, sizeof buf);
    if (n < 0)
        n = 0;
    while (n > 0 && (buf[n - 1] == '\0' || buf[n - 1] == ' '))
        n--;
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0' || buf[i] == '\n' || buf[i] == '\t')
            buf[i] = ' ';
    buf[n] = '\0';
    p->cmdline = strdup(buf);
    return p->cmdline ? p->cmdline : "";
}

const char *uid_name(uid_t uid)
{
    static struct {
        uid_t uid;
        char name[32];
    } cache[512];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].uid == uid)
            return cache[i].name;
    static char tmp[32];
    struct passwd *pw = getpwuid(uid);
    if (pw)
        str_copy(tmp, sizeof tmp, pw->pw_name);
    else
        sfmt(tmp, sizeof tmp, "%u", (unsigned)uid);
    if (n < 512) {
        cache[n].uid = uid;
        str_copy(cache[n].name, sizeof cache[n].name, tmp);
        return cache[n++].name;
    }
    return tmp;
}
