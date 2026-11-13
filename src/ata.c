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
/* ata.c - health of SATA drives (2.5" and 3.5" SSDs and hard disks).
 *
 * Drives keep SMART attributes about themselves: reallocated and pending
 * sectors, cable errors, wear, power-on hours... They are read with an ATA
 * PASS-THROUGH command (the same read-only query smartctl makes), which
 * needs root. A hard disk that has spun down is left asleep: hsm asks for
 * its power state first and only reads the attributes while it spins. */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ATA_EVERY 60.0 /* seconds between reads */

static unsigned long long le(const unsigned char *b, int n)
{
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

/* ------------------------------------------------------- ATA pass-through */

/* sense key of a SCSI sense buffer, -1 when there is none */
static int sense_key(const unsigned char *s, int len)
{
    if (len < 3)
        return -1;
    int rc = s[0] & 0x7f;
    if (rc == 0x72 || rc == 0x73)
        return s[1] & 0x0f;
    if (rc == 0x70 || rc == 0x71)
        return s[2] & 0x0f;
    return -1;
}

/* the ATA "count" register the drive returned (CHECK POWER MODE answers in it) */
static int sense_count(const unsigned char *s, int len)
{
    int rc = s[0] & 0x7f;
    if ((rc == 0x72 || rc == 0x73) && len > 8) { /* descriptor format: find the ATA status return */
        int total = MIN(len, 8 + s[7]);
        for (int i = 8; i + 1 < total; i += 2 + s[i + 1])
            if (s[i] == 0x09 && i + 13 < total)
                return s[i + 5];
    } else if ((rc == 0x70 || rc == 0x71) && len >= 7) { /* fixed format: in the information field */
        return s[6];
    }
    return -1;
}

/* One ATA command through SG_IO. buf != NULL reads one 512-byte sector.
 * Returns 0, or an errno (EOPNOTSUPP when the drive refused the command). */
static int ata_cmd_lba(int fd, unsigned char cmd, unsigned char feature, unsigned char lba_low, unsigned char *buf,
                       int *count_out)
{
    unsigned char cdb[16], sense[64];
    memset(cdb, 0, sizeof cdb);
    memset(sense, 0, sizeof sense);
    cdb[0] = 0x85; /* ATA PASS-THROUGH (16) */
    if (buf) {
        cdb[1] = 4 << 1;  /* protocol: PIO data-in */
        cdb[2] = 0x0e;    /* from the device, length in sectors, given in the count field */
    } else {
        cdb[1] = 3 << 1;  /* protocol: non-data */
        cdb[2] = 0x20;    /* CK_COND: return the registers, to read the answer */
    }
    cdb[4] = feature;
    cdb[6] = buf ? 1 : 0;
    cdb[8] = lba_low; /* SMART READ LOG: the log; EXECUTE OFF-LINE: the test */
    if (cmd == 0xB0) {    /* SMART: the "key" in LBA mid / high */
        cdb[10] = 0x4f;
        cdb[12] = 0xc2;
    }
    cdb[14] = cmd;

    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.dxfer_direction = buf ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
    io.dxfer_len = buf ? 512 : 0;
    io.dxferp = buf;
    io.timeout = 5000; /* ms */
    if (ioctl(fd, SG_IO, &io) < 0)
        return errno ? errno : EIO;
    if (io.host_status || (io.driver_status & 0x0f) == 0x06 /* DRIVER_TIMEOUT */)
        return EIO;
    int key = io.sb_len_wr ? sense_key(sense, io.sb_len_wr) : -1;
    if (count_out)
        *count_out = io.sb_len_wr ? sense_count(sense, io.sb_len_wr) : -1;
    if (io.status == 0)
        return 0;
    /* a "check condition" carrying the registers (or "recovered") is still a success */
    if (key == 0 || key == 1)
        return 0;
    return key == 5 ? EOPNOTSUPP : EIO; /* 5 = illegal request: not supported */
}

static int ata_cmd(int fd, unsigned char cmd, unsigned char feature, unsigned char *buf, int *count_out)
{
    return ata_cmd_lba(fd, cmd, feature, 0, buf, count_out);
}

/* a SMART command (selftest.c): feature D0 read data, D5 read log (lba_low =
 * the log), D4 run a self-test (lba_low = which); buf: one sector in */
int ata_smart(int fd, unsigned char feature, unsigned char lba_low, unsigned char *buf)
{
    return ata_cmd_lba(fd, 0xB0, feature, lba_low, buf, NULL);
}

/* ------------------------------------------------------------- discovery */

void ata_find(health_t *h)
{
    char base[PATHBUF];
    sfmt(base, sizeof base, "%s/block", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && h->nsata < MAX_SATA) {
        if (strncmp(e->d_name, "sd", 2) || !e->d_name[2] || strlen(e->d_name) > 6)
            continue;
        char p[PATHBUF], v[32];
        /* libata drives say "ATA" here; USB enclosures and RAID controllers don't */
        sfmt(p, sizeof p, "%s/%s/device/vendor", base, e->d_name);
        if (read_line(p, v, sizeof v) || strcmp(v, "ATA"))
            continue;
        long long rem = 0;
        sfmt(p, sizeof p, "%s/%s/removable", base, e->d_name);
        if (!read_ll(p, &rem) && rem)
            continue;
        sata_t *s = &h->sata[h->nsata++];
        memset(s, 0, sizeof *s);
        str_copy(s->name, sizeof s->name, e->d_name);
        sfmt(p, sizeof p, "%s/%s/device/model", base, e->d_name);
        if (read_line(p, s->model, sizeof s->model))
            s->model[0] = '\0';
        long long rot = 0;
        sfmt(p, sizeof p, "%s/%s/queue/rotational", base, e->d_name);
        s->rotational = !read_ll(p, &rot) && rot;
        s->temp_c = s->wear = -1;
        s->hours = s->cycles = s->realloc = s->pending = s->uncorrect = s->crc = s->unsafe = -1;
    }
    closedir(d);
}

/* ------------------------------------------------------------- attributes */

static const struct {
    int id;
    const char *name;
} ATTR[] = {
    { 1, "read errors" },          { 3, "spin-up time" },       { 5, "reallocated sectors" },
    { 7, "seek errors" },          { 10, "spin retries" },      { 11, "calibration retries" },
    { 170, "reserved blocks" },    { 171, "program failures" }, { 172, "erase failures" },
    { 177, "wear leveling" },      { 179, "reserved blocks" },  { 181, "program failures" },
    { 182, "erase failures" },     { 183, "SATA downshifts" },  { 184, "end-to-end errors" },
    { 187, "uncorrectable errors" }, { 196, "reallocation events" }, { 197, "pending sectors" },
    { 198, "uncorrectable sectors" }, { 200, "write errors" },  { 202, "lifetime left" },
    { 231, "life left" },          { 232, "reserved space" },   { 233, "wear" },
};

static void attr_name(int id, char *out, size_t n)
{
    for (size_t i = 0; i < sizeof ATTR / sizeof ATTR[0]; i++)
        if (ATTR[i].id == id) {
            str_copy(out, n, ATTR[i].name);
            return;
        }
    sfmt(out, n, "attribute %d", id);
}

static int checksum_ok(const unsigned char *b)
{
    unsigned char sum = 0;
    for (int i = 0; i < 512; i++)
        sum = (unsigned char)(sum + b[i]);
    return sum == 0;
}

/* SMART data (512 bytes) and thresholds (512 bytes) -> the drive's figures */
static void parse(sata_t *s, const unsigned char *data, const unsigned char *thr)
{
    long long seen[256];
    int val[256];
    for (int i = 0; i < 256; i++) {
        seen[i] = -1;
        val[i] = -1;
    }
    s->failing = 0;
    s->fail_attr[0] = '\0';
    for (int k = 0; k < 30; k++) {
        const unsigned char *a = data + 2 + k * 12;
        int id = a[0];
        if (!id)
            continue;
        int v = a[3];
        unsigned long long raw = le(a + 5, 6);
        seen[id] = (long long)(raw & 0xffffffffffffULL);
        val[id] = v;
        if (thr) { /* the maker's limit for the normalised value */
            for (int j = 0; j < 30; j++) {
                const unsigned char *t = thr + 2 + j * 12;
                if (t[0] == id && t[1] && t[1] != 0xfe && v && v <= t[1]) {
                    s->failing = 1;
                    attr_name(id, s->fail_attr, sizeof s->fail_attr);
                }
            }
        }
    }
#define RAW32(id) (seen[id] >= 0 ? seen[id] & 0xffffffffLL : -1)
    s->hours = RAW32(9);
    s->cycles = RAW32(12);
    s->realloc = RAW32(5);
    s->pending = RAW32(197);
    s->crc = RAW32(199);
    long long u1 = RAW32(187), u2 = RAW32(198);
    s->uncorrect = u1 < 0 && u2 < 0 ? -1 : MAX(u1, 0) + MAX(u2, 0);
    s->unsafe = seen[174] >= 0 ? RAW32(174) : RAW32(192);
#undef RAW32
    s->temp_c = -1;
    if (seen[194] >= 0 && (seen[194] & 0xff) > 0 && (seen[194] & 0xff) < 100)
        s->temp_c = (int)(seen[194] & 0xff);
    else if (seen[190] >= 0 && (seen[190] & 0xff) > 0 && (seen[190] & 0xff) < 100)
        s->temp_c = (int)(seen[190] & 0xff);
    s->wear = -1;
    if (!s->rotational) { /* life left, as a normalised value counting down from 100 */
        static const int wear_ids[] = { 177, 231, 202, 169, 233 };
        for (size_t i = 0; i < sizeof wear_ids / sizeof wear_ids[0] && s->wear < 0; i++) {
            int id = wear_ids[i], v = val[id];
            if (id == 233 && !strcasestr(s->model, "intel")) /* other makers use 233 for something else */
                continue;
            if (v >= 0 && v <= 100)
                s->wear = 100 - v;
        }
    }
}

/* power mode from CHECK POWER MODE: 1 spinning (or unknown), 0 asleep */
static int awake(int fd)
{
    int count = -1;
    if (ata_cmd(fd, 0xE5, 0, NULL, &count) != 0 || count < 0)
        return 1; /* can't tell: go ahead, as smartctl does */
    return !(count == 0x00 || count == 0x01 || count == 0x40 || count == 0x41);
}

static void read_drive(health_t *h, sata_t *s)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s", g_devfs, s->name);
    int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        s->state = (errno == EACCES || errno == EPERM) ? -1 : -2;
        return;
    }
    unsigned char data[512], thr[512];
    int ok = 0, have_thr = 0;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) { /* test hook: data + thresholds */
        ok = read(fd, data, 512) == 512;
        have_thr = ok && read(fd, thr, 512) == 512;
    } else if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
        if (s->rotational && !awake(fd)) {
            close(fd);
            if (s->state != 1)
                s->state = -3; /* asleep: don't wake it just to look */
            s->asleep = 1;
            return;
        }
        s->asleep = 0;
        int r = ata_cmd(fd, 0xB0, 0xD0, data, NULL); /* SMART READ DATA */
        if (r) {
            close(fd);
            s->state = (r == EACCES || r == EPERM) ? -1 : -2;
            return;
        }
        ok = 1;
        have_thr = ata_cmd(fd, 0xB0, 0xD1, thr, NULL) == 0 && checksum_ok(thr); /* READ THRESHOLDS */
    }
    close(fd);
    if (!ok || !checksum_ok(data)) {
        if (s->state != 1)
            s->state = -2;
        return;
    }
    int was = s->state, was_failing = s->failing;
    long long realloc = s->realloc, pending = s->pending, unc = s->uncorrect, crc = s->crc;
    parse(s, data, have_thr ? thr : NULL);
    s->state = 1;
    if (was != 1)
        return; /* first read: just the baseline */
    if (s->failing && !was_failing)
        health_event(h, EV_ERR, "disk", "%s: SMART says the drive is failing (%s): back up your data", s->name,
                     s->fail_attr);
    if (s->realloc > realloc && realloc >= 0)
        health_event(h, EV_ERR, "disk", "%s: %lld more reallocated sector%s (the drive is wearing out)", s->name,
                     s->realloc - realloc, s->realloc - realloc == 1 ? "" : "s");
    if (s->pending > pending && pending >= 0)
        health_event(h, EV_ERR, "disk", "%s: %lld new unreadable (pending) sector%s", s->name, s->pending - pending,
                     s->pending - pending == 1 ? "" : "s");
    if (s->uncorrect > unc && unc >= 0)
        health_event(h, EV_ERR, "disk", "%s: %lld new uncorrectable error%s", s->name, s->uncorrect - unc,
                     s->uncorrect - unc == 1 ? "" : "s");
    if (s->crc > crc && crc >= 0)
        health_event(h, EV_WARN, "disk", "%s: %lld new cable error%s (UDMA CRC): check the SATA cable", s->name,
                     s->crc - crc, s->crc - crc == 1 ? "" : "s");
}

void ata_sample(health_t *h)
{
    for (int i = 0; i < h->nsata; i++)
        read_drive(h, &h->sata[i]);
}

/* "wear 3% · 4120 h · 36 °C · no errors"; level 0 fine, 1 warning, 2 bad */
void sata_summary(const sata_t *s, char *out, size_t n, int *level, const char *sep, const char *deg)
{
    *level = 0;
    out[0] = '\0';
    switch (s->state) {
    case 0: str_copy(out, n, "reading..."); return;
    case -1: str_copy(out, n, "needs root (sudo hsm)"); return;
    case -2: str_copy(out, n, "no SMART data from this drive"); return;
    case -3: str_copy(out, n, "asleep (not woken up to check)"); return;
    }
    size_t l;
#define ADD(...) (l = strlen(out), sfmt(out + l, n - l, __VA_ARGS__))
    if (s->failing) {
        ADD("FAILING (%s)%s", s->fail_attr, sep);
        *level = 2;
    }
    if (s->realloc > 0 || s->pending > 0 || s->uncorrect > 0) {
        ADD("bad sectors: %lld reallocated, %lld pending%s", MAX(s->realloc, 0), MAX(s->pending, 0), sep);
        if (s->uncorrect > 0)
            ADD("%lld uncorrectable%s", s->uncorrect, sep);
        *level = 2;
    }
    if (s->crc > 0) {
        ADD("%lld cable errors%s", s->crc, sep);
        *level = MAX(*level, 1);
    }
    if (s->wear >= 0)
        ADD("wear %d%%%s", s->wear, sep);
    if (s->hours >= 0)
        ADD("%lld h%s", s->hours, sep);
    if (s->temp_c > 0)
        ADD("%d %s%s", s->temp_c, deg, sep);
    if (!*level)
        ADD("no errors%s", sep);
#undef ADD
    l = strlen(out); /* drop the separator after the last item */
    if (l >= strlen(sep))
        out[l - strlen(sep)] = '\0';
}
