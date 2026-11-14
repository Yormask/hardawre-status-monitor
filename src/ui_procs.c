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
#include "log.h"
#include "ui_int.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ============================================================ Processes */

enum { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_GPU, K_FDS, K_TIME, K_CMD };
static const struct {
    const char *h;
    int w, right, sort;
} COLDEF[] = {
    [K_PID] = { "PID", 7, 1, SORT_PID },   [K_USER] = { "USER", 9, 0, -1 },    [K_PRI] = { "PRI", 3, 1, -1 },
    [K_NI] = { "NI", 3, 1, -1 },           [K_THR] = { "THR", 4, 1, -1 },      [K_S] = { "S", 1, 0, -1 },
    [K_CPU] = { "CPU%", 5, 1, SORT_CPU },  [K_MEM] = { "MEM%", 5, 1, SORT_MEM }, [K_VIRT] = { "VIRT", 6, 1, -1 },
    [K_RSS] = { "RSS", 6, 1, SORT_MEM },   [K_RD] = { "READ/s", 8, 1, SORT_IO }, [K_WR] = { "WRITE/s", 8, 1, SORT_IO },
    [K_GPU] = { "GPU%", 5, 1, SORT_GPU },  [K_FDS] = { "FILES", 5, 1, SORT_FILES },
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
    case SORT_GPU: r = cmp_dbl_desc(p->gpu, q->gpu); break;
    case SORT_FILES: r = cmp_dbl_desc(p->nfds, q->nfds); break;
    }
    if (!r && g_sort != SORT_MEM)
        r = cmp_dbl_desc((double)p->rss, (double)q->rss);
    if (!r)
        r = (p->pid > q->pid) - (p->pid < q->pid);
    return g_rev ? -r : r;
}

/* ------------------------------------- processes of one service (cgroup) */

static pid_t *g_cgp;
static int g_ncgp, g_cgcap;

static int cmp_pidv(const void *a, const void *b)
{
    pid_t x = *(const pid_t *)a, y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

static void cg_collect(const char *dir, int depth)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/cgroup.procs", dir);
    FILE *f = fopen(p, "re");
    if (f) {
        long v;
        while (fscanf(f, "%ld", &v) == 1) {
            if (g_ncgp >= g_cgcap) {
                int nc = g_cgcap ? g_cgcap * 2 : 256;
                pid_t *np = realloc(g_cgp, (size_t)nc * sizeof *np);
                if (!np)
                    break;
                g_cgp = np;
                g_cgcap = nc;
            }
            g_cgp[g_ncgp++] = (pid_t)v;
        }
        fclose(f);
    }
    if (depth >= 8)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_type == DT_DIR && e->d_name[0] != '.') {
            char sub[PATHBUF];
            if (sfmt(sub, sizeof sub, "%s/%s", dir, e->d_name) < (int)sizeof sub)
                cg_collect(sub, depth + 1);
        }
    closedir(d);
}

static int in_service(pid_t pid)
{
    return g_ncgp && bsearch(&pid, g_cgp, (size_t)g_ncgp, sizeof *g_cgp, cmp_pidv) != NULL;
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

/* ---------------------------------------------------------- process tree */

/* per row of the tree: depth, the levels whose line continues below, and
 * whether the row is only there to show where a match came from */
#define TREE_DEPTH 48
static unsigned char *g_depth, *g_last, *g_ctx;
static unsigned long long *g_cont;
static int g_tcap, g_tree_ok; /* g_tree_ok: the arrays above describe the rows drawn now */

static int find_pid(const procs_t *ps, pid_t pid) /* ps->p is sorted by PID */
{
    int lo = 0, hi = ps->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ps->p[mid].pid == pid)
            return mid;
        if (ps->p[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* Reorder ord[0..n) (the processes to show, already sorted) as a tree:
 * children under their parent, brothers in sort order. With ancestors set,
 * the parents of each process are added (greyed) so you see where it came
 * from. Returns the new row count. */
static int tree_order(procs_t *ps, int *ord, int n, int ancestors)
{
    static int *inc, *par, *kids, *kstart, *kn, *all, *st, *ci;
    static int cap;
    int N = ps->n;
    g_tree_ok = 0;
    if (N <= 0 || n <= 0)
        return n;
    if (cap < N) {
        int nc = N + 256;
#define GROW(p) do { void *q = realloc(p, (size_t)nc * sizeof *(p)); if (!q) return n; p = q; } while (0)
        GROW(inc); GROW(par); GROW(kids); GROW(kstart); GROW(kn); GROW(all); GROW(st); GROW(ci);
#undef GROW
        cap = nc;
    }
    if (g_tcap < N) {
        int nc = N + 256;
        void *a = realloc(g_depth, (size_t)nc), *b = realloc(g_last, (size_t)nc), *c = realloc(g_ctx, (size_t)nc);
        g_depth = a ? a : g_depth;
        g_last = b ? b : g_last;
        g_ctx = c ? c : g_ctx;
        void *d = realloc(g_cont, (size_t)nc * sizeof *g_cont);
        g_cont = d ? d : g_cont;
        if (!a || !b || !c || !d)
            return n;
        g_tcap = nc;
    }
    memset(inc, 0, (size_t)N * sizeof *inc);
    for (int k = 0; k < n; k++)
        inc[ord[k]] = 1;
    if (ancestors)
        for (int k = 0; k < n; k++)
            for (int j = find_pid(ps, ps->p[ord[k]].ppid), guard = 0; j >= 0 && !inc[j] && guard < 64; guard++) {
                inc[j] = 2;
                j = ps->p[j].ppid != ps->p[j].pid ? find_pid(ps, ps->p[j].ppid) : -1;
            }
    int na = 0;
    for (int i = 0; i < N; i++)
        if (inc[i])
            all[na++] = i;
    if (na > 1)
        qsort(all, (size_t)na, sizeof *all, cmp_proc); /* brothers come out in sort order */
    for (int i = 0; i < N; i++)
        kn[i] = 0;
    for (int k = 0; k < na; k++) {
        int i = all[k], p = find_pid(ps, ps->p[i].ppid);
        par[i] = p >= 0 && inc[p] && p != i ? p : -1;
        if (par[i] >= 0)
            kn[par[i]]++;
    }
    for (int i = 0, sum = 0; i < N; i++) {
        kstart[i] = sum;
        sum += kn[i];
        kn[i] = 0;
    }
    for (int k = 0; k < na; k++) {
        int i = all[k];
        if (par[i] >= 0)
            kids[kstart[par[i]] + kn[par[i]]++] = i;
    }
    /* depth-first walk from each root */
    int out = 0;
    unsigned long long cont = 0;
    for (int k = 0; k < na; k++) {
        int r = all[k];
        if (par[r] >= 0)
            continue;
        g_depth[out] = 0;
        g_last[out] = 1;
        g_cont[out] = 0;
        g_ctx[out] = inc[r] == 2;
        ord[out++] = r;
        int sp = 0;
        st[sp] = r;
        ci[sp++] = 0;
        while (sp > 0 && out < N) {
            int node = st[sp - 1];
            if (ci[sp - 1] >= kn[node]) {
                sp--;
                continue;
            }
            int c = kids[kstart[node] + ci[sp - 1]++];
            int depth = sp, last = ci[sp - 1] == kn[node];
            g_depth[out] = (unsigned char)MIN(depth, 255);
            g_last[out] = (unsigned char)last;
            g_cont[out] = cont & ((1ULL << MIN(depth, 63)) - 1);
            g_ctx[out] = inc[c] == 2;
            ord[out++] = c;
            if (depth < 63) { /* below this row, the line at this level goes on unless it was the last */
                if (last)
                    cont &= ~(1ULL << depth);
                else
                    cont |= 1ULL << depth;
            }
            if (sp < N) {
                st[sp] = c;
                ci[sp++] = 0;
            }
        }
    }
    g_tree_ok = 1;
    return out;
}

/* "│  ├─ " in front of a process in the tree; returns columns used */
static int tree_prefix(int y, int x, int w, int row, attr_t a)
{
    int d = g_depth[row], used = 0;
    if (!d)
        return 0;
    int shown = MIN(d, TREE_DEPTH);
    for (int l = 1; l < shown && used + 3 <= w; l++, used += 3)
        put(y, x + used, 3, a, "%s", (g_cont[row] >> l) & 1 ? (U ? "│  " : "|  ") : "   ");
    if (used + 3 <= w) {
        put(y, x + used, 3, a, "%s", g_last[row] ? (U ? "└─ " : "`- ") : (U ? "├─ " : "|- "));
        used += 3;
    }
    return used;
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
    case K_CPU: {
        int flagged;
        sfmt(o, n, "%.1f", p->cpu);
        *a = p->cpu < 0.05 ? A_GRAY : p->cpu >= 85 ? CP(C_CRIT) | A_BOLD : p->cpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD;
        if (runaway_minutes(p->pid, p->start, &flagged) && flagged) /* busy for a long time: see the events */
            sfmt(o, n, "%.0f!", p->cpu); /* "100!": the column has room for 5 */
        break;
    }
    case K_MEM:
        sfmt(o, n, "%.1f", p->mem);
        *a = p->mem < 0.05 ? A_GRAY : p->mem >= 25 ? CP(C_WARN) | A_BOLD : A_NORMAL;
        break;
    case K_VIRT: fmt_bytes((double)p->vsize, o, n); *a = A_GRAY; break;
    case K_RSS:
        fmt_bytes((double)p->rss, o, n);
        if (leak_growing(p->pid, p->start)) { /* only ever grows: see the events, or Enter */
            app(o, n, "%s", U ? "↑" : "^");
            *a = CP(C_WARN) | A_BOLD;
        }
        break;
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
            *a = (k == K_RD ? CP(C_IN) : CP(C_OUT)) | A_BOLD;
        }
        break;
    }
    case K_GPU:
        if (p->gpu < 0) {
            str_copy(o, n, p->kthread ? "" : "-");
            *a = A_GRAY;
        } else {
            sfmt(o, n, "%.1f", p->gpu);
            *a = p->gpu < 0.05 ? A_GRAY : p->gpu >= 85 ? CP(C_CRIT) | A_BOLD : p->gpu >= 50 ? CP(C_WARN) | A_BOLD : A_BOLD;
        }
        break;
    case K_FDS: /* thousands of open files usually means a leak */
        if (p->nfds < 0) {
            str_copy(o, n, p->kthread ? "" : "-");
            *a = A_GRAY;
        } else {
            sfmt(o, n, "%d", p->nfds);
            *a = p->nfds >= 10000 ? CP(C_CRIT) | A_BOLD : p->nfds >= 1000 ? CP(C_WARN) | A_BOLD : A_NORMAL;
        }
        break;
    case K_TIME: fmt_time(p->ticks, hz, o, n); *a = A_GRAY; break;
    default: o[0] = '\0';
    }
}

static void draw_cmd(int y, int x, int w, proc_t *p, int full, attr_t base, int sel, int row, int tree)
{
    if (w <= 0)
        return;
    if (tree) {
        int m = tree_prefix(y, x, w, row, sel ? base : A_GRAY);
        x += m;
        w -= m;
        if (g_ctx[row] && !sel) { /* only here to show where a match came from: greyed */
            const char *c = p->kthread ? "" : proc_cmdline(p);
            if (!*c) {
                put(y, x, w, A_GRAY, "[%s]", p->comm);
                return;
            }
            const char *sl = memrchr(c, '/', strcspn(c, " "));
            put(y, x, w, A_GRAY, "%s", sl ? sl + 1 : c);
            return;
        }
        full = 0; /* the tree shows the family; the program's path takes too much room */
    }
    if (g_log.nplog && log_proc_find(p->pid, p->start) >= 0) { /* being logged */
        int m = put(y, x, w, sel ? base | A_BOLD : CP(C_CRIT) | A_BOLD, "%s ", U ? "●" : "*");
        x += m;
        w -= m;
    }
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
    g_ncgp = 0;
    if (u->svc_path[0] && s->svc.root[0]) { /* one service's processes (Enter in view 6) */
        char dir[PATHBUF];
        sfmt(dir, sizeof dir, "%s/%s", s->svc.root, u->svc_path);
        cg_collect(dir, 0);
        if (g_ncgp > 1)
            qsort(g_cgp, (size_t)g_ncgp, sizeof *g_cgp, cmp_pidv);
    }
    int n = 0;
    for (int i = 0; i < ps->n; i++) {
        proc_t *p = &ps->p[i];
        if (p->kthread && !u->show_kthreads && !u->svc_path[0])
            continue;
        if (!proc_match(p, u->filter))
            continue;
        if (u->svc_path[0] && !in_service(p->pid))
            continue;
        ord[n++] = i;
    }
    g_sort = u->sort;
    g_rev = u->reverse;
    g_pa = ps->p;
    if (n > 1)
        qsort(ord, (size_t)n, sizeof *ord, cmp_proc);
    int matches = n;
    if (u->tree) /* a filter keeps the parents in view, greyed; a service's processes stand alone */
        n = tree_order(ps, ord, n, u->filter[0] && !u->svc_path[0]);

    char info[256] = "";
    app(info, sizeof info, "%d tasks, %d thr, %d running", ps->n, ps->threads, ps->running);
    if (ps->diskwait)
        app(info, sizeof info, ", %d D", ps->diskwait);
    if (ps->zombie)
        app(info, sizeof info, ", %d zombie", ps->zombie);
    app(info, sizeof info, "%ssort %s%s", SEP, SORT_NAMES[u->sort], u->reverse ? " (rev)" : "");
    if (u->tree)
        app(info, sizeof info, "%stree", SEP);
    if (u->follow)
        app(info, sizeof info, "%sfollow", SEP);
    if (u->filter[0])
        app(info, sizeof info, "%sfilter \"%s\": %d", SEP, u->filter, matches);
    if (u->svc_path[0])
        app(info, sizeof info, "%s%s: %d (Esc: all)", SEP, u->svc_name, matches);
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
    int moved = u->sel_delta;
    sel += u->sel_delta;
    u->sel_delta = 0;
    if (sel >= n)
        sel = n - 1;
    if (sel < 0)
        sel = 0;
    int tree = u->tree && g_tree_ok;
    if (tree && n > matches) { /* greyed parents can't be selected: on to the nearest match */
        int dir = moved < 0 ? -1 : 1, k = sel;
        while (k >= 0 && k < n && g_ctx[k])
            k += dir;
        if (k < 0 || k >= n)
            for (k = sel; k >= 0 && k < n && g_ctx[k]; k -= dir)
                ;
        if (k >= 0 && k < n)
            sel = k;
    }
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
    int cols[20], nc = 0;
    static int gpu_seen; /* once a GPU client showed up, keep the column */
    gpu_seen |= ps->gpu_any;
    if (full) {
        static const int fc[] = { K_PID, K_USER, K_PRI, K_NI, K_THR, K_S, K_CPU, K_MEM, K_VIRT, K_RSS, K_RD, K_WR, K_GPU, K_FDS, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof fc / sizeof fc[0]; i++)
            if (fc[i] != K_GPU || gpu_seen)
                cols[nc++] = fc[i];
    } else {
        static const int dc[] = { K_PID, K_USER, K_S, K_CPU, K_MEM, K_RSS, K_RD, K_WR, K_GPU, K_TIME, K_CMD };
        for (size_t i = 0; i < sizeof dc / sizeof dc[0]; i++)
            if (dc[i] != K_GPU || gpu_seen)
                cols[nc++] = dc[i];
    }
    static const int drop[] = { K_PRI, K_VIRT, K_FDS, K_NI, K_TIME, K_THR, K_GPU, K_WR, K_RD, K_USER };
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

    /* header; a click on a heading sorts by it */
    fill(iy, ix, iw, CP(C_THEAD));
    int cx = ix;
    for (int i = 0; i < nc; i++) {
        int k = cols[i], cw = k == K_CMD ? ix + iw - cx : COLDEF[k].w;
        attr_t a = CP(C_THEAD) | (COLDEF[k].sort == u->sort ? A_BOLD | A_UNDERLINE : 0);
        const char *hd = k == K_CMD && u->tree ? "COMMAND (tree)" : COLDEF[k].h;
        if (COLDEF[k].right)
            put(iy, cx, cw, a, "%*s", cw, hd);
        else
            put(iy, cx, cw, a, "%-*s", cw, hd);
        if (COLDEF[k].sort >= 0)
            hit_add(iy, cx, 1, cw + 1, HIT_PROC_SORT, COLDEF[k].sort, 0);
        cx += cw + 1;
    }
    hit_add(iy + 1, ix, rows, iw, HIT_SCROLL, 0, 0);

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
        hit_add(yy, ix, 1, iw, HIT_PROC, k, 0);
        cx = ix;
        for (int i = 0; i < nc; i++) {
            int c = cols[i];
            if (c == K_CMD) {
                draw_cmd(yy, cx, ix + iw - cx, p, full, base, issel, k, tree);
                break;
            }
            char t[48];
            attr_t a;
            cell_text(p, c, ps->hz, t, sizeof t, &a);
            if (issel)
                a = base | (a & A_BOLD);
            int cw = COLDEF[c].w;
            if (COLDEF[c].right) /* printf pads by bytes: make up for multi-byte characters */
                put(yy, cx, cw, a, "%*s", cw + (int)strlen(t) - utf8_cols(t), t);
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
