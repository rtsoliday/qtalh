# QtALH CPU benchmarks

The current comparison, measured on 2026-10-14, runs ALH 1.2.35 and QtALH against
the same private IOC with X11 rendering. QtALH uses less CPU than ALH in all three
workloads, including updates with file logging enabled.

## Results

Values are medians of five alternating trials per application and workload.
**100% means one fully occupied CPU core.** Application CPU includes its threads
and excludes the IOC, X server, CA repeater, and validation monitor.

| Workload | Trial duration | ALH CPU | QtALH CPU | QtALH versus ALH |
| --- | ---: | ---: | ---: | ---: |
| Main window, idle | 10 s | 1.0000% | 0.2000% | 80.0% lower |
| Updates, logging disabled | 10 s | 1.4000% | 0.5000% | 64.3% lower |
| Updates, logging enabled | 30 s | 1.3000% | 0.8000% | 38.5% lower |

| Workload | ALH range | QtALH range |
| --- | ---: | ---: |
| Idle | 0.9000–1.0000% | 0.2000–0.2000% |
| Updates, logging disabled | 1.3000–1.5000% | 0.5000–0.6000% |
| Updates, logging enabled | 1.2667–1.3000% | 0.7667–0.8333% |

Each direction holds in all five paired rounds, with non-overlapping observed
ranges. These are controlled local measurements, not a formal statistical
significance test or a performance guarantee for other installations.

ALH and QtALH divide rendering work differently between the application and X11.
Including the dedicated Xvfb process preserves the same ranking:

| Workload | ALH + Xvfb CPU | QtALH + Xvfb CPU |
| --- | ---: | ---: |
| Idle | 1.0000% | 0.2000% |
| Updates, logging disabled | 1.4000% | 0.5000% |
| Updates, logging enabled | 1.3333% | 0.8333% |

These are medians of per-trial totals, not sums of separate medians. The raw CSV
also records Xvfb CPU separately. Small or zero readings are limited by Linux
clock-tick resolution; this virtual display does not measure a desktop compositor
or GPU-backed rendering. Workloads use different trial durations, so comparisons
between rows do not isolate the marginal cost of enabling logging.
[Raw trials](benchmarks/alh-vs-qtalh-current-2026-10-14.csv). The previous
comparison (2026-09-14, Intel Core i7-12700) is retained in
[its raw trials](benchmarks/alh-vs-qtalh-current-2026-09-14.csv).

## Effect of the October 2026 optimizations

The same harness, host, and settings compared the QtALH build before and after
the CPU optimizations described below:

| Workload | Before | After | Change |
| --- | ---: | ---: | ---: |
| Main window, idle | 0.2000% | 0.1000% | 50.0% lower |
| Updates, logging disabled | 0.6000% | 0.5000% | 16.7% lower |
| Updates, logging enabled | 1.5000% | 0.8333% | 44.4% lower |

[Raw trials](benchmarks/qtalh-before-after-2026-10-14.csv). The logged ranges do
not overlap; the idle and unlogged ranges do, because ten-second trials are close
to clock-tick resolution. Twenty-second `perf stat` task-clock measurements of
the same workloads (means of two alternating trials) gave 0.19% → 0.18% idle and
0.67% → 0.56% for updates without logging. With all 1,000 channels in the
displayed group, updates without logging fell from 0.97% to 0.73%. Startup CPU
with 20,000 unreachable channels fell from 1.23 s to 0.20 s.

The changes do not alter configuration, log, checkpoint, or queue formats:

- Alarm-log fingerprints cache each ring slot's hash, so a write hashes only the
  new record and the 64-byte checkpoint. SHA-256 uses x86 SHA extensions when
  the CPU provides them, with a portable fallback. Digests are identical to
  `QCryptographicHash`; recovery and the live log browser use the same code.
- Log records, history entries, and severity/status names are built without
  temporary strings. Database-queue user, host, and display tokens are resolved
  once per writer, as ALH's `getUserInfo` does.
- The views repaint only rows whose displayed values changed. Group views also
  re-measure only changed rows; the current row is repainted in full, which
  also fixes a stale styled selection highlight.
- Channel Access looks up channels by name instead of scanning every channel,
  and its periodic poll scans channels only while access rechecks or connection
  deadlines are pending. Regular expressions are compiled once.

## Workload and environment

- 1,000 channels in ten groups; active records alternate normal/MAJOR every
  0.5 seconds, nominally 2,000 transitions/second.
- Global acknowledgement mode, muted sound, default legacy appearance, and
  1000×600 main windows. QtALH also displays its default 220×35 runtime indicator;
  ALH does not display an equivalent separate runtime window with `-mainwindow`.
- Three-second warmup before timing. Each trial starts a fresh application;
  logged trials use fresh directories and the default 2,000-record circular log.
  Printer/database helpers are not configured.
- Private 1600×1000×24 Xvfb display, isolated Qt settings, and loopback-only CA
  discovery. IOC records use `PINI=YES`, idle `CALC=0`, and active `CALC=10-VAL`.
- Intel Core Ultra 9 285K (8 performance and 16 efficient cores, no SMT), Linux
  x86-64 (kernel 5.14), GCC 11.3.0, Qt 5.15.9, EPICS Base 7.0.8, the
  `performance` governor, ALH `-O2 -g -Wall -std=gnu99`, and QtALH
  `-O2 -g -Wall -Wextra -std=c++17 -fPIC` builds.
- The application uses logical CPU 2, IOC CPU 5, Xvfb CPU 6, and repeater/monitor
  CPU 7, all performance cores. Affinity is set before launch, inherited by new
  threads, and checked on the main thread after warmup. The cores are not
  exclusively reserved; other host activity and CPU frequency are not fixed.

Idle, updates, and logged updates run in that order. Trials 1/3/5 run ALH first;
trials 2/4 run QtALH first. No builds, test suites, or profilers run alongside
timing. Window inspection runs after the CPU measurement endpoints. Linux process accounting has about 0.1 percentage-point resolution over
ten seconds and 0.033 over thirty seconds.

QtALH analytics remains attached, with no open analytics dashboard or configured
notification subscriptions. Other layouts, channel counts, fonts, filtering
modes, operator interactions, and deployment platforms can change CPU use.
The logged comparison retains each application's existing persistence behavior;
see [logging implementation](qtalh-logging-analysis.md).

## Validation

All 30 measured runs of each comparison passed. Every active trial produced both
normal and MAJOR root-severity outputs during timing: 20 changes per unlogged
trial and 60 per logged trial. Each logged run retained exactly 2,000 lines
containing both states for every one of the 1,000 channels. Logging-disabled runs
produced no alarm or operation files. Application diagnostic logs were empty.

These checks verify aggregate processing and retained per-channel states; they
do not count every callback or prove zero event loss. Startup, graceful shutdown,
and crash-recovery durability are outside the timed workload. The applications
are terminated with SIGTERM after measurement. The optimized build also passed
the core, UI (both appearances), IOC, helper, notification, analytics, gallery,
and X11 visual suites; see the [test guide](../qtalh/tests/README.md) for coverage.

## Measured builds

The source base at measurement was `08d2aa81632552a5f9992659e013997b8b43aa78`.
ALH source was unchanged. "Before" is that revision; "after" and the ALH
comparison's QtALH add the optimizations above. SHA-256 identifies the exact
executables measured:

```text
alh    35d3b2f3c9e0145705eb4f7ced15bab9c94fe2108a80985f2c473c3eccdf5414
before 9d441b171319074f4b7f8dee97b69e42b7c0c705a33119f7b0f0d2e6ab299d5c
after  b9ec1cfede38766191dd01eea3668fc6bc6abe11d668c20c992d7fe924ba3116
```

## Reproduction

The [IOC benchmark](../qtalh/tests/benchmark_ioc.py) accepts executable paths,
EPICS tool paths, and CPU assignments on the command line. It requires Linux
`/proc`, Python 3, `stdbuf`, and EPICS `softIoc`, `caRepeater`, `caget`, `caput`,
and `camonitor`. `--xvfb` additionally requires Xvfb and `xwininfo`; `--cpus`
requires `taskset` and four allowed CPUs on distinct physical cores. Check
`lscpu -e=CPU,CORE,SOCKET` before choosing CPUs on another host.

```sh
make -C alh EPICS_BASE=/path/to/epics/base
make -C qtalh QT_VERSION=5 EPICS_BASE=/path/to/epics/base
python3 qtalh/tests/benchmark_ioc.py \
  bin/Linux-x86_64/alh bin/Linux-x86_64/qtalh --labels alh qtalh \
  --epics-bin /path/to/epics/base/bin/linux-x86_64 \
  --xvfb --cpus 2 5 6 7 --channels 1000 --max-records 2000 \
  --seconds 10 --logged-seconds 30 --trials 5 \
  --output /tmp/alh-vs-qtalh-repeat
```

Use the same options for the published workload, adjusting executable/EPICS paths
and CPU assignments for the target host. The tool uses global mode and muted
sound for both applications. `--modes idle updates logged-updates` selects the
workloads, which are all enabled by default. Log capacity must allow at least
two records per channel so every channel's normal and MAJOR states can be checked.

CSV is written to stdout. `--output` must name a new directory and retains the
CSV, configuration, executable hashes, commands, logs, window trees, and validation
records for that run. Without it, temporary evidence is removed on exit. These
local run artifacts do not need to be committed alongside the published CSV.
The runner stops the processes it owns on completion, failure, or interruption.

Without `--cpus`, processes inherit the caller's affinity. Without `--xvfb`, set
`DISPLAY` or `QT_QPA_PLATFORM` yourself; X-server CPU fields are then empty
(unavailable). Use X11 when comparing with Motif ALH. Offscreen rendering is
suitable only for QtALH comparisons and does not reproduce the published workload.

For isolated QtALH engine/UI investigations, the separate in-process harness
supports `idle`, `updates`, `filtered`, and `runtime` workloads:

```sh
make -C qtalh QT_VERSION=5 benchmark-cpu
QT_QPA_PLATFORM=offscreen qtalh/O.Linux-x86_64-qt5/benchmark_cpu 10000 updates 10
```

This harness verifies delivered events and final channel/root state. It does not
include Channel Access or run ALH; use the complete-application runner for the
published comparison.
