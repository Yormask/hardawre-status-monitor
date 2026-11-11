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
#include "ui.h"

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
            "Keys: 1-4 views, s setup, x kill, c/m/d/p/n/t sort, / filter,\n"
            "      b CPU bars, k kernel threads, +/- interval, space pause, ? help, q quit\n\n"
            "Settings are saved in %s\n\n"
            "Run as root (sudo %s) to see CPU power (RAPL), exact Intel GPU load\n"
            "and per-process disk I/O of other users.\n\n"
            "%s. This program comes with ABSOLUTELY NO WARRANTY.\n"
            "It is free software under the GNU GPL v3 or later; see the LICENSE file.\n",
            APP_NAME, APP_LONG_NAME, APP_VERSION, APP_NAME, config_path(), APP_NAME, APP_COPYRIGHT);
}

static void sleep_sec(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    while (nanosleep(&ts, &ts) == -1 && !g_quit)
        ;
}

int main(int argc, char **argv)
{
    double interval = -1; /* -1: use the saved setting */
    int once = 0, ascii = 0, ntop = 15;

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
    ui_init(utf8);

    double next = now_sec() + interval;
    while (!g_quit) {
        ui_draw(&S, &u);
        double now = now_sec();
        int ms = (int)((next - now) * 1000.0);
        timeout(ms < 0 ? 0 : ms);
        int ch = getch();
        if (ch != ERR && ch != KEY_RESIZE && !ui_key(&u, ch))
            break;
        now = now_sec();
        if (next > now + u.interval) /* interval was shortened */
            next = now + u.interval;
        if (now >= next) {
            if (!u.paused)
                sys_sample(&S);
            next = now + u.interval;
        }
    }

    ui_end();
    if (config_changed(&u)) /* remember sort order, interval, CPU bar mode... */
        config_save(&u);
    sys_free(&S);
    return 0;
}
