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
/* alert.c - alerts: a desktop notification (and / or the terminal bell, or
 * a command of your own) when a limit is crossed - the CPU gets too hot or
 * throttles, a disk becomes very slow, memory runs low - or a problem is
 * found (kernel errors, out-of-memory kills, failing drives).
 *
 * Notifications go through notify-send (libnotify), run as the logged-in
 * user even when hsm itself runs with sudo, so they show up on that user's
 * desktop. Each kind of alert waits 5 minutes before it repeats. */
#define _GNU_SOURCE
#include "log.h"
#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define COOLDOWN 300.0 /* seconds before the same alert repeats */
#define TEMP_FOR 5.0   /* the CPU has to stay above the limit this long */
#define DISK_FOR 10.0  /* ... a disk this slow this long */
#define MEM_FOR 5.0
#define GSLOW_FOR 180.0 /* s of the GPU slowing itself from heat (short breaks don't reset it) */

extern char **environ;

enum { AK_TEMP, AK_THROTTLE, AK_DISK, AK_MEM, AK_FULL, AK_PROBLEM, AK_GTEMP, AK_GSLOW, AK_COUNT };

static struct {
    unsigned long seen; /* events already looked at */
    double temp_since, mem_since, disk_since[MAX_DISKS];
    int temp_armed, mem_armed, disk_armed[MAX_DISKS];
    double thr_seen;
    /* GPUs: above the temperature limit since, slowed by heat (seconds, leaking away when not) */
    double gtemp_since[MAX_GPUS], gslow_s[MAX_GPUS], gslow_start[MAX_GPUS], gtick;
    int gtemp_armed[MAX_GPUS], gslow_armed[MAX_GPUS];
    double sent[AK_COUNT];
    char sent_cat[8][12];
    double sent_cat_t[8];
    int warned_missing;
    int reaper;
    struct {
        char mount[128];
        int armed, seen;
    } fs[MAX_FS * 2];
    int nfs;
} A;

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void alert_init(sys_t *s)
{
    A.seen = s->health.ev_seq; /* the kernel log from before we started is not news */
    A.temp_armed = A.mem_armed = 1;
    for (int i = 0; i < MAX_DISKS; i++)
        A.disk_armed[i] = 1;
    for (int i = 0; i < MAX_GPUS; i++)
        A.gtemp_armed[i] = A.gslow_armed[i] = 1;
    A.thr_seen = s->health.thr_last;
}

/* ---------------------------------------------------------- spawning */

static const char *find_exe(const char *name)
{
    static char p[PATHBUF];
    static const char *const dirs[] = { "/usr/bin", "/bin", "/usr/local/bin" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        sfmt(p, sizeof p, "%s/%s", dirs[i], name);
        if (access(p, X_OK) == 0)
            return p;
    }
    return NULL;
}

/* Run argv[0] in the background, as uid/gid when drop is set, with envp
 * (NULL = our own environment). The child is never waited for: SIGCHLD is
 * set up so that finished children don't linger as zombies. */
static int spawn(char *const argv[], char *const envp[], int drop, uid_t uid, gid_t gid)
{
    if (!A.reaper) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = SA_NOCLDWAIT;
        sigaction(SIGCHLD, &sa, NULL);
        A.reaper = 1;
    }
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536)
        maxfd = 65536;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        return 0;
    /* child: nothing here may touch the terminal or the screen */
    int nul = open("/dev/null", O_RDWR);
    if (nul >= 0) {
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(nul, 2);
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3, ~0U, 0) != 0)
#endif
        for (long fd = 3; fd < maxfd; fd++)
            close((int)fd);
    if (drop) {
        if (setgroups(1, &gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0 || getuid() != uid)
            _exit(127);
    }
    signal(SIGPIPE, SIG_DFL); /* hsm ignores it; the program we start shouldn't inherit that */
    setsid();
    execve(argv[0], argv, envp ? envp : environ);
    _exit(127);
}

/* the environment notify-send needs to reach uid's desktop session */
typedef struct {
    char bus[96], run[64], home[PATHBUF], user[64], lang[64];
    char *env[8];
} env_t;

static void user_env(env_t *e, uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    int k = 0;
    sfmt(e->bus, sizeof e->bus, "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", (unsigned)uid);
    sfmt(e->run, sizeof e->run, "XDG_RUNTIME_DIR=/run/user/%u", (unsigned)uid);
    sfmt(e->home, sizeof e->home, "HOME=%s", pw ? pw->pw_dir : "/");
    sfmt(e->user, sizeof e->user, "USER=%s", pw ? pw->pw_name : "");
    const char *l = getenv("LANG");
    sfmt(e->lang, sizeof e->lang, "LANG=%s", l && strlen(l) < 40 ? l : "C.UTF-8");
    e->env[k++] = e->bus;
    e->env[k++] = e->run;
    e->env[k++] = e->home;
    e->env[k++] = e->user;
    e->env[k++] = e->lang;
    e->env[k++] = (char *)"PATH=/usr/local/bin:/usr/bin:/bin";
    e->env[k] = NULL;
}

static int has_bus(uid_t uid)
{
    char p[64];
    struct stat st;
    sfmt(p, sizeof p, "/run/user/%u/bus", (unsigned)uid);
    return stat(p, &st) == 0 && S_ISSOCK(st.st_mode);
}

/* a desktop notification for the person using hsm; returns 0 when sent */
static int desktop(int level, const char *title, const char *body)
{
    const char *exe = find_exe("notify-send");
    if (!exe)
        return -1;
    char x[PATHBUF], t[160], b[400];
    str_copy(x, sizeof x, exe);
    str_copy(t, sizeof t, title);
    str_copy(b, sizeof b, body);
    char *argv[] = { x, (char *)"-a", (char *)APP_NAME, (char *)"-u", (char *)(level >= EV_ERR ? "critical" : "normal"),
                     (char *)"-i", (char *)(level >= EV_ERR ? "dialog-error" : "dialog-warning"), t, b, NULL };
    uid_t uid;
    gid_t gid;
    env_t e;
    if (geteuid() != 0 || getuid() != 0) { /* a normal user: our own session */
        if (getenv("DBUS_SESSION_BUS_ADDRESS"))
            return spawn(argv, NULL, 0, 0, 0);
        user_env(&e, getuid());
        return has_bus(getuid()) ? spawn(argv, e.env, 0, 0, 0) : -1;
    }
    if (config_user_ids(&uid, &gid)) { /* sudo: the user who ran it */
        user_env(&e, uid);
        return spawn(argv, e.env, 1, uid, gid);
    }
    if (getenv("DBUS_SESSION_BUS_ADDRESS")) /* root in its own desktop session */
        return spawn(argv, NULL, 0, 0, 0);
    /* root without sudo (a system service): everyone logged in to a desktop */
    DIR *d = opendir("/run/user");
    int sent = 0;
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) {
        char *end;
        unsigned long u = strtoul(de->d_name, &end, 10);
        if (*end || end == de->d_name || !has_bus((uid_t)u))
            continue;
        struct passwd *pw = getpwuid((uid_t)u);
        if (!pw)
            continue;
        user_env(&e, (uid_t)u);
        if (spawn(argv, e.env, u != 0, (uid_t)u, pw->pw_gid) == 0)
            sent++;
    }
    if (d)
        closedir(d);
    return sent ? 0 : -1;
}

/* alert_command from the config file: $1 title, $2 details, $3 warning/error */
static void command(const char *cmd, int level, const char *title, const char *body)
{
    char c[PATHBUF], t[160], b[400];
    str_copy(c, sizeof c, cmd);
    str_copy(t, sizeof t, title);
    str_copy(b, sizeof b, body);
    char *argv[] = { (char *)"/bin/sh", (char *)"-c", c, (char *)APP_NAME, t, b,
                     (char *)(level >= EV_ERR ? "error" : "warning"), NULL };
    uid_t uid;
    gid_t gid;
    if (geteuid() == 0 && config_user_ids(&uid, &gid)) { /* run it as the user, not as root */
        env_t e;
        user_env(&e, uid);
        spawn(argv, e.env, 1, uid, gid);
    } else {
        spawn(argv, NULL, 0, 0, 0);
    }
}

static void deliver(ui_state_t *u, int level, const char *title, const char *body, int tui)
{
    int m = u->alert_notify;
    if (m == ALERT_DESKTOP || m == ALERT_BOTH) {
        if (desktop(level, title, body) != 0) {
            if (!A.warned_missing && !find_exe("notify-send")) {
                A.warned_missing = 1;
                log_note(1, "Desktop alerts need notify-send (package libnotify): using the terminal bell");
            }
            m = ALERT_BELL;
        }
    }
    if (m == ALERT_BELL || m == ALERT_BOTH)
        u->bell = 1;
    if (u->alert_command[0])
        command(u->alert_command, level, title, body);
    if (tui) {
        ui_message(u, 1, "Alert: %s: %s", title, body);
    } else {
        char ts[16];
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);
        printf("%s  alert: %s: %s\n", ts, title, body);
        fflush(stdout);
    }
}

void alert_test(ui_state_t *u, int tui)
{
    int was = u->alert_notify;
    if (was == ALERT_OFF)
        u->alert_notify = ALERT_DESKTOP;
    deliver(u, EV_WARN, "hsm test alert", "Alerts work: this is how hsm tells you when something goes wrong.", tui);
    u->alert_notify = was;
}

/* ------------------------------------------------------------- checks */

static int ready(int kind, double now)
{
    if (A.sent[kind] > 0 && now - A.sent[kind] < COOLDOWN)
        return 0;
    A.sent[kind] = now;
    return 1;
}

static int cat_ready(const char *cat, double now)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(A.sent_cat[i], cat)) {
            slot = i;
            break;
        }
        if (A.sent_cat_t[i] < A.sent_cat_t[oldest])
            oldest = i;
    }
    if (slot >= 0 && now - A.sent_cat_t[slot] < COOLDOWN)
        return 0;
    if (slot < 0) {
        slot = oldest;
        str_copy(A.sent_cat[slot], sizeof A.sent_cat[slot], cat);
    }
    A.sent_cat_t[slot] = now;
    return 1;
}

static const char *problem_title(const event_t *e)
{
    if (!strcmp(e->cat, "freeze"))
        return "System froze";
    if (!strcmp(e->cat, "fan"))
        return "Fan stopped";
    if (!strcmp(e->cat, "memory"))
        return "Memory problem";
    if (!strcmp(e->cat, "disk"))
        return "Disk problem";
    if (!strcmp(e->cat, "thermal"))
        return "Too hot";
    if (!strcmp(e->cat, "gpu"))
        return "GPU problem";
    if (!strcmp(e->cat, "crash") || !strcmp(e->cat, "shutdown"))
        return "Crash";
    if (!strcmp(e->cat, "pcie"))
        return "PCIe error";
    return "Kernel error";
}

void alert_tick(sys_t *s, ui_state_t *u, int tui)
{
    health_t *h = &s->health;
    double now = wall(), mono = now_sec();
    int on = u->alert_notify != ALERT_OFF || u->alert_command[0];
    char who[80], body[300];

    /* problems: new errors in the event list */
    int nerr = 0;
    const event_t *first = NULL;
    for (int k = h->nev - 1; k >= 0; k--) {
        const event_t *e = health_event_at(h, k);
        if (e->seq <= A.seen || e->level < EV_ERR)
            continue;
        if (!first)
            first = e;
        nerr++;
    }
    A.seen = h->ev_seq;
    if (on && u->alert_problems && first && cat_ready(first->cat, now)) {
        if (nerr > 1)
            sfmt(body, sizeof body, "%s (and %d more)", first->text, nerr - 1);
        else
            str_copy(body, sizeof body, first->text);
        deliver(u, EV_ERR, problem_title(first), body, tui);
    }

    /* throttling */
    if (h->thr_last > A.thr_seen) {
        A.thr_seen = h->thr_last;
        if (on && u->alert_throttle && ready(AK_THROTTLE, now)) {
            health_blame(&s->procs, BLAME_CPU, who, sizeof who);
            if (s->cpu.temp_c >= 0)
                sfmt(body, sizeof body, "The CPU slowed down to protect itself from heat (%.0f °C)%s.",
                     s->cpu.temp_c, who);
            else
                sfmt(body, sizeof body, "The CPU slowed down to protect itself from heat%s.", who);
            deliver(u, EV_WARN, "CPU throttled", body, tui);
        }
    }

    /* CPU temperature */
    int lim = u->alert_cpu_temp;
    double t = s->cpu.temp_c;
    if (lim > 0 && t >= lim) {
        if (!A.temp_since)
            A.temp_since = mono;
        if (A.temp_armed && mono - A.temp_since >= TEMP_FOR) {
            A.temp_armed = 0;
            health_blame(&s->procs, BLAME_CPU, who, sizeof who);
            health_event(h, EV_WARN, "thermal", "CPU at %.0f °C, above the alert limit of %d °C%s", t, lim, who);
            A.seen = h->ev_seq;
            if (on && ready(AK_TEMP, now)) {
                char ti[48];
                sfmt(ti, sizeof ti, "CPU at %.0f °C", t);
                sfmt(body, sizeof body, "Above your limit of %d °C%s.", lim, who);
                deliver(u, EV_WARN, ti, body, tui);
            }
        }
    } else {
        A.temp_since = 0;
        if (lim <= 0 || t < lim - 5) /* cooled down properly: may alert again */
            A.temp_armed = 1;
    }

    /* GPUs: too hot, or slowing themselves down from heat for minutes */
    double gdt = A.gtick > 0 ? CLAMP(mono - A.gtick, 0, 10) : 0;
    A.gtick = mono;
    int glim = u->alert_gpu_temp;
    for (int i = 0; i < s->gpus.n && i < MAX_GPUS; i++) {
        const gpu_t *g = &s->gpus.g[i];
        double gt = g->temp_c;
        char gn[16];
        sfmt(gn, sizeof gn, s->gpus.n > 1 ? "GPU%d" : "GPU", i);
        if (glim > 0 && gt >= glim) {
            if (!A.gtemp_since[i])
                A.gtemp_since[i] = mono;
            if (A.gtemp_armed[i] && mono - A.gtemp_since[i] >= TEMP_FOR) {
                A.gtemp_armed[i] = 0;
                health_blame(&s->procs, BLAME_GPU, who, sizeof who);
                health_event(h, EV_WARN, "thermal", "%s at %.0f °C, above the alert limit of %d °C%s", gn, gt, glim, who);
                A.seen = h->ev_seq;
                if (on && ready(AK_GTEMP, now)) {
                    char ti[48];
                    sfmt(ti, sizeof ti, "%s at %.0f °C", gn, gt);
                    sfmt(body, sizeof body, "Above your limit of %d °C%s.", glim, who);
                    deliver(u, EV_WARN, ti, body, tui);
                }
            }
        } else {
            A.gtemp_since[i] = 0;
            if (glim <= 0 || gt < glim - 5) /* cooled down properly (or asleep): may alert again */
                A.gtemp_armed[i] = 1;
        }
        if (!g->reasons_ok)
            continue;
        int heat = (g->slow_now & GS_HEAT) != 0;
        if (heat && A.gslow_s[i] <= 0)
            A.gslow_start[i] = mono;
        A.gslow_s[i] = CLAMP(A.gslow_s[i] + (heat ? gdt : -gdt / 2), 0, 2 * GSLOW_FOR);
        if (A.gslow_s[i] <= 0)
            A.gslow_armed[i] = 1; /* a proper break: may alert again */
        if (u->alert_gpu_slow && A.gslow_armed[i] && A.gslow_s[i] >= GSLOW_FOR) {
            A.gslow_armed[i] = 0;
            int min = (int)((mono - A.gslow_start[i]) / 60 + 0.5);
            char tc[24] = "";
            if (gt >= 0)
                sfmt(tc, sizeof tc, " (%.0f °C)", gt);
            health_blame(&s->procs, BLAME_GPU, who, sizeof who);
            health_event(h, EV_WARN, "gpu", "%s slowed itself down from heat most of the last %d minutes%s%s", gn, min,
                         tc, who);
            A.seen = h->ev_seq;
            if (on && ready(AK_GSLOW, now)) {
                char ti[64];
                sfmt(ti, sizeof ti, "%s slowing down from heat", gn);
                sfmt(body, sizeof body, "Most of the last %d minutes%s%s. It gives up speed to stay cool.", min, tc, who);
                deliver(u, EV_WARN, ti, body, tui);
            }
        }
    }

    /* slow disks: time per request, while it is doing something */
    int dlim = u->alert_disk_ms;
    for (int i = 0; i < s->disks.n && i < MAX_DISKS; i++) {
        disk_t *d = &s->disks.d[i];
        double lat = MAX(d->rd_lat, d->wr_lat);
        int busy = d->rd_iops + d->wr_iops > 0;
        /* requests waiting, the disk "busy", yet none finishing: it stopped answering */
        int stuck = d->inflight > 0 && d->util >= 99 && !busy;
        if (dlim > 0 && ((busy && lat >= dlim) || stuck)) {
            if (!A.disk_since[i])
                A.disk_since[i] = mono;
            h->slow_last = now;
            double secs = mono - A.disk_since[i];
            if (A.disk_armed[i] && secs >= DISK_FOR) {
                A.disk_armed[i] = 0;
                health_blame(&s->procs, BLAME_IO, who, sizeof who);
                if (stuck)
                    health_event(h, EV_WARN, "disk", "%s not responding: requests waiting for %.0f s", d->name, secs);
                else
                    health_event(h, EV_WARN, "disk", "%s very slow: %.0f ms per request for %.0f s%s", d->name,
                                 lat, secs, who);
                A.seen = h->ev_seq;
                if (on && ready(AK_DISK, now)) {
                    char ti[64];
                    sfmt(ti, sizeof ti, stuck ? "Disk %s is not responding" : "Disk %s is very slow", d->name);
                    if (stuck)
                        sfmt(body, sizeof body, "Requests have been waiting for %.0f seconds.", secs);
                    else
                        sfmt(body, sizeof body, "%.0f ms per request for %.0f seconds%s.", lat, secs, who);
                    deliver(u, EV_WARN, ti, body, tui);
                }
            }
        } else if (!busy || lat < dlim / 2.0 || dlim <= 0) {
            A.disk_since[i] = 0;
            A.disk_armed[i] = 1;
        }
    }

    /* memory running low */
    int mlim = u->alert_mem_pct;
    double avail = s->mem.total ? 100.0 * (double)s->mem.avail / (double)s->mem.total : 100;
    if (mlim > 0 && avail < mlim) {
        if (!A.mem_since)
            A.mem_since = mono;
        h->lowmem_last = now;
        if (A.mem_armed && mono - A.mem_since >= MEM_FOR) {
            A.mem_armed = 0;
            char left[16];
            fmt_bytes((double)s->mem.avail, left, sizeof left);
            health_blame(&s->procs, BLAME_MEM, who, sizeof who);
            health_event(h, EV_WARN, "memory", "Memory almost full: %.0f%% (%s) left%s", avail, left, who);
            A.seen = h->ev_seq;
            if (on && ready(AK_MEM, now)) {
                sfmt(body, sizeof body, "Only %.0f%% (%s) of the memory is left%s.", avail, left, who);
                deliver(u, EV_WARN, "Memory almost full", body, tui);
            }
        }
    } else {
        A.mem_since = 0;
        if (mlim <= 0 || avail >= mlim + 3)
            A.mem_armed = 1;
    }

    /* file systems filling up: space, or inodes (how many files it can hold) */
    int flim = u->alert_disk_full;
    for (int j = 0; j < A.nfs; j++)
        A.fs[j].seen = 0;
    for (int i = 0; i < s->disks.nfs; i++) {
        fs_t *f = &s->disks.fs[i];
        int j = 0;
        while (j < A.nfs && strcmp(A.fs[j].mount, f->mount))
            j++;
        if (j == A.nfs) {
            if (A.nfs == MAX_FS * 2)
                continue;
            A.nfs++;
            str_copy(A.fs[j].mount, sizeof A.fs[j].mount, f->mount);
            A.fs[j].armed = 1;
        }
        A.fs[j].seen = 1;
        if (f->readonly || !f->total) /* nothing can fill it */
            continue;
        double pct = f->used + f->avail ? 100.0 * (double)f->used / (double)(f->used + f->avail) : 0;
        double ipct = f->files ? 100.0 * (double)(f->files - f->ffree) / (double)f->files : 0;
        if (flim > 0 && (pct >= flim || ipct >= flim)) {
            h->full_last = now;
            if (!A.fs[j].armed)
                continue;
            A.fs[j].armed = 0;
            char left[16], ti[160];
            fmt_bytes((double)f->avail, left, sizeof left);
            health_blame(&s->procs, BLAME_IO, who, sizeof who);
            if (pct >= flim) {
                health_event(h, EV_WARN, "disk", "%s almost full: %.0f%% used, %s left%s", f->mount, pct, left, who);
                sfmt(body, sizeof body, "%.0f%% used, only %s left%s.", pct, left, who);
            } else {
                health_event(h, EV_WARN, "disk", "%s has used %.0f%% of its inodes (files it can hold)", f->mount,
                             ipct);
                sfmt(body, sizeof body, "%.0f%% of its inodes are used: soon no new files can be made, "
                                        "whatever space is left.", ipct);
            }
            A.seen = h->ev_seq;
            if (on && ready(AK_FULL, now)) {
                sfmt(ti, sizeof ti, "%s almost full", f->mount);
                deliver(u, EV_WARN, ti, body, tui);
            }
        } else if (flim <= 0 || (pct < flim - 2 && ipct < flim - 2)) {
            A.fs[j].armed = 1;
        }
    }
    for (int j = 0; j < A.nfs; j++) /* unmounted */
        if (!A.fs[j].seen)
            A.fs[j--] = A.fs[--A.nfs];
}
