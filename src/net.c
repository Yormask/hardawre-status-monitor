/* net.c - per-interface traffic (/proc/net/dev), link state and addresses */
#define _GNU_SOURCE
#include "collect.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static netif_t *find_if(net_t *nt, const char *name)
{
    for (int i = 0; i < nt->n; i++)
        if (!strcmp(nt->i[i].name, name))
            return &nt->i[i];
    return NULL;
}

static void if_link(netif_t *f)
{
    char p[PATHBUF];
    long long v;
    char old[16];
    str_copy(old, sizeof old, f->state);
    sfmt(p, sizeof p, "%s/class/net/%s/operstate", g_sysfs, f->name);
    if (read_line(p, f->state, sizeof f->state))
        str_copy(f->state, sizeof f->state, "?");
    /* tun/wireguard/ppp report "unknown" while working */
    f->up = !strcmp(f->state, "up") || !strcmp(f->state, "unknown");
    if (strcmp(old, f->state)) {
        sfmt(p, sizeof p, "%s/class/net/%s/speed", g_sysfs, f->name);
        f->speed = (f->up && !read_ll(p, &v) && v > 0) ? (int)v : -1;
    }
}

void net_sample(net_t *nt, double dt)
{
    static char *buf;
    static size_t cap;
    char path[PATHBUF];
    sfmt(path, sizeof path, "%s/net/dev", g_procfs);
    if (read_file_grow(path, &buf, &cap) <= 0)
        return;

    for (int i = 0; i < nt->n; i++)
        nt->i[i].seen = 0;

    char *cur = buf, *line;
    int lineno = 0;
    while ((line = next_line(&cur)) != NULL) {
        if (lineno++ < 2) /* two header lines */
            continue;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *name = line;
        while (*name == ' ')
            name++;
        if (!strcmp(name, "lo"))
            continue;
        unsigned long long rxb, rxp, txb, txp;
        if (sscanf(colon + 1, "%llu %llu %*u %*u %*u %*u %*u %*u %llu %llu", &rxb, &rxp, &txb, &txp) != 4)
            continue;
        netif_t *f = find_if(nt, name);
        if (!f) {
            if (nt->n >= MAX_NETIF)
                continue;
            f = &nt->i[nt->n++];
            memset(f, 0, sizeof *f);
            str_copy(f->name, sizeof f->name, name);
            f->speed = -1;
            sfmt(path, sizeof path, "%s/class/net/%s/device", g_sysfs, name);
            f->is_virtual = !path_exists(path);
            sfmt(path, sizeof path, "%s/class/net/%s/wireless", g_sysfs, name);
            f->wireless = path_exists(path);
        }
        f->seen = 1;
        if_link(f);
        if (f->primed && dt > 0) {
            f->rx_bps = rxb >= f->rx_total ? (double)(rxb - f->rx_total) / dt : 0;
            f->tx_bps = txb >= f->tx_total ? (double)(txb - f->tx_total) / dt : 0;
        }
        f->rx_total = rxb;
        f->tx_total = txb;
        f->primed = 1;
    }

    int w = 0;
    for (int i = 0; i < nt->n; i++)
        if (nt->i[i].seen) {
            if (w != i)
                nt->i[w] = nt->i[i];
            w++;
        }
    nt->n = w;

    /* Total = physical NICs only, so bridges/veth/VPN tunnels are not
     * counted twice. Fall back to everything when there is no physical NIC
     * (containers, VMs with virtual-only interfaces). */
    double rx = 0, tx = 0, vrx = 0, vtx = 0;
    int phys = 0;
    for (int i = 0; i < nt->n; i++) {
        if (!nt->i[i].is_virtual) {
            rx += nt->i[i].rx_bps;
            tx += nt->i[i].tx_bps;
            phys++;
        }
        vrx += nt->i[i].rx_bps;
        vtx += nt->i[i].tx_bps;
    }
    nt->rx_bps = phys ? rx : vrx;
    nt->tx_bps = phys ? tx : vtx;
}

void net_addrs(net_t *nt)
{
    struct ifaddrs *ifa = NULL;
    for (int i = 0; i < nt->n; i++)
        nt->i[i].addr[0] = '\0';
    if (getifaddrs(&ifa) != 0)
        return;
    for (struct ifaddrs *a = ifa; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        netif_t *f = find_if(nt, a->ifa_name);
        if (!f || f->addr[0])
            continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr, f->addr, sizeof f->addr);
    }
    freeifaddrs(ifa);
}
