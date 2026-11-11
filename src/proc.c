/* proc.c - process table: CPU %, memory, per-process disk I/O */
#define _GNU_SOURCE
#include "collect.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PF_KTHREAD 0x00200000u

void procs_init(procs_t *ps)
{
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
