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
#include <fcntl.h>
#include <linux/perf_event.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
typedef struct { /* nvmlProcessUtilizationSample_t */
    unsigned int pid;
    unsigned long long ts;
    unsigned int sm, mem, enc, dec;
} nvpsample_t;

typedef struct { /* nvmlPciInfo_t (v3) */
    char bus_legacy[16];
    unsigned int domain, bus, device, pci_id, pci_sub;
    char bus_id[32];
} nvpci_t;

static void *nv_lib;
static int nv_live; /* nvmlInit done: the handles are valid */
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
static nvret_t (*nv_putil)(nvdev_t, nvpsample_t *, unsigned int *, unsigned long long);
static nvret_t (*nv_pci)(nvdev_t, nvpci_t *);
static nvret_t (*nv_reasons)(nvdev_t, unsigned long long *);
static nvret_t (*nv_thresh)(nvdev_t, int, unsigned int *);
static nvret_t (*nv_enc)(nvdev_t, unsigned int *, unsigned int *);
static nvret_t (*nv_dec)(nvdev_t, unsigned int *, unsigned int *);
static nvret_t (*nv_lgen)(nvdev_t, unsigned int *);
static nvret_t (*nv_lwidth)(nvdev_t, unsigned int *);
static nvret_t (*nv_lgen_max)(nvdev_t, unsigned int *);
static nvret_t (*nv_lwidth_max)(nvdev_t, unsigned int *);

#define NV_SYM(var, name) (*(void **)(&(var)) = dlsym(nv_lib, name))

/* why the clock is held back (nvmlClocksEventReasons) */
#define NVR_SW_POWER 0x4ULL
#define NVR_SW_THERMAL 0x20ULL
#define NVR_HW_THERMAL 0x40ULL
#define NVR_HW_POWER_BRAKE 0x80ULL

/* the library only: starting NVML wakes every NVIDIA GPU, so that waits */
static int nvml_lib(void)
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
    NV_SYM(nv_putil, "nvmlDeviceGetProcessUtilization");
    NV_SYM(nv_pci, "nvmlDeviceGetPciInfo_v3");
    NV_SYM(nv_reasons, "nvmlDeviceGetCurrentClocksEventReasons"); /* driver 535 on */
    if (!nv_reasons)
        NV_SYM(nv_reasons, "nvmlDeviceGetCurrentClocksThrottleReasons"); /* its old name */
    NV_SYM(nv_thresh, "nvmlDeviceGetTemperatureThreshold");
    NV_SYM(nv_enc, "nvmlDeviceGetEncoderUtilization");
    NV_SYM(nv_dec, "nvmlDeviceGetDecoderUtilization");
    NV_SYM(nv_lgen, "nvmlDeviceGetCurrPcieLinkGeneration");
    NV_SYM(nv_lwidth, "nvmlDeviceGetCurrPcieLinkWidth");
    NV_SYM(nv_lgen_max, "nvmlDeviceGetMaxPcieLinkGeneration");
    NV_SYM(nv_lwidth_max, "nvmlDeviceGetMaxPcieLinkWidth");
    if (!nv_init || !nv_count || !nv_handle) {
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
    unsigned int per; /* the encoder's and decoder's sampling period: not needed */
    if (nv_enc && !nv_enc(d, &u, &per) && u <= 100)
        g->enc_pct = u;
    if (nv_dec && !nv_dec(d, &u, &per) && u <= 100)
        g->dec_pct = u;
    /* the link: idle cards drop to a slower one to save power, then speed up again */
    if (nv_lgen && !nv_lgen(d, &u) && u > 0 && u < 10)
        g->pcie_gen = (int)u;
    if (nv_lwidth && !nv_lwidth(d, &u) && u > 0 && u <= 32)
        g->pcie_width = (int)u;
    if (!g->pcie_gen_max && nv_lgen_max && !nv_lgen_max(d, &u) && u > 0 && u < 10)
        g->pcie_gen_max = (int)u;
    if (!g->pcie_width_max && nv_lwidth_max && !nv_lwidth_max(d, &u) && u > 0 && u <= 32)
        g->pcie_width_max = (int)u;
    unsigned long long r;
    g->reasons = 0;
    if (nv_reasons && !nv_reasons(d, &r)) {
        g->reasons = r;
        g->reasons_ok = 1;
    }
    if (g->power_w >= 0)
        g->last_power = g->power_w;
}

/* ------------------------------------------------------ PCI name lookup */
FILE *pci_ids_open(void)
{
    const char *files[] = { secure_getenv("HSM_PCI_IDS"), "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
                            "/usr/share/pci.ids", "/usr/local/share/pci.ids" };
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof files / sizeof files[0] && !f; i++)
        if (files[i])
            f = fopen(files[i], "re");
    return f;
}

int pci_lookup_name(unsigned vendor, unsigned device, char *out, size_t n)
{
    FILE *f = pci_ids_open();
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

/* a binary file, whole (sysfs binary attributes) */
static ssize_t read_bin(const char *path, unsigned char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size);
    close(fd);
    return n;
}

/* AMD: why the GPU runs below full speed, from gpu_metrics. Layouts 1.3
 * (graphics cards) and 2.2 to 2.4 (APUs) carry the "independent throttle
 * status", the same bits on every chip: 0-31 power and current limits
 * (PPT, TDC, EDC), 32-47 temperatures (edge, hotspot, memory, VRs, and VRHOT
 * and PROCHOT, 44-47, where the hardware steps in), 56 peak power. Fields
 * the firmware doesn't fill in read as all ones. Returns GS_* or -1. */
static int amd_reasons(const gpu_t *g, int *hw)
{
    unsigned char b[512];
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/gpu_metrics", g->dev);
    ssize_t n = read_bin(p, b, sizeof b);
    if (n < 4)
        return -1;
    unsigned size = (unsigned)b[0] | (unsigned)b[1] << 8, fmt = b[2], content = b[3];
    size_t off = fmt == 1 && content == 3 ? 112 : fmt == 2 && content >= 2 && content <= 4 ? 120 : 0;
    if (!off || (size_t)n < off + 8 || size < off + 8)
        return -1;
    unsigned long long st = 0;
    for (int k = 7; k >= 0; k--) /* little-endian, whatever this machine is */
        st = st << 8 | b[off + (size_t)k];
    if (st == ~0ULL)
        return -1;
    *hw = (st & (0xFULL << 44)) != 0;
    return ((st & 0xFFFFFFFFULL) || (st & (1ULL << 56)) ? GS_POWER : 0) | ((st & 0xFFFF00000000ULL) ? GS_HEAT : 0);
}

/* Intel: i915 (Ice Lake and newer) has gt/gt0/throttle_reason_*, xe
 * tile0/gt0/freq0/throttle/reason_*, each 0 or 1, and a status that is 1
 * while any of them holds the clock back. Returns GS_* or -1. */
static int intel_reasons(const gpu_t *g, int *hw)
{
    char dir[PATHBUF], f[48];
    const char *pre;
    long long v;
    if (!strcmp(g->driver, "i915")) {
        sfmt(dir, sizeof dir, "%s/gt/gt0", g->drm);
        pre = "throttle_reason_";
    } else if (!strcmp(g->driver, "xe")) {
        sfmt(dir, sizeof dir, "%s/tile0/gt0/freq0/throttle", g->dev);
        pre = "reason_";
    } else {
        return -1;
    }
    sfmt(f, sizeof f, "%sstatus", !strcmp(g->driver, "i915") ? pre : "");
    if (rd(dir, f, &v))
        return -1;
    *hw = 0;
    if (!v)
        return 0;
    static const char *const heat[] = { "thermal", "prochot", "ratl", "vr_thermalert", "vr_thermal", "soc_thermal",
                                        "mem_thermal", "soc_avg_thermal", NULL };
    static const char *const power[] = { "pl1", "pl2", "pl4", "vr_tdc", "iccmax", "pwrbrk", "psys_pl1", "psys_pl2",
                                         "psys_crit", "fastvmode", NULL };
    int bits = 0;
    for (int k = 0; heat[k]; k++) {
        sfmt(f, sizeof f, "%s%s", pre, heat[k]);
        if (!rd(dir, f, &v) && v) {
            bits |= GS_HEAT;
            *hw |= !strcmp(heat[k], "prochot") || !strcmp(heat[k], "vr_thermalert");
        }
    }
    for (int k = 0; power[k]; k++) {
        sfmt(f, sizeof f, "%s%s", pre, power[k]);
        if (!rd(dir, f, &v) && v)
            bits |= GS_POWER;
    }
    return bits;
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

/* GPUs of SoCs (ARM boards, phones' chips in laptops): no PCI, known by their driver ("vc4-drm", "msm_drm") */
static const struct {
    const char *driver, *vendor, *name;
} SOC_GPU[] = { { "vc4", "BCM", "Broadcom VideoCore (vc4)" },   { "v3d", "BCM", "Broadcom V3D" },
                { "panfrost", "ARM", "Arm Mali (panfrost)" },   { "panthor", "ARM", "Arm Mali (panthor)" },
                { "lima", "ARM", "Arm Mali (lima)" },           { "msm", "QCOM", "Qualcomm Adreno (msm)" },
                { "asahi", "Apple", "Apple GPU (asahi)" },      { "etnaviv", "Vivante", "Vivante GPU (etnaviv)" },
                { "tegra", "NVIDIA", "NVIDIA Tegra" },          { "rockchip", "RK", "Rockchip display" },
                { "meson", "Amlogic", "Amlogic display" },      { "sun4i", "AW", "Allwinner display" },
                { "mediatek", "MTK", "MediaTek display" },      { "imx", "NXP", "i.MX display" },
                { "exynos", "Samsung", "Exynos display" } };

/* the vendor of a graphics device that isn't Intel, AMD or NVIDIA (virtual machines, servers) */
static const char *other_vendor(unsigned ven)
{
    static const struct {
        unsigned id;
        const char *name;
    } V[] = { { 0x1af4, "Virtio" }, { 0x15ad, "VMware" },  { 0x1234, "QEMU" },  { 0x1b36, "QXL" },
              { 0x80ee, "VBox" },   { 0x1013, "Cirrus" },  { 0x102b, "Matrox" }, { 0x1a03, "ASPEED" },
              { 0x1414, "Hyper-V" }, { 0x1d17, "Zhaoxin" }, { 0x1ed5, "Moore" },  { 0x5333, "S3" },
              { 0x1106, "VIA" },    { 0x18ca, "XGI" },     { 0x1de1, "Tekram" } };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++)
        if (V[i].id == ven)
            return V[i].name;
    return "GPU";
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
        if (read_hex(p, &ven)) { /* not PCI: an SoC's GPU, or simpledrm (the firmware's framebuffer) */
            sfmt(p, sizeof p, "%s/driver", g->dev);
            link_base(p, g->driver, sizeof g->driver);
            for (size_t k = 0; k < sizeof SOC_GPU / sizeof SOC_GPU[0]; k++)
                if (!strncmp(g->driver, SOC_GPU[k].driver, strlen(SOC_GPU[k].driver))) {
                    str_copy(g->vendor, sizeof g->vendor, SOC_GPU[k].vendor);
                    str_copy(g->name, sizeof g->name, SOC_GPU[k].name);
                    str_copy(g->note, sizeof g->note, "this driver gives no load figures");
                    g->integrated = 1;
                    g->clk_max_mhz = -1;
                    find_hwmon(g);
                    gs->n++;
                    break;
                }
            continue;
        }
        sfmt(p, sizeof p, "%s/device", g->dev);
        if (read_hex(p, &dev))
            dev = 0;
        link_base(g->dev, g->pci, sizeof g->pci);
        sfmt(p, sizeof p, "%s/driver", g->dev);
        link_base(p, g->driver, sizeof g->driver);

        const char *vname = ven == 0x8086 ? "Intel" : ven == 0x1002 ? "AMD" : ven == 0x10de ? "NVIDIA" : other_vendor(ven);
        str_copy(g->vendor, sizeof g->vendor, vname);
        if (ven == 0x10de && nvml_ok && !strcmp(g->driver, "nvidia"))
            continue; /* reported through NVML instead */

        char pn[80];
        if (pci_lookup_name(ven, dev, pn, sizeof pn) == 0 && !strncasecmp(pn, vname, strlen(vname)))
            str_copy(g->name, sizeof g->name, pn); /* "Virtio 1.0 GPU", "ASPEED Graphics Family" */
        else if (pci_lookup_name(ven, dev, pn, sizeof pn) == 0)
            sfmt(g->name, sizeof g->name, "%s %s", vname, pn);
        else if (strcmp(vname, "GPU"))
            sfmt(g->name, sizeof g->name, "%s GPU [%04x:%04x]", vname, ven, dev);
        else
            sfmt(g->name, sizeof g->name, "GPU [%04x:%04x]", ven, dev);

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

/* ---------------------------------------------- NVIDIA: letting it sleep
 *
 * A laptop's NVIDIA GPU powers itself off when nothing uses it (runtime D3),
 * and back on when something does - including every NVML query. Asking it
 * for its load once a second would keep it on for good, at 5-10 W. So:
 *   - whether it's asleep comes from its PCI device in sysfs, which doesn't
 *     wake it; a GPU that's asleep at start isn't touched at all;
 *   - after 10 s at 0% load, NVML lets go of it, so it can power off;
 *   - when something wakes it (a game), NVML picks it up again;
 *   - if it stays on without hsm (an external screen on it, a program that
 *     holds it), NVML stays too, and tries letting go again later. */

#define NV_IDLE_LET_GO 10.0 /* s at 0% load before NVML lets go */
#define NV_WAIT_SLEEP 20.0  /* s to wait for it to power off after that */

static void pm_read(const gpu_t *g, char *st, size_t n)
{
    st[0] = '\0';
    if (g->pm_path[0])
        read_line(g->pm_path, st, n);
}

/* NVIDIA GPUs on the nvidia driver, from sysfs (with or without a DRM card) */
static void add_nvidia_pci(gpus_t *gs)
{
    char base[PATHBUF], p[PATHBUF], b[128];
    sfmt(base, sizeof base, "%s/bus/pci/devices", g_sysfs);
    DIR *d = opendir(base);
    if (!d)
        return;
    char addr[MAX_GPUS][24];
    int na = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && na < MAX_GPUS) {
        if (e->d_name[0] == '.')
            continue;
        unsigned ven, cls;
        sfmt(p, sizeof p, "%s/%s/vendor", base, e->d_name);
        if (read_hex(p, &ven) || ven != 0x10de)
            continue;
        sfmt(p, sizeof p, "%s/%s/class", base, e->d_name);
        if (read_hex(p, &cls) || (cls >> 16) != 0x03) /* display controllers only (not its audio) */
            continue;
        char drv[32];
        sfmt(p, sizeof p, "%s/%s/driver", base, e->d_name);
        link_base(p, drv, sizeof drv);
        if (strcmp(drv, "nvidia"))
            continue;
        str_copy(addr[na++], sizeof addr[0], e->d_name);
    }
    closedir(d);
    for (int i = 1; i < na; i++) /* in bus order */
        for (int j = i; j > 0 && strcmp(addr[j], addr[j - 1]) < 0; j--) {
            char t[24];
            str_copy(t, sizeof t, addr[j]);
            str_copy(addr[j], sizeof addr[j], addr[j - 1]);
            str_copy(addr[j - 1], sizeof addr[j - 1], t);
        }
    for (int i = 0; i < na && gs->n < MAX_GPUS; i++) {
        gpu_t *g = &gs->g[gs->n++];
        memset(g, 0, sizeof *g);
        str_copy(g->vendor, sizeof g->vendor, "NVIDIA");
        str_copy(g->driver, sizeof g->driver, "nvidia");
        str_copy(g->pci, sizeof g->pci, addr[i]);
        sfmt(g->dev, sizeof g->dev, "%s/%s", base, addr[i]);
        unsigned dev = 0;
        sfmt(p, sizeof p, "%s/device", g->dev);
        read_hex(p, &dev);
        char pn[80];
        if (pci_lookup_name(0x10de, dev, pn, sizeof pn) == 0)
            sfmt(g->name, sizeof g->name, "NVIDIA %s", pn);
        else
            sfmt(g->name, sizeof g->name, "NVIDIA GPU [10de:%04x]", dev);
        g->clk_max_mhz = g->mem_total = g->temp_lim = g->temp_slow = g->temp_shut = -1;
        g->nv_state = NV_IDLE;
        /* may the kernel power it off? (power/control "auto", and the driver
         * didn't turn the feature off) */
        sfmt(g->pm_path, sizeof g->pm_path, "%s/power/runtime_status", g->dev);
        sfmt(p, sizeof p, "%s/power/control", g->dev);
        g->pm_auto = !read_line(p, b, sizeof b) && !strcmp(b, "auto") && path_exists(g->pm_path);
        sfmt(p, sizeof p, "%s/driver/nvidia/gpus/%s/power", g_procfs, addr[i]);
        char info[512];
        if (g->pm_auto && read_file(p, info, sizeof info) > 0) {
            char *rt = strstr(info, "Runtime D3 status:");
            if (rt) {
                rt[strcspn(rt, "\n")] = '\0';
                if (strstr(rt, "Disabled") || strstr(rt, "Not supported"))
                    g->pm_auto = 0;
            }
        }
        char st[32];
        pm_read(g, st, sizeof st);
        if (!strcmp(st, "suspended"))
            g->nv_state = NV_ASLEEP;
    }
}

static gpu_t *nv_match(gpus_t *gs, nvdev_t h, unsigned idx, unsigned cnt)
{
    nvpci_t pi;
    if (nv_pci && !nv_pci(h, &pi)) {
        char a[24];
        sfmt(a, sizeof a, "%04x:%02x:%02x.0", pi.domain & 0xffff, pi.bus & 0xff, pi.device & 0xff);
        for (int i = 0; i < gs->n; i++)
            if (gs->g[i].nv_state != NV_NONE && !strcasecmp(gs->g[i].pci, a))
                return &gs->g[i];
    }
    /* no PCI info: one NVIDIA GPU in sysfs and one in NVML are the same */
    int k = -1, nnv = 0;
    for (int i = 0; i < gs->n; i++)
        if (gs->g[i].nv_state != NV_NONE) {
            nnv++;
            k = i;
        }
    if (nnv == 1 && cnt == 1 && idx == 0 && !gs->g[k].nvml)
        return &gs->g[k];
    return NULL;
}

/* start NVML and give each NVIDIA GPU its handle; 0 = done */
static int nvml_up(gpus_t *gs)
{
    if (!nv_lib || nv_live)
        return nv_live ? 0 : -1;
    int r = nv_init();
    if (r != 0) { /* say why on the GPUs, instead of showing them idle */
        for (int i = 0; i < gs->n; i++)
            if (gs->g[i].nv_state != NV_NONE)
                sfmt(gs->g[i].note, sizeof gs->g[i].note, "%s",
                     r == 18  ? "NVML doesn't match the driver: reboot after a driver update"
                     : r == 9 ? "the NVIDIA driver isn't loaded"
                              : "NVML didn't start");
        return -1;
    }
    nv_live = 1;
    unsigned int cnt = 0;
    if (nv_count(&cnt) != 0)
        cnt = 0;
    for (unsigned int i = 0; i < cnt; i++) {
        nvdev_t h;
        if (nv_handle(i, &h) != 0)
            continue;
        gpu_t *g = nv_match(gs, h, i, cnt);
        if (!g) { /* not in sysfs (no PCI info there): NVML alone tells about it */
            if (gs->n >= MAX_GPUS)
                continue;
            g = &gs->g[gs->n++];
            memset(g, 0, sizeof *g);
            str_copy(g->vendor, sizeof g->vendor, "NVIDIA");
            str_copy(g->driver, sizeof g->driver, "nvidia");
            g->clk_max_mhz = g->mem_total = g->temp_lim = g->temp_slow = g->temp_shut = -1;
            g->name[0] = '\0';
        }
        g->nvml = h;
        g->nv_state = NV_ON;
        g->nv_idle_t = 0;
        g->note[0] = '\0';
        sfmt(g->card, sizeof g->card, "nvml%u", i);
        char nm[96];
        if (nv_name && nv_name(h, nm, sizeof nm) == 0 && nm[0]) { /* the driver's own name beats pci.ids */
            if (strncmp(nm, "NVIDIA", 6))
                sfmt(g->name, sizeof g->name, "NVIDIA %s", nm);
            else
                str_copy(g->name, sizeof g->name, nm);
        } else if (!g->name[0]) {
            sfmt(g->name, sizeof g->name, "NVIDIA GPU %u", i);
        }
        unsigned int t;
        if (nv_thresh && g->temp_lim < 0) { /* where it slows itself down, from the card's own table */
            if (!nv_thresh(h, 3 /* GPU_MAX: max operating */, &t) && t > 0 && t < 150)
                g->temp_lim = t;
            if (!nv_thresh(h, 1 /* SLOWDOWN: hardware */, &t) && t > 0 && t < 150)
                g->temp_slow = t;
            if (!nv_thresh(h, 0 /* SHUTDOWN */, &t) && t > 0 && t < 150)
                g->temp_shut = t;
            if (g->temp_lim < 0 && g->temp_slow > 0)
                g->temp_lim = g->temp_slow;
        }
    }
    return 0;
}

/* let go of NVML, so the GPUs can power off */
static void nvml_down(gpus_t *gs)
{
    if (!nv_live)
        return;
    double now = now_sec();
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        if (g->nv_state == NV_NONE)
            continue;
        g->nvml = NULL;
        g->nv_state = NV_IDLE;
        g->nv_wait_t = now;
        g->slow_now = 0;
        g->slow_since = 0;
    }
    if (nv_shutdown)
        nv_shutdown();
    nv_live = 0;
}

void gpus_init(gpus_t *gs)
{
    int lib = nvml_lib() == 0;
    add_sysfs_gpus(gs, lib);
    if (!lib)
        return;
    int first = gs->n;
    add_nvidia_pci(gs);
    /* asleep, every one of them: leave them be until something wakes them */
    int awake = 0;
    for (int i = first; i < gs->n; i++)
        awake |= gs->g[i].nv_state != NV_ASLEEP;
    if (awake || gs->n == first) /* or none in sysfs at all: NVML knows */
        nvml_up(gs);
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
    /* why it runs below full speed, where the driver says (NVIDIA's comes through NVML) */
    int hw = 0, bits = !strcmp(g->driver, "amdgpu") ? amd_reasons(g, &hw) : intel_reasons(g, &hw);
    g->sys_bits = bits > 0 ? bits : 0;
    g->slow_hw = bits > 0 && hw;
    if (bits >= 0)
        g->reasons_ok = 1;

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

/* held back by heat / by a power limit, out of the clock event reasons */
static int slow_bits(unsigned long long r)
{
    return ((r & (NVR_SW_THERMAL | NVR_HW_THERMAL)) ? GS_HEAT : 0) | ((r & (NVR_SW_POWER | NVR_HW_POWER_BRAKE)) ? GS_POWER : 0);
}

static void nv_policy(gpus_t *gs, double now)
{
    int any = 0, all_idle = 1, all_auto = 1;
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        if (g->nv_state == NV_NONE)
            continue;
        any = 1;
        all_auto &= g->pm_auto;
        if (g->nv_state == NV_ON) {
            if (g->util > 0 || g->util < 0)
                g->nv_idle_t = 0;
            else if (!g->nv_idle_t)
                g->nv_idle_t = now;
            all_idle &= g->nv_idle_t > 0 && now - g->nv_idle_t >= NV_IDLE_LET_GO;
        }
    }
    if (!any || !all_auto) /* can't power off: NVML stays, as it always did */
        return;
    int screen = 0; /* a screen on it: it can't power off, so letting go of it gains nothing */
    for (int i = 0; i < gs->n; i++) {
        char held[64];
        screen |= gpu_screen_holds(&gs->g[i], held, sizeof held)[0] != 0;
    }
    if (nv_live) {
        if (all_idle && now >= gs->nv_keep_until && !screen)
            nvml_down(gs);
        return;
    }
    /* let go of: asleep now, or woken up by something, or still on? */
    int wake = 0, stays_on = 0, held = 0;
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        if (g->nv_state == NV_NONE)
            continue;
        char st[32];
        pm_read(g, st, sizeof st);
        if (!strcmp(st, "suspended")) {
            if (g->nv_state != NV_ASLEEP)
                g->nv_fails = 0; /* it did power off */
            g->nv_state = NV_ASLEEP;
        } else if (g->nv_state == NV_ASLEEP) {
            wake = 1; /* a game, a video, another program: worth watching */
        } else if (screen) {
            held = 1; /* a screen keeps it on: no use waiting, and no failure either */
        } else if (now - g->nv_wait_t >= NV_WAIT_SLEEP) {
            stays_on = 1; /* something else keeps it on: hsm isn't the reason */
        }
    }
    if ((wake || stays_on || held) && now >= gs->nv_retry_t) {
        if (nvml_up(gs) != 0) {
            gs->nv_retry_t = now + 30;
            return;
        }
        int fails = 0;
        for (int i = 0; i < gs->n; i++)
            if (gs->g[i].nv_state != NV_NONE) {
                if (stays_on)
                    gs->g[i].nv_fails++;
                fails = MAX(fails, gs->g[i].nv_fails);
            }
        /* stays on anyway: try letting go again after 5, 10, 20, 40 min */
        gs->nv_keep_until = stays_on ? now + 300.0 * (1 << MIN(MAX(fails - 1, 0), 3)) : now;
    }
}

/* ------------------------------------------------------------- screens */

/* the base block of an EDID: the monitor's name and size */
static void edid_parse(const unsigned char *e, size_t n, screen_t *s)
{
    static const unsigned char head[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (n < 128 || memcmp(e, head, 8))
        return;
    if (e[21] && e[22]) /* cm */
        s->inch = sqrt((double)e[21] * e[21] + (double)e[22] * e[22]) / 2.54;
    for (int d = 54; d <= 108; d += 18) {
        const unsigned char *b = e + d;
        if (b[0] || b[1] || b[3] != 0xfc) /* a timing, or not the name */
            continue;
        char nm[16];
        int k = 0;
        for (int i = 5; i < 18 && b[i] != '\n' && b[i]; i++)
            nm[k++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : ' ';
        nm[k] = '\0';
        while (k > 0 && nm[k - 1] == ' ')
            nm[--k] = '\0';
        str_copy(s->monitor, sizeof s->monitor, nm);
    }
    /* the monitor's own timings, whatever the connection allows: the detailed
     * timings of the base block and of CTA-861 extensions, the CTA video codes
     * of fast modes (a TV's 4K at 120 Hz) and DisplayID type I and VII timings */
    s->nown = 0;
    for (size_t b = 0; b + 128 <= n && b < 128 * 8; b += 128) {
        const unsigned char *x = e + b;
        size_t dt0 = 54, dt1 = 126;
        if (b) {
            if (x[0] == 0x02) { /* CTA-861 */
                dt0 = x[2] >= 4 ? x[2] : 127;
                for (size_t k = 4; k < dt0 && k < 127;) { /* data blocks: the video codes */
                    int tag = x[k] >> 5, len = x[k] & 31;
                    if (tag == 2)
                        for (int v = 1; v <= len && k + (size_t)v < dt0; v++) {
                            int vic = x[k + v];
                            if (vic >= 129 && vic <= 192)
                                vic &= 0x7f;
                            static const struct { unsigned char vic; unsigned short w, h, hz; } V[] = {
                                { 63, 1920, 1080, 120 }, { 64, 1920, 1080, 100 }, { 117, 3840, 2160, 100 },
                                { 118, 3840, 2160, 120 }, { 119, 3840, 2160, 100 }, { 120, 3840, 2160, 120 } };
                            for (size_t q = 0; q < sizeof V / sizeof V[0]; q++)
                                if (V[q].vic == vic && s->nown < 16) {
                                    s->own[s->nown].w = V[q].w;
                                    s->own[s->nown].h = V[q].h;
                                    s->own[s->nown++].hz = V[q].hz;
                                }
                        }
                    k += 1 + (size_t)len;
                }
            } else if (x[0] == 0x70) { /* DisplayID */
                size_t end = MIN(5 + (size_t)x[2], 127);
                for (size_t k = 5; k + 3 <= end && x[k];) {
                    int tag = x[k], len = x[k + 2];
                    if ((tag == 0x03 || tag == 0x22) && k + 3 + (size_t)len <= end)
                        for (int t = 0; t + 20 <= len && s->nown < 16; t += 20) {
                            const unsigned char *d = x + k + 3 + t;
                            double clk = (double)((d[0] | d[1] << 8 | d[2] << 16) + 1) * (tag == 0x22 ? 1e3 : 1e4);
                            int w = (d[4] | d[5] << 8) + 1, hb = (d[6] | d[7] << 8) + 1;
                            int h = (d[12] | d[13] << 8) + 1, vb = (d[14] | d[15] << 8) + 1;
                            if (!(d[3] & 0x10) && w > 0 && h > 0) {
                                s->own[s->nown].w = (unsigned short)w;
                                s->own[s->nown].h = (unsigned short)h;
                                s->own[s->nown++].hz = (float)(clk / ((double)(w + hb) * (h + vb)));
                            }
                        }
                    k += 3 + (size_t)len;
                }
                continue;
            } else {
                continue;
            }
        }
        for (size_t d = dt0; d + 18 <= dt1 + 1 && s->nown < 16; d += 18) {
            const unsigned char *t = x + d;
            unsigned clk = t[0] | t[1] << 8; /* 10 kHz; 0 = a descriptor, not a timing */
            if (!clk)
                continue;
            int w = t[2] | (t[4] & 0xf0) << 4, hb = t[3] | (t[4] & 0x0f) << 8;
            int h = t[5] | (t[7] & 0xf0) << 4, vb = t[6] | (t[7] & 0x0f) << 8;
            if ((t[17] & 0x80) || w <= 0 || h <= 0) /* interlaced */
                continue;
            s->own[s->nown].w = (unsigned short)w;
            s->own[s->nown].h = (unsigned short)h;
            s->own[s->nown++].hz = (float)(clk * 1e4 / ((double)(w + hb) * (h + vb)));
        }
    }
}

/* the DRM card of a GPU known by its PCI address (NVIDIA comes through NVML): "card0", or "" */
static void drm_card_of(const gpu_t *g, const char *base, char *out, size_t n)
{
    out[0] = '\0';
    if (!strncmp(g->card, "card", 4)) {
        str_copy(out, n, g->card);
        return;
    }
    if (!g->pci[0])
        return;
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-'))
            continue;
        char p[PATHBUF], l[PATHBUF];
        sfmt(p, sizeof p, "%s/%s/device", base, e->d_name);
        ssize_t k = readlink(p, l, sizeof l - 1);
        if (k <= 0)
            continue;
        l[k] = '\0';
        const char *b = strrchr(l, '/');
        if (!strcmp(b ? b + 1 : l, g->pci)) {
            str_copy(out, n, e->d_name);
            break;
        }
    }
    closedir(d);
}

/* the screens on a GPU's ports: /sys/class/drm/card1-HDMI-A-1/ (status, enabled, modes, edid) */
/* -------------------------------------- the refresh rate a screen runs at */
#if defined(__has_include)
#if __has_include(<drm/drm_mode.h>)
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <sys/ioctl.h>
#define HAVE_DRM_UAPI 1
#endif
#endif

#ifdef HAVE_DRM_UAPI
static double mode_hz(const struct drm_mode_modeinfo *m)
{
    if (!m->clock || !m->htotal || !m->vtotal)
        return m->vrefresh;
    double hz = (double)m->clock * 1000.0 / ((double)m->htotal * (double)m->vtotal);
    if (m->flags & DRM_MODE_FLAG_INTERLACE)
        hz *= 2;
    if (m->flags & DRM_MODE_FLAG_DBLSCAN)
        hz /= 2;
    if (m->vscan > 1)
        hz /= m->vscan;
    return hz;
}

/* the value of the property called name on an object, -1 = none */
static long long drm_prop(int fd, unsigned obj, unsigned type, const char *name)
{
    struct drm_mode_obj_get_properties op;
    uint32_t ids[64];
    uint64_t vals[64];
    memset(&op, 0, sizeof op);
    op.obj_id = obj;
    op.obj_type = type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props)
        return -1;
    op.count_props = MIN(op.count_props, 64);
    op.props_ptr = (uint64_t)(uintptr_t)ids;
    op.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
        return -1;
    for (unsigned i = 0; i < op.count_props && i < 64; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof gp);
        gp.prop_id = ids[i];
        if (!ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) && !strcmp(gp.name, name))
            return (long long)vals[i];
    }
    return -1;
}

/* A desktop draws on the screens, so its compositor is the cards' master:
 * hsm's questions then never make it master for a moment (on a text console
 * the first to open a card becomes its master, and closing it puts the
 * console's mode back). This terminal's desktop, or an active graphical
 * session in logind's records (for sudo, ssh and services). */
static int desktop_running(void)
{
    const char *e;
    if (((e = getenv("WAYLAND_DISPLAY")) && *e) || ((e = getenv("DISPLAY")) && *e))
        return 1;
    DIR *d = opendir("/run/systemd/sessions");
    struct dirent *s;
    int yes = 0;
    while (d && !yes && (s = readdir(d)) != NULL) {
        if (s->d_name[0] == '.' || strchr(s->d_name, '.'))
            continue;
        char p[PATHBUF], b[2048];
        sfmt(p, sizeof p, "/run/systemd/sessions/%s", s->d_name);
        if (read_file(p, b, sizeof b) <= 0)
            continue;
        int gfx = strstr(b, "\nTYPE=wayland") || strstr(b, "\nTYPE=x11") || strstr(b, "\nTYPE=mir");
        yes = gfx && (strstr(b, "\nACTIVE=1") || strstr(b, "\nSTATE=active"));
    }
    if (d)
        closedir(d);
    return yes;
}

/* the connectors' modes through the card's DRM node (read-only questions
 * any client may ask; hsm never sets anything) */
static void drm_modes(gpu_t *g, const char *card)
{
    static const char *const TYPE[] = { "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS",
                                        "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI",
                                        "DPI", "Writeback", "SPI", "USB" };
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/dri/%s", g_devfs, card);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    /* with nothing else drawing (a text console), the first to open the card
     * becomes its master, and a master's questions make the screens be probed
     * again: give that up at once (an error just means it wasn't master) */
    ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
    struct drm_mode_card_res res;
    uint32_t conn[32];
    memset(&res, 0, sizeof res);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors) {
        close(fd);
        return;
    }
    unsigned nc = MIN(res.count_connectors, 32);
    memset(&res, 0, sizeof res);
    res.count_connectors = nc;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conn;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        close(fd);
        return;
    }
    static struct drm_mode_modeinfo modes[256];
    for (unsigned i = 0; i < MIN(res.count_connectors, nc); i++) {
        /* the modes asked for at once: asked for none, the kernel would probe
         * the screen again (for the card's master, or anyone on older kernels) */
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof c);
        c.connector_id = conn[i];
        c.count_modes = 256;
        c.modes_ptr = (uint64_t)(uintptr_t)modes;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != 1)
            continue;
        char name[32];
        sfmt(name, sizeof name, "%s-%u", c.connector_type < sizeof TYPE / sizeof TYPE[0] ? TYPE[c.connector_type] : "?",
             c.connector_type_id);
        screen_t *s = NULL;
        for (int k = 0; k < g->nscr; k++)
            if (!strcmp(g->scr[k].conn, name))
                s = &g->scr[k];
        if (!s)
            continue;
        unsigned nm = c.count_modes <= 256 ? c.count_modes : 0, enc = c.encoder_id; /* more than fit: none copied */
        /* what it runs at now: its encoder's CRTC */
        unsigned crtc_id = 0;
        if (enc) {
            struct drm_mode_get_encoder e;
            memset(&e, 0, sizeof e);
            e.encoder_id = enc;
            if (!ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e))
                crtc_id = e.crtc_id;
        }
        if (crtc_id) {
            struct drm_mode_crtc cr;
            memset(&cr, 0, sizeof cr);
            cr.crtc_id = crtc_id;
            if (!ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &cr) && cr.mode_valid) {
                s->mode_w = cr.mode.hdisplay;
                s->mode_h = cr.mode.vdisplay;
                s->hz = mode_hz(&cr.mode);
            }
            long long v = drm_prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED");
            s->vrr_on = v < 0 ? -1 : v != 0;
        }
        /* the most it offers at the size it runs at (or its preferred one) */
        int w = s->mode_w, h = s->mode_h;
        for (unsigned k = 0; !w && k < nm; k++)
            if (modes[k].type & DRM_MODE_TYPE_PREFERRED) {
                w = modes[k].hdisplay;
                h = modes[k].vdisplay;
            }
        for (unsigned k = 0; k < nm; k++)
            if (modes[k].hdisplay == w && modes[k].vdisplay == h && !(modes[k].flags & DRM_MODE_FLAG_INTERLACE))
                s->max_hz = MAX(s->max_hz, mode_hz(&modes[k]));
        long long v = drm_prop(fd, conn[i], DRM_MODE_OBJECT_CONNECTOR, "vrr_capable");
        s->vrr_cap = v < 0 ? -1 : v != 0;
    }
    close(fd);
}
#endif

/* test hook: HSM_DRM_FAKE=dir answers from dir/card0-DP-1 ("mode 2560x1440 60",
 * "max 165", "vrr_capable 1", "vrr 0") */
static void drm_fake(gpu_t *g, const char *dir, const char *card)
{
    for (int k = 0; k < g->nscr; k++) {
        screen_t *s = &g->scr[k];
        char p[PATHBUF], b[512];
        sfmt(p, sizeof p, "%s/%s-%s", dir, card, s->conn);
        if (read_file(p, b, sizeof b) <= 0)
            continue;
        char *cur = b, *l;
        while ((l = next_line(&cur)) != NULL) {
            int a, c, v;
            double hz;
            if (sscanf(l, "mode %dx%d %lf", &a, &c, &hz) == 3) {
                s->mode_w = a;
                s->mode_h = c;
                s->hz = hz;
            } else if (sscanf(l, "max %lf", &hz) == 1) {
                s->max_hz = hz;
            } else if (sscanf(l, "vrr_capable %d", &v) == 1) {
                s->vrr_cap = v;
            } else if (sscanf(l, "vrr %d", &v) == 1) {
                s->vrr_on = v;
            }
        }
    }
}

static double g_scr_want; /* when the Hardware view last showed the screens (monotonic) */

void gpu_screens_wanted(void)
{
    g_scr_want = now_sec();
}

static void screens_read(gpu_t *g)
{
    screen_t old[MAX_SCREENS];
    int nold = g->nscr;
    memcpy(old, g->scr, sizeof old);
    g->nscr = 0;
    char base[PATHBUF], card[24];
    sfmt(base, sizeof base, "%s/class/drm", g_sysfs);
    drm_card_of(g, base, card, sizeof card);
    if (!card[0])
        return;
    DIR *d = opendir(base);
    if (!d)
        return;
    size_t cl = strlen(card);
    struct dirent *e;
    /* "status" and "enabled" are read without a lock. A monitor's own data
     * (its modes, its EDID) waits for the graphics driver's lock, which the
     * desktop holds while it checks the screens again - for a second or
     * more when a game starts or quits. So that is read when a screen comes,
     * goes or is switched on or off, and once a minute (another monitor on
     * the same port), not every 5 s. */
    double tn = now_sec();
    int edid_due = !g->edid_t || tn - g->edid_t >= 60;
    if (edid_due)
        g->edid_t = tn;
    while ((e = readdir(d)) != NULL && g->nscr < MAX_SCREENS) {
        if (strncmp(e->d_name, card, cl) || e->d_name[cl] != '-' || strlen(e->d_name + cl + 1) >= sizeof g->scr[0].conn)
            continue;
        char p[PATHBUF], b[64];
        sfmt(p, sizeof p, "%s/%s/status", base, e->d_name);
        if (read_line(p, b, sizeof b) || strcmp(b, "connected"))
            continue;
        screen_t *s = &g->scr[g->nscr++];
        memset(s, 0, sizeof *s);
        s->vrr_cap = s->vrr_on = -1;
        str_copy(s->conn, sizeof s->conn, e->d_name + cl + 1);
        s->builtin = !strncmp(s->conn, "eDP", 3) || !strncmp(s->conn, "LVDS", 4) || !strncmp(s->conn, "DSI", 3);
        sfmt(p, sizeof p, "%s/%s/enabled", base, e->d_name);
        s->on = read_line(p, b, sizeof b) || strcmp(b, "disabled"); /* unknown: say it's in use */
        const screen_t *o = NULL;
        for (int j = 0; j < nold && !o; j++)
            if (!strcmp(old[j].conn, s->conn))
                o = &old[j];
        if (o && o->on == s->on && !edid_due) {
            str_copy(s->monitor, sizeof s->monitor, o->monitor);
            s->inch = o->inch;
            s->w = o->w;
            s->h = o->h;
            s->nown = o->nown;
            memcpy(s->own, o->own, sizeof s->own);
            continue;
        }
        sfmt(p, sizeof p, "%s/%s/modes", base, e->d_name);
        if (read_line(p, b, sizeof b) == 0 && sscanf(b, "%dx%d", &s->w, &s->h) != 2)
            s->w = s->h = 0;
        unsigned char ed[512];
        sfmt(p, sizeof p, "%s/%s/edid", base, e->d_name);
        ssize_t n = read_file(p, (char *)ed, sizeof ed);
        if (n >= 128)
            edid_parse(ed, (size_t)n, s);
    }
    closedir(d);
    /* the refresh rates, from the card's DRM node: every 60 s, every 5 s while
     * the Hardware view shows them, at once when a screen comes or goes. Only
     * from a card that lights a screen: one without could go to sleep, and
     * opening it would keep it awake. */
    int changed = g->nscr != nold, lit = 0;
    for (int k = 0; k < g->nscr; k++) {
        const screen_t *o = NULL;
        for (int j = 0; j < nold; j++)
            if (!strcmp(old[j].conn, g->scr[k].conn))
                o = &old[j];
        if (!o || o->on != g->scr[k].on || strcmp(o->monitor, g->scr[k].monitor))
            changed = 1;
        lit |= g->scr[k].on;
    }
    const char *fake = secure_getenv("HSM_DRM_FAKE");
    if (changed || !g->drm_t || tn - g->drm_t >= (tn - g_scr_want < 3 ? 4.5 : 60)) {
        g->drm_t = tn;
        char rs[24] = "", f[PATHBUF];
        sfmt(f, sizeof f, "%s/power/runtime_status", g->dev);
        read_line(f, rs, sizeof rs);
        if (fake && *fake)
            drm_fake(g, fake, card);
#ifdef HAVE_DRM_UAPI
        else if (lit && strcmp(rs, "suspended") && (g->nv_state == NV_NONE || g->nv_state == NV_ON) && desktop_running())
            drm_modes(g, card);
#endif
    } else { /* not asked this time: what was read last */
        for (int k = 0; k < g->nscr; k++)
            for (int j = 0; j < nold; j++)
                if (!strcmp(old[j].conn, g->scr[k].conn)) {
                    screen_t *s = &g->scr[k];
                    s->mode_w = old[j].mode_w;
                    s->mode_h = old[j].mode_h;
                    s->hz = old[j].hz;
                    s->max_hz = old[j].max_hz;
                    s->vrr_cap = old[j].vrr_cap;
                    s->vrr_on = old[j].vrr_on;
                }
    }
    for (int k = 0; k < g->nscr; k++) { /* what the monitor itself can do at the size it runs at */
        screen_t *s = &g->scr[k];
        int w = s->mode_w ? s->mode_w : s->w, h = s->mode_w ? s->mode_h : s->h;
        s->mon_hz = 0;
        for (int j = 0; j < s->nown; j++)
            if (s->own[j].w == w && s->own[j].h == h)
                s->mon_hz = MAX(s->mon_hz, s->own[j].hz);
    }
    for (int i = 1; i < g->nscr; i++) /* the built-in screen first, then by name */
        for (int k = i; k > 0; k--) {
            screen_t *a = &g->scr[k - 1], *c = &g->scr[k];
            if (a->builtin > c->builtin || (a->builtin == c->builtin && strcmp(a->conn, c->conn) <= 0))
                break;
            screen_t t = *a;
            *a = *c;
            *c = t;
        }
}

int screen_slow(const screen_t *s, double *can)
{
    *can = 0;
    /* the built-in panel may run slower on purpose (to save battery) */
    if (s->builtin || !s->on || s->hz <= 0)
        return SCR_FINE;
    if (s->max_hz >= 70 && s->max_hz > s->hz * 1.15) { /* a faster mode is there: a setting never changed */
        *can = s->max_hz;
        return SCR_SETTING;
    }
    if (s->max_hz > 0 && s->mon_hz >= 70 && s->mon_hz > MAX(s->hz, s->max_hz) * 1.15) {
        *can = s->mon_hz; /* the monitor can, the connection doesn't offer it */
        return SCR_LINK;
    }
    return SCR_FINE;
}

/* a screen on a GPU that could sleep without it: "HDMI-A-1 (DELL U2720Q)", or "" */
const char *gpu_screen_holds(const gpu_t *g, char *out, size_t n)
{
    out[0] = '\0';
    if (g->nv_state == NV_NONE || g->nv_state == NV_ASLEEP || g->integrated || !g->pm_auto) /* one that could power off, and is on */
        return out;
    for (int i = 0; i < g->nscr; i++)
        if (g->scr[i].on) {
            if (g->scr[i].monitor[0])
                sfmt(out, n, "%s (%s)", g->scr[i].conn, g->scr[i].monitor);
            else
                str_copy(out, n, g->scr[i].conn);
            break;
        }
    return out;
}

void gpus_sample(gpus_t *gs, double dt)
{
    double now = now_sec();
    gs->slow_s = 0;
    gs->slow[0] = '\0';
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        double t0 = now_sec();
        double mt = g->mem_total, cm = g->clk_max_mhz;
        g->util = g->mem_used = g->mem_total = -1;
        g->temp_c = g->power_w = g->power_cap_w = g->clk_mhz = g->mem_clk_mhz = g->fan_pct = g->fan_rpm = -1;
        g->enc_pct = g->dec_pct = -1;
        g->pcie_gen = g->pcie_width = 0;
        if (g->nvml) {
            nvml_sample(g);
        } else if (g->nv_state != NV_NONE) {
            /* let go of / asleep: nothing to ask it without waking it */
            g->util = 0;
            g->enc_pct = g->dec_pct = 0;
            g->power_w = g->nv_state == NV_ASLEEP ? 0 : g->last_power;
            g->mem_total = mt; /* what it has stays known: the panel keeps its shape */
            g->clk_max_mhz = cm;
            g->reasons = 0;
            g->slow_now = 0;
            g->slow_since = 0;
        } else {
            sysfs_sample(g);
        }
        double tscr = 0;
        if (!g->scr_t || now - g->scr_t >= 5.0) { /* screens come and go now and then */
            g->scr_t = now;
            double ts = now_sec();
            screens_read(g);
            tscr = now_sec() - ts;
            /* a graphics card's PCIe link (NVIDIA's comes from NVML, while it's awake);
             * not while it's powered off: its registers don't answer then */
            char rs[24] = "", f[PATHBUF];
            sfmt(f, sizeof f, "%s/power/runtime_status", g->dev);
            read_line(f, rs, sizeof rs);
            pcilink_t l;
            if (g->nv_state == NV_NONE && !g->integrated && strcmp(rs, "suspended") && pcie_link(g->dev, &l) == 0) {
                g->lk_gen = l.gen;
                g->lk_width = l.width;
                g->pcie_gen_max = l.best_gen;
                g->pcie_width_max = l.best_width;
            } else if (g->nv_state == NV_NONE) {
                g->lk_gen = g->lk_width = 0;
            }
        }
        if (g->nv_state == NV_NONE && g->lk_gen) {
            g->pcie_gen = g->lk_gen;
            g->pcie_width = g->lk_width;
        }
        double took = now_sec() - t0;
        if (took > gs->slow_s) {
            gs->slow_s = took;
            if (tscr > took / 2) /* waiting for the driver while the desktop checks the screens again */
                sfmt(gs->slow, sizeof gs->slow, "screens of the %s", g->name);
            else
                str_copy(gs->slow, sizeof gs->slow, g->name);
        }
        /* how much of its work it did held back, by heat or by a power limit; a
         * GPU whose load isn't known (xe) counts as busy, but not towards the share */
        if (g->reasons_ok && (g->nvml || g->nv_state == NV_NONE)) {
            int busy = g->util < 0 || g->util >= 30;
            int bits = !busy ? 0 : g->nvml ? slow_bits(g->reasons) : g->sys_bits;
            if (g->nvml)
                g->slow_hw = bits && (g->reasons & 0xC0ULL) != 0; /* hardware slowdown, power brake */
            if (busy && g->util >= 0 && dt > 0 && dt < 30) {
                g->busy_s += dt;
                if (bits & GS_HEAT)
                    g->heat_s += dt;
                if (bits & GS_POWER)
                    g->pwr_s += dt;
                if (bits)
                    g->held_s += dt;
            }
            g->slow_now = bits;
            if (!bits)
                g->slow_since = 0;
            else if (!g->slow_since)
                g->slow_since = now;
        }
    }
    if (nv_lib)
        nv_policy(gs, now);
}

void gpus_close(gpus_t *gs)
{
    for (int i = 0; i < gs->n; i++)
        for (int j = 0; j < gs->g[i].neng; j++)
            close(gs->g[i].eng[j].fd);
    if (nv_lib) {
        if (nv_live && nv_shutdown)
            nv_shutdown();
        nv_live = 0;
        dlclose(nv_lib);
        nv_lib = NULL;
    }
}

/* NVIDIA's proprietary driver has no DRM fdinfo statistics; NVML keeps a
 * short history of per-process "SM" (shader) utilization instead. Only the
 * samples newer than the last call are returned. */
static int cmp_proc_pid(const void *k, const void *e)
{
    pid_t a = *(const pid_t *)k, b = ((const proc_t *)e)->pid;
    return (a > b) - (a < b);
}

void gpus_proc_sample(gpus_t *gs, procs_t *ps)
{
    if (!nv_putil || !ps->n)
        return;
    for (int i = 0; i < gs->n; i++) {
        gpu_t *g = &gs->g[i];
        if (!g->nvml)
            continue;
        nvdev_t d = (nvdev_t)g->nvml;
        unsigned int cnt = 0;
        nv_putil(d, NULL, &cnt, g->nv_ts); /* NVML_ERROR_INSUFFICIENT_SIZE, with the count */
        if (!cnt)
            continue;
        if (cnt > 512)
            cnt = 512;
        static nvpsample_t smp[512];
        unsigned int n = cnt;
        if (nv_putil(d, smp, &n, g->nv_ts) != 0 || n > cnt)
            continue;
        for (unsigned int k = 0; k < n; k++) {
            if (smp[k].ts > g->nv_ts)
                g->nv_ts = smp[k].ts;
            pid_t pid = (pid_t)smp[k].pid;
            proc_t *p = bsearch(&pid, ps->p, (size_t)ps->n, sizeof *ps->p, cmp_proc_pid);
            if (!p)
                continue;
            double u = smp[k].sm > 100 ? 100 : (double)smp[k].sm;
            if (u > p->gpu) /* several samples per process: keep the busiest */
                p->gpu = u;
            ps->gpu_any = 1;
        }
    }
}
