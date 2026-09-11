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

This builds legacy programs if Motif is detected and runs core, UI, IOC, and
helper suites. It does **not** run visual comparisons. The equivalent
`make -C qtalh test` builds Qt prerequisites but does not build legacy helpers.
Individual targets allow testing the Qt core/UI/CA without Motif:

| Target (use `make -C qtalh TARGET`) | Coverage and extra prerequisites |
| --- | --- |
| `test-core` | Parser, options, alarm state, masks, fake clocks/PV callbacks, and queue encoding. No live IOC or display required. |
| `test-ui` | Models, editor, dialogs, lifecycle, and muted audio playback. Uses `QT_QPA_PLATFORM=offscreen`; needs a functioning Qt media backend. On macOS the audio case starts a native Cocoa child. |
| `test-ioc` | Real CA events, acknowledgements, outputs, reconnection, and access rights. Requires the built Base's `bin/<EPICS_HOST_ARCH>/softIoc`. |
| `test-helpers` | Log rotation/recovery, broadcasts, locks, and Unix queue/TCP/RPC compatibility. On Linux/macOS, requires `alh_printer`, `alh_DB`, and local rpcbind in addition to the Qt helpers built by this target. |
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

On macOS, use a working X11/XQuartz display for the Motif comparison. Offscreen
mode alone cannot capture another X11 application's window.

On Windows, use the launcher from the repository root:

```powershell
.\build-windows.bat test 4
```

It configures the MSVC, Qt, EPICS, DLL, and plugin paths and runs core, UI, IOC,
and portable helper tests. Native lock lifetime, logging, and broadcasts are
covered; Unix queues/RPC, POSIX locking, symlinks, and resource-limit cases are
excluded at compilation. `test-visual` reports unsupported on Windows.

## Isolation, artifacts, and focused runs

IOC tests create their own loopback-only IOC with process-specific PV prefixes
and ports, using [ioc.db](ioc.db) and [ioc.acf](ioc.acf). They do not write to
production records. Unix helper tests use owned System V queues, local TCP
capture servers, and a temporary RPC registration, which they clean up.
An absent prerequisite fails the relevant test rather than silently skipping it.

Qt objects and tests live in `qtalh/O.<OS>-<ARCH>-qt<major>/`. Screenshots and
IOC artifacts go in its `test-artifacts/` directory. Unix suites print Qt Test
results to stdout; Windows additionally writes and prints `core.txt`, `ui.txt`,
`ioc.txt`, and `helpers.txt` there. Tests use temporary directories for most log
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

Performance tools are described in [CPU benchmarks](../../docs/qtalh-performance.md#reproduction).
`benchmark-cpu` exercises the in-process engine/UI; `benchmark_ioc.py` measures
complete applications on Linux using `/proc` and a private IOC. They are separate
from the default regression target.

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
speaker output. On macOS the offscreen suite runs the audio case in a Cocoa
child process, which supplies the event loop needed by native media backends.

The FFmpeg command-line tool and its libvorbis encoder are needed only to
regenerate the fixture. From `qtalh/tests/`:

```sh
ffmpeg -f lavfi -i sine=frequency=880:duration=0.2:sample_rate=44100 \
  -c:a libvorbis -q:a 2 alarm.ogg
```
