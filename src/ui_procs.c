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
/* ui_procs.c - process table: sorting, filtering, selection */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ============================================================ Processes */

enum { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
static const struct {
    const char *h;
    int w, right, sort;
} COLDEF[] = {
    [K_PID] = { "PID", 7, 1, SORT_PID },   [K_USER] = { "USER", 9, 0, -1 },    [K_PRI] = { "PRI", 3, 1, -1 },
    [K_NI] = { "NI", 3, 1, -1 },           [K_THR] = { "THR", 4, 1, -1 },      [K_S] = { "S", 1, 0, -1 },
    [K_CPU] = { "CPU%", 5, 1, SORT_CPU },  [K_MEM] = { "MEM%", 5, 1, SORT_MEM }, [K_VIRT] = { "VIRT", 6, 1, -1 },
    [K_RSS] = { "RSS", 6, 1, SORT_MEM },   [K_RD] = { "READ/s", 8, 1, SORT_IO }, [K_WR] = { "WRITE/s", 8, 1, SORT_IO },
    [K_TIME] = { "TIME+", 9, 1, SORT_TIME }, [K_CMD] = { "COMMAND", 0, 0, SORT_NAME },
};

static int g_sort, g_rev;
static proc_t *g_pa;

static int cmp_dbl_desc(double a, double b)
{
    return (a < b) - (a > b);
}

static int cmp_proc(const void *a, const void *b)
{
    const proc_t *p = &g_pa[*(const int *)a], *q = &g_pa[*(const int *)b];
    int r = 0;
    switch (g_sort) {
    case SORT_CPU: r = cmp_dbl_desc(p->cpu, q->cpu); break;
    case SORT_MEM: r = cmp_dbl_desc((double)p->rss, (double)q->rss); break;
    case SORT_IO: r = cmp_dbl_desc(p->rd_bps + p->wr_bps, q->rd_bps + q->wr_bps); break;
    case SORT_PID: r = (p->pid > q->pid) - (p->pid < q->pid); break;
    case SORT_NAME: r = strcasecmp(p->comm, q->comm); break;
    case SORT_TIME: r = cmp_dbl_desc((double)p->ticks, (double)q->ticks); break;
    }
    if (!r && g_sort != SORT_MEM)
        r = cmp_dbl_desc((double)p->rss, (double)q->rss);
    if (!r)
        r = (p->pid > q->pid) - (p->pid < q->pid);
    return g_rev ? -r : r;
}

static int proc_match(proc_t *p, const char *f)
{
    if (!f[0])
        return 1;
    if (strcasestr(p->comm, f) || strcasestr(uid_name(p->uid), f))
        return 1;
    if (!p->kthread && strcasestr(proc_cmdline(p), f))
        return 1;
    char b[16];
    sfmt(b, sizeof b, "%d", (int)p->pid);
    return !strcmp(b, f);
}

static void fmt_time(unsigned long long ticks, long hz, char *out, size_t n)
{
    double s = (double)ticks / (double)hz;
    long t = (long)s;
    if (t < 3600)
        sfmt(out, n, "%ld:%05.2f", t / 60, s - (double)(t / 60 * 60));
    else
        sfmt(out, n, "%ldh%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

static void cell_text(proc_t *p, int k, long hz, char *o, size_t n, attr_t *a)
{
    *a = A_NORMAL;
    switch (k) {
    case K_PID: sfmt(o, n, "%d", (int)p->pid); break;
    case K_USER:
        str_copy(o, n, uid_name(p->uid));
        if (p->uid == 0)
            *a = A_GRAY;
        break;
    case K_PRI: sfmt(o, n, "%d", p->prio); break;
    case K_NI:
        sfmt(o, n, "%d", p->nice);
        *a = p->nice < 0 ? CP(C_CRIT) : p->nice > 0 ? CP(C_OK) : A_GRAY;
        break;
    case K_THR: sfmt(o, n, "%d", p->nthreads); break;
    case K_S:
        sfmt(o, n, "%c", p->state);
        *a = p->state == 'R' ? CP(C_OK) | A_BOLD
           : p->state == 'D' || p->state == 'Z' ? CP(C_CRIT) | A_BOLD
           : p->state == 'T' ? CP(C_WARN) : A_GRAY;
        break;
    case K_CPU:
        sfmt(o, n, "%.1f", p->cpu);
        *a = p->cpu < 0.05 ? A_GRAY : p->cpu >= 85 ? CP(C_CRIT) | A_BOLD : p->cpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD;
        break;
    case K_MEM:
        sfmt(o, n, "%.1f", p->mem);
        *a = p->mem < 0.05 ? A_GRAY : p->mem >= 25 ? CP(C_WARN) | A_BOLD : A_NORMAL;
        break;
    case K_VIRT: fmt_bytes((double)p->vsize, o, n); *a = A_GRAY; break;
    case K_RSS: fmt_bytes((double)p->rss, o, n); break;
    case K_RD:
    case K_WR: {
        double v = k == K_RD ? p->rd_bps : p->wr_bps;
        if (!p->io_ok) {
            str_copy(o, n, p->kthread ? "" : "-");
            *a = A_GRAY;
        } else if (v <= 0) {
            str_copy(o, n, "0");
            *a = A_GRAY;
        } else {
            fmt_rate(v, o, n);
            *a = (k == K_RD ? CP(C_INFO) : CP(C_ACC)) | A_BOLD;
        }
        break;
    }
    case K_TIME: fmt_time(p->ticks, hz, o, n); *a = A_GRAY; break;
    default: o[0] = '\0';
    }
}

static void draw_cmd(int y, int x, int w, proc_t *p, int full, attr_t base, int sel)
{
    if (w <= 0)
        return;
    const char *cmd = p->kthread ? "" : proc_cmdline(p);
    if (!*cmd) {
        put(y, x, w, sel ? base : A_GRAY, "[%s]", p->comm);
        return;
    }
    size_t a0 = strcspn(cmd, " ");
    const char *slash = memrchr(cmd, '/', a0);
    const char *bn = slash ? slash + 1 : cmd;
    int xx = x;
    if (full && bn > cmd)
        xx += put(y, xx, x + w - xx, sel ? base : A_GRAY, "%.*s", (int)(bn - cmd), cmd);
    xx += put(y, xx, x + w - xx, base | A_BOLD, "%.*s", (int)(cmd + a0 - bn), bn);
    put(y, xx, x + w - xx, sel ? base : A_NORMAL, "%s", cmd + a0);
}

void draw_procs(sys_t *s, ui_state_t *u, int y, int x, int h, int w, int full)
{
    procs_t *ps = &s->procs;
    static int *ord;
    static int ocap;
    if (h < 3)
        return;
    if (ocap < ps->n) {
        int nc = ps->n + 256;
        int *no = realloc(ord, (size_t)nc * sizeof *no);
        if (!no)
            return;
        ord = no;
        ocap = nc;
    }
    int n = 0;
    for (int i = 0; i < ps->n; i++) {
        proc_t *p = &ps->p[i];
        if (p->kthread && !u->show_kthreads)
            continue;
        if (!proc_match(p, u->filter))
            continue;
        ord[n++] = i;
    }
    g_sort = u->sort;
    g_rev = u->reverse;
    g_pa = ps->p;
    qsort(ord, (size_t)n, sizeof *ord, cmp_proc);

    char info[256] = "";
    app(info, sizeof info, "%d tasks, %d thr, %d running", ps->n, ps->threads, ps->running);
    if (ps->diskwait)
        app(info, sizeof info, ", %d D", ps->diskwait);
    if (ps->zombie)
        app(info, sizeof info, ", %d zombie", ps->zombie);
    app(info, sizeof info, "%ssort %s%s", SEP, SORT_NAMES[u->sort], u->reverse ? " (rev)" : "");
    if (u->follow)
        app(info, sizeof info, "%sfollow", SEP);
    if (u->filter[0])
        app(info, sizeof info, "%sfilter \"%s\": %d", SEP, u->filter, n);
    if (ps->io_denied && geteuid() != 0)
        app(info, sizeof info, "%sI/O of other users' processes needs root", SEP);
    box_(y, x, h, w, "Processes", info);

    int ix = x + 1, iw = w - 2, iy = y + 1, ih = h - 2, rows = ih - 1;
    if (rows < 1)
        return;
    u->page_rows = rows;

    /* selection: by row position, or glued to the process with follow on */
    int sel = -1;
    if (u->follow && u->sel_pid)
        for (int k = 0; k < n; k++)
            if (ps->p[ord[k]].pid == u->sel_pid) {
                sel = k;
                break;
            }
    if (sel < 0)
        sel = u->sel_idx;
    sel += u->sel_delta;
    u->sel_delta = 0;
    if (sel >= n)
        sel = n - 1;
    if (sel < 0)
        sel = 0;
    u->sel_idx = sel;
    u->sel_pid = n ? ps->p[ord[sel]].pid : 0;
    if (n) { /* remembered for the signal dialog */
        proc_t *sp = &ps->p[ord[sel]];
        u->sel_start = sp->start;
        u->sel_kthread = sp->kthread;
        str_copy(u->sel_user, sizeof u->sel_user, uid_name(sp->uid));
        str_copy(u->sel_desc, sizeof u->sel_desc, sp->kthread ? "" : proc_cmdline(sp));
        if (!u->sel_desc[0])
            sfmt(u->sel_desc, sizeof u->sel_desc, "[%s]", sp->comm);
    } else {
        u->sel_start = 0;
        u->sel_desc[0] = '\0';
    }
    if (sel < u->scroll)
        u->scroll = sel;
    if (sel >= u->scroll + rows)
        u->scroll = sel - rows + 1;
    u->scroll = CLAMP(u->scroll, 0, MAX(0, n - rows));

    /* choose columns, dropping the least important ones on narrow screens */
    int cols[16], nc = 0;
    if (full) {
        static const int fc[] = { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof fc / sizeof fc[0]; i++)
            cols[nc++] = fc[i];
    } else {
        static const int dc[] = { K_PID, K_USER, K_S, K_CPU, K_MEM, K_RSS, K_RD, K_WR, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof dc / sizeof dc[0]; i++)
            cols[nc++] = dc[i];
    }
    static const int drop[] = { K_PRI, K_VIRT, K_NI, K_TIME, K_THR, K_WR, K_RD, K_USER };
    for (size_t d = 0; d < sizeof drop / sizeof drop[0]; d++) {
        int used = 0;
        for (int i = 0; i < nc; i++)
            used += COLDEF[cols[i]].w + 1;
        if (iw - used >= (iw >= 100 ? 40 : 24)) /* room left for COMMAND */
            break;
        for (int i = 0; i < nc; i++)
            if (cols[i] == drop[d]) {
                memmove(&cols[i], &cols[i + 1], (size_t)(nc - i - 1) * sizeof cols[0]);
                nc--;
                break;
            }
    }

    /* header */
    fill(iy, ix, iw, CP(C_THEAD));
    int cx = ix;
    for (int i = 0; i < nc; i++) {
        int k = cols[i], cw = k == K_CMD ? ix + iw - cx : COLDEF[k].w;
        attr_t a = CP(C_THEAD) | (COLDEF[k].sort == u->sort ? A_BOLD | A_UNDERLINE : 0);
        if (COLDEF[k].right)
            put(iy, cx, cw, a, "%*s", cw, COLDEF[k].h);
        else
            put(iy, cx, cw, a, "%-*s", cw, COLDEF[k].h);
        cx += cw + 1;
    }

    /* rows */
    for (int r = 0; r < rows; r++) {
        int k = u->scroll + r;
        if (k >= n)
            break;
        proc_t *p = &ps->p[ord[k]];
        int yy = iy + 1 + r, issel = k == sel;
        attr_t base = issel ? CP(C_SEL) : A_NORMAL;
        if (issel)
            fill(yy, ix, iw, base);
        cx = ix;
        for (int i = 0; i < nc; i++) {
            int c = cols[i];
            if (c == K_CMD) {
                draw_cmd(yy, cx, ix + iw - cx, p, full, base, issel);
                break;
            }
            char t[48];
            attr_t a;
            cell_text(p, c, ps->hz, t, sizeof t, &a);
            if (issel)
                a = base | (a & A_BOLD);
            int cw = COLDEF[c].w;
            if (COLDEF[c].right)
                put(yy, cx, cw, a, "%*s", cw, t);
            else
                put(yy, cx, cw, a, "%-*.*s", cw, cw, t);
            cx += cw + 1;
        }
    }
    if (n > rows) {
        char m[48];
        sfmt(m, sizeof m, " %d-%d of %d ", u->scroll + 1, MIN(n, u->scroll + rows), n);
        put(y + h - 1, x + w - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}
