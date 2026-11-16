/*
 * hsm - hardware status monitor
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

/* A wired port's negotiated link (ethtool's link settings, which any user
 * may read): what the port can do, what the other end says it can do, and
 * what they agreed on. A gigabit port at 100 Mb/s with a gigabit switch on
 * the other end is a damaged cable (or one with two of its four pairs);
 * half duplex means the other end doesn't negotiate. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/ethtool.h>) && __has_include(<linux/sockios.h>)
#include <linux/ethtool.h>
#include <linux/sockios.h>
#endif
#endif

/* the copper (twisted pair) link modes, by bit: their speed in Mb/s */
static const struct {
    int bit, mbps;
} TP[] = { { 0, 10 }, { 1, 10 }, { 2, 100 }, { 3, 100 }, { 4, 1000 }, { 5, 1000 }, { 12, 10000 }, { 47, 2500 }, { 48, 5000 } };

static int top_speed(const unsigned *mask, int nwords)
{
    int best = 0;
    for (size_t i = 0; i < sizeof TP / sizeof TP[0]; i++) {
        int w = TP[i].bit / 32, b = TP[i].bit % 32;
        if (w < nwords && (mask[w] >> b & 1u))
            best = MAX(best, TP[i].mbps);
    }
    return best;
}

const char *eth_speed(int mbps, char *out, size_t n)
{
    if (mbps >= 1000 && mbps % 1000)
        sfmt(out, n, "%.1f Gb/s", mbps / 1000.0);
    else if (mbps >= 1000)
        sfmt(out, n, "%d Gb/s", mbps / 1000);
    else
        sfmt(out, n, "%d Mb/s", mbps);
    return out;
}

/* test hook: HSM_ETH_FAKE=dir answers from dir/eth0: "speed 100", "duplex full",
 * "port tp", "supported 10,100,1000", "partner 10,100,1000" */
static int eth_fake(netif_t *f, const char *dir, int *speed)
{
    char p[PATHBUF], b[512];
    sfmt(p, sizeof p, "%s/%s", dir, f->name);
    if (read_file(p, b, sizeof b) <= 0)
        return 0;
    char *cur = b, *l;
    f->eth_tp = 1;
    while ((l = next_line(&cur)) != NULL) {
        char v[128];
        if (sscanf(l, "speed %d", speed) == 1)
            continue;
        if (sscanf(l, "duplex %127s", v) == 1)
            f->eth_duplex = !strcmp(v, "full") ? 1 : !strcmp(v, "half") ? 0 : -1;
        else if (sscanf(l, "port %127s", v) == 1)
            f->eth_tp = !strcmp(v, "tp");
        else if (sscanf(l, "supported %127s", v) == 1 || sscanf(l, "partner %127s", v) == 1) {
            int best = 0;
            for (char *t = strtok(v, ","); t; t = strtok(NULL, ","))
                best = MAX(best, atoi(t));
            if (!strncmp(l, "supported", 9))
                f->eth_max = best;
            else
                f->eth_peer = best;
        }
    }
    return 1;
}

#ifdef ETHTOOL_GLINKSETTINGS
static int eth_read(int fd, netif_t *f, int *speed)
{
    static unsigned buf[sizeof(struct ethtool_link_settings) / 4 + 3 * 128];
    struct ethtool_link_settings *ls = (struct ethtool_link_settings *)(void *)buf;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    str_copy(ifr.ifr_name, sizeof ifr.ifr_name, f->name);
    ifr.ifr_data = (char *)buf;
    /* first: how many words the masks take (the kernel answers with minus that) */
    memset(buf, 0, sizeof buf);
    ls->cmd = ETHTOOL_GLINKSETTINGS;
    if (ioctl(fd, SIOCETHTOOL, &ifr) || ls->link_mode_masks_nwords >= 0 || ls->cmd != ETHTOOL_GLINKSETTINGS)
        return 0;
    int nw = -ls->link_mode_masks_nwords;
    if (nw > 128)
        return 0;
    memset(buf, 0, sizeof buf);
    ls->cmd = ETHTOOL_GLINKSETTINGS;
    ls->link_mode_masks_nwords = (signed char)nw;
    if (ioctl(fd, SIOCETHTOOL, &ifr) || ls->link_mode_masks_nwords != nw)
        return 0;
    const unsigned *m = (const unsigned *)ls->link_mode_masks;
    f->eth_tp = ls->port == PORT_TP || (ls->port == PORT_OTHER && top_speed(m, nw) > 0);
    f->eth_max = top_speed(m, nw);
    f->eth_peer = top_speed(m + 2 * nw, nw); /* the link partner's advertised modes */
    f->eth_duplex = ls->duplex == DUPLEX_FULL ? 1 : ls->duplex == DUPLEX_HALF ? 0 : -1;
    *speed = ls->speed == (unsigned)SPEED_UNKNOWN ? -1 : (int)ls->speed;
    return 1;
}
#endif

void eth_links(net_t *nt)
{
    const char *fake = secure_getenv("HSM_ETH_FAKE");
    int fd = -1;
#ifdef ETHTOOL_GLINKSETTINGS
    if (!fake || !*fake)
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#endif
    for (int i = 0; i < nt->n; i++) {
        netif_t *f = &nt->i[i];
        f->eth_ok = 0;
        f->eth_slow = ETH_OK;
        if (f->is_virtual || f->wireless || !f->up)
            continue;
        int speed = -1;
        f->eth_duplex = -1;
        f->eth_max = f->eth_peer = 0;
        if (fake && *fake)
            f->eth_ok = eth_fake(f, fake, &speed);
#ifdef ETHTOOL_GLINKSETTINGS
        else if (fd >= 0)
            f->eth_ok = eth_read(fd, f, &speed);
#endif
        if (!f->eth_ok)
            continue;
        if (speed > 0) /* what ethtool says is what sysfs says; the fake has its own */
            f->speed = speed;
        speed = f->speed;
        if (speed <= 0 || !f->eth_tp)
            continue;
        if (f->eth_duplex == 0)
            f->eth_slow = ETH_HALF;
        else if (f->eth_max > speed && f->eth_peer > speed)
            f->eth_slow = ETH_CABLE; /* both ends can do more */
        else if (f->eth_max > speed && !f->eth_peer)
            f->eth_slow = ETH_SLOWER; /* the cable, or the other end: can't tell */
    }
    if (fd >= 0)
        close(fd);
}
