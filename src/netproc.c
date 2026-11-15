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
/* netproc.c - network use per process, and the ports each one listens on.
 *
 * The kernel counts the bytes of every TCP connection (sock_diag, as `ss -ti`
 * shows: bytes received, bytes sent and acknowledged); a connection belongs
 * to the process that has its socket open, found from /proc/PID/fd. UDP
 * sockets have no such counts, so games, calls and QUIC show partly. Reading
 * the sockets of other users' processes needs root. Only done while the
 * process list wants it (procs_t.net_want_until), the socket-to-process map
 * again when a connection with traffic isn't in it (at most every 2 s, and
 * every 30 s anyway). */
#define _GNU_SOURCE
#include "collect.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    unsigned long long cookie, rx, tx;
    unsigned inode;
} sock_t;

typedef struct {
    unsigned inode;
    pid_t pid;
} owner_t;

static sock_t *g_cur, *g_prev;
static int g_ncur, g_nprev, g_capcur, g_capprev;
static owner_t *g_own;
static int g_nown, g_capown;
static double g_map_t;
static int g_fd = -1, g_primed;
static netport_t g_ports[512];
static int g_nports;

static int cmp_sock(const void *a, const void *b)
{
    const sock_t *x = a, *y = b;
    return (x->cookie > y->cookie) - (x->cookie < y->cookie);
}

static int cmp_own(const void *a, const void *b)
{
    const owner_t *x = a, *y = b;
    return (x->inode > y->inode) - (x->inode < y->inode);
}

static pid_t owner_of(unsigned inode)
{
    owner_t k = { inode, 0 };
    owner_t *o = g_nown ? bsearch(&k, g_own, (size_t)g_nown, sizeof *g_own, cmp_own) : NULL;
    return o ? o->pid : 0;
}

/* sockets -> processes: every socket the processes we may look at have open */
static void map_owners(void)
{
    g_nown = 0;
    DIR *d = opendir(g_procfs);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0)
            continue;
        char p[PATHBUF];
        sfmt(p, sizeof p, "%s/%ld/fd", g_procfs, pid);
        DIR *fd = opendir(p);
        struct dirent *f;
        while (fd && (f = readdir(fd)) != NULL) {
            if (f->d_name[0] == '.')
                continue;
            char lp[PATHBUF], l[64];
            sfmt(lp, sizeof lp, "%s/%ld/fd/%s", g_procfs, pid, f->d_name);
            ssize_t n = readlink(lp, l, sizeof l - 1);
            if (n < 9 || strncmp(l, "socket:[", 8))
                continue;
            l[n] = '\0';
            if (g_nown >= g_capown) {
                int nc = g_capown ? g_capown * 2 : 1024;
                owner_t *no = realloc(g_own, (size_t)nc * sizeof *no);
                if (!no)
                    break;
                g_own = no;
                g_capown = nc;
            }
            g_own[g_nown].inode = (unsigned)strtoul(l + 8, NULL, 10);
            g_own[g_nown].pid = (pid_t)pid;
            g_nown++;
        }
        if (fd)
            closedir(fd);
    }
    if (d)
        closedir(d);
    if (g_nown > 1) /* none at all (no processes to look at): nothing to sort */
        qsort(g_own, (size_t)g_nown, sizeof *g_own, cmp_own);
}

static int diag_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return -1;
    struct timeval tv = { 0, 250000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static void add_sock(unsigned long long cookie, unsigned inode, unsigned long long rx, unsigned long long tx)
{
    if (g_ncur >= g_capcur) {
        int nc = g_capcur ? g_capcur * 2 : 1024;
        sock_t *ns = realloc(g_cur, (size_t)nc * sizeof *ns);
        if (!ns)
            return;
        g_cur = ns;
        g_capcur = nc;
    }
    g_cur[g_ncur++] = (sock_t){ cookie, rx, tx, inode };
}

static void add_port(unsigned inode, int udp, int port, int v6, int local)
{
    for (int i = 0; i < g_nports; i++)
        if (g_ports[i].inode == inode)
            return;
    if (g_nports < (int)(sizeof g_ports / sizeof g_ports[0]))
        g_ports[g_nports++] = (netport_t){ inode, 0, udp, port, v6, local };
}

/* one dump: TCP or UDP sockets of one family. 0 or an errno. */
static int dump(int family, int proto)
{
    struct {
        struct nlmsghdr n;
        struct inet_diag_req_v2 r;
    } q;
    memset(&q, 0, sizeof q);
    q.n.nlmsg_len = sizeof q;
    q.n.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    q.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    q.n.nlmsg_seq = 1;
    q.r.sdiag_family = (unsigned char)family;
    q.r.sdiag_protocol = (unsigned char)proto;
    q.r.idiag_states = ~0u;
    if (proto == IPPROTO_TCP)
        q.r.idiag_ext = 1 << (INET_DIAG_INFO - 1);
    struct sockaddr_nl to;
    memset(&to, 0, sizeof to);
    to.nl_family = AF_NETLINK;
    if (sendto(g_fd, &q, sizeof q, 0, (struct sockaddr *)&to, sizeof to) != (ssize_t)sizeof q)
        return errno ? errno : EIO;
    static char buf[65536] __attribute__((aligned(8)));
    for (int round = 0; round < 4096; round++) {
        ssize_t n = recv(g_fd, buf, sizeof buf, 0);
        if (n < 0)
            return errno ? errno : EIO;
        for (struct nlmsghdr *h = (struct nlmsghdr *)(void *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE)
                return 0;
            if (h->nlmsg_type == NLMSG_ERROR)
                return EIO;
            if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct inet_diag_msg)))
                continue;
            const struct inet_diag_msg *m = NLMSG_DATA(h);
            unsigned long long cookie = (unsigned long long)m->id.idiag_cookie[0] |
                                        (unsigned long long)m->id.idiag_cookie[1] << 32;
            int sport = ntohs(m->id.idiag_sport), dport = ntohs(m->id.idiag_dport);
            /* listening: a TCP socket waiting for connections, a UDP one taking datagrams from anyone */
            int local = family == AF_INET ? (ntohl(m->id.idiag_src[0]) >> 24) == 127
                                          : (!m->id.idiag_src[0] && !m->id.idiag_src[1] && !m->id.idiag_src[2] &&
                                             ntohl(m->id.idiag_src[3]) == 1);
            if (proto == IPPROTO_TCP && m->idiag_state == 10 /* TCP_LISTEN */)
                add_port(m->idiag_inode, 0, sport, family == AF_INET6, local);
            else if (proto == IPPROTO_UDP && !dport && sport)
                add_port(m->idiag_inode, 1, sport, family == AF_INET6, local);
            if (proto != IPPROTO_TCP)
                continue;
            /* the byte counts, in the tcp_info that follows */
            int alen = (int)h->nlmsg_len - (int)NLMSG_LENGTH(sizeof *m);
            const struct rtattr *a = (const struct rtattr *)(const void *)((const char *)m + NLMSG_ALIGN(sizeof *m));
            for (; RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type != INET_DIAG_INFO)
                    continue;
                size_t need = offsetof(struct tcp_info, tcpi_bytes_received) + sizeof(unsigned long long);
                size_t have = (size_t)RTA_PAYLOAD(a);
                if (have < need)
                    break;
                struct tcp_info ti;
                memset(&ti, 0, sizeof ti);
                memcpy(&ti, RTA_DATA(a), have < sizeof ti ? have : sizeof ti);
                add_sock(cookie, m->idiag_inode, ti.tcpi_bytes_received, ti.tcpi_bytes_acked);
                break;
            }
        }
    }
    return EIO;
}

static proc_t *find_proc(procs_t *ps, pid_t pid)
{
    int lo = 0, hi = ps->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ps->p[mid].pid == pid)
            return &ps->p[mid];
        if (ps->p[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

void netproc_sample(procs_t *ps, double dt)
{
    double now = now_sec();
    for (int i = 0; i < ps->n; i++) { /* unknown unless its sockets can be looked at */
        proc_t *p = &ps->p[i];
        p->net_rx = p->net_tx = p->io_ok ? 0 : -1;
        p->nlisten = 0;
    }
    if (now >= ps->net_want_until) {
        g_primed = 0;
        ps->net_ok = 0;
        return;
    }
    if (g_fd < 0 && (g_fd = diag_open()) < 0) {
        ps->net_ok = 0;
        return;
    }
    /* swap: this sample's sockets become the previous ones */
    sock_t *t = g_prev;
    g_prev = g_cur;
    g_cur = t;
    int tc = g_capprev;
    g_capprev = g_capcur;
    g_capcur = tc;
    g_nprev = g_ncur;
    g_ncur = 0;
    g_nports = 0;
    int err = dump(AF_INET, IPPROTO_TCP);
    if (!err)
        err = dump(AF_INET6, IPPROTO_TCP);
    if (!err) {
        dump(AF_INET, IPPROTO_UDP);
        dump(AF_INET6, IPPROTO_UDP);
    }
    if (err) {
        close(g_fd);
        g_fd = -1;
        g_primed = 0;
        ps->net_ok = 0;
        return;
    }
    if (g_ncur > 1)
        qsort(g_cur, (size_t)g_ncur, sizeof *g_cur, cmp_sock);
    /* a connection with traffic whose process isn't known: look again (at most every 2 s) */
    int stale = now - g_map_t >= 30;
    for (int i = 0; i < g_ncur && !stale && now - g_map_t >= 2; i++) {
        sock_t *pv = g_nprev ? bsearch(&g_cur[i], g_prev, (size_t)g_nprev, sizeof *g_prev, cmp_sock) : NULL;
        if (pv && (g_cur[i].rx != pv->rx || g_cur[i].tx != pv->tx) && g_cur[i].inode && !owner_of(g_cur[i].inode))
            stale = 1;
    }
    if (stale) {
        map_owners();
        g_map_t = now;
    }
    ps->net_ok = 1;
    if (g_primed && dt > 0)
        for (int i = 0; i < g_ncur; i++) {
            sock_t *c = &g_cur[i];
            sock_t *pv = g_nprev ? bsearch(c, g_prev, (size_t)g_nprev, sizeof *g_prev, cmp_sock) : NULL;
            /* a connection new since the last sample: all of it happened since */
            unsigned long long rx0 = pv ? pv->rx : 0, tx0 = pv ? pv->tx : 0;
            if (c->rx < rx0 || c->tx < tx0)
                continue;
            pid_t pid = c->inode ? owner_of(c->inode) : 0;
            proc_t *p = pid ? find_proc(ps, pid) : NULL;
            if (!p)
                continue;
            if (p->net_rx < 0)
                p->net_rx = p->net_tx = 0;
            p->net_rx += (double)(c->rx - rx0) / dt;
            p->net_tx += (double)(c->tx - tx0) / dt;
        }
    for (int i = 0; i < g_nports; i++) {
        g_ports[i].pid = owner_of(g_ports[i].inode);
        proc_t *p = g_ports[i].pid ? find_proc(ps, g_ports[i].pid) : NULL;
        if (p)
            p->nlisten++;
    }
    g_primed = 1;
}

int netproc_ports(pid_t pid, netport_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_nports && n < max; i++) {
        if (g_ports[i].pid != pid)
            continue;
        int dup = 0; /* the same port on IPv4 and IPv6 once */
        for (int k = 0; k < n; k++)
            dup |= out[k].port == g_ports[i].port && out[k].udp == g_ports[i].udp;
        if (!dup)
            out[n++] = g_ports[i];
    }
    return n;
}
