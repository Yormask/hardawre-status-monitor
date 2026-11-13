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
/* ui_svc.c - Services view (6): CPU, memory, disk I/O and tasks per systemd
 * service, login session, desktop app and container; plus the small
 * "Services" dashboard panel. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_sort, g_rev;
static const svc_t *g_sv;

static int cmp_svc(const void *a, const void *b)
{
    const svc_t *p = &g_sv[*(const int *)a], *q = &g_sv[*(const int *)b];
    int r = 0;
    switch (g_sort) {
    case SORT_MEM: r = (p->mem < q->mem) - (p->mem > q->mem); break;
    case SORT_IO: r = (p->rd_bps + p->wr_bps < q->rd_bps + q->wr_bps) - (p->rd_bps + p->wr_bps > q->rd_bps + q->wr_bps); break;
    case SORT_NAME: r = strcasecmp(p->name, q->name); break;
    case SORT_TIME: r = (p->tasks < q->tasks) - (p->tasks > q->tasks); break;
    default: r = (p->cpu < q->cpu) - (p->cpu > q->cpu); break;
    }
    if (!r)
        r = (p->mem < q->mem) - (p->mem > q->mem);
    if (!r)
        r = strcmp(p->path, q->path);
    return g_rev ? -r : r;
}

/* the units to show, in display order; returns how many */
static int ordered(sys_t *s, ui_state_t *u, int **out)
{
    static int *ord;
    static int cap;
    svcs_t *v = &s->svc;
    if (cap < v->n) {
        int *no = realloc(ord, (size_t)(v->n + 64) * sizeof *no);
        if (!no)
            return 0;
        ord = no;
        cap = v->n + 64;
    }
    int n = 0;
    for (int i = 0; i < v->n; i++)
        if (!u->svc_filter[0] || strcasestr(v->s[i].name, u->svc_filter) || strcasestr(v->s[i].path, u->svc_filter))
            ord[n++] = i;
    g_sort = u->svc_sort;
    g_rev = u->svc_rev;
    g_sv = v->s;
    if (n > 1)
        qsort(ord, (size_t)n, sizeof *ord, cmp_svc);
    *out = ord;
    return n;
}

static void want(sys_t *s)
{
    s->svc.want_until = now_sec() + 3.0;
    if (s->svc.t == 0) /* first look: don't wait for the next sample */
        svcs_sample(&s->svc);
}

static void no_cgroup2(int y, int x, int w)
{
    put(y, x, w, A_BOLD, "Services need cgroup v2 (the \"unified\" hierarchy).");
    put(y + 1, x, w, A_GRAY, "systemd uses it by default since 2021: Ubuntu 21.10, Debian 11, Fedora 31, Arch.");
}

void services_view(sys_t *s, ui_state_t *u)
{
    want(s);
    svcs_t *v = &s->svc;
    int top = 1, h = LINES - 2, W = COLS;
    int *ord, n = ordered(s, u, &ord);
    char info[160] = "";
    if (v->state == 1) {
        app(info, sizeof info, "%d units", v->n);
        app(info, sizeof info, "%ssort %s%s", SEP, g_sort == SORT_TIME ? "TASKS" : SORT_NAMES[g_sort], u->svc_rev ? " (rev)" : "");
        if (u->svc_filter[0])
            app(info, sizeof info, "%sfilter \"%s\": %d", SEP, u->svc_filter, n);
    }
    box_(top, 0, h, W, "Services", info);
    int ix = 1, iw = W - 2, iy = top + 1, rows = h - 3;
    if (v->state != 1) {
        if (v->state == -1)
            no_cgroup2(iy + 1, ix + 2, iw - 4);
        return;
    }
    u->svc_rows = MAX(1, rows);
    u->svc_sel = n ? CLAMP(u->svc_sel, 0, n - 1) : 0;
    if (u->svc_sel < u->svc_scroll)
        u->svc_scroll = u->svc_sel;
    if (u->svc_sel >= u->svc_scroll + rows)
        u->svc_scroll = u->svc_sel - rows + 1;
    u->svc_scroll = CLAMP(u->svc_scroll, 0, MAX(0, n - rows));

    int any_oom = 0;
    for (int i = 0; i < v->n; i++)
        any_oom |= v->s[i].oom > 0;
    /* columns after the name: TYPE USER CPU% MEMORY READ/s WRITE/s TASKS [OOM] */
    static const struct {
        const char *h;
        int w, sort;
    } C[] = { { "TYPE", 9, -1 },     { "USER", 9, -1 },        { "CPU%", 6, SORT_CPU }, { "MEMORY", 7, SORT_MEM },
              { "READ/s", 8, SORT_IO }, { "WRITE/s", 8, SORT_IO }, { "TASKS", 6, SORT_TIME }, { "OOM", 4, -1 } };
    int nc = any_oom ? 8 : 7, fixed = 0;
    for (int i = 0; i < nc; i++)
        fixed += C[i].w + 1;
    int namew = CLAMP(iw - fixed - 1, 16, 34);
    int show_user = 1;
    if (namew + fixed > iw) { /* narrow: drop USER */
        show_user = 0;
        fixed -= C[1].w + 1;
        namew = MAX(12, iw - fixed - 1);
    }
    int pathx = ix + namew + 1 + fixed, pathw = ix + iw - pathx - 1; /* where it lives, if there's room */
    fill(iy, ix, iw, CP(C_THEAD));
    int cx = ix + put(iy, ix, namew + 1, CP(C_THEAD) | (g_sort == SORT_NAME ? A_BOLD | A_UNDERLINE : 0), " NAME");
    hit_add(iy, ix, 1, namew + 1, HIT_SVC_SORT, SORT_NAME, 0);
    cx = ix + namew + 1;
    for (int i = 0; i < nc; i++) {
        if (i == 1 && !show_user)
            continue;
        attr_t a = CP(C_THEAD) | (C[i].sort == g_sort ? A_BOLD | A_UNDERLINE : 0);
        put(iy, cx, C[i].w, a, i < 2 ? "%-*s" : "%*s", C[i].w, C[i].h);
        if (C[i].sort >= 0)
            hit_add(iy, cx, 1, C[i].w + 1, HIT_SVC_SORT, C[i].sort, 0);
        cx += C[i].w + 1;
    }
    hit_add(iy + 1, ix, rows, iw, HIT_SCROLL, 0, 0);
    if (pathw >= 12)
        put(iy, pathx + 1, pathw, CP(C_THEAD), "CGROUP");
    if (!n) {
        put(iy + 2, ix + 2, iw - 4, A_GRAY, "%s", u->svc_filter[0] ? "Nothing matches the filter" : "No systemd services or containers found");
        return;
    }
    for (int r = 0; r < rows && u->svc_scroll + r < n; r++) {
        int k = u->svc_scroll + r, sel = k == u->svc_sel;
        svc_t *e = &v->s[ord[k]];
        int yy = iy + 1 + r;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(yy, ix, iw, base);
        hit_add(yy, ix, 1, iw, HIT_SVC, k, 0);
        put(yy, ix + 1, namew - 1, base | A_BOLD, "%s", e->name);
        cx = ix + namew + 1;
        char t[32];
        for (int i = 0; i < nc; i++) {
            attr_t a = base;
            switch (i) {
            case 0: str_copy(t, sizeof t, svc_kind_name(e->kind)); if (!sel) a = A_GRAY; break;
            case 1:
                if (!show_user)
                    continue;
                str_copy(t, sizeof t, e->uid ? uid_name(e->uid) : "system");
                if (!sel && !e->uid)
                    a = A_GRAY;
                break;
            case 2:
                sfmt(t, sizeof t, "%.1f", e->cpu);
                if (!sel)
                    a = e->cpu < 0.05 ? A_GRAY : e->cpu >= 85 ? CP(C_CRIT) | A_BOLD : e->cpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD;
                break;
            case 3:
                if (e->mem >= 0)
                    fmt_bytes((double)e->mem, t, sizeof t);
                else
                    str_copy(t, sizeof t, "-");
                break;
            case 4:
            case 5: {
                double bps = i == 4 ? e->rd_bps : e->wr_bps;
                if (bps > 0) {
                    fmt_rate(bps, t, sizeof t);
                    if (!sel)
                        a = (i == 4 ? CP(C_IN) : CP(C_OUT)) | A_BOLD;
                } else {
                    str_copy(t, sizeof t, "0");
                    if (!sel)
                        a = A_GRAY;
                }
                break;
            }
            case 6:
                if (e->tasks >= 0)
                    sfmt(t, sizeof t, "%ld", e->tasks);
                else
                    str_copy(t, sizeof t, "-");
                break;
            case 7:
                sfmt(t, sizeof t, "%llu", e->oom);
                if (!sel)
                    a = e->oom ? CP(C_CRIT) | A_BOLD : A_GRAY;
                break;
            }
            put(yy, cx, C[i].w, a, i < 2 ? "%-*s" : "%*s", C[i].w, t);
            cx += C[i].w + 1;
        }
        if (pathw >= 12) { /* the parent slice: "system.slice", "user.slice/user-1000.slice/..." */
            const char *sl = strrchr(e->path, '/');
            int len = sl ? (int)(sl - e->path) : 0;
            put(yy, pathx + 1, pathw, sel ? base : A_GRAY, "%.*s", len ? len : 1, len ? e->path : "/");
        }
    }
    if (n > rows) {
        char m[48];
        sfmt(m, sizeof m, " %d-%d of %d ", u->svc_scroll + 1, MIN(n, u->svc_scroll + rows), n);
        put(top + h - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}

static void set_sort(ui_state_t *u, int s)
{
    if (u->svc_sort == s)
        u->svc_rev ^= 1;
    else {
        u->svc_sort = s;
        u->svc_rev = 0;
    }
}

int services_key(sys_t *s, ui_state_t *u, int ch)
{
    int page = MAX(1, u->svc_rows - 1);
    switch (ch) {
    case KEY_UP: u->svc_sel--; break;
    case KEY_DOWN: u->svc_sel++; break;
    case KEY_PPAGE: u->svc_sel -= page; break;
    case KEY_NPAGE: u->svc_sel += page; break;
    case KEY_HOME: u->svc_sel = 0; break;
    case KEY_END: u->svc_sel = 1 << 20; break;
    case 'c': set_sort(u, SORT_CPU); break;
    case 'm': set_sort(u, SORT_MEM); break;
    case 'd': set_sort(u, SORT_IO); break;
    case 'n': set_sort(u, SORT_NAME); break;
    case 't': set_sort(u, SORT_TIME); break;
    case 'r': u->svc_rev ^= 1; break;
    case 27: u->svc_filter[0] = '\0'; break;
    case '\n':
    case '\r':
    case KEY_ENTER: { /* show the processes in this unit */
        int *ord, n = ordered(s, u, &ord);
        if (!n)
            return 1;
        svc_t *e = &s->svc.s[ord[CLAMP(u->svc_sel, 0, n - 1)]];
        str_copy(u->svc_path, sizeof u->svc_path, e->path);
        str_copy(u->svc_name, sizeof u->svc_name, e->name);
        u->view = VIEW_PROCS;
        u->sel_idx = u->scroll = 0;
        ui_message(u, 0, "Processes of %s  (Esc shows all)", e->name);
        return 1;
    }
    default: return 0;
    }
    if (u->svc_sel < 0)
        u->svc_sel = 0;
    return 1;
}

/* a click on a row selects it; a double click shows its processes */
void services_click(sys_t *s, ui_state_t *u, int row, int dbl)
{
    u->svc_sel = row;
    if (dbl)
        services_key(s, u, '\n');
}

void services_sort_click(ui_state_t *u, int sort)
{
    set_sort(u, sort);
}

/* --------------------------------------------------- dashboard panel */

int svc_panel_need(sys_t *s, ui_state_t *u, int w)
{
    return s->svc.state == 1 ? 3 + MIN(MAX(s->svc.n, 1), 5) : 4;
}

void svc_panel(sys_t *s, ui_state_t *u, int y, int x, int h, int w)
{
    want(s);
    svcs_t *v = &s->svc;
    char info[32] = "";
    if (v->state == 1)
        sfmt(info, sizeof info, "%d units, by CPU", v->n);
    box_(y, x, h, w, "Services", info);
    int ix = x + 2, iw = w - 4, iy = y + 1, ih = h - 2;
    if (ih <= 0)
        return;
    if (v->state != 1) {
        if (v->state == -1)
            no_cgroup2(iy, ix, iw);
        return;
    }
    int keep_sort = u->svc_sort, keep_rev = u->svc_rev;
    u->svc_sort = SORT_CPU;
    u->svc_rev = 0;
    char keep_f[64];
    str_copy(keep_f, sizeof keep_f, u->svc_filter);
    u->svc_filter[0] = '\0';
    int *ord, n = ordered(s, u, &ord);
    u->svc_sort = keep_sort;
    u->svc_rev = keep_rev;
    str_copy(u->svc_filter, sizeof u->svc_filter, keep_f);
    put(iy, ix, iw, A_GRAY, "%-*s %6s %7s", iw - 15, "", "CPU%", "MEMORY");
    for (int r = 0; r < n && r + 1 < ih; r++) {
        svc_t *e = &v->s[ord[r]];
        char m[16];
        if (e->mem >= 0)
            fmt_bytes((double)e->mem, m, sizeof m);
        else
            str_copy(m, sizeof m, "-");
        put(iy + 1 + r, ix, iw - 15, A_NORMAL, "%s", e->name);
        put(iy + 1 + r, ix + iw - 14, 14, e->cpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD, "%6.1f %7s", e->cpu, m);
    }
}
