# QtALH CPU benchmarks

The current comparison, measured on 2026-09-14, runs ALH 1.2.35 and QtALH against
the same private IOC with X11 rendering. QtALH uses less CPU while idle and during
updates without logging; ALH uses less CPU when file logging is enabled.

## Results

Values are medians of five alternating trials per application and workload.
**100% means one fully occupied CPU core.** Application CPU includes its threads
and excludes the IOC, X server, CA repeater, and validation monitor.

| Workload | Trial duration | ALH CPU | QtALH CPU | QtALH versus ALH |
| --- | ---: | ---: | ---: | ---: |
| Main window, idle | 10 s | 0.9999% | 0.5000% | 50.0% lower |
| Updates, logging disabled | 10 s | 3.6998% | 2.4998% | 32.4% lower |
| Updates, logging enabled | 30 s | 3.2666% | 4.7666% | 45.9% higher |

| Workload | ALH range | QtALH range |
| --- | ---: | ---: |
| Idle | 0.8999–1.0999% | 0.4000–0.5000% |
| Updates, logging disabled | 3.1998–3.8998% | 2.3999–2.6999% |
| Updates, logging enabled | 3.1999–3.5000% | 4.5999–4.9666% |

Each direction holds in all five paired rounds, with non-overlapping observed
ranges. The logged difference is 1.5000 percentage points of one core. These are
controlled local measurements, not a formal statistical significance test or a
performance guarantee for other installations.

ALH and QtALH divide rendering work differently between the application and X11.
Including the dedicated Xvfb process preserves the same ranking:

| Workload | ALH + Xvfb CPU | QtALH + Xvfb CPU |
| --- | ---: | ---: |
| Idle | 0.9999% | 0.5000% |
| Updates, logging disabled | 3.7998% | 2.5999% |
| Updates, logging enabled | 3.4333% | 4.9332% |

These are medians of per-trial totals, not sums of separate medians. The raw CSV
also records Xvfb CPU separately. Small or zero readings are limited by Linux
clock-tick resolution; this virtual display does not measure a desktop compositor
or GPU-backed rendering. Workloads use different trial durations, so comparisons
between rows do not isolate the marginal cost of enabling logging.
[Raw trials](benchmarks/alh-vs-qtalh-current-2026-09-14.csv).

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
- Intel Core i7-12700, Linux x86-64, GCC 11.5.0, Qt 5.15.9, EPICS Base 7.0.8,
  the existing powersave governor, ALH `-O2 -g -Wall -std=gnu99`, and QtALH
  `-O2 -g -Wall -Wextra -std=c++17 -fPIC` builds.
- The application uses logical CPU 2, IOC CPU 6, Xvfb CPU 10, and repeater/monitor
  CPU 14. These map to physical cores 1, 3, 5, and 7 on this host. Affinity is
  set before launch, inherited by new threads, and checked on the main thread
  after warmup. The cores are not exclusively reserved; other host activity
  and CPU frequency are not fixed.

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

All 30 measured runs passed. Every active trial produced both normal and MAJOR
root-severity outputs during timing: 20 changes per unlogged trial and 60 per
logged trial. Each logged run retained exactly 2,000 lines containing both
states for every one of the 1,000 channels. Logging-disabled runs produced no
alarm or operation files. Application diagnostic logs were empty.

These checks verify aggregate processing and retained per-channel states; they
do not count every callback or prove zero event loss. Startup, graceful shutdown,
and crash-recovery durability are outside the timed workload. The applications
are terminated with SIGTERM after measurement. The same QtALH build also passed
98 IOC integration checks; see the [test guide](../qtalh/tests/README.md) for coverage.

## Measured builds

The source base at measurement was `c7b67219cd451d92a8df9bb60b8d5993890a2622`.
ALH source was unchanged. QtALH additionally included the snapshot metadata
cache, bounded text-width cache, unused alarm-log formatting bypass, and legacy
runtime-font fix. SHA-256 identifies the exact executables measured:

```text
alh 526082cfd1a9303c74da4baf8b393383a7ed30d0f16b765f60a43b2963d5291d
qtalh 5cb366f78bc74ecbfb53702d7e811c65fac6b7110e9ccf571d55d7e62b2738d3
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
  --xvfb --cpus 2 6 10 14 --channels 1000 --max-records 2000 \
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
