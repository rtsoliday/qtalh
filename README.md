# EPICS Alarm Handler and QtALH

The repository contains the original Motif programs in `alh/` and a C++17 Qt
port in `qtalh/`. Both use GNU Make and shared EPICS discovery in
`Makefile.rules`; neither requires an EPICS extensions tree or MEDM checkout.

The default QtALH appearance retains the familiar Motif look. Add `-style fusion`
to opt into modern controls and layouts, for example
`qtalh -style fusion -mainwindow facility.alhConfig`. See the
[appearance guide](docs/qtalh-appearance.md) for screenshots and details.

## Documentation

The [modern documentation site](https://qtalh-documentation.rtsoliday123.chatgpt.site)
provides searchable tutorials, operator guides, and technical reference.
The Sites mirror is publicly accessible. Run `make docs` to generate it in `docs/html`, then
serve it with `python3 -m http.server 8000 --directory docs/html --bind 127.0.0.1`.
Open `http://127.0.0.1:8000/`. For the APS server, build with
`make docs DOCS_BASE=/manuals/QtALH/` and copy all contents of `docs/html/`
into the server directory served at `/manuals/QtALH/`. Website builds need Node.js 22+ with npm and Python 3;
they do not require Qt or EPICS. See the
[documentation workflow](docs/site/develop/documentation.md) for details.

- [QtALH user guide](docs/qtalh-user-guide.md): first run, all command-line
  options, configuration syntax, logging, and troubleshooting.
- [Test guide](qtalh/tests/README.md): prerequisites, suite commands, artifacts,
  and the audio fixture.
- [Compatibility inventory](docs/qtalh-compatibility.md): implementation evidence
  and remaining acceptance work.
- [Appearance comparison](docs/qtalh-appearance.md): Motif/Qt workflow and visual differences.
- [CPU benchmarks](docs/qtalh-performance.md) and
  [logging investigation](docs/qtalh-logging-analysis.md): historical measurements
  and the subsequent checkpoint optimization.
- [Original ALH manual](alh/documentation/ALH.html): legacy operator reference;
  consult the QtALH guide and inventory for port-specific behavior.

## Build and run

### Windows

QtALH builds as a native x64 Windows application using MSVC, following the
QtEDM build setup. Install Visual Studio 2022 with the x64 C++ tools, GNU Make
(Strawberry Perl or Cygwin), Cygwin shell utilities, Qt for MSVC including
**Multimedia**, and a built EPICS Base for `windows-x64`.

From PowerShell or Command Prompt:

```powershell
.\build-windows.bat build 4
.\build-windows.bat test 4
.\build-windows.bat rebuild 4
```

The launcher discovers Visual Studio and defaults to Qt
`C:\Qt\6.11.2\msvc2022_64`, Cygwin `C:\cygwin64`, and the sibling
`..\epics-base` checkout. Override `QT_DIR`, `QT_VERSION` (5 or 6), `EPICS_BASE`,
`EPICS_HOST_ARCH`, `QTALH_CYGWIN_ROOT`, or `QTALH_MAKE_EXE` through environment
variables. Qt 5 requires 5.15 or newer. `QT_MULTIMEDIA_DIR` can point to a
separate matching Qt Multimedia SDK (same Qt version, MSVC kit and architecture).
For an already configured MSVC/Cygwin shell, use
`make -j4 OS=Windows ARCH=x86_64 QT_DIR=C:/Qt/6.11.2/msvc2022_64`.
Use dependency paths without spaces for EPICS Base and the repository.

The output is `bin/Windows-x86_64/qtalh.exe`. To run it, add the Qt, Multimedia
and EPICS `bin` directories to `PATH`; add the Qt and Multimedia `plugins`
directories to `QT_PLUGIN_PATH` when using a split SDK. The launcher sets these
for its test processes. For distribution, use Qt's `windeployqt` and include the
EPICS `ca.dll` and `Com.dll` runtime libraries.

Windows supports the main Qt application, Channel Access, alarm audio, file
logging, master/slave locks and file broadcasts. Configured shell commands use
`cmd.exe /d /s /c` and must use Windows syntax and paths. The Motif programs and
the System V/Sun RPC helpers `qtalh_printer` and `qtalh_DB` remain Linux/macOS
only; `-P` and `-O` report an explicit unsupported-option error on Windows.
Windows file locks coordinate native QtALH processes; they do not provide
POSIX lock interoperability with legacy ALH.

Verified with MSVC 2022, Qt 6.11.2 (including its installed Multimedia module)
and EPICS Base 7.0.10.1-DEV on Windows x64. The Windows flags explicitly enable
`/Zc:lambda` so older MSVC 2022 versions can compile Qt 6.11's generated moc
code in C++17 mode.

The Windows test target runs the core, offscreen UI/audio, loopback IOC and
portable helper tests, including logging rotation/recovery, broadcasts and
native lock lifetime. Unix-only queue/RPC, POSIX locking, symlink and resource
limit cases are excluded. Motif/X11 visual comparisons require Linux/macOS.
Test reports are saved under `qtalh/O.Windows-x86_64-qt6/test-artifacts/`
(or the corresponding Qt 5 directory) and printed after each suite.
`build-windows.bat clean` removes Windows objects without requiring Qt, EPICS
or Visual Studio.

### Linux and macOS

```sh
make -j4                         # build available Motif and Qt variants
make alh                        # explicitly build the three legacy programs
make qtalh QT_VERSION=5          # explicitly build the three Qt programs
make -C qtalh -j4                # standalone Qt build
make -C qtalh QT_VERSION=6 -j4   # when Qt 6 development packages are installed

bin/Linux-x86_64/qtalh -c examples/minimal.alhConfig
bin/Linux-x86_64/qtalh -S -D -s -mainwindow examples/minimal.alhConfig
bin/Linux-x86_64/qtalh --validate alh/test.alhConfig
bin/Linux-x86_64/qtalh --help
```

The example uses placeholder PV names; replace them with your IOC records to
monitor live values. The second command opens a passive, silent preview with
file logging disabled. Use `--validate` to check syntax without CA connections.

Standalone outputs are `bin/<uname -s>-<uname -m>/{alh,alh_printer,alh_DB,qtalh,qtalh_printer,qtalh_DB}`
(for example, `bin/Darwin-arm64/qtalh` on Apple Silicon).
Qt objects and generated resources are separated into
`qtalh/O.<OS>-<architecture>-qt5/` and `qtalh/O.<OS>-<architecture>-qt6/`.
Building a different Qt major copies that variant into the common binary directory.
Legacy objects live in `alh/O.<OS>-<architecture>/`.

When the repository is in an EPICS extensions tree at `extensions/src/qtalh`,
with `extensions/configure/CONFIG` and `RELEASE` present, the same build commands
install into `extensions/bin/<EPICS_HOST_ARCH>/` (for example,
`extensions/bin/linux-x86_64/qtalh`). Base is selected by the extensions
`configure/RELEASE`, including its local includes and host-specific overrides.
The extensions `CONFIG_SITE` files can redirect installation with
`INSTALL_LOCATION` or `INSTALL_LOCATION_EXTENSIONS`. Explicit `EPICS_BASE`,
`EPICS_HOST_ARCH`, and command-line `BIN_DIR` settings still take precedence.
`make install` is also supported, including when invoked by the extensions parent.
Outside a configured extensions tree, discovery and output paths are unchanged.

Requirements common to the builds: Linux or macOS, GNU Make, C/C++ compilers, Perl,
pkg-config, built EPICS Base with Channel Access/Com headers and libraries,
and RPC development files (TI-RPC on Linux, the system SDK on macOS).
QtALH additionally needs Qt **5.15 or newer**, or
Qt **6**, including Widgets, Network, PrintSupport, Multimedia, `moc`, and `rcc`.
Qt Test is needed for tests. Motif ALH needs Motif, X11, Xt and Xmu development
files. Explicit Qt builds do not include or link Motif/Xt.

On RHEL-family systems the Qt 5 packages include `qt5-qtbase-devel`,
`qt5-qtmultimedia-devel` and `libtirpc-devel`. On Debian-family systems they
include `qtbase5-dev`, `qtmultimedia5-dev`, and `libtirpc-dev`. Qt 6 equivalents
are `qt6-base-dev`, `qt6-base-dev-tools` and `qt6-multimedia-dev` on Debian-family
systems. Package names depend on the distribution.

On macOS, install the Xcode Command Line Tools and make the Qt pkg-config
modules available on `PKG_CONFIG_PATH`. MacPorts Qt installations are supported;
include the Multimedia module for the Qt major being used. For example, with
Qt 6 installed under `/opt/local/libexec/qt6`:

```sh
export PKG_CONFIG_PATH=/opt/local/libexec/qt6/lib/pkgconfig:$PKG_CONFIG_PATH
make -j4
bin/Darwin-arm64/qtalh --validate alh/test.alhConfig
```

Motif and X11 headers/libraries are discovered in standard locations, including
MacPorts, Homebrew, and XQuartz prefixes. Static Motif dependencies are linked
explicitly. Running legacy ALH requires an X server; QtALH uses native Qt windows.
The macOS build selects EPICS's `compiler/clang` and `os/Darwin` headers and
supports both static EPICS archives and shared `.dylib` libraries.

Qt 6 is preferred when its required modules are available; otherwise
Qt 5 is selected. Discovery normally uses pkg-config. On Linux, Qt 6 installations
without `.pc` files (including Ubuntu 22.04's Qt 6.2 packages) are also discovered
through `qmake6`, using its header, shared-library, and build-tool paths. Set
`QMAKE6=/path/to/qmake6` for a custom installation. The fallback also supplies
the build flags for QtALH, its helpers, and its tests; reinstalling the same Qt
packages or changing `PKG_CONFIG_PATH` is unnecessary when `.pc` files are absent.
`QT_VERSION=5` or `6` forces the choice. `MOC` and `RCC` can be
overridden for unusual installations and must match the selected Qt libraries.

Standalone EPICS Base discovery prefers `/usr/local/oag/base`, followed by nearby
`epics-base` checkouts, the old extensions-relative location, and
`$HOME/epics/base` or `$HOME/epics/base-7.0`. A sibling checkout can be shared
with MEDM/QtEDM. From the qtalh repository root, clone and build Base once:

```sh
git clone --recursive -b 7.0 https://github.com/epics-base/epics-base.git ../epics-base
make -C ../epics-base -j4
make -j4
```

Both the root build and `make -C qtalh` automatically use that checkout.
A missing Base produces the clone instructions; an unbuilt checkout produces
instructions to build it. Base is not cloned or built automatically.
The default build prints dependency notices and installation suggestions for
missing Motif or Qt development files before starting the available variants.
Motif requires both `Xm/Xm.h` and `libXm`; if neither variant is available,
the build fails after the notices. Explicit overrides take precedence:

```sh
make -j4 EPICS_BASE=/path/to/base EPICS_HOST_ARCH=linux-x86_64
```

`EPICS_HOST_ARCH` defaults to Base's installed `lib/perl/EpicsHostArch.pl`,
with fallbacks to `src/tools/EpicsHostArch.pl` in a source checkout and
`startup/EpicsHostArch.pl` for older layouts. Detection runs once per make
process; an explicit `EPICS_HOST_ARCH` bypasses it. Static EPICS
libraries are preferred, following MEDM; shared-only installations use an
embedded library search path. `CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`,
`LDFLAGS`, `LDLIBS`, `PKG_CONFIG`, `EPICS_COMPILER`, `RPC_CFLAGS`, and `RPC_LIBS`
can be overridden. Legacy
`MOTIF_INC`, `MOTIF_LIB`, `X11_INC`, and `X11_LIB` overrides remain available.
Run `make clean` when changing dependency locations or compiler flags.
`make distclean` also removes the six installed binaries. Cleanup needs no
installed EPICS or Qt development packages. At the repository root, cleanup also
uses Python 3 to remove generated documentation; `distclean` additionally removes
`docs/site/node_modules`. Authored documentation is preserved. Use `make docs-clean`
or `make docs-distclean` to clean only the documentation.
Windows cleanup uses `python` (other platforms use `python3`); override with
`PYTHON=/path/to/python` if needed.

The original extensions makefile is preserved as `alh/Makefile.epics`.
CDEV and CMLOG are outside the Qt port's scope.

## Validation

```sh
make -j4 test-qtalh              # core, offscreen UI, IOC, and helper tests
make -C qtalh test-core
make -C qtalh test-ui            # offscreen UI; native macOS child for audio
make -C qtalh test-ioc           # starts its own softIoc on loopback
make -C qtalh test-helpers       # requires legacy binaries and local rpcbind
make -C qtalh test-visual        # requires built legacy ALH, X11, xwininfo, softIoc
```

See [CPU benchmarks](docs/qtalh-performance.md) for measured improvements and
reproducible engine/UI and local IOC workloads.

IOC tests use repository-owned databases under `qtalh/tests/`, distinct CA
ports and PV prefixes, and an explicit loopback-only address list. Helper tests
use isolated System V queues, a local TCP capture server, and a temporary RPC
program registered with the local rpcbind. They do not contact production IOCs,
printers, or database services. They remove their own queues and RPC registration.
On Linux/macOS the full helper suite needs the legacy helper executables even
for a Qt-only installation; `make -C qtalh test` does not build them. See the
[test guide](qtalh/tests/README.md) for preparation and individual suites.
An unavailable prerequisite is a test failure, not a silently passing test.
The UI suite uses a bundled Ogg/Vorbis sound and tests muted playback twice.
On macOS, that case runs in a Cocoa child process because the offscreen plugin
does not drive the native media event loop reliably.

Screenshots and IOC logs are written to the selected Qt object directory's
`test-artifacts/`. The visual test captures matching Motif and Qt windows;
these are review artifacts, not an automated pixel-equivalence assertion. The
visual test also compares a short ordered alarm-log sequence against Motif.

Optional memory checks (from `qtalh/`, after building the tests):

```sh
valgrind --error-exitcode=99 --leak-check=full O.Linux-x86_64-qt5/test_core
QT_QPA_PLATFORM=offscreen valgrind --error-exitcode=99 --leak-check=full \
  O.Linux-x86_64-qt5/test_ui runtimeModels editorUndoSave copyBetweenWindows dialogs lifecycle
```

Verified build environments include Qt 5.15.3 with EPICS Base 7.0.8 on Linux
x86_64, and Qt 5.15.19 and Qt 6.11.2 with Apple Clang on macOS arm64.
The Qt 6 core, UI, IOC, and helper suites pass on macOS, including Ogg alarm
playback. Qt 5 core and UI regression tests also pass with the same sources.
See [the compatibility inventory](docs/qtalh-compatibility.md) for implemented
features, evidence, protocol details, and outstanding parity validation.

## Architecture and compatibility

`qtalh/core/` owns configuration and alarm state. It has no widget or Channel
Access dependency; `PvService` and clock/log/command callbacks allow deterministic
tests. `qtalh/services/` implements Channel Access, logging, locking and helper
protocols. `qtalh/ui/` contains item models, delegates, windows and dialogs.
Channel Access uses a shared non-preemptive context, socket notifiers and a
100 ms fallback poll, with serialized callbacks and explicit subscription cleanup.

Existing `.alhConfig` files are supported. Saves use the legacy textual syntax;
includes are expanded, and comments/formatting are not retained. Open/insert
failures leave the current document intact. `ALARMHANDLER` and `ALHMAINFONT`
are supported. `-S` is passive mode; `-D` disables file logging and does **not**
prevent CA writes. Unsupported Xt resource overrides produce a diagnostic.

The editor's **File → Activate ALH** opens a runtime copy of the current edits.
Properties and action dialogs follow the selected group/channel. Properties
Apply keeps the dialog open; Cancel restores the applied values. Historical
alarm and operator-log browsers search current and dated files by time and text,
with cancellable background searches and explicit result-limit messages.
Use `-debug` for timestamped diagnostics on stderr.

The helper command lines retain the original positional arguments:

```text
qtalh_printer TCPName TCPport Key ColorModel
qtalh_DB      TCPName TCPport Key
```

For `qtalh_DB`, the historically named `TCPport` argument is the **RPC program
number**, not a TCP port. Version and procedure are both 1. Printer color modes
are `bw`, `bw_bold`, `oki_bold`, and `hp_color`. Queue keys match `qtalh -P` and
`qtalh -O`. Mixed legacy/Qt helpers use the same native System V queue format.
On macOS the database helpers use TCP with the system Sun RPC implementation;
Linux retains TI-RPC's `netpath` transport selection.
Qt senders reject messages exceeding the legacy receive-buffer capacity
(242 text bytes on 64-bit Linux), reporting the failure rather than overflowing.

## Authors and acknowledgements

**Robert Soliday** develops and maintains the QtALH port. QtALH builds on the
work of the original ALH authors and the SNS and PSI contributors. See
[AUTHORS.md](AUTHORS.md) for the complete credits and contribution roles.

The original license is in [LICENSE](LICENSE). The Qt files are modified works
based on the original ALH parsing, alarm algorithms, workflows and protocols;
source comments and the inventory identify their provenance.
