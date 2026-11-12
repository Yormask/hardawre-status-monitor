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
/* health.c - signs of trouble: thermal throttling, pressure stalls, out-of-
 * memory kills, PCIe bus errors, NVMe drive health and the kernel's own
 * error messages, plus a list of "events" that collects all of these with
 * a time stamp, for the screen and for the logs. */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/nvme_ioctl.h>)
#include <linux/nvme_ioctl.h>
#define HAVE_NVME_IOCTL 1
#endif
#endif

#define NVME_EVERY 60.0 /* seconds between drive health reads */

static double wall_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- events */

static void ev_add(health_t *h, double t, int level, const char *cat, const char *text)
{
    event_t *e = &h->ev[h->ev_head];
    h->ev_head = (h->ev_head + 1) % MAX_EVENTS;
    if (h->nev < MAX_EVENTS)
        h->nev++;
    e->t = t;
    e->seq = ++h->ev_seq;
    e->level = level;
    str_copy(e->cat, sizeof e->cat, cat);
    str_copy(e->text, sizeof e->text, text);
}

void health_event(health_t *h, int level, const char *cat, const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    ev_add(h, wall_now(), level, cat, b);
}

const event_t *health_event_at(const health_t *h, int i)
{
    if (i < 0 || i >= h->nev)
        return NULL;
    return &h->ev[(h->ev_head - 1 - i + 2 * MAX_EVENTS) % MAX_EVENTS];
}

int health_alert(const health_t *h, char *out, size_t n)
{
    double now = wall_now();
    const char *what[6];
    int k = 0, lvl = 0;
    if (h->thr_last > 0 && now - h->thr_last < 10) {
        what[k++] = "throttling";
        lvl = 2;
    }
    if (h->hot_last > 0 && now - h->hot_last < 10) {
        what[k++] = "too hot";
        lvl = 2;
    }
    if (h->oom_last > 0 && now - h->oom_last < 60) {
        what[k++] = "out of memory";
        lvl = 2;
    }
    for (int i = 0; i < h->nnvme; i++)
        if (h->nvme[i].state == 1 && h->nvme[i].crit) {
            what[k++] = "drive warning";
            lvl = 2;
            break;
        }
    if (h->pci_last > 0 && now - h->pci_last < 60) {
        what[k++] = "PCIe errors";
        lvl = MAX(lvl, 1);
    }
    if (h->kmsg_last > 0 && now - h->kmsg_last < 60) {
        what[k++] = "kernel error";
        lvl = MAX(lvl, 1);
    }
    if (!k) {
        out[0] = '\0';
        return 0;
    }
    if (k == 1)
        str_copy(out, n, what[0]);
    else
        sfmt(out, n, "%s +%d", what[0], k - 1);
    return lvl;
}

/* ------------------------------------------------- pressure stall info */

static void psi_sample(psi_t *p, const char *name, double dt)
{
    char path[PATHBUF], buf[512];
    sfmt(path, sizeof path, "%s/pressure/%s", g_procfs, name);
    if (read_file(path, buf, sizeof buf) <= 0) { /* Linux < 4.20, or booted with psi=0 */
        p->ok = 0;
        return;
    }
    p->ok = 1;
    char *cur = buf, *line;
    while ((line = next_line(&cur)) != NULL) {
        double a10, a60, a300;
        unsigned long long tot;
        int full = !strncmp(line, "full", 4);
        if (sscanf(line + 4, " avg10=%lf avg60=%lf avg300=%lf total=%llu", &a10, &a60, &a300, &tot) != 4)
            continue;
        double pct = 0;
        unsigned long long *prev = full ? &p->p_full : &p->p_some;
        if (p->primed && dt > 0 && tot >= *prev)
            pct = CLAMP((double)(tot - *prev) / (dt * 1e6) * 100.0, 0, 100); /* total is in us */
        *prev = tot;
        if (full) {
            p->has_full = 1;
            p->full = pct;
            p->favg10 = a10;
            p->favg60 = a60;
            p->favg300 = a300;
        } else {
            p->some = pct;
            p->avg10 = a10;
            p->avg60 = a60;
            p->avg300 = a300;
        }
    }
    p->primed = 1;
}

/* ------------------------------------------------------ thermal throttling */

/* Intel CPUs count every time a core or the whole package had to slow down
 * because it reached its temperature limit (PROCHOT), and since Linux 5.5
 * also for how long. The counters live under each CPU's thermal_throttle/. */
static int thr_read(int cpu, const char *what, unsigned long long *out)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/thermal_throttle/%s", g_sysfs, cpu, what);
    return read_ull(p, out);
}

static void throttle_sample(health_t *h, const cpu_t *c, double dt, double now)
{
    if (!c->ncpu)
        return;
    char p[PATHBUF];
    if (!h->thr_tried) {
        sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/thermal_throttle", g_sysfs, c->id[0]);
        h->thr_ok = path_exists(p);
        h->thr_tried = 1;
    }
    if (!h->thr_ok)
        return;
    int first = !h->thr_start && !h->thr_count && !h->core_count; /* baseline on the first read */

    /* packages */
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i], pkg = c->pkg_id[id] >= 0 ? c->pkg_id[id] : 0, k;
        for (k = 0; k < h->nthr && h->thr[k].pkg != pkg; k++)
            ;
        if (k == h->nthr && h->nthr < MAX_PKGS) {
            memset(&h->thr[k], 0, sizeof h->thr[k]);
            h->thr[k].pkg = pkg;
            h->thr[k].cpu = id;
            h->thr[k].p_count = (unsigned long long)-1;
            h->nthr++;
        }
    }
    int events = 0;
    double worst = 0, ms = 0;
    unsigned long long cnt = 0, ccnt = 0;
    for (int k = 0; k < h->nthr; k++) {
        thr_pkg_t *t = &h->thr[k];
        unsigned long long n = 0, m = 0;
        thr_read(t->cpu, "package_throttle_count", &n);
        int has_ms = thr_read(t->cpu, "package_throttle_total_time_ms", &m) == 0;
        t->pct = 0;
        if (t->p_count != (unsigned long long)-1 && dt > 0) {
            if (n > t->p_count)
                events++;
            if (has_ms && m >= t->p_ms)
                t->pct = CLAMP((double)(m - t->p_ms) / (dt * 10.0), 0, 100);
            else if (n > t->p_count)
                t->pct = 100; /* old kernels: no time, just "it happened" */
        }
        t->count = n;
        t->ms = m;
        t->p_count = n;
        t->p_ms = m;
        cnt += n;
        ms += (double)m;
        worst = MAX(worst, t->pct);
    }
    /* cores: one CPU per physical core (hyper-threads share the counters) */
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i], dup = 0;
        if (id < 0 || id >= MAX_CPUS)
            continue;
        for (int j = 0; j < i && !dup; j++) {
            int o = c->id[j];
            dup = o >= 0 && o < MAX_CPUS && c->core_id[o] == c->core_id[id] && c->pkg_id[o] == c->pkg_id[id] &&
                  c->core_id[id] >= 0;
        }
        if (dup)
            continue;
        unsigned long long n = 0, m = 0;
        if (thr_read(id, "core_throttle_count", &n))
            continue;
        int has_ms = thr_read(id, "core_throttle_total_time_ms", &m) == 0;
        if (!first && dt > 0) {
            if (n > h->core_cnt[id])
                events++;
            if (has_ms && m >= h->core_ms[id])
                worst = MAX(worst, CLAMP((double)(m - h->core_ms[id]) / (dt * 10.0), 0, 100));
        }
        h->core_cnt[id] = n;
        h->core_ms[id] = m;
        ccnt += n;
    }
    h->thr_count = cnt;
    h->core_count = ccnt;
    h->thr_ms = ms;
    h->thr_pct = worst;
    if (first)
        h->thr_start = cnt + ccnt;
    if (events) {
        h->thr_last = now;
        if (now - h->thr_ev_t >= 60) {
            h->thr_ev_t = now;
            if (c->temp_c >= 0)
                health_event(h, EV_WARN, "thermal", "CPU slowed down to protect itself from heat (%.0f °C)", c->temp_c);
            else
                health_event(h, EV_WARN, "thermal", "CPU slowed down to protect itself from heat");
        }
    }
}

/* ------------------------------------------------------------ PCIe (AER) */

static unsigned long long aer_total(const char *dev, const char *file, const char *key, int *ok)
{
    char p[PATHBUF], buf[2048];
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/%s", g_sysfs, dev, file);
    if (read_file(p, buf, sizeof buf) <= 0)
        return 0;
    *ok = 1;
    char *k = strstr(buf, key);
    return k ? strtoull(k + strlen(key), NULL, 10) : 0;
}

static void pci_name(const char *dev, char *out, size_t n)
{
    char p[PATHBUF];
    unsigned ven = 0, did = 0;
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/vendor", g_sysfs, dev);
    read_hex(p, &ven);
    sfmt(p, sizeof p, "%s/bus/pci/devices/%s/device", g_sysfs, dev);
    read_hex(p, &did);
    if (pci_lookup_name(ven, did, out, n))
        sfmt(out, n, "PCI device %04x:%04x", ven, did);
}

static void aer_sample(health_t *h, double now)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/bus/pci/devices", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    unsigned long long tc = 0, tn = 0, tf = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int ok = 0;
        unsigned long long c = aer_total(e->d_name, "aer_dev_correctable", "TOTAL_ERR_COR ", &ok);
        unsigned long long nf = aer_total(e->d_name, "aer_dev_nonfatal", "TOTAL_ERR_NONFATAL ", &ok);
        unsigned long long f = aer_total(e->d_name, "aer_dev_fatal", "TOTAL_ERR_FATAL ", &ok);
        if (!ok)
            continue;
        h->aer_ok = 1;
        tc += c;
        tn += nf;
        tf += f;
        if (!c && !nf && !f)
            continue;
        int k;
        for (k = 0; k < h->npci && strcmp(h->pci[k].dev, e->d_name); k++)
            ;
        if (k == h->npci) {
            if (h->npci >= MAX_PCIDEV)
                continue;
            pcidev_t *pd = &h->pci[h->npci++];
            memset(pd, 0, sizeof *pd);
            str_copy(pd->dev, sizeof pd->dev, e->d_name);
            pci_name(e->d_name, pd->name, sizeof pd->name);
            if (h->primed) { /* the device had none before */
                pd->cor = pd->nonfatal = pd->fatal = 0;
            } else {
                pd->cor = c;
                pd->nonfatal = nf;
                pd->fatal = f;
            }
        }
        pcidev_t *pd = &h->pci[k];
        if (c > pd->cor || nf > pd->nonfatal || f > pd->fatal) {
            int bad = nf > pd->nonfatal || f > pd->fatal;
            health_event(h, bad ? EV_ERR : EV_WARN, "pcie", "PCIe %s: %llu new %s error%s (%s)", pd->dev,
                         bad ? (nf - pd->nonfatal) + (f - pd->fatal) : c - pd->cor, bad ? "uncorrected" : "corrected",
                         (bad ? (nf - pd->nonfatal) + (f - pd->fatal) : c - pd->cor) == 1 ? "" : "s", pd->name);
            h->pci_last = now;
        }
        pd->cor = c;
        pd->nonfatal = nf;
        pd->fatal = f;
    }
    closedir(d);
    h->pci_cor = tc;
    h->pci_nonfatal = tn;
    h->pci_fatal = tf;
}

/* ---------------------------------------------------------- NVMe health */

#ifndef HAVE_NVME_IOCTL /* the kernel ABI, for systems without the header */
struct nvme_admin_cmd {
    uint8_t opcode, flags;
    uint16_t rsvd1;
    uint32_t nsid, cdw2, cdw3;
    uint64_t metadata, addr;
    uint32_t metadata_len, data_len;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
    uint32_t timeout_ms, result;
};
#define NVME_IOCTL_ADMIN_CMD _IOWR('N', 0x41, struct nvme_admin_cmd)
#endif

static unsigned long long le(const unsigned char *b, int n)
{
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

/* "SMART / Health Information" log page (NVMe spec, Get Log Page 02h): a
 * read-only query the drive answers from its own counters. */
static int nvme_read_log(const char *name, unsigned char *buf, int *state)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s", g_devfs, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        *state = (errno == EACCES || errno == EPERM) ? -1 : -2;
        return -1;
    }
    struct stat st;
    int r = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) { /* test hook: a captured log page */
        r = read(fd, buf, 512) == 512 ? 0 : -1;
    } else if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
        struct nvme_admin_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.opcode = 0x02; /* Get Log Page */
        cmd.nsid = 0xffffffffu;
        cmd.addr = (uint64_t)(uintptr_t)buf;
        cmd.data_len = 512;
        cmd.cdw10 = (127u << 16) | 0x02; /* 128 dwords of log page 02h */
        r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0 ? 0 : -1;
        if (r)
            *state = (errno == EACCES || errno == EPERM) ? -1 : -2;
    }
    close(fd);
    if (!r)
        *state = 1;
    else if (*state == 1 || *state == 0)
        *state = -2;
    return r;
}

static void nvme_find(health_t *h)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/class/nvme", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && h->nnvme < MAX_NVME) {
        if (strncmp(e->d_name, "nvme", 4) || !e->d_name[4])
            continue;
        nvme_t *v = &h->nvme[h->nnvme++];
        memset(v, 0, sizeof *v);
        str_copy(v->name, sizeof v->name, e->d_name);
        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%s/model", base, e->d_name);
        if (read_line(p, v->model, sizeof v->model))
            v->model[0] = '\0';
    }
    closedir(d);
}

static void nvme_sample(health_t *h)
{
    static const char *const warn[] = { "spare space below threshold", "temperature outside its limits",
                                        "reliability degraded", "drive switched to read-only",
                                        "backup power failed", "memory region read-only" };
    for (int i = 0; i < h->nnvme; i++) {
        nvme_t *v = &h->nvme[i];
        unsigned char b[512];
        int was = v->state;
        unsigned old_crit = v->crit;
        unsigned long long old_media = v->media_err;
        if (nvme_read_log(v->name, b, &v->state))
            continue;
        v->crit = b[0];
        unsigned k = (unsigned)le(b + 1, 2);
        v->temp_c = k > 0 ? (double)k - 273.15 : -1;
        v->spare = b[3];
        v->spare_thr = b[4];
        v->used = b[5];
        v->read_b = (double)le(b + 32, 8) * 512000.0; /* units of 1000 x 512 bytes */
        v->written_b = (double)le(b + 48, 8) * 512000.0;
        v->cycles = le(b + 112, 8);
        v->hours = le(b + 128, 8);
        v->unsafe = le(b + 144, 8);
        v->media_err = le(b + 160, 8);
        v->err_log = le(b + 176, 8);
        v->warn_min = le(b + 192, 4);
        v->crit_min = le(b + 196, 4);
        for (int bit = 0; bit < 6; bit++)
            if ((v->crit & (1u << bit)) && (was != 1 || !(old_crit & (1u << bit))))
                health_event(h, EV_ERR, "disk", "%s: %s", v->name, warn[bit]);
        if (was == 1 && v->media_err > old_media)
            health_event(h, EV_ERR, "disk", "%s: %llu new media / data integrity error%s", v->name,
                         v->media_err - old_media, v->media_err - old_media == 1 ? "" : "s");
    }
}

/* ------------------------------------------------------------ kernel log */

/* Which kernel messages matter here. Matched anywhere in the text, at any
 * log level; anything at "error" level or worse is kept as well. */
static const struct {
    const char *needle, *cat;
    int level;
} KMSG[] = {
    { "temperature above threshold", "thermal", EV_WARN },
    { "clock throttled", "thermal", EV_WARN },
    { "critical temperature", "thermal", EV_ERR },
    { "thermal shutdown", "thermal", EV_ERR },
    { "Thermal shutdown", "thermal", EV_ERR },
    { "Out of memory", "memory", EV_ERR },
    { "invoked oom-killer", "memory", EV_ERR },
    { "Killed process", "memory", EV_ERR },
    { "Memory failure", "memory", EV_ERR },
    { "Hardware Error", "memory", EV_ERR },
    { "Machine check", "memory", EV_ERR },
    { "EDAC", "memory", EV_WARN },
    { "I/O error", "disk", EV_ERR },
    { "critical medium error", "disk", EV_ERR },
    { "Medium Error", "disk", EV_ERR },
    { "EXT4-fs error", "disk", EV_ERR },
    { "BTRFS error", "disk", EV_ERR },
    { "BTRFS critical", "disk", EV_ERR },
    { "Corruption", "disk", EV_ERR },
    { "filesystem read-only", "disk", EV_ERR },
    { "forced readonly", "disk", EV_ERR },
    { "has been shut down", "disk", EV_ERR },
    { "exception Emask", "disk", EV_WARN },
    { "hard resetting link", "disk", EV_WARN },
    { "controller is down", "disk", EV_ERR },
    { "resetting controller", "disk", EV_WARN },
    { "I/O timeout", "disk", EV_WARN },
    { "GPU HANG", "gpu", EV_ERR },
    { "GPU hang", "gpu", EV_ERR },
    { "GPU reset", "gpu", EV_ERR },
    { "ring gfx timeout", "gpu", EV_ERR },
    { "NVRM: Xid", "gpu", EV_ERR },
    { "AER:", "pcie", EV_WARN },
    { "PCIe Bus Error", "pcie", EV_WARN },
    { "segfault at", "crash", EV_WARN },
    { "general protection", "crash", EV_WARN },
    { "trap invalid opcode", "crash", EV_WARN },
    { "soft lockup", "crash", EV_ERR },
    { "hard LOCKUP", "crash", EV_ERR },
    { "blocked for more than", "crash", EV_WARN },
    { "self-detected stall", "crash", EV_ERR },
    { "Oops", "crash", EV_ERR },
    { "BUG:", "crash", EV_ERR },
    { "Kernel panic", "crash", EV_ERR },
    { "under-voltage", "power", EV_ERR },
    { "Under-voltage", "power", EV_ERR },
    { "PM: suspend entry", "power", EV_INFO },
    { "PM: suspend exit", "power", EV_INFO },
};

static const char *guess_cat(const char *m)
{
    static const struct {
        const char *needle, *cat;
    } G[] = { { "nvme", "disk" },  { "ata", "disk" },    { "sd ", "disk" },    { "EXT4", "disk" },
              { "XFS", "disk" },   { "i915", "gpu" },    { "amdgpu", "gpu" },  { "nouveau", "gpu" },
              { "drm", "gpu" },    { "NVRM", "gpu" },    { "pcieport", "pcie" }, { "mce", "memory" },
              { "usb", "usb" },    { "thermal", "thermal" }, { "ACPI", "acpi" } };
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++)
        if (strstr(m, G[i].needle))
            return G[i].cat;
    return "kernel";
}

/* "prio,seq,usec,flags[,...];text" */
static void kmsg_record(health_t *h, char *rec, double boot_wall)
{
    char *semi = strchr(rec, ';');
    unsigned prio;
    unsigned long long seq, us;
    if (!semi || sscanf(rec, "%u,%llu,%llu", &prio, &seq, &us) != 3)
        return;
    if (prio >> 3) /* written by user space (systemd, ...), not by the kernel */
        return;
    int klevel = (int)(prio & 7);
    char *msg = semi + 1;
    msg[strcspn(msg, "\n")] = '\0';
    const char *cat = NULL;
    int level = -1;
    for (size_t i = 0; i < sizeof KMSG / sizeof KMSG[0]; i++)
        if (strstr(msg, KMSG[i].needle)) {
            cat = KMSG[i].cat;
            level = KMSG[i].level;
            break;
        }
    if (!cat) {
        if (klevel > 3) /* only errors and worse without a matching keyword */
            return;
        cat = guess_cat(msg);
        /* firmware tables with bugs: noisy at every boot, almost always harmless */
        level = !strncmp(msg, "ACPI", 4) || strstr(msg, "firmware") ? EV_INFO : EV_ERR;
    }
    if (level == EV_ERR)
        h->kmsg_err++;
    else if (level == EV_WARN)
        h->kmsg_warn++;
    double t = boot_wall + (double)us / 1e6;
    if (level == EV_ERR && t > h->kmsg_last)
        h->kmsg_last = t;
    char text[200];
    sfmt(text, sizeof text, "%s", msg);
    ev_add(h, t, level, cat, text);
}

static void kmsg_open(health_t *h)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/kmsg", g_devfs);
    h->kmsg_fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (h->kmsg_fd < 0) { /* dmesg_restrict=1 (Ubuntu, Debian, Fedora...): root only */
        h->kmsg = (errno == EPERM || errno == EACCES) ? -1 : 0;
        return;
    }
    struct stat st;
    h->kmsg_file = fstat(h->kmsg_fd, &st) == 0 && S_ISREG(st.st_mode); /* test hook */
    h->kmsg = 1;
}

static void kmsg_sample(health_t *h)
{
    if (h->kmsg != 1)
        return;
    /* message times are microseconds since boot (CLOCK_MONOTONIC) */
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    double boot_wall = wall_now() - ((double)mono.tv_sec + (double)mono.tv_nsec / 1e9);
    static char rec[8192];
    if (h->kmsg_file) { /* one record per line, appended to over time */
        ssize_t n = pread(h->kmsg_fd, rec, sizeof rec - 1, h->kmsg_off);
        if (n <= 0)
            return;
        rec[n] = '\0';
        char *cur = rec, *nl;
        while ((nl = strchr(cur, '\n')) != NULL) {
            *nl = '\0';
            h->kmsg_off += (long)(nl - cur) + 1;
            if (*cur != ' ')
                kmsg_record(h, cur, boot_wall);
            cur = nl + 1;
        }
        return;
    }
    for (int k = 0; k < 20000; k++) { /* the whole backlog on the first call */
        ssize_t n = read(h->kmsg_fd, rec, sizeof rec - 1);
        if (n < 0) {
            if (errno == EPIPE) /* messages were overwritten before we read them */
                continue;
            if (errno == EINTR)
                continue;
            break; /* EAGAIN: nothing more */
        }
        if (n == 0)
            break;
        rec[n] = '\0';
        kmsg_record(h, rec, boot_wall);
    }
}

/* ----------------------------------------------------------------- main */

void health_init(health_t *h)
{
    h->kmsg_fd = -1;
    kmsg_open(h);
    nvme_find(h);
}

void health_close(health_t *h)
{
    if (h->kmsg_fd >= 0)
        close(h->kmsg_fd);
    h->kmsg_fd = -1;
}

static void temp_events(health_t *h, sensors_t *S, double now)
{
    for (int i = 0; i < S->n; i++) {
        sensor_t *t = &S->s[i];
        if (t->type != SENS_TEMP || !t->ok)
            continue;
        double lim = t->crit > 0 ? t->crit : (i == S->cpu_idx ? 100.0 : -1);
        if (lim <= 0 || t->value < lim - 2)
            continue;
        h->hot_last = now;
        if (now - t->hot_t < 300)
            continue;
        t->hot_t = now;
        char chip[24];
        sensors_chip_name(t->chip, chip, sizeof chip);
        health_event(h, EV_ERR, "thermal", "%s %s reached %.0f °C (limit %.0f °C)", chip, t->label, t->value, lim);
    }
}

void health_sample(sys_t *s)
{
    health_t *h = &s->health;
    double dt = s->dt, now = wall_now(), mono = now_sec();

    static const char *const psi_name[3] = { "cpu", "memory", "io" };
    for (int i = 0; i < 3; i++)
        psi_sample(&h->psi[i], psi_name[i], dt);

    throttle_sample(h, &s->cpu, dt, now);

    if (s->mem.oom_ok) {
        if (!h->primed)
            h->oom_start = h->oom_prev = s->mem.oom_kill;
        if (s->mem.oom_kill > h->oom_prev) {
            unsigned long long k = s->mem.oom_kill - h->oom_prev;
            h->oom_last = now;
            health_event(h, EV_ERR, "memory", "Memory ran out: the kernel killed %llu process%s", k, k == 1 ? "" : "es");
        }
        h->oom_prev = s->mem.oom_kill;
    }

    kmsg_sample(h);
    temp_events(h, &s->sens, now);

    if (mono >= h->next_slow) { /* every 5 s */
        h->next_slow = mono + 5.0;
        aer_sample(h, now);
        char p[PATHBUF], b[128];
        sfmt(p, sizeof p, "%s/sys/fs/file-nr", g_procfs);
        unsigned long long a = 0, u = 0, m = 0;
        if (read_file(p, b, sizeof b) > 0 && sscanf(b, "%llu %llu %llu", &a, &u, &m) == 3) {
            h->files = a;
            h->files_max = m;
        }
        if (h->governor[0] && s->cpu.governor[0] && strcmp(h->governor, s->cpu.governor))
            health_event(h, EV_INFO, "cpu", "CPU governor changed from %s to %s", h->governor, s->cpu.governor);
        str_copy(h->governor, sizeof h->governor, s->cpu.governor);
    }
    if (h->nnvme && mono >= h->next_nvme) {
        h->next_nvme = mono + NVME_EVERY;
        nvme_sample(h);
    }
    h->primed = 1;
}
