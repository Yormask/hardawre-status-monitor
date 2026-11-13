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
/* selftest.c - a drive's own tests and error log.
 *
 * NVMe and SATA drives can test themselves: a short test (a minute or two)
 * checks the electronics and a sample of the surface, an extended one reads
 * everything (minutes on an SSD, hours on a big hard disk). The drive keeps
 * working normally meanwhile and keeps the last results in a log, next to a
 * log of the errors it reported. hsm reads both and starts tests on request
 * (root only, as smartctl -t). A test hsm started is followed until it
 * finishes, and the result becomes an event. */
#define _GNU_SOURCE
#include "collect.h"

#include <errno.h>
#include <fcntl.h>
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

#define SIM_SHORT 20.0 /* test hook: seconds a simulated short test takes */
#define SIM_LONG 60.0
#define FOLLOW_EVERY 30.0

static drive_t D[MAX_NVME + MAX_SATA];
static int ND;

static unsigned long long le(const unsigned char *b, int n)
{
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int drives_sync(const health_t *h)
{
    drive_t old[MAX_NVME + MAX_SATA];
    int nold = ND;
    memcpy(old, D, sizeof D);
    ND = 0;
    for (int i = 0; i < h->nnvme + h->nsata && ND < MAX_NVME + MAX_SATA; i++) {
        int nv = i < h->nnvme;
        const char *name = nv ? h->nvme[i].name : h->sata[i - h->nnvme].name;
        const char *model = nv ? h->nvme[i].model : h->sata[i - h->nnvme].model;
        drive_t *d = &D[ND++];
        int k = 0;
        while (k < nold && strcmp(old[k].name, name))
            k++;
        if (k < nold) { /* keep what is known, and a test being followed */
            *d = old[k];
            continue;
        }
        memset(d, 0, sizeof *d);
        str_copy(d->name, sizeof d->name, name);
        str_copy(d->model, sizeof d->model, model);
        d->nvme = nv;
        d->errors = d->err_hours = d->hours = -1;
        d->short_min = d->ext_min = -1;
        d->progress = -1;
    }
    return ND;
}

drive_t *drive_at(int i)
{
    return i >= 0 && i < ND ? &D[i] : NULL;
}

static int open_dev(drive_t *d, int *test)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s", g_devfs, d->name);
    int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        d->state = errno == EACCES || errno == EPERM ? -1 : -2;
        return -1;
    }
    struct stat st;
    *test = fstat(fd, &st) == 0 && S_ISREG(st.st_mode); /* test hook: a file with the logs */
    return fd;
}

static int pread_all(int fd, void *buf, size_t n, off_t at)
{
    return pread(fd, buf, n, at) == (ssize_t)n ? 0 : -1;
}

/* ------------------------------------------------------------------ NVMe */

#ifdef HAVE_NVME_IOCTL
static int nvme_admin(int fd, unsigned opcode, unsigned nsid, unsigned cdw10, void *buf, unsigned len)
{
    struct nvme_admin_cmd c;
    memset(&c, 0, sizeof c);
    c.opcode = (unsigned char)opcode;
    c.nsid = nsid;
    c.addr = (uint64_t)(uintptr_t)buf;
    c.data_len = len;
    c.cdw10 = cdw10;
    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c) == 0 ? 0 : (errno ? errno : EIO);
}

static int nvme_log(int fd, unsigned lid, void *buf, unsigned len)
{
    return nvme_admin(fd, 0x02, 0xffffffffu, ((len / 4 - 1) << 16) | lid, buf, len);
}
#endif

static const char *nvme_result(int r)
{
    static const char *const R[] = { "completed without error",
                                     "stopped (asked to)",
                                     "stopped by a controller reset",
                                     "stopped: a namespace was removed",
                                     "stopped by a format",
                                     "FAILED: fatal or unknown test error",
                                     "FAILED: a segment failed",
                                     "FAILED: one or more segments failed",
                                     "stopped for an unknown reason",
                                     "stopped by a sanitize" };
    return r >= 0 && r < 10 ? R[r] : "?";
}

/* "invalid field in a command": most errors in the log are tools asking
 * for things the drive doesn't do; media errors are the ones that count */
static void nvme_status_text(unsigned sf, char *out, size_t n, int *bad)
{
    unsigned sc = (sf >> 1) & 0xff, sct = (sf >> 9) & 7;
    *bad = 0;
    if (sct == 2) {
        static const struct {
            unsigned sc;
            const char *t;
        } M[] = { { 0x80, "write fault" },          { 0x81, "unrecovered read error" },
                  { 0x82, "end-to-end guard check error" }, { 0x85, "compare failure" },
                  { 0x86, "access denied" },        { 0x87, "unwritten block" } };
        const char *t = "media error";
        for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
            if (M[i].sc == sc)
                t = M[i].t;
        sfmt(out, n, "%s", t);
        *bad = sc != 0x87;
    } else if (sct == 0) {
        static const struct {
            unsigned sc;
            const char *t;
        } G[] = { { 0x01, "unknown command" }, { 0x02, "invalid field in a command" }, { 0x04, "data transfer error" },
                  { 0x06, "internal error" },  { 0x07, "command aborted on request" }, { 0x0b, "invalid namespace" } };
        const char *t = NULL;
        for (size_t i = 0; i < sizeof G / sizeof G[0]; i++)
            if (G[i].sc == sc)
                t = G[i].t;
        if (t)
            sfmt(out, n, "%s", t);
        else
            sfmt(out, n, "status %02x", sc);
        *bad = sc == 0x04 || sc == 0x06;
        size_t l = strlen(out);
        if (!*bad)
            sfmt(out + l, n - l, "%s", " (a refused command, usually harmless)");
    } else {
        sfmt(out, n, "command-specific status %u/%02x (usually harmless)", sct, sc);
    }
}

static void read_nvme(drive_t *d)
{
    int test;
    int fd = open_dev(d, &test);
    if (fd < 0)
        return;
    unsigned char smart[512], st[564], el[1024];
    int have_st = 0, have_el = 0, have_smart = 0;
    if (test) {
        have_smart = pread_all(fd, smart, 512, 0) == 0;
        have_st = pread_all(fd, st, sizeof st, 512) == 0;
        have_el = pread_all(fd, el, sizeof el, 512 + 564) == 0;
    } else {
#ifdef HAVE_NVME_IOCTL
        int r = nvme_log(fd, 0x02, smart, 512);
        if (r == EACCES || r == EPERM) {
            close(fd);
            d->state = -1;
            return;
        }
        have_smart = r == 0;
        have_st = nvme_log(fd, 0x06, st, sizeof st) == 0;
        have_el = nvme_log(fd, 0x01, el, sizeof el) == 0;
        if (d->ext_min < 0) { /* Identify Controller: how long an extended test takes */
            unsigned char id[4096];
            if (nvme_admin(fd, 0x06, 0, 1, id, sizeof id) == 0) {
                unsigned edstt = (unsigned)le(id + 316, 2);
                d->ext_min = edstt ? (int)edstt : -1;
                d->short_min = 2; /* at most, by the spec */
            }
        }
#endif
    }
    close(fd);
    if (!have_smart && !have_st) {
        d->state = -2;
        return;
    }
    d->state = 1;
    if (have_smart) {
        d->hours = (long long)le(smart + 128, 8);
        d->errors = (long long)le(smart + 176, 8);
    }
    d->ntest = 0;
    d->running = 0;
    d->progress = -1;
    d->can_test = have_st;
    if (have_st) {
        int op = st[0] & 0xf;
        if (op == 1 || op == 2) {
            d->running = op;
            d->progress = st[1] & 0x7f;
        }
        for (int i = 0; i < 20 && d->ntest < MAX_STEST; i++) {
            const unsigned char *e = st + 4 + i * 28;
            int res = e[0] & 0xf, code = e[0] >> 4;
            if (res == 0xf)
                continue; /* unused */
            stest_t *t = &d->test[d->ntest++];
            t->type = code == 2 ? 2 : code == 1 ? 1 : 3;
            t->failed = res >= 5 && res <= 7;
            t->stopped = !t->failed && res != 0;
            str_copy(t->what, sizeof t->what, nvme_result(res));
            t->hours = (long long)le(e + 4, 8);
            t->lba = e[2] & 2 ? (long long)le(e + 16, 8) : -1;
        }
    }
    d->err_text[0] = '\0';
    d->err_bad = 0;
    d->err_hours = -1;
    if (have_el) { /* the newest entry: the highest error count */
        unsigned long long best = 0;
        for (int i = 0; i < 16; i++) {
            const unsigned char *e = el + i * 64;
            unsigned long long cnt = le(e, 8);
            if (cnt > best) {
                best = cnt;
                nvme_status_text((unsigned)le(e + 12, 2), d->err_text, sizeof d->err_text, &d->err_bad);
            }
        }
    }
    if (d->sim_until > 0) { /* test hook: the simulated test */
        double now = wall();
        if (now < d->sim_until) {
            d->running = d->sim_type;
            d->progress = (int)(100.0 * (1.0 - (d->sim_until - now) / (d->sim_type == 1 ? SIM_SHORT : SIM_LONG)));
        }
    }
}

/* ------------------------------------------------------------------ SATA */

static const char *ata_status(int s)
{
    static const char *const R[] = { "completed without error",
                                     "stopped (asked to)",
                                     "stopped by a reset",
                                     "FAILED: fatal error",
                                     "FAILED: unknown test element",
                                     "FAILED: electrical element",
                                     "FAILED: servo / seek element",
                                     "FAILED: read element",
                                     "FAILED: handling damage" };
    return s >= 0 && s < 9 ? R[s] : s == 15 ? "running" : "?";
}

static void ata_error_text(const unsigned char *eds, char *out, size_t n, int *bad)
{
    unsigned er = eds[1];
    long long lba = (long long)eds[3] | (long long)eds[4] << 8 | (long long)eds[5] << 16 | (long long)(eds[6] & 0xf) << 24;
    *bad = 0;
    if (er & 0x40) {
        sfmt(out, n, "uncorrectable read error at sector %lld", lba);
        *bad = 1;
    } else if (er & 0x80) {
        sfmt(out, n, "%s", "transfer error on the cable (interface CRC)");
    } else if (er & 0x10) {
        sfmt(out, n, "sector %lld not found", lba);
        *bad = 1;
    } else if (er & 0x04) {
        sfmt(out, n, "%s", "command aborted (often harmless)");
    } else {
        sfmt(out, n, "error register %02x", er);
    }
}

static int csum_ok(const unsigned char *b)
{
    unsigned char s = 0;
    for (int i = 0; i < 512; i++)
        s = (unsigned char)(s + b[i]);
    return s == 0;
}

static void read_sata(drive_t *d)
{
    int test;
    int fd = open_dev(d, &test);
    if (fd < 0)
        return;
    unsigned char data[512], st[512], el[512];
    int have_data = 0, have_st = 0, have_el = 0;
    if (test) {
        have_data = pread_all(fd, data, 512, 0) == 0;
        have_st = pread_all(fd, st, 512, 1024) == 0;
        have_el = pread_all(fd, el, 512, 1536) == 0;
    } else {
        int r = ata_smart(fd, 0xD0, 0, data);
        if (r == EACCES || r == EPERM) {
            close(fd);
            d->state = -1;
            return;
        }
        have_data = r == 0;
        have_st = have_data && ata_smart(fd, 0xD5, 0x06, st) == 0;
        have_el = have_data && ata_smart(fd, 0xD5, 0x01, el) == 0;
    }
    close(fd);
    if (!have_data || !csum_ok(data)) {
        d->state = -2;
        return;
    }
    d->state = 1;
    for (int k = 0; k < 30; k++) /* power-on hours: attribute 9 */
        if (data[2 + k * 12] == 9)
            d->hours = (long long)le(data + 2 + k * 12 + 5, 4);
    /* what the drive says about its tests: the one running, how long they take */
    unsigned sts = data[363];
    d->running = 0;
    d->progress = -1;
    if (sts >> 4 == 15) {
        d->running = 1; /* which one isn't said here; the log tells */
        d->progress = 100 - 10 * (int)(sts & 0xf);
    }
    d->short_min = data[372] ? data[372] : -1;
    d->ext_min = data[373] == 0xff ? (int)le(data + 375, 2) : data[373] ? data[373] : -1;
    d->can_test = (data[367] & 0x10) != 0 || have_st; /* offline capability: self-test supported */
    d->ntest = 0;
    if (have_st && csum_ok(st)) {
        int idx = st[508]; /* the newest entry, 1-based; the log goes round */
        for (int k = 0; k < 21 && idx > 0 && d->ntest < MAX_STEST; k++) {
            int j = ((idx - 1 - k) % 21 + 21) % 21;
            const unsigned char *e = st + 2 + j * 24;
            if (!e[0] && !e[1] && !e[2] && !e[3])
                break;
            stest_t *t = &d->test[d->ntest++];
            int type = e[0] & 0x7f; /* 1 short, 2 extended, 3 conveyance; +128 "captive" */
            t->type = type == 1 ? 1 : type == 2 ? 2 : 3;
            int s = e[1] >> 4;
            t->failed = s >= 3 && s <= 8;
            t->stopped = s == 1 || s == 2;
            str_copy(t->what, sizeof t->what, ata_status(s));
            t->hours = (long long)le(e + 2, 2);
            t->lba = t->failed ? (long long)le(e + 5, 4) : -1;
            if (k == 0 && d->running && s == 15)
                d->running = t->type;
        }
    }
    d->errors = -1;
    d->err_text[0] = '\0';
    d->err_bad = 0;
    d->err_hours = -1;
    if (have_el && csum_ok(el)) {
        d->errors = (long long)le(el + 452, 2);
        int idx = el[1];
        if (idx >= 1 && idx <= 5) {
            const unsigned char *eds = el + 2 + (idx - 1) * 90 + 60;
            ata_error_text(eds, d->err_text, sizeof d->err_text, &d->err_bad);
            d->err_hours = (long long)le(eds + 28, 2);
        }
    }
    if (d->sim_until > 0) {
        double now = wall();
        if (now < d->sim_until) {
            d->running = d->sim_type;
            d->progress = (int)(100.0 * (1.0 - (d->sim_until - now) / (d->sim_type == 1 ? SIM_SHORT : SIM_LONG)));
        }
    }
}

int drive_read(drive_t *d)
{
    if (d->nvme)
        read_nvme(d);
    else
        read_sata(d);
    /* the simulated test's result, newest first */
    if (d->sim_until > 0 && wall() >= d->sim_until && d->ntest < MAX_STEST) {
        memmove(&d->test[1], &d->test[0], (size_t)d->ntest * sizeof d->test[0]);
        stest_t *t = &d->test[0];
        memset(t, 0, sizeof *t);
        t->type = d->sim_type;
        t->hours = d->hours;
        t->lba = -1;
        str_copy(t->what, sizeof t->what, "completed without error");
        d->ntest++;
    }
    d->read_t = wall();
    return d->state == 1 ? 0 : -1;
}

/* 1 short, 2 extended, 0 stop the running one */
int drive_start(drive_t *d, int type)
{
    int test;
    int fd = open_dev(d, &test);
    if (fd < 0)
        return errno ? errno : EIO;
    int r = 0;
    if (test) { /* test hook: pretend */
        d->sim_type = type;
        d->sim_until = type ? wall() + (type == 1 ? SIM_SHORT : SIM_LONG) : 0;
    } else if (d->nvme) {
#ifdef HAVE_NVME_IOCTL
        r = nvme_admin(fd, 0x14, 0xffffffffu, type == 0 ? 0xf : (unsigned)type, NULL, 0);
#else
        r = EOPNOTSUPP;
#endif
    } else {
        r = ata_smart(fd, 0xD4, type == 0 ? 127 : type == 1 ? 1 : 2, NULL);
    }
    close(fd);
    if (r == 0 && type) {
        d->follow = type; /* an event when it is done */
        d->follow_t = wall() - FOLLOW_EVERY + 5; /* a first look soon: a short test can be quick */
    } else if (r == 0) {
        d->follow = 0;
    }
    return r;
}

/* the tests hsm started: done yet? */
void drives_tick(health_t *h)
{
    double now = wall();
    for (int i = 0; i < ND; i++) {
        drive_t *d = &D[i];
        if (!d->follow || now - d->follow_t < FOLLOW_EVERY)
            continue;
        d->follow_t = now;
        drive_read(d);
        if (d->running || d->state != 1)
            continue;
        d->follow = 0;
        if (!d->ntest)
            continue;
        const stest_t *t = &d->test[0];
        char lba[48] = "";
        if (t->lba >= 0)
            sfmt(lba, sizeof lba, " (first bad sector: %lld)", t->lba);
        health_event(h, t->failed ? EV_ERR : t->stopped ? EV_WARN : EV_INFO, "disk", "%s: %s self-test %s%s%s",
                     d->name, t->type == 2 ? "extended" : t->type == 1 ? "short" : "conveyance", t->what, lba,
                     t->failed ? ": back up your data" : "");
    }
}
