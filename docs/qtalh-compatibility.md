# QtALH compatibility inventory

This is an implementation and validation inventory, not a certification of
complete parity. The reference is the checked-in ALH 1.2.35 runtime source;
`alh/documentation/ALH.html` and its images supply the operator reference.
Where the two differ, runtime source is authoritative. Original sources remain
in `alh/`; all Qt runtime changes are under `qtalh/`.

## Configuration and alarm processing

| In-scope feature | Source / Qt implementation | Evidence |
| --- | --- | --- |
| GROUP, CHANNEL, INCLUDE, five mask bits | alConfig.c / core/config.cc, model.cc | Sample and directive round trips, recursive include, include file-type and malformed-file tests |
| BEEPSEVERITY, BEEPSEVR | beepSevr.c / core/engine.cc | Directive, ancestor threshold and save tests |
| GUIDANCE inline/block, ALIAS, COMMAND | alConfig.c, guidance.c, process.c / core/config.cc, ui/window.cc | Parser round trips; guidance dialog smoke test |
| HEARTBEATPV | heartbeat.c / core/engine.cc | Fake-clock and isolated IOC output tests |
| ACKPV, SEVRPV | acknowledge.c, alLib.c / core/engine.cc | Global ACKS/ack-PV/severity output IOC tests |
| FORCEPV, FORCEPV_CALC, FORCEPV_CALC_A through F | force.c / core/engine.cc | Scalar force/reset IOC test; constant CALC/reconfiguration tests; EPICS postfix evaluator |
| ALARMCOUNTFILTER | alFilter.c / core/engine.cc | Deterministic filter timeout and edge handling |
| SEVRCOMMAND, STATCOMMAND | alLib.c / core/engine.cc | Captured command callbacks for upward/downward/status transitions |
| Transactional open/insert/save (whole-file rejection; see [configuration errors](#configuration-errors-and-migration)) | alConfig.c / core/config.cc, ui/window.cc | Bad directives, bad parents, bad CALC, include cycle, atomic QSaveFile; editor save/undo test |
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

`$FORCEPV_CALC_A` through `F` inputs are constants only when the complete token
is a number, such as `0`, `-2`, or `2e0`; any other token is a PV name, both when
loading and in the Force PV dialog. ALH 1.2.35 used the token's leading number
instead. When loading, a token with a nonzero leading number became that
constant and any other token, including `0`, named a PV; the dialog treated any
token not starting with a letter as its leading number. Under ALH, inputs such as
`1ID:gate` or `32idc:shutter` were therefore constants 1 and 32, and a loaded
`0` named a PV that normally never connected, so the calculation never ran.
QtALH monitors those PVs and treats `0` as the constant zero, so a migrated
calculation can apply or reset masks differently. Check inputs that begin with
a digit.

Value-only CA updates refresh the displayed value without adding alarm-log records.
Global ACKS/ACKT changes remain logged, including IOC confirmations of operator
mask changes. Re-enabling a disabled channel runs its severity commands for the
active severity; removing NoAck or Disable acknowledges cleared global transients.
Failed acknowledgements remain visible, and passive mode never writes them.
Failed operator ACKT writes retain the previous displayed mask bit; reapplying
the setting retries the write. Group mask changes handle write failures per
channel, including changes that cancel or restore monitoring.
Scalar Force PV comparisons retain double precision; CALC force and numeric-reset
comparisons retain legacy float precision. Qt intentionally uses that same float
comparison for the previous forced value in an `NE` reset (also selected when
reset equals force). ALH instead compares the previous value as a double, which
can leave a force mask applied after a rounded match. For example, with force
16777216, CALC results 16777217 then 16777220 apply and reset the mask in Qt;
ALH applies it but fails to reset. Qt preserves this correction, with regressions
for channel/group forcing, explicit `NE`, equal force/reset values, and save/reload.
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
QtALH rejects nonfinite numeric results (`NaN`, infinity, and overflow), unlike
ALH, because a nonfinite reset can leave a force mask applied indefinitely.
Use a finite reset value or `NE`; valid finite values retain their existing behavior.
Reset numbers use the complete token, extending ALH's nine-character reset field
so decimal and scientific-notation values survive saving, reloading, and editor actions.
Force and reset values are saved in their shortest exact form (`0.3`, `1e-05`), so
reset values of up to nine characters remain readable by legacy ALH; ALH truncates
longer reset values, such as `1.2345e-06` or `1234567890`.
Lowercase `ne` is accepted. Loaded directives are normalized to their effective
values for runtime, editing, and saving. For example, `gate 0 1 -D---` becomes
`gate ----- 1 0`; the misplaced mask is not moved to another field.

A `FORCEPV CALC` directive requires a nonempty `FORCEPV_CALC` expression. This
check runs after the entire configuration and its includes have been read, so
expression ordering remains flexible. Invalid reloads preserve the running
configuration and alarm state.

Heartbeats use a dedicated precise Qt timer, independent of the 200 ms UI refresh.
Heartbeat, count-filter, timed NoAck, and silence-interval deadlines use a monotonic
clock, separate from wall-clock event timestamps and absolute shelf expirations.
System clock corrections therefore do not extend or prematurely expire these intervals.
Heartbeat deadlines retain the configured cadence; delayed callbacks skip missed beats
without sending a catch-up burst.
Unavailable heartbeat PVs suspend the precise timer and generate one diagnostic
per outage. The ordinary refresh resumes scheduling when CA reports write access.
Severity/heartbeat outputs use DBR_SHORT and ACKPV uses DBR_ENUM, preserving
legacy integer-to-string conversion as well as numeric output values. Intervals are rounded to milliseconds (minimum
1 ms) and must be finite, positive, and fit Qt's millisecond timer range. Timing
still depends on event-loop availability. Broadcast reloads preserve the operator's
current Silence Forever setting and its menu state.

In runtime mode, Save As exports the current masks without changing the monitored
configuration filename or its broadcast and master-lock identity, matching legacy
ALH. Reloads continue to read the original configuration. Editor Save As changes
the document filename as usual.

Runtime File > Open replaces the configuration in place, as ALH's
`setupConfig`/`setupArea` does: Channel Access subscriptions and heartbeat
output for the previous configuration stop, and later reloads read the opened
file. As with ALH's lock file, logs and the master lock stay bound to the
startup configuration. QtALH also keeps the `.MESS`/`.MESSLOCK` broadcast
identity, where ALH rebinds it to the opened file without releasing the previous
descriptors or polling timer.


Channel startup severity commands use ALH's initial ERROR baseline: an initial
MAJOR or NO_ALARM invokes matching DOWN commands, while an initial ERROR does
not invoke channel severity commands. Startup severity outputs are published once;
group startup commands retain their separate initial aggregate behavior. Empty
groups and groups containing only cancelled channels publish their initial severity
output in global active mode. Descriptions use read-only string
subscriptions, preserving readable text even when global alarm writes are denied.
Initially disabled channels publish -1
in global active mode, including channels that are also cancelled. Startup ACKT
settings wait for write access, and are not replayed after a successful write.
ALH wrote them only to channels connected after its startup connection wait and
did not retry; Qt also writes when an IOC connects or grants write access later.
Numeric and case-insensitive severity-command triggers (for example `UP_2` or
`DOWN_minor`) are normalized to the uppercase names when parsed and saved, and
then match only that severity. Legacy ALH compares these names case-sensitively
and treats an unrecognized severity as `ANY`, so ALH runs such a command on every
change in that direction. `$BEEPSEVERITY` and `$BEEPSEVR` also accept numeric or
lowercase severities, which ALH ignores. `$STATCOMMAND` triggers must match a
status name exactly; ALH's prefix match runs `READ_ACCESS` and `WRITE_ACCESS`
commands on `READ` and `WRITE` status instead. Use the uppercase severity and
status names in configurations shared with ALH. A `READ` command also runs on a failed
Channel Access alarm read (see Communication errors), which ALH ignores.

Failed severity-output writes retain only the latest requested value for each PV.
They retry when the PV connects or regains write access, and are discarded when
the service is cleared for a reload or close. Ordinary value writes, heartbeat
writes, and acknowledgements are not queued for later replay. A Channel Access
output that is not connected or not writable produces one "PV not connected or
not writable" diagnostic per outage, not one per write attempt; the outage ends
when the PV becomes writable or a write to it succeeds. A `pva://` output that
is not connected behaves the same way; its outage ends when the PV connects or a
write to it completes. Legacy ALH was silent
for a disconnected severity PV and logged every write refused for lack of
access.

Masks accept legacy short or reordered uppercase letters (for example `D`, `DA`,
and `LATDC`) and save in canonical `CDATL` positions. As in legacy `alSetMask`,
other characters, including lowercase letters, are ignored rather than rejected,
and `--validate` does not report them: `DISABLED` loads as `-DA-L`, and `cd` sets
no bits. Group names must not duplicate an ancestor or use the reserved
name `NULL`; these ambiguous names are rejected transactionally during parsing
and editing. Repeated names on separate branches remain supported.

Per-node `$BEEPSEVR`, `$ACKPV`, and `$FORCEPV_CALC` use the last directive,
while first-wins options such as ALIAS and COMMAND retain their legacy precedence.
GROUP, CHANNEL, and INCLUDE declarations accept trailing comments beginning with
a whitespace-separated `#`; hashes in names, commands, and guidance are preserved.
Missing `$ALARMCOUNTFILTER` arguments default to count 1 and seconds 1 and save
in full form. Multiple inline GUIDANCE blocks
are combined in order, preserving blank lines through display, editing and saving.

All relative INCLUDE paths use the configured directory (`-f` or `ALARMHANDLER`),
including nested includes, GUI open/insert, and broadcast reload. Without an
explicit directory, the `loadConfig` library API uses the top-level file's directory.
ALH does not check `ferror()`, so a read error could leave a shorter valid
configuration. QtALH rejects a read error in the configuration or any include;
`--validate` fails and GUI open/insert and broadcast reload keep the running
configuration. INCLUDE targets must be regular files (or symbolic links to
them). ALH opens whatever the name refers to: a FIFO blocks startup or reload
until a writer opens it, and a device such as `/dev/zero` is read without end.
QtALH rejects a device, FIFO, socket, or directory with
`Include is not a regular file` before opening it, so an INCLUDE of a pipe
such as `/dev/stdin` no longer works. Directories were already rejected, as
`Cannot open include`.
The top-level configuration may still be a pipe (`qtalh <(generator)`).

Startup ACKT writes also apply to cancelled channels through connections without
alarm subscriptions. A completed startup setting is tracked per PV and is not
replayed when monitoring is later added. Global group acknowledgement follows
ALH's group traversal. It enters the selected group and each descendant subgroup
that still has an unacknowledged alarm on a channel that is neither Disabled nor
NoAck. In every group it enters, it writes ACKS, and any `$ACKPV`, for each
channel with an outstanding IOC acknowledgement, including Disabled and NoAck
channels. A subgroup whose only outstanding alarms are on Disabled or NoAck
channels is not entered; shelved channels remain excluded. Qualifying downward
unacknowledged severity changes also reset Silence Current, matching ALH.

Dated alarm records use the event's local date, including count-filter callbacks
processed after midnight. Current log viewers and operation logs keep today's
destination. Database operation codes are carried explicitly: channel/group mask
changes use 7/8 and automatic forcing uses 9/10. Modify Mask retains ALH's code-7
summary and per-channel code-8 `Group Mask ID ---` audit records; timed NoAck also
emits these per-channel records when database logging is enabled. Free-text
operation descriptions do not determine protocol codes.

Acknowledgement, timed NoAck and silence operation texts match ALH. Mask, Force
PV and beep-severity texts do not, so scripts that search operation logs or
database records for ALH phrases need updating:

| Action | QtALH text | ALH text |
| --- | --- | --- |
| Force Mask Apply | `Change Mask <mask>` | `OPER Channel Set Mask <name> [mask]` or `OPER Group Set Mask <name> [mask]` |
| Force Mask Reset | `Reset Mask` | `OPER Channel Reset Mask <name> [mask]` or `OPER Group Reset Masks <name>` |
| Automatic force | `Force Mask <mask>` | `Channel forcePV FORCE <mask> [value] [pv]` or `Group forcePV FORCE ...` |
| Automatic reset | `Force PV Reset Mask` | `Channel forcePV RESET <mask> [value] [pv]` or `Group forcePV RESET ...` |
| Modify Mask | `Modify Mask <bit> <Off\|On\|Reset>` | `Channel Mask [Enable / Disable] ON <mask>` or `Group Mask [...]` |
| Force PV Apply | `Change Force PV <directive>`, `Disable Force PV`, or `Enable Force PV` | `Channel forcePV modified <mask> [value] [pv]` (group text has a leading space) |
| Beep Severity | `Set beep severity <severity>` | `Beep Severity set to <severity>` |

QtALH's automatic force and reset records omit the force value and PV name that
ALH includes. Database codes 7–10 are the same in both.

Disabled-channel startup callbacks update channel state without triggering group
severity commands from siblings still awaiting connection. Groups containing only
disabled or cancelled channels still publish their initial severity output.
Combined Cancel/Disable changes follow ALH's Cancel-before-Disable ordering;
removing Disable while Cancel remains set does not restore a local latch.
Group beep indicators summarize the highest threshold in the group and its
descendants, separately from the group's own audio threshold. Acknowledgement
operation records retain the current severity after the latched severity, for
both file and database logging.

Initially cancelled and disabled channels respect ALARMCOUNTFILTER on their first
monitor after Add/Enable; only the ordinary initial ERROR baseline bypasses filtering.
Disabled channels start at NO_ALARM, so enabling them before their first monitor
cannot synthesize an ERROR or run an ERROR command. The initial-connection log
exclusion does not discard the first alarm after Add on an initially cancelled
channel. Enable/Add of a cached active alarm records its state in the alarm log
(unless NoLog is set) and history, and publishes its severity in global active
mode. Changing Disable while Cancel remains set does not write the channel's
severity output. Operator Modify Mask and Force Mask Apply/Reset clear Silence
Current even when the audible severity is unchanged.

Group mask actions process direct channels before subgroups, matching ALH even
when declarations are interleaved. Saving and reloading therefore preserves the
sequence of severity commands during Apply, Reset, and Modify Mask actions.
Fixed-argument directives accept whitespace-separated trailing `#` comments;
command, alias, and guidance text remains intact. CALC accepts trailing comments
after a valid expression while preferring a valid full expression, so spaced
expressions and the `#` not-equal operator are both accepted. Spaced expressions
are an intentional QtALH extension: legacy ALH reads only the first
whitespace-delimited token of `$FORCEPV_CALC`, so it evaluates `A > B` and
`A # B` as `A`. The same file can then apply a force mask in one program and
not the other. QtALH saves the complete expression; write expressions without
spaces, such as `A#B`, in files that legacy ALH must also read. `$SEVRPV -`
is an unset placeholder: the first subsequent real PV takes precedence.

### Configuration errors and migration

QtALH uses a configuration only when every line of it and its includes is
valid. Legacy ALH reads line by line: it logs an `Input error` for a line it
cannot use, or silently ignores the problem, and loads the rest of the file.
QtALH can therefore reject a file that ALH has run for years. At startup and
with `--validate`, QtALH prints the first problem and exits with status 1, so
no alarms are monitored until the file is corrected. GUI Open/Insert and
broadcast reloads report the error and keep the current configuration.
Examples of lines that ALH tolerates but QtALH rejects:

| Line | Legacy ALH | QtALH |
| --- | --- | --- |
| Unrecognized statement, such as `FOO bar` | `Invalid input line`; skipped | `Unknown statement` |
| `GROUP` or `CHANNEL` naming a parent that is not the current group or an ancestor | `Cannot find parent`; skipped | `Parent group not found` |
| Unrecognized or stray `$` line, such as a second `$END` | `Invalid Optional Line`; skipped | `Unsupported directive` |
| `$STATCOMMAND` or `$ALARMCOUNTFILTER` after a group | `Context not a channel`; skipped | `applies to channels` |
| `$ACKPV` after a group | Not checked | `ACKPV applies to channels` |
| Extra non-comment tokens on `GROUP`, `CHANNEL` (after the mask), `INCLUDE`, or `$HEARTBEATPV` lines | Ignored | Declaration or heartbeat error |
| Non-numeric `$HEARTBEATPV` interval | 1 second | Heartbeat interval error |
| `$BEEPSEVERITY` or `$BEEPSEVR` value that is not a severity, such as `HIGH` | Ignored; earlier threshold kept | `Unknown severity` |
| Severity with a suffix, such as `$BEEPSEVR MAJOR_ALARM` or `UP_MAJOR_ALARM` | Prefix match (`MAJOR`) | `Unknown severity` |
| `$SEVRCOMMAND` trigger without `UP_`/`DOWN_`, or with an unknown severity | Usually runs as `ANY` (downward if the trigger starts with `D`) | Trigger or severity error |
| `$STATCOMMAND` with an unknown status | Stored; never runs | `Unknown status command trigger` |
| `INCLUDE` naming a FIFO or a device | A FIFO blocks until a writer opens it; a device such as `/dev/zero` never finishes | `Include is not a regular file` |

ALH applies directives that follow a skipped declaration to an earlier group or
channel, so review them when correcting the declaration. Before migrating, run
`qtalh --validate` with the production `-f` directory (or `ALARMHANDLER`) on
every top-level configuration. Each run reports one problem; repeat until the
file validates. `Input error` entries in ALH's operation log identify lines that
ALH already skips; silently ignored lines produce no entry.

## Operator and editor workflows

| Workflow | Qt implementation | Validation |
| --- | --- | --- |
| Compact runtime window, main tree/group panes, severity colors/letters, masks/counts | Window, AlarmModel, RowDelegate | Offscreen and real-display screenshots; alarm-cell click |
| Acknowledge, guidance, related command, force PV, force mask, modify masks, beep severity, timed NoAck | Action menu and dialogs; timed NoAck toggles the selection at once, without ALH's Modify NoAck Mask dialog | Core action tests; dialog construction/lifecycle smoke tests |
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
Ctrl+N timed NoAck (applied at once, without ALH's dialog). View shortcuts:
`+`, `*`, Ctrl+`*`, `-`. Editor supports
Alt+Backspace undo, Ctrl+Y redo, Shift+Delete cut, Ctrl+Insert copy,
Shift+Insert paste. File open uses Ctrl+O.

The default modern appearance keeps ALH's yellow MINOR/red MAJOR/white INVALID
and ERROR meanings, row order, menus and footer descriptions in an updated
presentation; see [the appearance guide](qtalh-appearance.md).
With `-style motif`, the classic appearance retains the blue-gray background,
yellow MINOR/red MAJOR/white
INVALID and ERROR indicators, group tree at left and group contents at right,
and legacy footer descriptions (one legend line is worded differently; see
[Observed differences from legacy runs](#observed-differences-from-legacy-runs-2026-10-17)).
The runtime and main rows now use custom
Motif-style controls, branch lines,
content-sized names and the reference initial expansion. See
[the Radiation Monitors comparison](qtalh-appearance.md). Qt scroll bars, some
dialog arrangements and menu labels still differ from Motif. Reports use a
simpler indented text format.
Current Alarm History lists the newest entry first in both appearances; ALH
lists the oldest of its ten entries first and the newest last.
Font family/size/weight from XLFD are approximated where possible. Native file,
print and font rendering differences are expected. These differences currently
exceed a claim of pixel or complete arrangement parity and need operator review.

## Additional operator workflows (2026-09-10)

The following previously missing features are now implemented:

- **Historical log browsers:** both alarm and operator-log browsers search the
  named current log and its `.yyyy-MM-dd` siblings. From/To controls use local
  time and include the entire final minute. With performs a case-sensitive
  substring filter over complete records, including legacy continuation lines. Modern ALH, legacy ctime, and XML-ish timestamps are
  recognized, and results are sorted chronologically. Searches run in a worker
  thread with Stop support. Closing a browser or reloading requests cancellation
  without waiting for filesystem I/O; workers release themselves when reads finish.
  Results are limited to 100,000 records or 10 MiB;
  truncation, unreadable files, and records without recognizable timestamps are reported.
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

## Qt-specific timed shelving

Runtime channels can be shelved for 1 minute to 365 days with a required
operator-entered username and reason.
Group actions skip already shelved channels; group unshelving clears all
remaining descendant shelves. Shelving is an independent presentation state,
not a legacy mask bit. Separate incremental aggregates exclude shelved channels
from visible alarm counts, filters, the runtime indicator, and audio without
changing alarm processing, raw aggregates, commands, or output PVs.

The runtime provides Shelve/Unshelve actions, inline markers, a shelved count,
and a list with underlying state, deadlines, countdowns, reasons, early
unshelving, and individual changes. Core expiry uses the existing deadline
scheduler. Latches remain governed by the normal local/global rules, and bulk
acknowledgement skips shelved channels.

Same-runtime reload restores original shelves by unique full node identity and
preserves matched local outstanding latches; global latches follow IOC updates.
Missing or ambiguous identities are logged and discarded. Shelves are never
serialized into `.alhConfig`, persisted across restart, or shared with other
runtimes. Save As does not alter them. The original Motif application and helper
protocols are unchanged.

Regression coverage includes fake-clock boundaries, transient latches, group
behavior, mask/filter/Force PV interactions, unchanged automation and PV writes,
reload identity matching, UI controls in both appearances, a live loopback IOC,
and 10,000-channel shelving/expiry. See the test guide for reproduction; these
checks do not establish field acceptance on every platform.

## Command line and environment

Supported legacy switches: `-c`, `-global`, `-S`, `-D`, `-s`, `-B`, `-L`,
`-Lfile`, `-T`, `-xml`, `-debug`, `-desc_field`, `-caputackt`, `-mainwindow`,
`-noerrorpopup`, `-maskcolor`, `-a`, `-o`, `-f`, `-l`, `-p`, `-P`, `-O`,
`-m`, `-filter`, `-display`, `--display`, `-geometry`, `-fn`, `-font`, `-help`, `-h`,
`-v`, `-version`. Help/version work without a GUI connection. Qt-specific
`-platform` and `-style` are accepted. The modern appearance is the default;
`-style motif` selects the classic Motif-influenced appearance, and `-style
fusion` or another Qt style applies that style to the modern layouts. `-style=name` is also accepted. `--help`, `--version`, `--validate`,
`--to-phoebus`, `--from-phoebus`, and `--` are additional conveniences. Unknown
switches, including arbitrary Xt resource overrides, fail with a diagnostic.

Other command-line differences from ALH:

- Help and version text go to stdout with exit status 0; ALH writes them to
  stderr and exits with status 1. The version line is
  `QtALH 1.0.0 (based on ALH 1.2.35) Qt <version> <EPICS version>` rather than
  `ALH Version 1.2.35  (<EPICS version>)`.
- Option values may begin with `-`, so a missing value takes the next switch
  as its value (for example, `-a -L` uses `-L` as the alarm filename and does
  not enable locking). ALH rejects such values for its file, directory, count,
  and key options; `-geometry` and `-fn` accept them in both.
- `-P` and `-O` do not require `-L`; ALH refuses them without it. Without `-L`
  every instance forwards its alarm records, so several unlocked consoles send
  duplicate printer and database records.
- `-fn` and `-font` take precedence over `ALHMAINFONT`; in ALH, `ALHMAINFONT`
  wins.

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
Global alarm records retain ALH's `ackT`/`noackT` tokens in text, XML, and forwarded
bodies. Database headers identify the effective OS user and supply nonempty
fallbacks for missing identity fields, including `unknown_display` when `DISPLAY`
is unset. Header whitespace is normalized to keep field positions intact.
Printer output retains source offsets and source color-mode behavior, including
legacy quirks, rather than silently changing the wire output.

Tests cover Qt-sender to each legacy helper and each Qt helper against local
TCP/RPC endpoints, plus a legacy-layout sender received by the Qt queue decoder.
All four printer color modes are covered. Both Qt helpers continue draining
queued records immediately after processing a record; the 50 ms poll delay is
used while idle. Printer connection/write failures retain the current record and
retry after one second; connection or write inactivity times out after ten seconds.
The printer reports each outage once on stderr and reports when delivery resumes.
Ordered bursts (100 records on Linux, 32 on macOS to fit its default queue limits)
are tested against local TCP and RPC endpoints. The printer test collects each
TCP stream in connection acceptance order; callback order across independent
sockets does not determine record order. With database logging enabled,
acknowledgements emit the legacy code-6 `Ack Channel---` record for each affected
channel, including channels acknowledged through a parent group.
Physical printers and production RPC services are intentionally not used.
Without `-T` or `-L`, ALH opens the alarm log with `fopen(..., "w+")`; QtALH
likewise empties the alarm log it opens at startup or through **Setup → New Alarm
Log File…**. With `-T` or `-L`, both reopen an existing alarm log without
emptying it.
Alarm logs are registered under their canonical path after creation. Before
truncating a newly opened path, its native file identity is compared with active
writers, so directory symlinks, file symlinks, and hard links share one writer and
circular cursor. Opening another window preserves existing records in both
unlocked and locked modes.
The `-L` command-line option defaults to unlimited alarm retention, matching ALH;
QtALH permits an explicit `-m` override in either argument order. Without `-L`,
the default remains 2000 records.
At startup, ALH opens `-T` and `-L` alarm logs for appending and does not shrink
an existing file. QtALH bounds these files with `-T`, or with `-L` and an explicit
`-m`. When an existing log, such as today's `-T` file, holds more records than the
limit, the first alarm keeps only the newest records and deletes the earlier ones.
`-m 0` keeps every existing record.
Bounded alarm logs save their circular write position in a sibling
`.qtalh-position.<identity>` file, kept open by the shared log writer. The file contains two
96-byte binary checkpoints. Each contains the format marker, a sequence number,
record count, next slot, an incremental ring fingerprint, and a SHA-256 checksum
of the checkpoint. Position updates alternate between slots using `pwrite` on Unix or
`QFile::seek`/`write`/`flush` on Windows after the alarm record is flushed, without per-record file creation, rename, or fsync.
Recovery selects the newest intact checkpoint whose fingerprint matches the log.
A torn or stale checkpoint is rejected; a matching checkpoint in the other slot
remains usable, including when the second slot is truncated. This preserves
insertion order across restart (with `-T` or `-L`; otherwise startup empties the
log), capacity changes, and master handoff even when timestamps are equal. The incremental fingerprint combines hashes of individual
records and their physical slots without rehashing the entire log per alarm.

Earlier experimental QtALH JSON metadata formats are not supported.
Plain ALH logs remain supported. Without a matching checkpoint, recovery uses the
oldest record timestamp; the original order of equal or out-of-order timestamps
cannot be recovered reliably from the legacy format alone. An alarm write and its
checkpoint are separate writes, as before: interruption between them can require
that fallback. A position-save failure is reported once, and again only after a
checkpoint succeeds or the failure changes; every alarm record is still written to
the log. Unlike ALH, which opens only the log file, bounded logging also creates
the checkpoint in the log directory, so a pre-created log in a read-only directory
records alarms but saves no write position. Short/interrupted checkpoint writes
are retried where possible.

Log timestamps are cached within their existing one-second precision, with older
filter-event timestamps and clock reversals handled by changing the cache key.

Startup log-open diagnostics are delivered after the caller installs its error
handler. Failed files are retried independently on records and the two-second
service timer; a failed operation log cannot reopen or truncate the healthy alarm
log. Pending startup broadcasts are consumed after message/reload handlers are
installed, while master-lock acquisition remains immediate.

Explicit `./` and `../` configuration and log filenames override the configured
directories, as in ALH. Geometry supports position-only forms and negative edge
offsets, including `-0`; it targets the compact runtime by default and the main
window with `-mainwindow` or `-c`.

Changing an alarm or operation log destination opens the new file before
replacing the current destination. Failed opens preserve the working log and
its reported path. Plain and dated log switching are covered by regression tests.

Related-process menus retain the legacy handling of leading, trailing and
repeated `!` delimiters, and reject incomplete label/command pairs.

Unlike ALH, which passes command text unchanged to the shell, QtALH replaces a
leading executable whose basename is `medm` or `medm.exe` (bare, quoted, or an
absolute path such as a site wrapper) with `qtedm` resolved from `PATH`,
preserving the arguments. This applies to `$COMMAND`, `$SEVRCOMMAND`, and
`$STATCOMMAND`; the configuration text is unchanged, and the P tooltip shows
the effective command. A command whose first word is not `medm`, such as
`exec /opt/site/bin/medm ...`, is passed unchanged.

Broadcast senders retain exclusive ownership for the delivery interval, including
between windows in the same process. Attempts to overwrite a pending broadcast
return a busy error. Message IDs include a monotonic millisecond value and process
ID within the legacy reader's buffer, so successive sends remain distinguishable.

On Linux/macOS, log lock files use the legacy `.LOCK` basename. Linux takes open
file description locks (`F_OFD_SETLK`), which conflict with legacy ALH's POSIX
`lockf` locks but, unlike them, survive other code in the process (such as a Qt 6
file dialog sniffing file types) opening and closing the lock file; macOS uses
POSIX `lockf`. Lock descriptors are not inherited by commands. Broadcasts use
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
and data rows. The bounded-log burst regression covers writing 1,000 records.
Restart tests cover both checkpoint slots, equal timestamps, changed capacity,
torn/truncated checkpoints, and stale metadata after external writes. A forced
short-write test verifies that the alarm is retained, the other slot survives,
and checkpoint writes recover after the error clears. The optional real-display fixture captures both applications
against the same loopback IOC, with a HIHI/MAJOR event and matching group count.
It also compares ordered logs for normal → MINOR → MAJOR → normal against
Motif. This is a representative differential trace, not exhaustive trace or
pixel equivalence.

The 10,000-channel core benchmark asserts final severity and acknowledgement
counts. Model child lists are cached to avoid quadratic lookup in large configurations.
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
On 2026-10-15, before pvAccess support was added, Qt 6.11.2 built on macOS arm64
and the core, UI, IOC, and helper suites passed,
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
subscriptions are cancelled before model destruction. Reconfiguring Force PVs
also releases obsolete numeric CA channels and their callbacks immediately.
Connections explicitly prepared for output writes remain available, and other
owners monitoring the same PV are unaffected.

## QtALH personal notification extension

QtALH provides built-in personal sendmail/email and generic JSON webhook
subscriptions with delayed escalation stages, grouped summaries, cooldowns, and
optional resolution messages. This is a QtALH extension, separate from legacy
ALH command hooks and database IPC. It adds no ALH configuration directives or
Channel Access writes. Each runtime starts paused and operates independently;
there is no shared notification owner or server. See the
[notification guide](site/operate/notifications.md).

## QtALH session analytics extension

Session analytics are an in-memory QtALH extension to runtime operation. They
observe processed alarm state without changing legacy configuration, command
hooks, logging, acknowledgement, shelving, or output PV behavior. Analytics and
notifications use independent engine observers. No external analytics service,
historical log import, or database dependency is added. See the
[analytics guide](site/operate/analytics.md).

## Selection, silence, and historical-order follow-up

Display-filter changes preserve visible action targets and clear targets removed
by filtering, including subsequent alarm updates. Selection dialogs close and
selection-dependent actions disable when their target disappears. Timed silence
ends when a different interval is selected; reselecting the current interval
does not restart the countdown. Silence controls, interval changes, timed expiry,
and automatic current-silence resets write operation-log records.

Historical searches validate both circular-log checkpoint slots against the full
file and use the newest matching slot to preserve insertion order for timestamp
ties. Filtering and result limits follow that circular order. Missing, damaged,
or stale checkpoints fall back to physical order for ties. Validation streams
bounded lines with cancellation checks. Regression tests cover plain/XML logs,
result limits, filtering, and stale/corrupt checkpoint slots, as well as selection
and silence workflows in both appearances.


## Acknowledgement ordering and log recovery follow-up

Group acknowledgement processes direct channels before subgroups, preserving
ACKPV write order across configuration save/reload. Repeated monitor requests
for the same owner and PV reuse the subscription, including when a constant
startup Force PV adds a channel before the normal startup traversal reaches it.
Other owners monitoring the same PV retain independent subscriptions.

Log readers locate checkpoints beside the canonical log path, matching the writer
when a file symlink is used. Live recovery checks both checkpoint slots against
the file and uses the newest matching slot. Subsequent incremental delivery uses
the sequence of that matched slot, rather than a newer stale checkpoint.
Regression coverage includes shared ACKPV ordering, real IOC callback counts,
plain/XML log aliases, both recovery slots, and updates after recovery.


## Combined masks, timed NoAck, and editor filters

Combined mask changes retain the legacy Enable/NoAck side effects before applying
ACKT and NoLog. Enabling a disabled channel with a cleared global transient sends
ACKS and ACKPV even when the same request sets NoAck; failed acknowledgements keep
the latch and passive mode sends no writes. Cached alarms exposed by Enable are
logged using the prior ACKT/NoLog settings.

Timed NoAck processes direct channels before subgroups on application, reset,
and expiration. Independent descendant timers still protect their channels from
an ancestor reset. Shared ACKPV write order remains stable across save/reload.
The editor ignores runtime display filters, and Paste/Insert reject a missing
destination selection instead of dereferencing it.

## Included hierarchy and Add/Disable compatibility

Inside INCLUDE files, repeated `GROUP NULL` declarations attach to the current
group, including after channels and nested includes. Regression coverage checks
the resulting hierarchy, save/reload, inherited beep thresholds and Force PV scope.
As in ALH, each included file starts with no current group/channel. Group and
channel directives before its first GROUP, CHANNEL or INCLUDE are never applied
to the INCLUDE's parent group. ALH logs and ignores them; QtALH rejects the
configuration, as it does for the same lines at the start of a top-level file.
`$BEEPSEVERITY` and `$HEARTBEATPV` are facility-wide and may appear there.

A channel can retain an alarm through `D → CD → C`. Changing that channel from
`C` to `D` now applies Add before Disable, exposing the cached alarm with the
previous settings before suppressing it again. Alarm/history records, channel
and group severity commands, and the channel's severity output are preserved.
Core regressions cover local/global, passive/active, channel/group, automatic/manual,
and NoLog combinations, with balanced aggregates and no fresh observation inferred
from the cached state.

## Timer ordering and group severity outputs

When several count-filter or timed NoAck deadlines expire before a refresh, they
run in deadline order. This preserves intermediate group severity commands and
output values. An earlier callback can cancel a later pending timeout. Alarm
records retain the original event timestamps; shelving retains its separate
absolute wall-clock expiration.

Channel Access connection deadlines use monotonic time for startup, Add after
Cancel, and numeric Force PV inputs. System-clock adjustments cannot delay the
missing-PV ERROR latch or cause these intervals to expire early.

Cancel and Disable update group severity and execute group severity commands
without writing group SEVRPVs, matching ALH. Disabled channels still publish -1
where required. Add/Enable of cached active alarms and subsequent monitor
transitions continue to publish channel and group severity outputs.
Unlike ALH, enabling a channel at NO_ALARM in global active mode also writes 0
to its SEVRPV. ALH writes nothing then, so the output keeps its previous value
(-1 after Disable) until the next severity change.


## Guidance terminators, filter integers, and logging compatibility

Inline guidance uses ALH's `$END` prefix recognition, including trailing comments.
Like ALH, any other line whose first character after spaces and tabs is `$`
ends the block. ALH reports it as an invalid end; Qt rejects the configuration
rather than treating subsequent channel declarations as guidance text. Phoebus
import writes guidance lines that begin with `$` as `\$`.
Count-filter integers retain ALH's decimal, octal, and hexadecimal interpretation
and are normalized to decimal when saved; range validation remains in effect.

Stop Logging suppression, broadcast delivery locks, and master-lock polling use
monotonic deadlines. Wall-clock corrections do not extend or shorten these
intervals; log timestamps and broadcast IDs continue to use wall time.
XML printer/database messages include the legacy `<date>` and `<time>` fields,
matching the timestamp representation in the log file.


## Repeated PV startup settings and operation labels

With `-global -caputackt`, repeated PVs take the final configured ACKT value in
ALH's startup traversal: recursively visit subgroups before each group's direct
channels, retaining order within each list. This includes cancelled channels and
is independent of interleaved declarations and save/reload ordering. Qt writes
that final value once per PV when writable; later IOC/operator settings are not
replaced by another duplicate subscription. Conflicting settings remain accepted
with this legacy precedence.

Operation records use the root group's ALIAS when present, falling back to its
name. The label is refreshed when reloading, including when an alias is removed.
Channel/group identifiers within the record and the database facility identifier
remain the underlying names. Text/XML file and database queue regressions cover
these distinct fields, and UI regressions verify alias changes through reload.


## Communication errors and acknowledgement submission

Qt corrects two inherited ALH bugs in global mode. Connection/read-access/write-access
failures latch a local ERROR acknowledgement instead of replacing outstanding IOC
ACKS with the synthetic event's zero. The local latch keeps ERROR audible and visible
under the unacknowledged filter. Acknowledging it performs no IOC or ACKPV write;
outstanding IOC acknowledgements remain pending. The latch survives recovery when
transient acknowledgement is enabled and clears on recovery with NoAckT. Cancel,
Disable, NoAck, shelving, and passive acknowledgement restrictions still apply.
Notification escalation remains paused during the communication gap.

Asynchronous Channel Access exceptions (for example an IOC rejecting a put with
`DISP=1`, or a lost virtual circuit) go to the message area and operation log, as
ALH's `alCaException` does, limited to 25 reports and one "Too many exceptions"
notice per CA context. Unlike ALH, which then restores the CA default handler,
QtALH keeps its handler and only prints later exceptions, so an error-severity
exception (for example `ECA_BADCHID` from a CA gateway during an IOC outage) never
aborts the process.

ACKS writes that cannot be sent (disconnected, not writable, or refused by the CA
client library) emit a failure operation rather than acknowledgement-success records
or ACKPV writes. Group acknowledgement continues with other channels. A sent write is
not confirmed by the IOC before these side effects. If the IOC rejects it (for example
`DISP=1` or an access-security change), ACKPV and the success records still occur, as
in ALH, and the rejection is reported as a Channel Access exception in the
message area and operation log (and a popup unless `-noerrorpopup`). The alarm stays
unacknowledged until the IOC monitor reports ACKS cleared.

If ACKS is submitted but the separate ACKPV submission is rejected, QtALH now
reports partial submission and keeps the failed ACKPV action in Runtime
Overrides. Explicit **Retry ACKPV** submits only that captured PV/value, never
ACKS. Repeated failures aggregate as a count per channel; each retry submits
one action and decrements only after acceptance. There is no automatic replay
on reconnection. Accepted asynchronous writes with an uncertain IOC outcome
are never offered for retry. Cancel/reload discard these runtime actions, and
passive mode prevents retry. This also covers automatic transient acknowledgement.

Read-access loss, disconnection, and Cancel invalidate cached CA alarm events. Recovery
waits for fresh monitored data, avoiding replay of stale alarms and their commands or
severity outputs. Write-only access loss retains readable, current observations.

A failed CA alarm monitor callback also invalidates the observation. ALH only prints
"alCaNewAlarmEvent failed" and keeps the previous alarm state. QtALH reports the CA
error in the message area and raises `<READ,ERROR>`, or the more specific
NOT_CONNECTED, NO_READ_ACCESS or NO_WRITE_ACCESS status when that loss is already
known. Like other communication errors, this writes an alarm-log record, runs
`$SEVRCOMMAND` ERROR commands, latches the local ERROR acknowledgement in global mode
and writes `$SEVRPV` 4 in global active mode. It also runs a `$STATCOMMAND READ`
command, because `READ` is the same status name as an IOC record READ alarm; such a
record alarm has a severity no higher than INVALID.

Repeated `GROUP NULL` declarations attach to the current group in both top-level and
included configuration files, matching ALH. Saves normalize these parent references.
The Force PV `-999` initialization sentinel is intentionally retained: a first or
reinitialized sample of `-999` is treated as unchanged, including when configured as
the force value.


## Filtered recovery and duplicate CALC inputs

When a channel is at NO_ALARM with outstanding ACKS, count filtering still
suppresses unregistered transient alarms as in ALH. Fresh normal samples now
refresh the value and restore observation coverage after a short filtered
communication outage, without adding alarm/history records, acknowledgements,
commands, or severity writes. Analytics therefore ends the gap on recovery.

Force PV CALC callbacks skip unchanged input values, matching ALH. Stateful
expressions no longer advance on duplicate or alarm-only input callbacks. An
unavailable input still blocks calculation, and its first valid sample after
recovery is processed even when the value matches the one before the outage.


## Force PV edits, queue recovery, and reload identity

Unchanged Force PV Apply is a no-op, preserving live calculation variables,
subscriptions and pending automatic ACKT writes. Mask/force/reset and enabled
settings reuse the last valid result without advancing stateful expressions.
Changed inputs replace only their own subscriptions; unchanged inputs and CALC
variables survive. Changed expressions evaluate once when all required inputs
are available. Edits made during an input outage wait for valid data. This
avoids both the former Qt variable reset and ALH's gratuitous evaluation on
unchanged Apply; the existing -999 value sentinel remains unchanged.

Printer/database delivery tracks lost original records separately by destination
and facility identity. A compact legacy code-4 summary (printer code 5) reports
the exact count and first/last failure times as millisecond Unix epochs. The
ordinary two-second timer retries pending summaries even without new alarms.
Failed summary attempts neither increase the loss count nor block smaller
ordinary records. Queue writes remain nonblocking and limited to the legacy
receive-buffer size. Diagnostic header tokens alone may be shortened with a
trailing `~` to keep a summary bounded; ordinary record identifiers are intact.

Successful reloads update both the database facility identifier (root name) and
operator label (alias or name). Failed reloads retain both. Files, master locks,
broadcast identity and pending loss intervals survive metadata updates; older
loss summaries keep their original facility attribution. This corrects ALH's
one-time database application-name initialization.

## Shared ACKT requests and stateful CALC inputs

Deferred automatic ACKT writes belong to the target PV, independently of the
number of display rows monitoring it. A newer eligible mask request through any
row supersedes the pending request, including a no-op or a failed manual request.
Manual failures still require explicit retry. Pending writes retain their force
source and request order, so editing/disabling an older source cannot cancel a
newer source's request, and group reset keeps its direct-channel-before-subgroup
precedence. Successful writes are removed; other monitored rows update through
IOC confirmations. Passive restrictions and startup ACKT precedence are unchanged.

Accepted ACKT requests and the latest record-wide IOC observation are also
retained while duplicate rows catch up. A rapid opposite request through a
duplicate row is compared with that record state, rather than the row's stale
mask, so the latest accepted request wins. Other rows retain their displayed
feedback until their monitors confirm the setting; failed manual submissions
still need an explicit retry. Only a request that names NoAckT is compared
this way: Modify Mask NoAckT, a changed T box in Force Mask, or a Force PV.
Requests for other bits (Add, Disable, NoAck, NoLog, the NoAck timer, Reset
and Restore Selected) keep each row's own T bit and write ACKT only when that
row's bit changes, as ALH's alChangeChanMask does, so a cancelled or not yet
updated duplicate row cannot write its stale NoAckT. An accepted request yields
to a later different value from a row that has already confirmed it, such as
a change by another client or an IOC reboot.

For QtALH 1.0.0, local NoAckT preserves ALH's existing latch behavior: enabling
it after a transient has already cleared does not erase that unacknowledged
event. It stays unacknowledged until the operator acknowledges it or a later
alarm transition replaces it: with NoAckT set, each transition sets the
unacknowledged severity to the new severity, so a later recovery clears it.

Force CALC retains received input values separately from its mutable calculation
variables. Assignments to A through F can no longer make an unchanged callback
advance the expression or make a genuine input change look unchanged. Only the
changed/recovered input slot is replaced; other calculation variables survive
callbacks and expression edits. Unavailable inputs block evaluation and cached
result replay, and the first valid sample after recovery is processed even when
its value matches the previous observation. Numeric Force subscriptions use
DBE_VALUE; alarm-only changes do not advance calculations. This fixes ALH's shared
input/workspace comparison bug while preserving Qt's existing edit, recovery,
rounded comparison, and NE-reset behavior, including the retained -999 sentinel.

## Broadcast recovery and multiline alarm records

Broadcast readers wait for the legacy message, date and sender lines before
remembering a message ID. A partial file can therefore be completed and retried
without losing its reload or stop-logging action. Failed `.MESSLOCK` opens report
the path and system error, then retry on the service timer and on Send. Recovery
uses the existing shared descriptor registry and preserves delivery ownership.

New log records escape embedded LF/CR as `\n`/`\r` in text and `&#10;`/`&#13;` in
XML, including the records sent to printer/database queues. Ordinary single-line
records keep their existing format. XML alarm records also escape `&`, `<`, `>`
and `"` in the PV name, description and value as `&amp;`, `&lt;`, `&gt;` and
`&quot;`; ALH inserts these strings unchanged, so a value `a"b` is logged as
`a&quot;b`. XML operation records also escape `&`, `<`, `>` and `"` in the
facility label, node name and message text, including the copies sent to the
database queue; ALH writes that text unchanged, so a root ALIAS `RF & vacuum`
appears as `RF &amp; vacuum`. Text-format records are not entity-escaped.
On bounded-log recovery, older multiline records are joined at recognizable
timestamp/entry headers before checkpoint
validation and retention. Surviving records are rewritten with escaped line
breaks. A matching original checkpoint still determines order, including equal
timestamps. Without one, the existing timestamp fallback applies. Legacy text
has no unambiguous framing when a continuation itself looks like a record header.


## Replaced log paths and legacy timestamp recovery

Selecting an alarm-log path opens the current file before sharing a writer by
native file identity. After a log is renamed and replaced, selecting the same
path writes to the replacement; ordinary aliases still share their live writer.
Circular-log recovery uses the same timestamp parser as the browsers, including
legacy ctime dates with space-padded days. Without a matching checkpoint, these
logs retain their newest records in circular order instead of physical file order.


## Renamed logs and independent checkpoints

Live recovery scans a duplicate of the open log handle, including asynchronous
scans, so replacing the pathname before the first alarm or during recovery does
not redirect or discard records. Content-change checks use the original handle.
Explicitly selecting a replacement still switches the writer to that file.

Checkpoint names include the log's native identity. A versioned, identity-bound
locator on the log preserves the original checkpoint location across renames;
a replacement at the old pathname gets a separate checkpoint. The live writer
registry supplies the locator when filesystem attributes are unavailable, while
persistent lookup across processes requires xattrs or NTFS streams. Legacy
unsuffixed checkpoints remain readable and are validated before migration.
Regression coverage includes rotation before the first alarm, rotation during a
large background scan, different retention limits through a renamed alias, and
live/search ordering of equal-timestamp records with two active replacement logs.


## Recovery retries, runtime errors, and clipboard names

A failed pre-write alarm recovery retains the pending FIFO entry and retries a
fresh snapshot. Write/checkpoint failures are distinguished from recovery errors
so a record already committed is not replayed. Shutdown retries synchronously;
persistent recovery failures preserve the temporary spool with a diagnostic
identifying its path, unread offset, and destination. Regressions change a large
log while it is scanned and verify ordered, single delivery both in the event
loop and at window teardown.

Window errors are audited to the operation log, matching ALH's `errMsg` path,
including with `-noerrorpopup`. A recursion guard prevents operation-log failures
from auditing themselves indefinitely. Tests cover enabled/disabled logging and
an unwritable operation-log device.

Clipboard serialization chooses a wrapper group name absent from the selected
subtree and validates the payload before changing the clipboard or removing a
Cut selection. Cross-window Cut/Paste and undo/redo cover groups named
`__QTALH_CLIPBOARD__` and `__QTALH_CLIPBOARD__1`.

## pvAccess channels (2026-10-15)

QtALH extension; Motif ALH supports only Channel Access. PV names starting with
`pva://` route to a pvAccess client (`services/pv_access.cc`) behind the same
`PvService` interface as Channel Access, so the engine, configuration format,
and unprefixed names are unchanged. pvAccess callbacks are queued to the Qt
thread, matching the non-preemptive CA context. PVA support is part of every
build and requires EPICS 7 Base with pvAccess and pvData.

Alarm status comes from the condition QSRV places in `alarm.message`, and
values follow `display.precision`, as `DBR_STSACK_STRING` reports them through
CA. When the record has a non-empty `AMSG`, QSRV sends that text instead of the
condition name (`pva2pva` `mapStatus`), so QtALH falls back on the PVA alarm
status: `DRIVER` (which QSRV also uses for `UDF` and `TIMEOUT`) gives `COMM`,
`UNDEFINED` gives `UDF`, and others give `SOFT`. Displayed and logged status
and `$STATCOMMAND` then differ from CA; for example, `LINK` with `AMSG`
`field INP` becomes `SOFT`. Missing PVs report `NOT_CONNECTED` after one
second, as with CA. Force
inputs ignore updates that the server marks as alarm-only and updates that
repeat the previous value. QSRV marks the value as changed on every alarm
event, however. With a record deadband (MDEL > 0), an alarm transition can
therefore deliver a value change that a CA `DBE_VALUE` subscription would not.
That value can apply or reset a force mask, or advance a stateful CALC. Set
MDEL to 0 when a `pva://` force input must behave like CA.

QSRV rejects writes to ACKS/ACKT (`dbPut Attempt to modify noMod field`,
verified with softIocPVA from Base 7.0), and PVA alarms carry no acknowledgement
fields. Global acknowledgement of a `pva://` channel is therefore kept in the
running instance by applying `recGblResetAlarms`, `putAcks`, and `putAckt`
rules. It is not shared with other instances or with CA clients of the record.
PVA has no access-rights events, so the synthetic read/write-access
states are not produced for these channels. Writes continue while the server
rejects them; a repeated identical failure is reported once per outage, ending
at the next successful write or reconnect.
`test-ioc` covers monitoring, enumerated values, writes, missing PVs, IOC restart, runtime
acknowledgement with ACKT, descriptions, mixed CA/PVA field reads, Force PVs,
and passive mode against softIocPVA.

## Observed differences from legacy runs (2026-10-17)

Side-by-side runs of legacy ALH and QtALH against the same private IOC and
configuration showed these further differences. Local-mode alarm logs were
byte-for-byte identical.

- **Startup operation-log records.** ALH writes `Setup Alarm Log File : <file>`
  and `Setup Config File : <file>` at startup, without the facility and node
  prefix and with the path as given. QtALH writes only
  `<facility>: :  Setup Config File : <absolute path>`; it writes
  `Setup Alarm Log File : <file>` only when the file is changed with
  **Setup → New Alarm Log File…**.
- **Global-mode alarm-log records.** With `-global`, ALH writes a startup record
  for each initially cancelled or disabled channel (for example
  `<pv> NO_ALARM NO_ALARM NO_ALARM noackT` with an empty value). For a channel
  with a count filter, ALH also writes a record with the previous status and
  severity and the new value (for example `<pv> HIHI MAJOR MAJOR ackT 0`) when
  the value clears inside the filter interval. QtALH writes neither record; all
  other global-mode records, including the XML `<status-noack>` and
  `<severity-noack>` fields, matched.
- **Classic footer legend.** The legend line reads
  `Channel Alarm Data: <Status,Severity> (<Unack Severity>)` in QtALH and
  `Channel Alarm Data:  <Status,Severity>,<Unack Severity>` in ALH. Channel rows
  use ALH's `<status,severity>,<unack severity>` format in both.
- **Phoebus round trip.** Export keeps trailing spaces on the last guidance line
  inside `<details>`, but import trims each element's text, so a second export of
  the round-tripped file differs (for example `alarms.    </details>` becomes
  `alarms.</details>`). No conversion note reports this; see
  [Phoebus XML import & export](site/configure/phoebus.md).
