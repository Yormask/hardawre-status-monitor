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

/* util.c - file reading, formatting and history helpers */
#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *g_sysfs = "/sys";
const char *g_procfs = "/proc";

ssize_t read_file(const char *path, char *buf, size_t size)
{
    if (!buf || size == 0)
        return -1;
    buf[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < size - 1) {
        ssize_t r = read(fd, buf + off, size - 1 - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            buf[0] = '\0';
            errno = e;
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    buf[off] = '\0';
    return (ssize_t)off;
}

ssize_t read_file_grow(const char *path, char **buf, size_t *cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (!*buf || *cap < 64) {
        free(*buf);
        *cap = 16384;
        *buf = malloc(*cap);
        if (!*buf) {
            close(fd);
            *cap = 0;
            return -1;
        }
    }
    size_t off = 0;
    for (;;) {
        if (off + 1 >= *cap) {
            size_t ncap = *cap * 2;
            char *nb = realloc(*buf, ncap);
            if (!nb)
                break;
            *buf = nb;
            *cap = ncap;
        }
        ssize_t r = read(fd, *buf + off, *cap - 1 - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    (*buf)[off] = '\0';
    return (ssize_t)off;
}

int read_line(const char *path, char *buf, size_t size)
{
    if (read_file(path, buf, size) < 0)
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

int read_ll(const char *path, long long *out)
{
    char b[64], *end;
    if (read_file(path, b, sizeof b) <= 0)
        return -1;
    long long v = strtoll(b, &end, 10);
    if (end == b)
        return -1;
    *out = v;
    return 0;
}

int read_ull(const char *path, unsigned long long *out)
{
    char b[64], *end;
    if (read_file(path, b, sizeof b) <= 0)
        return -1;
    unsigned long long v = strtoull(b, &end, 10);
    if (end == b)
        return -1;
    *out = v;
    return 0;
}

int read_hex(const char *path, unsigned *out)
{
    char b[32], *end;
    if (read_file(path, b, sizeof b) <= 0)
        return -1;
    unsigned long v = strtoul(b, &end, 16);
    if (end == b)
        return -1;
    *out = (unsigned)v;
    return 0;
}

int path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

char *next_line(char **cursor)
{
    char *s = *cursor;
    if (!s || !*s)
        return NULL;
    char *nl = strchr(s, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = s + strlen(s);
    }
    return s;
}

int sfmt(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}

void str_copy(char *dst, size_t n, const char *src)
{
    if (!n)
        return;
    size_t l = strnlen(src, n - 1);
    memcpy(dst, src, l);
    dst[l] = '\0';
}

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void fmt_bytes(double v, char *out, size_t n)
{
    static const char units[] = "BKMGTPE";
    int i = 0;
    if (v < 0) {
        snprintf(out, n, "-");
        return;
    }
    while (v >= 1000 && i < 6) { /* switch unit early so we never print 4 digits */
        v /= 1024;
        i++;
    }
    if (i == 0)
        snprintf(out, n, "%.0fB", v);
    else if (v < 10)
        snprintf(out, n, "%.2f%c", v, units[i]);
    else if (v < 100)
        snprintf(out, n, "%.1f%c", v, units[i]);
    else
        snprintf(out, n, "%.0f%c", v, units[i]);
}

void fmt_rate(double v, char *out, size_t n)
{
    char b[16];
    fmt_bytes(v, b, sizeof b);
    snprintf(out, n, "%s/s", b);
}

void fmt_si(double v, char *out, size_t n)
{
    if (v < 1000)
        snprintf(out, n, "%.0f", v);
    else if (v < 1e6)
        snprintf(out, n, "%.1fk", v / 1e3);
    else if (v < 1e9)
        snprintf(out, n, "%.1fM", v / 1e6);
    else
        snprintf(out, n, "%.1fG", v / 1e9);
}

void fmt_duration(double s, char *out, size_t n)
{
    if (s < 0) {
        snprintf(out, n, "-");
        return;
    }
    long t = (long)s;
    long d = t / 86400, h = (t / 3600) % 24, m = (t / 60) % 60, sec = t % 60;
    if (d)
        snprintf(out, n, "%ldd %02ldh", d, h);
    else if (h)
        snprintf(out, n, "%ldh %02ldm", h, m);
    else
        snprintf(out, n, "%ldm %02lds", m, sec);
}

int utf8_cols(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

size_t utf8_bytes_for_cols(const char *s, int cols)
{
    size_t i = 0;
    int c = 0;
    while (s[i]) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (c == cols)
                break;
            c++;
        }
        i++;
    }
    return i;
}

void hist_push(hist_t *h, double v)
{
    h->v[h->head] = (float)v;
    h->head = (h->head + 1) % HIST_LEN;
    if (h->count < HIST_LEN)
        h->count++;
}

double hist_get(const hist_t *h, int age)
{
    if (age < 0 || age >= h->count)
        return -1;
    int i = (h->head - 1 - age) % HIST_LEN;
    if (i < 0)
        i += HIST_LEN;
    return h->v[i];
}

double hist_max(const hist_t *h, int n)
{
    double m = 0;
    for (int a = 0; a < n && a < h->count; a++) {
        double v = hist_get(h, a);
        if (v > m)
            m = v;
    }
    return m;
}
