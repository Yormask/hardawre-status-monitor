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

/* netcheck.c - the network's quality: delay and lost pings, and DNS
 *
 * Off unless switched on (Setup, or net_check in the config file). A small
 * thread pings the router and one internet address every 2 seconds, with
 * the kernel's ping sockets, which most distributions allow every user
 * (net.ipv4.ping_group_range); as root it can fall back to a raw socket.
 * Comparing the two tells a Wi-Fi or cable problem (the router itself
 * answers late or not at all) from one further away (only the internet
 * does), and the delays explain lag spikes in games. Every 10 seconds it
 * also asks the DNS server for example.com: "pings work but pages don't
 * load" is often a DNS server that answers slowly or not at all. The
 * server is the first one systemd-resolved uses, else the first in
 * /etc/resolv.conf. */
#define _GNU_SOURCE
#include "collect.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EVERY 2.0        /* s between pings; an answer later than that counts as lost */
#define WINDOW 30        /* pings remembered: the last minute */
#define BAD_FOR 2        /* pings in a row with trouble before it counts */
#define GOOD_FOR 5       /* ... and without, before it's over */
#define DEFAULT_HOST "1.1.1.1"
#define DNS_EVERY 10.0   /* s between DNS questions */

typedef struct {
    struct sockaddr_in to;
    double every;        /* s between questions */
    int have;            /* an address to ping */
    double ms[WINDOW];   /* ring: the delay of each ping, -1 = lost */
    int n, head;
    uint16_t seq;        /* the ping waiting for its answer */
    double sent;         /* when it went (monotonic), 0 = nothing waiting */
    int bad_run, good_run;
    double last_bad;     /* the latest ping lost or far too slow (wall) */
} tgt_t;

static struct {
    pthread_mutex_t mu;
    pthread_t thr;
    int running;
    int wake[2];         /* a pipe that wakes the thread to stop */
    char host[128];
    netq_t q;            /* what the thread found: under mu */
} N = { .mu = PTHREAD_MUTEX_INITIALIZER, .wake = { -1, -1 } };

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint16_t csum(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1)
        s += (uint32_t)p[len - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return htons((uint16_t)~s);
}

/* the default route's gateway and interface (lowest metric), from /proc/net/route */
static int router(struct in_addr *gw, char *iface, size_t n)
{
    char p[PATHBUF], line[256];
    snprintf(p, sizeof p, "%s/net/route", g_procfs);
    FILE *f = fopen(p, "re");
    if (!f)
        return -1;
    int found = 0;
    unsigned long best = ~0UL;
    while (fgets(line, sizeof line, f)) {
        char ifc[32];
        unsigned long dst, g, flags, refc, use, metric, mask;
        if (sscanf(line, "%31s %lx %lx %lx %lu %lu %lu %lx", ifc, &dst, &g, &flags, &refc, &use, &metric, &mask) != 8)
            continue;
        if (dst || mask || (flags & 3) != 3 || metric >= best) /* up, through a gateway, the default */
            continue;
        best = metric;
        gw->s_addr = (in_addr_t)g; /* the file holds it as the kernel keeps it */
        snprintf(iface, n, "%s", ifc);
        found = 1;
    }
    fclose(f);
    return found ? 0 : -1;
}

static int resolve(const char *host, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1)
        return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static void push(tgt_t *t, double ms)
{
    t->ms[t->head] = ms;
    t->head = (t->head + 1) % WINDOW;
    if (t->n < WINDOW)
        t->n++;
}

static double at(const tgt_t *t, int k) /* k = 0: the latest */
{
    return t->ms[(t->head - 1 - k + 2 * WINDOW) % WINDOW];
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* the figures of one target, and whether it is in trouble (called after every ping) */
static void judge(tgt_t *t, nqt_t *q)
{
    q->rtt_ms = t->n ? at(t, 0) : -1;
    double sum = 0, mn = 1e9, mx = 0, jit = 0, prev = -1;
    int got = 0, jn = 0;
    for (int k = t->n - 1; k >= 0; k--) { /* oldest first */
        double v = at(t, k);
        if (v < 0)
            continue;
        sum += v;
        mn = MIN(mn, v);
        mx = MAX(mx, v);
        if (prev >= 0) {
            jit += fabs(v - prev);
            jn++;
        }
        prev = v;
        got++;
    }
    q->avg_ms = got ? sum / got : -1;
    q->min_ms = got ? mn : -1;
    q->max_ms = got ? mx : -1;
    q->jitter_ms = jn ? jit / jn : -1;
    q->loss_pct = t->n ? 100.0 * (t->n - got) / t->n : -1;

    /* trouble: 3 of the last 10 lost, or the last few answers far slower than usual */
    int last = MIN(t->n, 10), lost = 0, oldest_bad = 0;
    double rec[5];
    int nr = 0;
    for (int k = 0; k < last; k++) {
        double v = at(t, k);
        if (v < 0 || (v > 100 && q->normal_ms > 0 && v > 3 * q->normal_ms))
            oldest_bad = k; /* when this trouble began: about k pings ago */
        if (v < 0)
            lost++;
        else if (nr < 5)
            rec[nr++] = v;
    }
    double med = -1;
    if (nr >= 3) {
        qsort(rec, (size_t)nr, sizeof rec[0], cmp_d);
        med = rec[nr / 2];
    }
    int now = lost >= 3                                                                  ? NQ_LOSS
            : med > 0 && med > 100 && (q->normal_ms <= 0 || med > 3 * q->normal_ms) ? NQ_SLOW
                                                                                         : NQ_FINE;
    double latest = t->n ? at(t, 0) : 0;
    if (latest < 0 || (latest > 100 && q->normal_ms > 0 && latest > 3 * q->normal_ms))
        t->last_bad = wall();
    if (now == NQ_FINE && q->rtt_ms >= 0 && !q->trouble) /* what usual looks like */
        q->normal_ms = q->normal_ms <= 0 ? q->rtt_ms : q->normal_ms * 0.95 + q->rtt_ms * 0.05;
    if (now) {
        t->good_run = 0;
        if (++t->bad_run >= BAD_FOR && !q->trouble) {
            q->trouble = now;
            q->episode++;
            q->since = wall() - MAX(oldest_bad, BAD_FOR - 1) * t->every;
            q->until = 0;
            q->worst_loss = q->worst_ms = 0;
        }
    } else {
        t->bad_run = 0;
        if (q->trouble && ++t->good_run >= GOOD_FOR) { /* over: since the last bad ping */
            q->trouble = NQ_FINE;
            q->until = t->last_bad > q->since ? t->last_bad + t->every : wall();
        }
    }
    if (q->trouble) {
        if (now)
            q->trouble = now; /* what it is now: loss can turn into slowness and back */
        q->worst_loss = MAX(q->worst_loss, 100.0 * lost / MAX(last, 1));
        q->worst_ms = MAX(q->worst_ms, med);
    }
}

/* ------------------------------------------------------------- DNS */

/* the DNS server this machine asks: systemd-resolved's first upstream one
 * (its own 127.0.0.53 would answer from its cache), else resolv.conf's */
static int dns_server(struct sockaddr_in *out)
{
    const char *files[] = { "/run/systemd/resolve/resolv.conf", "/etc/resolv.conf" };
    const char *fake = secure_getenv("HSM_RESOLV"); /* test hook: another resolv.conf */
    if (fake && *fake)
        files[0] = files[1] = fake;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "re");
        char line[256], ip[64];
        while (f && fgets(line, sizeof line, f)) {
            if (sscanf(line, " nameserver %63s", ip) != 1)
                continue;
            memset(out, 0, sizeof *out);
            out->sin_family = AF_INET;
            out->sin_port = htons(53);
            if (inet_pton(AF_INET, ip, &out->sin_addr) == 1) {
                fclose(f);
                return 0;
            }
        }
        if (f)
            fclose(f);
    }
    return -1;
}

/* "example.com, address?" with this ID */
static int send_dns(int fd, uint16_t qid)
{
    static const unsigned char name[] = "\7example\3com";
    unsigned char b[64];
    size_t n = 0;
    b[n++] = (unsigned char)(qid >> 8);
    b[n++] = (unsigned char)qid;
    b[n++] = 0x01; /* recursion desired */
    b[n++] = 0x00;
    b[n++] = 0; /* one question */
    b[n++] = 1;
    memset(b + n, 0, 6);
    n += 6;
    memcpy(b + n, name, sizeof name); /* with the closing 0 */
    n += sizeof name;
    b[n++] = 0; /* type A */
    b[n++] = 1;
    b[n++] = 0; /* class IN */
    b[n++] = 1;
    return send(fd, b, n, 0) < 0 ? errno : 0;
}

/* an answer: 1 good, 0 the server said it failed (SERVFAIL, REFUSED), -1 not ours */
static int read_dns(int fd, uint16_t qid)
{
    unsigned char b[512];
    ssize_t r = recv(fd, b, sizeof b, 0);
    if (r < 12 || (uint16_t)(b[0] << 8 | b[1]) != qid || !(b[2] & 0x80))
        return -1;
    int rcode = b[3] & 0x0f;
    return rcode == 0 || rcode == 3 ? 1 : 0; /* no such name is still an answer */
}

static int send_ping(int fd, int raw, uint16_t id, tgt_t *t, uint16_t seq)
{
    unsigned char buf[sizeof(struct icmphdr) + 16];
    memset(buf, 0, sizeof buf);
    struct icmphdr *h = (struct icmphdr *)buf;
    h->type = ICMP_ECHO;
    h->un.echo.id = htons(id); /* a ping socket puts its own in */
    h->un.echo.sequence = htons(seq);
    memcpy(buf + sizeof *h, "hsm network check", 16);
    h->checksum = csum(buf, sizeof buf); /* a ping socket works it out again */
    (void)raw;
    return sendto(fd, buf, sizeof buf, 0, (const struct sockaddr *)&t->to, sizeof t->to) < 0 ? errno : 0;
}

static void read_answers(int fd, int raw, uint16_t id, tgt_t *T, nqt_t *Q)
{
    unsigned char buf[1500];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0)
            return;
        const unsigned char *p = buf;
        size_t len = (size_t)r;
        if (raw) { /* the IP header comes first */
            size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
            if (len < ihl + sizeof(struct icmphdr))
                continue;
            p += ihl;
            len -= ihl;
        }
        if (len < sizeof(struct icmphdr))
            continue;
        const struct icmphdr *h = (const struct icmphdr *)p;
        if (h->type != ICMP_ECHOREPLY || (raw && ntohs(h->un.echo.id) != id))
            continue;
        uint16_t seq = ntohs(h->un.echo.sequence);
        for (int k = 0; k < NQ_COUNT; k++) {
            tgt_t *t = &T[k];
            if (k != NQ_DNS && t->sent > 0 && t->seq == seq && t->to.sin_addr.s_addr == from.sin_addr.s_addr) {
                push(t, (mono() - t->sent) * 1000.0);
                t->sent = 0;
                pthread_mutex_lock(&N.mu);
                judge(t, &Q[k]);
                N.q.t[k] = Q[k];
                pthread_mutex_unlock(&N.mu);
            }
        }
    }
}

static void *pinger(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL); /* every signal goes to the main thread */

    int raw = 0;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        int e = errno;
        fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP); /* root */
        if (fd >= 0)
            raw = 1;
        else
            errno = e;
    }
    if (fd < 0) {
        pthread_mutex_lock(&N.mu);
        N.q.err = errno;
        pthread_mutex_unlock(&N.mu);
        struct pollfd pf = { N.wake[0], POLLIN, 0 };
        poll(&pf, 1, -1); /* until it's switched off */
        return NULL;
    }
    uint16_t id = (uint16_t)(getpid() & 0xffff), seq = 0, qid = (uint16_t)(mono() * 1000);
    tgt_t T[NQ_COUNT];
    nqt_t Q[NQ_COUNT];
    memset(T, 0, sizeof T);
    memset(Q, 0, sizeof Q);
    int dfd = -1;
    double next_dns = 0, next_dsrv = 0;
    for (int k = 0; k < NQ_COUNT; k++)
        Q[k].rtt_ms = Q[k].avg_ms = Q[k].min_ms = Q[k].max_ms = Q[k].jitter_ms = Q[k].loss_pct = Q[k].normal_ms = -1;
    char iface[32] = "";
    double next_route = 0, next_host = 0, next = mono();
    for (;;) {
        double now = mono();
        if (now >= next_route) { /* the router: it changes with the network */
            next_route = now + 10;
            struct in_addr gw;
            char ifc[32] = "";
            int have = router(&gw, ifc, sizeof ifc) == 0;
            if (!have || !T[NQ_ROUTER].have || gw.s_addr != T[NQ_ROUTER].to.sin_addr.s_addr) {
                /* another network, or none: start over (a trouble on the old one is over) */
                nqt_t *q = &Q[NQ_ROUTER];
                memset(&T[NQ_ROUTER], 0, sizeof T[NQ_ROUTER]);
                if (q->trouble) {
                    q->trouble = NQ_FINE;
                    q->until = wall();
                }
                q->addr[0] = '\0';
                q->noroute = 0;
                q->rtt_ms = q->avg_ms = q->min_ms = q->max_ms = q->jitter_ms = q->loss_pct = q->normal_ms = -1;
                if (have) {
                    T[NQ_ROUTER].have = 1;
                    T[NQ_ROUTER].to.sin_family = AF_INET;
                    T[NQ_ROUTER].to.sin_addr = gw;
                    inet_ntop(AF_INET, &gw, Q[NQ_ROUTER].addr, sizeof Q[NQ_ROUTER].addr);
                }
            }
            snprintf(iface, sizeof iface, "%s", ifc);
        }
        if (now >= next_dsrv) { /* the DNS server: it changes with the network too */
            next_dsrv = now + 30;
            struct sockaddr_in ds;
            int have = dns_server(&ds) == 0;
            if (!have || !T[NQ_DNS].have || ds.sin_addr.s_addr != T[NQ_DNS].to.sin_addr.s_addr) {
                nqt_t *q = &Q[NQ_DNS];
                memset(&T[NQ_DNS], 0, sizeof T[NQ_DNS]);
                if (q->trouble) {
                    q->trouble = NQ_FINE;
                    q->until = wall();
                }
                q->addr[0] = '\0';
                q->rtt_ms = q->avg_ms = q->min_ms = q->max_ms = q->jitter_ms = q->loss_pct = q->normal_ms = -1;
                if (dfd >= 0)
                    close(dfd);
                dfd = -1;
                if (have && (dfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) >= 0 &&
                    connect(dfd, (struct sockaddr *)&ds, sizeof ds) == 0) {
                    T[NQ_DNS].have = 1;
                    T[NQ_DNS].to = ds;
                    inet_ntop(AF_INET, &ds.sin_addr, q->addr, sizeof q->addr);
                } else if (dfd >= 0) {
                    close(dfd);
                    dfd = -1;
                }
            }
            T[NQ_DNS].every = DNS_EVERY;
        }
        if (now >= next_dns && T[NQ_DNS].have) {
            next_dns = MAX(next_dns + DNS_EVERY, now);
            tgt_t *t = &T[NQ_DNS];
            if (t->sent > 0) { /* no answer in 10 s: lost */
                push(t, -1);
                judge(t, &Q[NQ_DNS]);
            }
            qid++;
            int e = send_dns(dfd, qid);
            Q[NQ_DNS].noroute = e == ENETUNREACH || e == EHOSTUNREACH;
            t->sent = e ? 0 : mono();
            t->seq = qid;
        }
        T[NQ_ROUTER].every = T[NQ_NET].every = EVERY;
        if (!T[NQ_NET].have && now >= next_host) { /* the internet address, looked up again if it failed */
            next_host = now + 60;
            if (resolve(N.host, &T[NQ_NET].to) == 0) {
                T[NQ_NET].have = 1;
                inet_ntop(AF_INET, &T[NQ_NET].to.sin_addr, Q[NQ_NET].addr, sizeof Q[NQ_NET].addr);
            }
        }
        if (now >= next) {
            next = MAX(next + EVERY, now);
            seq++;
            for (int k = 0; k < NQ_COUNT; k++) {
                tgt_t *t = &T[k];
                if (!t->have || k == NQ_DNS)
                    continue;
                if (t->sent > 0) { /* no answer in time */
                    push(t, -1);
                    judge(t, &Q[k]);
                }
                int e = send_ping(fd, raw, id, t, seq);
                Q[k].noroute = e == ENETUNREACH || e == EHOSTUNREACH;
                if (!e) {
                    t->seq = seq;
                    t->sent = mono();
                } else {
                    t->sent = 0; /* nowhere to send it: not a lost ping */
                }
            }
            pthread_mutex_lock(&N.mu);
            memcpy(N.q.t, Q, sizeof Q);
            snprintf(N.q.iface, sizeof N.q.iface, "%s", iface);
            pthread_mutex_unlock(&N.mu);
        }
        struct pollfd pf[3] = { { fd, POLLIN, 0 }, { N.wake[0], POLLIN, 0 }, { dfd, POLLIN, 0 } };
        int ms = (int)((MIN(next, T[NQ_DNS].have ? next_dns : next) - mono()) * 1000.0) + 1;
        if (poll(pf, dfd >= 0 ? 3 : 2, MAX(ms, 0)) > 0) {
            if (pf[1].revents)
                break;
            if (pf[0].revents & POLLIN)
                read_answers(fd, raw, id, T, Q);
            if (dfd >= 0 && (pf[2].revents & POLLIN)) {
                tgt_t *t = &T[NQ_DNS];
                int r = read_dns(dfd, t->seq);
                if (r >= 0 && t->sent > 0) {
                    push(t, r ? (mono() - t->sent) * 1000.0 : -1); /* a failure counts as no answer */
                    t->sent = 0;
                    pthread_mutex_lock(&N.mu);
                    judge(t, &Q[NQ_DNS]);
                    N.q.t[NQ_DNS] = Q[NQ_DNS];
                    pthread_mutex_unlock(&N.mu);
                } /* r < 0: someone else's datagram, already read and dropped */
            }
        }
    }
    close(fd);
    if (dfd >= 0)
        close(dfd);
    return NULL;
}

static void stop(void)
{
    if (!N.running)
        return;
    if (write(N.wake[1], "x", 1) < 0) {
        /* the pipe is ours and empty: this doesn't happen */
    }
    pthread_join(N.thr, NULL);
    close(N.wake[0]);
    close(N.wake[1]);
    N.wake[0] = N.wake[1] = -1;
    N.running = 0;
}

void netq_set(int on, const char *host)
{
    const char *h = host && host[0] ? host : DEFAULT_HOST;
    if (N.running && on && !strcmp(h, N.host))
        return;
    stop();
    pthread_mutex_lock(&N.mu);
    memset(&N.q, 0, sizeof N.q);
    pthread_mutex_unlock(&N.mu);
    if (!on)
        return;
    snprintf(N.host, sizeof N.host, "%s", h);
    if (pipe2(N.wake, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    pthread_mutex_lock(&N.mu);
    N.q.on = 1;
    for (int k = 0; k < NQ_COUNT; k++)
        N.q.t[k].rtt_ms = N.q.t[k].avg_ms = N.q.t[k].min_ms = N.q.t[k].max_ms = N.q.t[k].jitter_ms =
            N.q.t[k].loss_pct = N.q.t[k].normal_ms = -1;
    pthread_mutex_unlock(&N.mu);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024); /* getaddrinfo wants some room */
    if (pthread_create(&N.thr, &at, pinger, NULL) == 0) {
        N.running = 1;
    } else {
        close(N.wake[0]);
        close(N.wake[1]);
        N.wake[0] = N.wake[1] = -1;
        pthread_mutex_lock(&N.mu);
        N.q.on = 0;
        pthread_mutex_unlock(&N.mu);
    }
    pthread_attr_destroy(&at);
}

void netq_read(netq_t *out)
{
    pthread_mutex_lock(&N.mu);
    *out = N.q;
    pthread_mutex_unlock(&N.mu);
}
