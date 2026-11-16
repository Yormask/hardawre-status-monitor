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
/* json.c - hsm --json: one snapshot of everything as JSON, for scripts.
 * Units are in the key names (_pct, _c, _mhz, _w, _bytes, _bps, _ms);
 * anything the machine doesn't report is null. */
#define _GNU_SOURCE
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_depth, g_first[32];

static FILE *g_out; /* where the JSON goes: stdout, or a reply of hsm --serve */

static void fputc_out(int c)
{
    fputc(c, g_out);
}

static void indent(void)
{
    for (int i = 0; i < g_depth; i++)
        fputs("  ", g_out);
}

/* a JSON string; bytes that are not valid UTF-8 become U+FFFD */
static void str(const char *s)
{
    fputc_out('"');
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p;
        if (c == '"' || c == '\\') {
            fprintf(g_out, "\\%c", c);
            p++;
        } else if (c < 0x20) {
            fprintf(g_out, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            fputc_out((int)c);
            p++;
        } else {
            int n = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
            int ok = n > 0;
            for (int k = 1; k < n && ok; k++)
                ok = (p[k] & 0xc0) == 0x80;
            if (ok) {
                fwrite(p, 1, (size_t)n, g_out);
                p += n;
            } else {
                fputs("\\ufffd", g_out);
                p++;
            }
        }
    }
    fputc_out('"');
}

static void key(const char *k)
{
    if (!g_first[g_depth])
        fputc_out(',');
    g_first[g_depth] = 0;
    fputc_out('\n');
    indent();
    if (k) {
        str(k);
        fputs(": ", g_out);
    }
}

static void open_(const char *k, char c)
{
    key(k);
    fputc_out(c);
    g_first[++g_depth] = 1;
}

static void close_(char c)
{
    int empty = g_first[g_depth];
    g_depth--;
    if (!empty) {
        fputc_out('\n');
        indent();
    }
    fputc_out(c);
}

static void s_(const char *k, const char *v)
{
    key(k);
    if (v)
        str(v);
    else
        fputs("null", g_out);
}

static void sn(const char *k, const char *v) /* "" = null */
{
    s_(k, v && *v ? v : NULL);
}

static void num(const char *k, double v, int dec)
{
    key(k);
    if (isnan(v) || isinf(v))
        fputs("null", g_out);
    else
        fprintf(g_out, "%.*f", dec, v);
}

static void pos(const char *k, double v, int dec) /* negative = unknown */
{
    num(k, v < 0 ? NAN : v, dec);
}

static void integer(const char *k, long long v)
{
    key(k);
    fprintf(g_out, "%lld", v);
}

static void boolean(const char *k, int v)
{
    key(k);
    fputs(v ? "true" : "false", g_out);
}

static void null_(const char *k)
{
    key(k);
    fputs("null", g_out);
}

/* ---------------------------------------------------------------- parts */

static void cpu(sys_t *s)
{
    cpu_t *c = &s->cpu;
    open_("cpu", '{');
    s_("model", c->model);
    integer("cores", c->cores);
    integer("threads", c->threads);
    open_("core_kinds", '['); /* P-cores and E-cores, ARM's big and LITTLE ones; [] = all alike */
    for (int t = 0; t < c->nctype; t++) {
        open_(NULL, '{');
        char l[2] = { c->ctype[t].letter, 0 };
        s_("kind", l);
        s_("name", c->ctype[t].name);
        integer("cores", c->ctype[t].cores);
        integer("threads", c->ctype[t].threads);
        pos("base_mhz", c->kbase[t] > 0 ? c->kbase[t] : -1, 0);
        int bo = cpu_boost_mhz(c, t);
        pos("boost_mhz", bo > 0 ? bo : -1, 0);
        close_('}');
    }
    close_(']');
    sn("vendor_id", c->vendor_id);
    if (c->family >= 0) {
        integer("family", c->family);
        integer("model_number", c->model_no);
        integer("stepping", c->stepping);
    } else {
        null_("family");
        null_("model_number");
        null_("stepping");
    }
    sn("microcode", c->ucode);
    sn("isa_level", c->isa);
    open_("features", '['); /* the notable instruction sets: "AVX2", "AES" */
    for (const char *f = c->feats; *f;) {
        const char *e = strstr(f, ", ");
        size_t l = e ? (size_t)(e - f) : strlen(f);
        char one[32];
        str_copy(one, MIN(sizeof one, l + 1), f);
        s_(NULL, one);
        f += l + (e ? 2 : 0);
    }
    close_(']');
    sn("smt", c->smt); /* on, off, forceoff, notsupported; null = unknown */
    if (c->smt_active >= 0)
        boolean("smt_active", c->smt_active);
    else
        null_("smt_active");
    pos("base_mhz", c->base_mhz > 0 ? c->base_mhz : -1, 0);
    int boost = cpu_boost_mhz(c, -1);
    pos("boost_mhz", boost > 0 ? boost : -1, 0);
    if (c->numa > 0)
        integer("numa_nodes", c->numa);
    else
        null_("numa_nodes");
    open_("caches", '[');
    for (int k = 0; k < c->ncache; k++) {
        const cache_t *e = &c->cache[k];
        open_(NULL, '{');
        integer("level", e->level);
        s_("type", e->type == 'd' ? "data" : e->type == 'i' ? "instruction" : "unified");
        integer("size_kb", e->kb);
        integer("count", e->n);
        integer("cores_each", e->share); /* cores that share one */
        if (e->kind >= 0 && e->kind < c->nctype) {
            char l[2] = { c->ctype[(int)e->kind].letter, 0 };
            s_("core_kind", l);
        } else
            null_("core_kind");
        close_('}');
    }
    close_(']');
    num("usage_pct", c->total, 1);
    num("user_pct", c->user, 1);
    num("system_pct", c->sys, 1);
    num("iowait_pct", c->iowait, 1);
    num("steal_pct", c->steal, 1);
    pos("temp_c", c->temp_c, 1);
    num("clock_mhz", c->mhz_avg > 0 ? c->mhz_avg : NAN, 0);
    pos("power_w", s->power.pkg_w, 2);
    open_("load", '[');
    for (int i = 0; i < 3; i++)
        num(NULL, c->load[i], 2);
    close_(']');
    sn("governor", c->governor);
    sn("driver", c->driver);
    sn("energy_preference", c->epp);
    sn("platform_profile", c->profile);
    if (c->turbo >= 0)
        boolean("turbo", c->turbo);
    else
        null_("turbo");
    const cpulim_t *L = &s->power.lim;
    if (L->ok) { /* what applies: the lower of the CPU's and the firmware's */
        open_("power_limit", '{');
        pos("sustained_w", L->eff_long_w, 1);
        pos("sustained_window_s", L->long_s, 3);
        pos("boost_w", L->eff_short_w, 1);
        if (s->health.pl_known)
            boolean("holding_back", s->health.pl_now != 0);
        else
            null_("holding_back");
        close_('}');
    } else {
        null_("power_limit");
    }
    open_("per_core", '[');
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i];
        open_(NULL, '{');
        integer("cpu", id);
        if (id >= 0 && id < MAX_CPUS && c->core_type[id] >= 0) {
            char l[2] = { c->ctype[(int)c->core_type[id]].letter, 0 };
            s_("kind", l);
        }
        num("usage_pct", c->pct[i], 1);
        num("clock_mhz", c->mhz[i] > 0 ? c->mhz[i] : NAN, 0);
        double t = id >= 0 && id < MAX_CPUS ? sensors_core_temp(&s->sens, c->pkg_id[id], c->core_id[id] >= 0 ? c->core_id[id] : id) : -1;
        pos("temp_c", t, 1);
        close_('}');
    }
    close_(']');
    close_('}');
}

static void memory(sys_t *s)
{
    mem_t *m = &s->mem;
    open_("memory", '{');
    integer("total_bytes", (long long)m->total);
    integer("used_bytes", (long long)m->used);
    integer("available_bytes", (long long)m->avail);
    integer("cache_bytes", (long long)m->buffcache);
    num("used_pct", m->total ? 100.0 * (double)m->used / (double)m->total : NAN, 1);
    const machine_t *mc = &s->mach;
    if (mc->ram_state == 1) { /* the modules (root) */
        open_("modules", '[');
        for (int i = 0; i < mc->nram; i++) {
            const ramdev_t *r = &mc->ram[i];
            static const char *const form[] = { "", "DIMM", "SO-DIMM", "soldered" };
            open_(NULL, '{');
            sn("slot", r->slot);
            sn("bank", r->bank);
            integer("size_bytes", (long long)r->mb * 1048576LL);
            sn("type", r->type);
            sn("form", form[CLAMP(r->form, 0, 3)]);
            pos("speed_mts", r->speed > 0 ? r->speed : -1, 0);
            pos("configured_mts", r->conf > 0 ? r->conf : -1, 0);
            sn("maker", r->maker);
            sn("part", r->part);
            if (r->total_w && r->data_w)
                boolean("ecc_module", r->total_w > r->data_w);
            close_('}');
        }
        close_(']');
        integer("slots", mc->ram_slots);
        if (mc->ram_channels)
            integer("channels", mc->ram_channels);
        else
            null_("channels");
    } else {
        null_("modules");
    }
    { /* ECC: on or off, and the errors since boot */
        const ecc_t *e = &s->health.ecc;
        char why[96];
        int st = ecc_state(e, mc, why, sizeof why);
        open_("ecc", '{');
        if (st >= 0)
            boolean("on", st);
        else
            null_("on");
        sn("mode", e->mode);
        s_("source", e->ok ? "edac" : st >= 0 ? "firmware" : "none");
        if (e->ok) {
            integer("corrected", e->ce);
            integer("uncorrected", e->ue);
            open_("modules", '[');
            for (int i = 0; i < e->ndimm; i++) {
                open_(NULL, '{');
                s_("label", e->dimm[i].label);
                integer("corrected", e->dimm[i].ce);
                integer("uncorrected", e->dimm[i].ue);
                close_('}');
            }
            close_(']');
        } else {
            null_("corrected");
            null_("uncorrected");
        }
        close_('}');
    }
    integer("swap_total_bytes", (long long)m->swap_total);
    integer("swap_used_bytes", (long long)m->swap_used);
    open_("swaps", '[');
    for (int i = 0; i < m->nswap; i++) {
        const swapdev_t *d = &m->swap[i];
        open_(NULL, '{');
        s_("name", d->name);
        s_("type", d->zram ? "zram" : d->file ? "file" : "partition");
        integer("size_bytes", (long long)d->size);
        integer("used_bytes", (long long)d->used);
        integer("priority", d->prio);
        if (d->zram) {
            integer("stored_bytes", (long long)d->orig);
            integer("compressed_bytes", (long long)d->compr);
            integer("ram_bytes", (long long)d->ram);
            num("ratio", d->compr ? (double)d->orig / (double)d->compr : NAN, 2);
            sn("algorithm", d->algo);
        }
        close_('}');
    }
    close_(']');
    if (m->zswap >= 0)
        boolean("zswap", m->zswap);
    else
        null_("zswap");
    if (m->oom_ok)
        integer("oom_kills_since_boot", (long long)m->oom_kill);
    else
        null_("oom_kills_since_boot");
    close_('}');
}

static void gpus(sys_t *s)
{
    open_("gpus", '[');
    for (int i = 0; i < s->gpus.n; i++) {
        gpu_t *g = &s->gpus.g[i];
        open_(NULL, '{');
        s_("name", g->name);
        sn("driver", g->driver);
        boolean("integrated", g->integrated);
        pos("load_pct", g->util, 1);
        pos("temp_c", g->temp_c, 1);
        pos("power_w", g->power_w, 2);
        pos("clock_mhz", g->clk_mhz, 0);
        pos("vram_used_bytes", g->mem_used, 0);
        pos("vram_total_bytes", g->mem_total, 0);
        pos("fan_pct", g->fan_pct, 0);
        if (g->nv_state != NV_NONE) {
            s_("state", g->nv_state == NV_ASLEEP ? "asleep" : g->nv_state == NV_IDLE ? "idle" : "awake");
            pos("temp_limit_c", g->temp_lim, 0);
            pos("video_encode_pct", g->enc_pct, 0);
            pos("video_decode_pct", g->dec_pct, 0);
            pos("pcie_gen", g->pcie_gen > 0 ? g->pcie_gen : -1, 0);
            pos("pcie_width", g->pcie_width > 0 ? g->pcie_width : -1, 0);
            pos("pcie_gen_max", g->pcie_gen_max > 0 ? g->pcie_gen_max : -1, 0);
            pos("pcie_width_max", g->pcie_width_max > 0 ? g->pcie_width_max : -1, 0);
        }
        if (g->reasons_ok) {
            boolean("slowed_by_heat", (g->slow_now & GS_HEAT) != 0);
            boolean("slowed_by_power_limit", (g->slow_now & GS_POWER) != 0);
            pos("held_back_pct_of_busy_time", g->busy_s > 0 ? 100.0 * g->held_s / g->busy_s : -1, 1);
        }
        open_("screens", '[');
        for (int k = 0; k < g->nscr; k++) {
            const screen_t *sc = &g->scr[k];
            open_(NULL, '{');
            s_("port", sc->conn);
            sn("monitor", sc->monitor);
            boolean("built_in", sc->builtin);
            boolean("in_use", sc->on);
            if (sc->mode_w > 0) { /* the size it runs at, else its preferred size */
                integer("width", sc->mode_w);
                integer("height", sc->mode_h);
            } else if (sc->w > 0) {
                integer("width", sc->w);
                integer("height", sc->h);
            }
            pos("inches", sc->inch > 0 ? sc->inch : -1, 1);
            pos("refresh_hz", sc->hz > 0 ? sc->hz : -1, 1);
            pos("max_refresh_hz", sc->max_hz > 0 ? sc->max_hz : -1, 1);
            pos("monitor_max_refresh_hz", sc->mon_hz > 0 ? sc->mon_hz : -1, 1);
            double can;
            int why = screen_slow(sc, &can);
            if (why)
                s_("refresh_held_back_by", why == SCR_SETTING ? "setting" : "connection");
            else
                null_("refresh_held_back_by");
            if (sc->vrr_cap >= 0)
                boolean("vrr_capable", sc->vrr_cap);
            if (sc->vrr_on >= 0)
                boolean("vrr_on", sc->vrr_on);
            close_('}');
        }
        close_(']');
        char held[64];
        sn("kept_awake_by", gpu_screen_holds(g, held, sizeof held));
        close_('}');
    }
    close_(']');
}

static void power(sys_t *s)
{
    power_t *p = &s->power;
    open_("power", '{');
    pos("total_w", s->total_w, 2);
    sn("total_source", s->total_w_src);
    open_("rapl", '[');
    for (int i = 0; i < p->nrapl; i++)
        if (p->z[i].readable) {
            open_(NULL, '{');
            s_("name", p->z[i].name);
            pos("power_w", p->z[i].watts, 2);
            close_('}');
        }
    close_(']');
    open_("batteries", '[');
    for (int i = 0; i < p->nbat; i++) {
        battery_t *b = &p->bat[i];
        open_(NULL, '{');
        s_("name", b->name);
        sn("status", b->status);
        pos("charge_pct", b->capacity, 0);
        pos("power_w", b->watts, 2);
        pos("time_left_s", b->time_left, 0);
        pos("health_pct", b->health, 0);
        if (b->cycles >= 0)
            integer("cycles", b->cycles);
        else
            null_("cycles");
        if (b->lim_end > 0)
            integer("charge_limit_pct", b->lim_end);
        else
            null_("charge_limit_pct");
        close_('}');
    }
    close_(']');
    if (p->chg_offer_w > 0 || p->chg_w > 0) { /* a USB-C charger */
        open_("charger", '{');
        pos("offered_w", p->chg_offer_w, 1);
        sn("voltages", p->chg_volts);
        pos("agreed_w", p->chg_w, 1);
        pos("agreed_v", p->chg_v, 2);
        pos("agreed_a", p->chg_a, 2);
        if (p->chg_w > 0 && p->nbat)
            boolean("weak", charger_weak(p));
        close_('}');
    } else {
        null_("charger");
    }
    open_("device_batteries", '['); /* wireless mice, keyboards, headsets, controllers */
    for (int i = 0; i < p->ndev; i++) {
        const devbat_t *b = &p->dev[i];
        open_(NULL, '{');
        s_("name", b->name);
        pos("charge_pct", b->capacity, 0);
        sn("level", b->level);
        sn("status", b->status);
        boolean("low", b->low);
        close_('}');
    }
    close_(']');
    if (p->ac_online >= 0)
        boolean("ac_online", p->ac_online);
    else
        null_("ac_online");
    close_('}');
}

static void storage(sys_t *s)
{
    open_("disks", '[');
    for (int i = 0; i < s->disks.n; i++) {
        disk_t *d = &s->disks.d[i];
        open_(NULL, '{');
        s_("name", d->name);
        sn("model", d->model);
        sn("kind", d->kind);
        integer("size_bytes", (long long)d->size);
        num("read_bps", d->rd_bps, 0);
        num("write_bps", d->wr_bps, 0);
        num("busy_pct", d->util, 1);
        num("read_ms", d->rd_lat, 2);
        num("write_ms", d->wr_lat, 2);
        integer("in_flight", d->inflight);
        close_('}');
    }
    close_(']');
    open_("filesystems", '[');
    for (int i = 0; i < s->disks.nfs; i++) {
        fs_t *f = &s->disks.fs[i];
        open_(NULL, '{');
        s_("mount", f->mount);
        s_("device", f->dev);
        s_("type", f->type);
        integer("total_bytes", (long long)f->total);
        integer("used_bytes", (long long)f->used);
        integer("available_bytes", (long long)f->avail);
        if (f->trim == TRIM_NA) { /* not on an SSD */
            null_("trim");
        } else {
            static const char *const T[] = { "", "discard", "weekly", "none", "blocked" };
            open_("trim", '{');
            s_("how", T[CLAMP(f->trim, 0, 4)]);
            if (f->trim == TRIM_BLOCKED)
                s_("blocked_by", strstr(f->trim_why, "encryption") ? "encryption" : strstr(f->trim_why, "USB") ? "usb_adapter" : "drive");
            else if (f->trim_why[0])
                s_("timer", f->trim_why);
            pos("last_run_s_ago", f->trim_ago, 0);
            close_('}');
        }
        close_('}');
    }
    close_(']');
}

static void network(sys_t *s)
{
    open_("network", '[');
    for (int i = 0; i < s->net.n; i++) {
        netif_t *f = &s->net.i[i];
        open_(NULL, '{');
        s_("name", f->name);
        boolean("up", f->up);
        boolean("virtual", f->is_virtual);
        sn("address", f->addr);
        num("rx_bps", f->rx_bps, 0);
        num("tx_bps", f->tx_bps, 0);
        if (f->speed > 0)
            integer("speed_mbps", f->speed);
        else
            null_("speed_mbps");
        if (f->eth_ok) { /* a wired port: what it and the other end can do, as negotiated */
            pos("link_max_mbps", f->eth_max > 0 ? f->eth_max : -1, 0);
            pos("link_partner_max_mbps", f->eth_peer > 0 ? f->eth_peer : -1, 0);
            sn("duplex", f->eth_duplex == 1 ? "full" : f->eth_duplex == 0 ? "half" : "");
            if (f->eth_slow)
                s_("link_problem", f->eth_slow == ETH_CABLE ? "cable" : f->eth_slow == ETH_HALF ? "half_duplex" : "slower");
            else
                null_("link_problem");
        }
        num("errors_per_s", f->err_rate, 2);
        num("drops_per_s", f->drop_rate, 2);
        if (f->wireless && f->quality >= 0) {
            num("signal_dbm", f->signal_dbm, 0);
            integer("link_quality_pct", f->quality);
        }
        if (f->wireless && f->wifi.ok) {
            const wifi_t *w = &f->wifi;
            open_("wifi", '{');
            s_("band", wifi_band(w->freq));
            integer("frequency_mhz", w->freq);
            integer("channel", w->channel);
            if (w->width)
                integer("width_mhz", w->width);
            else
                null_("width_mhz");
            if (w->gen)
                integer("generation", w->gen);
            else
                null_("generation");
            pos("tx_mbps", w->tx_mbps, 1);
            pos("rx_mbps", w->rx_mbps, 1);
            pos("resent_pct", w->retry_pct, 1);
            integer("connected_s", w->connected_s);
            if (w->crowd_ok) { /* the other networks the last scan found */
                open_("neighbours", '{');
                integer("same_channel", w->crowd_same);
                integer("overlapping", w->crowd_near);
                integer("strong", w->crowd_strong);
                integer("on_band", w->crowd_band);
                pos("scan_age_s", w->crowd_age, 0);
                if (w->freq < 3000) {
                    open_("channels_1_6_11", '[');
                    for (int k = 0; k < 3; k++)
                        integer(NULL, w->ch24[k]);
                    close_(']');
                }
                close_('}');
            } else {
                null_("neighbours");
            }
            close_('}');
        } else if (f->wireless) {
            null_("wifi");
        }
        close_('}');
    }
    close_(']');
}

/* the network check: pings to the router and the internet, null while it's off */
static void network_check(sys_t *s)
{
    const netq_t *q = &s->net.q;
    if (!q->on) {
        null_("network_check");
        return;
    }
    static const char *const nm[NQ_COUNT] = { "router", "internet", "dns" };
    open_("network_check", '{');
    for (int k = 0; k < NQ_COUNT; k++) {
        const nqt_t *t = &q->t[k];
        open_(nm[k], '{');
        sn("address", t->addr);
        pos("ping_ms", t->rtt_ms, 1);
        pos("average_ms", t->avg_ms, 1);
        pos("jitter_ms", t->jitter_ms, 1);
        pos("lost_pct", t->loss_pct, 0);
        boolean("trouble", t->trouble != NQ_FINE);
        close_('}');
    }
    close_('}');
}

static void pci(sys_t *s)
{
    const pcibus_t *b = &s->pcibus;
    if (!b->ok) {
        null_("pci");
        return;
    }
    open_("pci", '[');
    for (int i = 0; i < b->n; i++) {
        const pcid_t *d = &b->d[i];
        char h[16];
        open_(NULL, '{');
        s_("slot", d->slot);
        sfmt(h, sizeof h, "0x%06x", d->cls);
        s_("class", h);
        s_("kind", d->kind);
        sfmt(h, sizeof h, "%04x:%04x", d->ven, d->dev);
        s_("id", h);
        s_("name", d->name);
        sn("driver", d->driver);
        boolean("listed", d->shown);       /* false: a bridge or chipset part */
        boolean("missing_driver", d->needs); /* a device people use, with none */
        boolean("virtual_function", d->vf);
        close_('}');
    }
    close_(']');
}

static void usb(sys_t *s)
{
    const usb_t *u = &s->usb;
    if (!u->ok) {
        null_("usb");
        return;
    }
    open_("usb", '[');
    for (int i = 0; i < u->n; i++) {
        const usbdev_t *d = &u->d[i];
        char id[12];
        open_(NULL, '{');
        s_("port", d->id);
        s_("name", d->name);
        sfmt(id, sizeof id, "%04x:%04x", d->vid, d->pid);
        s_("id", id);
        s_("kind", d->kind);
        sn("disk", d->blk);
        num("speed_mbps", d->mbps, d->mbps < 12 ? 1 : 0);
        if (d->can > 0)
            integer("can_do_mbps", d->can);
        else
            null_("can_do_mbps");
        boolean("slow", d->slow);
        if (d->port3 >= 0)
            boolean("usb3_port", d->port3);
        else
            null_("usb3_port");
        s_("where", d->where == 1 ? "socket" : d->where == 2 ? "built in" : d->where == 3 ? "behind a hub" : "unknown");
        integer("power_ma", d->ma);
        close_('}');
    }
    close_(']');
}

static void sensors(sys_t *s)
{
    static const char *const type[] = { "temp", "fan", "power", "voltage", "other" };
    open_("sensors", '[');
    for (int i = 0; i < s->sens.n; i++) {
        sensor_t *t = &s->sens.s[i];
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        open_(NULL, '{');
        s_("id", t->id);
        s_("chip", chip);
        s_("label", t->label);
        if (t->name[0])
            s_("name", t->name);
        s_("type", type[t->type]);
        num("value", t->ok ? t->value : NAN, t->type == SENS_VOLT || t->type == SENS_OTHER ? 3 : 1);
        s_("unit", sensor_unit(t));
        if (t->file) { /* your own value, from a file */
            s_("file", t->path);
            boolean("stale", t->fstale);
            if (t->ftotal)
                boolean("machine_total", 1);
        }
        if (t->type == SENS_TEMP && t->crit > 0)
            num("critical", t->crit, 1);
        if (t->ulo > 0)
            num("your_min", t->ulo, 3);
        if (t->uhi > 0)
            num("your_max", t->uhi, 3);
        if (t->ulo > 0 || t->uhi > 0)
            boolean("past_limit", sensor_bad(t));
        if (t->hidden)
            boolean("hidden", 1);
        close_('}');
    }
    close_(']');
}

static void health(sys_t *s)
{
    health_t *h = &s->health;
    open_("health", '{');
    char al[64];
    int lvl = health_alert(h, al, sizeof al);
    s_("status", lvl > 1 ? "problem" : lvl ? "warning" : "ok");
    sn("problem", al);
    open_("pressure", '{');
    static const char *const nm[3] = { "cpu", "memory", "io" };
    for (int i = 0; i < 3; i++) {
        psi_t *p = &h->psi[i];
        if (!p->ok) {
            null_(nm[i]);
            continue;
        }
        open_(nm[i], '{');
        num("some_avg10_pct", p->avg10, 2);
        num("some_avg60_pct", p->avg60, 2);
        if (p->has_full)
            num("full_avg10_pct", p->favg10, 2);
        close_('}');
    }
    close_('}');
    open_("since_start", '{'); /* counted while this hsm runs (none for --once / --json) */
    integer("freezes", h->freezes);
    num("frozen_s", h->frozen_total, 1);
    num("longest_freeze_s", h->freeze_max, 1);
    integer("suspends", h->suspends);
    integer("link_drops", h->link_drops);
    integer("fans_stopped", h->fan_stops);
    close_('}');
    if (h->thr_ok) {
        open_("throttling", '{');
        num("now_pct", h->thr_pct, 1);
        integer("events_since_boot", (long long)(h->thr_count + h->core_count));
        num("package_ms_since_boot", h->thr_ms, 0);
        close_('}');
    } else {
        null_("throttling");
    }
    if (h->aer_ok) {
        open_("pcie_errors", '{');
        integer("corrected", (long long)h->pci_cor);
        integer("nonfatal", (long long)h->pci_nonfatal);
        integer("fatal", (long long)h->pci_fatal);
        close_('}');
    } else {
        null_("pcie_errors");
    }
    open_("drives", '[');
    for (int i = 0; i < h->nnvme; i++) {
        nvme_t *v = &h->nvme[i];
        open_(NULL, '{');
        s_("name", v->name);
        s_("type", "nvme");
        sn("model", v->model);
        s_("state", v->state == 1 ? "ok" : v->state == -1 ? "needs root" : v->state == 0 ? "not read yet" : "n/a");
        if (v->link_ok) { /* its PCIe link */
            open_("pcie", '{');
            integer("gen", v->link.gen);
            integer("width", v->link.width);
            integer("gen_max", v->link.best_gen);
            integer("width_max", v->link.best_width);
            boolean("slower_than_it_could", v->link.below);
            boolean("slot_limits_it", v->link.slot_less);
            close_('}');
        }
        if (v->state == 1) {
            integer("critical_warning", v->crit);
            integer("wear_used_pct", v->used);
            integer("spare_pct", v->spare);
            pos("temp_c", v->temp_c, 0);
            integer("power_on_hours", (long long)v->hours);
            integer("unsafe_shutdowns", (long long)v->unsafe);
            integer("media_errors", (long long)v->media_err);
            open_("heat_slowdowns", '{'); /* counted by the drive since new */
            integer("light", (long long)v->heat_n[0]);
            integer("light_s", (long long)v->heat_s[0]);
            integer("hard", (long long)v->heat_n[1]);
            integer("hard_s", (long long)v->heat_s[1]);
            boolean("now", v->heat_open);
            close_('}');
        }
        close_('}');
    }
    for (int i = 0; i < h->nsata; i++) {
        sata_t *d = &h->sata[i];
        open_(NULL, '{');
        s_("name", d->name);
        s_("type", d->usb ? (d->rotational ? "usb-hdd" : "usb-ssd") : d->rotational ? "sata-hdd" : "sata-ssd");
        sn("model", d->model);
        s_("state", d->state == 1 ? "ok" : d->state == -1 ? "needs root" : d->state == -3 ? "asleep" : d->state == 0 ? "not read yet"
                  : d->state == -4 ? "read when asked" : "n/a");
        if (d->usb && d->read_at > 0)
            integer("read_at", (long long)d->read_at);
        if (d->state == 1) {
            boolean("failing", d->failing);
            sn("failing_attribute", d->fail_attr);
            pos("wear_used_pct", d->wear, 0);
            pos("temp_c", d->temp_c, 0);
#define LL(k, v) do { if ((v) >= 0) integer(k, v); else null_(k); } while (0)
            LL("power_on_hours", d->hours);
            LL("reallocated_sectors", d->realloc);
            LL("pending_sectors", d->pending);
            LL("uncorrectable_errors", d->uncorrect);
            LL("cable_errors", d->crc);
            LL("unsafe_shutdowns", d->unsafe);
#undef LL
        }
        close_('}');
    }
    close_(']');
    open_("arrays", '['); /* software RAID, ZFS pools, btrfs file systems */
    for (int i = 0; i < h->arr.n; i++) {
        const array_t *a = &h->arr.a[i];
        char why[48];
        int l = array_problem(a, why, sizeof why);
        open_(NULL, '{');
        s_("type", a->kind == ARR_MD ? "md" : a->kind == ARR_ZFS ? "zfs" : "btrfs");
        s_("name", a->name);
        if (a->kind == ARR_BTRFS)
            s_("uuid", a->key);
        sn("level", a->level);
        if (a->kind == ARR_BTRFS)
            sn("metadata", a->meta);
        sn("state", a->state);
        s_("health", l == 2 ? "bad" : l == 1 ? "attention" : "ok");
        sn("problem", why);
        if (a->kind != ARR_ZFS) {
            integer("disks", a->ndisks);
            integer("degraded", a->degraded);
            open_("members", '[');
            for (int k = 0; k < a->nmem; k++) {
                const member_t *m = &a->mem[k];
                open_(NULL, '{');
                s_("name", m->name);
                s_("state", m->bad ? (a->kind == ARR_BTRFS ? "missing" : "failed") : m->spare ? "spare" : "ok");
                if (a->kind == ARR_BTRFS) {
                    integer("write_errors", m->err[BE_WRITE]);
                    integer("read_errors", m->err[BE_READ]);
                    integer("flush_errors", m->err[BE_FLUSH]);
                    integer("corruption_errors", m->err[BE_CORRUPT]);
                    integer("generation_errors", m->err[BE_GEN]);
                } else {
                    integer("corrected_read_errors", m->err[BE_READ]);
                }
                close_('}');
            }
            close_(']');
        } else {
            boolean("degraded", a->degraded);
        }
        if (a->kind == ARR_MD) {
            sn("sync", a->sync);
            pos("sync_pct", a->sync_pct, 1);
            pos("sync_left_s", a->sync_left_s, 0);
            pos("sync_bps", a->sync_kbs > 0 ? a->sync_kbs * 1024 : -1, 0);
            integer("mismatch_sectors", a->mismatch);
        }
        close_('}');
    }
    close_(']');
    s_("kernel_log", h->kmsg == 1 ? "read" : h->kmsg == -1 ? "needs root" : "n/a");
    open_("events", '[');
    for (int i = h->nev - 1; i >= 0; i--) { /* oldest first */
        const event_t *e = health_event_at(h, i);
        char ts[40];
        time_t t = (time_t)e->t;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);
        open_(NULL, '{');
        s_("time", ts);
        s_("level", e->level == EV_ERR ? "error" : e->level == EV_WARN ? "warning" : "info");
        s_("category", e->cat);
        s_("text", e->text);
        close_('}');
    }
    close_(']');
    close_('}');
}

static void services(sys_t *s)
{
    if (s->svc.state != 1) {
        null_("services");
        return;
    }
    open_("services", '[');
    for (int i = 0; i < s->svc.n; i++) {
        svc_t *v = &s->svc.s[i];
        open_(NULL, '{');
        s_("name", v->name);
        s_("cgroup", v->path);
        s_("type", svc_kind_name(v->kind));
        num("cpu_pct", v->cpu, 1);
        if (v->mem >= 0)
            integer("memory_bytes", v->mem);
        else
            null_("memory_bytes");
        num("read_bps", v->rd_bps, 0);
        num("write_bps", v->wr_bps, 0);
        if (v->tasks >= 0)
            integer("tasks", v->tasks);
        else
            null_("tasks");
        close_('}');
    }
    close_(']');
}

static const proc_t *g_pp;

static int by_cpu(const void *a, const void *b)
{
    const proc_t *p = &g_pp[*(const int *)a], *q = &g_pp[*(const int *)b];
    if (p->cpu != q->cpu)
        return p->cpu < q->cpu ? 1 : -1;
    return (p->rss < q->rss) - (p->rss > q->rss);
}

static void processes(sys_t *s, int nproc)
{
    procs_t *ps = &s->procs;
    int *ord = malloc((size_t)MAX(ps->n, 1) * sizeof *ord);
    open_("processes", '[');
    if (ord) {
        for (int i = 0; i < ps->n; i++)
            ord[i] = i;
        g_pp = ps->p;
        if (ps->n > 1)
            qsort(ord, (size_t)ps->n, sizeof *ord, by_cpu);
        int n = nproc < 0 ? ps->n : MIN(nproc, ps->n);
        for (int k = 0; k < n; k++) {
            proc_t *p = &ps->p[ord[k]];
            char st[2] = { p->state, 0 };
            open_(NULL, '{');
            integer("pid", p->pid);
            integer("ppid", p->ppid);
            s_("user", uid_name(p->uid));
            s_("name", p->comm);
            s_("state", st);
            boolean("kernel_thread", p->kthread);
            num("cpu_pct", p->cpu, 1);
            num("memory_pct", p->mem, 2);
            integer("rss_bytes", (long long)p->rss);
            integer("threads", p->nthreads);
            integer("nice", p->nice);
            num("read_bps", p->io_ok ? p->rd_bps : NAN, 0);
            num("write_bps", p->io_ok ? p->wr_bps : NAN, 0);
            pos("gpu_pct", p->gpu, 1);
            if (s->procs.net_ok && p->net_rx >= 0) { /* TCP only */
                num("net_in_bps", p->net_rx, 0);
                num("net_out_bps", p->net_tx, 0);
            }
            netport_t np[16];
            int nnp = p->kthread ? 0 : netproc_ports(p->pid, np, 16);
            if (nnp) {
                open_("listens", '[');
                for (int j = 0; j < nnp; j++) {
                    open_(NULL, '{');
                    s_("proto", np[j].udp ? "udp" : "tcp");
                    integer("port", np[j].port);
                    boolean("local_only", np[j].local);
                    close_('}');
                }
                close_(']');
            }
            if (p->nfds >= 0)
                integer("open_files", p->nfds);
            else
                null_("open_files");
            num("cpu_time_s", ps->hz > 0 ? (double)p->ticks / (double)ps->hz : NAN, 2);
            int flagged, busy = runaway_minutes(p->pid, p->start, &flagged);
            integer("busy_minutes", busy); /* a whole core or more, without a break */
            boolean("runaway", flagged);
            sn("command", p->kthread ? "" : proc_cmdline(p));
            close_('}');
        }
    }
    close_(']');
    free(ord);
}

void json_write(FILE *f, sys_t *s, int nproc)
{
    g_out = f;
    char ts[40];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);
    g_depth = 0;
    g_first[0] = 1;
    fputc_out('{');
    g_first[++g_depth] = 1;
    s_("hsm_version", APP_VERSION);
    s_("time", ts);
    s_("host", s->host);
    s_("os", s->os);
    s_("kernel", s->kernel);
    s_("arch", s->arch);
    num("uptime_s", s->cpu.uptime, 0);
    boolean("root", geteuid() == 0);
    if (s->mach.ok) {
        const machine_t *mc = &s->mach;
        char d[16] = "";
        if (mc->bios_date)
            sfmt(d, sizeof d, "%04d-%02d-%02d", mc->bios_date / 10000, mc->bios_date / 100 % 100, mc->bios_date % 100);
        open_("machine", '{');
        sn("vendor", mc->vendor);
        sn("model", mc->model);
        sn("type", mc->type);
        sn("board", mc->board);
        sn("bios_vendor", mc->bios_vendor);
        sn("bios_version", mc->bios_version);
        sn("bios_date", d);
        close_('}');
    } else {
        null_("machine");
    }
    { /* what the firmware switched on */
        const firmware_t *fw = &s->fw;
        static const char *const vs[] = { NULL, "on", "no_kvm", "off", "maybe_off", "none", "vm" };
        open_("firmware", '{');
        s_("boot", fw->uefi == 1 ? "uefi" : fw->uefi == 0 ? "bios" : NULL);
        pos("efi_bits", fw->efi_bits > 0 ? fw->efi_bits : -1, 0);
        if (fw->secure_boot >= 0)
            boolean("secure_boot", fw->secure_boot);
        else
            null_("secure_boot");
        if (fw->setup_mode >= 0)
            boolean("setup_mode", fw->setup_mode);
        else
            null_("setup_mode");
        sn("lockdown", fw->lockdown);
        s_("tpm", fw->tpm == 20 ? "2.0" : fw->tpm == 12 ? "1.2" : fw->tpm ? "unknown" : NULL);
        s_("virtualization", fw->virt == 'v' ? "vt-x" : fw->virt == 's' ? "amd-v" : fw->virt == 'a' ? "arm" : NULL);
        s_("virtualization_state", vs[CLAMP(fw->virt_state, 0, 6)]);
        boolean("kvm", fw->kvm);
        boolean("virtual_machine", fw->vm);
        boolean("iommu", fw->iommu > 0);
        sn("iommu_type", fw->iommu_kind);
        integer("iommu_groups", fw->iommu_groups);
        close_('}');
    }
    cpu(s);
    memory(s);
    gpus(s);
    power(s);
    storage(s);
    network(s);
    network_check(s);
    usb(s);
    pci(s);
    sensors(s);
    health(s);
    services(s);
    processes(s, nproc);
    close_('}');
    fputc_out('\n');
}

void json_print(sys_t *s, int nproc)
{
    json_write(stdout, s, nproc);
}
