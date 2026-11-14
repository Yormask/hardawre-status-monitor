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
/* ui_pdetail.c - everything about one process (Enter on it): where it
 * comes from, what it uses now and how that changed, whether its memory
 * keeps growing, its open files and its children. x, N and L still act on
 * it; a click on the parent or a child opens that one instead. */
#define _GNU_SOURCE
#include "log.h"
#include "ui_int.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    unsigned long long start;
    /* /proc/PID/status, in bytes (-1 = unknown) */
    long long anon, file, shmem, swap, hwm, vmpeak;
    long long vcsw, nvcsw;
    double vcsw_rate, nvcsw_rate, csw_t;
    char exe[PATHBUF], cwd[PATHBUF], unit[128];
    int exe_err, cwd_err;
    long long fd_soft;
    int oom; /* -1 unknown */
    /* open files by kind, scanned every 2 s */
    int nfile, nsock, npipe, nother, nunk, fd_ok, fd_err;
    double fd_t;
    unsigned long samples; /* sys_t.samples when last read */
    hist_t cpu, mem;       /* since the page opened */
} pd_t;

static pd_t D;
static int g_rows, g_room; /* content rows and screen rows at the last draw, for scrolling */

static proc_t *find(sys_t *s, pid_t pid, unsigned long long start)
{
    for (int i = 0; i < s->procs.n; i++)
        if (s->procs.p[i].pid == pid && (!start || s->procs.p[i].start == start))
            return &s->procs.p[i];
    return NULL;
}

static long long kb_field(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (!p || (p != buf && p[-1] != '\n'))
        return -1;
    long long v;
    return sscanf(p + strlen(key), " %lld", &v) == 1 ? v : -1;
}

static void link_of(pid_t pid, const char *what, char *out, size_t n, int *err)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%d/%s", g_procfs, (int)pid, what);
    ssize_t l = readlink(p, out, n - 1);
    *err = l < 0 ? errno : 0;
    out[l < 0 ? 0 : l] = '\0';
}

static void read_status(proc_t *p, double now)
{
    char path[PATHBUF], buf[4096];
    sfmt(path, sizeof path, "%s/%d/status", g_procfs, (int)p->pid);
    D.anon = D.file = D.shmem = D.swap = D.hwm = D.vmpeak = -1;
    if (read_file(path, buf, sizeof buf) <= 0)
        return;
    long long v;
    if ((v = kb_field(buf, "RssAnon:")) >= 0)
        D.anon = v * 1024;
    if ((v = kb_field(buf, "RssFile:")) >= 0)
        D.file = v * 1024;
    if ((v = kb_field(buf, "RssShmem:")) >= 0)
        D.shmem = v * 1024;
    if ((v = kb_field(buf, "VmSwap:")) >= 0)
        D.swap = v * 1024;
    if ((v = kb_field(buf, "VmHWM:")) >= 0)
        D.hwm = v * 1024;
    if ((v = kb_field(buf, "VmPeak:")) >= 0)
        D.vmpeak = v * 1024;
    long long a = kb_field(buf, "voluntary_ctxt_switches:"), b = kb_field(buf, "nonvoluntary_ctxt_switches:");
    if (a >= 0 && b >= 0) {
        if (D.csw_t > 0 && now > D.csw_t && a >= D.vcsw && b >= D.nvcsw) {
            D.vcsw_rate = (double)(a - D.vcsw) / (now - D.csw_t);
            D.nvcsw_rate = (double)(b - D.nvcsw) / (now - D.csw_t);
        }
        D.vcsw = a;
        D.nvcsw = b;
        D.csw_t = now;
    }
}

static void read_once(proc_t *p)
{
    char path[PATHBUF], buf[2048];
    link_of(p->pid, "exe", D.exe, sizeof D.exe, &D.exe_err);
    link_of(p->pid, "cwd", D.cwd, sizeof D.cwd, &D.cwd_err);
    D.unit[0] = '\0';
    sfmt(path, sizeof path, "%s/%d/cgroup", g_procfs, (int)p->pid);
    if (read_file(path, buf, sizeof buf) > 0) { /* "0::/system.slice/sshd.service": the last part */
        char *cur = buf, *line;
        while ((line = next_line(&cur)) != NULL)
            if (!strncmp(line, "0::/", 4)) {
                const char *sl = strrchr(line, '/');
                str_copy(D.unit, sizeof D.unit, sl && sl[1] ? sl + 1 : "");
            }
    }
    D.fd_soft = -1;
    sfmt(path, sizeof path, "%s/%d/limits", g_procfs, (int)p->pid);
    if (read_file(path, buf, sizeof buf) > 0) {
        char *l = strstr(buf, "Max open files");
        long long soft;
        if (l && sscanf(l + 14, " %lld", &soft) == 1)
            D.fd_soft = soft;
    }
}

static void read_oom(proc_t *p)
{
    char path[PATHBUF];
    long long v;
    sfmt(path, sizeof path, "%s/%d/oom_score", g_procfs, (int)p->pid);
    D.oom = read_ll(path, &v) == 0 ? (int)v : -1;
}

static void scan_fds(proc_t *p, double now)
{
    if (D.fd_t > 0 && now - D.fd_t < 2.0)
        return;
    D.fd_t = now;
    char dir[PATHBUF];
    sfmt(dir, sizeof dir, "%s/%d/fd", g_procfs, (int)p->pid);
    DIR *d = opendir(dir);
    D.fd_ok = d != NULL;
    D.fd_err = d ? 0 : errno;
    if (!d)
        return;
    D.nfile = D.nsock = D.npipe = D.nother = D.nunk = 0;
    struct dirent *e;
    int k = 0;
    while ((e = readdir(d)) != NULL && k < 20000) {
        if (e->d_name[0] == '.')
            continue;
        k++;
        char lp[PATHBUF], t[PATHBUF];
        sfmt(lp, sizeof lp, "%s/%s", dir, e->d_name);
        ssize_t l = readlink(lp, t, sizeof t - 1);
        if (l <= 0) {
            D.nunk++;
            continue;
        }
        t[l] = '\0';
        if (t[0] == '/')
            D.nfile++;
        else if (!strncmp(t, "socket:", 7))
            D.nsock++;
        else if (!strncmp(t, "pipe:", 5))
            D.npipe++;
        else
            D.nother++; /* anon_inode:[eventfd], [eventpoll], ... */
    }
    closedir(d);
}

void pdetail_open(ui_state_t *u, pid_t pid, unsigned long long start)
{
    if (!pid)
        return;
    u->pd_open = 1;
    u->pd_view = u->view;
    u->pd_scroll = 0;
    if (D.pid != pid || D.start != start) {
        memset(&D, 0, sizeof D);
        D.pid = pid;
        D.start = start;
        D.oom = -1;
    }
}

int pdetail_key(ui_state_t *u, int ch)
{
    switch (ch) {
    case 27:
    case '\n':
    case '\r':
    case KEY_ENTER:
    case KEY_LEFT:
        u->pd_open = 0;
        return 1;
    case KEY_UP: u->pd_scroll--; break;
    case KEY_DOWN: u->pd_scroll++; break;
    case KEY_PPAGE: u->pd_scroll -= 10; break;
    case KEY_NPAGE: u->pd_scroll += 10; break;
    case KEY_HOME: u->pd_scroll = 0; break;
    default: return 0; /* x, N, L, q... as anywhere else */
    }
    u->pd_scroll = CLAMP(u->pd_scroll, 0, MAX(0, g_rows - g_room));
    return 1;
}

void pdetail_click(sys_t *s, ui_state_t *u, int pid)
{
    proc_t *p = find(s, (pid_t)pid, 0);
    if (p)
        pdetail_open(u, p->pid, p->start);
}

/* one "Label  text" line; lines above the scroll position are skipped */
typedef struct {
    int y0, y, ymax, x, w, skip, rows;
} pl_t;

static int pl_row(pl_t *c)
{
    c->rows++;
    if (c->skip > 0) {
        c->skip--;
        return -1;
    }
    if (c->y >= c->ymax)
        return -1;
    return c->y++;
}

static int pl_kv(pl_t *c, const char *k, attr_t a, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int pl_kv(pl_t *c, const char *k, attr_t a, const char *fmt, ...)
{
    int y = pl_row(c);
    if (y < 0)
        return -1;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(y, c->x, 11, CP(C_INFO), "%s", k);
    put(y, c->x + 11, c->w - 11, a, "%s", b);
    return y;
}

static void when(double wall, char *out, size_t n)
{
    time_t t = (time_t)wall, now = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, now - t < 20 * 3600 ? "%H:%M:%S" : "%d %b %Y %H:%M", &tm);
}

static const char *no_access(void)
{
    return geteuid() == 0 ? "(the kernel doesn't show it here)" : "(another user's process: needs root)";
}

static const char *state_name(char st)
{
    switch (st) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "waiting for a disk (uninterruptible)";
    case 'Z': return "zombie: ended, its parent hasn't collected it";
    case 'T': return "stopped";
    case 't': return "stopped by a debugger";
    case 'I': return "idle";
    case 'X': return "dead";
    }
    return "?";
}

void pdetail_view(sys_t *s, ui_state_t *u)
{
    int top = 1, H = LINES - 2, W = COLS;
    proc_t *p = find(s, D.pid, D.start);
    if (!p) {
        box_(top, 0, H, W, "Process", "Esc back");
        put(top + 2, 2, W - 4, CP(C_WARN) | A_BOLD, "Process %d has ended.", (int)D.pid);
        put(top + 3, 2, W - 4, A_GRAY, "%s", "Esc, Enter: back to the list");
        u->sel_pid = 0;
        return;
    }
    double now = now_sec();
    /* the keys x, N, L act on what is shown */
    u->sel_pid = p->pid;
    u->sel_start = p->start;
    u->sel_kthread = p->kthread;
    str_copy(u->sel_user, sizeof u->sel_user, uid_name(p->uid));
    str_copy(u->sel_desc, sizeof u->sel_desc, p->kthread ? "" : proc_cmdline(p));
    if (!u->sel_desc[0])
        sfmt(u->sel_desc, sizeof u->sel_desc, "[%s]", p->comm);

    if (D.samples != s->samples) { /* a new sample: new figures */
        if (!D.samples)
            read_once(p);
        D.samples = s->samples;
        read_status(p, now);
        read_oom(p);
        hist_push(&D.cpu, p->cpu);
        hist_push(&D.mem, (double)p->rss);
    }
    scan_fds(p, now);

    char title[96], a[24], b[24], c2[24], d[24];
    sfmt(title, sizeof title, "Process %d %s %s", (int)p->pid, U ? "·" : "-", p->comm);
    box_(top, 0, H, W, title, NULL);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2;
    if (ih < 3)
        return;
    u->pd_scroll = MAX(0, u->pd_scroll);
    pl_t c = { iy, iy, iy + ih, ix, iw, u->pd_scroll, 0 };
    hit_add(iy, ix, ih, iw, HIT_SCROLL, 0, 0);

    /* where it comes from */
    const char *cmd = p->kthread ? "" : proc_cmdline(p);
    if (*cmd) { /* long command lines wrap */
        int y = pl_row(&c);
        if (y >= 0) {
            put(y, ix, 11, CP(C_INFO), "Command");
            int cw = iw - 11, len = (int)strlen(cmd);
            const char *q = cmd;
            for (int line = 0; line < 4 && q < cmd + len; line++) {
                size_t nb = utf8_bytes_for_cols(q, cw);
                if (line && (y = pl_row(&c)) < 0)
                    break;
                put(y, ix + 11, cw, A_BOLD, "%.*s", (int)nb, q);
                q += nb;
                if (!nb)
                    break;
            }
        }
    } else {
        pl_kv(&c, "Command", A_GRAY, "[%s] (a kernel thread)", p->comm);
    }
    if (D.exe[0])
        pl_kv(&c, "Program", A_NORMAL, "%s", D.exe);
    else if (!p->kthread && D.exe_err)
        pl_kv(&c, "Program", A_GRAY, "%s", no_access());
    double started = (double)time(NULL) - (s->cpu.uptime - (double)p->start / (double)s->procs.hz);
    when(started, a, sizeof a);
    fmt_duration((double)time(NULL) - started, b, sizeof b);
    pl_kv(&c, "Started", A_NORMAL, "%s (%s ago) by %s (uid %u)", a, b, uid_name(p->uid), (unsigned)p->uid);
    proc_t *pp = find(s, p->ppid, 0);
    int y = pl_kv(&c, "Parent", A_NORMAL, "%d %s", (int)p->ppid, pp ? pp->comm : p->ppid ? "" : "(none)");
    if (y >= 0 && pp)
        hit_add(y, ix + 11, 1, iw - 11, HIT_PD, pp->pid, 0);
    if (D.unit[0])
        pl_kv(&c, "Service", A_NORMAL, "%s", D.unit);
    if (D.cwd[0])
        pl_kv(&c, "Folder", A_NORMAL, "%s", D.cwd);
    pl_kv(&c, "State", p->state == 'D' || p->state == 'Z' ? CP(C_WARN) | A_BOLD : A_NORMAL, "%c %s %s %d thread%s %s nice %d",
          p->state, state_name(p->state), SEP, p->nthreads, p->nthreads == 1 ? "" : "s", SEP, p->nice);
    pl_row(&c);

    /* what it uses */
    double cpu_s = (double)p->ticks / (double)s->procs.hz, life = (double)time(NULL) - started;
    fmt_duration(cpu_s, a, sizeof a);
    if (cpu_s < 60)
        sfmt(a, sizeof a, "%.1f s", cpu_s);
    pl_kv(&c, "CPU", pct_attr(MIN(p->cpu, 100)) | A_BOLD, "%.1f%% now %s %s used in all %s %.1f%% on average", p->cpu,
          SEP, a, SEP, life > 0 ? 100.0 * cpu_s / life : 0);
    int flagged, busy_min = runaway_minutes(p->pid, p->start, &flagged);
    if (busy_min >= 5) /* a while at a whole core without a break */
        pl_kv(&c, "", flagged ? CP(C_WARN) | A_BOLD : A_NORMAL, "a whole core or more for %d minutes without a break%s",
              busy_min, flagged ? " (see the events)" : "");
    int gy = pl_row(&c), gy2 = pl_row(&c);
    if (gy >= 0 && gy2 == gy + 1) {
        double mx = MAX(100.0, hist_max(&D.cpu, graph_span(iw - 11)));
        graph(gy, ix + 11, 2, MIN(iw - 11, 80), &D.cpu, mx, 0, 0);
        put(gy, ix, 10, A_GRAY, "%s", "since open");
    }
    fmt_bytes((double)p->rss, a, sizeof a);
    char parts[160] = "";
    if (D.anon >= 0) {
        fmt_bytes((double)D.anon, b, sizeof b);
        fmt_bytes((double)MAX(0, D.file), c2, sizeof c2);
        fmt_bytes((double)MAX(0, D.shmem), d, sizeof d);
        sfmt(parts, sizeof parts, " (own %s, files %s, shared %s)", b, c2, d);
    }
    pl_kv(&c, "Memory", A_BOLD, "%s in RAM%s %s %.1f%% of it", a, parts, SEP, p->mem);
    char more[200] = "";
    fmt_bytes((double)p->vsize, b, sizeof b);
    app(more, sizeof more, "virtual %s", b);
    if (D.hwm > 0) {
        fmt_bytes((double)D.hwm, b, sizeof b);
        app(more, sizeof more, "%shighest %s", SEP, b);
    }
    if (D.swap > 0) {
        fmt_bytes((double)D.swap, b, sizeof b);
        app(more, sizeof more, "%sswapped out %s", SEP, b);
    }
    if (D.oom >= 0)
        app(more, sizeof more, "%sOOM score %d (higher goes first when memory runs out)", SEP, D.oom);
    pl_kv(&c, "", A_NORMAL, "%s", more);

    /* memory over the last two hours (leak.c follows processes above 64 MB) */
    float lh[LEAK_MIN];
    int growing;
    double rate;
    int nl = leak_history(p->pid, p->start, lh, LEAK_MIN, &growing, &rate);
    if (nl >= 2) {
        char r[24];
        fmt_bytes(fabs(rate) * 1024 * 1024, r, sizeof r);
        y = pl_kv(&c, "Growth", growing ? CP(C_WARN) | A_BOLD : A_NORMAL, "%s%s/h over the last %d min%s", rate < 0 ? "-" : "+",
                  r, MIN(nl, 30), growing ? ": it only ever grows (a leak, or a cache without limit)" : "");
        int gy3 = pl_row(&c), gy4 = pl_row(&c);
        if (gy3 >= 0 && gy4 == gy3 + 1) {
            int gw = MIN(iw - 11, nl);
            float lo = lh[nl - gw], hi = lh[nl - gw];
            for (int k = nl - gw; k < nl; k++) {
                lo = MIN(lo, lh[k]);
                hi = MAX(hi, lh[k]);
            }
            graph_cols(gy3, ix + 11, 2, gw, lh + nl - gw, lo * 0.9, hi, growing ? CP(C_WARN) : CP(C_IN), -1, 0);
            put(gy3, ix, 10, A_GRAY, "%d min", gw);
        }
    } else if (!p->kthread) {
        pl_kv(&c, "Growth", A_GRAY, "%s",
              (double)p->rss >= 64.0 * 1024 * 1024 ? "followed once a minute; the first figures come in a minute"
                                                   : "followed once it uses 64 MB or more");
    }

    if (p->io_ok) {
        fmt_rate(p->rd_bps, a, sizeof a);
        fmt_rate(p->wr_bps, b, sizeof b);
        fmt_bytes((double)p->io_r, c2, sizeof c2);
        fmt_bytes((double)p->io_w, d, sizeof d);
        pl_kv(&c, "Disk", A_NORMAL, "read %s, write %s %s %s read, %s written in all", a, b, SEP, c2, d);
    } else if (!p->kthread) {
        pl_kv(&c, "Disk", A_GRAY, "%s", no_access());
    }
    if (p->gpu >= 0) {
        fmt_bytes((double)p->gpu_mem, a, sizeof a);
        pl_kv(&c, "GPU", A_NORMAL, "%.1f%% %s %s of GPU memory", p->gpu, SEP, a);
    }
    if (D.fd_ok) {
        int n = D.nfile + D.nsock + D.npipe + D.nother + D.nunk;
        char lim[48] = "";
        if (D.fd_soft > 0)
            sfmt(lim, sizeof lim, " of %lld allowed", D.fd_soft);
        attr_t fa = D.fd_soft > 0 && n >= D.fd_soft * 0.8 ? CP(C_WARN) | A_BOLD : A_NORMAL;
        if (D.nunk == n)
            pl_kv(&c, "Open", fa, "%d%s", n, lim);
        else
            pl_kv(&c, "Open", fa, "%d%s %s %d file%s, %d socket%s, %d pipe%s, %d other", n, lim, SEP, D.nfile,
                  D.nfile == 1 ? "" : "s", D.nsock, D.nsock == 1 ? "" : "s", D.npipe, D.npipe == 1 ? "" : "s",
                  D.nother + D.nunk);
    } else if (!p->kthread && D.fd_err) {
        pl_kv(&c, "Open", A_GRAY, "%s", no_access());
    }
    if (D.csw_t > 0)
        pl_kv(&c, "Switches", A_NORMAL, "%.0f/s waiting for something, %.0f/s taken off the CPU", D.vcsw_rate,
              D.nvcsw_rate);
    if (g_log.nplog && log_proc_find(p->pid, p->start) >= 0)
        pl_kv(&c, "Logging", CP(C_OK) | A_BOLD, "%s", "this process is being logged (L stops)");
    pl_row(&c);

    /* children */
    int nk = 0;
    for (int i = 0; i < s->procs.n; i++)
        nk += s->procs.p[i].ppid == p->pid;
    if (nk) {
        y = pl_row(&c);
        if (y >= 0)
            put(y, ix, iw, A_GRAY | A_BOLD, "Started by it (%d)%s", nk, U ? "   click one to open it" : "");
        for (int i = 0; i < s->procs.n; i++) {
            proc_t *k = &s->procs.p[i];
            if (k->ppid != p->pid)
                continue;
            y = pl_row(&c);
            if (y < 0)
                continue;
            fmt_bytes((double)k->rss, a, sizeof a);
            int xx = ix + 2;
            xx += put(y, xx, 8, A_NORMAL, "%7d ", (int)k->pid);
            xx += put(y, xx, 7, pct_attr(MIN(k->cpu, 100)), "%5.1f%% ", k->cpu);
            xx += put(y, xx, 7, A_NORMAL, "%6s ", a);
            const char *kc = k->kthread ? "" : proc_cmdline(k);
            if (*kc)
                put(y, xx, ix + iw - xx, A_BOLD, "%s", kc);
            else
                put(y, xx, ix + iw - xx, A_GRAY, "[%s]", k->comm);
            hit_add(y, ix, 1, iw, HIT_PD, k->pid, 0);
        }
    }
    g_rows = c.rows;
    g_room = ih;
    if (c.rows > ih) {
        char m[32];
        sfmt(m, sizeof m, " %s more ", U ? "↑↓" : "up/down");
        put(top + H - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}
