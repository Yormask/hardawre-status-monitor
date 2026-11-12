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
/* cgroup.c - CPU, memory, disk I/O and task counts per systemd service,
 * login session, desktop app and container, from the cgroup v2 hierarchy.
 *
 * Every unit (and every container) gets its own cgroup, and the kernel
 * keeps running totals for each, so this answers "which service is eating
 * my memory" without looking at a single process. Only done while the
 * Services view or panel is on screen. */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_EVERY 5.0 /* seconds between looks for new / removed units */

const char *svc_kind_name(int kind)
{
    static const char *const k[] = { "service", "scope", "container", "session", "app", "cgroup" };
    return kind >= 0 && kind <= SVC_OTHER ? k[kind] : "?";
}

static int ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static const char *base(const char *rel)
{
    const char *sl = strrchr(rel, '/');
    return sl ? sl + 1 : rel;
}

/* "app-gnome-firefox-12345.scope" -> "gnome-firefox", "docker-3f2a...scope" -> "docker 3f2a1b2c3d4e" */
static void describe(svc_t *v)
{
    const char *n = base(v->path);
    char b[128];
    str_copy(b, sizeof b, n);
    if (ends(b, ".service"))
        b[strlen(b) - 8] = '\0';
    else if (ends(b, ".scope"))
        b[strlen(b) - 6] = '\0';

    static const struct {
        const char *prefix, *show;
    } ct[] = { { "docker-", "docker" }, { "libpod-", "podman" }, { "crio-", "cri-o" }, { "cri-containerd-", "containerd" } };
    for (size_t i = 0; i < sizeof ct / sizeof ct[0]; i++)
        if (!strncmp(b, ct[i].prefix, strlen(ct[i].prefix))) {
            sfmt(v->name, sizeof v->name, "%s %.12s", ct[i].show, b + strlen(ct[i].prefix));
            v->kind = SVC_CONTAINER;
            return;
        }
    if (!strncmp(b, "session-", 8)) {
        sfmt(v->name, sizeof v->name, "session %s", b + 8);
        v->kind = SVC_SESSION;
        return;
    }
    if (!strncmp(b, "app-", 4) && ends(n, ".scope")) { /* desktop apps: drop "app-" and the trailing "-PID" */
        char *d = b + strlen(b);
        while (d > b && d[-1] >= '0' && d[-1] <= '9')
            d--;
        if (d > b + 4 && d[-1] == '-' && *d)
            d[-1] = '\0';
        str_copy(v->name, sizeof v->name, b + 4);
        v->kind = SVC_APP;
        return;
    }
    str_copy(v->name, sizeof v->name, b);
    v->kind = ends(n, ".service") ? (strstr(v->path, "user@") ? SVC_APP : SVC_SERVICE)
            : ends(n, ".scope")   ? SVC_SCOPE
                                  : SVC_CONTAINER;
}

static uid_t owner(const char *rel) /* ".../user-1000.slice/..." -> 1000 */
{
    const char *u = strstr(rel, "user-");
    if (!u)
        return 0;
    char *end;
    long id = strtol(u + 5, &end, 10);
    return (end != u + 5 && !strncmp(end, ".slice", 6) && id > 0) ? (uid_t)id : 0;
}

static int container_tree(const char *rel)
{
    static const char *const roots[] = { "docker/", "lxc/", "lxc.payload", "kubepods", "machine/", "libpod_parent", "garden/" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++)
        if (!strncmp(rel, roots[i], strlen(roots[i])) || strstr(rel, roots[i]))
            return 1;
    return 0;
}

static svc_t *find(svcs_t *v, const char *rel)
{
    for (int i = 0; i < v->n; i++)
        if (!strcmp(v->s[i].path, rel))
            return &v->s[i];
    return NULL;
}

static void keep(svcs_t *v, const char *rel)
{
    svc_t *s = find(v, rel);
    if (!s) {
        if (v->n >= v->cap) {
            int nc = v->cap ? v->cap * 2 : 128;
            svc_t *ns = realloc(v->s, (size_t)nc * sizeof *ns);
            if (!ns)
                return;
            v->s = ns;
            v->cap = nc;
        }
        s = &v->s[v->n++];
        memset(s, 0, sizeof *s);
        str_copy(s->path, sizeof s->path, rel);
        s->uid = owner(rel);
        s->mem = -1;
        s->tasks = -1;
        describe(s);
    }
    s->seen = 1;
}

/* Units are leaves: a .service / .scope with no listed unit below it (so
 * user@1000.service is shown as the apps and scopes inside it). Containers
 * not run through systemd (docker's cgroupfs driver, LXC, Kubernetes) are
 * the leaf directories in their trees. Returns 1 if something was kept. */
static int walk(svcs_t *v, const char *rel, int depth)
{
    char abs[PATHBUF];
    if (*rel)
        sfmt(abs, sizeof abs, "%s/%s", v->root, rel);
    else
        str_copy(abs, sizeof abs, v->root);
    DIR *d = opendir(abs);
    if (!d)
        return 0;
    int below = 0, nsub = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (e->d_type != DT_DIR) {
            if (e->d_type != DT_UNKNOWN)
                continue;
            struct stat st;
            if (fstatat(dirfd(d), e->d_name, &st, 0) || !S_ISDIR(st.st_mode))
                continue;
        }
        nsub++;
        char sub[256];
        if (sfmt(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name) >= (int)sizeof sub)
            continue;
        if (depth < 10)
            below |= walk(v, sub, depth + 1);
    }
    closedir(d);
    if (!*rel || below)
        return below;
    const char *n = base(rel);
    int unit = ends(n, ".service") || ends(n, ".scope");
    if (unit || (!nsub && !ends(n, ".slice") && container_tree(rel))) {
        keep(v, rel);
        return 1;
    }
    return 0;
}

static int cg_read(const svcs_t *v, const svc_t *s, const char *file, char *buf, size_t n)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s/%s", v->root, s->path, file);
    return read_file(p, buf, n) > 0 ? 0 : -1;
}

static void stats(svcs_t *v, svc_t *s, double dt, int slow)
{
    char b[4096];
    unsigned long long usage = 0, rd = 0, wr = 0;
    int have_cpu = 0, have_io = 0;
    if (!cg_read(v, s, "cpu.stat", b, sizeof b)) {
        char *u = strstr(b, "usage_usec ");
        if (u) {
            usage = strtoull(u + 11, NULL, 10);
            have_cpu = 1;
        }
    }
    s->mem = !cg_read(v, s, "memory.current", b, sizeof b) ? strtoll(b, NULL, 10) : -1;
    s->tasks = !cg_read(v, s, "pids.current", b, sizeof b) ? strtol(b, NULL, 10) : -1;
    if (!cg_read(v, s, "io.stat", b, sizeof b)) { /* one line per device */
        have_io = 1;
        for (char *q = b; (q = strstr(q, "rbytes=")) != NULL; q += 7)
            rd += strtoull(q + 7, NULL, 10);
        for (char *q = b; (q = strstr(q, "wbytes=")) != NULL; q += 7)
            wr += strtoull(q + 7, NULL, 10);
    }
    if (slow && !cg_read(v, s, "memory.events", b, sizeof b)) {
        char *o = strstr(b, "oom_kill ");
        if (o)
            s->oom = strtoull(o + 9, NULL, 10);
    }
    if (s->primed && dt > 0) {
        s->cpu = have_cpu && usage >= s->p_usage ? (double)(usage - s->p_usage) / (dt * 1e6) * 100.0 : 0;
        s->rd_bps = have_io && rd >= s->p_rd ? (double)(rd - s->p_rd) / dt : 0;
        s->wr_bps = have_io && wr >= s->p_wr ? (double)(wr - s->p_wr) / dt : 0;
    }
    s->p_usage = usage;
    s->p_rd = rd;
    s->p_wr = wr;
    s->primed = 1;
}

void svcs_sample(svcs_t *v)
{
    double now = now_sec();
    if (now > v->want_until)
        return;
    if (v->state == 0) { /* pure cgroup v2, or the v2 part of a "hybrid" setup */
        sfmt(v->root, sizeof v->root, "%s/fs/cgroup", g_sysfs);
        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/cgroup.controllers", v->root);
        if (!path_exists(p)) {
            sfmt(v->root, sizeof v->root, "%s/fs/cgroup/unified", g_sysfs);
            sfmt(p, sizeof p, "%s/cgroup.controllers", v->root);
        }
        v->state = path_exists(p) ? 1 : -1;
    }
    if (v->state != 1)
        return;
    int slow = now >= v->next_scan;
    if (slow) {
        v->next_scan = now + SCAN_EVERY;
        for (int i = 0; i < v->n; i++)
            v->s[i].seen = 0;
        walk(v, "", 0);
        int w = 0;
        for (int i = 0; i < v->n; i++)
            if (v->s[i].seen)
                v->s[w++] = v->s[i];
        v->n = w;
    }
    double dt = v->t > 0 && now - v->t < 30 ? now - v->t : 0; /* after a pause: start over */
    v->t = now;
    for (int i = 0; i < v->n; i++) {
        if (dt == 0)
            v->s[i].primed = 0;
        stats(v, &v->s[i], dt, slow);
    }
}

void svcs_free(svcs_t *v)
{
    free(v->s);
    v->s = NULL;
    v->n = v->cap = 0;
}
