# LHS — Linux Hardware Stats

A fast, lightweight terminal monitor for Linux, written in C. It shows everything your machine is doing on one screen: CPU, GPU, memory, power draw, disk and network throughput, temperatures, fans, and every process with its CPU, memory and disk I/O.

* **Fast**: reads `/proc` and `/sys` directly. It never spawns helper programs or parses tool output. One full sample of every subsystem takes about 0.3 ms.
* **Small**: about 130 KB binary, about 4 MB RAM, about 0.1% CPU at a 1 s refresh.
* **No hard dependencies** beyond ncurses. NVIDIA support is loaded at runtime only if the driver is installed.

## What it shows

| Area | Details | Source |
|---|---|---|
| CPU | total and per-core usage, user/sys/iowait/steal, per-core clock, package temperature, load average, context switches, interrupts and forks per second, usage history graph | `/proc/stat`, cpufreq, hwmon |
| GPU | load, VRAM, core and memory clock, temperature, power and power limit, fan | see below |
| Memory | used/available, cache, shared, dirty, swap usage, swap-in/out rate, major page faults | `/proc/meminfo`, `/proc/vmstat` |
| Power | CPU package, cores, iGPU (uncore), DRAM, platform (psys), GPU power, battery drain and time left, AC state, estimated total | RAPL powercap, hwmon, `power_supply` |
| Disks | read/write throughput, IOPS and busy % per physical drive, totals since boot, model, NVMe/SSD/HDD | `/proc/diskstats`, `/sys/block` |
| Filesystems | used/total for local filesystems | `/proc/self/mounts`, `statvfs` |
| Network | download/upload rate per interface, link state, speed, IPv4 address, totals | `/proc/net/dev`, `/sys/class/net` |
| Sensors | every temperature, fan and power sensor the kernel exposes (CPU cores, NVMe, chipset, GPU...) | `/sys/class/hwmon` |
| Processes | PID, user, state, threads, nice, CPU %, MEM %, RSS/VIRT, **disk read/write per process**, CPU time, full command line | `/proc/[pid]/*` |

### GPU support

| Vendor | How | Notes |
|---|---|---|
| Intel (i915) | engine busy % (3D / Video / Blit) from the i915 perf counters, the same source `intel_gpu_top` uses. Clocks from sysfs. iGPU power from RAPL "uncore". | Exact load needs root or `CAP_PERFMON`. Without it, LHS estimates load from the GPU's RC6 sleep time (shown as `Load~`). |
| AMD (amdgpu) | `gpu_busy_percent`, VRAM, plus hwmon temperature, power, clocks and fan | Works as a normal user. |
| NVIDIA (proprietary) | NVML (`libnvidia-ml.so`), loaded with `dlopen()` at startup | Works as a normal user. Nothing is needed at build time. |
| Others (nouveau, xe, ...) | whatever the driver exposes through hwmon | Load isn't available for these yet. |

## Build and install

You need a C compiler, `make` and the ncurses development headers.

```sh
# Debian / Ubuntu / Mint
sudo apt install build-essential libncurses-dev pkg-config
# Fedora
sudo dnf install gcc make ncurses-devel pkgconf
# Arch / Manjaro
sudo pacman -S --needed base-devel ncurses
# openSUSE
sudo zypper install gcc make ncurses-devel pkg-config

make
./lhs                     # run from the build folder
sudo make install         # installs to /usr/local/bin/lhs
```

`make debug` builds with AddressSanitizer and UBSan for development. `make uninstall` removes the installed binary.

## Usage

```
lhs [options]
  -d, --delay SEC   refresh interval in seconds (default 1, min 0.1)
  -1, --once        print one plain-text snapshot and exit (good for scripts / ssh)
  -n, --top N       processes listed by --once (default 15)
  -a, --ascii       plain ASCII drawing for terminals without Unicode
  -h, --help        help
  -v, --version     version
```

### Views and keys

| Key | Action |
|---|---|
| `1` `2` `3` / `Tab` | **Dashboard** (everything at once) / **Processes** (full table) / **Hardware** (all sensors, GPU details, RAPL domains, drives, filesystems, interfaces) |
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | move the selection, or scroll the Hardware view |
| `c` `m` `d` `p` `n` `t` | sort by CPU, memory, disk I/O, PID, name, CPU time. Press the same key again to reverse. |
| `r` | reverse the sort |
| `/` | filter by process name, command line, user or PID (`Enter` keeps it, `Esc` clears it) |
| `k` | show or hide kernel threads |
| `f` | selection follows the process instead of staying on the row |
| `+` `-` | slower / faster refresh (0.25 s to 10 s) |
| `Space` | pause sampling |
| `?` | help |
| `q` / `F10` | quit |

## Permissions: what needs root

Most things work as a normal user. Three readings are restricted by the kernel:

| Reading | Why | Options |
|---|---|---|
| CPU / DRAM / iGPU power (RAPL) | `energy_uj` is root-only since Linux 5.10 (PLATYPUS power side channel, CVE-2020-8694) | Run `sudo lhs`. See below for a permanent change. |
| Exact Intel GPU engine load | system-wide perf counters need `CAP_PERFMON` or `perf_event_paranoid <= 0` | Run `sudo lhs`, or `sudo setcap cap_perfmon=ep /usr/local/bin/lhs`. Without either, LHS falls back to the RC6 estimate. |
| Disk I/O of other users' processes | `/proc/PID/io` is protected like ptrace | Run `sudo lhs`. Your own processes always show I/O. |

**Making RAPL readable permanently (optional, single-user machines only).** This re-opens the side channel the kernel restriction closes, so only do it on a machine where you are the only user. Create `/etc/tmpfiles.d/lhs-rapl.conf`:

```
z /sys/class/powercap/intel-rapl:*/energy_uj   0444 - - -
z /sys/class/powercap/intel-rapl:*:*/energy_uj 0444 - - -
```

Then run `sudo systemd-tmpfiles --create` (it is also applied at every boot).

## How power is estimated

The Power panel title shows the best whole-machine figure it can find, in this order:

1. **Battery drain** when a laptop is discharging. This is the true total for the whole system.
2. **RAPL psys** (platform domain), on recent Intel laptops.
3. **CPU package + DRAM + discrete GPUs**. Integrated GPUs are already inside the package figure, so they aren't added twice. This leaves out the motherboard, drives, fans and PSU losses, so wall power will be higher.

## Project layout

```
src/main.c      argument parsing, main loop (sampling + input on one thread, no busy-waiting)
src/sys.c       ties the collectors together, history buffers, total-power estimate
src/cpu.c       /proc/stat, cpufreq, /proc/cpuinfo topology, load, uptime
src/mem.c       /proc/meminfo, /proc/vmstat
src/disk.c      /proc/diskstats (physical drives only), filesystems
src/net.c       /proc/net/dev, link state, addresses
src/proc.c      process table, per-process CPU / memory / disk I/O
src/sensors.c   hwmon temperatures, fans, power
src/gpu.c       Intel i915 (perf PMU + RC6), AMD amdgpu, NVIDIA NVML via dlopen, generic hwmon
src/power.c     RAPL powercap zones, batteries, AC adapter
src/ui.c        ncurses dashboard, process table, hardware view
src/report.c    --once plain-text snapshot
src/util.c      file reading, formatting, history ring buffer
```

Every collector keeps its own previous counters and turns them into rates using the real elapsed time between samples, so rates stay correct even if a frame is late.

### Testing against fake hardware

For development, `LHS_SYSFS`, `LHS_PROCFS` and `LHS_PCI_IDS` point LHS at a different `/sys`, `/proc` or `pci.ids`. That lets you test GPU, RAPL, battery and sensor code on machines without that hardware. These variables are ignored when the binary runs setuid or with file capabilities.

## Ideas for later

* sending signals to processes (kill / renice) from the process view
* tree view of processes
* per-process GPU usage (DRM fdinfo)
* Intel `xe` driver and ARM (Mali / Adreno devfreq) GPU load
* CSV / JSON logging mode
* configurable colour themes
