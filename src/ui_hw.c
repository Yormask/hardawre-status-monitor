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
/* ui_hw.c - Hardware view: every sensor, GPU detail, drive, filesystem and interface */
#define _GNU_SOURCE
#include "ui_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ============================================================= Hardware */

typedef struct {
    int y0, x, w, h, line, scroll;
} lc_t;

static int lc_row(lc_t *c)
{
    int r = c->line - c->scroll;
    c->line++;
    return (r >= 0 && r < c->h) ? c->y0 + r : -1;
}

static void lc_text(lc_t *c, attr_t a, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void lc_text(lc_t *c, attr_t a, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(y, c->x, c->w, a, "%s", b);
}

static void lc_title(lc_t *c, const char *t)
{
    if (c->line)
        lc_row(c);
    int y = lc_row(c);
    if (y >= 0) {
        int n = put(y, c->x, c->w, CP(C_TITLE) | A_BOLD, "%s ", t);
        attrset(CP(C_BORDER));
        for (int i = n; i < c->w; i++)
            mvaddstr(y, c->x + i, U ? "─" : "-");
        attrset(A_NORMAL);
    }
}

static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void lc_kv(lc_t *c, const char *k, attr_t va, const char *fmt, ...)
{
    int y = lc_row(c);
    if (y < 0)
        return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    int kw = MIN(16, c->w / 3);
    put(y, c->x, kw, A_GRAY, "%s", k);
    put(y, c->x + kw, c->w - kw, va, "%s", b);
}

static void hw_sensors(sys_t *s, lc_t *c)
{
    sensors_t *S = &s->sens;
    lc_title(c, "Sensors");
    if (!S->n) {
        lc_text(c, A_GRAY, "No hwmon sensors found in /sys/class/hwmon");
        return;
    }
    int last = -1;
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (t->chip_idx != last) {
            lc_text(c, CP(C_INFO) | A_BOLD, "%s", t->chip);
            last = t->chip_idx;
        }
        int y = lc_row(c);
        if (y < 0)
            continue;
        int lw = MIN(22, c->w / 2), xx = c->x + 2 + lw;
        put(y, c->x + 2, lw, A_NORMAL, "%s", t->label);
        if (!t->ok) {
            put(y, xx, c->w - lw - 2, A_GRAY, "n/a");
            continue;
        }
        if (t->type == SENS_TEMP) {
            xx += put(y, xx, 9, temp_attr(t->value) | A_BOLD, "%5.1f%s", t->value, DEG);
            double ref = t->crit > 0 ? t->crit : 100.0;
            int bw = MIN(20, c->x + c->w - xx - 14);
            if (bw > 3) {
                bar(y, xx + 1, bw, 100.0 * t->value / ref, gradient_on() ? 0 : bar_temp_attr(t->value));
                xx += bw + 1;
            }
            if (t->crit > 0)
                put(y, xx + 1, c->x + c->w - xx - 1, A_GRAY, "crit %.0f%s", t->crit, DEG);
        } else if (t->type == SENS_FAN) {
            put(y, xx, c->w - lw - 2, A_BOLD, "%5.0f RPM", t->value);
        } else if (t->type == SENS_VOLT) {
            int bad = (t->hi > 0 && t->value > t->hi) || (t->lo > 0 && t->value < t->lo);
            xx += put(y, xx, 10, bad ? CP(C_CRIT) | A_BOLD : A_BOLD, "%7.3f V", t->value);
            if (t->lo > 0 || t->hi > 0)
                put(y, xx + 2, c->x + c->w - xx - 2, A_GRAY, "range %.2f - %.2f V", t->lo, t->hi);
        } else {
            put(y, xx, c->w - lw - 2, CP(C_WARN) | A_BOLD, "%6.2f W", t->value);
        }
    }
}

static void hw_health(sys_t *s, lc_t *c)
{
    health_t *H = &s->health;
    char a[24], b[24];
    lc_title(c, "Health");

    /* pressure stall information */
    if (H->psi[0].ok) {
        lc_text(c, A_GRAY, "%-16s %6s %6s %6s %6s   %-21s", "Waiting (PSI)", "now", "10s", "1min", "5min", "all tasks at once");
        static const char *const nm[3] = { "  CPU", "  Memory", "  Disk I/O" };
        for (int i = 0; i < 3; i++) {
            psi_t *p = &H->psi[i];
            char full[64] = "";
            if (i && p->has_full)
                sfmt(full, sizeof full, "%5.1f%% %5.1f%% %5.1f%%", p->full, p->favg10, p->favg60);
            int y = lc_row(c);
            if (y < 0)
                continue;
            int xx = c->x;
            xx += put(y, xx, 16, CP(C_INFO), "%-16s ", nm[i]);
            xx += put(y, xx, c->x + c->w - xx, pct_attr(p->some >= 25 ? 100 : p->some >= 10 ? 70 : 0) | A_BOLD, "%5.1f%% ", p->some);
            xx += put(y, xx, c->x + c->w - xx, A_NORMAL, "%5.1f%% %5.1f%% %5.1f%%   ", p->avg10, p->avg60, p->avg300);
            put(y, xx, c->x + c->w - xx, A_GRAY, "%s", full);
        }
        lc_text(c, A_GRAY, "  share of time tasks had to wait; 10%% or more is noticeable");
    } else {
        lc_kv(c, "Waiting (PSI)", A_GRAY, "%s", "not available: needs Linux 4.20+ with PSI enabled");
    }

    /* thermal throttling */
    if (H->thr_ok) {
        unsigned long long all = H->thr_count + H->core_count, since = all - H->thr_start;
        double now = (double)time(NULL);
        attr_t at = H->thr_last > 0 && now - H->thr_last < 10 ? CP(C_CRIT) | A_BOLD : since ? CP(C_WARN) | A_BOLD : A_BOLD;
        lc_kv(c, "Throttling", at, "%llu since boot (%.1f s in total)%s", all, H->thr_ms / 1000.0,
              since ? "" : ", none since start");
        if (since && H->thr_last > 0 && now - H->thr_last < 10)
            lc_kv(c, "", at, "%llu since hsm started, NOW (%.0f%% of the time)", since, H->thr_pct);
        else if (since)
            lc_kv(c, "", at, "%llu since hsm started", since);
        for (int i = 0; i < H->nthr && H->nthr > 1; i++)
            lc_kv(c, "", A_GRAY, "package %d: %llu times, %.1f s", H->thr[i].pkg, H->thr[i].count, (double)H->thr[i].ms / 1000.0);
        lc_text(c, A_GRAY, "  the CPU slows itself down when it reaches its temperature limit");
    } else {
        lc_kv(c, "Throttling", A_GRAY, "%s", "not reported by this CPU (Intel only)");
    }

    if (s->mem.oom_ok) {
        unsigned long long since = s->mem.oom_kill - H->oom_start;
        lc_kv(c, "OOM kills", since ? CP(C_CRIT) | A_BOLD : s->mem.oom_kill ? CP(C_WARN) : A_BOLD,
              "%llu since boot, %llu since hsm started", s->mem.oom_kill, since);
    }
    if (H->aer_ok) {
        attr_t pa = H->pci_nonfatal + H->pci_fatal ? CP(C_CRIT) | A_BOLD : H->pci_cor ? CP(C_WARN) : A_BOLD;
        lc_kv(c, "PCIe errors", pa, "%llu corrected, %llu non-fatal, %llu fatal since boot", H->pci_cor,
              H->pci_nonfatal, H->pci_fatal);
        for (int i = 0; i < H->npci; i++)
            lc_kv(c, "", A_GRAY, "%s %s: %llu / %llu / %llu", H->pci[i].dev, H->pci[i].name, H->pci[i].cor,
                  H->pci[i].nonfatal, H->pci[i].fatal);
    } else {
        lc_kv(c, "PCIe errors", A_GRAY, "%s", "not reported (no AER on this machine)");
    }
    if (H->files_max) {
        fmt_si((double)H->files, a, sizeof a);
        lc_kv(c, "Open files", A_NORMAL, "%s handles in use (whole system)", a);
    }

    /* NVMe drive health */
    for (int i = 0; i < H->nnvme; i++) {
        nvme_t *v = &H->nvme[i];
        char k[32];
        sfmt(k, sizeof k, "Drive %s", v->name);
        if (v->state != 1) {
            lc_kv(c, k, A_GRAY, "%s: health %s", v->model[0] ? v->model : "NVMe",
                  v->state == -1 ? "needs root (sudo hsm)" : "not available");
            continue;
        }
        lc_kv(c, k, v->crit || v->media_err ? CP(C_CRIT) | A_BOLD : CP(C_OK) | A_BOLD, "%s: %s", v->model,
              v->crit ? "WARNING" : v->media_err ? "media errors" : "OK");
        if (v->crit)
            lc_kv(c, "", CP(C_CRIT) | A_BOLD, "%s%s%s%s", v->crit & 1 ? "spare low  " : "", v->crit & 2 ? "temperature  " : "",
                  v->crit & 4 ? "reliability degraded  " : "", v->crit & 8 ? "read-only  " : "");
        lc_kv(c, "", v->used >= 90 ? CP(C_WARN) : A_NORMAL, "%d%% of rated wear used, spare %d%% (warns below %d%%)",
              v->used, v->spare, v->spare_thr);
        fmt_bytes(v->written_b, a, sizeof a);
        fmt_bytes(v->read_b, b, sizeof b);
        lc_kv(c, "", A_NORMAL, "%llu h powered on, %llu power cycles", v->hours, v->cycles);
        lc_kv(c, "", A_NORMAL, "%s written, %s read, now %.0f%s", a, b, v->temp_c, DEG);
        lc_kv(c, "", v->media_err ? CP(C_CRIT) | A_BOLD : v->unsafe ? A_NORMAL : A_GRAY,
              "%llu unsafe shutdowns, %llu media errors, %llu logged errors", v->unsafe, v->media_err, v->err_log);
        if (v->warn_min || v->crit_min)
            lc_kv(c, "", CP(C_WARN), "%llu min above its warning temperature, %llu min above critical", v->warn_min,
                  v->crit_min);
    }

    /* kernel messages and other events */
    if (H->kmsg == 1)
        lc_kv(c, "Kernel log", H->kmsg_err ? CP(C_WARN) | A_BOLD : A_BOLD, "%u errors, %u warnings since boot",
              H->kmsg_err, H->kmsg_warn);
    else
        lc_kv(c, "Kernel log", A_GRAY, "%s", H->kmsg == -1 ? "needs root (sudo hsm)" : "not available");
    if (!H->nev) {
        lc_text(c, A_GRAY, "  no events");
        return;
    }
    lc_text(c, A_GRAY, "Events, newest first:");
    for (int i = 0; i < H->nev && i < 40; i++) {
        int y = lc_row(c);
        if (y >= 0)
            event_line(y, c->x + 2, c->w - 2, health_event_at(H, i));
    }
}

static void hw_gpus(sys_t *s, lc_t *c)
{
    lc_title(c, "Graphics");
    if (!s->gpus.n) {
        lc_text(c, A_GRAY, "No GPU found in /sys/class/drm");
        return;
    }
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        char b[256] = "";
        lc_text(c, A_BOLD, "%s", g->name);
        app(b, sizeof b, "%s", g->driver[0] ? g->driver : "?");
        if (g->pci[0])
            app(b, sizeof b, "%sPCI %s", SEP, g->pci);
        app(b, sizeof b, "%s%s%s", SEP, g->card, g->integrated ? " (integrated)" : "");
        lc_kv(c, "  Driver", A_NORMAL, "%s", b);
        if (g->util >= 0) {
            b[0] = '\0';
            for (int e = 0; e < g->neng; e++)
                app(b, sizeof b, "%s%s %.0f%%", e ? "  " : "   (", g->eng[e].name, g->eng[e].pct);
            if (g->neng)
                app(b, sizeof b, ")");
            lc_kv(c, "  Load", pct_attr(g->util) | A_BOLD, "%s%.1f%%%s", g->util_est ? "~" : "", g->util, b);
        } else {
            lc_kv(c, "  Load", CP(C_WARN), "%s", g->note[0] ? g->note : "n/a");
        }
        if (g->mem_total > 0) {
            char a1[16], a2[16];
            fmt_bytes(g->mem_used, a1, sizeof a1);
            fmt_bytes(g->mem_total, a2, sizeof a2);
            lc_kv(c, "  VRAM", A_BOLD, "%s / %s (%.0f%%)", a1, a2, 100.0 * g->mem_used / g->mem_total);
        }
        if (g->clk_mhz >= 0)
            lc_kv(c, "  Clock", A_BOLD, "%.0f MHz%s", g->clk_mhz, g->clk_max_mhz > 0 ? "" : "");
        if (g->clk_max_mhz > 0)
            lc_kv(c, "  Max clock", A_NORMAL, "%.0f MHz", g->clk_max_mhz);
        if (g->mem_clk_mhz > 0)
            lc_kv(c, "  Memory clock", A_NORMAL, "%.0f MHz", g->mem_clk_mhz);
        if (g->temp_c >= 0)
            lc_kv(c, "  Temperature", temp_attr(g->temp_c) | A_BOLD, "%.1f%s", g->temp_c, DEG);
        if (g->power_w >= 0) {
            if (g->power_cap_w > 0)
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W of %.0f W limit", g->power_w, g->power_cap_w);
            else
                lc_kv(c, "  Power", CP(C_WARN) | A_BOLD, "%.1f W%s", g->power_w,
                      g->integrated && !strcmp(g->vendor, "Intel") ? "  (RAPL uncore)" : "");
        }
        if (g->fan_pct >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f%%", g->fan_pct);
        else if (g->fan_rpm >= 0)
            lc_kv(c, "  Fan", A_BOLD, "%.0f RPM", g->fan_rpm);
        if (g->util_est)
            lc_kv(c, "  Note", A_GRAY, "%s", "load estimated from RC6 sleep time; run as root for exact per-engine load");
        else if (g->util >= 0 && g->note[0])
            lc_kv(c, "  Note", A_GRAY, "%s", g->note);
    }
}

static void hw_power(sys_t *s, lc_t *c)
{
    power_t *p = &s->power;
    lc_title(c, "Power");
    if (s->total_w >= 0)
        lc_kv(c, "Estimated draw", CP(C_WARN) | A_BOLD, "%.1f W  (%s)", s->total_w, s->total_w_src);
    for (int i = 0; i < p->nrapl; i++) {
        rapl_t *z = &p->z[i];
        char k[48];
        sfmt(k, sizeof k, "%sRAPL %s", z->depth ? "  " : "", z->name);
        if (!z->readable)
            lc_kv(c, k, CP(C_WARN), "%s", "no permission");
        else if (z->watts >= 0)
            lc_kv(c, k, A_BOLD, "%.2f W", z->watts);
        else
            lc_kv(c, k, A_GRAY, "%s", "...");
    }
    if (p->rapl_denied && geteuid() != 0) {
        lc_text(c, CP(C_WARN), "RAPL energy counters are root-only since Linux 5.10.");
        lc_text(c, CP(C_WARN), "Run 'sudo hsm' to see CPU / DRAM / iGPU power.");
    }
    if (!p->nrapl)
        lc_text(c, A_GRAY, "No RAPL power domains (/sys/class/powercap)");
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        char tl[24] = "";
        if (b->time_left > 0)
            fmt_duration(b->time_left, tl, sizeof tl);
        lc_kv(c, b->name, A_BOLD, "%s, %.0f%%%s%s%s", b->status, b->capacity, b->watts >= 0 ? "" : "",
              tl[0] ? ", " : "", tl);
        if (b->watts >= 0)
            lc_kv(c, "  Rate", CP(C_WARN) | A_BOLD, "%.2f W", b->watts);
        if (b->health >= 0)
            lc_kv(c, "  Health", b->health < 60 ? CP(C_CRIT) | A_BOLD : b->health < 80 ? CP(C_WARN) : A_BOLD,
                  "%.0f%% of its original capacity%s", b->health, b->health < 80 ? " (worn)" : "");
        if (b->cycles >= 0)
            lc_kv(c, "  Charge cycles", A_NORMAL, "%ld", b->cycles);
    }
    if (p->ac_online >= 0)
        lc_kv(c, "AC adapter", A_BOLD, "%s", p->ac_online ? "online" : "offline");
}

static void hw_system(sys_t *s, lc_t *c)
{
    cpu_t *cv = &s->cpu;
    mem_t *m = &s->mem;
    char a[24], b[24], d[24];
    lc_title(c, "System");
    lc_kv(c, "Host", A_BOLD, "%s", s->host);
    lc_kv(c, "OS", A_NORMAL, "%s", s->os);
    lc_kv(c, "Kernel", A_NORMAL, "%s (%s)", s->kernel, s->arch);
    fmt_duration(cv->uptime, a, sizeof a);
    lc_kv(c, "Uptime", A_NORMAL, "%s", a);
    lc_kv(c, "Load average", A_BOLD, "%.2f  %.2f  %.2f", cv->load[0], cv->load[1], cv->load[2]);
    lc_kv(c, "Tasks", A_NORMAL, "%d processes, %d threads, %d running, %d blocked", s->procs.n, s->procs.threads,
          s->procs.running, s->procs.diskwait);
    fmt_si(cv->ctxt_rate, a, sizeof a);
    fmt_si(cv->intr_rate, b, sizeof b);
    fmt_si(cv->fork_rate, d, sizeof d);
    lc_kv(c, "Activity", A_NORMAL, "%s ctx switches/s, %s interrupts/s, %s forks/s", a, b, d);

    lc_title(c, "CPU");
    lc_kv(c, "Model", A_BOLD, "%s", cv->model);
    lc_kv(c, "Topology", A_NORMAL, "%d cores, %d threads (%d online)", cv->cores, cv->threads, cv->ncpu);
    lc_kv(c, "Usage", A_NORMAL, "%.1f%%  (user %.1f%%, system %.1f%%, iowait %.1f%%, steal %.1f%%)", cv->total,
          cv->user, cv->sys, cv->iowait, cv->steal);
    if (cv->mhz_avg > 0)
        lc_kv(c, "Clock", A_NORMAL, "avg %.0f MHz, max %.0f MHz", cv->mhz_avg, cv->mhz_max);
    if (cv->temp_c >= 0)
        lc_kv(c, "Temperature", temp_attr(cv->temp_c) | A_BOLD, "%.1f%s", cv->temp_c, DEG);
    if (cv->driver[0] || cv->governor[0]) {
        char pw[256] = "";
        app(pw, sizeof pw, "%s", cv->driver[0] ? cv->driver : "?");
        if (cv->pstate[0])
            app(pw, sizeof pw, " (%s)", cv->pstate);
        if (cv->governor[0])
            app(pw, sizeof pw, "%sgovernor %s", SEP, cv->governor);
        lc_kv(c, "Power settings", A_NORMAL, "%s", pw);
        pw[0] = '\0';
        if (cv->epp[0])
            lc_kv(c, "", A_NORMAL, "energy preference %s", cv->epp);
        if (cv->turbo >= 0)
            app(pw, sizeof pw, "turbo %s", cv->turbo ? "on" : "OFF");
        if (cv->lim_max > 0)
            app(pw, sizeof pw, "%slimits %.0f-%.0f MHz", pw[0] ? SEP : "", cv->lim_min, cv->lim_max);
        if (cv->profile[0])
            app(pw, sizeof pw, "%sprofile %s", pw[0] ? SEP : "", cv->profile);
        int capped = cv->lim_max > 0 && cv->hw_max > 0 && cv->lim_max < cv->hw_max - 1;
        if (pw[0])
            lc_kv(c, "", capped || cv->turbo == 0 ? CP(C_WARN) : A_NORMAL, "%s%s", pw,
                  capped ? "  (below what the CPU can do)" : "");
    }
    if (cv->nidle) { /* "C1 2%  C6 12%  C10 55%", wrapped to the column */
        char id[256] = "", w[24], tok[48];
        int first = 1, vw = c->w - MIN(16, c->w / 3) - 1;
        for (int k = 0; k < cv->nidle; k++) {
            if (!strcmp(cv->idle[k].name, "POLL"))
                continue;
            sfmt(tok, sizeof tok, "%s %.0f%%%s  ", cv->idle[k].name, cv->idle[k].pct, cv->idle[k].disabled ? " (off)" : "");
            if (id[0] && utf8_cols(id) + utf8_cols(tok) > vw) {
                lc_kv(c, first ? "Idle states" : "", A_NORMAL, "%s", id);
                first = 0;
                id[0] = '\0';
            }
            app(id, sizeof id, "%s", tok);
        }
        if (id[0])
            lc_kv(c, first ? "Idle states" : "", A_NORMAL, "%s", id);
        fmt_si(cv->wakeups, w, sizeof w);
        lc_kv(c, "", A_GRAY, "%s wake-ups/s (deeper states save more power)", w);
    }
    if (cv->mhz_avg > 0) {
        int per = MAX(1, (c->w - 2) / 13);
        char line[512] = "";
        for (int i = 0; i < cv->ncpu; i++) {
            app(line, sizeof line, "cpu%-3d%5.0f  ", cv->id[i], cv->mhz[i]);
            if ((i + 1) % per == 0 || i == cv->ncpu - 1) {
                lc_text(c, A_GRAY, "  %s", line);
                line[0] = '\0';
            }
        }
    }

    lc_title(c, "Memory");
    fmt_bytes((double)m->total, a, sizeof a);
    lc_kv(c, "Total", A_BOLD, "%s", a);
    fmt_bytes((double)m->used, a, sizeof a);
    lc_kv(c, "Used", A_BOLD, "%s (%.1f%%)", a, m->total ? 100.0 * (double)m->used / (double)m->total : 0);
    fmt_bytes((double)m->avail, a, sizeof a);
    lc_kv(c, "Available", A_NORMAL, "%s", a);
    fmt_bytes((double)m->buffcache, a, sizeof a);
    fmt_bytes((double)m->shmem, b, sizeof b);
    lc_kv(c, "Buff/cache", A_NORMAL, "%s  (shared %s)", a, b);
    fmt_bytes((double)m->dirty, a, sizeof a);
    fmt_bytes((double)m->writeback, b, sizeof b);
    lc_kv(c, "Dirty/writeback", A_NORMAL, "%s / %s", a, b);
    fmt_bytes((double)m->swap_used, a, sizeof a);
    fmt_bytes((double)m->swap_total, b, sizeof b);
    lc_kv(c, "Swap", A_NORMAL, "%s / %s", a, b);
    fmt_rate(m->swapin_bps, a, sizeof a);
    fmt_rate(m->swapout_bps, b, sizeof b);
    lc_kv(c, "Swap I/O", A_NORMAL, "in %s, out %s, %.0f major faults/s", a, b, m->majflt_rate);
}

static void hw_storage(sys_t *s, lc_t *c)
{
    disks_t *D = &s->disks;
    char a[24], b[24], d[24];
    lc_title(c, "Drives");
    if (!D->n)
        lc_text(c, A_GRAY, "No physical drives found");
    for (int i = 0; i < D->n; i++) {
        disk_t *k = &D->d[i];
        fmt_bytes((double)k->size, a, sizeof a);
        lc_text(c, A_BOLD, "%-10s %-5s %6s  %s", k->name, k->kind, a, k->model);
        fmt_rate(k->rd_bps, a, sizeof a);
        fmt_rate(k->wr_bps, b, sizeof b);
        fmt_si(k->rd_iops + k->wr_iops, d, sizeof d);
        lc_text(c, A_NORMAL, "  now   read %s  write %s  %s IOPS  busy %.0f%%", a, b, d, k->util);
        double lat = MAX(k->rd_lat, k->wr_lat);
        lc_text(c, lat >= 200 ? CP(C_CRIT) | A_BOLD : lat >= 50 ? CP(C_WARN) : A_NORMAL,
                "  time per request   read %.1f ms  write %.1f ms  in flight %u", k->rd_lat, k->wr_lat, k->inflight);
        fmt_bytes((double)k->rd_total, a, sizeof a);
        fmt_bytes((double)k->wr_total, b, sizeof b);
        lc_text(c, A_GRAY, "  since boot   read %s  written %s", a, b);
    }
    lc_title(c, "Filesystems");
    if (!D->nfs)
        lc_text(c, A_GRAY, "No local filesystems found");
    for (int i = 0; i < D->nfs; i++) {
        fs_t *f = &D->fs[i];
        int y = lc_row(c);
        if (y < 0)
            continue;
        char v[64];
        fmt_bytes((double)f->used, a, sizeof a);
        fmt_bytes((double)f->total, b, sizeof b);
        sfmt(v, sizeof v, "%s/%s %-5s", a, b, f->type);
        double pct = f->total ? 100.0 * (double)f->used / (double)f->total : 0;
        int lw = MIN(18, c->w / 3);
        put(y, c->x, lw - 1, A_BOLD, "%s", f->mount);
        meter(y, c->x + lw, c->w - lw, "", 0, pct, v, 18, 0);
    }
}

static void hw_network(sys_t *s, lc_t *c)
{
    char a[24], b[24];
    lc_title(c, "Network");
    if (!s->net.n)
        lc_text(c, A_GRAY, "No interfaces (besides loopback)");
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        char sp[24] = "";
        if (f->speed > 0)
            sfmt(sp, sizeof sp, "%d Mb/s", f->speed);
        lc_text(c, f->up ? A_BOLD : A_GRAY, "%-12s %-8s %-8s %-11s %s", f->name,
                f->wireless ? "wifi" : f->is_virtual ? "virtual" : "ethernet", f->state, sp, f->addr);
        fmt_rate(f->rx_bps, a, sizeof a);
        fmt_rate(f->tx_bps, b, sizeof b);
        char ta[24], tb[24];
        fmt_bytes((double)f->rx_total, ta, sizeof ta);
        fmt_bytes((double)f->tx_total, tb, sizeof tb);
        lc_text(c, A_GRAY, "  %s %s  %s %s   total rx %s  tx %s", U ? "↓" : "in", a, U ? "↑" : "out", b, ta, tb);
        if (f->quality >= 0)
            lc_text(c, f->signal_dbm >= -67 ? CP(C_OK) : f->signal_dbm >= -75 ? CP(C_WARN) : CP(C_CRIT),
                    "  Wi-Fi signal %.0f dBm, link quality %d%%", f->signal_dbm, f->quality);
        if (f->errors || f->drops || f->err_rate > 0 || f->drop_rate > 0)
            lc_text(c, f->err_rate + f->drop_rate > 0 ? CP(C_CRIT) | A_BOLD : A_GRAY,
                    "  %llu errors, %llu dropped packets since boot (now %.0f/s)", f->errors, f->drops,
                    f->err_rate + f->drop_rate);
    }
}

void hardware(sys_t *s, ui_state_t *u)
{
    int top = 1, h = LINES - 2, inner = h - 2;
    u->hw_scroll = CLAMP(u->hw_scroll, 0, u->hw_max);
    if (COLS >= 110) {
        int lw = COLS / 2;
        box_(top, 0, h, lw, "Health · Sensors · Graphics · Power", NULL);
        box_(top, lw, h, COLS - lw, "System · Storage · Network", NULL);
        lc_t L = { top + 1, 2, lw - 4, inner, 0, u->hw_scroll };
        lc_t R = { top + 1, lw + 2, COLS - lw - 4, inner, 0, u->hw_scroll };
        hw_health(s, &L);
        hw_sensors(s, &L);
        hw_gpus(s, &L);
        hw_power(s, &L);
        hw_system(s, &R);
        hw_storage(s, &R);
        hw_network(s, &R);
        u->hw_max = MAX(0, MAX(L.line, R.line) - inner);
    } else {
        box_(top, 0, h, COLS, "Hardware", NULL);
        lc_t C = { top + 1, 2, COLS - 4, inner, 0, u->hw_scroll };
        hw_health(s, &C);
        hw_system(s, &C);
        hw_sensors(s, &C);
        hw_gpus(s, &C);
        hw_power(s, &C);
        hw_storage(s, &C);
        hw_network(s, &C);
        u->hw_max = MAX(0, C.line - inner);
    }
    if (u->hw_max > 0) {
        char m[64];
        sfmt(m, sizeof m, " %s scroll %d/%d ", U ? "↑↓" : "up/down", u->hw_scroll, u->hw_max);
        put(top + h - 1, COLS - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
}
