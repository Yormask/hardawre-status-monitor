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

/* collect.h - data collectors for every subsystem hsm monitors.
 *
 * Every collector reads directly from /proc and /sys (no external tools, no
 * helper processes), keeps the previous raw counters itself and turns them
 * into rates using the real elapsed time `dt` passed in by the caller. */
#ifndef HSM_COLLECT_H
#define HSM_COLLECT_H

#include "util.h"
#include <sys/types.h>
#include <stdio.h>

/* ------------------------------------------------------------------ CPU */
#define MAX_CPUS 512

typedef struct {
    double user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_jiffies_t;

#define MAX_CSTATES 12
typedef struct {
    char name[16];      /* "C1", "C6", "POLL" */
    double pct;         /* average share of time all CPUs spent in it */
    unsigned latency;   /* exit latency in us */
    int disabled;
} cstate_t;

typedef struct { /* one kind of CPU cache */
    unsigned char level;
    char type;        /* 'd' data, 'i' instruction, 'u' both */
    int kb;           /* size of one */
    int share;        /* cores that share one */
    signed char kind; /* kind of core (ctype index), -1 = all alike */
    int n;            /* how many there are */
} cache_t;

typedef struct {
    char model[128];
    int cores, threads;   /* topology from /proc/cpuinfo                   */
    int ncpu;             /* CPUs currently online (lines in /proc/stat)   */
    int id[MAX_CPUS];     /* kernel CPU number of each entry               */
    double pct[MAX_CPUS]; /* busy % per logical CPU                        */
    double mhz[MAX_CPUS]; /* current clock per logical CPU (0 = unknown)   */
    /* per kernel CPU number (index = id[]): hardware max clock, topology */
    double mhz_hw[MAX_CPUS];
    int core_id[MAX_CPUS], pkg_id[MAX_CPUS];
    unsigned char topo_done[MAX_CPUS];
    /* kinds of cores (Intel P-cores and E-cores, ARM big and LITTLE ones);
     * nctype 0 = all alike */
    int nctype;
    struct {
        char name[24];  /* "P-core", "E-core", "LP E-core", "Cortex-A76" */
        char letter;    /* on the core rows: 'P', 'E', 'L' */
        int cores, threads;
    } ctype[4];
    signed char core_type[MAX_CPUS]; /* per kernel CPU number: index into ctype, -1 = unknown */
    double total, user, sys, iowait, steal;
    double mhz_avg, mhz_max;
    double temp_c; /* package temperature, -1 = unknown */
    double load[3], uptime;
    unsigned procs_running, procs_blocked;
    double ctxt_rate, intr_rate, fork_rate;

    /* power settings (cpufreq / intel_pstate / ACPI platform profile); "" = n/a */
    char driver[24], governor[24], epp[40], pstate[16], profile[32];
    int turbo;                    /* 1 on, 0 off, -1 unknown */
    double lim_min, lim_max;      /* scaling limits in MHz, -1 = unknown */
    double hw_min, hw_max;        /* what the hardware can do, MHz */
    /* idle states (cpuidle), averaged over all CPUs */
    int nidle;
    cstate_t idle[MAX_CSTATES];
    double wakeups; /* idle-state entries per second, about the number of wake-ups */

    /* details: read once (SMT every 5 s) */
    int det_done;
    char vendor_id[16];       /* GenuineIntel, AuthenticAMD; "" on ARM */
    int hypervisor;           /* runs in a virtual machine (x86 says so) */
    int virt;                 /* the CPU can run virtual machines: 'v' VT-x, 's' AMD-V, 0 = no or unknown */
    int family, model_no, stepping; /* x86 CPUID; -1 = unknown */
    char ucode[24];           /* microcode revision, "0x2c"; "" = unknown */
    char isa[16];             /* x86-64-v3; "" on ARM */
    char feats[200];          /* the notable instruction sets: "AVX2, FMA3, AES, SHA" */
    char smt[16];             /* on, off, forceoff, notsupported, notimplemented; "" = unknown */
    int smt_active;           /* a core runs two threads now: 1, 0; -1 = unknown */
    int base_mhz;             /* the clock it's rated at; 0 = unknown */
    int kbase[4];             /* the same per kind of core (P-cores 3600, E-cores 2700) */
    int numa;                 /* NUMA nodes; 0 = unknown */
    int ncache;
    cache_t cache[16];

    /* private */
    cpu_jiffies_t prev_all, prev[MAX_CPUS];
    unsigned long long prev_ctxt, prev_intr, prev_forks;
    int primed, freq_src;
    unsigned long long idle_time[MAX_CPUS][MAX_CSTATES], idle_use[MAX_CPUS][MAX_CSTATES];
    double idle_t;
    int idle_primed;
} cpu_t;
void cpu_details(cpu_t *c);                      /* caches, instruction sets, microcode, SMT, base clock */
const char *cache_size(int kb, char *out, size_t n); /* "48K", "1.25M", "32M" */
int cache_lines(const cpu_t *c, char out[][96], int max); /* "L1  48K data + 32K instructions per P-core" */
void smt_text(const cpu_t *c, char *out, size_t n);  /* "on: 2 threads per P-core" */
int cpu_boost_mhz(const cpu_t *c, int kind);         /* most a core of a kind (-1 any) runs at; 0 = unknown */
void clock_text(const cpu_t *c, int boost, char *out, size_t n); /* "3600 MHz (P-cores), 2700 MHz (E-cores)" */

void cpu_init(cpu_t *c);
void cpu_types_text(const cpu_t *c, char *out, size_t n); /* "6P+8E", "" = all cores alike */
void cpu_sample(cpu_t *c, double dt);
void cpu_power_sample(cpu_t *c); /* governor, turbo...: slow-changing, every few seconds */

/* --------------------------------------------------------------- Memory */
#define MAX_SWAPS 8

/* a swap device from /proc/swaps; zram is compressed RAM */
typedef struct {
    char name[48];                  /* "zram0", "nvme0n1p3", "/swapfile" */
    int zram, file;                 /* compressed RAM; a swap file (else a partition) */
    unsigned long long size, used;  /* bytes */
    int prio;
    unsigned long long orig, compr, ram; /* zram: what's stored, compressed to, and the RAM it takes, bytes */
    char algo[16];                  /* zram: the compressor */
} swapdev_t;

typedef struct {
    unsigned long long total, free, avail, buffers, cached, sreclaim, shmem;
    unsigned long long swap_total, swap_free, dirty, writeback;
    unsigned long long used, buffcache, swap_used; /* derived */
    double swapin_bps, swapout_bps, majflt_rate;
    unsigned long long oom_kill; /* processes killed for lack of memory since boot */
    int oom_ok;                  /* the kernel reports oom_kill */

    unsigned long long p_swpin, p_swpout, p_majflt;
    int primed;
    int nswap, nzram;
    swapdev_t swap[MAX_SWAPS];
    int zswap;                      /* zswap (a compressed cache in front of swap): 1 on, 0 off, -1 not built in */
    char zswap_algo[16];
    unsigned long long zswap_stored, zswap_pool; /* bytes, root only (debugfs); 0 = unknown */
} mem_t;

void mem_sample(mem_t *m, double dt);

/* -------------------------------------------------------------- Storage */
#define MAX_DISKS 64
#define MAX_FS 64

typedef struct {
    char name[32], model[64], kind[8];
    unsigned long long size, rd_total, wr_total; /* bytes */
    double rd_bps, wr_bps, rd_iops, wr_iops, util;
    double rd_lat, wr_lat; /* average time per read / write in ms over the last sample */
    unsigned inflight;     /* requests in progress right now */

    unsigned long long p_rsec, p_wsec, p_rio, p_wio, p_ticks, p_rms, p_wms;
    int primed, seen;
} disk_t;

enum { TRIM_NA, TRIM_DISCARD, TRIM_TIMER, TRIM_NONE, TRIM_BLOCKED };
typedef struct {
    char mount[128], dev[96], type[16];
    unsigned long long total, used, avail;
    unsigned long long files, ffree; /* inodes (file slots); files 0 = the file system has no fixed number */
    int readonly;
    int discard;                     /* mounted with "discard": TRIM on every delete */
    /* TRIM, for a file system on an SSD: how it's done (TRIM_*), and what to know */
    int trim;
    char trim_why[96];               /* blocked: by what; timer: which */
    double trim_ago;                 /* the timer last ran, seconds ago; -1 = not yet, or unknown */
} fs_t;


typedef struct {
    int n;
    disk_t d[MAX_DISKS];
    int nfs;
    fs_t fs[MAX_FS];
    double rd_bps, wr_bps;
} disks_t;
void trim_sample(disks_t *ds); /* how each file system on an SSD gets trimmed */
const char *trim_text(const fs_t *f, char *out, size_t n, int *warn); /* "weekly (fstrim.timer), last 2d 04h ago" */

void disks_sample(disks_t *ds, double dt);
void fs_sample(disks_t *ds);

/* -------------------------------------------------------------- Network */
#define MAX_NETIF 64

/* Wi-Fi details from nl80211 */
typedef struct {
    int ok;                    /* connected: these are known */
    int sta;
    int freq, center, width;   /* MHz: the channel's frequency, its centre, its width */
    int channel, gen;          /* gen: Wi-Fi 4 (n), 5 (ac), 6 (ax), 7 (be); 0 older or unknown */
    int signal;                /* dBm, 0 = unknown */
    double tx_mbps, rx_mbps;   /* link speed each way, -1 = unknown */
    unsigned long long tx_pkts, tx_retries, tx_failed;
    unsigned connected_s, beacon_loss;
    double retry_pct;          /* frames sent again, of all sent, lately; -1 = unknown */
    double win_t;              /* ... counted from here (monotonic) */
    unsigned long long win_pkts, win_retries;
    int band_told;             /* the frequency the last band event was about */
    double band_since;         /* on another band since (monotonic), 0 = no */
    /* how crowded its channel is: the other networks the last scan found
     * (the kernel keeps them; hsm starts no scan of its own) */
    unsigned char bssid[6];    /* the access point it's connected to */
    int crowd_ok;              /* known */
    int crowd_same, crowd_near; /* other networks on its channel / overlapping it (2.4 GHz neighbours, or inside its width) */
    int crowd_strong;          /* ... of those, strong ones (-70 dBm or more): these slow it down most */
    int crowd_band;            /* every other network seen on this band */
    int ch24[3];               /* 2.4 GHz: networks on or next to channels 1, 6 and 11 */
    double crowd_age;          /* seconds since the scan */
    double crowd_t;            /* asked at (monotonic), every 30 s */
} wifi_t;

typedef struct {
    char name[32], state[16], addr[48];
    int up, is_virtual, wireless, speed; /* speed in Mb/s, -1 = unknown */
    unsigned long long rx_total, tx_total;
    double rx_bps, tx_bps;
    unsigned long long errors, drops; /* since boot, both directions */
    double err_rate, drop_rate;       /* per second */
    double signal_dbm;                /* Wi-Fi signal, 0 = n/a */
    int quality;                      /* Wi-Fi link quality %, -1 = n/a */
    long long link_downs;             /* times the link (cable, Wi-Fi) went down since boot, -1 = n/a */
    long long downs_seen;             /* health: the count already reported */
    double down_t;                    /* health: when the link went down (wall), 0 = it's up */
    int primed, seen;
    wifi_t wifi; /* wireless interfaces */
    /* a wired port's link, as negotiated (ethtool; no root needed) */
    int eth_ok;          /* read */
    int eth_tp;          /* a copper (RJ45) port: where a bad cable shows as a slower link */
    int eth_max;         /* the most the port can do, Mb/s */
    int eth_peer;        /* the most the other end says it can do, Mb/s; 0 = it doesn't say */
    int eth_duplex;      /* 1 full, 0 half, -1 unknown */
    int eth_slow;        /* ETH_* below */
    int eth_told;        /* health: the speed already told about (Mb/s, -2 half duplex) */
} netif_t;
enum { ETH_OK, ETH_CABLE, ETH_SLOWER, ETH_HALF };

/* network quality: the router and one internet address pinged every 2 s
 * (netcheck.c, off unless switched on) */
enum { NQ_ROUTER, NQ_NET, NQ_DNS, NQ_COUNT }; /* NQ_DNS: a name looked up at the DNS server, every 10 s */
enum { NQ_FINE, NQ_LOSS, NQ_SLOW };   /* trouble right now */
typedef struct {
    char addr[48];                    /* what is pinged, "" = nothing (no router) */
    int noroute;                      /* the network says it can't be reached */
    double rtt_ms;                    /* the latest answer, -1 = lost, or none yet */
    double avg_ms, min_ms, max_ms, jitter_ms; /* the last minute's answers, -1 = none */
    double loss_pct;                  /* the last minute, -1 = nothing sent yet */
    double normal_ms;                 /* its usual delay, while all was well, -1 = unknown */
    int trouble;                      /* NQ_* now (after a few pings in a row) */
    int episode;                      /* counts troubles: a new number when one starts */
    double since, until;              /* the latest trouble started / ended (wall); until 0 = still on */
    double worst_loss, worst_ms;      /* the worst of this trouble */
} nqt_t;
typedef struct {
    int on;                           /* the check runs */
    int err;                          /* it can't: errno of the ping socket (EACCES: no permission) */
    char iface[32];                   /* the interface the router is on */
    nqt_t t[NQ_COUNT];
} netq_t;
void netq_set(int on, const char *host); /* start or stop it (the setting); host "" = 1.1.1.1 */
void netq_read(netq_t *out);             /* a copy of the figures */

typedef struct {
    int n;
    netif_t i[MAX_NETIF];
    double rx_bps, tx_bps;
    netq_t q; /* the network check's figures, copied every sample */
} net_t;
void eth_links(net_t *nt);                     /* the wired ports' negotiated links */
const char *eth_speed(int mbps, char *out, size_t n); /* "100 Mb/s", "2.5 Gb/s" */

void net_sample(net_t *n, double dt);
void wifi_sample(netif_t *f, double mono);                         /* in net_sample, for a wireless interface */
int wifi_band_moved(netif_t *f, double mono, char *out, size_t n); /* 1: a band event to tell */
int wifi_channel(int mhz);
const char *wifi_band(int mhz); /* "2.4 GHz", "5 GHz", "6 GHz", "" */
void net_addrs(net_t *n);

/* ------------------------------------------------------------ Processes */
typedef struct {
    pid_t pid, ppid;
    uid_t uid;
    char state, comm[24];
    char *cmdline; /* loaded lazily by proc_cmdline(), owned by the record */
    int prio, nice, nthreads, kthread, io_ok;
    unsigned long long ticks, start, vsize, rss, io_r, io_w;
    double cpu, mem, rd_bps, wr_bps;
    int nfds;                   /* open files, -1 = unknown (other user's process) */
    double gpu;                 /* GPU busy %, -1 = not using a GPU / unknown */
    unsigned long long gpu_mem; /* GPU memory in bytes */
    double net_rx, net_tx;      /* TCP bytes/s in and out (netproc.c), -1 = unknown (another user's) */
    int nlisten;                /* ports it listens on */

    /* private: open-file and GPU client tracking */
    double fd_t, drm_t;
    int drm_nfds, ndrm;
    int drm_fd[6];
    unsigned long long gpu_ns[8]; /* busy time per GPU engine (procs_t.geng) */
    int gpu_primed;
} proc_t;

typedef struct {
    proc_t *p, *prev; /* both sorted by pid */
    int n, cap, nprev, prevcap;
    int threads, running, sleeping, diskwait, zombie, stopped, io_denied;
    long hz, pagesize;
    void *dir; /* DIR* on /proc, kept open between samples */
    int gpu_any;       /* some process reported GPU use (DRM fdinfo or NVML) */
    int drm;           /* the machine has DRM (GPU) devices */
    int ngeng;         /* GPU engine names seen in fdinfo ("render", "gfx"...) */
    char geng[8][24];
    double gcap[8];    /* engine capacity (parallel units) */
    pid_t slow_pid;    /* the process that took longest to read this time, and how long */
    double slow_s;
    double net_want_until; /* network use per process is read until then (the process list asks) */
    int net_ok;            /* ... and it could be */
} procs_t;

/* netproc.c: network use per process (TCP), and the ports they listen on */
typedef struct {
    unsigned inode;
    pid_t pid;
    int udp, port, v6;
    int local; /* only this machine can reach it (127.0.0.1, ::1) */
} netport_t;
void netproc_sample(procs_t *ps, double dt);
int netproc_ports(pid_t pid, netport_t *out, int max);

void procs_init(procs_t *ps);
void procs_sample(procs_t *ps, double dt, unsigned long long memtotal);
void procs_free(procs_t *ps);
const char *proc_cmdline(proc_t *p);
int proc_send_signal(pid_t pid, unsigned long long start, int sig); /* 0 or errno */
int proc_renice(pid_t pid, unsigned long long start, int nice);    /* every thread; 0 or errno */
/* which CPUs it may run on: set[i] for CPU i; returns how many, or -errno */
int proc_affinity_get(pid_t pid, unsigned char *set, int max);
int proc_affinity_set(pid_t pid, unsigned long long start, const unsigned char *set, int max); /* every thread */
/* disk priority (ionice): IOP_* class, level 0 (first) to 7 (last) */
enum { IOP_NONE, IOP_RT, IOP_BE, IOP_IDLE };
int proc_ioprio_get(pid_t pid, int *cls, int *level);
int proc_ioprio_set(pid_t pid, unsigned long long start, int cls, int level); /* every thread; 0 or errno */
const char *uid_name(uid_t uid);

/* ------------------------------------------------------ Sensors (hwmon) */
enum { SENS_TEMP, SENS_FAN, SENS_POWER, SENS_VOLT, SENS_OTHER };
#define MAX_SENSORS 256

/* your own value, read from a file: a smart plug's wall power, a UPS, a
 * room thermometer ("file_sensor = ..." lines in the config file) */
#define MAX_FSENS 16
typedef struct {
    char path[PATHBUF];
    char name[48];
    char unit[12];   /* as written: "W", "C", "%", "ppm"; it decides the kind of sensor */
    char key[32];    /* the number after this word (JSON, "key=value"); "" = the first number */
    double scale;    /* multiplied by this, 0 = 1 */
    int total;       /* the whole machine's power (a smart plug only it is on): the power total */
} fsdef_t;

typedef struct {
    int type, chip_idx, ok;
    char chip[32], label[48], path[PATHBUF];
    double value, crit; /* °C / RPM / W / V ; crit -1 = none */
    double peak, fmax;  /* highest value seen; fans: fanN_max if exposed */
    double lo, hi;      /* voltages: the chip's min / max limits, 0 = none */
    double hot_t;       /* health: when a "too hot" event was last raised */
    int fan_seen;       /* health: this fan has been seen spinning */
    int fan_flagged;    /* health: "stopped while hot" was reported */
    double fan_zero_t;  /* health: since when it reads 0 while hot */
    /* your own name and limits ("sensor = ..." lines in the config file) */
    char id[96];        /* "chip/label": stays the same across boots */
    char name[48];      /* "" = the label */
    double ulo, uhi;    /* your limits in the sensor's unit, 0 = none (fans: ulo = minimum RPM) */
    int hidden;         /* left out of the screen and the logs */
    double lim_t;       /* health: since when it is past your limit, 0 = it isn't */
    int lim_flagged;    /* health: reported */
    /* a reading that never moves while the others do: a placeholder the
     * firmware fills in (ACPI's 27.8 °C), or an input with nothing on it */
    double vmin, vmax, t_first;
    int still;
    /* your own value from a file (fsdef_t): */
    int file;           /* 1 = it is one */
    char unit[12];      /* its unit, as shown ("°C" ones are kept in °C) */
    char fkey[32];
    double fscale, fadd; /* value = number * fscale + fadd (°F, kW, mV...) */
    int ftotal;         /* the machine's whole power */
    int fstale;         /* the file hasn't changed for 15 minutes: the value is old */
    double fmtime;      /* the file's last change (wall) */
} sensor_t;

typedef struct {
    int n;
    sensor_t s[MAX_SENSORS];
    int cpu_idx; /* sensor used as "CPU temperature", -1 = none */
    char slow[32]; /* the chip that took longest to read this time, and how long */
    double slow_s;
} sensors_t;

/* your settings for one sensor, kept even while it isn't there (a USB
 * device, a graphics card that's out) */
#define MAX_SRULES 128
typedef struct {
    char id[96];   /* sensor_t.id */
    char name[48]; /* "" = keep the label */
    double lo, hi; /* 0 = none */
    int hide;
} srule_t;
void sensors_apply(sensors_t *S, const srule_t *r, int n);
void sensors_files(sensors_t *S, const fsdef_t *d, int n); /* your own values, after the hwmon ones */
const char *sensor_unit(const sensor_t *t); /* "C", "RPM", "W", "V", or a file sensor's own */
const char *sensor_label(const sensor_t *t);
int sensor_plausible(const sensor_t *t); /* 0: a temperature nothing reaches (an input with nothing on it) */ /* your name, or the chip's label */
int sensor_bad(const sensor_t *t);           /* past your limits (voltages: or the chip's) */

void sensors_init(sensors_t *s);
void sensors_sample(sensors_t *s);
double sensors_cpu_temp(const sensors_t *s);
double sensors_core_temp(const sensors_t *s, int pkg, int core); /* coretemp "Core N", -1 */
void sensors_chip_name(const char *chip, char *out, size_t n); /* "nvme#2" -> "NVMe 2" */
void sensor_short_name(const sensors_t *S, int i, char *out, size_t n); /* "ACPI 2", "BAT1", "Core 3" */

/* ------------------------------------------------------------------ GPU */
#define MAX_GPUS 8
#define MAX_ENG 5

typedef struct {
    char name[8];
    int fd;
    unsigned long long prev;
    double pct;
} gpu_engine_t;

#define MAX_SCREENS 8

/* a screen on one of a GPU's ports (DRM connectors; reading them doesn't wake the GPU) */
typedef struct {
    char conn[24];    /* "eDP-1", "HDMI-A-1", "DP-2" */
    char monitor[24]; /* the name in its EDID, "DELL U2720Q"; "" unknown */
    int w, h;         /* its preferred mode */
    double inch;      /* diagonal, from the EDID's size; 0 = unknown */
    int builtin;      /* the laptop's own panel (eDP, LVDS, DSI) */
    int on;           /* in use (connected, and something draws on it) */
    /* what it runs at (DRM, read-only; never asked of a GPU that is powered off) */
    int mode_w, mode_h; /* now, 0 = unknown */
    double hz;          /* refresh rate now, 0 = unknown */
    double max_hz;      /* the most the connection offers at that size */
    int vrr_cap, vrr_on; /* variable refresh (FreeSync / G-Sync / Adaptive-Sync): it can / on now; -1 unknown */
    int nown;           /* the monitor's own timings (its EDID): what it can do, whatever the connection allows */
    struct {
        unsigned short w, h;
        float hz;
    } own[16];
    double mon_hz;      /* the most of those at the size it runs at, 0 = unknown */
} screen_t;
enum { SCR_FINE, SCR_SETTING, SCR_LINK }; /* screen_slow(): fine / a faster rate is a setting away / the connection holds it back */

typedef struct {
    char name[96], vendor[8], driver[16], card[16], pci[24], note[80];
    int integrated;
    int nscr;                   /* screens connected to its ports */
    screen_t scr[MAX_SCREENS];
    double scr_t;               /* read at (monotonic): every 5 s */
    double edid_t;              /* the monitors' own data (EDID, modes) read at (monotonic) */
    double drm_t;               /* refresh rates read at (monotonic) */
    char scr_told[128];         /* health: the screens already told about ("HDMI-A-1,DP-1,") */
    char hz_told[128];          /* health: screens told to run below their refresh rate ("DP-2@60,") */
    int scr_primed;
    double util;                /* busiest engine %, -1 = unknown */
    double mem_used, mem_total; /* bytes, -1 = unknown            */
    double temp_c, power_w, power_cap_w, clk_mhz, clk_max_mhz, mem_clk_mhz, fan_pct, fan_rpm;
    int neng;
    gpu_engine_t eng[MAX_ENG]; /* Intel i915 per-engine busy (perf PMU) */
    double eng_t;
    int util_est;              /* util is an estimate (Intel: from RC6 idle time) */
    unsigned long long rc6_prev;
    double rc6_t;

    /* NVIDIA (NVML): sleeping, limits, why it runs slower than it could */
    int nv_state;          /* NV_* below; NV_NONE for other GPUs */
    double temp_lim;       /* slows itself down from here (max operating temperature), -1 = unknown */
    double temp_slow, temp_shut; /* hardware slowdown / shutdown temperatures, -1 = unknown */
    int reasons_ok;        /* the driver says why the clock is held back */
    unsigned long long reasons; /* NVML clock event reasons now */
    int slow_now;          /* held back now: GS_HEAT | GS_POWER */
    int slow_hw;           /* ... and the hardware stepped in (a thermal or power brake, PROCHOT) */
    int sys_bits;          /* AMD / Intel: GS_* as their driver reported it this sample */
    double slow_since;     /* monotonic: held back without a break since, 0 = not now */
    double busy_s, heat_s, pwr_s, held_s; /* since start: seconds busy, and held back by heat / power / either while busy */
    double slow_ev_t;      /* when the last "slowing down" event was raised */
    double enc_pct, dec_pct;          /* video encoder / decoder busy, -1 = unknown (NVIDIA, while awake) */
    int pcie_gen, pcie_width;         /* the PCIe link now, 0 = unknown (NVIDIA, while awake) */
    int pcie_gen_max, pcie_width_max; /* the best this card and its slot can do, 0 = unknown */
    int pcie_slow_n, pcie_told;       /* health: under load at a slower link than it can do (checks in a row), told */
    int lk_gen, lk_width;             /* private: the link read from sysfs every 5 s (not NVIDIA) */

    /* private */
    char dev[PATHBUF], hwmon[PATHBUF], drm[PATHBUF];
    unsigned long long p_energy;
    double p_energy_t;
    void *nvml; /* nvmlDevice_t while the GPU is read through NVML */
    unsigned long long nv_ts; /* NVML per-process samples already seen */
    char pm_path[PATHBUF];    /* its PCI device's power/runtime_status: asleep or not, without waking it */
    int pm_auto;              /* the kernel may power it off when idle (power/control = auto, driver agrees) */
    double nv_idle_t;         /* monotonic: idle (0% load) without a break since, 0 = busy */
    double nv_wait_t;         /* NV_IDLE: when NVML let go of it */
    double last_power;        /* NV_IDLE: the last figure, for the totals */
    int nv_fails;             /* let go of it, and it stayed on anyway (something else uses it) */
} gpu_t;

enum { NV_NONE, NV_ON, NV_IDLE, NV_ASLEEP };
enum { GS_HEAT = 1, GS_POWER = 2 };

typedef struct {
    int n;
    gpu_t g[MAX_GPUS];
    double nv_keep_until; /* NVML stays attached until then (the GPU didn't sleep when let go) */
    double nv_retry_t;    /* NVML init failed: try again from then */
    char slow[112];       /* the GPU that took longest to read this time ("screens of the ..." when its screens did) */
    double slow_s;
} gpus_t;

void gpus_init(gpus_t *g);
void gpus_sample(gpus_t *g, double dt);
const char *gpu_screen_holds(const gpu_t *g, char *out, size_t n); /* a screen keeping a dGPU awake: "HDMI-A-1 (DELL U2720Q)" */
int screen_slow(const screen_t *s, double *can); /* an outside screen well below the refresh rate it can do: SCR_* */
void gpu_screens_wanted(void);                   /* the Hardware view shows them: read refresh rates every 5 s, not 60 */
void gpus_close(gpus_t *g);
void gpus_proc_sample(gpus_t *g, procs_t *ps); /* NVIDIA: per-process GPU use through NVML */
int pci_lookup_name(unsigned vendor, unsigned device, char *out, size_t n); /* from pci.ids */
FILE *pci_ids_open(void);                                                   /* the pci.ids there is, or NULL */

/* the PCI devices and their drivers: a device people use with no driver
 * (a card reader, a Wi-Fi card) is the first thing to check when it doesn't work */
#define MAX_PCI 128
typedef struct {
    char slot[16];   /* "0000:03:00.0" */
    unsigned cls;    /* class, subclass, interface: 0x030000 */
    unsigned ven, dev;
    char kind[20];   /* "graphics", "Wi-Fi", "card reader" */
    char name[96];   /* "Realtek RTS5227 PCI Express Card Reader" */
    char driver[32]; /* "" = none */
    int shown;       /* a device people use (not a bridge or chipset part) */
    int needs;       /* one of those, with no driver */
    int vf;          /* an SR-IOV virtual function (its driver is up to the VM) */
    int named;       /* private: looked up */
} pcid_t;
typedef struct {
    int ok;          /* /sys/bus/pci is there */
    int n, nodrv;    /* devices; ones people use with no driver */
    int hidden;      /* bridges and chipset parts, not listed */
    pcid_t d[MAX_PCI];
} pcibus_t;
void pci_sample(pcibus_t *b); /* what's there, and its driver */
int pci_vm_driver(const char *drv); /* vfio-pci, pci-stub: kept for virtual machines */

/* ---------------------------------------------------------------- Power */
#define MAX_RAPL 16

typedef struct {
    char name[32], path[PATHBUF];
    int depth, readable, primed;
    unsigned long long max_range, prev;
    double prev_t; /* when prev was read: watts come from the time between two reads */
    double watts;
} rapl_t;

typedef struct {
    char name[16], status[24], path[PATHBUF];
    double watts, capacity, time_left; /* -1 = unknown; time_left in s */
    double health;                     /* full charge now vs. new, %, -1 = unknown */
    double full_wh, design_wh;         /* what a full charge holds now / when new, -1 = unknown */
    double energy_wh, energy_full_wh;  /* the charge in it now / when full, Wh, -1 = unknown */
    long cycles;                       /* charge cycles, -1 = unknown */
    /* a charge limit (laptops that can stop charging early to slow wear) */
    int lim_ok;              /* the battery has the setting */
    int lim_end, lim_start;  /* stops at / starts again below, %; -1 = not set */
    double lim_t;            /* when it was last read (monotonic) */
} battery_t;

/* how much the CPU package may draw: readable without root. The same limits
 * can be set twice, in the CPU's registers (MSR) and by the firmware through
 * MMIO; the lower of the two applies. */
typedef struct {
    int ok;                          /* the package has a sustained limit */
    double long_w, long_s;           /* sustained (PL1) and the window it is averaged over; -1 = none */
    double short_w;                  /* short boosts (PL2), -1 = none */
    int long_off;                    /* the sustained limit is switched off */
    double fw_long_w, fw_short_w;    /* the firmware's copy (MMIO), -1 = none */
    double eff_long_w, eff_short_w;  /* what applies: the lower of the two */
    double t;                        /* when they were read (monotonic) */
} cpulim_t;

/* the battery of a wireless mouse, keyboard, headset or game controller
 * (power_supply entries with scope "Device") */
#define MAX_DEVBAT 8
typedef struct {
    char id[48];      /* "hidpp_battery_0", "ps-controller-battery-..." */
    char name[64];    /* the device: "G502 LIGHTSPEED", "Wireless Controller" */
    char path[PATHBUF];
    double capacity;  /* %, -1 = it only says roughly (level) */
    char level[16];   /* "Full", "High", "Normal", "Low", "Critical", "" */
    char status[16];  /* "Charging", "Discharging", "Full", ... */
    int low;          /* low now (health / alerts) */
} devbat_t;
const char *devbat_text(const devbat_t *b, char *out, size_t n); /* "45%", "Low", "80%, charging" */

typedef struct {
    int nrapl;
    rapl_t z[MAX_RAPL];
    int rapl_denied; /* zones exist but energy_uj is root-only */
    int nbat;
    battery_t bat[4];
    char ac_path[PATHBUF];
    char usbc_path[4][PATHBUF]; /* no "Mains" supply: the USB-C ports that charge (type USB) */
    int nusbc;
    int ac_online; /* -1 = unknown */
    double pkg_w, core_w, uncore_w, dram_w, psys_w; /* -1 = unknown */
    /* the "psys" (whole platform) domain is left unconnected by some laptop
     * firmware: it then reads far less than the CPU package inside it */
    int psys_low, psys_bad;
    cpulim_t lim;  /* the CPU's power limits */
    /* a USB-C charger: what it offers (its PD source capabilities) and what
     * the laptop agreed on (the contract); -1 = unknown */
    double chg_offer_w;       /* the most it offers */
    char chg_volts[32];       /* its fixed voltages: "5/9/15/20 V" */
    double chg_w, chg_v, chg_a; /* the contract: W, V, A */
    int drain_told;           /* health: "drains while plugged in" told for this plug-in */
    double drain_since;       /* ... draining since (monotonic) */
    int draining;             /* ... for 30 s or more now */
    double drain_w;           /* ... by this much */
    int ndev;
    devbat_t dev[MAX_DEVBAT]; /* wireless devices' batteries: they come and go */
    double dev_t;             /* looked for at (monotonic), every 10 s */
    char slow[40]; /* the part that took longest to read this time, for slow-reading events */
    double slow_s;
} power_t;
int charger_weak(const power_t *p);  /* a laptop on a USB-C charger that gives less than 25 W */

void power_init(power_t *p);
void power_sample(power_t *p, double dt);

/* ---------------------------------------------------------------- PCIe */
/* the link a PCI device runs at, against what it and its slot can do */
typedef struct {
    int gen, width;           /* now: PCIe generation, lanes */
    int dev_gen, dev_width;   /* the most the device can do */
    int slot_gen, slot_width; /* the most the slot (the port above it) can do, 0 = unknown */
    int best_gen, best_width; /* the most both can do */
    int below;                /* slower than both allow: a riser, a bad contact, a BIOS setting */
    int slot_less;            /* the slot can't do what the device can (by design: a chipset slot) */
} pcilink_t;
int pcie_link(const char *dev, pcilink_t *l); /* dev: its sysfs folder; 0 = known */
const char *pcie_text(const pcilink_t *l, char *out, size_t n); /* "PCIe 3.0 x2: the slot's most (...)" */

/* --------------------------------------------------------------- Health */
#define MAX_PKGS 8
#define MAX_PCIDEV 64
#define MAX_NVME 8
#define MAX_EVENTS 200

enum { EV_INFO, EV_WARN, EV_ERR };

/* Something worth knowing about: a kernel error message, throttling,
 * an out-of-memory kill, a new PCIe error... */
typedef struct {
    double t;          /* wall clock, seconds since the epoch */
    unsigned long seq; /* increasing, to find events newer than a point */
    int level;         /* EV_* */
    char cat[10];      /* "thermal", "memory", "disk", "gpu", "pcie", "crash", "kernel"... */
    char text[200];
    int rep;           /* the same line came this many times in a row ("... (3 times)") */
    double t_rep;      /* when it came last */
} event_t;

/* pressure stall information: share of time tasks waited for a resource */
typedef struct {
    int ok, has_full, primed;
    double some, full; /* % of the last sample */
    double avg10, avg60, avg300, favg10, favg60, favg300; /* kernel's averages of "some" / "full" */
    unsigned long long p_some, p_full;
} psi_t;

typedef struct {
    int pkg, cpu;                 /* package id, and a CPU in it to read from */
    unsigned long long count, ms; /* package throttling since boot: times, total ms */
    unsigned long long p_ms, p_count;
    double pct;                   /* % of the last sample spent throttled */
} thr_pkg_t;

typedef struct {
    char dev[16], name[80];
    unsigned long long cor, nonfatal, fatal; /* AER error counts since boot */
} pcidev_t;

typedef struct {
    char name[16], model[48];
    int state;    /* 1 read, 0 not yet, -1 needs root, -2 not supported */
    unsigned crit; /* critical warning bits (0 = fine) */
    double temp_c;
    int spare, spare_thr, used; /* % */
    unsigned long long hours, cycles, unsafe, media_err, err_log, warn_min, crit_min;
    double read_b, written_b;
    /* slowing itself down from heat, counted by the drive since new: [0] a little
     * (thermal management temperature 1), [1] hard (temperature 2) */
    unsigned long long heat_n[2], heat_s[2]; /* times, seconds */
    int heat_open, heat_hard;                /* slowing down now (or lately); it got to the hard kind */
    double heat_from, heat_t, heat_max_c;    /* wall clock: first and last time seen; hottest meanwhile */
    unsigned long long heat_n0[2], heat_s0[2]; /* the counters when it began */
    pcilink_t link;          /* its PCIe link (no root needed) */
    int link_ok, link_low, link_told; /* known; slower than it could be (reads in a row); told */
} nvme_t;

#define MAX_SATA 8

/* a SATA SSD or hard disk, from its SMART attributes */
typedef struct {
    char name[16], model[48];
    int state;        /* 1 read, 0 not yet, -1 needs root, -2 no SMART data, -3 asleep, not woken,
                       * -4 a USB drive not read yet (only when you ask: D, then r) */
    int rotational;   /* a hard disk */
    int usb;          /* in a USB enclosure: read only when you ask */
    int ask;          /* ... and you asked: read it at the next chance */
    int nvme_bridge;  /* an NVMe drive in a USB adapter: its health needs the maker's own commands */
    double read_at;   /* wall clock of the last good read */
    int asleep;       /* spun down at the last check (the figures are from before) */
    int failing;      /* an attribute is at or below the maker's limit */
    char fail_attr[28];
    int temp_c, wear; /* wear: % of the rated life used (SSDs), -1 = unknown */
    long long hours, cycles, realloc, pending, uncorrect, crc, unsafe; /* -1 = unknown */
} sata_t;

/* ------------------------------------------- RAID arrays and pools */
#define MAX_ARRAYS 16
#define MAX_MEMBERS 16
enum { ARR_MD = 1, ARR_ZFS, ARR_BTRFS };
enum { BE_WRITE, BE_READ, BE_FLUSH, BE_CORRUPT, BE_GEN, BE_N }; /* btrfs's error counters */

typedef struct {
    char name[32];  /* sda1; a btrfs device's name, or "device 2" */
    int id;         /* md: its slot (-1 spare); btrfs: devid */
    int bad;        /* md: faulty; btrfs: missing */
    int spare;      /* md: a spare (or the disk being rebuilt onto) */
    long long err[BE_N]; /* btrfs: its error counters; md: [BE_READ] read errors it corrected */
} member_t;

typedef struct {
    int kind;                  /* ARR_* */
    char key[48];              /* md0, the pool's name, the btrfs file system's UUID */
    char name[64];             /* md0, the pool, the btrfs mount point (or label) */
    char level[24];            /* md: raid1, raid5...; btrfs: the data profile (raid1, single, dup...) */
    char meta[16];             /* btrfs: the metadata profile */
    char state[24];            /* md: array_state (clean, active, inactive, broken...); ZFS: ONLINE, DEGRADED... */
    int ndisks;                /* md: raid_disks; btrfs: devices */
    int degraded;              /* md: disks missing or failed; btrfs: devices missing; ZFS: not ONLINE */
    int nmem;
    member_t mem[MAX_MEMBERS];
    char sync[16];             /* md: sync_action (idle, resync, recover, check, repair, reshape, frozen) */
    double sync_pct, sync_kbs; /* how far, how fast (KB/s); -1 = unknown */
    double sync_left_s;        /* time left at that pace, -1 = unknown */
    long long mismatch;        /* md: sectors the last check found different between the copies */
    long long errs;            /* btrfs: all error counters together */
    int seen;                  /* found at the last scan */
    /* health's memory */
    int told_bad;              /* "degraded" told */
    char told_sync[16];        /* the sync_action last seen ("" = not yet) */
    double sync_from;          /* when the running sync was first seen (wall) */
    int primed;
    struct {
        int id;
        long long err[BE_N];
    } prev[MAX_MEMBERS];       /* btrfs: the counters last seen, for events */
    int nprev;
} array_t;

typedef struct {
    int n;
    array_t a[MAX_ARRAYS];
    int nbad;                  /* degraded, failed or not ONLINE now */
} arrays_t;

void arrays_sample(arrays_t *A);
/* what's wrong with it, if anything: 2 failed or degraded, 1 a rebuild or
 * errors to know about, 0 fine; the text says it in a few words */
int array_problem(const array_t *a, char *out, size_t n);
const char *array_level(const array_t *a, char *out, size_t n); /* "RAID 5", "ZFS pool", "btrfs, raid1" */

/* ------------------------------------------- ECC memory errors (EDAC) */
#define MAX_EDAC_DIMMS 32
typedef struct {
    int ok;                    /* an EDAC memory controller is registered (the kernel counts the errors) */
    char mode[16];             /* SECDED, S4ECD4ED... ("" = unknown) */
    char driver[32];           /* the controller: "F19h", "ie31200"... */
    long long ce, ue;          /* corrected / uncorrected since boot, all controllers */
    int ndimm;
    struct {
        char label[48];
        long long ce, ue;
    } dimm[MAX_EDAC_DIMMS];
    long long ce_seen, ue_seen; /* health: the counts already told about */
    long long dce_seen[MAX_EDAC_DIMMS], due_seen[MAX_EDAC_DIMMS];
    int nseen, primed;
} ecc_t;

void ecc_sample(ecc_t *e);

/* the batteries at one moment, for what they lost while the computer slept */
typedef struct {
    int ok;        /* there is a battery and it reported its charge */
    int charger;   /* plugged in (or charging / full) */
    double pct;    /* charge, % */
    double wh;     /* charge, Wh, -1 = unknown */
} batsnap_t;

typedef struct {
    psi_t psi[3]; /* cpu, memory, io */

    int thr_ok;   /* the CPU reports thermal throttling counters */
    int nthr;
    thr_pkg_t thr[MAX_PKGS];
    unsigned long long thr_count, core_count; /* since boot: package / core throttling events */
    double thr_ms, thr_pct; /* total package throttled time since boot; worst % last sample */
    double thr_last;        /* wall clock of the last throttling seen, 0 = none since start */
    unsigned long long thr_start; /* thr_count + core_count when hsm started, plus what they jumped
                                   * by while the computer slept and woke (not throttling) */
    int thr_primed;               /* the counters were read once */

    unsigned long long oom_start; /* oom_kill when hsm started */
    double oom_last;

    int aer_ok, npci;
    pcidev_t pci[MAX_PCIDEV];
    unsigned long long pci_cor, pci_nonfatal, pci_fatal;
    double pci_last;

    int nnvme;
    nvme_t nvme[MAX_NVME];
    int nsata;
    sata_t sata[MAX_SATA];
    arrays_t arr;              /* software RAID arrays, ZFS pools, btrfs file systems */
    ecc_t ecc;                 /* memory errors ECC found */

    unsigned long long files, files_max; /* open file handles, whole system */

    int kmsg;                  /* 1 reading the kernel log, -1 needs root, 0 not available */
    unsigned kmsg_err, kmsg_warn; /* relevant messages since boot */
    double kmsg_last;          /* wall clock of the newest error, 0 = none */

    int nev, ev_head;
    unsigned long ev_seq;
    event_t ev[MAX_EVENTS];

    double hot_last; /* a temperature reached its critical level */
    double slow_last, lowmem_last; /* a disk was very slow / memory ran low (alert limits) */
    double full_last;              /* a file system passed the "almost full" alert limit */

    /* freezes: the whole system stood still (watchdog thread); suspends */
    int freezes;
    double freeze_max, freeze_last, freeze_t; /* longest, latest (s), when it was noticed (wall) */
    double frozen_total;                      /* seconds frozen since start */
    double asleep_total, resume_t;            /* time suspended since start, when it last woke (wall) */
    int suspends;
    int link_drops;                           /* network links lost since start */
    int fan_stops;                            /* fans that stopped while hot */
    double fan_last, link_last, ro_last;      /* a fan is stopped / a link dropped / a disk went read-only */
    double ulim_last;                         /* a sensor is past the limit you gave it */

    /* the CPU held back by its power limit rather than by heat (needs the
     * package's power, so root) */
    int pl_known;                 /* the package's power and its limits are known */
    int pl_now;                   /* PL_LONG at its sustained limit, PL_SHORT at its boost limit, 0 not */
    double pl_held_s, pl_busy_s;  /* seconds held back / seconds busy since start */

    char slow[40];  /* the check that took longest this time, and how long */
    double slow_s;

    /* private */
    int kmsg_fd, kmsg_file, primed, thr_tried;
    long kmsg_off;
    unsigned long long oom_prev;
    unsigned long long core_ms[MAX_CPUS], core_cnt[MAX_CPUS];
    double next_slow, next_nvme, next_sata, thr_ev_t;
    char governor[24];
    int susp_ok;                   /* the kernel counts failed suspends */
    unsigned long long susp_fail;
    double pl_since, pl_last, pl_ev_t; /* the stretch began / last at the limit (mono) / last event (wall) */
    int pl_kind, pl_told;
    int nq_seen[NQ_COUNT], nq_open[NQ_COUNT]; /* network check: troubles told about / still going */
    batsnap_t bat_prev;            /* the batteries at the previous sample */
    struct {                       /* the "woke up" event waits a few seconds for the battery to settle */
        int on;
        double dur, wall, due;     /* how long it slept, when it went to sleep (wall), when to tell */
        batsnap_t pre;             /* the batteries just before */
    } wake;
} health_t;

void health_init(health_t *h);
void health_event(health_t *h, int level, const char *cat, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
const event_t *health_event_at(const health_t *h, int i); /* 0 = newest, NULL past the end */
/* what an event means, in plain words, for the ones that need it; 0 = none */
int event_explain(const char *cat, const char *text, char *out, size_t n);
/* active problems for the header: throttling now, OOM kill just now... "" = none */
int health_alert(const health_t *h, char *out, size_t n);
/* the busiest process right now, for event texts: " while firefox used 180% CPU" ("" = none) */
enum { BLAME_CPU, BLAME_MEM, BLAME_IO, BLAME_GPU };
enum { PL_LONG = 1, PL_SHORT = 2 }; /* which power limit holds the CPU back */
void health_blame(const procs_t *ps, int what, char *out, size_t n);

/* freeze.c: a watchdog thread that notices when the whole system stops */
enum { FZ_STALL, FZ_SUSPEND };
typedef struct {
    double wall, dur; /* when it started, how long */
    int kind;
} fz_t;
void freeze_start(void);
int freeze_take(fz_t *out); /* 1 = got one */
/* seconds the computer slept since the last call (0 = it didn't), for the
 * main loop: a sample that spans a suspend knows it at once */
double freeze_slept(void);

/* leak.c: processes whose memory keeps growing (leak_sample is with sys_t below) */
#define LEAK_MIN 120 /* minutes of memory history kept per process */
/* the memory history of one process, oldest first (MB a minute); returns
 * how many; *growing = it was flagged, *rate = MB per hour lately */
int leak_history(pid_t pid, unsigned long long start, float *out, int max, int *growing, double *rate);
int leak_growing(pid_t pid, unsigned long long start);
#define RUNAWAY_MIN 15 /* minutes at a whole core before a process is called out */
int runaway_minutes(pid_t pid, unsigned long long start, int *flagged); /* minutes in a row busy */
int runaway_count(void);       /* called out and still busy */

/* ata.c: SATA drives */
void ata_find(health_t *h);
void ata_sample(health_t *h);
void ata_usb_scan(health_t *h);            /* USB drives plugged in or out */
int ata_usb_read(health_t *h, const char *name); /* read a USB drive's health now (you asked); 0 or errno */

/* selftest.c: a drive's own tests and error log */
#define MAX_STEST 21
typedef struct {
    int type;          /* 1 short, 2 extended, 3 other */
    int failed, stopped;
    char what[48];     /* "completed without error", "FAILED: read element"... */
    long long hours;   /* the drive's power-on hours when it ran */
    long long lba;     /* first bad sector, -1 = none */
} stest_t;

typedef struct {
    char name[16], model[48];
    int nvme;
    int state;               /* 1 read, 0 not yet, -1 needs root, -2 not supported */
    int can_test;
    int running, progress;   /* a test going on now: 1 short, 2 extended; % done (-1 unknown) */
    int short_min, ext_min;  /* how long the tests take, minutes, -1 = unknown */
    int ntest;
    stest_t test[MAX_STEST]; /* newest first */
    long long hours;         /* power-on hours now */
    long long errors;        /* errors the drive logged in its life, -1 unknown */
    char err_text[96];       /* the latest one */
    int err_bad;             /* ... was a real problem (media error), not a refused command */
    long long err_hours;     /* when (power-on hours), -1 unknown */
    double read_t;
    int usb;                 /* in a USB enclosure: read only when you ask (r) */
    int follow;              /* hsm started a test: report when it ends */
    double follow_t;
    int sim_type;            /* test hook: a pretend test */
    double sim_until;
} drive_t;

int drives_sync(const health_t *h); /* the drives health found; returns how many */
drive_t *drive_at(int i);
int drive_read(drive_t *d);          /* self-test log, status, error log; 0 = read */
int drive_start(drive_t *d, int type); /* 1 short, 2 extended, 0 stop it; 0 or errno */
void drives_tick(health_t *h);       /* events for the tests hsm started */
int ata_smart(int fd, unsigned char feature, unsigned char lba_low, unsigned char *buf); /* 0 or errno */
void sata_summary(const sata_t *s, char *out, size_t n, int *level, const char *sep, const char *deg);

/* ------------------------------------------------- Services (cgroup v2) */
enum { SVC_SERVICE, SVC_SCOPE, SVC_CONTAINER, SVC_SESSION, SVC_APP, SVC_OTHER };

typedef struct {
    char path[256];  /* relative to the cgroup root: "system.slice/nginx.service" */
    char name[80];   /* shown: "nginx", "docker 3f2a1b2c3d4e" */
    int kind;        /* SVC_* */
    uid_t uid;       /* owner for user units, 0 = system */
    double cpu;      /* % of one CPU, like top */
    long long mem;   /* bytes, -1 = n/a */
    double rd_bps, wr_bps;
    long tasks;      /* -1 = n/a */
    unsigned long long oom; /* processes the OOM killer took from it */

    unsigned long long p_usage, p_rd, p_wr;
    int primed, seen;
} svc_t;

typedef struct {
    int n, cap, state; /* state: 1 ok, 0 not looked yet, -1 no cgroup v2 */
    svc_t *s;
    char root[PATHBUF];
    double t, next_scan, want_until;
} svcs_t;

void svcs_sample(svcs_t *v); /* only does work while want_until is in the future */
void svcs_free(svcs_t *v);
const char *svc_kind_name(int kind);

/* the firmware and what it switched on: UEFI and Secure Boot, the TPM, the
 * CPU's virtualisation (VT-x / AMD-V) and the IOMMU */
enum { VIRT_UNKNOWN, VIRT_ON, VIRT_NOKVM, VIRT_OFF, VIRT_MAYBE_OFF, VIRT_NONE, VIRT_VM };
typedef struct {
    int done;
    int uefi;            /* 1 UEFI, 0 legacy BIOS, -1 can't tell (ARM boards) */
    int efi_bits;        /* the firmware's own: 64, 32 (some tablets); 0 = unknown */
    int secure_boot;     /* 1 on, 0 off, -1 unknown */
    int setup_mode;      /* 1: no keys enrolled, anything boots; -1 unknown */
    char lockdown[16];   /* the kernel's lockdown: none, integrity, confidentiality; "" = unknown */
    int tpm;             /* 20 = TPM 2.0, 12 = TPM 1.2, 1 = one, version unknown, 0 = none found */
    int virt;            /* 'v' VT-x, 's' AMD-V, 'a' ARM, 0 = none */
    int virt_state;      /* VIRT_* */
    int kvm;             /* /dev/kvm is there */
    int vm;              /* this is a virtual machine */
    int iommu;           /* IOMMU units on; 0 = off */
    char iommu_kind[16]; /* "VT-d", "AMD-Vi", "SMMU" */
    int iommu_groups;
} firmware_t;
void firmware_sample(firmware_t *f, const cpu_t *c);  /* once (the KVM part every time) */
void virt_text(const firmware_t *f, char *out, size_t n); /* "VT-x on, KVM ready" */
void boot_text(const firmware_t *f, char *out, size_t n); /* "UEFI, Secure Boot on" */
void iommu_text(const firmware_t *f, char *out, size_t n); /* "on (VT-d), 18 groups" */

/* --------------------------------------------------------- Whole system */
/* a memory module, from the SMBIOS tables (root only) */
#define MAX_RAM 16
typedef struct {
    char slot[32], bank[32];  /* where: "ChannelA-DIMM0", "BANK 0" */
    char type[8];             /* "DDR4", "DDR5", "LPDDR5"... */
    char maker[32], part[32];
    unsigned long mb;
    int speed, conf;          /* MT/s: the most it can do, what it runs at; 0 = unknown */
    int form;                 /* 1 DIMM, 2 SO-DIMM, 3 soldered, 0 unknown */
    char channel;             /* 'A', 'B'...; 0 = can't tell */
    int total_w, data_w;      /* bits wide, with and without the check bits (72 / 64: an ECC module); 0 = unknown */
} ramdev_t;

/* the machine, as its firmware describes it (/sys/class/dmi/id, readable by
 * everyone; serial numbers are root-only and left out) */
typedef struct {
    int ok;
    char vendor[64], model[96], type[64]; /* type: the maker's code when the model is a name (Lenovo) */
    char board[96];
    char bios_vendor[64], bios_version[64];
    int bios_date;                        /* YYYYMMDD, 0 = unknown */
    int chassis;                          /* SMBIOS chassis type: 3 desktop, 10 notebook, 35 mini PC...; 0 = unknown */
    int ram_state;                        /* memory modules: 1 read, -1 needs root, 0 not reported */
    int nram, ram_slots;                  /* modules in, slots there are */
    int ram_channels;                     /* channels in use: 1 = single channel; 0 = can't tell */
    int ram_soldered;                     /* soldered memory (laptops): nothing to move */
    int ram_ecc;                          /* the board's error correction (SMBIOS type 16): 3 none, 4 parity,
                                           * 5 single-bit ECC, 6 multi-bit ECC, 7 CRC; 0 = unknown */
    ramdev_t ram[MAX_RAM];
} machine_t;

/* ECC on, from EDAC or the firmware's tables: 1 on, 0 off, -1 can't tell; why says it in a few words */
int ecc_state(const ecc_t *e, const machine_t *m, char *why, size_t n);
void smbios_memory(machine_t *m); /* the memory modules (root) */
int machine_desktop(const machine_t *m); /* a desktop's chassis (or a tower, mini PC, all-in-one) */
int machine_laptop(const machine_t *m, const power_t *p); /* a laptop's or a tablet's (or a battery inside) */

/* ------------------------------------------- what keeps a drive busy */
typedef struct {
    char mount[128], dev[96], type[16];
    int usb;                 /* on a USB drive */
    int nproc, more;         /* processes using it: listed / past the list */
    int swap, loops, nested; /* swap files on it, loop devices from files on it, file systems mounted inside it */
    pid_t lastp;             /* private */
} drvfs_t;
typedef struct {
    pid_t pid;
    uid_t uid;
    char comm[24];
    int fs;                  /* the drvfs_t it is about */
    int cwd, root, exe;      /* its working folder / root / program is there */
    int nfiles, maps;        /* files it has open there / mapped in (libraries) */
    char file[160];          /* the first of them */
} drvuse_t;
/* who uses each file system (on request: it reads every process's files);
 * returns how many drvuse_t; *denied = processes that couldn't be checked */
int busy_scan(drvfs_t *fs, int nfs, drvuse_t *out, int max, int *denied);

/* ------------------------------------------------------------------ USB */
#define MAX_USB 40

/* a USB device: what it is, the speed it runs at and where it's plugged in */
typedef struct {
    char id[20];       /* where: "1-2" (bus 1, port 2), "3-1.4" (port 4 of a hub in port 1) */
    char name[64];     /* "SanDisk Ultra", "Logitech USB Receiver", or "0781:5581" */
    char kind[16];     /* "storage", "keyboard", "mouse", "camera", "audio", "hub", "Bluetooth"... */
    char blk[16];      /* the disk it gives, "sdb" (storage) */
    unsigned vid, pid;
    int devnum;        /* a new one each time it connects */
    double mbps;       /* the speed it runs at: 1.5, 12, 480, 5000, 10000, 20000 */
    int can;           /* the most it can do, Mb/s (its BOS descriptor), 0: unknown */
    int port3;         /* the port it's in can do USB 3: 1 yes, 0 USB 2 only, -1 unknown */
    int where;         /* 1 an outside socket, 2 built in, 3 behind a hub, 0 unknown */
    int depth;         /* hubs on the way */
    int slow;          /* it can do USB 3 but runs at USB 2 speed */
    int ma;            /* the power it asks for, mA */
    double seen;       /* wall clock: when it came (0: there when hsm started) */
    int told;          /* its "connected" event is out (or it needs none) */
} usbdev_t;

typedef struct {
    int ok; /* the USB tree can be read */
    int n;
    usbdev_t d[MAX_USB];
    unsigned long long sig; /* the names and device numbers, to notice a change cheaply */
    int primed, nslow;
    double next_blk;
    int ngone;
    struct {
        usbdev_t d;
        double t;
    } gone[8]; /* just disconnected: told after 3 s, unless it comes back meanwhile */
} usb_t;

typedef struct {
    char host[64], kernel[64], os[96], arch[24];
    machine_t mach;
    firmware_t fw;
    pcibus_t pcibus;
    double t_last, dt;
    unsigned long samples;
    /* the computer slept since the sample before: for how long (0 = it
     * didn't), when it woke (monotonic); how many times and how long in
     * all since hsm started */
    double slept, woke_t, asleep;
    unsigned sleeps;

    cpu_t cpu;
    mem_t mem;
    disks_t disks;
    net_t net;
    procs_t procs;
    sensors_t sens;
    gpus_t gpus;
    power_t power;
    health_t health;
    svcs_t svc;
    usb_t usb;

    double total_w; /* best whole-machine power estimate, -1 = unknown (never a partial sum) */
    const char *total_w_src;
    const char *total_why; /* why it's unknown: "needs root" (the CPU part), "" */
    int pw_partial;        /* the power history holds only the GPUs (no total known) */
    double part_w;         /* ... and what they draw */

    hist_t h_cpu, h_mem, h_gpu[MAX_GPUS], h_dr, h_dw, h_rx, h_tx, h_pw, h_temp;
    double next_slow;

    /* a reading that took a second or more: reported once the next sample
     * shows the whole system didn't stand still meanwhile (that's a freeze) */
    char slow_what[96];
    double slow_dur;
    int slow_frz, slow_susp;
    struct {
        char what[96];
        double t;
    } slow_seen[8]; /* each one at most every 10 minutes */
} sys_t;

void sys_init(sys_t *s);
void sys_sample(sys_t *s);
void sys_free(sys_t *s);
void health_sample(sys_t *s); /* after the other collectors: needs CPU, memory and sensors */
void usb_sample(sys_t *s);    /* after health_sample: its events, quiet after a resume */
const char *usb_speed(double mbps, char *out, size_t n); /* "USB 2 480M", "USB 3 10G" */
void leak_sample(sys_t *s);   /* once a minute, from health_sample */
void runaway_sample(sys_t *s); /* once a minute, from health_sample */
void health_close(health_t *h);

#endif
