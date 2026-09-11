# QtALH user guide

QtALH monitors EPICS Channel Access alarm records and edits legacy `.alhConfig`
files. See the [README](../README.md#build-and-run) for installation and the
[compatibility inventory](qtalh-compatibility.md) for implementation evidence
and differences from Motif ALH. This guide describes the Qt executable;
[the original manual](../alh/documentation/ALH.html) describes legacy ALH.

## First run

Run commands from the repository root after building. The examples use Linux
x86-64; substitute `bin/Darwin-arm64/qtalh` on Apple Silicon or
`bin/Windows-x86_64/qtalh.exe` on Windows.

```sh
bin/Linux-x86_64/qtalh --help
bin/Linux-x86_64/qtalh --version
bin/Linux-x86_64/qtalh --validate examples/minimal.alhConfig
bin/Linux-x86_64/qtalh -c examples/minimal.alhConfig
```

The [starter configuration](../examples/minimal.alhConfig) contains two
placeholder PV names. Use the editor's Properties dialog to replace them with
records from your IOC, then Save As to a new configuration. `qtalh -c` starts
an empty editor. Editing does not start alarm monitoring. File → Activate ALH
opens a separate runtime copy of the current edits; an unnamed document must
first be saved. Later runtime reloads read that saved file.

For a runtime preview:

```sh
bin/Linux-x86_64/qtalh -S -D -s -mainwindow examples/minimal.alhConfig
```

This connects to the configured PV names using your EPICS CA environment.
Unresolved names display ERROR. `--validate` checks parsing only, with no GUI
or CA connections; it does not check PV existence, access rights, audio, or log
permissions. Help, version, and successful validation exit with status 0;
option/configuration errors print to stderr and exit with status 1.

Without `-mainwindow`, runtime starts with a compact facility button. Click it
to open the main tree and group contents. Closing the main runtime window hides
it while monitoring continues. Closing the compact window or using File
Exit/Close asks for confirmation before stopping that runtime.

## Command-line reference

Syntax: `qtalh [OPTIONS] [configfile]`. One configuration is supported per
invocation; the UI can open additional windows. A missing runtime filename
means `ALH-default.alhConfig`. If it does not exist, interactive startup opens
a file chooser. The headless validator instead reports the missing file.

| Option | Meaning and default |
| --- | --- |
| `-c` | Configuration editor; otherwise runtime monitoring. |
| `-global` | Use IOC ACKS/ACKT fields for global acknowledgement. Default: local acknowledgement. |
| `-S` | Passive mode: suppress CA writes and operator acknowledgement. |
| `-caputackt` | Apply configured transient acknowledgement settings at startup in global active mode. |
| `-D` | Disable alarm and operation **file** logging. Does not disable CA writes or configured helper queues. |
| `-s` | Start with Silence Forever enabled; it can be changed in Setup. |
| `-p file` | Play a local audio file instead of the system beep. Formats depend on the installed Qt Multimedia backend; the suite exercises Ogg/Vorbis. |
| `-f dir` | Configuration and INCLUDE directory. Default: `ALARMHANDLER`, or the working directory (`.`). |
| `-l dir` | Log directory. Default: the configuration directory. Create it before starting. |
| `-a file` | Alarm filename, relative to the log directory unless absolute. Default: `ALH-default.alhAlarm`. |
| `-o file` | Operation filename, relative to the log directory unless absolute. Default: `ALH-default.alhOpmod`. |
| `-m count` | Alarm retention in records; default 2000. `0` means unlimited. Does not bound the operation log. |
| `-T` | Append `.yyyy-MM-dd` to each log filename, using local dates. The alarm record limit still applies. |
| `-xml` | Write legacy XML-like entry records; these are not a complete XML document. |
| `-L` | Coordinate master/slave alarm logging with a lock. |
| `-Lfile basename` | Override the lock basename; requires `-L` to enable locking. Default: resolved configuration filename. |
| `-B` | Enable file-based message, reload, and temporary stop-logging broadcasts. |
| `-P key` | Send alarm records to a printer System V queue; positive decimal integer, Linux/macOS only. |
| `-O key` | Send database records to a System V queue; positive decimal integer, Linux/macOS only. |
| `-filter no\|active\|unack` | Initial display filter; default `no`. Active includes active or unacknowledged alarms. Filtering does not stop monitoring. |
| `-mainwindow` | Open the main runtime window at startup. |
| `-maskcolor` | Color the mask controls. |
| `-noerrorpopup` | Suppress error message boxes; errors still appear in the window message area and Qt diagnostic output. |
| `-desc_field` | Subscribe to each channel record's `.DESC` field for descriptions. |
| `-debug` | Timestamped startup, alarm, CA, command, logging, and broadcast diagnostics on stderr. Default: off. |
| `-display display`, `--display display` | Set `DISPLAY` before opening the GUI (for X11). |
| `-geometry geometry` | Initial geometry such as `1000x600+20+20`. |
| `-fn font`, `-font font` | Runtime button font; defaults to `ALHMAINFONT` when set. XLFD fonts are approximated. |
| `-platform platform` | Qt platform selection, for example `offscreen` or `xcb`. |
| `-style style` | Qt widget style override. |
| `-help`, `--help`, `-h` | Print help without opening a display. |
| `-version`, `--version`, `-v` | Print ALH, Qt, and EPICS version information without opening a display. |
| `--validate` | Parse the configuration and includes, report group/channel counts, then exit. |
| `--` | End options before the configuration filename. |

Local mode keeps acknowledgements in this runtime. Global active mode can write
ACKS/ACKT and configured acknowledgement, severity, and heartbeat PVs. Passive
mode still monitors alarms and can log them. It does not disable configured
shell commands; `-D` and `-s` also leave command execution enabled.

EPICS Base reads the standard `EPICS_CA_*` environment variables.
`QT_QPA_PLATFORM` selects Qt's display backend; `QT_PLUGIN_PATH` can locate
plugins from a separate SDK. On Windows, `COMSPEC` selects the command shell
(default `cmd.exe`). Sound paths and an explicit relative `-Lfile` basename
resolve from the process working directory, rather than `-f` or `-l`.
Unknown switches and arbitrary Xt resource overrides are errors. CDEV and
CMLOG extensions are not supported by QtALH.

## Configuration format

Declarations and directive names are case-sensitive. Blank lines and lines
starting with `#` are ignored. Names and declaration filenames are whitespace
separated tokens; quoting does not allow spaces in those fields. Directive
text such as aliases, commands, and guidance can contain spaces.

```text
GROUP NULL Facility
GROUP Facility Vacuum
CHANNEL Vacuum example:pressure -----
$ALIAS Vacuum pressure
$BEEPSEVR MAJOR
$GUIDANCE
Check the vacuum controller before acknowledging this alarm.
$END
```

Exactly one root group is required. A parent must be the current group or one
of its ancestors (for a channel, lookup starts at its parent), so arrange the
file in tree order. A group cannot be named `NULL` or repeat an ancestor's name;
repeated group names in separate branches are allowed. Group nesting is limited
to 50 levels.

`CHANNEL parent pv [mask]` defaults to `-----`. Masks are sets of uppercase
letters and save in canonical `CDATL` positions; short/reordered forms such as
`D` and `DA` are accepted.

| Letter | Setting |
| --- | --- |
| `C` | Cancel: stop the channel's alarm subscription. |
| `D` | Disable: exclude the alarm from active group severity, history, commands, and audible indication; monitoring continues unless also cancelled. |
| `A` | NoAck: exclude the channel from acknowledgement requirements. |
| `T` | NoAckT: do not retain cleared transient alarms for acknowledgement. In global mode this corresponds to IOC ACKT being zero. |
| `L` | NoLog: suppress this channel's alarm records. |

Disable alone does not suppress alarm-file records; use NoLog for that. Timed
NoAck is an operator setting and displays `H` in the mask summary.

`INCLUDE parent filename` attaches another file beneath the named parent.
Every relative include, including nested includes, resolves against `-f` or
`ALARMHANDLER` (default `.`), not the containing include's directory. For example,
use `qtalh -f /path/to/configs main.alhConfig`. Absolute paths are accepted.
Include cycles and excessive nesting are errors. The C++ `loadConfig` API uses
the top-level file's directory when no configuration directory is supplied.

Directives normally apply to the preceding group/channel:

| Directive | Arguments and behavior |
| --- | --- |
| `$ALIAS` | Display label text; the underlying PV/group name is unchanged. |
| `$GUIDANCE` | URL or filename, opened externally. Relative guidance files resolve from the document directory. With no inline value, read text through `$END` into a guidance dialog. |
| `$COMMAND` | Related command, or `label ! command ! label ! command` menu pairs. Commands starting with `medm` (including a quoted or absolute executable path) silently launch `qtedm` from `PATH` instead, preserving arguments. Configuration text is unchanged. This substitution also applies to severity/status commands. |
| `$BEEPSEVERITY` | Facility beep threshold (default MINOR); may occur before the root. |
| `$BEEPSEVR` | Threshold for the current node; ancestor thresholds also apply. |
| `$HEARTBEATPV` | `pv [interval_seconds [short_integer_value]]`; defaults 1 second and value 1. The first heartbeat in the facility, including includes, wins. Writes require global active mode. |
| `$ACKPV` | `pv short_integer_value`; channel only. Written on global active acknowledgement. |
| `$SEVRPV` | Output PV for the node's severity (0–4); disabled channels publish -1. Writes require global active mode. `-` disables this output. |
| `$FORCEPV` | `pv mask [force_value [reset_value]]`; default force 1, reset 0. At force, apply the mask; at reset, restore configured channel masks. `NE` or a reset equal to force resets on leaving the force value. Applies to descendant channels for a group. |
| `$FORCEPV_CALC` | EPICS CALC expression required when the FORCEPV name is `CALC`. |
| `$FORCEPV_CALC_A` through `$FORCEPV_CALC_F` | Numeric constants or PV inputs for CALC. Unspecified inputs default to zero; all named PV inputs must become available before evaluation. |
| `$SEVRCOMMAND` | `UP_severity command` or `DOWN_severity command`, matching direction and destination severity; `ANY` matches any change in that direction, `UP_ALARM` matches leaving NO_ALARM. Repeat to add commands. |
| `$STATCOMMAND` | `status command`; channel only, runs when entering that status. Repeat to add commands. Status names follow EPICS alarm names and the additional connection/access states in `qtalh/core/model.cc`. |
| `$ALARMCOUNTFILTER` | `count seconds`; channel only. Legacy alarm count/time filtering: count -1 delays alarm onset, 0 also holds clearing transitions, positive counts track repeated alarm edges within the interval. Zero seconds disables filtering. Accepted count range: -1 through 1,000,000; seconds must be a nonnegative integer. |

Severities are `NO_ALARM`, `MINOR`, `MAJOR`, `INVALID`, and `ERROR` (0–4).
Severity values also accept numbers and case-insensitive names; command direction
prefixes `UP_` and `DOWN_` are uppercase. Heartbeat intervals must be finite,
positive, and no greater than 2147483.647 seconds; scheduling rounds to milliseconds
with a 1 ms minimum. Event-loop delays can delay a beat; missed beats are skipped.

Commands use `/bin/sh -c` on Linux/macOS and `cmd.exe /d /s /c` on Windows.
Use commands appropriate to the host. A leading `MASTER_ONLY` restricts a
command to the logging master. Stop-logging broadcasts also temporarily suppress
commands. The editor does not execute runtime alarm commands.

Open/insert failures leave the current document intact. Save uses an atomic
file replacement, expands includes, normalizes masks, orders channels before
child groups, and discards comments and original formatting. Runtime Save As
exports masks without changing the original configuration's reload, lock, or
broadcast identity; editor Save As changes the document filename.

## Logging and shared operation

Create a writable log directory and select it explicitly when desired:

```sh
mkdir -p logs
bin/Linux-x86_64/qtalh -l logs -a alarm.log -o operation.log -m 2000 -L my.alhConfig
```

Replace `my.alhConfig` with your configuration. Alarm logs retain the most recent
2000 records by default using a circular file; physical line order after wrapping
is not chronological. Operation logs append without a record limit. Dated logs
switch on the local date and keep earlier files; automatic deletion of old daily
files is not implemented. Historical browsers search the current file and dated
siblings, sort by time, and support local From/To times and case-sensitive With
text. They report cancellation, unreadable files, and the 100,000-record/10 MiB
result limits. Live viewers and the ten-entry in-memory alarm history are separate.

Bounded alarm logs keep a sibling `.qtalh-position` file containing two binary
checkpoints. Keep it with the alarm log to preserve exact ring order across
restarts, including equal timestamps. If it is missing or does not match, recovery
uses timestamps, which cannot reliably order ties or out-of-order records.
Records are flushed before checkpoint updates; the two files are separate writes
and are not an atomic transaction or a per-record durable fsync. See the
[checkpoint details](qtalh-logging-analysis.md#implemented-changes).

`-L` uses `<configuration>.LOCK` by default and checks ownership at startup and
about every 20 seconds. It selects the alarm writer; operation records remain
per-process. Multiple independent processes sharing a circular alarm file should
use the same lock basename and logging settings. `-Lfile /path/to/shared` selects
`/path/to/shared.LOCK`. On Linux/macOS, locks use the legacy POSIX format; Windows
uses native file locks without POSIX interoperability.

`-B` uses `<configuration>.MESS` and `.MESSLOCK`, independently of `-Lfile`.
Participants must use the same configuration identity and have access to those
files. Messages are polled every two seconds, and senders hold delivery ownership
for 60 seconds; another send reports busy during that interval. Broadcast actions
send a message, reload the named configuration, or suppress alarm logging and
commands for 1–10 minutes. Monitoring continues. Reload preserves Silence Forever.

Linux/macOS helpers retain these positional arguments:

```text
qtalh_printer host tcp_port queue_key bw|bw_bold|oki_bold|hp_color
qtalh_DB      host rpc_program_number queue_key
```

Keys must match `-P`/`-O`. The database argument is an RPC program number,
version 1/procedure 1, rather than a TCP port. Native System V queue layout is
ABI-dependent. Mixed legacy/Qt records are limited to `250 - sizeof(long)` text
bytes (242 on 64-bit Linux); oversized queue records report errors without
preventing file logging. Windows does not build these helpers and rejects
`-P`/`-O`.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| EPICS headers/libraries not found | Build Base first; set `EPICS_BASE` and `EPICS_HOST_ARCH` to that installation. See [build discovery](../README.md#linux-and-macos). |
| Qt modules or build tools missing | Install matching Widgets, Network, PrintSupport, and Multimedia development files; tests also need Qt Test. Use `QT_VERSION=5` or `6` consistently and matching `moc`/`rcc`. |
| Cannot open a display | Use the native GUI session or a working X11 display. Use `--validate` for a headless syntax check; test targets supply offscreen mode where appropriate. |
| Channel remains ERROR | Verify the PV name, IOC availability, CA address environment, and read access. `-debug -noerrorpopup` exposes connection/access diagnostics on stderr. |
| Acknowledgement or output PV does not change | Check local/global/passive mode and IOC write access. Failed acknowledgements remain visible; manual ACKT changes require an explicit retry. |
| Cannot open logs or lock files | Create the parent directory and check permissions on the selected files and checkpoint sidecar. A failed log destination change preserves the current destination. |
| Sound fails | Check the audio path, Qt Multimedia plugins/backend, output device, Silence Forever, and beep threshold. Muted fixture playback verifies decoding, not speaker output. |
| Helper or visual tests fail | Check the [test prerequisites](../qtalh/tests/README.md); missing rpcbind, legacy executables, softIoc, or X11 are failures, not skipped coverage. |

For developer orientation, `qtalh/core/` contains parsing and alarm state,
`qtalh/services/` contains CA/logging/options/protocols, and `qtalh/ui/` contains
models, dialogs, and windows. Core depends on Qt Core and EPICS calculation
routines, but not Widgets or CA. The service interfaces and clock/command/log
callbacks support deterministic core tests. Preserve the original license and
source provenance when modifying the port.
