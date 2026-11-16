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

/* What the firmware set up: UEFI or the old BIOS way, Secure Boot, the TPM,
 * the CPU's virtualisation (VT-x / AMD-V, often switched off in the firmware
 * setup) and the IOMMU. Read once; whether KVM can run is checked again. */

#define _GNU_SOURCE
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* a one-byte UEFI variable: 4 bytes of attributes, then the value; -1 = can't read */
static int efi_byte(const char *name)
{
    char p[PATHBUF];
    unsigned char b[8];
    sfmt(p, sizeof p, "%s/firmware/efi/efivars/%s-" EFI_GLOBAL, g_sysfs, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, b, sizeof b);
    close(fd);
    return r >= 5 ? b[4] != 0 : -1;
}

static int count_dir(const char *p, const char *prefix, char *first, size_t n)
{
    DIR *d = opendir(p);
    if (!d)
        return 0;
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || (prefix && strncmp(e->d_name, prefix, strlen(prefix))))
            continue;
        if (!k && first)
            str_copy(first, n, e->d_name);
        k++;
    }
    closedir(d);
    return k;
}

/* a model-specific register (root, with the msr module); -1 = can't read */
static int msr(unsigned reg, uint64_t *v)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/cpu/0/msr", g_devfs);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = pread(fd, v, sizeof *v, (off_t)reg);
    close(fd);
    return r == (ssize_t)sizeof *v ? 0 : -1;
}

/* the CPU itself, not what the kernel left in its flags: has it VT-x / AMD-V?
 * 1 yes, 0 no, -1 can't ask (not x86, or a test setup) */
static int cpu_has_virt(int intel)
{
#if defined(__x86_64__) || defined(__i386__)
    if (strcmp(g_procfs, "/proc"))
        return -1;
    unsigned a, b, c, d;
    if (intel)
        return __get_cpuid(1, &a, &b, &c, &d) ? (int)((c >> 5) & 1) : -1; /* VMX */
    return __get_cpuid(0x80000001, &a, &b, &c, &d) ? (int)((c >> 2) & 1) : -1; /* SVM */
#else
    (void)intel;
    return -1;
#endif
}

static void virt_check(firmware_t *f, const cpu_t *c)
{
    char p[PATHBUF];
    sfmt(p, sizeof p, "%s/kvm", g_devfs);
    f->kvm = exists(p);
    f->vm = c->hypervisor;
    int intel = !strcmp(c->vendor_id, "GenuineIntel"), amd = !strcmp(c->vendor_id, "AuthenticAMD");
    if (!c->vendor_id[0]) { /* ARM: KVM is the only sign */
        f->virt = f->kvm ? 'a' : 0;
        f->virt_state = f->kvm ? VIRT_ON : VIRT_UNKNOWN;
        return;
    }
    f->virt = c->virt ? c->virt : intel ? 'v' : amd ? 's' : 0;
    uint64_t v;
    if (c->virt) {
        if (f->kvm)
            f->virt_state = VIRT_ON;
        else if (c->virt == 's' && !msr(0xC0010114, &v)) /* VM_CR: SVMDIS */
            f->virt_state = v & (1u << 4) ? VIRT_OFF : VIRT_NOKVM;
        else if (c->virt == 'v' && !msr(0x3A, &v)) /* FEATURE_CONTROL: locked without VMX */
            f->virt_state = (v & 1) && !(v & 4) ? VIRT_OFF : VIRT_NOKVM;
        else
            f->virt_state = VIRT_NOKVM;
        return;
    }
    if (f->vm) { /* the host doesn't pass it on */
        f->virt_state = VIRT_VM;
        return;
    }
    /* no flag: the kernel takes VT-x out when the firmware locked it off */
    int has = intel || amd ? cpu_has_virt(intel) : -1;
    f->virt_state = has == 1 ? VIRT_OFF : has == 0 ? VIRT_NONE : intel ? VIRT_MAYBE_OFF : VIRT_NONE;
    if (has == 0)
        f->virt = 0;
}

void firmware_sample(firmware_t *f, const cpu_t *c)
{
    if (!c->det_done)
        return;
    virt_check(f, c);
    if (f->done)
        return;
    f->done = 1;
    char p[PATHBUF], b[64];
    long long v;

    sfmt(p, sizeof p, "%s/firmware/efi", g_sysfs);
    f->uefi = exists(p) ? 1 : c->vendor_id[0] ? 0 : -1;
    sfmt(p, sizeof p, "%s/firmware/efi/fw_platform_size", g_sysfs);
    f->efi_bits = f->uefi == 1 && !read_ll(p, &v) ? (int)v : 0;
    f->secure_boot = f->setup_mode = -1;
    if (f->uefi == 1) {
        f->secure_boot = efi_byte("SecureBoot");
        f->setup_mode = efi_byte("SetupMode");
    } else if (f->uefi == 0)
        f->secure_boot = 0;
    sfmt(p, sizeof p, "%s/kernel/security/lockdown", g_sysfs); /* "none [integrity] confidentiality" */
    f->lockdown[0] = '\0';
    if (!read_line(p, b, sizeof b)) {
        char *o = strchr(b, '['), *e = o ? strchr(o, ']') : NULL;
        if (o && e) {
            *e = '\0';
            str_copy(f->lockdown, sizeof f->lockdown, o + 1);
        }
    }

    f->tpm = 0;
    sfmt(p, sizeof p, "%s/class/tpm/tpm0", g_sysfs);
    if (exists(p)) {
        f->tpm = 1;
        sfmt(p, sizeof p, "%s/class/tpm/tpm0/tpm_version_major", g_sysfs);
        if (!read_ll(p, &v))
            f->tpm = v == 2 ? 20 : v == 1 ? 12 : 1;
        else { /* older kernels: a 1.2 TPM has caps, "TCG version: 1.2" */
            char *buf = NULL;
            size_t cap = 0;
            sfmt(p, sizeof p, "%s/class/tpm/tpm0/caps", g_sysfs);
            if (read_file_grow(p, &buf, &cap) > 0 && strstr(buf, "TCG version: 1.2"))
                f->tpm = 12;
            free(buf);
        }
    }

    char first[64] = "";
    sfmt(p, sizeof p, "%s/class/iommu", g_sysfs);
    f->iommu = count_dir(p, NULL, first, sizeof first);
    str_copy(f->iommu_kind, sizeof f->iommu_kind,
             !strncmp(first, "dmar", 4)   ? "VT-d"
             : !strncmp(first, "ivhd", 4) ? "AMD-Vi"
             : strstr(first, "smmu")      ? "SMMU"
                                          : "");
    sfmt(p, sizeof p, "%s/kernel/iommu_groups", g_sysfs);
    f->iommu_groups = count_dir(p, NULL, NULL, 0);
}

/* ---------------------------------------------------------------- in words */

void boot_text(const firmware_t *f, char *out, size_t n)
{
    out[0] = '\0';
    if (f->uefi < 0)
        return;
    if (!f->uefi) {
        sfmt(out, n, "legacy BIOS boot (no Secure Boot)");
        return;
    }
    sfmt(out, n, "UEFI%s", f->efi_bits == 32 ? " (32-bit)" : "");
    if (f->secure_boot == 1)
        sfmt(out + strlen(out), n - strlen(out), ", Secure Boot on");
    else if (f->secure_boot == 0)
        sfmt(out + strlen(out), n - strlen(out), ", Secure Boot off%s",
             f->setup_mode == 1 ? " (setup mode: no keys enrolled)" : "");
    if (f->lockdown[0] && strcmp(f->lockdown, "none"))
        sfmt(out + strlen(out), n - strlen(out), ", kernel lockdown %s", f->lockdown);
}

void virt_text(const firmware_t *f, char *out, size_t n)
{
    const char *nm = f->virt == 'v' ? "VT-x" : f->virt == 's' ? "AMD-V" : "";
    const char *opt = f->virt == 'v' ? "Intel Virtualization Technology" : "SVM Mode";
    out[0] = '\0';
    switch (f->virt_state) {
    case VIRT_ON:
        sfmt(out, n, "%s%son, KVM ready%s", nm, *nm ? " " : "", f->vm ? " (nested, in a virtual machine)" : "");
        break;
    case VIRT_NOKVM:
        sfmt(out, n, "%s: off in the firmware setup (%s), or KVM isn't loaded", nm, opt);
        break;
    case VIRT_OFF:
        sfmt(out, n, "%s switched off in the firmware setup (%s)", nm, opt);
        break;
    case VIRT_MAYBE_OFF:
        sfmt(out, n, "%s not available: off in the firmware setup, or the CPU has none", nm);
        break;
    case VIRT_NONE:
        sfmt(out, n, "none: the CPU has neither VT-x nor AMD-V");
        break;
    case VIRT_VM:
        sfmt(out, n, "not passed on to this virtual machine (nested virtualisation off)");
        break;
    default:
        break;
    }
}

void iommu_text(const firmware_t *f, char *out, size_t n)
{
    out[0] = '\0';
    if (f->iommu > 0) {
        sfmt(out, n, "on%s%s%s", f->iommu_kind[0] ? " (" : "", f->iommu_kind, f->iommu_kind[0] ? ")" : "");
        if (f->iommu_groups > 0)
            sfmt(out + strlen(out), n - strlen(out), ", %d groups", f->iommu_groups);
    } else if ((f->virt == 'v' || f->virt == 's') && !f->vm && f->virt_state != VIRT_NONE)
        sfmt(out, n, "off");
}
