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
/* diag.c - hsm --report: one text file to post when asking for help (a
 * forum, a bug report): the Hardware view, the drives' health, the last
 * week's problems, the cooling checks and the sessions that ended badly.
 * The host name, user names in paths, IP and MAC addresses and anything
 * that looks like a serial number are left out. */
#define _GNU_SOURCE
#include "history.h"
#include "ui.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    FILE *f;
    const char *host;
    char user[64];
} rp_t;

static int hexd(int c)
{
    return isxdigit((unsigned char)c);
}

/* a MAC address at p: 6 pairs of hex digits joined by ':' or '-'; its length or 0 */
static int is_mac(const char *p)
{
    for (int i = 0; i < 6; i++) {
        if (!hexd(p[i * 3]) || !hexd(p[i * 3 + 1]))
            return 0;
        if (i < 5 && p[i * 3 + 2] != ':' && p[i * 3 + 2] != '-')
            return 0;
    }
    return hexd(p[17]) || p[17] == ':' ? 0 : 17;
}

/* an IPv4 address at p (the four parts 0-255); its length or 0 */
static int is_ip4(const char *p)
{
    const char *q = p;
    for (int i = 0; i < 4; i++) {
        int d = 0, v = 0;
        while (isdigit((unsigned char)*q) && d < 4) {
            v = v * 10 + (*q++ - '0');
            d++;
        }
        if (!d || d > 3 || v > 255)
            return 0;
        if (i < 3 && *q++ != '.')
            return 0;
    }
    if (isdigit((unsigned char)*q) || (*q == '.' && isdigit((unsigned char)q[1])))
        return 0;
    return (int)(q - p);
}

/* an IPv6 address at p: hex groups and colons, "::" or a letter in it (so
 * a time like 12:03:44 isn't one); its length or 0 */
static int is_ip6(const char *p)
{
    int colons = 0, letters = 0, dbl = 0, len = 0;
    while (hexd(p[len]) || p[len] == ':') {
        if (p[len] == ':') {
            colons++;
            dbl |= p[len + 1] == ':';
        } else if (isalpha((unsigned char)p[len])) {
            letters++;
        }
        len++;
    }
    if (colons < 2 || (!letters && !dbl) || len < 3)
        return 0;
    if (isalnum((unsigned char)p[len]))
        return 0;
    return len;
}

/* the line with the personal parts replaced */
static void scrub(const rp_t *r, const char *in, char *out, size_t n)
{
    size_t o = 0;
    size_t hl = r->host && strlen(r->host) >= 2 ? strlen(r->host) : 0;
    for (const char *p = in; *p && o + 24 < n;) {
        int edge = p == in || !isalnum((unsigned char)p[-1]);
        int l;
        if (edge && (l = is_mac(p))) {
            o += (size_t)snprintf(out + o, n - o, "xx:xx:xx:xx:xx:xx");
            p += l;
        } else if (edge && p[-1 + (p == in)] != '.' && (l = is_ip4(p))) {
            int keep = !strncmp(p, "127.", 4) || !strncmp(p, "0.0.0.0", 7); /* not about anyone */
            if (keep) {
                memcpy(out + o, p, (size_t)l);
                o += (size_t)l;
            } else {
                o += (size_t)snprintf(out + o, n - o, "x.x.x.x");
            }
            p += l;
        } else if (edge && (l = is_ip6(p))) {
            if (l == 3 && !strncmp(p, "::1", 3)) {
                memcpy(out + o, p, 3);
                o += 3;
            } else {
                o += (size_t)snprintf(out + o, n - o, "x:x::x");
            }
            p += l;
        } else if (hl && edge && !strncmp(p, r->host, hl) && !isalnum((unsigned char)p[hl])) {
            o += (size_t)snprintf(out + o, n - o, "<host>");
            p += hl;
        } else if (r->user[0] && (!strncmp(p, "/home/", 6) || !strncmp(p, "/media/", 7) || !strncmp(p, "/run/media/", 11))) {
            const char *u = p + (p[1] == 'h' ? 6 : p[1] == 'm' ? 7 : 11);
            size_t ul = strlen(r->user);
            if (!strncmp(u, r->user, ul) && (u[ul] == '/' || !u[ul] || u[ul] == ' ' || u[ul] == ')')) {
                memcpy(out + o, p, (size_t)(u - p));
                o += (size_t)(u - p);
                o += (size_t)snprintf(out + o, n - o, "<user>");
                p = u + ul;
            } else {
                out[o++] = *p++;
            }
        } else if (edge && !strncasecmp(p, "serial", 6)) { /* "Serial: X1234": the word stays, the value goes */
            const char *q = p + 6;
            while (*q && *q != ':' && *q != '=' && q - p < 16)
                q++;
            if (*q == ':' || *q == '=') {
                memcpy(out + o, p, (size_t)(q - p + 1));
                o += (size_t)(q - p + 1);
                o += (size_t)snprintf(out + o, n - o, " (left out)");
                q++;
                while (*q == ' ')
                    q++;
                while (*q && *q != ' ' && *q != ',' && *q != ';')
                    q++;
                p = q;
            } else {
                out[o++] = *p++;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

static void line(const char *t, void *ctx)
{
    const rp_t *r = ctx;
    char b[4096];
    scrub(r, t, b, sizeof b);
    fprintf(r->f, "%s\n", b);
}

static void linef(rp_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void linef(rp_t *r, const char *fmt, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    line(b, r);
}

static void title(rp_t *r, const char *t)
{
    char b[128] = "";
    size_t l = strlen(t);
    snprintf(b, sizeof b, "%s ", t);
    for (size_t k = l + 1; k < 99 && k < sizeof b - 1; k++)
        b[k] = '-';
    b[MIN(99, sizeof b - 1)] = '\0';
    fprintf(r->f, "\n%s\n", b);
}

/* the warnings and errors of the last 7 days, oldest first, from events.log (and .1) */
static void week_events(rp_t *r)
{
    title(r, "Problems in the last 7 days (warnings and errors)");
    time_t since = time(NULL) - 7 * 86400;
    int shown = 0, info = 0, warn = 0, err = 0;
    for (int part = 1; part >= 0; part--) {
        char p[PATHBUF], b[1024];
        snprintf(p, sizeof p, "%s/events.log%s", config_state_dir(), part ? ".1" : "");
        FILE *f = fopen(p, "re");
        if (!f)
            continue;
        while (fgets(b, sizeof b, f)) {
            b[strcspn(b, "\n")] = '\0';
            hevent_t e;
            if (history_parse_event(b, &e) || e.t < since)
                continue;
            if (e.level == EV_INFO) {
                info++;
                continue;
            }
            e.level == EV_ERR ? err++ : warn++;
            if (shown >= 400)
                continue;
            struct tm tm;
            time_t t = (time_t)e.t;
            char ts[32];
            localtime_r(&t, &tm);
            strftime(ts, sizeof ts, "%a %d %b %H:%M:%S", &tm);
            linef(r, "%s  %-5s %-8s %s", ts, e.level == EV_ERR ? "error" : "warn", e.cat, e.text);
            shown++;
        }
        fclose(f);
    }
    if (!warn && !err)
        linef(r, "%s", "none");
    linef(r, "(%d error%s, %d warning%s%s; %d other event%s, such as suspends and devices plugged in, left out)", err,
          err == 1 ? "" : "s", warn, warn == 1 ? "" : "s", shown < warn + err ? ", the first 400 shown" : "", info,
          info == 1 ? "" : "s");
}

/* the last few lines of one of hsm's own logs */
static void tail_of(rp_t *r, const char *name, const char *what, int max)
{
    char p[PATHBUF], b[1024];
    snprintf(p, sizeof p, "%s/%s", config_state_dir(), name);
    FILE *f = fopen(p, "re");
    title(r, what);
    if (!f) {
        linef(r, "%s", "none");
        return;
    }
    static char keep[16][1024];
    int n = 0;
    while (fgets(b, sizeof b, f)) {
        b[strcspn(b, "\n")] = '\0';
        if (!b[0] || b[0] == '#')
            continue;
        str_copy(keep[n % 16], sizeof keep[0], b);
        n++;
    }
    fclose(f);
    if (!n)
        linef(r, "%s", "none");
    for (int i = MAX(0, n - MIN(max, 16)); i < n; i++)
        line(keep[i % 16], r);
}

int report_write(sys_t *s, ui_state_t *u, const char *path)
{
    rp_t r;
    memset(&r, 0, sizeof r);
    r.host = s->host;
    const char *su = getenv("SUDO_USER");
    struct passwd *pw = getpwuid(getuid());
    str_copy(r.user, sizeof r.user, su && *su ? su : pw ? pw->pw_name : "");
    if (!strcmp(r.user, "root"))
        r.user[0] = '\0';
    char def[PATHBUF];
    if (!path || !*path) { /* hsm-report-2026-11-14_23-30.txt, here */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(def, sizeof def, "hsm-report-%Y-%m-%d_%H-%M.txt", &tm);
        path = def;
    }
    int out = !strcmp(path, "-");
    r.f = out ? stdout : fopen(path, "we");
    if (!r.f) {
        fprintf(stderr, "%s: %s: %s\n", APP_NAME, path, strerror(errno));
        return 1;
    }
    ui_headless(&u->lay, 1);
    time_t t = time(NULL);
    struct tm tm;
    char when[64];
    localtime_r(&t, &tm);
    strftime(when, sizeof when, "%d %b %Y %H:%M (UTC%z)", &tm);
    fprintf(r.f, "hsm %s report, %s\n", APP_VERSION, when);
    fprintf(r.f, "%s\n", geteuid() == 0 ? "Made as root: drive health, the kernel log and power readings are in it."
                                        : "Made without root: drive health, the kernel log and CPU power need sudo hsm --report.");
    fprintf(r.f, "Left out: the host name, user names in paths, IP and MAC addresses, serial numbers.\n");
    title(&r, "Hardware (as in hsm's Hardware view)");
    hardware_text(s, 100, line, &r);
    week_events(&r);
    tail_of(&r, "cooling.log", "Cooling checks (the latest last)", 5);
    tail_of(&r, "unexpected-ends.log", "Sessions that ended unexpectedly (crashes, power cuts)", 8);
    fprintf(r.f, "\nMade by hsm %s, https://github.com/Yormask/linux-hardware-stats\n", APP_VERSION);
    if (out) {
        fflush(stdout);
        return 0;
    }
    int bad = ferror(r.f) | (fclose(r.f) != 0);
    uid_t uid;
    gid_t gid;
    if (!bad && config_user_ids(&uid, &gid) && chown(path, uid, gid)) { /* under sudo: yours, not root's */
    }
    if (bad) {
        fprintf(stderr, "%s: could not write %s\n", APP_NAME, path);
        return 1;
    }
    printf("Report written to %s\n", path);
    printf("It leaves out your host name, user name, IP and MAC addresses and serial numbers; have a look before you post it.\n");
    if (geteuid())
        printf("Drive health, the kernel log and CPU power need root: sudo %s --report\n", APP_NAME);
    return 0;
}
