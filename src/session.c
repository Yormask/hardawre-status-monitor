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
/* session.c - crash detection across reboots.
 *
 * While hsm runs it keeps a small file in ~/.local/state/hsm/sessions with
 * the boot it runs in and a "last seen" time, refreshed every 15 seconds and
 * deleted on a normal exit. A file left behind at the next start means the
 * session did not end normally:
 *   - written during an earlier boot: the computer went down while hsm was
 *     running (crash, freeze, power cut, forced power-off);
 *   - same boot, but that hsm is gone: it was killed or crashed.
 * The kernel's own crash report from the previous boot, when the firmware
 * kept one (pstore), is shown too. */
#define _GNU_SOURCE
#include "log.h"
#include "watch.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BEAT_EVERY 15.0       /* seconds between "still alive" updates */
#define PSTORE_MAX_AGE 30     /* days: older crash reports are not news */
#define HISTORY_MAX 50        /* lines kept in the history file */

static struct {
    char dir[PATHBUF], path[PATHBUF], boot[48], mode[16];
    int fd;
    double start, next;
} S = { .fd = -1 };

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void when(double t, char *out, size_t n, const char *fmt)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, n, fmt, &tm);
}

const char *session_dir(void)
{
    if (!S.dir[0])
        sfmt(S.dir, sizeof S.dir, "%s/sessions", config_state_dir());
    return S.dir;
}

/* ---------------------------------------------------------- our session */

static int write_state(void)
{
    char b[256];
    /* fixed widths: the file never shrinks, so rewriting it in place is enough */
    int n = sfmt(b, sizeof b, "boot_id=%-40s\npid=%-10d\nmode=%-8s\nstarted=%-14.0f\nalive=%-14.0f\n", S.boot,
                 (int)getpid(), S.mode, S.start, wall());
    if (n <= 0 || pwrite(S.fd, b, (size_t)n, 0) != n)
        return -1;
    return fdatasync(S.fd);
}

static void create_own(void)
{
    int who = config_as_user();
    if (who < 0)
        return;
    if (config_mkdir_p(session_dir()) == 0) {
        sfmt(S.path, sizeof S.path, "%s/%d.session", session_dir(), (int)getpid());
        unlink(S.path); /* a stale file of an earlier hsm with the same PID (already reported) */
        S.fd = open(S.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    }
    config_as_root(who);
    if (S.fd >= 0 && write_state() != 0) {
        close(S.fd);
        S.fd = -1;
    }
}

void session_tick(void)
{
    if (S.fd < 0 || now_sec() < S.next)
        return;
    S.next = now_sec() + BEAT_EVERY;
    write_state();
}

void session_end(void)
{
    if (S.fd < 0)
        return;
    close(S.fd);
    S.fd = -1;
    int who = config_as_user();
    if (who >= 0) {
        unlink(S.path);
        config_as_root(who);
    }
}

/* ------------------------------------------------------- earlier sessions */

typedef struct {
    char boot[48], mode[16];
    long pid;
    double started, alive;
} sess_t;

static int parse_session(int dfd, const char *name, sess_t *se)
{
    memset(se, 0, sizeof *se);
    int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char b[512];
    ssize_t n = read(fd, b, sizeof b - 1);
    struct stat st;
    if (fstat(fd, &st) == 0)
        se->alive = (double)st.st_mtime; /* in case the file is damaged */
    close(fd);
    if (n < 0)
        return -1;
    b[n] = '\0';
    char *cur = b, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *v = eq + 1;
        v[strcspn(v, " ")] = '\0';
        if (!strcmp(line, "boot_id"))
            str_copy(se->boot, sizeof se->boot, v);
        else if (!strcmp(line, "pid"))
            se->pid = atol(v);
        else if (!strcmp(line, "mode"))
            str_copy(se->mode, sizeof se->mode, v);
        else if (!strcmp(line, "started"))
            se->started = atof(v);
        else if (!strcmp(line, "alive") && atof(v) > 0)
            se->alive = atof(v);
    }
    if (se->pid <= 0) { /* name is "<pid>.session" */
        se->pid = atol(name);
    }
    return 0;
}

/* when a process started (wall clock), -1 if it doesn't exist. Always the
 * real /proc: this is about processes, not about the data shown. */
static double pid_started(long pid)
{
    char p[64], b[1024];
    sfmt(p, sizeof p, "/proc/%ld/stat", pid);
    if (read_file(p, b, sizeof b) <= 0)
        return -1;
    char *rp = strrchr(b, ')');
    unsigned long long st = 0;
    if (!rp || sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                      &st) != 1)
        return -1;
    static char sb[8192];
    if (read_file("/proc/stat", sb, sizeof sb) <= 0)
        return -1;
    char *bt = strstr(sb, "\nbtime ");
    long hz = sysconf(_SC_CLK_TCK);
    return bt && hz > 0 ? atof(bt + 7) + (double)st / (double)hz : -1;
}

/* is the hsm that wrote this session file still running? The PID alone is
 * not enough (PIDs are reused): its start time has to match too */
static int hsm_alive(const sess_t *se)
{
    if (se->pid <= 0 || (kill((pid_t)se->pid, 0) != 0 && errno != EPERM))
        return 0;
    double t = pid_started(se->pid);
    if (t > 0 && se->started > 0)
        return se->started >= t - 5 && se->started <= t + 120; /* hsm writes it soon after starting */
    char p[64], comm[32];
    sfmt(p, sizeof p, "/proc/%ld/comm", se->pid);
    return read_line(p, comm, sizeof comm) == 0 && !strcmp(comm, APP_NAME);
}

static void history_add(const char *text)
{
    char p[PATHBUF], ts[32];
    sfmt(p, sizeof p, "%s/unexpected-ends.log", config_state_dir());
    when(wall(), ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    int who = config_as_user();
    if (who < 0)
        return;
    /* keep the newest lines only */
    char *old = NULL;
    size_t cap = 0;
    ssize_t len = read_file_grow(p, &old, &cap);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        if (f) {
            if (len > 0) {
                int lines = 0;
                for (ssize_t i = 0; i < len; i++)
                    lines += old[i] == '\n';
                char *c = old;
                for (int skip = lines - (HISTORY_MAX - 1); skip > 0 && c; skip--) {
                    c = strchr(c, '\n');
                    if (c)
                        c++;
                }
                if (c)
                    fputs(c, f);
            }
            fprintf(f, "%s  %s\n", ts, text);
            fclose(f);
        } else {
            close(fd);
        }
    }
    free(old);
    config_as_root(who);
}

int session_history(char lines[][160], int max)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/unexpected-ends.log", config_state_dir());
    char *b = NULL;
    size_t cap = 0;
    int n = 0;
    if (read_file_grow(p, &b, &cap) > 0) {
        char *ls[HISTORY_MAX + 8], *cur = b, *line;
        int k = 0;
        while ((line = next_line(&cur)) != NULL && k < HISTORY_MAX + 8)
            if (*line)
                ls[k++] = line;
        for (int i = k - 1; i >= 0 && n < max; i--) /* newest first */
            str_copy(lines[n++], 160, ls[i]);
    }
    free(b);
    return n;
}

/* how that hsm was run, for the notice: "", " with --watch" */
static const char *mode_text(const char *mode)
{
    if (!strcmp(mode, "log+watch"))
        return " with --log --watch";
    if (!strncmp(mode, "log", 3))
        return " with --log";
    if (!strcmp(mode, "watch"))
        return " with --watch";
    return "";
}

static void report_session(sys_t *s, const sess_t *se, int other_boot)
{
    char seen[48], day[48], text[200], inc[PATHBUF] = "";
    when(se->alive, seen, sizeof seen, "%H:%M:%S");
    when(se->alive, day, sizeof day, "%a %d %b %Y");
    const char *why = other_boot ? "shutdown" : "killed";
    rec_recover(se->pid, se->alive, why, inc, sizeof inc);

    if (other_boot) {
        sfmt(text, sizeof text, "The last session ended unexpectedly at %s on %s: the computer went down while hsm "
                                "was running", seen, day);
        health_event(&s->health, EV_ERR, "shutdown", "%s", text);
        if (!g_notice.n)
            str_copy(g_notice.title, sizeof g_notice.title, "The last session ended unexpectedly");
        notice_add("The computer went down while hsm was running%s.", mode_text(se->mode));
        notice_add("Last sign of life: %s, %s (checked every 15 s).", seen, day);
        notice_add("It was not shut down normally: a crash, a freeze, a power cut or a forced");
        notice_add("power-off (holding the power button).");
        sfmt(text, sizeof text, "computer went down while hsm was running (%s); last seen %s %s", se->mode,
             day, seen);
    } else {
        sfmt(text, sizeof text, "hsm (PID %ld) stopped unexpectedly at %s: killed or crashed", se->pid, seen);
        health_event(&s->health, EV_WARN, "shutdown", "%s", text);
        if (!g_notice.n)
            str_copy(g_notice.title, sizeof g_notice.title, "hsm stopped unexpectedly last time");
        notice_add("The previous hsm (PID %ld, run%s) stopped at %s, %s,", se->pid,
                   *mode_text(se->mode) ? mode_text(se->mode) : " on screen", seen, day);
        notice_add("without closing: it was killed (kill -9, out of memory) or it crashed.");
        sfmt(text, sizeof text, "hsm (PID %ld, %s) killed or crashed; last seen %s %s", se->pid, se->mode, day, seen);
    }
    history_add(text);
    if (inc[0]) { /* the folder is the usual log folder: the name is enough (v opens it) */
        const char *b = strrchr(inc, '/');
        notice_add("The minutes before it, saved from the flight recorder:");
        notice_add("  %s", b ? b + 1 : inc);
        str_copy(g_notice.log, sizeof g_notice.log, inc);
    }
}

static void check_sessions(sys_t *s)
{
    int dfd = open(session_dir(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0)
        return;
    int fd2 = dup(dfd);
    DIR *d = fd2 >= 0 ? fdopendir(fd2) : NULL;
    if (!d) {
        if (fd2 >= 0)
            close(fd2);
        close(dfd);
        return;
    }
    /* oldest first, so the newest ends up on top of the event list */
    struct ent {
        char name[64];
        sess_t se;
    } *E = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 9 || l >= 64 || strcmp(e->d_name + l - 8, ".session"))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            struct ent *ne = realloc(E, (size_t)cap * sizeof *E);
            if (!ne)
                break;
            E = ne;
        }
        str_copy(E[n].name, sizeof E[n].name, e->d_name);
        if (parse_session(dfd, e->d_name, &E[n].se) == 0)
            n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) /* insertion sort by last seen */
        for (int j = i; j > 0 && E[j].se.alive < E[j - 1].se.alive; j--) {
            struct ent t = E[j];
            E[j] = E[j - 1];
            E[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        sess_t *se = &E[i].se;
        int other_boot = strcmp(se->boot, S.boot) != 0;
        if (!other_boot && (se->pid == (long)getpid() || hsm_alive(se)))
            continue; /* another hsm that is running right now */
        report_session(s, se, other_boot);
        int who = config_as_user();
        if (who >= 0) {
            unlinkat(dfd, E[i].name, 0);
            config_as_root(who);
        }
    }
    free(E);
    close(dfd);
}

/* --------------------------------------------------------------- pstore */

/* the most telling line of a kernel crash log */
static int crash_line(const char *text, char *out, size_t n)
{
    static const char *const keys[] = { "Kernel panic", "BUG:", "Oops", "general protection fault",
                                        "hard LOCKUP", "soft lockup", "watchdog:", "Machine check",
                                        "RIP:" };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const char *p = strstr(text, keys[k]);
        if (!p)
            continue;
        const char *b = p, *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - b) : strlen(b);
        if (l >= n)
            l = n - 1;
        memcpy(out, b, l);
        out[l] = '\0';
        for (char *q = out; *q; q++)
            if ((unsigned char)*q < 32)
                *q = ' ';
        return 1;
    }
    return 0;
}

static int seen_before(const char *list, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = list; p && (p = strstr(p, key)) != NULL; p += kl)
        if ((p == list || p[-1] == '\n') && (p[kl] == '\n' || !p[kl]))
            return 1;
    return 0;
}

typedef struct {
    char path[PATHBUF], key[PATHBUF + 24];
    double t;
} crash_t;

static void pstore_scan(const char *dir, int depth, crash_t *C, int *n, int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < max) {
        if (e->d_name[0] == '.')
            continue;
        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && depth == 0) {
            pstore_scan(p, 1, C, n, max); /* systemd-pstore: one folder per crash */
        } else if (S_ISREG(st.st_mode) && !strncmp(e->d_name, "dmesg", 5)) {
            crash_t *c = &C[(*n)++];
            str_copy(c->path, sizeof c->path, p);
            sfmt(c->key, sizeof c->key, "%s %lld", p, (long long)st.st_mtime);
            c->t = (double)st.st_mtime;
        }
    }
    closedir(d);
}

static void check_pstore(sys_t *s)
{
    char live[PATHBUF], seenp[PATHBUF];
    sfmt(live, sizeof live, "%s/fs/pstore", g_sysfs);
    sfmt(seenp, sizeof seenp, "%s/pstore-seen", config_state_dir());
    const char *archive = secure_getenv("HSM_PSTORE");
    if (!archive || !*archive)
        archive = "/var/lib/systemd/pstore"; /* where systemd-pstore moves them at boot */
    static crash_t C[64];
    int n = 0;
    pstore_scan(live, 0, C, &n, 64);
    pstore_scan(archive, 0, C, &n, 64);
    if (!n)
        return;
    char *seen = NULL;
    size_t cap = 0;
    if (read_file_grow(seenp, &seen, &cap) < 0) {
        free(seen);
        seen = NULL;
    }
    double now = wall();
    int reported = 0, any_new = 0;
    for (int i = 0; i < n; i++) {
        if (seen && seen_before(seen, C[i].key))
            continue;
        any_new = 1;
        if (now - C[i].t > PSTORE_MAX_AGE * 86400.0)
            continue;
        /* the parts of one crash share a time: report it once */
        int dup = 0;
        for (int j = 0; j < i && !dup; j++)
            dup = (!seen || !seen_before(seen, C[j].key)) && C[j].t > C[i].t - 5 && C[j].t < C[i].t + 5;
        if (dup)
            continue;
        char *text = NULL, line[160] = "", t[48];
        size_t tc = 0;
        if (read_file_grow(C[i].path, &text, &tc) > 0)
            crash_line(text, line, sizeof line);
        free(text);
        when(C[i].t, t, sizeof t, "%a %d %b %H:%M");
        health_event(&s->health, EV_ERR, "crash", "Kernel crash report from %s: %s", t,
                     line[0] ? line : "(see the file)");
        if (!g_notice.n)
            str_copy(g_notice.title, sizeof g_notice.title, "The kernel crashed");
        if (reported++ < 2) {
            notice_add("Kernel crash report from %s, saved by the firmware (pstore):", t);
            if (line[0])
                notice_add("  %s", line);
            notice_add("  %s", C[i].path);
        }
    }
    if (any_new) { /* remember them, so each crash is reported once */
        int who = config_as_user();
        if (who >= 0 && config_mkdir_p(config_state_dir()) == 0) {
            int fd = open(seenp, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd >= 0) {
                for (int i = 0; i < n; i++)
                    if (!seen || !seen_before(seen, C[i].key))
                        dprintf(fd, "%s\n", C[i].key);
                close(fd);
            }
        }
        if (who >= 0)
            config_as_root(who);
    }
    free(seen);
}

/* ----------------------------------------------------------------- start */

void session_start(sys_t *s, const char *mode)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/sys/kernel/random/boot_id", g_procfs);
    if (read_line(p, S.boot, sizeof S.boot))
        str_copy(S.boot, sizeof S.boot, "unknown");
    str_copy(S.mode, sizeof S.mode, mode);
    S.start = wall();
    check_sessions(s);
    check_pstore(s);
    create_own();
    S.next = now_sec() + BEAT_EVERY;
}
