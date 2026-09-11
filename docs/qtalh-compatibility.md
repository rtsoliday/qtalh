# QtALH compatibility inventory

This is an implementation and validation inventory, not a certification of
complete parity. The reference is the checked-in ALH 1.2.35 runtime source;
`alh/documentation/ALH.html` and its images supply the operator reference.
Where the two differ, runtime source is authoritative. Original sources remain
in `alh/`; all Qt runtime changes are under `qtalh/`.

## Configuration and alarm processing

| In-scope feature | Source / Qt implementation | Evidence |
| --- | --- | --- |
| GROUP, CHANNEL, INCLUDE, five mask bits | alConfig.c / core/config.cc, model.cc | Sample and directive round trips, recursive include and malformed-file tests |
| BEEPSEVERITY, BEEPSEVR | beepSevr.c / core/engine.cc | Directive, ancestor threshold and save tests |
| GUIDANCE inline/block, ALIAS, COMMAND | alConfig.c, guidance.c, process.c / core/config.cc, ui/window.cc | Parser round trips; guidance dialog smoke test |
| HEARTBEATPV | heartbeat.c / core/engine.cc | Fake-clock and isolated IOC output tests |
| ACKPV, SEVRPV | acknowledge.c, alLib.c / core/engine.cc | Global ACKS/ack-PV/severity output IOC tests |
| FORCEPV, FORCEPV_CALC, FORCEPV_CALC_A through F | force.c / core/engine.cc | Scalar force/reset IOC test; constant CALC/reconfiguration tests; EPICS postfix evaluator |
| ALARMCOUNTFILTER | alFilter.c / core/engine.cc | Deterministic filter timeout and edge handling |
| SEVRCOMMAND, STATCOMMAND | alLib.c / core/engine.cc | Captured command callbacks for upward/downward/status transitions |
| Transactional open/insert/save | alConfig.c / core/config.cc, ui/window.cc | Bad directives, bad parents, bad CALC, include cycle, atomic QSaveFile; editor save/undo test |
| Local latch and transient alarms | alLib.c, acknowledge.c / core/engine.cc | Local acknowledgement and transient-mask regressions |
| Global ACKS/ACKT | alCA.c, alLib.c / services/channel_access.cc, core/engine.cc | Real IOC acknowledgement and startup ACKT tests |
| Cancel/Disable/NoAck/NoAckT/NoLog | mask.c, showmask.c / core/engine.cc | Mask, log suppression, subscription cancellation tests |
| Group severity/unack/mask counts | alLib.c / core/engine.cc | Hierarchy tests and 10,000-channel burst/acknowledgement |
| Passive mode and CA write restrictions | alCA.c, alLib.c / core/engine.cc, services/channel_access.cc | Fake output capture; real passive/read-only IOC tests |
| Access rights, connection loss and reconnect | alCA.c / services/channel_access.cc | Read-only record; IOC kill/restart; missing-PV startup |
| Timed NoAck, silence and beep thresholds | noAck.c, beepSevr.c / core/engine.cc | Fake clock and hierarchical beep tests |
| History and every-transition processing | current.c / core/engine.cc | State tests; UI repaint separated from alarm callbacks |
| Multiple windows and cancellation ownership | axArea.c, alCA.c / ui/window.cc, services/channel_access.cc | Repeated window destruction; shared CA context and duplicate-PV tests |

The heartbeat argument order is `$HEARTBEATPV PV interval value`, as read by
`alConfig.c`, with defaults of one second and value one. Local mode does not
write heartbeat/severity/ack PVs. Global passive mode also suppresses these
writes. `-D` suppresses file logging only; helper queues retain legacy behavior.
Numeric CALC inputs wait for all named PV inputs; constants can evaluate without
subscriptions. Cancelled subscriptions retain their owner identity so one row
cannot cancel another row using the same PV.

Value-only CA updates refresh the displayed value without adding alarm-log records.
Global ACKS/ACKT changes remain logged, including IOC confirmations of operator
mask changes. Re-enabling a disabled channel runs its severity commands for the
active severity; removing NoAck or Disable acknowledges cleared global transients.
Failed acknowledgements remain visible, and passive mode never writes them.
Failed operator ACKT writes retain the previous displayed mask bit; reapplying
the setting retries the write. Group mask changes handle write failures per
channel, including changes that cancel or restore monitoring.
Scalar Force PV comparisons retain double precision; CALC comparisons retain the
legacy float behavior.
Automatic Force PV ACKT writes that fail retain the latest requested bit per channel
and retry once the target is connected and writable, including cancelled alarm
channels. Until an actual IOC ACKT value has been observed, automatic forcing
writes or defers the requested bit even when it matches the configured mask. A later mask request supersedes the pending setting; disabling or
reconfiguring its Force PV, or closing/reloading the engine, discards it. Successful
settings are not replayed over subsequent IOC changes. Manual ACKT changes retain
the existing explicit-retry behavior.

Mask parsing follows legacy `alSetMask`: only uppercase `C`, `D`, `A`, `T`,
and `L` set bits; other characters are ignored. `$FORCEPV` retains the legacy
PV/mask/force/reset order and numeric parsing defaults (force 1, reset 0).
An invalid force value stops numeric parsing; an invalid reset value becomes 0.
Lowercase `ne` is accepted. Loaded directives are normalized to their effective
values for runtime, editing, and saving. For example, `gate 0 1 -D---` becomes
`gate ----- 1 0`; the misplaced mask is not moved to another field.

A `FORCEPV CALC` directive requires a nonempty `FORCEPV_CALC` expression. This
check runs after the entire configuration and its includes have been read, so
expression ordering remains flexible. Invalid reloads preserve the running
configuration and alarm state.

Heartbeats use a dedicated precise Qt timer, independent of the 200 ms UI refresh.
Their deadlines retain the configured cadence; delayed callbacks skip missed beats
without sending a catch-up burst. Intervals are rounded to milliseconds (minimum
1 ms) and must be finite, positive, and fit Qt's millisecond timer range. Timing
still depends on event-loop availability. Broadcast reloads preserve the operator's
current Silence Forever setting and its menu state.

In runtime mode, Save As exports the current masks without changing the monitored
configuration filename or its broadcast and master-lock identity, matching legacy
ALH. Reloads continue to read the original configuration. Editor Save As changes
the document filename as usual.


Startup channel ERROR events run channel severity commands and publish severity
outputs once, as group startup events do. Empty groups and groups containing only cancelled channels publish their initial
severity output in global active mode. Descriptions use read-only string
subscriptions, preserving readable text even when global alarm writes are denied.
Initially disabled channels publish -1
in global active mode, including channels that are also cancelled. Startup ACKT
settings wait for write access, and are not replayed after a successful write.
Numeric and case-insensitive severity-command triggers are
normalized to the legacy uppercase names when parsed and saved.

Failed severity-output writes retain only the latest requested value for each PV.
They retry when the PV connects or regains write access, and are discarded when
the service is cleared for a reload or close. Ordinary value writes, heartbeat
writes, and acknowledgements are not queued for later replay.

Masks accept legacy short or reordered uppercase letters (for example `D`, `DA`,
and `LATDC`) and save in canonical `CDATL` positions. Unknown mask characters
remain errors. Group names must not duplicate an ancestor or use the reserved
name `NULL`; these ambiguous names are rejected transactionally during parsing
and editing. Repeated names on separate branches remain supported.

All relative INCLUDE paths use the configured directory (`-f` or `ALARMHANDLER`),
including nested includes, GUI open/insert, and broadcast reload. Without an
explicit directory, the `loadConfig` library API uses the top-level file's directory.

## Operator and editor workflows

| Workflow | Qt implementation | Validation |
| --- | --- | --- |
| Compact runtime window, main tree/group panes, severity colors/letters, masks/counts | Window, AlarmModel, RowDelegate | Offscreen and real-display screenshots; alarm-cell click |
| Acknowledge, guidance, related command, force PV, force mask, modify masks, beep severity, timed NoAck | Action menu and dialogs | Core action tests; dialog construction/lifecycle smoke tests |
| Expand one level/branch/all, collapse branch | View menu / tree view | Implemented; manual workflow comparison remains |
| History, config file, alarm/opmod log windows and search | View menu / text viewers | History dialog smoke test; file/log service tests |
| All/active/unack display filters | AlarmModel | Membership and filtered selection/expansion regressions |
| Global beep setting, silence interval, silence forever, log-file selection | Setup menu | Threshold/timer core tests; 5/10/15/30/60-minute choices, default 30 |
| New/open/insert/save/save-as, multiple editor windows | Editor File and Insert menus | Editor save plus transaction tests |
| Group/channel properties, cut/copy/paste/clear, undo/redo | Editor and snapshot model | Add/undo/redo/save and cross-window clipboard tests; properties dialog smoke test |
| Reports and printing | Text report, QPrinter | Implemented; actual paper output unverified |
| Guidance URL/file/text, related processes, help | QDesktopServices, detached QProcess | Guidance dialog tested; external browser/command integration needs operator review |
| Beep and audio files, including Ogg/Vorbis | QApplication beep, QMediaPlayer | Threshold logic tested; bundled tests/alarm.ogg decoded and played twice muted; speaker output unverified |
| Main/runtime fonts, geometry, display | options.cc and Window | Options parsed; screenshot checks at fixed scale |
| Master/slave lock and message/stop-logging/reload broadcast | services/logging.cc | Legacy-format broadcast/reload/stop messages; native POSIX lock handoff and shared-window lock lifetime tests |
| Alarm/opmod files, bounded rotation, dated names, XML-ish format | services/logging.cc | File logging, bounded rotation, and dated/XML tests |

Action shortcuts: Ctrl+A acknowledge, Ctrl+G guidance, Ctrl+P related process,
Ctrl+V force PV, Ctrl+M force mask, Ctrl+S modify mask, Ctrl+B beep severity,
Ctrl+N timed NoAck. View shortcuts: `+`, `*`, Ctrl+`*`, `-`. Editor supports
Alt+Backspace undo, Ctrl+Y redo, Shift+Delete cut, Ctrl+Insert copy,
Shift+Insert paste. File open uses Ctrl+O.

Appearance retains the blue-gray background, yellow MINOR/red MAJOR/white
INVALID and ERROR indicators, group tree at left and group contents at right,
and legacy footer descriptions. The runtime and main rows now use custom
Motif-style controls, branch lines,
content-sized names and the reference initial expansion. See
[the Radiation Monitors comparison](qtalh-appearance.md). Qt scroll bars, some
dialog arrangements and menu labels still differ from Motif. Reports use a
simpler indented text format.
Font family/size/weight from XLFD are approximated where possible. Native file,
print and font rendering differences are expected. These differences currently
exceed a claim of pixel or complete arrangement parity and need operator review.

## Additional operator workflows (2026-09-10)

The following previously missing features are now implemented:

- **Historical log browsers:** both alarm and operator-log browsers search the
  named current log and its `.yyyy-MM-dd` siblings. From/To controls use local
  time and include the entire final minute. With performs a case-sensitive
  substring filter. Modern ALH, legacy ctime, and XML-ish timestamps are
  recognized, and results are sorted chronologically. Searches run in a worker
  thread with Stop support. Results are limited to 100,000 records or 10 MiB;
  truncation, unreadable files, and unrecognized timestamp lines are reported.
  The ordinary live log-file viewers remain available separately.
- **Persistent selection dialogs:** Properties, Force PV, Modify Mask, Force
  Mask, and Beep Severity follow the selected group/channel and refresh when
  their displayed state changes. Reopening the action raises the existing
  window. Background updates preserve unfinished field edits; selecting another
  node loads that node's fields. Force Mask's current summary still updates
  during editing. Editor Properties uses Apply, Cancel, Dismiss, and Help:
  Apply validates and changes the document without closing the window; Cancel
  restores the applied configuration. Selection and dialog shells survive
  Apply, undo/redo, and configuration reload.
- **Disabled Force PV count:** the main-window footer displays the facility-wide
  number of groups/channels whose Force PV is disabled, and clears at zero.
- **Activate ALH:** the editor File menu opens an independent runtime window
  from a validated copy of the current edited configuration, including unsaved
  changes. An unnamed document must first be saved to establish its filename.
  Runtime mask changes do not modify the editor, and closing the editor does
  not close the runtime window. A subsequent runtime reload reads the named
  disk configuration, as usual.
- **Debug diagnostics:** `-debug` writes timestamped, categorized messages to
  stderr for startup/configuration, alarms and operator changes, CA creation,
  connection/access/write activity, commands, logging paths/master changes,
  and broadcasts. Debugging is off by default and does not depend on Qt's
  platform logging destination.

Regression checks cover date boundaries, mixed timestamp formats, filtering,
result limits/cancellation, both browser actions, selection and action-target
rebinding, unfinished edits, Properties Apply/Cancel and undo/redo, the disabled
count, independent runtime activation, and quiet/enabled debug CLI output.
At the 2026-09-10 workflow checkpoint, the local suites passed 90 core, 42 UI,
29 IOC, 53 helper, and 4 visual checks (including setup/cleanup and data rows).
Targeted Valgrind runs for the
new browser and dialog/runtime lifetimes report zero errors and no lost blocks.

## Command line and environment

Supported legacy switches: `-c`, `-global`, `-S`, `-D`, `-s`, `-B`, `-L`,
`-Lfile`, `-T`, `-xml`, `-debug`, `-desc_field`, `-caputackt`, `-mainwindow`,
`-noerrorpopup`, `-maskcolor`, `-a`, `-o`, `-f`, `-l`, `-p`, `-P`, `-O`,
`-m`, `-filter`, `-display`, `--display`, `-geometry`, `-fn`, `-font`, `-help`, `-h`,
`-v`, `-version`. Help/version work without a GUI connection. Qt-specific
`-platform` and `-style` are accepted. `-style fusion` opts into modern layouts
and styling across the GUI; omission or `-style motif` preserves the existing
appearance. `-style=name` is also accepted. `--help`, `--version`, `--validate`, and
`--` are additional conveniences. Unknown switches, including arbitrary Xt
resource overrides, fail with a diagnostic.

`ALARMHANDLER` sets the configuration directory and `ALHMAINFONT` the runtime
button font. Unless `-l` is supplied, the log directory follows the configuration
directory from `-f` or `ALARMHANDLER`, matching legacy ALH. Standard `EPICS_CA_*` variables are read by EPICS Base. `DISPLAY`
and `QT_QPA_PLATFORM` control the display backend. Windows uses `COMSPEC`
(default `cmd.exe`) for configured shell commands. CDEV/CMLOG command-line
extensions are outside scope.

For defaults, option arguments, configuration syntax, and operational guidance,
see the [QtALH user guide](qtalh-user-guide.md). Windows builds the GUI and
portable file services; System V queue helpers and `-P`/`-O` are unavailable.

## Linux/macOS helper and service protocols

`qtalh_printer host port key color` and `qtalh_DB host program key` are headless
QCoreApplication programs. They preserve the original executable argument order
without replacing `alh_printer` or `alh_DB`. The database program calls the RPC
program supplied in argument 2, version 1, procedure 1, using a single XDR string
and a void reply. The Qt printer uses asynchronous TCP; blocking RPC calls
are confined to the separate database helper process. Linux uses TI-RPC
`netpath` transport selection; macOS uses TCP through the system Sun RPC SDK.

Legacy `msgsnd(text, strlen(text), ...)` uses the first native `sizeof(long)`
text bytes as `mtype`, followed by `strlen(text)` bytes of payload. The original
code consequently reads beyond the string allocation. Qt explicitly allocates
and zero-pads the tail, reconstructs the leading text bytes on receipt, and
bounds its messages. This format is native-ABI dependent, just as the legacy
format is; cross-endian/cross-word-size communication is not claimed.

For mixed deployments Qt sending is limited to `250 - sizeof(long)` text bytes
to fit the legacy receiver. Oversized records are reported and not queued;
file logging is unaffected. Qt receiving allocates an 8192-byte payload buffer.
Printer output retains source offsets and source color-mode behavior, including
legacy quirks, rather than silently changing the wire output.

Tests cover Qt-sender to each legacy helper and each Qt helper against local
TCP/RPC endpoints, plus a legacy-layout sender received by the Qt queue decoder.
All four printer color modes are covered. Both Qt helpers continue draining
queued records immediately after processing a record; the 50 ms poll delay is
used while idle (and for printer connection retries). Ordered bursts (100 records
on Linux, 32 on macOS to fit its default queue limits)
are tested against local TCP and RPC endpoints. The printer test collects each
TCP stream in connection acceptance order; callback order across independent
sockets does not determine record order. With database logging enabled,
acknowledgements emit the legacy code-6 `Ack Channel---` record for each affected
channel, including channels acknowledged through a parent group.
Physical printers and production RPC services are intentionally not used.
Alarm logs are registered under their canonical path after creation, so windows
opening the same new log through directory or file symlinks share one writer and
circular cursor. Opening another window preserves existing records in both
unlocked and locked modes.
Bounded alarm logs save their circular write position in a sibling
`.qtalh-position` file, kept open by the shared log writer. The file contains two
96-byte binary checkpoints. Each contains the format marker, a sequence number,
record count, next slot, an incremental ring fingerprint, and a SHA-256 checksum
of the checkpoint. Position updates alternate between slots using `pwrite` on Unix or
`QFile::seek`/`write`/`flush` on Windows after the alarm record is flushed, without per-record file creation, rename, or fsync.
Recovery selects the newest intact checkpoint whose fingerprint matches the log.
A torn or stale checkpoint is rejected; a matching checkpoint in the other slot
remains usable, including when the second slot is truncated. This preserves
insertion order across restart, capacity changes, and master handoff even when
timestamps are equal. The incremental fingerprint combines hashes of individual
records and their physical slots without rehashing the entire log per alarm.

Earlier experimental QtALH JSON metadata formats are not supported.
Plain ALH logs remain supported. Without a matching checkpoint, recovery uses the
oldest record timestamp; the original order of equal or out-of-order timestamps
cannot be recovered reliably from the legacy format alone. An alarm write and its
checkpoint are separate writes, as before: interruption between them can require
that fallback. Position-save failures are reported while the alarm record remains
in the log. Short/interrupted checkpoint writes are retried where possible.

Log timestamps are cached within their existing one-second precision, with older
filter-event timestamps and clock reversals handled by changing the cache key.

Changing an alarm or operation log destination opens the new file before
replacing the current destination. Failed opens preserve the working log and
its reported path. Plain and dated log switching are covered by regression tests.

Related-process menus retain the legacy handling of leading, trailing and
repeated `!` delimiters, and reject incomplete label/command pairs.

Broadcast senders retain exclusive ownership for the delivery interval, including
between windows in the same process. Attempts to overwrite a pending broadcast
return a busy error. Message IDs include a monotonic millisecond value and process
ID within the legacy reader's buffer, so successive sends remain distinguishable.

On Linux/macOS, log lock files use POSIX `lockf` and the legacy `.LOCK` basename;
broadcasts use
`.MESS`/`.MESSLOCK` and the legacy four-line record. Native POSIX lock handoff
and closing a second window are tested. Application windows on Unix share lock
descriptors and
broadcast ownership by device/inode, so directory symlinks, file symlinks, and
hard links cannot cause duplicate descriptors to release another window's lock.
Windows uses native file locking and shares lock lifetime between application
windows; it does not interoperate with POSIX locks. End-to-end legacy GUI
broadcast consumption still requires validation.

## Validation status and remaining acceptance work

The recorded 2026-09-11 Linux Qt 5 checkpoint passed core (92), UI (43),
IOC (29), helpers (63), and visual (4),
with no failures or skipped tests. Regressions cover Force PV ACKT recovery before
the first IOC value, alias-safe lock lifetime and broadcast ownership, runtime
Save As/reload identity, reload silence, incomplete CALC rejection, and heartbeat
cadence. The log-sharing follow-up rebuilt QtALH and passed all 53 helper tests,
including six new symlink cases that failed before the fix. The printer burst
test now validates connection acceptance order and
passes with the unchanged printer helper. Counts include Qt Test setup/cleanup
and data rows. The bounded-log burst regression writes 1,000 records in about
0.15 seconds locally, compared with 27.3 seconds in the pre-fix review probe.
Restart tests cover both checkpoint slots, equal timestamps, changed capacity,
torn/truncated checkpoints, and stale metadata after external writes. A forced
short-write test verifies that the alarm is retained, the other slot survives,
and checkpoint writes recover after the error clears. The optional real-display fixture captures both applications
against the same loopback IOC, with a HIHI/MAJOR event and matching group count.
It also compares ordered logs for normal → MINOR → MAJOR → normal against
Motif. This is a representative differential trace, not exhaustive trace or
pixel equivalence.

The 10,000-channel core benchmark asserts final severity and acknowledgement
counts. The offscreen 10,000-row window plus alarm burst took approximately
0.73 seconds locally; model child lists are cached to avoid quadratic lookup.
The [CPU and logging comparisons](qtalh-performance.md) cover controlled
Motif/Qt workloads. Broader responsiveness/memory comparisons,
complete differential alarm traces, exhaustive menu/dialog action coverage,
and complete operational memory profiling remain acceptance work. Live CALC
inputs and changing access-rights IOC scenarios pass. Valgrind reports no memory
errors or lost allocations in the full core suite and in the checked runtime,
editor, clipboard, dialog and repeated-window UI lifecycles. It identified an
unowned group-properties field during development; that leak is fixed and the
full leak check passes without suppressions. The AddressSanitizer build
could not link because the system libasan.so.5.0.0 runtime is absent.
Qt 6.11.2 builds on macOS arm64; the core, UI, IOC, and helper suites pass,
including muted Ogg/Vorbis alarm playback through the native Cocoa event loop.
A Qt-only root build with MOTIF_INC=/nonexistent passes and reports the omitted
legacy variant. Qt binaries have no direct Motif/Xt dependency. A separate
machine/container with Motif packages physically absent has not been tested.

These are historical validation records, not a guarantee for every host or
revision. Use the [test guide](../qtalh/tests/README.md) to reproduce checks;
platform-specific cases and result counts differ.

Do not treat this inventory or successful build as completion of every criterion
in the original parity plan. The implementation is available for testing while
those explicit acceptance gaps and documented visual differences remain.

The event-loop implementation follows the non-preemptive callback guidance in
[EPICS CA](https://docs.epics-controls.org/en/latest/ca-ref/function-call-interface-general-guidelines.html)
and [Qt QSocketNotifier](https://doc.qt.io/qt-6/qsocketnotifier.html). Callbacks
are serialized on the GUI thread; polling is guarded against reentrancy and
subscriptions are cancelled before model destruction.
