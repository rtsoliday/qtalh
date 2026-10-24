# QtALH tests

See the [build instructions](../../README.md#build-and-run) for EPICS and Qt
setup. Use the same `QT_VERSION`, `EPICS_BASE`, and `EPICS_HOST_ARCH` overrides
for builds and tests. Qt Test development files are required in addition to the
application modules. The suite counts in historical reports include setup,
cleanup, and data rows; they vary with platform and source revision.

## Running suites

From the repository root on Linux/macOS:

```sh
make -j4 test-qtalh QT_VERSION=5
```

This builds legacy programs if Motif is detected and runs the core, modern and
classic UI, IOC, pvAccess, helper, notification, analytics, and SDDS suites. It does **not** run visual comparisons. The equivalent
`make -C qtalh test` builds Qt prerequisites but does not build legacy helpers.
Individual targets allow testing the Qt core/UI/CA/PVA without Motif:

| Target (use `make -C qtalh TARGET`) | Coverage and extra prerequisites |
| --- | --- |
| `test-core` | Parser, options, alarm state, masks, fake clocks/PV callbacks, and queue encoding. No live IOC or display required. |
| `test-ui` | Models, editor, dialogs, lifecycle, and muted audio playback in the default modern appearance, including style startup errors, font-size shortcuts across windows/dialogs, large-font geometry, and dark-palette indicators. Help coverage includes bundled topics and links, navigation and scrolling, highlighted search and keyboard wrapping, context Help/F1, modal ownership, reuse, About's email link and Open help button, and Beeper's idle motion, drill sequence, mouse/keyboard activation, and timer lifecycle. Uses `QT_QPA_PLATFORM=offscreen`; needs a functioning Qt media backend and its Ogg decoders. With Qt 5 on Linux the suite decodes through GStreamer's `fakesink`, so it needs no sound server or output device; set `QT_GSTREAMER_PLAYBIN_AUDIOSINK=autoaudiosink` to include the desktop's output route. The heartbeat case starts a private `caRepeater` and `softIoc` from Base's `bin/<EPICS_HOST_ARCH>` on loopback. On macOS the audio cases start native Cocoa children. |
| `test-ui-motif` | The same UI suite with the classic Motif appearance (`QTALH_TEST_STYLE=motif`), including its bevels, row heights and width slider. Part of `make -C qtalh test`. |
| `test-ui-fusion` | The same UI suite in a fresh Fusion process (`QTALH_TEST_STYLE=fusion`). Builds the GUI executable. |
| `test-gallery` | Separate modern (`dialogs/`) and classic (`dialogs-motif/`) galleries, including every Properties tab and the Force PV scroll area; no Motif, IOC, or X11 dependency. Also available with the Windows Makefile. |
| `test-ioc` | Real CA and pvAccess events, acknowledgements, outputs, reconnection, and access rights. Requires the built Base's `bin/<EPICS_HOST_ARCH>/softIocPVA`, which serves the test records over both protocols on loopback. |
| `test-pva` | Deterministic pvAccess callback tests for failed/ended monitors, stale data and write completions after disconnect, shared alarm subscriptions, acknowledgement changes, callback cancellation, never-connected channels, alarm-only force-input updates, value and alarm-status extraction, write conversions, rejected completions, oversized updates, inbox overflow with reads in flight, `readStrings`, and late callbacks after destruction (including from another thread). Uses a test provider without network sockets or an IOC. |
| `test-helpers` | Log rotation/recovery, broadcasts, locks, and Unix queue/TCP/RPC compatibility. On Linux/macOS, requires `alh_printer`, `alh_DB`, local rpcbind, and `python3` (POSIX lock competitors) in addition to the Qt helpers built by this target. |
| `test-build` | Linux only. Dry-run checks of Qt discovery: a `qmake6` found only on `PATH` is not used; `PKG_CONFIG_PATH` may not mix Qt installations. With a usable SDDS installation, also checks that `make clean all` keeps SDDS export and that static SDDS libraries add no run-time library search path. Compiles nothing apart from the SDDS link probe. Not part of `make -C qtalh test`. |
| `test-sdds` | Optional binary alarm snapshot and analytics table/chart readback, UTF-8, exact large counts and timestamps, unknown/lower-bound durations, empty output, atomic failure handling, menu availability, sorted rows and selected charts captured before a refresh. Uses offscreen Qt; data cases skip when SDDS is unavailable. The target uses the default appearance; run `test_sdds` with `QTALH_TEST_STYLE=motif` for the classic one. |
| `test-visual` | Motif/Qt screenshots, dialog gallery, and an ordered alarm-log comparison. Requires a built `alh`, `softIoc`, an X11 display, and `xwininfo`. Does not build the legacy application. |

Prepare legacy prerequisites with `make alh` before Unix helper/visual suites.
The legacy build requires Motif/X11 development packages; default Qt-only builds
can succeed without them, but the full Unix interoperability suite cannot.
The RPC tests register a temporary program with an **already running** local
rpcbind service; they do not start that service. Check availability with
`rpcinfo -p localhost`.

On Linux with `xvfb-run` installed, run the visual suite on a private display:

```sh
make alh QT_VERSION=5
xvfb-run -a -s '-screen 0 1600x1000x24' \
  env QT_QPA_PLATFORM=xcb make -C qtalh QT_VERSION=5 test-visual
```

On macOS, use a working X11/XQuartz display (or a private Xvfb) for the Motif
comparison, with `xwininfo` and `xwd` on `PATH`. The Qt test may use
`QT_QPA_PLATFORM=offscreen`: it captures the legacy X11 window through `xwd`
into `test-artifacts/motif-reference.xwd`, since Cocoa window IDs cannot address
X11 windows. Native Windows/macOS print panels are omitted from the offscreen
dialog galleries.

On Windows, use the launcher from the repository root:

```powershell
.\build-windows.bat test 4
```

It configures the MSVC, Qt, EPICS, DLL, and plugin paths and runs core,
notifications, analytics, pvAccess, modern/classic UI, IOC, and portable helper tests.
Native lock lifetime, logging, and broadcasts are
covered; Unix queues/RPC, POSIX locking, symlinks, and resource-limit cases are
excluded at compilation. `test-visual` reports unsupported on Windows.
The optional `test-gallery` target captures Qt dialogs in both appearances;
Windows' native print dialog is outside the offscreen widget tree and is omitted.

On 2026-10-15, at commit 93ac87b, native MSVC with Qt 6.11.2 and EPICS Base
7.0.10.1-DEV passed a clean application build and test compilation without
compiler warnings.
`build-windows.bat test 4` passed 1,085 checks with no failures and three
expected skips (Linux `/dev/full` in both UI runs and the modern-only styling
contract in the classic run). The optional Fusion suite passed 226 checks
with one Linux-only skip; both dialog galleries passed three checks each.
The PVA-enabled run includes 13 deterministic pvAccess checks and six live
pvAccess IOC cases, including reconnection, writes, local acknowledgements,
descriptions, force inputs, passive mode, and duplicate observers.

### Windows verification (2026-10-18)

The pre-release audit started from 39e17ce. This checkpoint includes the
Phoebus alias-spacing correction, shared CA wakeup ownership and handoff,
repeater startup retries, and native test-header dependencies. A clean native
application build passed in 54.813 seconds with no compiler warnings or
errors, using MSVC 2022, Qt 6.11.2 and EPICS Base 7.0.10.1-DEV. The complete required
suite ran in a scratch source copy with loopback-only CA/PVA settings and
passed 1,390 checks with no failures and 17 skips in 196.234 seconds:

| Suite | Passed | Skipped |
| --- | ---: | ---: |
| Core | 435 | 14 |
| Modern UI | 258 | 2 |
| Classic UI | 259 | 1 |
| Loopback IOC | 150 | 0 |
| pvAccess | 77 | 0 |
| Portable helpers | 125 | 0 |
| Notifications | 57 | 0 |
| Analytics | 29 | 0 |

The optional Fusion suite passed 259 checks with one skip. Both separate
offscreen dialog-gallery runs passed, producing 46 modern and 32 classic PNG
captures. The optional Fusion and gallery targets completed in 122.360 seconds.
These checks cover test-owned `caRepeater` startup
retries and cleanup, shared build rules, bounded log recovery, and pvAccess
callback queues.

Qt 5 was not installed on this Windows host. That run did not cover Linux or
macOS builds, the Motif/X11 visual comparison, Unix helper interoperability,
or clean-machine distribution installation. Native print panels and physical
audio/printer output were not tested. No continuous integration job builds or
tests the application; the repository workflow builds only the documentation.
The later audit corrections F-901 to F-933 have not been built or tested on
Windows or macOS.

### Linux verification (2026-10-18)

The 1.0.0 re-audit verified the code at 3664e13 (branch
release-audit-2026-10-18, including F-901 to F-932) on RHEL 9.8 x86-64 with
GCC 11.3.0, Qt 5.15.9 and EPICS Base 7.0.8. A clean build had no compiler
warnings, including the test programs. Inside a loopback-only network
namespace, `make -C qtalh test` and `test-ui-fusion` passed:

| Suite | Passed | Skipped |
| --- | ---: | ---: |
| Core | 464 | 0 |
| Modern UI | 293 | 0 |
| Classic UI | 292 | 1 |
| Fusion UI | 293 | 0 |
| Loopback IOC | 165 | 0 |
| pvAccess | 81 | 0 |
| Unix helpers, queues, TCP and RPC | 224 | 0 |
| Notifications | 57 | 0 |
| Analytics | 29 | 0 |

The three RPC helper cases need the host's `rpcbind`, so they ran outside the
namespace. `test-build` passed 5 checks, `test-visual` passed under Xvfb and
`test-gallery` passed in both appearances. AddressSanitizer/UBSan and
ThreadSanitizer runs of the suites reported no errors in QtALH code; the only
sanitizer-run failures were the known fixed time bounds in
`analyticsLargeDashboard` and `batchingAndTenThousandChannels`.

## SDDS verification (2026-10-21)

Linux x86-64 with GCC 11.3.0, Qt 5.15.9 and EPICS Base 7.0.8 passed the new
SDDS suite in the modern and classic appearances (8 checks each), and against
the older `/usr/local/epics/extensions` SDDS libraries (8 checks). Readback
verified binary encoding, snapshot fields, analytics provenance, chart series,
exact counts above 2^53, UTF-8, pre-epoch times and unknown/lower-bound values.
Without SDDS, availability checks passed and five data cases skipped.

An existing build directory successfully switched through usable, broken and
restored SDDS libraries without cleaning. Broken/absent support produced no
SDDS diagnostic or link dependency; cleanup skipped probing. Existing core
(464), analytics (29), modern UI (293), and focused classic analytics UI (5)
checks passed. The UI run used `QT_GSTREAMER_PLAYBIN_AUDIOSINK=fakesink` because
the host's default audio sink could not open its resource. Documentation built
and passed local-link checks. These SDDS changes have not been built with Qt 6,
macOS or Windows.

## macOS verification (2026-10-15)

These results are for commit 3aa5d58, which predates pvAccess support
(8d0f3f2). The table has no pvAccess suite, the IOC count has no live pvAccess
cases, and no recorded macOS build includes the pvAccess client. No later
revision has been verified on macOS.

Apple Silicon macOS 26.6.2 with EPICS Base 7.0.9.1-DEV passed application and
test builds with Qt 5.15.19 and Qt 6.11.2 without compiler warnings. Both Qt
versions passed the following suites after the macOS compatibility fixes:

| Suite | Qt 5 passed | Qt 6 passed |
| --- | ---: | ---: |
| Core | 330 | 330 |
| Modern UI | 239 | 239 |
| Classic UI | 238 | 238 |
| Fusion UI | 239 | 239 |
| Loopback IOC | 100 | 100 |
| Unix helpers, queues, TCP and RPC | 181 | 181 |
| Notifications | 46 | 46 |
| Analytics | 27 | 27 |
| Modern and classic galleries | 6 | 6 |
| Full X11 visual target | 4 | 4 |
| Focused layout checks at 150% and 200% scale | 10 | 10 |

There were no failures. The three complete UI appearances have four expected
skips per Qt version: Linux `/dev/full` coverage in each appearance and the
modern-only styling contract in the classic run. Audio playback and missing-file
fallback ran in native Cocoa children; playback was muted. The visual target
used a private Xvfb, captured the Motif window through `xwd`, and verified the
ordered normal/MINOR/MAJOR/normal alarm-log transitions in both applications.
Native print panels and physical audio/printer output were not verified.

The documentation build also passed for `/` and `/manuals/QtALH/`: 34 HTML
pages, 1,927 local links/assets, 41 option spellings, and 14 directives.

## Isolation, artifacts, and focused runs

IOC tests create their own loopback-only IOC with process-specific PV prefixes
and ports, using [ioc.db](ioc.db) and [ioc.acf](ioc.acf). They do not write to
production records. Each also starts its own `caRepeater` on its private
repeater port and stops it at exit; otherwise Channel Access would start a
detached repeater that outlives the test and holds piped output open.
The UI suite also owns a private repeater for runtime subprocesses and editor
activation outside the individual IOC cases; it stops that repeater at suite exit.
The harnesses also override inherited settings that take precedence over
their loopback address lists (`isolatedEpicsSettings()` in
[ca_repeater.h](ca_repeater.h)): they clear `EPICS_CA_NAME_SERVERS` and pin
the `EPICS_CAS_*` and `EPICS_PVAS_*` server ports and beacon lists to the
private loopback ones, so a shell set up for a site gateway is not contacted.
Unix helper tests use owned System V queues, local TCP
capture servers, and a temporary RPC registration, which they clean up.
An absent prerequisite fails the relevant test rather than silently skipping it.
A few UI cases instead report a skip when the host cannot exercise them: the
renamed-log and hard-link checkpoint rows when the temporary directory's
filesystem keeps no extended attributes, and the comma-decimal locale case when
no such locale is installed.

Qt objects and tests live in `qtalh/O.<OS>-<ARCH>-qt<major>/`. Screenshots and
IOC artifacts go in its `test-artifacts/` directory. Unix suites print Qt Test
results to stdout; Windows additionally writes and prints `core.txt`, `ui.txt`,
`ui-motif.txt`, `ioc.txt`, `pva.txt`, `helpers.txt`, `notifications.txt`, and
`analytics.txt` there (`ui-fusion.txt` for `test-ui-fusion`). Tests use temporary directories for most log
fixtures; those fixtures are removed after the test. Visual captures are review
artifacts, not an automated assertion of pixel equivalence.

Run test executables from **`qtalh/`** so relative fixtures such as `tests/ioc.db`
resolve correctly. Qt Test supports `-functions` to list cases and case names
to select them. For example, after the corresponding targets have built:

```sh
cd qtalh
O.Linux-x86_64-qt5/test_core -functions
QT_QPA_PLATFORM=offscreen O.Linux-x86_64-qt5/test_ui \
  runtimeModels editorUndoSave copyBetweenWindows dialogs lifecycle
QT_QPA_PLATFORM=offscreen O.Linux-x86_64-qt5/test_visual dialogGallery
```

The standalone `dialogGallery` case captures Qt dialogs without the legacy
reference case. Use the full X11 `test-visual` target for the Motif comparison.
Replace the object-directory suffix to match the selected platform and Qt major.
For optional Valgrind runs, prefix a focused command with
`valgrind --error-exitcode=99 --leak-check=full` (after any environment assignment).
The callback-lifetime regressions detect a use of freed memory only under
Valgrind or AddressSanitizer; a normal build passes them even without the fix:

```sh
valgrind --error-exitcode=99 O.Linux-x86_64-qt5/test_ioc numericDiagnosticCanDestroyService \
  creationDiagnosticCanRemoveOwner alarmCallbackCanDestroyService \
  freshAlarmCallbackCanDestroyService freshNumericCallbackCanCancelItself
valgrind --error-exitcode=99 O.Linux-x86_64-qt5/test_pva callbackCanDestroyService
```

Performance tools are described in [CPU benchmarks](../../docs/qtalh-performance.md#reproduction).
`benchmark-cpu` exercises the in-process engine/UI. `benchmark_ioc.py` compares
complete applications with a private loopback IOC, initialized records, and a
root-severity processing check. `--xvfb` starts a private display and measures
X-server CPU separately; `--cpus APP IOC XVFB MONITOR` pins the processes to
distinct physical cores. `--logged-seconds` selects longer logged measurements,
`--labels` names the CSV versions, and `--output` retains local evidence. Both
normal and MAJOR log entries are verified for every channel. See `--help` and
the benchmark page for the reproduction command. These tools are separate from
the default regression target.

## Appearance validation

Run `make -C qtalh QT_VERSION=5 test-core test-ui test-ui-motif test-ui-fusion test-gallery`.
Use `QT_VERSION=6` with a matching installed Qt SDK to repeat the checks.
The modern gallery is in `test-artifacts/dialogs/` and the classic gallery in
`test-artifacts/dialogs-motif/`. Unset `QTALH_TEST_STYLE` selects the modern
appearance; `motif`, `fusion` or another style name selects that appearance.
UI screenshots are prefixed `modern-`, `classic-` or the style name. It is a
test/benchmark harness setting, not an application preference. Actual GUI
launches use `-style`.

For display scaling, run the focused checks in separate processes:

```sh
cd qtalh
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=1.5 \
  O.Linux-x86_64-qt5/test_ui motifLayoutAndHitTargets styledRowGeometryAndPalette dialogs
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 \
  O.Linux-x86_64-qt5/test_ui motifLayoutAndHitTargets styledRowGeometryAndPalette dialogs
```

The geometry/interaction case exercises both appearances despite its historical
name. The dedicated styled contract is skipped in classic runs. Screenshots are
review artifacts, not portable pixel-equivalence assertions. Compare classic
captures against the original revision with the same Qt library, display backend,
fonts, and scale. The CPU benchmark also accepts `QTALH_TEST_STYLE`.

## Modern appearance verification (2026-10-14)

On Linux with Qt 5.15.9, the modern default passed 221 UI checks, the classic
appearance 220 (one styled-only check skipped), and Fusion 221, with 320 core,
46 notification and 27 analytics checks. A temporary build of the previous
revision produced a classic dialog gallery pixel-identical to the new
`dialogs-motif/` captures, apart from temporary paths and timestamps.

## Styling verification (2026-09-11)

On Linux with Qt 5.15.3, the implementation passed 101 core checks, 49 legacy
UI checks (one styled-only check skipped), and 50 Fusion UI checks. Both dialog
galleries passed. Focused geometry/dialog checks passed at 150% and 200% scale
and with the X11 backend. The final scaled checks also cover fixed-width text
and preserving the Properties tab and scroll position across selection changes.
Styled file tests exercise opening, saving, cancellation, and both overwrite
prompt responses without modifying the selected file.

A temporary build of the original revision produced pixel-identical legacy
captures for the main window, runtime/channel/editor Properties, Force PV,
Modify Mask, Force Mask, Beep Severity, Guidance, About, and message entry.
Editor/history differences were confined to temporary filenames and timestamps.
Current CPU results and the reproducible ALH comparison are in [CPU benchmarks](../../docs/qtalh-performance.md).

The documentation build validated 29 HTML pages and their local links/assets.
Qt 6 development modules and native macOS/Windows environments were unavailable
on this host; builds and native smoke checks there remain to be performed.

## Recorded verification

The documentation audit on 2026-09-11 ran on Linux x86-64 with Qt 5.15.9 and
EPICS Base 7.0.10.1-DEV. `make -j4 test-qtalh QT_VERSION=5` and the private-X11
visual command above passed: 92 core, 43 UI, 29 IOC, 63 helper, and 4 visual
checks (231 total, no failures or skips). After the command-line audio help was
corrected, the application and core suite were rebuilt and the core suite passed
again. Headless help/version, the starter and legacy configurations, the guide's
configuration snippet, relative INCLUDE resolution, and invalid-input exit
status were checked. Local documentation links and documented option spellings
were also checked against the files and parser.

This audit did not rerun Windows, macOS, or Qt 6 tests. Their earlier results in
the README and compatibility inventory remain historical platform evidence.

## Audio fixture

[alarm.ogg](alarm.ogg) is a generated 0.2-second, 880 Hz sine wave encoded as
Ogg Vorbis. The UI test decodes and plays it twice with output muted, on both
Qt 5 and Qt 6. It does not require a system sound theme, and does not verify
speaker output. On macOS the offscreen suite runs playback and missing-file
fallback cases in Cocoa child processes, which supply the event loop needed by
native media backends to report both playback completion and errors.

The FFmpeg command-line tool and its libvorbis encoder are needed only to
regenerate the fixture. From `qtalh/tests/`:

```sh
ffmpeg -f lavfi -i sine=frequency=880:duration=0.2:sample_rate=44100 \
  -c:a libvorbis -q:a 2 alarm.ogg
```

## Timed shelving checks

The core suite covers shelf expiry, preserved transients, group operations,
independent display aggregates, mask/filter interactions, automation equivalence,
reload identity/latches, and 10,000-channel bursts. The IOC suite verifies that
shelving one runtime leaves IOC acknowledgements, severity outputs, and a second
client operating normally. The UI suites exercise reasons/durations, fixed
selection targets, list changes, filters, reloads, and Save As.

Run `test-core`, `test-ui`, `test-ui-motif`, and `test-ioc` from `qtalh`.
The shelving UI fixture writes `modern-shelve-dialog.png`,
`modern-shelved-main.png`, and `modern-shelved-list.png`, with corresponding
`classic-` captures, under the build's `test-artifacts` directory. These are
synthetic local fixtures; their clocks are controlled by the tests.

### Notification subscriptions

`make -C qtalh test-notifications` runs fake-clock scheduling tests, reload and
suppression cases, 10,000-channel batching, queue pressure, settings conflict and
corruption handling, MIME/header validation, a local fake mailer, and loopback
HTTP retry/redirect/timeout tests. No real email or external webhook is used.
The target is included in `test` on Unix and Windows. UI tests exercise settings,
match preview, explicit runtime enable, and paused restart in both appearances;
screenshots are `modern-notifications.png` and `classic-notifications.png`.

### Session analytics

`make -C qtalh test-analytics` covers metric definitions with a fake clock,
rolling boundaries, suppression, gaps, reload/reset, CSV safety, independent
notification observers, background-result invalidation, and bounded history
under 10,000 channels / 300,000 transitions. It is included in `test` on Unix and
Windows. UI tests exercise all four tabs, charts, scope/search, CSV export,
reset, and the 10,000-channel dashboard in both appearances. IOC tests check
confirmed/requested/external acknowledgement and access gaps. Screenshots are
`modern-analytics-0.png` through `-3.png` and their `classic-analytics-*` equivalents.
