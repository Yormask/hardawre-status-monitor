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

/* main.c - argument parsing and the main loop */
#define _GNU_SOURCE
#include "log.h"
#include "ui.h"

#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sys_t S; /* big (history buffers, per-CPU arrays): keep it off the stack */
static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
            "%s (%s) %s\nLive monitor for CPU, GPU, memory, power, disk and network activity.\n\n"
            "Usage: %s [options]\n"
            "  -d, --delay SEC   refresh interval in seconds (default: saved setting or 1)\n"
            "  -1, --once        print one plain-text snapshot and exit\n"
            "  -n, --top N       processes listed by --once (default 15)\n"
            "  -a, --ascii       draw with plain ASCII instead of Unicode\n"
            "  -h, --help        show this help\n"
            "  -v, --version     show version\n\n"
            "Logging without the screen (runs until Ctrl+C, kill, or the processes exit):\n"
            "  --log             log the values chosen in the Logging view (5) to a CSV file\n"
            "  --log-pid PID     log one process's usage (can be given up to %d times)\n"
            "  --log-every SEC   seconds between rows (default: the saved setting)\n"
            "  --log-dir DIR     where to write the files (default: %s)\n\n"
            "Keys: 1-5 views, s setup, l log sensors, L log the selected process, x kill,\n"
            "      c/m/d/p/n/t sort, / filter, b CPU bars, k kernel threads,\n"
            "      +/- interval, space pause, ? help, q quit\n\n"
            "Settings are saved in %s\n\n"
            "Run as root (sudo %s) to see CPU power (RAPL), exact Intel GPU load\n"
            "and per-process disk I/O of other users.\n\n"
            "%s. This program comes with ABSOLUTELY NO WARRANTY.\n"
            "It is free software under the GNU GPL v3 or later; see the LICENSE file.\n",
            APP_NAME, APP_LONG_NAME, APP_VERSION, APP_NAME, MAX_PLOG, config_log_dir(NULL), config_path(),
            APP_NAME, APP_COPYRIGHT);
}

static void sleep_sec(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    while (nanosleep(&ts, &ts) == -1 && !g_quit)
        ;
}

static void print_note(void)
{
    if (g_log.note_new) {
        fprintf(stderr, "%s: %s\n", APP_NAME, g_log.note);
        g_log.note_new = 0;
    }
}

static const char *pretty(const char *path)
{
    static char b[PATHBUF];
    config_pretty_path(path, b, sizeof b);
    return b;
}

/* hsm --log / --log-pid: record to CSV files without drawing anything */
static int headless(ui_state_t *u, int sys_log, const pid_t *pids, int npid, double every, const char *dir)
{
    char err[PATHBUF + 64];
    if (dir) {
        if (dir[0] == '/' || dir[0] == '~') {
            str_copy(u->log_dir, sizeof u->log_dir, dir);
        } else {
            char cwd[PATHBUF];
            if (!getcwd(cwd, sizeof cwd)) {
                fprintf(stderr, "%s: %s\n", APP_NAME, strerror(errno));
                return 1;
            }
            sfmt(u->log_dir, sizeof u->log_dir, "%s/%s", cwd, dir);
        }
    }
    if (every <= 0)
        every = u->log_every > 0 ? u->log_every : u->interval;
    str_copy(g_log.dir, sizeof g_log.dir, config_log_dir(u));
    g_log.every = 0; /* a row on every sample: we sample at the log interval */

    sleep_sec(0.25);
    sys_sample(&S);
    log_build_channels(&S, u->log_skip);
    if (sys_log) {
        if (log_start(&S, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", APP_NAME, err);
            return 1;
        }
        printf("Logging %d values every %g s to %s\n", log_count_on(), every, g_log.path);
    }
    for (int i = 0; i < npid; i++) {
        proc_t *p = NULL;
        for (int k = 0; k < S.procs.n && !p; k++)
            if (S.procs.p[k].pid == pids[i])
                p = &S.procs.p[k];
        if (!p) {
            fprintf(stderr, "%s: no process with PID %d\n", APP_NAME, (int)pids[i]);
            continue;
        }
        if (log_proc_start(&S, p->pid, p->start, err, sizeof err) != 0)
            fprintf(stderr, "%s: %s\n", APP_NAME, err);
        else
            printf("Logging PID %d (%s) every %g s to %s\n", (int)p->pid, p->comm, every,
                   g_log.pl[g_log.nplog - 1].path);
    }
    if (!g_log.f && !g_log.nplog) {
        log_close_all();
        return 1;
    }
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);

    double next = now_sec() + every;
    while (!g_quit && (g_log.f || g_log.nplog)) {
        double d = next - now_sec();
        if (d > 0)
            sleep_sec(d);
        if (g_quit)
            break;
        sys_sample(&S);
        log_tick(&S);
        print_note();
        next += every;
        if (next < now_sec()) /* fell behind (suspend, slow disk): don't try to catch up */
            next = now_sec() + every;
    }
    print_note();
    if (g_log.f)
        printf("Saved %lu rows to %s\n", g_log.rows, pretty(g_log.path));
    for (int i = 0; i < g_log.nplog; i++)
        printf("Saved %lu rows to %s\n", g_log.pl[i].rows, pretty(g_log.pl[i].path));
    log_close_all();
    return 0;
}

int main(int argc, char **argv)
{
    double interval = -1; /* -1: use the saved setting */
    int once = 0, ascii = 0, ntop = 15;
    int sys_log = 0, npid = 0;
    pid_t pids[MAX_PLOG];
    double log_every = -1;
    const char *log_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-d") || !strcmp(a, "--delay")) && i + 1 < argc) {
            interval = atof(argv[++i]);
        } else if (!strncmp(a, "--delay=", 8)) {
            interval = atof(a + 8);
        } else if (!strcmp(a, "-1") || !strcmp(a, "--once")) {
            once = 1;
        } else if ((!strcmp(a, "-n") || !strcmp(a, "--top")) && i + 1 < argc) {
            ntop = atoi(argv[++i]);
        } else if (!strcmp(a, "-a") || !strcmp(a, "--ascii")) {
            ascii = 1;
        } else if (!strcmp(a, "--log")) {
            sys_log = 1;
        } else if (!strcmp(a, "--log-pid") && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end || v <= 0 || v > 0x3fffffff) {
                fprintf(stderr, "%s: '%s' is not a process ID\n", APP_NAME, argv[i]);
                return 2;
            }
            if (npid >= MAX_PLOG) {
                fprintf(stderr, "%s: at most %d --log-pid options\n", APP_NAME, MAX_PLOG);
                return 2;
            }
            pids[npid++] = (pid_t)v;
        } else if (!strcmp(a, "--log-every") && i + 1 < argc) {
            log_every = atof(argv[++i]);
            if (log_every < 0.1 || log_every > 86400) {
                fprintf(stderr, "%s: --log-every must be between 0.1 and 86400 seconds\n", APP_NAME);
                return 2;
            }
        } else if (!strcmp(a, "--log-dir") && i + 1 < argc) {
            log_dir = argv[++i];
            if (!*log_dir || strlen(log_dir) >= PATHBUF - 128) {
                fprintf(stderr, "%s: bad --log-dir\n", APP_NAME);
                return 2;
            }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("%s %s\n%s\n"
                   "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
                   "This is free software: you are free to change and redistribute it.\n"
                   "There is NO WARRANTY, to the extent permitted by law.\n",
                   APP_NAME, APP_VERSION, APP_COPYRIGHT);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n\n", APP_NAME, a);
            usage(stderr);
            return 2;
        }
    }
    if (interval != -1 && interval < 0.1)
        interval = 0.1;
    if (interval > 60)
        interval = 60;

    /* Test hooks. secure_getenv() ignores them when the program runs setuid or with
     * file capabilities, so they can't be used to read arbitrary files. */
    const char *e;
    if ((e = secure_getenv("HSM_SYSFS")) && *e)
        g_sysfs = e;
    if ((e = secure_getenv("HSM_PROCFS")) && *e)
        g_procfs = e;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    struct sigaction old;
    if (sigaction(SIGHUP, NULL, &old) == 0 && old.sa_handler != SIG_IGN) /* keep nohup working */
        sigaction(SIGHUP, &sa, NULL);

    setlocale(LC_ALL, "");
    int utf8 = !ascii && !strcmp(nl_langinfo(CODESET), "UTF-8");

    sys_init(&S);

    if (once) {
        sleep_sec(interval > 0 ? interval : 1.0);
        sys_sample(&S);
        report_print(&S, ntop);
        sys_free(&S);
        return 0;
    }
    if (sys_log || npid) {
        static ui_state_t hu;
        config_defaults(&hu);
        config_load(&hu);
        if (interval > 0)
            hu.interval = interval;
        int r = headless(&hu, sys_log, pids, npid, log_every, log_dir);
        sys_free(&S);
        return r;
    }
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s: stdout is not a terminal (use --once for a text snapshot)\n", APP_NAME);
        sys_free(&S);
        return 1;
    }

    sleep_sec(0.25); /* short warm-up so the first frame already shows rates */
    sys_sample(&S);

    static ui_state_t u;
    config_defaults(&u);
    config_load(&u); /* ~/.config/hsm/config, if it exists */
    if (interval > 0)
        u.interval = interval;
    log_build_channels(&S, u.log_skip);
    if (ui_init(utf8) != 0) {
        const char *t = getenv("TERM");
        fprintf(stderr,
                "%s: unknown terminal type '%s'.\n"
                "Try:  TERM=xterm-256color %s\n"
                "(or install your distribution's ncurses-term package)\n",
                APP_NAME, t ? t : "", APP_NAME);
        sys_free(&S);
        return 1;
    }

    double next = now_sec() + u.interval;
    while (!g_quit) {
        ui_draw(&S, &u);
        double now = now_sec();
        int ms = (int)((next - now) * 1000.0);
        timeout(ms < 0 ? 0 : ms);
        int ch = getch();
        if (ch == 27) { /* a lone Esc, or the start of a key sequence we don't know? */
            nodelay(stdscr, TRUE);
            int c2 = getch();
            if (c2 != ERR) { /* more bytes were already waiting: swallow the sequence */
                if (c2 == '[' || c2 == 'O')
                    for (int c3, k = 0; (c3 = getch()) != ERR && k < 16; k++)
                        if (c3 >= 0x40 && c3 <= 0x7e)
                            break;
                ch = ERR;
            }
            nodelay(stdscr, FALSE);
        }
        if (ch != ERR && ch != KEY_RESIZE && !ui_key(&S, &u, ch))
            break;
        now = now_sec();
        if (next > now + u.interval) /* interval was shortened */
            next = now + u.interval;
        if (now >= next) {
            if (!u.paused) {
                sys_sample(&S);
                log_tick(&S);
            }
            next = now + u.interval;
        }
    }

    ui_end();
    if (g_log.f)
        printf("Log saved: %s (%lu rows)\n", pretty(g_log.path), g_log.rows);
    for (int i = 0; i < g_log.nplog; i++)
        printf("Log saved: %s (%lu rows)\n", pretty(g_log.pl[i].path), g_log.pl[i].rows);
    log_close_all();
    if (config_changed(&u)) /* remember sort order, interval, CPU bar mode... */
        config_save(&u);
    sys_free(&S);
    return 0;
}
