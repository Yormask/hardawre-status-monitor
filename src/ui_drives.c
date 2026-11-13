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
/* ui_drives.c - Hardware view > D: the drives' self-tests and their own
 * error logs; start a short or extended test, see how it went. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sys_t *g_s;

static void ago(long long hours_now, long long then, char *out, size_t n)
{
    out[0] = '\0';
    if (hours_now < 0 || then < 0 || then > hours_now)
        return;
    long long d = hours_now - then;
    if (d < 1)
        sfmt(out, n, "%s", "within the hour");
    else if (d < 48)
        sfmt(out, n, "%lld h ago", d);
    else
        sfmt(out, n, "%lld days ago", d / 24);
}

void drives_open(sys_t *s, ui_state_t *u)
{
    g_s = s;
    u->drv_open = 1;
    u->drv_confirm = 0;
    int n = drives_sync(&s->health);
    for (int i = 0; i < n; i++)
        drive_read(drive_at(i));
}

static void start(ui_state_t *u, int type)
{
    drive_t *d = drive_at(u->drv_sel);
    if (!d)
        return;
    if (d->state == -1) {
        ui_message(u, 1, "Starting a self-test needs root: sudo hsm");
        return;
    }
    if (!d->can_test || d->state != 1) {
        ui_message(u, 1, "%s doesn't offer self-tests", d->name);
        return;
    }
    if (type && d->running) {
        ui_message(u, 1, "A test is running on %s already (a stops it)", d->name);
        return;
    }
    if (!type && !d->running) {
        ui_message(u, 0, "No test is running on %s", d->name);
        return;
    }
    int r = drive_start(d, type);
    if (r) {
        ui_message(u, 1, "%s: the drive refused (%s)", d->name, strerror(r));
        return;
    }
    if (type)
        ui_message(u, 0, "%s: %s self-test started; it runs inside the drive, an event says how it went", d->name,
                   type == 1 ? "short" : "extended");
    else
        ui_message(u, 0, "%s: test stopped", d->name);
    drive_read(d);
}

int drives_key(ui_state_t *u, int ch)
{
    if (u->drv_confirm) {
        int t = u->drv_confirm;
        u->drv_confirm = 0;
        if (ch == 'y' || ch == 'Y')
            start(u, t);
        else
            ui_message(u, 0, "Not started");
        return 1;
    }
    int n = g_s ? drives_sync(&g_s->health) : 0;
    drive_t *d = drive_at(u->drv_sel);
    switch (ch) {
    case 27:
    case KEY_LEFT: u->drv_open = 0; return 1;
    case KEY_UP: u->drv_sel = MAX(0, u->drv_sel - 1); return 1;
    case KEY_DOWN: u->drv_sel = MIN(MAX(0, n - 1), u->drv_sel + 1); return 1;
    case 'r':
        if (d)
            drive_read(d);
        return 1;
    case 's':
    case 'e':
        if (!d)
            return 1;
        if (d->state == -1) {
            ui_message(u, 1, "Starting a self-test needs root: sudo hsm");
            return 1;
        }
        u->drv_confirm = ch == 's' ? 1 : 2;
        return 1;
    case 'a': start(u, 0); return 1;
    }
    return 0;
}

void drives_click(ui_state_t *u, int i)
{
    u->drv_sel = i;
}

void drives_view(sys_t *s, ui_state_t *u)
{
    g_s = s;
    int n = drives_sync(&s->health);
    int top = 1, H = LINES - 2, W = COLS;
    box_(top, 0, H, W, "Drives: self-tests and their own error logs", NULL);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2;
    if (ih < 4)
        return;
    if (!n) {
        put(iy, ix, iw, A_GRAY, "%s", "No NVMe or SATA drives found (USB drives can't be tested from here).");
        return;
    }
    u->drv_sel = CLAMP(u->drv_sel, 0, n - 1);
    double now = (double)time(NULL);
    int y = iy, ymax = iy + ih - 4;
    for (int i = 0; i < n && y < ymax; i++) {
        drive_t *d = drive_at(i);
        if (d->running && now - d->read_t > 5) /* a test is running: follow it */
            drive_read(d);
        int sel = i == u->drv_sel;
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(y, ix - 1, iw + 2, base);
        hit_add(y, ix, 1, iw, HIT_DRIVE, i, 0);
        int xx = ix;
        xx += put(y, xx, iw, sel ? base | A_BOLD : CP(C_INFO) | A_BOLD, "%-8s", d->name);
        xx += put(y, xx, ix + iw - xx, sel ? base : A_BOLD, " %s", d->model[0] ? d->model : (d->nvme ? "NVMe drive" : "SATA drive"));
        if (d->hours >= 0) {
            char h[40];
            sfmt(h, sizeof h, "%lld h powered on", d->hours);
            put(y, ix + iw - utf8_cols(h), utf8_cols(h), sel ? base : A_GRAY, "%s", h);
        }
        y++;
        if (d->state == -1) {
            put(y++, ix + 2, iw - 2, A_GRAY, "%s", "needs root (sudo hsm) to read its logs and run tests");
            y++;
            continue;
        }
        if (d->state != 1) {
            put(y++, ix + 2, iw - 2, A_GRAY, "%s", d->state ? "no self-test log from this drive" : "reading...");
            y++;
            continue;
        }
        /* now */
        char len[64] = "";
        if (d->short_min > 0 || d->ext_min > 0) {
            sfmt(len, sizeof len, "  (short %s%d min, extended ", d->nvme ? "up to " : "", MAX(d->short_min, 1));
            if (d->ext_min > 0)
                app(len, sizeof len, "%d min)", d->ext_min);
            else
                app(len, sizeof len, "?)");
        }
        if (d->running)
            put(y++, ix + 2, iw - 2, CP(C_WARN) | A_BOLD, "Running: %s self-test (a stops it)",
                d->running == 2 ? "extended" : "short");
        else
            put(y++, ix + 2, iw - 2, A_NORMAL, "No test running%s", d->can_test ? len : ", and this drive doesn't offer any");
        if (d->running && d->progress >= 0 && y < ymax) {
            int bw = MIN(40, iw - 20);
            put(y, ix + 4, 6, A_GRAY, "%3d%%", d->progress);
            bar(y, ix + 10, bw, d->progress, CP(C_IN));
            y++;
        }
        /* its error log */
        if (y < ymax) {
            if (d->errors < 0) {
                put(y++, ix + 2, iw - 2, A_GRAY, "%s", "Error log: not offered by this drive");
            } else if (!d->errors) {
                put(y++, ix + 2, iw - 2, CP(C_OK), "%s", "Error log: no errors in its whole life");
            } else {
                char when[32];
                ago(d->hours, d->err_hours, when, sizeof when);
                put(y++, ix + 2, iw - 2, d->err_bad ? CP(C_CRIT) | A_BOLD : A_NORMAL, "Error log: %lld in its life%s%s%s%s%s",
                    d->errors, d->err_text[0] ? "; the latest: " : "", d->err_text, when[0] ? " (" : "", when,
                    when[0] ? ")" : "");
            }
        }
        /* its tests, newest first */
        if (!d->ntest && y < ymax)
            put(y++, ix + 2, iw - 2, A_GRAY, "%s", "Self-tests: none run yet");
        for (int k = 0; k < d->ntest && k < 4 && y < ymax; k++) {
            const stest_t *t = &d->test[k];
            char when[32], at[40] = "";
            ago(d->hours, t->hours, when, sizeof when);
            if (t->lba >= 0)
                sfmt(at, sizeof at, ", first bad sector %lld", t->lba);
            int xx2 = ix + 2;
            if (!k)
                xx2 += put(y, xx2, 12, A_GRAY, "Self-tests: ");
            else
                xx2 += put(y, xx2, 12, A_GRAY, "%12s", "");
            xx2 += put(y, xx2, 10, A_NORMAL, "%-9s ", t->type == 2 ? "extended" : t->type == 1 ? "short" : "other");
            xx2 += put(y, xx2, ix + iw - xx2, t->failed ? CP(C_CRIT) | A_BOLD : t->stopped ? CP(C_WARN) : CP(C_OK),
                       "%s%s", t->what, at);
            if (t->hours >= 0)
                put(y, xx2, ix + iw - xx2, A_GRAY, "   at %lld h%s%s%s", t->hours, when[0] ? " (" : "", when, when[0] ? ")" : "");
            y++;
        }
        if (d->ntest > 4 && y < ymax)
            put(y++, ix + 14, iw - 14, A_GRAY, "%d older", d->ntest - 4);
        y++;
    }
    /* the question, or what the tests are for */
    int qy = iy + ih - 3;
    drive_t *d = drive_at(u->drv_sel);
    if (u->drv_confirm && d) {
        int m = u->drv_confirm == 1 ? d->short_min : d->ext_min;
        char dur[48] = "", q[200];
        if (m > 0)
            sfmt(dur, sizeof dur, " (about %d min)", m);
        sfmt(q, sizeof q, "Start a %s self-test on %s%s? The drive keeps working normally meanwhile.  y / n",
             u->drv_confirm == 1 ? "short" : "extended", d->name, dur);
        wrap_text(qy, ix, iw, 3, CP(C_WARN) | A_BOLD, q);
    } else {
        wrap_text(qy, ix, iw, 3, A_GRAY,
                  "A short test checks the electronics and part of the surface, an extended one all of it. They run "
                  "inside the drive, which keeps working; hsm adds an event when the ones it started end.");
    }
}
