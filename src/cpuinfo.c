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

/* The CPU in detail, as CPU-Z or lscpu would show it: its caches (per kind
 * of core on hybrid CPUs), the instruction sets that matter (and the x86-64
 * level some distributions require), family / model / stepping and the
 * microcode it runs, SMT, its rated clock and NUMA nodes. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cache_size(int kb, char *out, size_t n)
{
    if (kb < 1024 || kb % 256) /* 48K, 640K, 1920K */
        sfmt(out, n, "%dK", kb);
    else if (kb % 1024 == 0) /* 2M, 32M */
        sfmt(out, n, "%dM", kb / 1024);
    else if (kb % 512 == 0) /* 1.5M */
        sfmt(out, n, "%.1fM", kb / 1024.0);
    else /* 1.25M */
        sfmt(out, n, "%.2fM", kb / 1024.0);
    return out;
}

/* a word in a space-separated list */
static int has(const char *list, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = list; (p = strstr(p, w)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0' || p[n] == '\n'))
            return 1;
    return 0;
}

static int has_all(const char *list, const char *const *w, int n)
{
    for (int i = 0; i < n; i++)
        if (!has(list, w[i]))
            return 0;
    return 1;
}

static void add_feat(char *out, size_t n, const char *name)
{
    sfmt(out + strlen(out), n - strlen(out), "%s%s", out[0] ? ", " : "", name);
}

/* x86: the level glibc and distributions go by, and the notable extensions */
static void x86_feats(cpu_t *c, const char *fl)
{
    static const char *const V2[] = { "cx16", "lahf_lm", "popcnt", "sse4_1", "sse4_2", "ssse3", "pni" };
    static const char *const V3[] = { "avx", "avx2", "bmi1", "bmi2", "f16c", "fma", "abm", "movbe", "xsave" };
    static const char *const V4[] = { "avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl" };
    int lv = has(fl, "lm") ? 1 : 0;
    if (lv && has_all(fl, V2, 7))
        lv = 2;
    if (lv == 2 && has_all(fl, V3, 9))
        lv = 3;
    if (lv == 3 && has_all(fl, V4, 5))
        lv = 4;
    if (lv)
        sfmt(c->isa, sizeof c->isa, "x86-64-v%d", lv);
    char *f = c->feats;
    size_t n = sizeof c->feats;
    f[0] = '\0';
    if (has(fl, "avx2"))
        add_feat(f, n, "AVX2");
    else if (has(fl, "avx"))
        add_feat(f, n, "AVX");
    if (has(fl, "avx512f"))
        add_feat(f, n, "AVX-512");
    if (has(fl, "avx10"))
        add_feat(f, n, "AVX10");
    if (has(fl, "fma"))
        add_feat(f, n, "FMA3");
    if (has(fl, "avx_vnni") || has(fl, "avx512_vnni"))
        add_feat(f, n, "VNNI");
    if (has(fl, "avx512_bf16") || has(fl, "avx_bf16"))
        add_feat(f, n, "BF16");
    if (has(fl, "amx_tile"))
        add_feat(f, n, "AMX");
    if (has(fl, "aes"))
        add_feat(f, n, "AES");
    if (has(fl, "sha_ni"))
        add_feat(f, n, "SHA");
    if (has(fl, "vaes"))
        add_feat(f, n, "VAES");
    if (has(fl, "rdrand"))
        add_feat(f, n, "RDRAND");
}

/* ARM: the extensions programs ask for */
static void arm_feats(cpu_t *c, const char *fl)
{
    static const struct {
        const char *flag, *name;
    } F[] = { { "asimd", "NEON" },      { "sve2", "SVE2" },     { "sve", "SVE" },     { "asimddp", "dot product" },
              { "i8mm", "I8MM" },       { "bf16", "BF16" },     { "atomics", "LSE atomics" }, { "aes", "AES" },
              { "sha2", "SHA2" },       { "sha3", "SHA3" },     { "crc32", "CRC32" } };
    c->feats[0] = '\0';
    int sve2 = has(fl, "sve2");
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++)
        if (has(fl, F[i].flag) && !(sve2 && !strcmp(F[i].flag, "sve")))
            add_feat(c->feats, sizeof c->feats, F[i].name);
}

static void read_cpuinfo(cpu_t *c)
{
    char p[PATHBUF], *buf = NULL;
    size_t cap = 0;
    sfmt(p, sizeof p, "%s/cpuinfo", g_procfs);
    if (read_file_grow(p, &buf, &cap) <= 0) {
        free(buf);
        return;
    }
    char *cur = buf, *line;
    int got_flags = 0;
    while ((line = next_line(&cur)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        char *e = colon;
        while (e > line && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        *e = '\0';
        if (!strcmp(line, "vendor_id") && !c->vendor_id[0])
            str_copy(c->vendor_id, sizeof c->vendor_id, val);
        else if (!strcmp(line, "cpu family") && c->family < 0)
            c->family = atoi(val);
        else if (!strcmp(line, "model") && c->model_no < 0)
            c->model_no = atoi(val);
        else if (!strcmp(line, "stepping") && c->stepping < 0)
            c->stepping = atoi(val);
        else if (!strcmp(line, "microcode") && !c->ucode[0])
            str_copy(c->ucode, sizeof c->ucode, val);
        else if (!strcmp(line, "flags") && !got_flags) {
            x86_feats(c, val);
            c->hypervisor = has(val, "hypervisor");
            c->virt = has(val, "vmx") ? 'v' : has(val, "svm") ? 's' : 0;
            got_flags = 1;
        } else if (!strcmp(line, "Features") && !got_flags) {
            arm_feats(c, val);
            got_flags = 1;
        }
    }
    free(buf);
}

/* the size of a cache: "48K", "1280K", "32M" */
static int kb_of(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s)
        return 0;
    return (int)(*end == 'M' ? v * 1024 : *end == 'G' ? v * 1024 * 1024 : v);
}

/* level, then kind of core (shared ones last), then data before instructions, then size */
static int cache_before(const cache_t *x, const cache_t *y)
{
    int kx = x->kind < 0 ? 99 : x->kind, ky = y->kind < 0 ? 99 : y->kind;
    if (x->level != y->level)
        return x->level < y->level;
    if (kx != ky)
        return kx < ky;
    if (x->type != y->type) /* d, i, u */
        return x->type < y->type;
    return x->kb > y->kb;
}

/* package and core of a CPU, read once: "pkg:core" packed in an int */
static int core_key(int cpu, int *memo)
{
    if (memo[cpu] != -2)
        return memo[cpu];
    char p[PATHBUF];
    long long core = -1, pkg = 0;
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/core_id", g_sysfs, cpu);
    if (read_ll(p, &core) || core < 0) {
        memo[cpu] = -1 - cpu; /* unknown: count each CPU as its own core */
        return memo[cpu];
    }
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/topology/physical_package_id", g_sysfs, cpu);
    read_ll(p, &pkg);
    memo[cpu] = (int)((pkg & 0x3ff) << 20 | (core & 0xfffff));
    return memo[cpu];
}

static void read_caches(cpu_t *c)
{
    static int memo[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++)
        memo[i] = -2;
    c->ncache = 0;
    static unsigned char set[MAX_CPUS];
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        char base[PATHBUF];
        sfmt(base, sizeof base, "%s/devices/system/cpu/cpu%d/cache", g_sysfs, cpu);
        DIR *d = opendir(base);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "index", 5))
                continue;
            char dir[PATHBUF], p[PATHBUF], b[64], ty[24] = "", shared[1024] = "";
            sfmt(dir, sizeof dir, "%s/%s", base, e->d_name);
            long long lvl = 0;
            sfmt(p, sizeof p, "%s/level", dir);
            if (read_ll(p, &lvl) || lvl < 1 || lvl > 4)
                continue;
            sfmt(p, sizeof p, "%s/type", dir);
            read_line(p, ty, sizeof ty);
            sfmt(p, sizeof p, "%s/size", dir);
            if (read_line(p, b, sizeof b))
                continue;
            int kb = kb_of(b);
            if (kb <= 0)
                continue;
            char t = !strcmp(ty, "Data") ? 'd' : !strcmp(ty, "Instruction") ? 'i' : 'u';
            /* the CPUs that share it; each of them lists it, so it counts once: at the first of them */
            memset(set, 0, sizeof set);
            sfmt(p, sizeof p, "%s/shared_cpu_list", dir);
            if (read_line(p, shared, sizeof shared) || cpulist_parse(shared, set, MAX_CPUS) <= 0)
                set[cpu] = 1;
            int first = 0;
            while (first < MAX_CPUS && !set[first])
                first++;
            if (first != cpu)
                continue;
            /* the cores among them (not threads), and their kind */
            int cores = 0;
            signed char kind = -2; /* -2 none yet, -1 mixed */
            for (int i = 0; i < MAX_CPUS; i++) {
                if (!set[i])
                    continue;
                signed char ki = c->nctype ? c->core_type[i] : -1;
                kind = kind == -2 ? ki : kind == ki ? kind : -1;
                int key = core_key(i, memo), dupc = 0;
                for (int k = 0; k < i && !dupc; k++)
                    dupc = set[k] && core_key(k, memo) == key;
                cores += !dupc;
            }
            if (kind == -2)
                kind = -1;
            cores = MAX(cores, 1);
            int k = 0;
            while (k < c->ncache && !(c->cache[k].level == lvl && c->cache[k].type == t && c->cache[k].kb == kb &&
                                      c->cache[k].share == cores && c->cache[k].kind == kind))
                k++;
            if (k == c->ncache && c->ncache < 16) {
                c->cache[k].level = (unsigned char)lvl;
                c->cache[k].type = t;
                c->cache[k].kb = kb;
                c->cache[k].share = cores;
                c->cache[k].kind = kind;
                c->cache[k].n = 0;
                c->ncache++;
            }
            if (k < c->ncache)
                c->cache[k].n++;
        }
        closedir(d);
    }
    for (int i = 1; i < c->ncache; i++) { /* in order: L1 first; insertion sort, a handful */
        cache_t x = c->cache[i];
        int j = i;
        while (j > 0 && cache_before(&x, &c->cache[j - 1])) {
            c->cache[j] = c->cache[j - 1];
            j--;
        }
        c->cache[j] = x;
    }
}

/* the clock a CPU is rated at, MHz; 0 = unknown */
static int base_of(int cpu)
{
    char p[PATHBUF];
    long long v;
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/cpufreq/base_frequency", g_sysfs, cpu); /* intel_pstate, kHz */
    if (!read_ll(p, &v) && v > 0)
        return (int)(v / 1000);
    sfmt(p, sizeof p, "%s/devices/system/cpu/cpu%d/acpi_cppc/nominal_freq", g_sysfs, cpu); /* AMD and others, MHz */
    if (!read_ll(p, &v) && v > 0)
        return (int)v;
    return 0;
}

void cpu_details(cpu_t *c)
{
    char p[PATHBUF];
    long long v;
    sfmt(p, sizeof p, "%s/devices/system/cpu/smt/control", g_sysfs);
    if (read_line(p, c->smt, sizeof c->smt))
        c->smt[0] = '\0';
    sfmt(p, sizeof p, "%s/devices/system/cpu/smt/active", g_sysfs);
    c->smt_active = read_ll(p, &v) ? -1 : v != 0;
    if (c->det_done)
        return;
    c->det_done = 1;
    c->family = c->model_no = c->stepping = -1;
    read_cpuinfo(c);
    read_caches(c);
    c->base_mhz = base_of(c->ncpu ? c->id[0] : 0);
    for (int k = 0; k < c->nctype; k++) /* the first CPU of each kind */
        for (int i = 0; i < c->ncpu; i++)
            if (c->core_type[c->id[i]] == k) {
                c->kbase[k] = base_of(c->id[i]);
                break;
            }
    sfmt(p, sizeof p, "%s/devices/system/node", g_sysfs);
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (!strncmp(e->d_name, "node", 4) && e->d_name[4] >= '0' && e->d_name[4] <= '9')
            c->numa++;
    if (d)
        closedir(d);
}

/* ---------------------------------------------------------------- in words */

/* "P-core" / "P-cores", "Cortex-A76 core" / "Cortex-A76 cores", "core" / "cores" */
static const char *kind_name(const cpu_t *c, int kind, int plural, char *out, size_t n)
{
    const char *nm = kind >= 0 && kind < c->nctype ? c->ctype[kind].name : "";
    size_t l = strlen(nm);
    if (!*nm)
        sfmt(out, n, "%s", plural ? "cores" : "core");
    else if (l >= 4 && !strcmp(nm + l - 4, "core"))
        sfmt(out, n, "%s%s", nm, plural ? "s" : "");
    else
        sfmt(out, n, "%s %s", nm, plural ? "cores" : "core");
    return out;
}

/* who has one: " per P-core", " shared by all 4 E-cores", ", each shared by 8 cores" (after "2 x 32M") */
static void who(const cpu_t *c, int kind, int share, int count, char *out, size_t n)
{
    char k1[40], kn[40];
    kind_name(c, kind, 0, k1, sizeof k1);
    kind_name(c, kind, 1, kn, sizeof kn);
    int total = kind >= 0 && kind < c->nctype ? c->ctype[kind].cores : c->cores;
    if (share <= 1)
        sfmt(out, n, " per %s", k1);
    else if (count == 1 && share >= total && total == 2)
        sfmt(out, n, " shared by both %s", kn);
    else if (count == 1 && share >= total)
        sfmt(out, n, " shared by all %d %s", total, kn);
    else if (count == 1)
        sfmt(out, n, " shared by %d %s", share, kn);
    else
        sfmt(out, n, ", each shared by %d %s", share, kn);
}

int cache_lines(const cpu_t *c, char out[][96], int max)
{
    int n = MIN(c->ncache, 16), lines = 0;
    const cache_t *o[16]; /* in order already */
    for (int i = 0; i < n; i++)
        o[i] = &c->cache[i];
    int last_level = 0;
    for (int j = 0; j < n && lines < max; j++) {
        const cache_t *e = o[j], *ins = NULL;
        char sz[16], w[96], lv[8];
        lv[0] = '\0';
        if (e->level != last_level)
            sfmt(lv, sizeof lv, "L%d", e->level);
        last_level = e->level;
        /* L1: data and instructions together, when they belong to the same cores */
        if (e->type == 'd' && j + 1 < n) {
            const cache_t *x = o[j + 1];
            if (x->level == e->level && x->type == 'i' && x->kind == e->kind && x->share == e->share && x->n == e->n)
                ins = x;
        }
        who(c, e->kind, e->share, e->n, w, sizeof w);
        char many[16] = ""; /* "2 x 32M": two of them, each for some of the cores */
        if (e->n > 1 && e->share > 1)
            sfmt(many, sizeof many, "%d x ", e->n);
        if (ins) {
            char si[16];
            sfmt(out[lines++], 96, "%-3s %s%s data + %s instructions%s", lv, many, cache_size(e->kb, sz, sizeof sz),
                 cache_size(ins->kb, si, sizeof si), w);
            j++;
        } else
            sfmt(out[lines++], 96, "%-3s %s%s%s%s", lv, many, cache_size(e->kb, sz, sizeof sz),
                 e->type == 'd' ? " data" : e->type == 'i' ? " instructions" : "", w);
    }
    return lines;
}

void smt_text(const cpu_t *c, char *out, size_t n)
{
    out[0] = '\0';
    if (!c->smt[0] || !strcmp(c->smt, "notimplemented"))
        return;
    if (!strcmp(c->smt, "on")) {
        if (c->smt_active == 0) {
            sfmt(out, n, "on, but the second threads are offline");
            return;
        }
        for (int k = 0; k < c->nctype; k++) /* the kinds that have it */
            if (c->ctype[k].cores && c->ctype[k].threads > c->ctype[k].cores) {
                char kn[40];
                sfmt(out + strlen(out), n - strlen(out), "%s%d threads per %s", out[0] ? ", " : "on: ",
                     c->ctype[k].threads / c->ctype[k].cores, kind_name(c, k, 0, kn, sizeof kn));
            }
        if (!out[0])
            sfmt(out, n, "on: %d threads per core", c->cores ? MAX(c->threads / c->cores, 1) : 2);
    } else if (!strcmp(c->smt, "off"))
        sfmt(out, n, "off, switched off in Linux (nosmt or smt/control)");
    else if (!strcmp(c->smt, "forceoff"))
        sfmt(out, n, "off, switched off for good at boot (nosmt=force)");
    else if (!strcmp(c->smt, "notsupported"))
        sfmt(out, n, "%s", c->hypervisor     ? "no (the virtual machine's CPUs have none)"
                           : c->vendor_id[0] ? "no: the CPU has none, or it's off in the firmware setup"
                                             : "no: the CPU has none");
    else
        sfmt(out, n, "%s", c->smt);
}

int cpu_boost_mhz(const cpu_t *c, int kind)
{
    double m = 0;
    for (int i = 0; i < c->ncpu; i++) {
        int id = c->id[i];
        if (id < 0 || id >= MAX_CPUS || (kind >= 0 && c->core_type[id] != kind))
            continue;
        m = MAX(m, c->mhz_hw[id]);
    }
    return (int)(m + 0.5);
}

void clock_text(const cpu_t *c, int boost, char *out, size_t n)
{
    out[0] = '\0';
    int kinds = 0;
    for (int k = 0; k < c->nctype; k++) {
        int v = boost ? cpu_boost_mhz(c, k) : c->kbase[k];
        if (v > 0)
            kinds++;
    }
    if (kinds > 1 || (kinds == 1 && c->nctype == 1)) {
        for (int k = 0; k < c->nctype; k++) {
            int v = boost ? cpu_boost_mhz(c, k) : c->kbase[k];
            char kn[40];
            if (v > 0)
                sfmt(out + strlen(out), n - strlen(out), "%s%d MHz (%s)", out[0] ? ", " : "", v,
                     kind_name(c, k, 1, kn, sizeof kn));
        }
        return;
    }
    int v = boost ? cpu_boost_mhz(c, -1) : c->base_mhz;
    if (v > 0)
        sfmt(out, n, "%d MHz", v);
}
