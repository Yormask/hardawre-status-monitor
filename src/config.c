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
/* config.c - load / save settings in ~/.config/hsm/config
 *
 * Plain "key = value" lines, so the file can also be edited by hand.
 * When hsm runs through sudo, the file of the user who ran sudo is used,
 * and it is read and written with that user's identity (not root's), so
 * a symlink in their home directory can't trick root into touching
 * another file. */
#define _GNU_SOURCE
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const PANEL_KEY[P_COUNT] = { "cpu", "gpu", "memory", "power", "disk", "network", "temps", "fans", "filesystems" };
const char *const PANEL_NAME[P_COUNT] = { "CPU",     "GPU",          "Memory", "Power",      "Disk I/O",
                                          "Network", "Temperatures", "Fans",   "Filesystems" };
const char *const SINGLE_NAME[SINGLE_COUNT] = {
    "Cyan", "Blue", "Green", "Yellow", "Red", "Magenta", "White",
    "Bright cyan", "Bright blue", "Bright green", "Bright yellow", "Bright red", "Bright magenta", "Bright white",
};
const short SINGLE_COLOR[SINGLE_COUNT] = { 6, 4, 2, 3, 1, 5, 7, 14, 12, 10, 11, 9, 13, 15 };
static const char *const SINGLE_KEY[SINGLE_COUNT] = {
    "cyan", "blue", "green", "yellow", "red", "magenta", "white",
    "bright-cyan", "bright-blue", "bright-green", "bright-yellow", "bright-red", "bright-magenta", "bright-white",
};
const char *const BAR_NAME[BAR_COUNT] = { "Blocks", "Squares", "Line", "Classic" };
const char *const GRAPH_NAME[GRAPH_COUNT] = { "Blocks", "Braille (fine)", "Off" };
const char *const COLOR_NAME[COLOR_COUNT] = { "By value", "Gradient", "Single colour", "Monochrome" };

void cpubar_desc(int mask, char *out, size_t n)
{
    out[0] = '\0';
    static const char *const nm[3] = { "usage", "clock", "temperature" };
    for (int b = 0, k = 0; b < 3; b++)
        if (mask & (1 << b)) {
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", k++ ? " + " : "", nm[b]);
        }
}


static const char *const BAR_KEY[BAR_COUNT] = { "blocks", "squares", "line", "classic" };
static const char *const GRAPH_KEY[GRAPH_COUNT] = { "blocks", "braille", "off" };
static const char *const COLOR_KEY[COLOR_COUNT] = { "value", "gradient", "single", "mono" };
static const char *const CPUBAR_KEY[3] = { "usage", "clock", "temp" };
static const char *const SORT_KEY[SORT_COUNT] = { "cpu", "memory", "io", "pid", "name", "time" };

/* what was last loaded from / written to the file, to know if anything changed */
typedef struct {
    layout_t lay;
    double interval, log_every;
    int sort, reverse, kthreads;
    char log_dir[PATHBUF], log_skip[4096];
} snap_t;
static snap_t g_snap;

static void snapshot(const ui_state_t *u, snap_t *s)
{
    memset(s, 0, sizeof *s);
    s->lay = u->lay;
    s->interval = u->interval;
    s->log_every = u->log_every;
    s->sort = u->sort;
    s->reverse = u->reverse;
    s->kthreads = u->show_kthreads;
    str_copy(s->log_dir, sizeof s->log_dir, u->log_dir);
    str_copy(s->log_skip, sizeof s->log_skip, u->log_skip);
}

int config_changed(const ui_state_t *u)
{
    snap_t now;
    snapshot(u, &now);
    return memcmp(&now, &g_snap, sizeof now) != 0;
}

void layout_defaults(layout_t *l)
{
    static const int left[] = { P_CPU, P_GPU };
    static const int right[] = { P_MEM, P_POWER, P_DISK, P_NET, P_TEMPS, P_FANS, P_FS };
    memset(l, 0, sizeof *l);
    for (size_t i = 0; i < sizeof left / sizeof left[0]; i++)
        l->col[0][l->ncol[0]++] = left[i];
    for (size_t i = 0; i < sizeof right / sizeof right[0]; i++)
        l->col[1][l->ncol[1]++] = right[i];
    for (int p = 0; p < P_COUNT; p++)
        l->on[p] = p != P_TEMPS && p != P_FANS && p != P_FS;
    l->show_procs = 1;
    l->cpu_bars = CPUBAR_USAGE;
}

void config_reset_all(ui_state_t *u)
{
    layout_defaults(&u->lay);
    u->log_every = 1.0;
    u->log_skip[0] = '\0';
    u->interval = 1.0;
    u->sort = SORT_CPU;
    u->reverse = 0;
    u->show_kthreads = 0;
}

void config_defaults(ui_state_t *u)
{
    layout_defaults(&u->lay);
    u->interval = 1.0;
    u->sort = SORT_CPU;
    u->reverse = 0;
    u->show_kthreads = 0;
    u->confirm = -1;
    u->log_every = 1.0;
    u->log_dir[0] = '\0';
    u->log_skip[0] = '\0';
    snapshot(u, &g_snap);
}

/* ------------------------------------------------------------ location */

static char g_path[PATHBUF], g_dir[PATHBUF], g_logdir[PATHBUF], g_home[PATHBUF];

static uid_t g_uid = (uid_t)-1;
static gid_t g_gid = (gid_t)-1;

static void resolve(void)
{
    if (g_path[0])
        return;
    const char *home = NULL, *xdg = getenv("XDG_CONFIG_HOME");
    const char *su = getenv("SUDO_USER");
    if (geteuid() == 0 && su && *su && strcmp(su, "root")) {
        struct passwd *pw = getpwnam(su);
        if (pw && pw->pw_uid != 0) {
            home = pw->pw_dir;
            g_uid = pw->pw_uid;
            g_gid = pw->pw_gid;
            xdg = NULL; /* root's environment, not the user's */
        }
    }
    if (!home)
        home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    if (xdg && xdg[0] == '/')
        sfmt(g_dir, sizeof g_dir, "%s/hsm", xdg);
    else
        sfmt(g_dir, sizeof g_dir, "%s/.config/hsm", home);
    sfmt(g_path, sizeof g_path, "%s/config", g_dir);
    str_copy(g_home, sizeof g_home, home);
    /* logs: $XDG_STATE_HOME/hsm/logs, default ~/.local/state/hsm/logs */
    const char *st = g_uid == (uid_t)-1 ? getenv("XDG_STATE_HOME") : NULL;
    if (st && st[0] == '/')
        sfmt(g_logdir, sizeof g_logdir, "%s/hsm/logs", st);
    else
        sfmt(g_logdir, sizeof g_logdir, "%s/.local/state/hsm/logs", home);
}

const char *config_log_dir(const ui_state_t *u)
{
    resolve();
    static char buf[PATHBUF];
    if (!u || !u->log_dir[0])
        return g_logdir;
    if (u->log_dir[0] == '~' && (u->log_dir[1] == '/' || !u->log_dir[1])) {
        sfmt(buf, sizeof buf, "%s%s", g_home, u->log_dir + 1);
        return buf;
    }
    return u->log_dir;
}

const char *config_path(void)
{
    resolve();
    return g_path;
}

/* Become the sudo user for file access; returns 1 if identity changed. */
static int as_user(void)
{
    resolve();
    if (g_uid == (uid_t)-1)
        return 0;
    if (setegid(g_gid) != 0)
        return -1;
    if (seteuid(g_uid) != 0) {
        if (setegid(0) != 0)
            abort(); /* cannot restore our own group: never continue half-switched */
        return -1;
    }
    return 1;
}

static void as_root(int changed)
{
    if (changed == 1 && (seteuid(0) != 0 || setegid(0) != 0))
        abort();
}

void config_pretty_path(const char *path, char *out, size_t n)
{
    resolve();
    size_t hl = strlen(g_home);
    if (g_uid == (uid_t)-1 && hl > 1 && !strncmp(path, g_home, hl) /* under sudo "~" would mean root's home */ && (path[hl] == '/' || !path[hl]))
        sfmt(out, n, "~%s", path + hl);
    else
        str_copy(out, n, path);
}

int config_as_user(void)
{
    return as_user();
}

void config_as_root(int changed)
{
    as_root(changed);
}

/* --------------------------------------------------------------- parse */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int find_key(const char *const *keys, int n, const char *v)
{
    for (int i = 0; i < n; i++)
        if (!strcasecmp(keys[i], v))
            return i;
    return -1;
}

static int parse_bool(const char *v)
{
    return !strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true");
}

/* "cpu, gpu" -> panel ids appended to column c (skipping ones already placed) */
static void parse_list(layout_t *l, int c, char *v, int *placed)
{
    char *save = NULL;
    for (char *t = strtok_r(v, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int p = find_key(PANEL_KEY, P_COUNT, trim(t));
        if (p < 0 || placed[p])
            continue;
        placed[p] = 1;
        l->col[c][l->ncol[c]++] = p;
    }
}

int config_load(ui_state_t *u)
{
    resolve();
    int who = as_user();
    if (who < 0)
        return -1;
    FILE *f = fopen(g_path, "re");
    as_root(who);
    if (!f)
        return -1;

    layout_t *l = &u->lay;
    int placed[P_COUNT] = { 0 }, have_cols = 0;
    char hidden[512] = "";
    layout_t nl = *l;
    nl.ncol[0] = nl.ncol[1] = 0;

    char line[8192]; /* log_skip can be long */
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        for (char *h = s + 1; *h; h++) /* "key = value   # comment" */
            if (*h == '#' && (h[-1] == ' ' || h[-1] == '\t')) {
                *h = '\0';
                break;
            }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        int i;
        if (!strcmp(k, "left")) {
            parse_list(&nl, 0, v, placed);
            have_cols = 1;
        } else if (!strcmp(k, "right")) {
            parse_list(&nl, 1, v, placed);
            have_cols = 1;
        } else if (!strcmp(k, "hidden")) {
            str_copy(hidden, sizeof hidden, v);
        } else if (!strcmp(k, "processes")) {
            l->show_procs = parse_bool(v);
        } else if (!strcmp(k, "left_width")) {
            i = atoi(v);
            l->left_pct = (i >= 30 && i <= 75) ? i / 5 * 5 : 0;
        } else if (!strcmp(k, "bars") && (i = find_key(BAR_KEY, BAR_COUNT, v)) >= 0) {
            l->bar_style = i;
        } else if (!strcmp(k, "graphs") && (i = find_key(GRAPH_KEY, GRAPH_COUNT, v)) >= 0) {
            l->graph_style = i;
        } else if (!strcmp(k, "colors") && (i = find_key(COLOR_KEY, COLOR_COUNT, v)) >= 0) {
            l->color_mode = i;
        } else if (!strcmp(k, "cpu_bars")) { /* "usage, temp", "all" */
            int m = 0;
            char *save = NULL;
            for (char *t = strtok_r(v, ",+ ", &save); t; t = strtok_r(NULL, ",+ ", &save)) {
                if (!strcasecmp(t, "all"))
                    m = CPUBAR_ALL;
                else if ((i = find_key(CPUBAR_KEY, 3, t)) >= 0)
                    m |= 1 << i;
            }
            if (m)
                l->cpu_bars = m;

        } else if (!strcmp(k, "interval")) {
            double d = atof(v);
            if (d >= 0.1 && d <= 60)
                u->interval = d;
        } else if (!strcmp(k, "sort") && (i = find_key(SORT_KEY, SORT_COUNT, v)) >= 0) {
            u->sort = i;
        } else if (!strcmp(k, "reverse")) {
            u->reverse = parse_bool(v);
        } else if (!strcmp(k, "kernel_threads")) {
            u->show_kthreads = parse_bool(v);
        } else if (!strcmp(k, "single_color") && (i = find_key(SINGLE_KEY, SINGLE_COUNT, v)) >= 0) {
            l->single_color = i;
        } else if (!strcmp(k, "log_dir")) {
            str_copy(u->log_dir, sizeof u->log_dir, strcasecmp(v, "default") ? v : "");
        } else if (!strcmp(k, "log_every")) {
            double d = !strcasecmp(v, "refresh") ? 0 : atof(v);
            if (d >= 0 && d <= 3600)
                u->log_every = d;
        } else if (!strcmp(k, "log_skip")) {
            str_copy(u->log_skip, sizeof u->log_skip, v);
        }
    }
    fclose(f);

    if (have_cols) {
        /* panels the file doesn't mention (e.g. added in a newer version)
         * go to the right column, hidden */
        for (int p = 0; p < P_COUNT; p++) {
            nl.on[p] = 1;
            if (!placed[p]) {
                nl.col[1][nl.ncol[1]++] = p;
                nl.on[p] = 0;
            }
        }
        char *save = NULL;
        for (char *t = strtok_r(hidden, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            int p = find_key(PANEL_KEY, P_COUNT, trim(t));
            if (p >= 0)
                nl.on[p] = 0;
        }
        memcpy(l->col, nl.col, sizeof l->col);
        memcpy(l->ncol, nl.ncol, sizeof l->ncol);
        memcpy(l->on, nl.on, sizeof l->on);
    }
    snapshot(u, &g_snap);
    return 0;
}

/* ---------------------------------------------------------------- save */

static void write_list(FILE *f, const char *key, const layout_t *l, int c)
{
    fprintf(f, "%s =", key);
    for (int i = 0; i < l->ncol[c]; i++)
        fprintf(f, "%s %s", i ? "," : "", PANEL_KEY[l->col[c][i]]);
    fprintf(f, "\n");
}

int config_mkdir_p(const char *dir)
{
    char tmp[PATHBUF];
    str_copy(tmp, sizeof tmp, dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

int config_save(const ui_state_t *u)
{
    resolve();
    int who = as_user();
    if (who < 0)
        return -1;
    int rc = -1;
    char tmp[PATHBUF + 8];
    sfmt(tmp, sizeof tmp, "%s.tmp", g_path);
    if (config_mkdir_p(g_dir) == 0) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (f) {
            const layout_t *l = &u->lay;
            fprintf(f, "# hsm settings. Changed from the Setup screen (key s); safe to edit by hand.\n");
            fprintf(f, "# Panels: cpu gpu memory power disk network temps fans filesystems\n\n");
            write_list(f, "left", l, 0);
            write_list(f, "right", l, 1);
            fprintf(f, "hidden =");
            for (int p = 0, n = 0; p < P_COUNT; p++)
                if (!l->on[p])
                    fprintf(f, "%s %s", n++ ? "," : "", PANEL_KEY[p]);
            fprintf(f, "\nprocesses = %s\n", l->show_procs ? "on" : "off");
            if (l->left_pct)
                fprintf(f, "left_width = %d\n", l->left_pct);
            else
                fprintf(f, "left_width = auto\n");
            fprintf(f, "\nbars = %s\ngraphs = %s\ncolors = %s\ncpu_bars =", BAR_KEY[l->bar_style],
                    GRAPH_KEY[l->graph_style], COLOR_KEY[l->color_mode]);
            for (int b = 0, k = 0; b < 3; b++)
                if (l->cpu_bars & (1 << b))
                    fprintf(f, "%s %s", k++ ? "," : "", CPUBAR_KEY[b]);
            fprintf(f, "\n");
            fprintf(f, "single_color = %s\n", SINGLE_KEY[l->single_color]);
            fprintf(f, "\ninterval = %g\nsort = %s\nreverse = %s\nkernel_threads = %s\n", u->interval,
                    SORT_KEY[u->sort], u->reverse ? "on" : "off", u->show_kthreads ? "on" : "off");
            fprintf(f, "\n# Logging (view 5): folder, seconds between rows (refresh = every update),\n"
                       "# and the channels that are switched off\n");
            fprintf(f, "log_dir = %s\n", u->log_dir[0] ? u->log_dir : "default");
            if (u->log_every > 0)
                fprintf(f, "log_every = %g\n", u->log_every);
            else
                fprintf(f, "log_every = refresh\n");
            fprintf(f, "log_skip = %s\n", u->log_skip);
            if (fclose(f) == 0 && rename(tmp, g_path) == 0) {
                rc = 0;
                snapshot(u, &g_snap);
            }
            else
                unlink(tmp);
        } else if (fd >= 0) {
            close(fd);
        }
    }
    as_root(who);
    return rc;
}
