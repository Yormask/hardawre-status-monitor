# hsm — hardware status monitor

A fast, lightweight hardware and process monitor for the Linux terminal, written in C. It shows everything your machine is doing on one screen: CPU, GPU, memory, power draw, disk and network throughput, temperatures, fans, and every process with its CPU, memory and disk I/O. You choose which panels appear, arrange them, and pick how bars and graphs look. Colours are your terminal's standard ones, so they follow your terminal's theme. It can also **log** any sensor, or single processes, to CSV files, so a crash caused by heat or overload can be traced afterwards.

* **Fast**: reads `/proc` and `/sys` directly. It never spawns helper programs or parses tool output. One full sample of every subsystem takes about 0.3 ms.
* **Small**: about 250 KB binary, about 4 MB RAM, about 0.1% CPU at a 1 s refresh.
* **Logging**: record temperatures, load, clocks, power, fans, disk and network to a CSV file, and follow individual processes in their own files. It works from the screen or headless (`hsm --log`).
* **Customizable**: show, hide and rearrange panels; choose bar style, graph style and colour mode. Settings are saved automatically.
* **No hard dependencies** beyond ncurses. NVIDIA support is loaded at runtime only if the driver is installed.

## What it shows

| Panel | Details | Source |
|---|---|---|
| CPU | total and per-core bars (usage, clock speed and/or per-core temperature, in any combination), user/sys/iowait/steal, average clock, package temperature and power, load average, context switches, interrupts and forks per second, history graph | `/proc/stat`, cpufreq, hwmon, RAPL |
| GPU | load, VRAM, core clock bar, memory clock, temperature, power and power limit, fan | see below |
| Memory | used/available, cache, shared, dirty, swap usage, swap-in/out rate, major page faults | `/proc/meminfo`, `/proc/vmstat` |
| Power | CPU package, cores, iGPU (uncore), DRAM, platform (psys), GPU power, battery drain and time left, AC state, estimated total | RAPL powercap, hwmon, `power_supply` |
| Disk I/O | read/write throughput, IOPS and busy % per physical drive | `/proc/diskstats`, `/sys/block` |
| Network | download/upload rate per interface, link speed, IPv4 address | `/proc/net/dev`, `/sys/class/net` |
| Temperatures | every temperature sensor (CPU package and each core, NVMe drives, GPU, chipset, Wi-Fi...) with CPU temperature history | `/sys/class/hwmon` |
| Fans | every fan speed sensor, plus NVIDIA GPU fans | `/sys/class/hwmon`, NVML |
| Filesystems | used/total for local filesystems | `/proc/self/mounts`, `statvfs` |
| Processes | PID, user, state, threads, nice, CPU %, MEM %, RSS/VIRT, **disk read/write per process**, CPU time, full command line | `/proc/[pid]/*` |

The **Hardware** view (key `3`) lists every sensor, GPU detail, RAPL power domain, drive (with totals since boot), filesystem and network interface.

### GPU support

| Vendor | How | Notes |
|---|---|---|
| Intel (i915) | engine busy % (3D / Video / Blit) from the i915 perf counters, the same source `intel_gpu_top` uses. Clocks from sysfs. iGPU power from RAPL "uncore". | Exact load needs root or `CAP_PERFMON`. Without it, hsm estimates load from the GPU's RC6 sleep time (shown as `Load~`). |
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

git clone https://github.com/Yormask/linux-hardware-stats
cd linux-hardware-stats
make
./hsm                     # run from the build folder
sudo make install         # installs to /usr/local/bin/hsm
```

After installing, run `hsm` from anywhere. Use `sudo hsm` to also see power and exact Intel GPU load, and to signal other users' processes (see [Permissions](#permissions-what-needs-root)).

`make debug` builds with AddressSanitizer and UBSan for development. `make uninstall` removes the installed binary.

**Upgrading from an older version** that was called `lhs` or `hardware-stats`: remove the old binary with `sudo rm -f /usr/local/bin/lhs /usr/local/bin/hardware-stats`.

## Usage

```
hsm [options]
  -d, --delay SEC   refresh interval in seconds (default: saved setting or 1)
  -1, --once        print one plain-text snapshot and exit (good for scripts / ssh)
  -n, --top N       processes listed by --once (default 15)
  -a, --ascii       plain ASCII drawing for terminals without Unicode
  -h, --help        help
  -v, --version     version, copyright and license

Logging without the screen (runs until Ctrl+C, kill, or the logged processes exit):
  --log             log the values chosen in the Logging view (5) to a CSV file
  --log-pid PID     log one process's usage (can be given up to 16 times)
  --log-every SEC   seconds between rows (default: the saved setting)
  --log-dir DIR     where to write the files (default: ~/.local/state/hsm/logs)
```

### Keys

| Key | Action |
|---|---|
| `1` `2` `3` `4` `5` / `Tab` | **Dashboard** / **Processes** (full table) / **Hardware** (every sensor and device) / **Setup** / **Logging** |
| `s` / `F2` | open Setup |
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | move the selection, or scroll the Hardware view |
| `l` | start or stop logging sensors to a CSV file (see [Logging](#logging)) |
| `L` | start or stop logging the selected process |
| `x` / `F9` | send a signal to (kill) the selected process |
| `c` `m` `d` `p` `n` `t` | sort by CPU, memory, disk I/O, PID, name, CPU time. Press the same key again to reverse. |
| `r` | reverse the sort |
| `/` | filter by process name, command line, user or PID (`Enter` keeps it, `Esc` clears it) |
| `b` | CPU core bars: cycle through usage, clock, temperature and their combinations |
| `k` | show or hide kernel threads |
| `f` | selection follows the process instead of staying on the row |
| `+` `-` | slower / faster refresh (0.25 s to 10 s) |
| `Space` | pause sampling (logging pauses too) |
| `?` | help |
| `q` / `F10` | quit |

## Customizing the dashboard

Press `s` (or `4`) to open **Setup**. Changes apply immediately and are saved automatically.

**Dashboard panels** (left side of Setup): the dashboard has a left and a right column, with the process list below.

| Key | Action |
|---|---|
| `↑` `↓` | choose a panel |
| `Space` / `Enter` | show or hide it |
| `[` `]` or `Shift`+`↑` `↓` | move it up or down (it moves between columns at the ends) |
| `←` `→` | move it to the left or right column |
| `R`, then `Enter` or `R` | reset **all** settings to the defaults |
| `Esc` | back to the dashboard |

A live **Preview** shows where every panel will go. If a column ends up empty, the other one uses the full width.

**Appearance & behaviour** (use `←` `→` to change a value; each style shows a sample):

| Option | Choices |
|---|---|
| Process list | shown / hidden below the panels |
| Bar style | Blocks `████▌···`, Squares `■■■■■■`, Line `━━━━──`, Classic `[|||   ]` |
| Graphs | Blocks, Braille (twice the detail), Off (more room for everything else) |
| Colours | By value (green / yellow / red), Gradient (colour changes along the bar), Single colour, Monochrome |
| Colour | only in Single colour mode: cyan, blue, green, yellow, red, magenta, white, or the bright version of each (the bright ones need a 16-colour terminal) |
| Left column width | Automatic or 35% to 70% |
| Refresh interval | 0.25 s to 10 s |
| Kernel threads | shown / hidden in the process list |

**CPU core bars**: switch each kind on or off independently, so a core can show one, two or all three bars side by side:

| Bar | Shows |
|---|---|
| Usage | how busy the core is |
| Clock speed | current clock compared with the CPU's maximum |
| Temperature | that core's temperature (per core on Intel; package temperature on AMD) |

At least one kind stays on. The `b` key cycles through all the combinations without opening Setup.

**Reset**: the settings list ends with *Reset ALL settings to defaults* (or press `R` anywhere in Setup). It asks you to press Enter a second time, so nothing is lost by accident; any other key cancels.

Any panel with spare height uses it for a history graph.

### The config file

Settings live in `~/.config/hsm/config` (or `$XDG_CONFIG_HOME/hsm/config`). It's plain text, so you can also edit it by hand:

```
left = cpu, temps, gpu
right = memory, power, disk, network, fans, filesystems
hidden = filesystems
processes = on
left_width = auto
bars = blocks          # blocks | squares | line | classic
graphs = braille       # blocks | braille | off
colors = gradient      # value | gradient | single | mono
cpu_bars = usage, temp # any of: usage, clock, temp  (or: all)
single_color = green   # used by colors = single; cyan blue green yellow red magenta white,
                       # or bright-cyan, bright-blue, ... bright-white
interval = 1
sort = cpu
reverse = off
kernel_threads = off
log_dir = default      # or a folder, e.g. ~/hsm-logs
log_every = 5          # seconds between log rows, or "refresh" for every update
log_skip = net.eth0.rx, net.eth0.tx   # values left out of the system log
```

hsm saves the file when you change something in Setup, and on exit only if you changed something during that session, so hand edits aren't overwritten. When you run `sudo hsm`, it uses *your* config file (not root's) and reads and writes it as you, so the file stays owned by you.

## Logging

Logs help you find out what happened before a crash, a shutdown or a slowdown: how hot the CPU got, which process was using it, whether a fan stopped, how hard a disk was working. Everything is written as CSV, so you can open the files in LibreOffice Calc, Excel or Google Sheets, or plot them with any tool.

**System log.** Press `5` to open the **Logging** view. It lists every value hsm can record, grouped by CPU, CPU cores, Memory, GPU, Power, Temperatures, Fans, Disks and Network, each with its current reading:

| Key | Action |
|---|---|
| `↑` `↓` `←` `→` `PgUp` `PgDn` | move through the list |
| `Space` / `Enter` | select or deselect a value |
| `a` / `n` | select all / none |
| `i` | time between rows: every refresh, 1, 2, 5, 10, 30 or 60 seconds |
| `l` | start / stop the log (this key works in every view) |

Everything is selected at first. Your selection and interval are saved. While a log is running, the header shows a red **● REC** and the selection can't change, so every row has the same columns.

**Process logs.** Select a process in the Processes view (`2`) or the dashboard's process list and press `L`. Its row gets a red `●` and its usage goes to its own file. Press `L` on it again (or `Space` on it in the Logging view) to stop. Up to 16 processes can be logged at once. When a process exits, its log is closed and the footer tells you.

**Where the files go.** `~/.local/state/hsm/logs/` (or `$XDG_STATE_HOME/hsm/logs/`), or the folder set by `log_dir` in the config file. Under `sudo hsm` they still go to *your* folder and belong to you. A new file is made each time you start a log; nothing is ever overwritten.

```
hsm-system-2026-11-11_18-08-27.csv
hsm-process-1288-firefox-2026-11-11_18-08-49.csv
```

System log columns: `time`, `elapsed_s`, then one column per selected value with its unit in the heading:

```
time,elapsed_s,"CPU total (%)","CPU temperature (C)","Average CPU clock (MHz)","GPU load (%)",...
2026-11-11 18:08:27,0.0,12.5,54.0,3602,8.0,...
```

Process log columns: `time`, `elapsed_s`, `pid`, `state`, CPU (% of one core), memory %, RSS (MB), threads, disk read and write (MB/s), and the whole system's CPU % and CPU temperature next to it, so you can see whether the process was the cause.

A value that can't be read at that moment (for example RAPL power without root) is left empty. Every row is written as soon as it is taken, and the file is synced to disk every 5 seconds, so a log survives a crash or power loss with at most the last few seconds missing.

**Logging in the background (no screen).** For tracking down a crash that happens overnight, run hsm headless:

```sh
hsm --log --log-every 5                      # the values chosen in the Logging view
hsm --log-pid 4321 --log-every 1             # one process; stops when it exits
sudo hsm --log --log-pid 4321                # with RAPL power; files still owned by you
nohup hsm --log --log-every 10 >/dev/null 2>&1 &   # keeps running after you log out
```

It prints where the files are, writes until `Ctrl+C` or `kill`, then prints how many rows were saved. To start it at every boot, a small systemd service works well (`/etc/systemd/system/hsm-log.service`, then `sudo systemctl enable --now hsm-log`):

```ini
[Unit]
Description=hsm hardware log

[Service]
ExecStart=/usr/local/bin/hsm --log --log-every 10 --log-dir /var/log/hsm
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Each start makes a new file, so old logs are kept; delete the ones you don't need now and then.

## Killing processes

Select a process in the dashboard's process list or the Processes view (`2`), press `x` and choose a signal:

| Signal | What it does |
|---|---|
| SIGTERM | asks the program to quit (normal, safe default) |
| SIGKILL | forces it to stop immediately; it can't clean up |
| SIGINT | interrupt, like pressing Ctrl+C |
| SIGHUP | "hang up"; many services reload their config |
| SIGSTOP | pauses (freezes) the process |
| SIGCONT | resumes a paused process |

Before sending, hsm checks that the PID still belongs to the same process you selected (PIDs get reused when programs exit). The footer shows the result, for example "Permission denied" if the process belongs to another user.

## Permissions: what needs root

Most things work as a normal user. A few are restricted by the kernel:

| Reading | Why | Options |
|---|---|---|
| CPU / DRAM / iGPU power (RAPL) | `energy_uj` is root-only since Linux 5.10 (PLATYPUS power side channel, CVE-2020-8694) | Run `sudo hsm`. See below for a permanent change. |
| Exact Intel GPU engine load | system-wide perf counters need `CAP_PERFMON` or `perf_event_paranoid <= 0` | Run `sudo hsm`, or `sudo setcap cap_perfmon=ep /usr/local/bin/hsm`. Without either, it falls back to the RC6 estimate. |
| Disk I/O of other users' processes | `/proc/PID/io` is protected like ptrace | Run `sudo hsm`. Your own processes always show I/O. |
| Signals to other users' processes | normal Unix permissions | Run `sudo hsm`. |

**Making RAPL readable permanently (optional, single-user machines only).** This re-opens the side channel the kernel restriction closes, so only do it on a machine where you are the only user. Create `/etc/tmpfiles.d/hsm-rapl.conf`:

```
z /sys/class/powercap/intel-rapl:*/energy_uj   0444 - - -
z /sys/class/powercap/intel-rapl:*:*/energy_uj 0444 - - -
```

Then run `sudo systemd-tmpfiles --create` (it is also applied at every boot).

**"unknown terminal type"?** Your `TERM` names a terminal ncurses doesn't know. Run `TERM=xterm-256color hsm`, or install your distribution's `ncurses-term` package.

**No fans in the Fans panel?** Many desktop boards only report fan speeds once their Super I/O sensor driver is loaded. Install `lm-sensors` and run `sudo sensors-detect`.

## How power is estimated

The Power panel title shows the best whole-machine figure it can find, in this order:

1. **Battery drain** when a laptop is discharging. This is the true total for the whole system.
2. **RAPL psys** (platform domain), on recent Intel laptops.
3. **CPU package + DRAM + discrete GPUs**. Integrated GPUs are already inside the package figure, so they aren't added twice. This leaves out the motherboard, drives, fans and PSU losses, so wall power will be higher.

## Project layout

```
src/main.c        argument parsing, main loop (sampling + input on one thread, no busy-waiting), headless --log
src/config.c      settings file: load, save, defaults (sudo-safe)
src/sys.c         ties the collectors together, history buffers, total-power estimate
src/cpu.c         /proc/stat, cpufreq, topology, /proc/cpuinfo, load, uptime
src/mem.c         /proc/meminfo, /proc/vmstat
src/disk.c        /proc/diskstats (physical drives only), filesystems
src/net.c         /proc/net/dev, link state, addresses
src/proc.c        process table, per-process CPU / memory / disk I/O, safe signal sending
src/sensors.c     hwmon temperatures, fans, power; per-core temperature lookup
src/gpu.c         Intel i915 (perf PMU + RC6), AMD amdgpu, NVIDIA NVML via dlopen, generic hwmon
src/power.c       RAPL powercap zones, batteries, AC adapter
src/ui.c          header, footer, help, signal dialog, keys, view switching
src/ui_prim.c     drawing primitives: text, boxes, bar styles, block/braille graphs, colour modes
src/ui_panels.c   dashboard panels and the two-column layout engine
src/ui_procs.c    process table: sorting, filtering, selection
src/ui_hw.c       Hardware view
src/ui_setup.c    Setup screen: panels, styles, CPU bars, single colour, reset
src/ui_log.c      Logging view: choose values, start / stop logs, process logs
src/log.c         logging: the list of values, CSV system and process logs (sudo-safe)
src/report.c      --once plain-text snapshot
src/util.c        file reading, formatting, history ring buffer
```

Every collector keeps its own previous counters and turns them into rates using the real elapsed time between samples, so rates stay correct even if a frame is late.

### Testing against fake hardware

For development, `HSM_SYSFS`, `HSM_PROCFS` and `HSM_PCI_IDS` point the program at a different `/sys`, `/proc` or `pci.ids`. That lets you test GPU, RAPL, battery, fan and sensor code on machines without that hardware. These variables are ignored when the binary runs setuid or with file capabilities.

## Ideas for later

* renice processes from the process view
* tree view of processes
* per-process GPU usage (DRM fdinfo)
* Intel `xe` driver and ARM (Mali / Adreno devfreq) GPU load
* view a saved log as graphs inside hsm
* JSON output for --once

## License

Copyright (C) 2026  Yormask

hsm is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

It is distributed in the hope that it will be useful, but **without any warranty**; without even the implied warranty of merchantability or fitness for a particular purpose. See the [LICENSE](LICENSE) file for the full text.
