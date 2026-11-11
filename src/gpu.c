// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hardware-stats - live monitor for CPU, GPU, memory, power, disks,
 * network and processes on Linux
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

/* gpu.c - GPU load, memory, clocks, temperature and power.
 *
 *   Intel  (i915): engine busy % via the i915 perf PMU (same source as
 *                  intel_gpu_top), clocks from sysfs, iGPU power from RAPL
 *   AMD  (amdgpu): gpu_busy_percent, VRAM, hwmon temp/power/clock/fan
 *   NVIDIA       : NVML, loaded at runtime with dlopen() so the program builds and
 *                  runs without the NVIDIA driver installed
 *   others       : whatever the driver's hwmon exposes (nouveau, xe, ...)
 */
#define _GNU_SOURCE
#include "collect.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* --------------------------------------------------------- NVML (dlopen) */
typedef int nvret_t;
typedef struct nvmlDevice_st *nvdev_t;
typedef struct {
    unsigned int gpu, memory;
} nvutil_t;
typedef struct {
    unsigned long long total, free, used;
} nvmem_t;

static void *nv_lib;
static nvret_t (*nv_init)(void);
static nvret_t (*nv_shutdown)(void);
static nvret_t (*nv_count)(unsigned int *);
static nvret_t (*nv_handle)(unsigned int, nvdev_t *);
static nvret_t (*nv_name)(nvdev_t, char *, unsigned int);
static nvret_t (*nv_util)(nvdev_t, nvutil_t *);
static nvret_t (*nv_mem)(nvdev_t, nvmem_t *);
static nvret_t (*nv_temp)(nvdev_t, int, unsigned int *);
static nvret_t (*nv_power)(nvdev_t, unsigned int *);
static nvret_t (*nv_plimit)(nvdev_t, unsigned int *);
static nvret_t (*nv_clock)(nvdev_t, int, unsigned int *);
static nvret_t (*nv_maxclock)(nvdev_t, int, unsigned int *);
static nvret_t (*nv_fan)(nvdev_t, unsigned int *);

#define NV_SYM(var, name) (*(void **)(&(var)) = dlsym(nv_lib, name))

static int nvml_load(void)
{
    nv_lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nv_lib)
        nv_lib = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    if (!nv_lib)
        return -1;
    NV_SYM(nv_init, "nvmlInit_v2");
    NV_SYM(nv_shutdown, "nvmlShutdown");
    NV_SYM(nv_count, "nvmlDeviceGetCount_v2");
    NV_SYM(nv_handle, "nvmlDeviceGetHandleByIndex_v2");
    NV_SYM(nv_name, "nvmlDeviceGetName");
    NV_SYM(nv_util, "nvmlDeviceGetUtilizationRates");
    NV_SYM(nv_mem, "nvmlDeviceGetMemoryInfo");
    NV_SYM(nv_temp, "nvmlDeviceGetTemperature");
    NV_SYM(nv_power, "nvmlDeviceGetPowerUsage");
    NV_SYM(nv_plimit, "nvmlDeviceGetEnforcedPowerLimit");
    NV_SYM(nv_clock, "nvmlDeviceGetClockInfo");
    NV_SYM(nv_maxclock, "nvmlDeviceGetMaxClockInfo");
    NV_SYM(nv_fan, "nvmlDeviceGetFanSpeed");
    if (!nv_init || !nv_count || !nv_handle || nv_init() != 0) {
        dlclose(nv_lib);
        nv_lib = NULL;
        return -1;
    }
    return 0;
}

static void nvml_sample(gpu_t *g)
{
    nvdev_t d = (nvdev_t)g->nvml;
    unsigned int u;
    nvutil_t ut;
    nvmem_t mm;
    if (nv_util && !nv_util(d, &ut))
        g->util = ut.gpu;
    if (nv_mem && !nv_mem(d, &mm)) {
        g->mem_used = (double)mm.used;
        g->mem_total = (double)mm.total;
    }
    if (nv_temp && !nv_temp(d, 0 /* NVML_TEMPERATURE_GPU */, &u))
        g->temp_c = u;
    if (nv_power && !nv_power(d, &u))
        g->power_w = u / 1000.0; /* mW */
    if (nv_plimit && !nv_plimit(d, &u))
        g->power_cap_w = u / 1000.0;
    if (nv_clock && !nv_clock(d, 0 /* graphics */, &u))
        g->clk_mhz = u;
    if (nv_clock && !nv_clock(d, 2 /* memory */, &u))
        g->mem_clk_mhz = u;
    if (nv_maxclock && !nv_maxclock(d, 0, &u))
        g->clk_max_mhz = u;
    if (nv_fan && !nv_fan(d, &u))
        g->fan_pct = u;
}

/* ------------------------------------------------------ PCI name lookup */
static int pci_name(unsigned vendor, unsigned device, char *out, size_t n)
{
    const char *files[] = { secure_getenv("HWSTATS_PCI_IDS"), "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                            "/usr/share/pci.ids", "/usr/local/share/pci.ids" };
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !f; i++)
        if (files[i])
            f = fopen(files[i], "re");
    if (!f)
        return -1;
    char line[512];
    int in_vendor = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (line[0] != '\t') {
            if (in_vendor)
                break;
            unsigned v;
            if (sscanf(line, "%4x", &v) == 1 && v == vendor)
                in_vendor = 1;
            continue;
        }
        if (!in_vendor || line[1] == '\t')
            continue;
        unsigned dv;
        if (sscanf(line + 1, "%4x", &dv) == 1 && dv == device) {
            char *name = line + 5;
            while (*name == ' ')
                name++;
            name[strcspn(name, "\n")] = '\0';
            /* "CoffeeLake-S GT2 [UHD Graphics 630]" -> "UHD Graphics 630" */
            char *lb = strchr(name, '['), *rb = lb ? strrchr(lb, ']') : NULL;
            if (lb && rb) {
                *rb = '\0';
                name = lb + 1;
            }
            str_copy(out, n, name);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/* --------------------------------------------------- Intel i915 perf PMU */
static long perf_open(int type, unsigned long long config, int cpu)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = (unsigned)type;
    a.size = sizeof a;
    a.config = config;
    return syscall(SYS_perf_event_open, &a, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC);
}

static void intel_perf_init(gpu_t *g)
{
    char pmu[PATHBUF], p[PATHBUF], b[128], alt[48];
    /* discrete cards get "i915_0000_03_00.0", the integrated GPU plain "i915" */
    sfmt(alt, sizeof alt, "i915_%s", g->pci);
    for (char *q = alt; *q; q++)
        if (*q == ':')
            *q = '_';
    const char *cands[2] = { alt, g->integrated ? "i915" : NULL };
    long long type = -1;
    for (int i = 0; i < 2 && type < 0; i++) {
        if (!cands[i])
            continue;
        sfmt(pmu, sizeof pmu, "%s/bus/event_source/devices/%s", g_sysfs, cands[i]);
        sfmt(p, sizeof p, "%s/type", pmu);
        if (read_ll(p, &type))
            type = -1;
    }
    if (type < 0) {
        str_copy(g->note, sizeof g->note, "n/a (i915 perf PMU not found)");
        return;
    }
    int cpu = 0;
    sfmt(p, sizeof p, "%s/cpumask", pmu);
    if (!read_line(p, b, sizeof b))
        cpu = atoi(b);

    static const struct {
        const char *ev, *label;
    } evs[] = { { "rcs0-busy", "3D" },  { "vcs0-busy", "Video" },  { "vecs0-busy", "VidEnh" },
                { "bcs0-busy", "Blit" }, { "ccs0-busy", "Compute" } };
    for (size_t i = 0; i < sizeof evs / sizeof evs[0] && g->neng < MAX_ENG; i++) {
        sfmt(p, sizeof p, "%s/events/%s", pmu, evs[i].ev);
        if (read_line(p, b, sizeof b))
            continue;
        char *cfg = strstr(b, "config=");
        if (!cfg)
            continue;
        long fd = perf_open((int)type, strtoull(cfg + 7, NULL, 0), cpu);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM)
                str_copy(g->note, sizeof g->note, "needs root (i915 perf counters)");
            else
                str_copy(g->note, sizeof g->note, "n/a (perf_event_open failed)");
            break;
        }
        gpu_engine_t *e = &g->eng[g->neng++];
        str_copy(e->name, sizeof e->name, evs[i].label);
        e->fd = (int)fd;
    }
}

static void intel_perf_sample(gpu_t *g)
{
    double t = now_sec(), dt = t - g->eng_t, mx = 0;
    for (int i = 0; i < g->neng; i++) {
        gpu_engine_t *e = &g->eng[i];
        unsigned long long v;
        if (read(e->fd, &v, sizeof v) != (ssize_t)sizeof v)
            continue;
        /* counters are nanoseconds the engine was busy */
        if (g->eng_t > 0 && dt > 0 && v >= e->prev)
            e->pct = CLAMP((double)(v - e->prev) / (dt * 1e9) * 100.0, 0.0, 100.0);
        e->prev = v;
        if (e->pct > mx)
            mx = e->pct;
    }
    g->eng_t = t;
    g->util = mx;
}

/* ------------------------------------------------------------ sysfs GPUs */
static int rd(const char *dir, const char *name, long long *v)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/%s", dir, name);
    return read_ll(p, v);
}

static double first_ll(const char *dir, const char *const *names)
{
    long long v;
    for (int i = 0; names[i]; i++)
        if (!rd(dir, names[i], &v))
            return (double)v;
    return -1;
}

static void link_base(const char *path, char *out, size_t n)
{
    char t[PATHBUF];
    ssize_t l = readlink(path, t, sizeof t - 1);
    if (l <= 0) {
        out[0] = '\0';
        return;
    }
    t[l] = '\0';
    const char *b = strrchr(t, '/');
    str_copy(out, n, b ? b + 1 : t);
}

static void find_hwmon(gpu_t *g)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/hwmon", g->dev);
    DIR *d = opendir(p);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (!strncmp(e->d_name, "hwmon", 5)) {
            sfmt(g->hwmon, sizeof g->hwmon, "%s/%s", p, e->d_name);
            break;
        }
    closedir(d);
}

static double amd_max_sclk(const gpu_t *g)
{
    char p[PATHBUF], b[1024];
    sfmt(p, sizeof p, "%s/pp_dpm_sclk", g->dev);
    if (read_file(p, b, sizeof b) <= 0)
        return -1;
    double mx = -1;
    char *cur = b, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *c = strchr(line, ':');
        if (c) {
            double v = atof(c + 1);
            if (v > mx)
                mx = v;
        }
    }
    return mx;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void add_sysfs_gpus(gpus_t *gs, int nvml_ok)
{
    char base[PATHBUF], p[PATHBUF];
    sfmt(base, sizeof base, "%s/class/drm", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    int ids[32], nid = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nid < 32) {
        int id, len = 0;
        /* "card0" but not connectors such as "card0-HDMI-A-1" */
        if (sscanf(e->d_name, "card%d%n", &id, &len) == 1 && e->d_name[len] == '\0')
            ids[nid++] = id;
    }
    closedir(d);
    qsort(ids, (size_t)nid, sizeof ids[0], cmp_int);

    for (int i = 0; i < nid && gs->n < MAX_GPUS; i++) {
        gpu_t *g = &gs->g[gs->n];
        memset(g, 0, sizeof *g);
        sfmt(g->card, sizeof g->card, "card%d", ids[i]);
        sfmt(g->drm, sizeof g->drm, "%s/%s", base, g->card);
        sfmt(g->dev, sizeof g->dev, "%s/device", g->drm);
        unsigned ven, dev;
        sfmt(p, sizeof p, "%s/vendor", g->dev);
        if (read_hex(p, &ven))
            continue; /* simpledrm / non-PCI framebuffer */
        sfmt(p, sizeof p, "%s/device", g->dev);
        if (read_hex(p, &dev))
            dev = 0;
        link_base(g->dev, g->pci, sizeof g->pci);
        sfmt(p, sizeof p, "%s/driver", g->dev);
        link_base(p, g->driver, sizeof g->driver);

        const char *vname = ven == 0x8086 ? "Intel" : ven == 0x1002 ? "AMD" : ven == 0x10de ? "NVIDIA" : "GPU";
        str_copy(g->vendor, sizeof g->vendor, vname);
        if (ven == 0x10de && nvml_ok && !strcmp(g->driver, "nvidia"))
            continue; /* reported through NVML instead */

        char pn[80];
        if (pci_name(ven, dev, pn, sizeof pn) == 0)
            sfmt(g->name, sizeof g->name, "%s %s", vname, pn);
        else
            sfmt(g->name, sizeof g->name, "%s GPU [%04x:%04x]", vname, ven, dev);

        g->clk_max_mhz = -1;
        if (ven == 0x8086) {
            g->integrated = !strncmp(g->pci, "0000:00:", 8);
        } else if (ven == 0x1002) {
            long long vram;
            sfmt(p, sizeof p, "%s/mem_info_vram_total", g->dev);
            /* APUs only get a small carve-out as "VRAM" */
            g->integrated = !read_ll(p, &vram) && vram > 0 && vram < (2LL << 30);
            g->clk_max_mhz = amd_max_sclk(g);
        }
        find_hwmon(g);

        if (!strcmp(g->driver, "i915"))
            intel_perf_init(g);
        else if (!strcmp(g->driver, "xe"))
            str_copy(g->note, sizeof g->note, "not yet supported for the xe driver");
        else if (!strcmp(g->driver, "nvidia"))
            str_copy(g->note, sizeof g->note, "install NVIDIA's libnvidia-ml (NVML) for stats");
        else if (!strcmp(g->driver, "nouveau"))
            str_copy(g->note, sizeof g->note, "nouveau driver exposes no load info");

        if (!strcmp(g->driver, "i915")) {
            static const char *const mx[] = { "gt_RP0_freq_mhz", "gt/gt0/rps_RP0_freq_mhz", "gt_max_freq_mhz", NULL };
            g->clk_max_mhz = first_ll(g->drm, mx);
        } else if (!strcmp(g->driver, "xe")) {
            static const char *const mx[] = { "tile0/gt0/freq0/max_freq", NULL };
            g->clk_max_mhz = first_ll(g->dev, mx);
        }
        gs->n++;
    }
}

void gpus_init(gpus_t *gs)
{
    int nvml_ok = nvml_load() == 0;
    add_sysfs_gpus(gs, nvml_ok);
    if (nvml_ok) {
        unsigned int cnt = 0;
        if (nv_count(&cnt) != 0)
            cnt = 0;
        for (unsigned int i = 0; i < cnt && gs->n < MAX_GPUS; i++) {
            nvdev_t h;
            if (nv_handle(i, &h) != 0)
                continue;
            gpu_t *g = &gs->g[gs->n++];
            memset(g, 0, sizeof *g);
            g->nvml = h;
            str_copy(g->vendor, sizeof g->vendor, "NVIDIA");
            str_copy(g->driver, sizeof g->driver, "nvidia");
            sfmt(g->card, sizeof g->card, "nvml%u", i);
            if (!nv_name || nv_name(h, g->name, sizeof g->name) != 0)
                sfmt(g->name, sizeof g->name, "NVIDIA GPU %u", i);
            if (strncmp(g->name, "NVIDIA", 6)) {
                char tmp[96];
                str_copy(tmp, sizeof tmp, g->name);
                sfmt(g->name, sizeof g->name, "NVIDIA %s", tmp);
            }
        }
    }
}

static void sysfs_sample(gpu_t *g)
{
    long long v;
    if (!strcmp(g->driver, "amdgpu")) {
        if (!rd(g->dev, "gpu_busy_percent", &v))
            g->util = (double)v;
        if (!rd(g->dev, "mem_info_vram_used", &v))
            g->mem_used = (double)v;
        if (!rd(g->dev, "mem_info_vram_total", &v))
            g->mem_total = (double)v;
    }
    if (g->neng) {
        intel_perf_sample(g);
    } else if (!strcmp(g->driver, "i915")) {
        /* Without the (root-only) perf counters, estimate load from the time
         * the GPU did NOT spend in its RC6 sleep state. World-readable. */
        static const char *const rc6[] = { "power/rc6_residency_ms", "gt/gt0/rc6_residency_ms", NULL };
        double ms = first_ll(g->drm, rc6), t = now_sec();
        if (ms >= 0) {
            if (g->rc6_t > 0 && t > g->rc6_t && (unsigned long long)ms >= g->rc6_prev) {
                double idle = ((double)((unsigned long long)ms - g->rc6_prev) / 1000.0) / (t - g->rc6_t);
                g->util = CLAMP(100.0 * (1.0 - idle), 0.0, 100.0);
                g->util_est = 1;
            }
            g->rc6_prev = (unsigned long long)ms;
            g->rc6_t = t;
        }
    }

    if (!strcmp(g->driver, "i915")) {
        static const char *const act[] = { "gt_act_freq_mhz", "gt/gt0/rps_act_freq_mhz", "gt_cur_freq_mhz", NULL };
        g->clk_mhz = first_ll(g->drm, act);
    } else if (!strcmp(g->driver, "xe")) {
        static const char *const act[] = { "tile0/gt0/freq0/act_freq", "tile0/gt0/freq0/cur_freq", NULL };
        g->clk_mhz = first_ll(g->dev, act);
    }

    if (g->hwmon[0]) {
        const char *h = g->hwmon;
        if (!rd(h, "temp1_input", &v))
            g->temp_c = (double)v / 1000.0;
        if (!rd(h, "power1_average", &v) || !rd(h, "power1_input", &v)) {
            g->power_w = (double)v / 1e6;
        } else if (!rd(h, "energy1_input", &v)) { /* Intel Arc / xe: microjoules */
            double t = now_sec();
            if (g->p_energy_t > 0 && (unsigned long long)v >= g->p_energy && t > g->p_energy_t)
                g->power_w = (double)((unsigned long long)v - g->p_energy) / 1e6 / (t - g->p_energy_t);
            g->p_energy = (unsigned long long)v;
            g->p_energy_t = t;
        }
        if (!rd(h, "power1_cap", &v) && v > 0)
            g->power_cap_w = (double)v / 1e6;
        if (g->clk_mhz < 0 && !rd(h, "freq1_input", &v))
            g->clk_mhz = (double)v / 1e6; /* Hz */
        if (!rd(h, "freq2_input", &v))
            g->mem_clk_mhz = (double)v / 1e6;
        if (!rd(h, "fan1_input", &v))
            g->fan_rpm = (double)v;
        if (!rd(h, "pwm1", &v)) {
            long long mx = 255;
            if (rd(h, "pwm1_max", &mx) || mx <= 0)
                mx = 255;
            g->fan_pct = 100.0 * (double)v / (double)mx;
        }
    }
}

void gpus_sample(gpus_t *gs, double dt)
{
    (void)dt;
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        g->util = g->mem_used = g->mem_total = -1;
        g->temp_c = g->power_w = g->power_cap_w = g->clk_mhz = g->mem_clk_mhz = g->fan_pct = g->fan_rpm = -1;
        if (g->nvml)
            nvml_sample(g);
        else
            sysfs_sample(g);
    }
}

void gpus_close(gpus_t *gs)
{
    for (int i = 0; i < gs->n; i++)
        for (int j = 0; j < gs->g[i].neng; j++)
            close(gs->g[i].eng[j].fd);
    if (nv_lib) {
        if (nv_shutdown)
            nv_shutdown();
        dlclose(nv_lib);
        nv_lib = NULL;
    }
}
