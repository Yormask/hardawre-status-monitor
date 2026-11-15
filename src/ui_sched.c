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
/* ui_sched.c - which CPUs a process may run on (key a) and its disk
 * priority (key i), like taskset and ionice: two dialogs, and the lines the
 * process details page shows about them. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_aff_per = 8; /* CPUs per row in the dialog, set when it's drawn */

/* ------------------------------------------------------------ helpers */

/* "0-7,12": the CPUs set in set[], online ones only */
static void cpus_text(const cpu_t *c, const unsigned char *set, char *out, size_t n)
{
    out[0] = '\0';
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i];
        if (id < 0 || id >= MAX_CPUS || !set[id])
            continue;
        int j = i;
        while (j + 1 < c->ncpu && c->id[j + 1] == c->id[j] + 1 && c->id[j + 1] < MAX_CPUS && set[c->id[j + 1]])
            j++;
        size_t l = strlen(out);
        if (j > i)
            snprintf(out + l, n - l, "%s%d-%d", l ? "," : "", id, c->id[j]);
        else
            snprintf(out + l, n - l, "%s%d", l ? "," : "", id);
        i = j;
    }
}

/* the kind of core set[] is exactly (all P-cores, all E-cores), -1 none */
static int set_kind(const cpu_t *c, const unsigned char *set)
{
    for (int t = 0; t < c->nctype; t++) {
        int same = 1;
        for (int i = 0; i < c->ncpu && same; i++) {
            int id = c->id[i];
            if (id >= 0 && id < MAX_CPUS)
                same = (set[id] != 0) == (c->core_type[id] == t);
        }
        if (same)
            return t;
    }
    return -1;
}

static int online_set(const cpu_t *c, const unsigned char *set)
{
    int n = 0;
    for (int i = 0; i < c->ncpu; i++)
        n += c->id[i] >= 0 && c->id[i] < MAX_CPUS && set[c->id[i]];
    return n;
}

void sched_aff_text(const sys_t *s, pid_t pid, char *out, size_t n)
{
    static unsigned char set[MAX_CPUS];
    const cpu_t *c = &s->cpu;
    if (proc_affinity_get(pid, set, MAX_CPUS) < 0) {
        snprintf(out, n, "?");
        return;
    }
    int k = online_set(c, set);
    if (k >= c->ncpu) {
        snprintf(out, n, "all %d CPUs", c->ncpu);
        return;
    }
    char list[256];
    cpus_text(c, set, list, sizeof list);
    int t = set_kind(c, set);
    const char *cs = k == 1 ? "CPU" : "CPUs";
    if (t >= 0)
        snprintf(out, n, "%s %s: the %s%s (%d of %d)", cs, list, c->ctype[t].name,
                 strlen(c->ctype[t].name) >= 4 && !strcmp(c->ctype[t].name + strlen(c->ctype[t].name) - 4, "core") ? "s"
                                                                                                                 : " cores",
                 k, c->ncpu);
    else
        snprintf(out, n, "%s %s (%d of %d)", cs, list, k, c->ncpu);
}

/* nice -20..19 maps to best-effort levels 0..7 while no class was set */
static int nice_level(int nice)
{
    int l = (nice + 20) / 5;
    return l < 0 ? 0 : l > 7 ? 7 : l;
}

void sched_io_text(pid_t pid, int nice, char *out, size_t n)
{
    int cls, lvl;
    if (proc_ioprio_get(pid, &cls, &lvl)) {
        snprintf(out, n, "?");
        return;
    }
    if (cls == IOP_NONE)
        snprintf(out, n, "normal %d (follows its nice value)", nice_level(nice));
    else if (cls == IOP_RT)
        snprintf(out, n, "realtime %d (gets the disk before everyone else)", lvl);
    else if (cls == IOP_IDLE)
        snprintf(out, n, "idle (only when no one else uses the disk)");
    else
        snprintf(out, n, "normal %d (set by hand)", lvl);
}

/* Disk priority works with the bfq and mq-deadline I/O schedulers; none
 * (common for NVMe drives) and kyber ignore it. "" = every drive honours it. */
static void io_sched_note(const sys_t *s, char *out, size_t n)
{
    char ign[160] = "", which[24] = "";
    int nign = 0, nall = 0;
    for (int i = 0; i < s->disks.n; i++) {
        char p[PATHBUF], b[160];
        snprintf(p, sizeof p, "%s/block/%s/queue/scheduler", g_sysfs, s->disks.d[i].name);
        if (read_line(p, b, sizeof b))
            continue;
        char *o = strchr(b, '['), *e = o ? strchr(o, ']') : NULL;
        if (!o || !e)
            continue;
        *e = '\0';
        nall++;
        if (strcmp(o + 1, "none") && strcmp(o + 1, "kyber"))
            continue;
        if (!nign++)
            snprintf(which, sizeof which, "%s", o + 1);
        size_t l = strlen(ign);
        if (l + strlen(s->disks.d[i].name) + 3 < sizeof ign)
            snprintf(ign + l, sizeof ign - l, "%s%s", l ? ", " : "", s->disks.d[i].name);
    }
    out[0] = '\0';
    if (nign && nign == nall)
        snprintf(out, n, "Your drives' I/O scheduler (%s) ignores this: it needs bfq or mq-deadline.", which);
    else if (nign)
        snprintf(out, n, "%s %s it (scheduler %s): it needs bfq or mq-deadline.", ign, nign > 1 ? "ignore" : "ignores",
                 which);
}

static void sched_fail(ui_state_t *u, int rc, pid_t pid, const char *what)
{
    if (rc == EPERM || rc == EACCES)
        ui_message(u, 1, "Permission denied: PID %d belongs to another user (or needs root). Run hsm with sudo.",
                   (int)pid);
    else if (rc == ESRCH)
        ui_message(u, 1, "PID %d has already exited", (int)pid);
    else
        ui_message(u, 1, "Could not change %s of PID %d: %s", what, (int)pid, strerror(rc));
}

/* ------------------------------------------------- which CPUs (key a) */

void aff_open(sys_t *s, ui_state_t *u)
{
    proc_t *p = ui_proc_target(s, u);
    if (!p)
        return;
    if (proc_affinity_get(p->pid, u->aff_set, MAX_CPUS) < 0) {
        ui_message(u, 1, "Could not read which CPUs PID %d may use: %s", (int)p->pid, strerror(errno));
        return;
    }
    memcpy(u->aff_old, u->aff_set, sizeof u->aff_old);
    u->aff_open = 1;
    u->aff_sel = 0;
    u->aff_pid = p->pid;
    u->aff_start = p->start;
    snprintf(u->aff_desc, sizeof u->aff_desc, "%s", u->sel_desc);
}

static void aff_apply(sys_t *s, ui_state_t *u)
{
    const cpu_t *c = &s->cpu;
    int k = online_set(c, u->aff_set);
    if (!k) {
        ui_message(u, 1, "Pick at least one CPU");
        return;
    }
    u->aff_open = 0;
    if (!memcmp(u->aff_set, u->aff_old, sizeof u->aff_old))
        return;
    int rc = proc_affinity_set(u->aff_pid, u->aff_start, u->aff_set, MAX_CPUS);
    if (rc) {
        sched_fail(u, rc, u->aff_pid, "which CPUs it may use");
        return;
    }
    char t[256];
    sched_aff_text(s, u->aff_pid, t, sizeof t);
    ui_message(u, 0, "PID %d now runs on %s", (int)u->aff_pid, t);
}

void aff_dialog(sys_t *s, ui_state_t *u)
{
    const cpu_t *c = &s->cpu;
    int n = c->ncpu, maxid = 0;
    for (int i = 0; i < n; i++)
        maxid = c->id[i] > maxid ? c->id[i] : maxid;
    int cw = (maxid >= 100 ? 3 : maxid >= 10 ? 2 : 1) + 3; /* "■ 12 " */
    int w = MIN(78, COLS), iw = w - 6;
    int groups = c->nctype >= 2 ? c->nctype : 1;
    int lw = 0; /* the kinds' names in front of their CPUs */
    if (groups > 1)
        for (int t = 0; t < c->nctype; t++)
            lw = MAX(lw, (int)strlen(c->ctype[t].name) + 2);
    int per = MAX(1, (iw - lw) / cw);
    g_aff_per = per;
    int rows = 0;
    for (int g = 0; g < groups; g++) {
        int k = 0;
        for (int i = 0; i < n; i++)
            k += groups == 1 || (c->id[i] < MAX_CPUS && c->core_type[c->id[i]] == g);
        rows += MAX(1, (k + per - 1) / per);
    }
    int h = MIN(LINES, rows + 11);
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2);
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Which CPUs it may use", NULL);
    hit_add(y, x, h, w, HIT_SCROLL, -1, 0);
    int xx = x + 3;
    xx += put(y + 2, xx, iw, A_GRAY, "PID ");
    put(y + 2, xx, x + 3 + iw - xx, A_BOLD, "%d", (int)u->aff_pid);
    put(y + 3, x + 3, iw, A_NORMAL, "%s", u->aff_desc);
    int yy = y + 5;
    for (int g = 0; g < groups && yy < y + h - 5; g++) {
        if (groups > 1)
            put(yy, x + 3, lw, A_GRAY, "%s%s", c->ctype[g].name,
                strlen(c->ctype[g].name) >= 4 && !strcmp(c->ctype[g].name + strlen(c->ctype[g].name) - 4, "core") ? "s"
                                                                                                                   : "");
        int col = 0;
        for (int i = 0; i < n; i++) {
            int id = c->id[i];
            if (id < 0 || id >= MAX_CPUS || (groups > 1 && c->core_type[id] != g))
                continue;
            if (col == per) {
                col = 0;
                yy++;
            }
            int cx = x + 3 + lw + col * cw;
            attr_t a = i == u->aff_sel ? CP(C_SEL) | A_BOLD : u->aff_set[id] ? A_BOLD : A_GRAY;
            put(yy, cx, cw - 1, a, "%s %d", u->aff_set[id] ? (U ? "■" : "x") : (U ? "□" : "."), id);
            hit_add(yy, cx, 1, cw - 1, HIT_AFF, i, 0);
            col++;
        }
        yy++;
    }
    int k = online_set(c, u->aff_set), k0 = online_set(c, u->aff_old);
    char now[128] = "now: all", list[96];
    if (k0 < n) {
        cpus_text(c, u->aff_old, list, sizeof list);
        snprintf(now, sizeof now, "now: %s", list);
    }
    put(y + h - 5, x + 3, iw, k ? A_BOLD : CP(C_WARN) | A_BOLD, "%d of %d CPUs%s%s", k, n, SEP, now);
    put(y + h - 4, x + 3, iw, A_GRAY, "%s",
        geteuid() ? "Your own processes only; other users' need sudo hsm. Its threads all follow."
                  : "Every thread of the process follows.");
    xx = x + 3;
    int xm = x + w - 2;
    xx += key_hint(y + h - 2, xx, xm, "Space", "toggle", ' ');
    xx += key_hint(y + h - 2, xx, xm, "a", "all", 'a');
    for (int t = 0; t < c->nctype; t++) {
        char kk[2] = { c->ctype[t].letter, 0 }, lab[32];
        snprintf(lab, sizeof lab, "only %c", c->ctype[t].letter);
        int dup = 0;
        for (int j = 0; j < t; j++)
            dup |= c->ctype[j].letter == c->ctype[t].letter;
        if (!dup)
            xx += key_hint(y + h - 2, xx, xm, kk, lab, c->ctype[t].letter);
    }
    xx += key_hint(y + h - 2, xx, xm, "Enter", "apply", '\n');
    key_hint(y + h - 2, xx, xm, "Esc", "cancel", 27);
}

void aff_key(sys_t *s, ui_state_t *u, int ch)
{
    const cpu_t *c = &s->cpu;
    int n = c->ncpu;
    switch (ch) {
    case KEY_LEFT: u->aff_sel--; break;
    case KEY_RIGHT: u->aff_sel++; break;
    case KEY_UP: u->aff_sel -= g_aff_per; break;
    case KEY_DOWN: u->aff_sel += g_aff_per; break;
    case KEY_HOME: u->aff_sel = 0; break;
    case KEY_END: u->aff_sel = n - 1; break;
    case ' ':
        if (u->aff_sel >= 0 && u->aff_sel < n && c->id[u->aff_sel] < MAX_CPUS)
            u->aff_set[c->id[u->aff_sel]] ^= 1;
        break;
    case 'a':
        for (int i = 0; i < n; i++)
            if (c->id[i] >= 0 && c->id[i] < MAX_CPUS)
                u->aff_set[c->id[i]] = 1;
        break;
    case '\n':
    case '\r':
    case KEY_ENTER: aff_apply(s, u); break;
    case 27:
    case 'q': u->aff_open = 0; break;
    default: /* P, E, L: only the cores of that kind */
        for (int t = 0; t < c->nctype; t++) {
            if (ch != c->ctype[t].letter)
                continue;
            for (int i = 0; i < n; i++) {
                int id = c->id[i];
                if (id < 0 || id >= MAX_CPUS)
                    continue;
                int kt = c->core_type[id];
                u->aff_set[id] = kt >= 0 && c->ctype[kt].letter == ch;
            }
            break;
        }
        break;
    }
    u->aff_sel = CLAMP(u->aff_sel, 0, MAX(0, n - 1));
}

void aff_click(sys_t *s, ui_state_t *u, int i, int dbl)
{
    (void)dbl;
    u->aff_sel = i;
    aff_key(s, u, ' ');
}

/* ------------------------------------------------- disk priority (key i) */

static const char *const IOP_NAME[] = { "", "Realtime", "Normal", "Idle" };

void iop_open(sys_t *s, ui_state_t *u)
{
    proc_t *p = ui_proc_target(s, u);
    if (!p)
        return;
    int cls, lvl;
    int rc = proc_ioprio_get(p->pid, &cls, &lvl);
    if (rc) {
        ui_message(u, 1, "Could not read the disk priority of PID %d: %s", (int)p->pid, strerror(rc));
        return;
    }
    if (cls == IOP_NONE) { /* follows nice: shown as what it amounts to */
        cls = IOP_BE;
        lvl = nice_level(p->nice);
        u->iop_follows = 1;
    } else {
        u->iop_follows = 0;
    }
    u->iop_open = 1;
    u->iop_cls = u->iop_ocls = cls;
    u->iop_lvl = u->iop_olvl = cls == IOP_IDLE ? 4 : lvl;
    u->iop_pid = p->pid;
    u->iop_start = p->start;
    snprintf(u->iop_desc, sizeof u->iop_desc, "%s", u->sel_desc);
}

static void iop_apply(ui_state_t *u)
{
    u->iop_open = 0;
    if (u->iop_cls == u->iop_ocls && (u->iop_cls == IOP_IDLE || u->iop_lvl == u->iop_olvl))
        return;
    int rc = proc_ioprio_set(u->iop_pid, u->iop_start, u->iop_cls, u->iop_lvl);
    if (rc == EPERM && u->iop_cls == IOP_RT) {
        ui_message(u, 1, "Only root can give a process realtime disk priority: run hsm with sudo");
        return;
    }
    if (rc) {
        sched_fail(u, rc, u->iop_pid, "the disk priority");
        return;
    }
    if (u->iop_cls == IOP_IDLE)
        ui_message(u, 0, "PID %d: disk priority idle (it uses the disk only when no one else does)", (int)u->iop_pid);
    else
        ui_message(u, 0, "PID %d: disk priority %s %d", (int)u->iop_pid, u->iop_cls == IOP_RT ? "realtime" : "normal",
                   u->iop_lvl);
}

void iop_dialog(sys_t *s, ui_state_t *u)
{
    int w = MIN(76, COLS), h = MIN(15, LINES), iw = w - 6;
    int y = MAX(0, (LINES - h) / 2), x = MAX(0, (COLS - w) / 2);
    for (int r = 0; r < h; r++)
        fill(y + r, x, w, A_NORMAL);
    box_(y, x, h, w, "Disk priority", NULL);
    hit_add(y, x, h, w, HIT_SCROLL, -1, 0);
    int xx = x + 3;
    xx += put(y + 2, xx, iw, A_GRAY, "PID ");
    put(y + 2, xx, x + 3 + iw - xx, A_BOLD, "%d", (int)u->iop_pid);
    put(y + 3, x + 3, iw, A_NORMAL, "%s", u->iop_desc);
    static const int order[3] = { IOP_RT, IOP_BE, IOP_IDLE };
    static const char *const what[] = { "", "gets the disk before everyone else (needs root)",
                                        "takes turns, 0 first ... 7 last", "only when no one else uses the disk" };
    for (int k = 0; k < 3; k++) {
        int c = order[k], on = u->iop_cls == c, yy = y + 5 + k;
        xx = x + 3;
        hit_add(yy, xx, 1, iw, HIT_IOP, c, 0);
        xx += put(yy, xx, iw, on ? CP(C_SEL) | A_BOLD : A_NORMAL, " %s %-9s", on ? (U ? "●" : "*") : (U ? "○" : "o"),
                  IOP_NAME[c]);
        xx++;
        if (c != IOP_IDLE && on) { /* its level: < 4 > */
            hit_add(yy, xx, 1, 2, HIT_IOP, c, -1);
            xx += put(yy, xx, x + 3 + iw - xx, CP(C_INFO) | A_BOLD, "%s", U ? "◀ " : "< ");
            xx += put(yy, xx, x + 3 + iw - xx, A_BOLD, "%d", u->iop_lvl);
            hit_add(yy, xx, 1, 2, HIT_IOP, c, 1);
            xx += put(yy, xx, x + 3 + iw - xx, CP(C_INFO) | A_BOLD, "%s", U ? " ▶" : " >");
            xx += 2;
        }
        put(yy, xx, x + 3 + iw - xx, A_GRAY, "%s", what[c]);
    }
    char now[64];
    if (u->iop_ocls == IOP_IDLE)
        snprintf(now, sizeof now, "now: idle");
    else
        snprintf(now, sizeof now, "now: %s %d%s", u->iop_ocls == IOP_RT ? "realtime" : "normal", u->iop_olvl,
                 u->iop_follows ? " (follows its nice value)" : "");
    put(y + 9, x + 3, iw, A_GRAY, "%s", now);
    char note[256];
    io_sched_note(s, note, sizeof note);
    if (note[0])
        wrap_text(y + 10, x + 3, iw, 2, CP(C_WARN), note);
    xx = x + 3;
    int xm = x + w - 2;
    xx += key_hint(y + h - 2, xx, xm, "Up/Dn", "kind", 0);
    xx += key_hint(y + h - 2, xx, xm, "Left/Right", "level", 0);
    xx += key_hint(y + h - 2, xx, xm, "Enter", "apply", '\n');
    key_hint(y + h - 2, xx, xm, "Esc", "cancel", 27);
}

void iop_key(ui_state_t *u, int ch)
{
    static const int up[] = { 0, IOP_RT, IOP_RT, IOP_BE }, down[] = { 0, IOP_BE, IOP_IDLE, IOP_IDLE };
    switch (ch) {
    case KEY_UP: u->iop_cls = up[u->iop_cls]; break;
    case KEY_DOWN: u->iop_cls = down[u->iop_cls]; break;
    case KEY_LEFT:
    case '-': u->iop_lvl--; break;
    case KEY_RIGHT:
    case '+':
    case '=': u->iop_lvl++; break;
    case '\n':
    case '\r':
    case KEY_ENTER: iop_apply(u); break;
    case 27:
    case 'q':
    case 'i': u->iop_open = 0; break;
    }
    u->iop_lvl = CLAMP(u->iop_lvl, 0, 7);
}

void iop_click(ui_state_t *u, int cls, int step, int dbl)
{
    u->iop_cls = cls;
    if (step)
        u->iop_lvl = CLAMP(u->iop_lvl + step, 0, 7);
    if (dbl && !step)
        iop_apply(u);
}
