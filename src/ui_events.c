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
/* ui_events.c - view 7, Events: everything hsm noticed, kept across runs
 * and reboots (events.log), searchable; view 8, History: one line per day
 * (history.csv) and how things changed over the weeks. */
#define _GNU_SOURCE
#include "history.h"
#include "ui_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* =============================================================== Events */

/* an event as kept for the view: the text in its own allocation, so a
 * long events.log costs what it holds, not 256 bytes a line */
typedef struct {
    long long t;
    int order, level;
    char cat[12];
    char *text;
} vev_t;

static int *g_idx; /* the shown ones, newest first */
static int g_nidx, g_cidx;

static struct {
    vev_t *e; /* oldest first */
    int n, cap;
    time_t mtime[2];
    off_t size[2];
    ino_t ino[2];
    off_t done; /* events.log read up to here (whole lines) */
    double checked;
    int from_file;
} E;


static int by_time(const void *a, const void *b)
{
    const vev_t *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : x->order - y->order;
}

static void ev_clear(void)
{
    for (int i = 0; i < E.n; i++)
        free(E.e[i].text);
    E.n = 0;
}

static void ev_push(const hevent_t *x)
{
    if (E.n == E.cap) {
        int nc = E.cap ? E.cap * 2 : 1024;
        vev_t *ne = realloc(E.e, (size_t)nc * sizeof *ne);
        if (!ne)
            return;
        E.e = ne;
        E.cap = nc;
    }
    vev_t *v = &E.e[E.n];
    v->text = strdup(x->text);
    if (!v->text)
        return;
    v->t = x->t;
    v->level = x->level;
    str_copy(v->cat, sizeof v->cat, x->cat);
    v->order = E.n;
    E.n++;
}

/* leaving the view: the memory goes back (a 4 MB events.log is many lines) */
void events_release(void)
{
    ev_clear();
    free(E.e);
    E.e = NULL;
    E.cap = 0;
    E.from_file = 0;
    E.checked = 0;
    E.done = 0;
    memset(E.mtime, 0, sizeof E.mtime);
    memset(E.size, 0, sizeof E.size);
    free(g_idx);
    g_idx = NULL;
    g_nidx = g_cidx = 0;
}

/* the whole lines from 'from' on; returns where they end */
static off_t read_file_events(const char *path, off_t from)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return from;
    if (from > 0 && fseeko(f, from, SEEK_SET) != 0) {
        fclose(f);
        return from;
    }
    char line[512];
    off_t end = from;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (!l || line[l - 1] != '\n') { /* being written right now, or too long: skip what's there */
            if (l == sizeof line - 1) { /* a very long line: its rest goes too */
                int c;
                while ((c = fgetc(f)) != EOF && c != '\n')
                    ;
                end = ftello(f);
            }
            continue;
        }
        end = ftello(f);
        line[l - 1] = '\0';
        hevent_t x;
        if (history_parse_event(line, &x) == 0)
            ev_push(&x);
    }
    fclose(f);
    return end;
}

/* the files again when they changed; without them, what this hsm saw */
static void ev_load(sys_t *s)
{
    double now = now_sec();
    if (E.checked > 0 && now - E.checked < 1.0)
        return;
    E.checked = now;
    char p[2][PATHBUF];
    sfmt(p[0], sizeof p[0], "%s/events.log.1", config_state_dir());
    sfmt(p[1], sizeof p[1], "%s/events.log", config_state_dir());
    struct stat st[2];
    int ok[2], same = E.from_file;
    for (int i = 0; i < 2; i++) {
        ok[i] = stat(p[i], &st[i]) == 0;
        if (!ok[i])
            st[i].st_mtime = 0, st[i].st_size = 0, st[i].st_ino = 0;
        same &= st[i].st_mtime == E.mtime[i] && st[i].st_size == E.size[i];
    }
    if (same)
        return;
    /* only new lines at the end of events.log: read just those */
    int grew = E.from_file && ok[1] && st[0].st_ino == E.ino[0] && st[0].st_size == E.size[0] &&
               st[1].st_ino == E.ino[1] && st[1].st_size >= E.done;
    if (!grew) {
        ev_clear();
        E.done = 0;
    }
    E.from_file = ok[1];
    for (int i = 0; i < 2; i++) {
        if (ok[i] && i == 1)
            E.done = read_file_events(p[i], E.done);
        else if (ok[i] && !grew)
            read_file_events(p[i], 0);
        E.mtime[i] = st[i].st_mtime;
        E.size[i] = st[i].st_size;
        E.ino[i] = st[i].st_ino;
    }
    if (!E.from_file) { /* nothing on disk (yet): this run's events */
        const health_t *h = &s->health;
        for (int k = h->nev - 1; k >= 0; k--) {
            const event_t *e = health_event_at(h, k);
            hevent_t x = { (long long)e->t, 0, e->level, "", "" };
            str_copy(x.cat, sizeof x.cat, e->cat);
            str_copy(x.text, sizeof x.text, e->text);
            ev_push(&x);
        }
        E.checked = 0; /* in memory: look again next time */
    }
    /* the file is in the order things were written down: the kernel log's
     * backlog comes in at start, after newer lines of earlier runs */
    if (E.n > 1)
        qsort(E.e, (size_t)E.n, sizeof *E.e, by_time);
}

static int ev_match(const vev_t *x, const ui_state_t *u)
{
    if (x->level < u->ev_level)
        return 0;
    if (!u->ev_filter[0])
        return 1;
    return strcasestr(x->text, u->ev_filter) || strcasestr(x->cat, u->ev_filter);
}

static void day_label(long long t, char *out, size_t n)
{
    time_t tt = (time_t)t, now = time(NULL);
    struct tm tm, tn;
    localtime_r(&tt, &tm);
    localtime_r(&now, &tn);
    int days = (int)((now - tt) / 86400);
    if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday)
        str_copy(out, n, "Today");
    else if (days < 2 && ((tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday - 1) || (tn.tm_yday == 0 && tm.tm_mon == 11 && tm.tm_mday == 31)))
        str_copy(out, n, "Yesterday");
    else
        strftime(out, n, "%a %d %b %Y", &tm);
}

static const char *const LEVEL_NAME[3] = { "info", "warn", "error" };

void events_view(sys_t *s, ui_state_t *u)
{
    ev_load(s);
    if (g_cidx < E.n) {
        int nc = E.n + 256;
        int *ni = realloc(g_idx, (size_t)nc * sizeof *ni);
        if (!ni)
            return;
        g_idx = ni;
        g_cidx = nc;
    }
    g_nidx = 0;
    int err24 = 0, warn24 = 0, err7 = 0, warn7 = 0;
    long long now = (long long)time(NULL);
    for (int i = E.n - 1; i >= 0; i--) {
        const vev_t *x = &E.e[i];
        if (now - x->t < 86400) {
            err24 += x->level == EV_ERR;
            warn24 += x->level == EV_WARN;
        }
        if (now - x->t < 7 * 86400) {
            err7 += x->level == EV_ERR;
            warn7 += x->level == EV_WARN;
        }
        if (ev_match(x, u))
            g_idx[g_nidx++] = i;
    }
    char info[200] = "";
    if (E.n) {
        char since[32];
        time_t t0 = (time_t)E.e[0].t;
        struct tm tm;
        localtime_r(&t0, &tm);
        strftime(since, sizeof since, "%d %b %Y", &tm);
        sfmt(info, sizeof info, "%d since %s", E.n, since);
    }
    static const char *const SHOW[3] = { "all", "warnings and errors", "errors only" };
    app(info, sizeof info, "%sshowing %s (e)", SEP, SHOW[CLAMP(u->ev_level, 0, 2)]);
    if (u->ev_filter[0])
        app(info, sizeof info, "%sfilter \"%s\": %d", SEP, u->ev_filter, g_nidx);
    int top = 1, H = LINES - 2, W = COLS;
    box_(top, 0, H, W, "Events", info);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2;
    if (ih < 5)
        return;
    /* the last day and week in one line */
    int xx = ix;
    xx += put(iy, xx, iw, A_GRAY, "Last 24 h: ");
    xx += put(iy, xx, ix + iw - xx, err24 ? CP(C_CRIT) | A_BOLD : CP(C_OK), "%d error%s", err24, err24 == 1 ? "" : "s");
    xx += put(iy, xx, ix + iw - xx, warn24 ? CP(C_WARN) : A_GRAY, ", %d warning%s", warn24, warn24 == 1 ? "" : "s");
    xx += put(iy, xx, ix + iw - xx, A_GRAY, "   last 7 days: ");
    xx += put(iy, xx, ix + iw - xx, err7 ? CP(C_CRIT) : A_GRAY, "%d error%s", err7, err7 == 1 ? "" : "s");
    put(iy, xx, ix + iw - xx, warn7 ? CP(C_WARN) : A_GRAY, ", %d warning%s", warn7, warn7 == 1 ? "" : "s");
    if (!E.from_file)
        put(iy + 1, ix, iw, A_GRAY, "%s",
            history_writer() || !E.n ? "Saved in events.log from now on; nothing saved before this run"
                                     : "Only this run: events.log can't be read");
    int ly = iy + 2, detail = 5, rows = ih - 2 - detail;
    if (!g_nidx) {
        put(ly, ix, iw, A_GRAY, "%s",
            E.n ? "No events match. Esc clears the filter, e shows more levels."
                : "No events yet: throttling, freezes, kernel errors, full disks and the like will show up here.");
        return;
    }
    /* rows: day headings between the events; selection by event */
    u->ev_sel = CLAMP(u->ev_sel, 0, g_nidx - 1);
    /* build the visual lines: -1 - k = heading before event k */
    static int *vis;
    static int vcap;
    int need = g_nidx * 2 + 2;
    if (vcap < need) {
        int *nv = realloc(vis, (size_t)need * sizeof *nv);
        if (!nv)
            return;
        vis = nv;
        vcap = need;
    }
    int nv = 0, selrow = 0;
    char prev[32] = "";
    for (int k = 0; k < g_nidx; k++) {
        char dl[32];
        day_label(E.e[g_idx[k]].t, dl, sizeof dl);
        if (strcmp(dl, prev)) {
            vis[nv++] = -1 - k;
            str_copy(prev, sizeof prev, dl);
        }
        if (k == u->ev_sel)
            selrow = nv;
        vis[nv++] = k;
    }
    if (selrow < u->ev_scroll + 1) /* its day heading stays in view too */
        u->ev_scroll = MAX(0, selrow - 1);
    if (selrow >= u->ev_scroll + rows)
        u->ev_scroll = selrow - rows + 1;
    u->ev_scroll = CLAMP(u->ev_scroll, 0, MAX(0, nv - rows));
    hit_add(ly, ix, rows, iw, HIT_SCROLL, 0, 0);
    for (int r = 0; r < rows && u->ev_scroll + r < nv; r++) {
        int v = vis[u->ev_scroll + r], y = ly + r;
        if (v < 0) {
            char dl[32];
            day_label(E.e[g_idx[-1 - v]].t, dl, sizeof dl);
            put(y, ix, iw, CP(C_INFO) | A_BOLD, "%s", dl);
            continue;
        }
        const vev_t *x = &E.e[g_idx[v]];
        int sel = v == u->ev_sel;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(y, ix - 1, iw + 2, base);
        hit_add(y, ix, 1, iw, HIT_EVENT, v, 0);
        char ts[16];
        time_t tt = (time_t)x->t;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);
        int cx = ix + 2;
        cx += put(y, cx, 10, sel ? base : A_GRAY, "%-10s", ts);
        attr_t la = sel ? base | A_BOLD
                  : x->level == EV_ERR ? CP(C_CRIT) | A_BOLD
                  : x->level == EV_WARN ? CP(C_WARN) : A_GRAY;
        cx += put(y, cx, 7, la, "%-6s ", LEVEL_NAME[CLAMP(x->level, 0, 2)]);
        cx += put(y, cx, 10, sel ? base : la & ~A_BOLD, "%-9s ", x->cat);
        put(y, cx, ix + iw - cx, sel ? base | A_BOLD : A_NORMAL, "%s", x->text);
    }
    if (nv > rows) {
        char m[40];
        sfmt(m, sizeof m, " %d of %d ", u->ev_sel + 1, g_nidx);
        put(top + H - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
    /* the whole text of the selected one, which the list may cut off */
    const vev_t *x = &E.e[g_idx[u->ev_sel]];
    int dy = ly + rows;
    char full[400], ts[40];
    time_t tt = (time_t)x->t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(ts, sizeof ts, "%a %d %b %Y %H:%M:%S", &tm);
    sfmt(full, sizeof full, "%s  %s  %s: %s", ts, LEVEL_NAME[CLAMP(x->level, 0, 2)], x->cat, x->text);
    put(dy, ix, iw, A_GRAY, "%s", U ? "────" : "----");
    const char *q = full;
    int line = 1;
    for (; line < 3 && *q; line++) {
        size_t nb = utf8_bytes_for_cols(q, iw);
        put(dy + line, ix, iw, A_NORMAL, "%.*s", (int)nb, q);
        q += nb;
    }
    char ex[400]; /* what it means, for the kernel's cryptic ones */
    if (event_explain(x->cat, x->text, ex, sizeof ex))
        wrap_text(dy + line, ix, iw, detail - line, CP(C_INFO), ex);
}

int events_key(ui_state_t *u, int ch)
{
    switch (ch) {
    case KEY_UP: u->ev_sel--; break;
    case KEY_DOWN: u->ev_sel++; break;
    case KEY_PPAGE: u->ev_sel -= 10; break;
    case KEY_NPAGE: u->ev_sel += 10; break;
    case KEY_HOME: u->ev_sel = 0; break;
    case KEY_END: u->ev_sel = 1 << 20; break;
    case 'e':
        u->ev_level = (u->ev_level + 1) % 3;
        u->ev_sel = u->ev_scroll = 0;
        break;
    case 27:
        if (!u->ev_filter[0])
            return 0;
        u->ev_filter[0] = '\0';
        u->ev_sel = u->ev_scroll = 0;
        break;
    default: return 0;
    }
    u->ev_sel = CLAMP(u->ev_sel, 0, MAX(0, g_nidx - 1));
    return 1;
}

void events_click(ui_state_t *u, int k)
{
    u->ev_sel = k;
}

/* ============================================================== History */

static struct {
    hday_t *d; /* oldest first */
    int n, cap;
    time_t mtime;
    off_t size;
    double checked;
} Y;

static void hist_load(void)
{
    double now = now_sec();
    if (Y.checked > 0 && now - Y.checked < 2.0)
        return;
    Y.checked = now;
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/history.csv", config_state_dir());
    struct stat st;
    if (stat(p, &st) != 0) {
        Y.n = 0;
        Y.mtime = 0;
        return;
    }
    if (st.st_mtime == Y.mtime && st.st_size == Y.size && Y.n)
        return;
    Y.mtime = st.st_mtime;
    Y.size = st.st_size;
    Y.n = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        hday_t d;
        if (history_parse_day(line, &d) != 0)
            continue;
        if (Y.n == Y.cap) {
            int nc = Y.cap ? Y.cap * 2 : 128;
            hday_t *nd = realloc(Y.d, (size_t)nc * sizeof *nd);
            if (!nd)
                break;
            Y.d = nd;
            Y.cap = nc;
        }
        Y.d[Y.n++] = d;
    }
    fclose(f);
}

/* days as shown: the file, with today's figures as this hsm has them now */
static int days(hday_t **out)
{
    static hday_t *all;
    static int cap;
    hday_t t;
    int have = history_today(&t) == 0;
    if (cap < Y.n + 1) {
        hday_t *na = realloc(all, (size_t)(Y.n + 1) * sizeof *na);
        if (!na)
            return 0;
        all = na;
        cap = Y.n + 1;
    }
    int n = 0;
    for (int i = 0; i < Y.n; i++)
        if (!have || Y.d[i].day != t.day)
            all[n++] = Y.d[i];
    if (have) {
        int k = n;
        while (k > 0 && all[k - 1].day > t.day)
            k--;
        memmove(&all[k + 1], &all[k], (size_t)(n - k) * sizeof *all);
        all[k] = t;
        n++;
    }
    *out = all;
    return n;
}

typedef struct {
    const char *name, *unit;
    int dec;
    double (*get)(const hday_t *);
} metric_t;

/* "40 s", "25 min", "2.2 h" */
static void fmt_busy(double s, char *out, size_t n)
{
    if (s < 90)
        sfmt(out, n, "%.0f s", s);
    else if (s < 90 * 60)
        sfmt(out, n, "%.0f min", s / 60);
    else
        sfmt(out, n, "%.1f h", s / 3600);
}

/* what ran on the selected day: the programs that used the most */
static void day_programs(const hday_t *d, int y, int x, int h, int w)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = d->day / 10000 - 1900;
    tm.tm_mon = d->day / 100 % 100 - 1;
    tm.tm_mday = d->day % 100;
    tm.tm_hour = 12;
    mktime(&tm);
    char dl[24];
    strftime(dl, sizeof dl, "%a %d %b", &tm);
    prog_day_t pd;
    if (h < 1)
        return;
    if (prog_get_day(d->day, &pd) != 0) {
        put(y, x, w, A_GRAY, "No programs counted on %s (hsm 1.9 and later count them while it keeps the history)", dl);
        return;
    }
    put(y, x, w, A_GRAY | A_BOLD, "What ran on %s, by", dl);
    static const char *const kind[PROG_KINDS] = { "CPU time", "GPU time", "Written" };
    int lw = 12;
    for (int k = 0, r = 1; k < PROG_KINDS && r < h + 1; k++) {
        if (!pd.n[k])
            continue;
        int yy = y + r++, xx = x + lw, xm = x + w;
        put(yy, x + 2, lw - 2, CP(C_INFO), "%s", kind[k]);
        for (int i = 0; i < pd.n[k]; i++) {
            char v[24], item[64];
            if (k == 2)
                fmt_bytes(pd.top[k][i].v, v, sizeof v);
            else
                fmt_busy(pd.top[k][i].v, v, sizeof v);
            sfmt(item, sizeof item, "%s%s ", i ? SEP : "", pd.top[k][i].name);
            int need = utf8_cols(item) + utf8_cols(v);
            if (xx + need > xm)
                break;
            xx += put(yy, xx, xm - xx, A_BOLD, "%s", item);
            xx += put(yy, xx, xm - xx, A_NORMAL, "%s", v);
        }
    }
}

static double g_temp_max(const hday_t *d) { return d->temp_max; }
static double g_temp_avg(const hday_t *d) { return d->temp_avg; }
static double g_cpu_avg(const hday_t *d) { return d->cpu_avg; }
static double g_written(const hday_t *d) { return d->hours > 0 ? d->written_gb * 24.0 / MAX(d->hours, 1.0) : NAN; }
static double g_wear(const hday_t *d) { return d->wear; }
static double g_errors(const hday_t *d) { return (double)d->errors; }
static double g_drive_t(const hday_t *d) { return d->drive_temp_max; }
static double g_battery(const hday_t *d) { return d->battery; }
static double g_fullest(const hday_t *d) { return d->fullest; }

/* YYYYMMDD -> days since 1970 (proleptic Gregorian) */
static long dnum(int ymd)
{
    long y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* the mean of a metric over the days [a, b) before today */
static double span_mean(const hday_t *D, int n, long today, int a, int b, double (*get)(const hday_t *), int *cnt)
{
    double s = 0;
    int k = 0;
    for (int i = n - 1; i >= 0; i--) {
        long back = today - dnum(D[i].day);
        if (back < a)
            continue;
        if (back >= b)
            break;
        double v = get(&D[i]);
        if (!isnan(v)) {
            s += v;
            k++;
        }
    }
    *cnt = k;
    return k ? s / k : NAN;
}

void history_view(sys_t *s, ui_state_t *u)
{
    hist_load();
    hday_t *D;
    int n = days(&D);
    char info[160];
    sfmt(info, sizeof info, "%d day%s", n, n == 1 ? "" : "s");
    if (n)
        app(info, sizeof info, "%s%s", SEP, history_writer() ? "this hsm keeps it" : "another hsm keeps it");
    int top = 1, H = LINES - 2, W = COLS;
    box_(top, 0, H, W, "History", info);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2;
    if (ih < 6)
        return;
    if (!n) {
        put(iy, ix, iw, A_BOLD, "%s", "No history yet.");
        put(iy + 2, ix, iw, A_NORMAL, "%s",
            "While it runs, hsm keeps one line per day: averages, peaks and totals, in history.csv next to");
        put(iy + 3, ix, iw, A_NORMAL, "%s",
            "events.log. Today's line appears within 10 minutes. Run hsm --watch as a service (see the README)");
        put(iy + 4, ix, iw, A_NORMAL, "%s", "so whole days are covered, not only the hours the screen is open.");
        return;
    }

    /* trends: a column per day, the last week against a month before */
    static const metric_t M[] = {
        { "CPU temp, peak", "°C", 0, g_temp_max }, { "CPU temp, avg", "°C", 0, g_temp_avg },
        { "CPU use, avg", "%", 0, g_cpu_avg },    { "Written per day", "G", 1, g_written },
        { "Drive temp, peak", "°C", 0, g_drive_t }, { "Drive wear", "%", 0, g_wear },
        { "Fullest disk", "%", 0, g_fullest },    { "Errors", "", 0, g_errors },
        { "Battery health", "%", 0, g_battery },
    };
    int nm = (int)(sizeof M / sizeof M[0]);
    int trows = MIN(nm, ih / 2 - 1), lw = 18, gw = MIN(60, iw - lw - 50);
    if (gw < 10)
        gw = MAX(0, iw - lw - 2), trows = MIN(trows, 3);
    put(iy, ix, iw, A_GRAY | A_BOLD, "Trends, a column per day (oldest left; gaps: days without hsm)");
    int ty = iy + 1, shown = 0;
    long today = dnum(log_day_key((double)time(NULL)));
    for (int m = 0; m < nm && shown < trows; m++) {
        int have = 0;
        for (int i = 0; i < n && !have; i++)
            have = !isnan(M[m].get(&D[i]));
        if (!have)
            continue;
        int y = ty + shown++;
        put(y, ix, lw, CP(C_INFO), "%s", M[m].name);
        /* calendar days: a day without a line stays empty */
        int k = (int)MIN((long)gw, today - dnum(D[0].day) + 1);
        float v[512];
        double lo = INFINITY, hi = -INFINITY;
        for (int c = 0; c < k; c++)
            v[c] = NAN;
        for (int i = 0; i < n; i++) {
            long c = k - 1 - (today - dnum(D[i].day));
            if (c < 0 || c >= k)
                continue;
            double x = M[m].get(&D[i]);
            v[c] = (float)x;
            if (!isnan(x)) {
                lo = MIN(lo, x);
                hi = MAX(hi, x);
            }
        }
        if (hi <= lo)
            hi = lo + 1;
        graph_cols(y, ix + lw, 1, k, v, lo - (hi - lo) * 0.1, hi, CP(C_IN), -1, 0);
        int c1, c2;
        double now7 = span_mean(D, n, today, 0, 7, M[m].get, &c1), then7 = span_mean(D, n, today, 28, 35, M[m].get, &c2);
        int tx = ix + lw + gw + 2;
        if (c1 && c2) {
            double dlt = now7 - then7;
            int bad = M[m].get == g_battery ? dlt < -1 : dlt > (M[m].get == g_errors ? 0.5 : 2);
            const char *un = !U && !strcmp(M[m].unit, "°C") ? "C" : M[m].unit;
            put(y, tx, ix + iw - tx, bad ? CP(C_WARN) | A_BOLD : A_NORMAL, "last week %.*f%s, a month before %.*f%s (%+.*f)",
                M[m].dec, now7, un, M[m].dec, then7, un, M[m].dec, dlt);
        } else if (c1) {
            put(y, tx, ix + iw - tx, A_NORMAL, "last week %.*f%s", M[m].dec, now7,
                !U && !strcmp(M[m].unit, "°C") ? "C" : M[m].unit);
        }
        /* how long the most worn drive has left at this pace */
        if (M[m].get == g_wear && n >= 14) {
            double first = NAN, last = NAN;
            long fi = -1, li = -1;
            for (int i = 0; i < n; i++)
                if (!isnan(D[i].wear)) {
                    if (fi < 0)
                        fi = dnum(D[i].day), first = D[i].wear;
                    li = dnum(D[i].day), last = D[i].wear;
                }
            if (li - fi >= 14 && last > first) {
                double per_day = (last - first) / (double)(li - fi), left = (100 - last) / per_day / 365.0;
                int ex = tx + 48;
                if (ix + iw - ex > 20)
                    put(y, ex, ix + iw - ex, A_GRAY, "100%% in about %.0f year%s at this pace", left, left < 1.5 ? "" : "s");
            }
        }
    }

    /* the days, newest first */
    int y0 = ty + shown + 1;
    if (y0 >= iy + ih - 2)
        return;
    static const struct {
        const char *h;
        int w;
    } C[] = { { "Day", 12 },  { "Hours", 6 },      { "CPU avg", 8 },  { "max", 5 },    { "Temp", 6 },
              { "max", 5 },   { "Mem avg", 8 },    { "max", 5 },      { "Power", 6 },  { "Written", 8 },
              { "Net in", 7 }, { "Errors", 7 },    { "Warn", 5 },     { "Freeze", 7 }, { "Wear", 5 },
              { "Fullest", 8 } };
    int nc = (int)(sizeof C / sizeof C[0]), xx = ix;
    for (int c = 0; c < nc && xx + C[c].w <= ix + iw; c++) {
        put(y0, xx, C[c].w, CP(C_THEAD), c ? "%*s" : "%-*s", C[c].w, C[c].h);
        put(y0, xx + C[c].w, 1, CP(C_THEAD), " ");
        xx += C[c].w + 1;
    }
    if (xx < ix + iw)
        fill(y0, xx, ix + iw - xx, CP(C_THEAD));
    int rows = iy + ih - y0 - 1;
    int prog_h = rows >= 8 ? 5 : 0; /* the selected day's programs, below the list */
    rows -= prog_h;
    u->hist_sel = CLAMP(u->hist_sel, 0, n - 1);
    if (u->hist_sel < u->hist_scroll)
        u->hist_scroll = u->hist_sel;
    if (u->hist_sel >= u->hist_scroll + rows)
        u->hist_scroll = u->hist_sel - rows + 1;
    u->hist_scroll = CLAMP(u->hist_scroll, 0, MAX(0, n - rows));
    hit_add(y0 + 1, ix, rows, iw, HIT_SCROLL, 0, 0);
    for (int r = 0; r < rows && u->hist_scroll + r < n; r++) {
        const hday_t *d = &D[n - 1 - u->hist_scroll - r];
        int y = y0 + 1 + r, cx = ix, sel = u->hist_scroll + r == u->hist_sel;
        hit_add(y, ix, 1, iw, HIT_HIST, u->hist_scroll + r, 0);
        if (sel)
            fill(y, ix, iw, CP(C_SEL));
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = d->day / 10000 - 1900;
        tm.tm_mon = d->day / 100 % 100 - 1;
        tm.tm_mday = d->day % 100;
        tm.tm_hour = 12;
        mktime(&tm);
        char dl[16];
        strftime(dl, sizeof dl, "%a %d %b", &tm);
        double vals[16] = { 0, d->hours, d->cpu_avg, d->cpu_max, d->temp_avg, d->temp_max, d->mem_avg, d->mem_max,
                            d->power_avg, d->written_gb, d->rx_gb, (double)d->errors, (double)d->warnings,
                            (double)d->freezes, d->wear, d->fullest };
        const char *deg = U ? "°" : "C";
        for (int c = 0; c < nc && cx + C[c].w <= ix + iw; c++) {
            int w = C[c].w;
            if (!c) {
                put(y, cx, w, sel ? CP(C_SEL) | A_BOLD : d->hours > 0 ? A_BOLD : A_GRAY, "%-*s", w, dl);
                cx += w + 1;
                continue;
            }
            double v = vals[c];
            char t[24];
            attr_t a = A_NORMAL;
            switch (c) {
            case 1: sfmt(t, sizeof t, "%.1f", v); break;
            case 4:
            case 5: sfmt(t, sizeof t, "%.0f%s", v, deg); break;
            case 8: sfmt(t, sizeof t, "%.0f W", v); break;
            case 9:
            case 10: sfmt(t, sizeof t, "%.1fG", v); break;
            case 11:
            case 12:
            case 13: sfmt(t, sizeof t, "%.0f", v); break;
            default: sfmt(t, sizeof t, "%.0f%%", v); break;
            }
            if (c == 5)
                a = temp_attr(v);
            else if (c == 11)
                a = v > 0 ? CP(C_CRIT) | A_BOLD : A_GRAY;
            else if (c == 12 || c == 13)
                a = v > 0 ? CP(C_WARN) : A_GRAY;
            else if (c == 15 && v >= 90)
                a = CP(C_WARN) | A_BOLD;
            if (isnan(v)) {
                str_copy(t, sizeof t, "-");
                a = A_GRAY;
            }
            if (sel)
                a = CP(C_SEL);
            int tc = utf8_cols(t);
            put(y, cx + MAX(0, w - tc), MIN(w, tc), a, "%s", t);
            cx += w + 1;
        }
    }
    if (prog_h)
        day_programs(&D[n - 1 - u->hist_sel], y0 + 1 + rows + 1, ix, prog_h - 1, iw);
    if (n > rows) {
        char m[32];
        sfmt(m, sizeof m, " %s more ", U ? "↑↓" : "up/down");
        put(top + H - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
    (void)s;
}

int history_key(ui_state_t *u, int ch)
{
    switch (ch) { /* the view keeps the selection within the days and in sight */
    case KEY_UP: u->hist_sel--; break;
    case KEY_DOWN: u->hist_sel++; break;
    case KEY_PPAGE: u->hist_sel -= 10; break;
    case KEY_NPAGE: u->hist_sel += 10; break;
    case KEY_HOME: u->hist_sel = 0; break;
    case KEY_END: u->hist_sel = 1 << 20; break;
    default: return 0;
    }
    if (u->hist_sel < 0)
        u->hist_sel = 0;
    return 1;
}

void history_click(ui_state_t *u, int k)
{
    u->hist_sel = k;
}
