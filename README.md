# EPICS Alarm Handler and QtALH

The repository contains the original Motif programs in `alh/` and a C++17 Qt
port in `qtalh/`. Both use GNU Make and shared EPICS discovery in
`Makefile.rules`; neither requires an EPICS extensions tree or MEDM checkout.
The Git directory is now at this repository root. History and the origin remote
are preserved.

## Build and run

```sh
make -j4                         # build available Motif and Qt variants
make alh                        # explicitly build the three legacy programs
make qtalh QT_VERSION=5          # explicitly build the three Qt programs
make -C qtalh -j4                # standalone Qt build
make -C qtalh QT_VERSION=6 -j4   # when Qt 6 development packages are installed

bin/Linux-x86_64/qtalh -mainwindow example.alhConfig
bin/Linux-x86_64/qtalh -c example.alhConfig
bin/Linux-x86_64/qtalh --validate alh/test.alhConfig
bin/Linux-x86_64/qtalh --help
```

Outputs are `bin/<uname -s>-<uname -m>/{alh,alh_printer,alh_DB,qtalh,qtalh_printer,qtalh_DB}`
(for example, `bin/Darwin-arm64/qtalh` on Apple Silicon).
Qt objects and generated resources are separated into
`qtalh/O.<OS>-<architecture>-qt5/` and `qtalh/O.<OS>-<architecture>-qt6/`.
Building a different Qt major copies that variant into the common binary directory.
Legacy objects live in `alh/O.<OS>-<architecture>/`.

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

Qt 6 is preferred when the required pkg-config modules are available; otherwise
Qt 5 is selected. `QT_VERSION=5` or `6` forces the choice. `MOC` and `RCC` can be
overridden for unusual installations and must match the selected Qt libraries.

EPICS Base discovery prefers `/usr/local/oag/base`, followed by nearby
`epics-base` checkouts, the old extensions-relative location, and
`$HOME/epics/base` or `$HOME/epics/base-7.0`. Explicit overrides take precedence:

```sh
make -j4 EPICS_BASE=/path/to/base EPICS_HOST_ARCH=linux-x86_64
```

`EPICS_HOST_ARCH` defaults to Base's `startup/EpicsHostArch.pl`. Static EPICS
libraries are preferred, following MEDM; shared-only installations use an
embedded library search path. `CC`, `CXX`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`,
`LDFLAGS`, `LDLIBS`, `PKG_CONFIG`, `EPICS_COMPILER`, `RPC_CFLAGS`, and `RPC_LIBS`
can be overridden. Legacy
`MOTIF_INC`, `MOTIF_LIB`, `X11_INC`, and `X11_LIB` overrides remain available.
Run `make clean` when changing dependency locations or compiler flags.
`make distclean` also removes the six installed binaries. Cleanup needs no
installed EPICS or Qt development packages.

The original extensions makefile is preserved as `alh/Makefile.epics`.
CDEV, CMLOG, and platforms other than Linux/macOS are outside the Qt port's scope.

## Validation

```sh
make -j4 test-qtalh              # core, offscreen UI, IOC, and helper tests
make -C qtalh test-core
make -C qtalh test-ui            # offscreen UI; native macOS child for audio
make -C qtalh test-ioc           # starts its own softIoc on loopback
make -C qtalh test-helpers       # requires legacy binaries and local rpcbind
make -C qtalh test-visual        # requires X display, xwininfo, and legacy ALH
```

See [CPU benchmarks](docs/qtalh-performance.md) for measured improvements and
reproducible engine/UI and local IOC workloads.

IOC tests use repository-owned databases under `qtalh/tests/`, distinct CA
ports and PV prefixes, and an explicit loopback-only address list. Helper tests
use isolated System V queues, a local TCP capture server, and a temporary RPC
program registered with the local rpcbind. They do not contact production IOCs,
printers, or database services. They remove their own queues and RPC registration.
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

The original license is in [LICENSE](LICENSE). The Qt files are modified works
based on the original ALH parsing, alarm algorithms, workflows and protocols;
source comments and the inventory identify their provenance.
