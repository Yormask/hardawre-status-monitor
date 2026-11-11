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

/* ------------------------------------------------------------------ CPU */
#define MAX_CPUS 512

typedef struct {
    double user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_jiffies_t;

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
    double total, user, sys, iowait, steal;
    double mhz_avg, mhz_max;
    double temp_c; /* package temperature, -1 = unknown */
    double load[3], uptime;
    unsigned procs_running, procs_blocked;
    double ctxt_rate, intr_rate, fork_rate;

    /* private */
    cpu_jiffies_t prev_all, prev[MAX_CPUS];
    unsigned long long prev_ctxt, prev_intr, prev_forks;
    int primed, freq_src;
} cpu_t;

void cpu_init(cpu_t *c);
void cpu_sample(cpu_t *c, double dt);

/* --------------------------------------------------------------- Memory */
typedef struct {
    unsigned long long total, free, avail, buffers, cached, sreclaim, shmem;
    unsigned long long swap_total, swap_free, dirty, writeback;
    unsigned long long used, buffcache, swap_used; /* derived */
    double swapin_bps, swapout_bps, majflt_rate;

    unsigned long long p_swpin, p_swpout, p_majflt;
    int primed;
} mem_t;

void mem_sample(mem_t *m, double dt);

/* -------------------------------------------------------------- Storage */
#define MAX_DISKS 64
#define MAX_FS 64

typedef struct {
    char name[32], model[64], kind[8];
    unsigned long long size, rd_total, wr_total; /* bytes */
    double rd_bps, wr_bps, rd_iops, wr_iops, util;

    unsigned long long p_rsec, p_wsec, p_rio, p_wio, p_ticks;
    int primed, seen;
} disk_t;

typedef struct {
    char mount[128], dev[96], type[16];
    unsigned long long total, used, avail;
} fs_t;

typedef struct {
    int n;
    disk_t d[MAX_DISKS];
    int nfs;
    fs_t fs[MAX_FS];
    double rd_bps, wr_bps;
} disks_t;

void disks_sample(disks_t *ds, double dt);
void fs_sample(disks_t *ds);

/* -------------------------------------------------------------- Network */
#define MAX_NETIF 64

typedef struct {
    char name[32], state[16], addr[48];
    int up, is_virtual, wireless, speed; /* speed in Mb/s, -1 = unknown */
    unsigned long long rx_total, tx_total;
    double rx_bps, tx_bps;
    int primed, seen;
} netif_t;

typedef struct {
    int n;
    netif_t i[MAX_NETIF];
    double rx_bps, tx_bps;
} net_t;

void net_sample(net_t *n, double dt);
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
} proc_t;

typedef struct {
    proc_t *p, *prev; /* both sorted by pid */
    int n, cap, nprev, prevcap;
    int threads, running, sleeping, diskwait, zombie, stopped, io_denied;
    long hz, pagesize;
    void *dir; /* DIR* on /proc, kept open between samples */
} procs_t;

void procs_init(procs_t *ps);
void procs_sample(procs_t *ps, double dt, unsigned long long memtotal);
void procs_free(procs_t *ps);
const char *proc_cmdline(proc_t *p);
int proc_send_signal(pid_t pid, unsigned long long start, int sig); /* 0 or errno */
const char *uid_name(uid_t uid);

/* ------------------------------------------------------ Sensors (hwmon) */
enum { SENS_TEMP, SENS_FAN, SENS_POWER };
#define MAX_SENSORS 256

typedef struct {
    int type, chip_idx, ok;
    char chip[32], label[48], path[PATHBUF];
    double value, crit; /* °C / RPM / W ; crit -1 = none */
    double peak, fmax;  /* highest value seen; fans: fanN_max if exposed */
} sensor_t;

typedef struct {
    int n;
    sensor_t s[MAX_SENSORS];
    int cpu_idx; /* sensor used as "CPU temperature", -1 = none */
} sensors_t;

void sensors_init(sensors_t *s);
void sensors_sample(sensors_t *s);
double sensors_cpu_temp(const sensors_t *s);
double sensors_core_temp(const sensors_t *s, int pkg, int core); /* coretemp "Core N", -1 */
void sensors_chip_name(const char *chip, char *out, size_t n); /* "nvme#2" -> "NVMe 2" */

/* ------------------------------------------------------------------ GPU */
#define MAX_GPUS 8
#define MAX_ENG 5

typedef struct {
    char name[8];
    int fd;
    unsigned long long prev;
    double pct;
} gpu_engine_t;

typedef struct {
    char name[96], vendor[8], driver[16], card[16], pci[24], note[80];
    int integrated;
    double util;                /* busiest engine %, -1 = unknown */
    double mem_used, mem_total; /* bytes, -1 = unknown            */
    double temp_c, power_w, power_cap_w, clk_mhz, clk_max_mhz, mem_clk_mhz, fan_pct, fan_rpm;
    int neng;
    gpu_engine_t eng[MAX_ENG]; /* Intel i915 per-engine busy (perf PMU) */
    double eng_t;
    int util_est;              /* util is an estimate (Intel: from RC6 idle time) */
    unsigned long long rc6_prev;
    double rc6_t;

    /* private */
    char dev[PATHBUF], hwmon[PATHBUF], drm[PATHBUF];
    unsigned long long p_energy;
    double p_energy_t;
    void *nvml; /* nvmlDevice_t when the GPU is driven through NVML */
} gpu_t;

typedef struct {
    int n;
    gpu_t g[MAX_GPUS];
} gpus_t;

void gpus_init(gpus_t *g);
void gpus_sample(gpus_t *g, double dt);
void gpus_close(gpus_t *g);

/* ---------------------------------------------------------------- Power */
#define MAX_RAPL 16

typedef struct {
    char name[32], path[PATHBUF];
    int depth, readable, primed;
    unsigned long long max_range, prev;
    double watts;
} rapl_t;

typedef struct {
    char name[16], status[24], path[PATHBUF];
    double watts, capacity, time_left; /* -1 = unknown; time_left in s */
} battery_t;

typedef struct {
    int nrapl;
    rapl_t z[MAX_RAPL];
    int rapl_denied; /* zones exist but energy_uj is root-only */
    int nbat;
    battery_t bat[4];
    char ac_path[PATHBUF];
    int ac_online; /* -1 = unknown */
    double pkg_w, core_w, uncore_w, dram_w, psys_w; /* -1 = unknown */
} power_t;

void power_init(power_t *p);
void power_sample(power_t *p, double dt);

/* --------------------------------------------------------- Whole system */
typedef struct {
    char host[64], kernel[64], os[96], arch[24];
    double t_last, dt;
    unsigned long samples;

    cpu_t cpu;
    mem_t mem;
    disks_t disks;
    net_t net;
    procs_t procs;
    sensors_t sens;
    gpus_t gpus;
    power_t power;

    double total_w; /* best whole-machine power estimate, -1 = unknown */
    const char *total_w_src;

    hist_t h_cpu, h_mem, h_gpu[MAX_GPUS], h_dr, h_dw, h_rx, h_tx, h_pw, h_temp;
    double next_slow;
} sys_t;

void sys_init(sys_t *s);
void sys_sample(sys_t *s);
void sys_free(sys_t *s);

#endif
