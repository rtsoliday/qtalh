# QtALH CPU benchmarks

The [persistent logging checkpoint implementation](qtalh-logging-analysis.md#implemented-changes)
addresses the logging cost below. Earlier measurements remain historical baselines.

Measured on 2026-09-10 using an Intel Core i7-4770 (3.40 GHz), Linux x86-64,
Qt 5.15.3, EPICS Base 7.0.8, and the normal `-O2` build. Values below are
median process CPU percentages from three 10-second trials. **100% means one
fully occupied CPU core**, not the entire machine. Startup is excluded.

## Engine and UI workloads

`tests/benchmark_cpu.cc` constructs 10,000 channels in 100 groups and runs the
actual Window and Engine event loops with Qt's offscreen platform. The update
workloads deliver 100 transitions every 50 ms, alternating normal/MAJOR for each
channel. Every trial delivered 20,000 transitions and verified each channel's
final severity and the root's aggregate counts. File logging and audible sound
are disabled in both versions; Channel Access is measured separately below.

| Workload | Before CPU | After CPU | Reduction |
| --- | ---: | ---: | ---: |
| Main window, idle | 0.8517% | 0.0255% | 97.0% |
| Main window, 2,000 transitions/second | 8.1621% | 5.6344% | 31.0% |
| Active-alarm filter, 2,000 transitions/second | 5.9555% | 4.5215% | 24.1% |
| Runtime window only, idle | 0.6561% | 0.0262% | 96.0% |

[Individual trials](benchmarks/qtalh-cpu-synthetic-2026-09-10.csv).

## Complete application with Channel Access and X11

The actual QtALH executables monitored 1,000 local `calc` records, grouped into
10 groups, with the main window rendered through Xvfb/X11. Active records scanned
every 0.5 seconds and alternated 0/10 across a MAJOR threshold. The logging-enabled
runs used the default 2,000-record circular alarm log and a fresh log directory
for every trial; both normal and MAJOR transitions were verified in the output.

| Workload | Before CPU | After CPU | Reduction |
| --- | ---: | ---: | ---: |
| Idle | 0.5994% | 0.2997% | 50.0% |
| 2,000 transitions/second, file logging off | 5.2948% | 3.0982% | 41.5% |
| 2,000 transitions/second, file logging on | 32.0682% | 29.3802% | 8.4% |

[Individual trials](benchmarks/qtalh-cpu-ioc-2026-09-10.csv).
At that checkpoint, the logged workload remained dominated by per-record
file/position writes;
those writes and their durability were preserved. An exploratory run that
reused log paths was excluded from the logged results in favor of matched
initial files. These full-application figures are more representative of a
connected installation than the isolated engine/UI figures above.

## Legacy ALH comparison (2026-09-11)

Both legacy ALH and the QtALH executable after the engine/UI optimization
(but before the logging checkpoint optimization) were rerun through the same
complete-application IOC benchmark described above: 1,000 channels, 10 groups,
three 10-second trials after three seconds of warmup, and a private 1600x1000
Xvfb/X11 display. The same local IOC supplied both applications. Execution order
alternated, sound was muted, and each logging trial used a fresh directory and
the default 2,000-record ring. CPU percentages exclude the IOC and X server.
No application code was changed for this comparison.

| Workload | ALH CPU | QtALH CPU | QtALH relative to ALH |
| --- | ---: | ---: | ---: |
| Idle | 0.9990% | 0.2997% | 70.0% lower |
| 2,000 transitions/second, file logging off | 4.2956% | 2.8970% | 32.6% lower |
| 2,000 transitions/second, file logging on | 2.8970% | 30.2713% | 10.45 times ALH |

[Individual trials](benchmarks/alh-vs-qtalh-cpu-2026-09-11.csv).
That QtALH version used less CPU while idle and with file logging disabled, but
substantially more CPU with circular logging enabled. The later
[checkpoint measurements](qtalh-logging-analysis.md#measured-result) supersede
this circular-logging result for the current implementation. Every logged trial
passed the normal/MAJOR
transition check; inspected ring files contained all 1,000 distinct PV names.

The [follow-up logging investigation](qtalh-logging-analysis.md) confirms that
alarm payloads match and attributes 74% of circular-logging CPU samples to
position-file persistence.

Source inspection of that version showed additional per-record work:
it hashed changed records and atomically replaced a position-metadata file on
each write (`AlarmLogFile::savePosition` in `qtalh/services/logging.cc`). Legacy
`filePrintf` in `alh/alLog.c` writes and flushes the alarm record directly, with
its fixed-length ring cursor kept in memory. This difference is a likely
contributor to the logging gap; this comparison does not separately profile
how much CPU each logging operation consumes.

The earlier 10,000-channel in-process benchmark uses QtALH's Window/Engine APIs
and cannot directly run legacy ALH. The common IOC benchmark above provides the
comparison using both real applications. To reproduce it with the existing
benchmark script, its `before` column represents ALH and `after` represents QtALH:

```sh
DISPLAY=:97 QT_QPA_PLATFORM=xcb python3 qtalh/tests/benchmark_ioc.py \
  bin/Linux-x86_64/alh bin/Linux-x86_64/qtalh \
  --epics-bin /path/to/base/bin/linux-x86_64 \
  --channels 1000 --seconds 10 --trials 3
```

Executable SHA-256 values for this comparison:

```text
ALH:   8c06b7930dbbbb1eb92ced665ea107409e60c545985c99e656caa4fcee8312da
QtALH: 70f56a05261089abdc15de6cd9befd34d3e09e7b703f82476d823ddcde81e23d
```

## Implementation

`perf` samples identified recurring document traversal/state lookup in idle
`Engine::tick()`, view refresh work, and repeated local-time conversion in alarm
history during bursts. The changes are:

- Track the earliest alarm-filter or NoAck deadline. Keep the existing 200 ms
  tick, expiry traversal order, deferred write retries, and separate heartbeat
  timer; scan alarm states only when a deadline can have elapsed. Callers that
  obtain mutable state retain the conservative scan behavior.
- Separate one-second status/blink updates from full model refreshes. Alarm
  changes still schedule the existing 200 ms UI refresh. Operation and description
  changes explicitly notify the UI. Logging ownership, silence expiry, live
  dialogs, and the runtime alarm indicator continue updating while idle.
- Reuse the history timestamp within its existing one-second precision, including
  clock reversals and delayed filter events. Every history entry and alarm/log
  callback is still processed.
- Repaint the runtime button only when its color changes, and replace history
  text only when its contents change.

No Channel Access polling interval, alarm transition processing, log durability,
command execution, audible alarm behavior, or display content was removed or
throttled. These measurements describe controlled local workloads; CPU use with
other group layouts, IOC traffic, logging modes, fonts, and display servers will
vary. The very small idle values are especially sensitive to timer alignment
and measurement noise.

## Reproduction

Build and run the engine/UI workloads:

```sh
make -C qtalh QT_VERSION=5 benchmark-cpu
# Individual workload: CHANNEL_COUNT MODE DURATION_SECONDS
QT_QPA_PLATFORM=offscreen qtalh/O.Linux-x86_64-qt5/benchmark_cpu 10000 updates 10
```

Keep a copy of the application/benchmark executables before applying changes to
compare versions. The standalone benchmark modes are `idle`, `updates`,
`filtered`, and `runtime`. Repeat each mode at least three times.

For complete applications on Linux, `qtalh/tests/benchmark_ioc.py` starts a
private soft IOC,
uses unique PV names and loopback-only CA addresses, warms up each application
for three seconds, and samples its process CPU time from `/proc`. It alternates
version order between trials, checks the logged transitions, and terminates its
own processes. Point `DISPLAY` at a private Xvfb server to include X11 widget
rendering, or use `QT_QPA_PLATFORM=offscreen`.

```sh
DISPLAY=:97 QT_QPA_PLATFORM=xcb python3 qtalh/tests/benchmark_ioc.py \
  /path/to/before/qtalh bin/Linux-x86_64/qtalh \
  --epics-bin /path/to/base/bin/linux-x86_64 \
  --channels 1000 --seconds 10 --trials 3
```

The IOC script requires Linux `/proc`, Python 3, and `softIoc`, `caRepeater`,
and `caget` in `--epics-bin`. Replace the example Base path above; the script's
default is `/usr/local/oag/base/bin/linux-x86_64` and does not use Make's Base
discovery. `DISPLAY=:97` assumes an X server is already running there. For an
ALH comparison, use X11; the Qt-only offscreen platform cannot render Motif.
`--modes idle updates logged-updates` selects workloads (all three by default).
The script writes trial CSV to stdout and removes temporary logs on exit;
redirect stdout to retain a new measurement. The specialized checkpoint,
content, and syscall investigations have retained data in `docs/benchmarks/`,
but their custom capture harnesses are not checked in. The script above does
not reproduce those specialized experiments by itself.

The IOC and X server CPU are excluded from application CPU. CA workloads with
0.5-second scans nominally produce 2,000 transitions/second; the standalone
engine/UI benchmark additionally counts and verifies delivered events.

## Validation

At the 2026-09-10 engine/UI optimization checkpoint, all 221 checks passed:
92 core, 43 UI, 29 IOC, 53 helper, and 4 visual checks
(including suite setup/cleanup). These suites cover alarm processing, acknowledgements,
force masks, reconnection, logging, editor actions, persistent dialogs, sound,
and appearance. Added regressions exercise earlier/later timer deadlines,
nested NoAck expiry, retained mutable state, timestamp boundaries and clock
reversals, idle filtered-model stability, history updates, runtime blinking,
and silence expiry. The heartbeat interval and deferred-write retry paths retain
their existing dedicated coverage.

A focused Valgrind run of the new UI regression reported no memory-access errors
and no definite or indirect leaks. Its 448-byte possible-loss report originates
in EPICS thread-priority initialization/TLS allocation before `main`.
