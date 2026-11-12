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

#define MAX_CSTATES 12
typedef struct {
    char name[16];      /* "C1", "C6", "POLL" */
    double pct;         /* average share of time all CPUs spent in it */
    unsigned latency;   /* exit latency in us */
    int disabled;
} cstate_t;

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

    /* power settings (cpufreq / intel_pstate / ACPI platform profile); "" = n/a */
    char driver[24], governor[24], epp[40], pstate[16], profile[32];
    int turbo;                    /* 1 on, 0 off, -1 unknown */
    double lim_min, lim_max;      /* scaling limits in MHz, -1 = unknown */
    double hw_min, hw_max;        /* what the hardware can do, MHz */
    /* idle states (cpuidle), averaged over all CPUs */
    int nidle;
    cstate_t idle[MAX_CSTATES];
    double wakeups; /* idle-state entries per second, about the number of wake-ups */

    /* private */
    cpu_jiffies_t prev_all, prev[MAX_CPUS];
    unsigned long long prev_ctxt, prev_intr, prev_forks;
    int primed, freq_src;
    unsigned long long idle_time[MAX_CPUS][MAX_CSTATES], idle_use[MAX_CPUS][MAX_CSTATES];
    double idle_t;
    int idle_primed;
} cpu_t;

void cpu_init(cpu_t *c);
void cpu_sample(cpu_t *c, double dt);
void cpu_power_sample(cpu_t *c); /* governor, turbo...: slow-changing, every few seconds */

/* --------------------------------------------------------------- Memory */
typedef struct {
    unsigned long long total, free, avail, buffers, cached, sreclaim, shmem;
    unsigned long long swap_total, swap_free, dirty, writeback;
    unsigned long long used, buffcache, swap_used; /* derived */
    double swapin_bps, swapout_bps, majflt_rate;
    unsigned long long oom_kill; /* processes killed for lack of memory since boot */
    int oom_ok;                  /* the kernel reports oom_kill */

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
    double rd_lat, wr_lat; /* average time per read / write in ms over the last sample */
    unsigned inflight;     /* requests in progress right now */

    unsigned long long p_rsec, p_wsec, p_rio, p_wio, p_ticks, p_rms, p_wms;
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
    unsigned long long errors, drops; /* since boot, both directions */
    double err_rate, drop_rate;       /* per second */
    double signal_dbm;                /* Wi-Fi signal, 0 = n/a */
    int quality;                      /* Wi-Fi link quality %, -1 = n/a */
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
    int nfds;                   /* open files, -1 = unknown (other user's process) */
    double gpu;                 /* GPU busy %, -1 = not using a GPU / unknown */
    unsigned long long gpu_mem; /* GPU memory in bytes */

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
} procs_t;

void procs_init(procs_t *ps);
void procs_sample(procs_t *ps, double dt, unsigned long long memtotal);
void procs_free(procs_t *ps);
const char *proc_cmdline(proc_t *p);
int proc_send_signal(pid_t pid, unsigned long long start, int sig); /* 0 or errno */
const char *uid_name(uid_t uid);

/* ------------------------------------------------------ Sensors (hwmon) */
enum { SENS_TEMP, SENS_FAN, SENS_POWER, SENS_VOLT };
#define MAX_SENSORS 256

typedef struct {
    int type, chip_idx, ok;
    char chip[32], label[48], path[PATHBUF];
    double value, crit; /* °C / RPM / W / V ; crit -1 = none */
    double peak, fmax;  /* highest value seen; fans: fanN_max if exposed */
    double lo, hi;      /* voltages: the chip's min / max limits, 0 = none */
    double hot_t;       /* health: when a "too hot" event was last raised */
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
    unsigned long long nv_ts; /* NVML per-process samples already seen */
} gpu_t;

typedef struct {
    int n;
    gpu_t g[MAX_GPUS];
} gpus_t;

void gpus_init(gpus_t *g);
void gpus_sample(gpus_t *g, double dt);
void gpus_close(gpus_t *g);
void gpus_proc_sample(gpus_t *g, procs_t *ps); /* NVIDIA: per-process GPU use through NVML */
int pci_lookup_name(unsigned vendor, unsigned device, char *out, size_t n); /* from pci.ids */

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
    double health;                     /* full charge now vs. new, %, -1 = unknown */
    long cycles;                       /* charge cycles, -1 = unknown */
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
} nvme_t;

typedef struct {
    psi_t psi[3]; /* cpu, memory, io */

    int thr_ok;   /* the CPU reports thermal throttling counters */
    int nthr;
    thr_pkg_t thr[MAX_PKGS];
    unsigned long long thr_count, core_count; /* since boot: package / core throttling events */
    double thr_ms, thr_pct; /* total package throttled time since boot; worst % last sample */
    double thr_last;        /* wall clock of the last throttling seen, 0 = none since start */
    unsigned long long thr_start; /* thr_count + core_count when hsm started */

    unsigned long long oom_start; /* oom_kill when hsm started */
    double oom_last;

    int aer_ok, npci;
    pcidev_t pci[MAX_PCIDEV];
    unsigned long long pci_cor, pci_nonfatal, pci_fatal;
    double pci_last;

    int nnvme;
    nvme_t nvme[MAX_NVME];

    unsigned long long files, files_max; /* open file handles, whole system */

    int kmsg;                  /* 1 reading the kernel log, -1 needs root, 0 not available */
    unsigned kmsg_err, kmsg_warn; /* relevant messages since boot */
    double kmsg_last;          /* wall clock of the newest error, 0 = none */

    int nev, ev_head;
    unsigned long ev_seq;
    event_t ev[MAX_EVENTS];

    double hot_last; /* a temperature reached its critical level */

    /* private */
    int kmsg_fd, kmsg_file, primed, thr_tried;
    long kmsg_off;
    unsigned long long oom_prev;
    unsigned long long core_ms[MAX_CPUS], core_cnt[MAX_CPUS];
    double next_slow, next_nvme, thr_ev_t;
    char governor[24];
} health_t;

void health_init(health_t *h);
void health_event(health_t *h, int level, const char *cat, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
const event_t *health_event_at(const health_t *h, int i); /* 0 = newest, NULL past the end */
/* active problems for the header: throttling now, OOM kill just now... "" = none */
int health_alert(const health_t *h, char *out, size_t n);

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
    health_t health;
    svcs_t svc;

    double total_w; /* best whole-machine power estimate, -1 = unknown */
    const char *total_w_src;

    hist_t h_cpu, h_mem, h_gpu[MAX_GPUS], h_dr, h_dw, h_rx, h_tx, h_pw, h_temp;
    double next_slow;
} sys_t;

void sys_init(sys_t *s);
void sys_sample(sys_t *s);
void sys_free(sys_t *s);
void health_sample(sys_t *s); /* after the other collectors: needs CPU, memory and sensors */
void health_close(health_t *h);

#endif
