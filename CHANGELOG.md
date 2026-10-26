# QtALH release notes

`qtalh --version` and Help → About report the QtALH version (`QTALH_VERSION`
in `qtalh/core/version.h`) and the Motif ALH release it is based on. Each
release is tagged `v<version>`.

## 1.1.0 (unreleased)

Changes since 1.0.0:

- Optional binary SDDS alarm snapshots and analytics table/chart exports.
  Compatible SDDS headers and libraries are detected at build time; normal
  operation does not require SDDS. Embedded Help includes the snapshot workflow.
- The configuration tool's Save preserves comments, blank lines, formatting and
  INCLUDE lines, rewriting only changed lines. Edited INCLUDE files are listed
  for confirmation before saving; View → Configuration File shows source text
  with runtime changes in place. Renaming groups updates INCLUDE parents, and
  saves reject conflicting copies of one included file reached through aliases.
  A save replaces none of its files when one of them cannot be written.
- Force PV calculations support twelve inputs (A–L), shared PV inputs, complete
  numeric constants and expressions containing spaces. Inputs G–L warn about
  legacy ALH compatibility. Late numeric subscribers receive the current cached
  sample, and cancelled inputs discard queued replays.
- Notification destinations and subscriptions can be declared in `.alhConfig`
  with `$NOTIFY_EMAIL`, `$NOTIFY_WEBHOOK` and `$NOTIFY`. `-notifications` enables
  them at startup; the compact window also shows the runtime's notification
  state. Email bodies group channels by hierarchy with readable alarm names.
  Invalid PV wildcard ranges, and webhook URLs that deliveries would refuse
  (HTTP to a host other than loopback, credentials or a fragment), are rejected
  with a diagnostic.
- Editor Clear, Cut, Copy and Paste preserve notification references and carry
  required destinations between windows, rejecting unresolved references and
  conflicting definitions.
- Shelving is hidden unless `USE_SHELVING=1`. When enabled, shelves can end at a
  selected date and time, and name backgrounds mark shelved channels and groups.
  Shelf deadlines reject timestamp arithmetic overflow.
- Phoebus File menu commands are hidden unless `QTALH_PHOEBUS_MENU` is set to a
  value other than empty or `0`; command-line conversions remain available.
- Runtime names mark changed settings with a pencil and explanatory tooltip.
  Bars below either alarm pane count alarms scrolled out of view and can reveal
  the nearest one. They leave room for the final row and update after font changes.
  Fixed a crash when clicking an alarm-pane arrow to reveal an alarm outside the view.
- Silence colors the status area and, unless `-maskcolor` is set, the alarm panes
  blue. Persistent footer indicators identify Silence Forever, a nondefault
  facility beep threshold, a display filter, and disabled Force PVs.
- The compact Alarm Handler indicator uses ALH's neutral blue-gray between
  flashes, so white INVALID/ERROR alarms remain visible in the modern appearance.
  The Alarm legend disclosure keeps readable text when expanded or hovered on
  the blue silence background.
- Selection, middle-click copy/drag, guidance and command previews, tree expansion,
  dialog layouts and default font size have been updated from operator feedback.
  Configuration-disabled or cancelled channels are excluded from analytics.
- **Action → NoAck for One Hour…** (Ctrl+N) opens ALH's Modify NoAck Mask dialog
  instead of toggling NoAck at once. **Setup → New Alarm Log File…** and
  **New Operation Modification Log File…** were removed; choose log files with
  `-a` and `-o` at startup.
- Editor **Insert → Group…** and **Insert → Channel…** place the new node right
  after the selection, or first in a group selected in the tree, instead of at
  the end of the group.
- Current Alarm History keeps 100 entries and sorts by any column. Live log
  views show up to 10,000 records or 2.5 MiB. Audible alarms repeat every two
  seconds, and a custom sound that is still playing is not restarted. Alarm
  Analytics opens on unsuppressed activity.
- On Linux with Qt 5, repeated custom alarm sounds play their full duration.
  Reloading the clip avoids GStreamer truncating short Ogg sounds after the
  first beep, matching ALH's playback of the same sound file.
- A missing configuration file is named on standard error, and the file chooser
  preselects a file whose name differs only in case. `-aCM`, `-oCM` and Xt
  resource switches report specific errors.
- Legacy extra tokens are ignored on `GROUP`, `CHANNEL`, `INCLUDE`,
  `$HEARTBEATPV` and `$ALARMCOUNTFILTER` lines. Groups may repeat ancestor names;
  edits or saves that would reload the hierarchy differently are refused with
  the group to rename. Documentation now explains these rules and preserving saves.
- Paths attached to `-a`, `-o`, `-f`, `-l` or `-p` are accepted when they start
  with `/` or `.`, for example `-p/usr/share/sounds/freedesktop/stereo/bell.oga`.
- On Linux and macOS, startup raises the soft open-file limit to the hard limit
  and reports when usage reaches 80%; commands and notification mailers it
  starts retain the original limit.
- The local feature lab honors its selected client and IOC ports even when
  inherited `EPICS_CAS_*` server port settings differ.

QtALH 1.0.0 rejects a whole configuration file that uses `$NOTIFY_EMAIL`,
`$NOTIFY_WEBHOOK`, `$NOTIFY` or `$FORCEPV_CALC_G` through `$FORCEPV_CALC_L`.
Upgrade every QtALH that reads a shared configuration or INCLUDE file before
adding these lines to it.

Requirements remain EPICS 7 Base with pvAccess/pvData and Qt 5.15 or newer or
Qt 6 with Widgets, Network, PrintSupport and Multimedia. The documentation
website requires Node.js 22+ with npm and Python 3.9 or newer.

## 1.0.0

First QtALH release: a C++17 Qt port of EPICS ALH 1.2.35 that reads the same
`.alhConfig` files and accepts the legacy command-line switches. See the
[compatibility inventory](docs/qtalh-compatibility.md) for the parity evidence
and the remaining differences.

Changes that operators and site maintainers should review before deployment:

- The modern appearance is the default. `-style motif` selects the classic
  Motif look; `-style fusion` or another Qt style is also accepted.
- PV names prefixed with `pva://` are monitored over pvAccess; all other names
  use Channel Access as in ALH.
- New View menu tools: Active Alarms, Find Alarm, Alarm Details, Runtime
  Overrides and the Alarm Summary Panel.
- Phoebus alarm XML import and export (File menu, `--to-phoebus`,
  `--from-phoebus`).
- Timed shelving of channels and groups with automatic re-enable. Shelving
  requires a username, and the operation log records it.
- Personal notification subscriptions with escalation, and session alarm
  analytics.
- Commands starting with `medm` launch `qtedm` from `PATH` instead.
- New `--help`, `--version` and `--validate` conveniences. Unknown switches
  and Xt resource overrides are errors.
- A directive before an INCLUDE file's first `GROUP` or `CHANNEL` is an error.
  ALH ignored it with "Logic error: glink is NULL"; `$BEEPSEVERITY` and
  `$HEARTBEATPV` are still accepted there.
- Configuration files that are not valid in the locale encoding are read as
  ISO-8859-1 and saved without changing their non-ASCII text.
- As in ALH, runtime File → Open replaces the monitored configuration, and with
  `-B` SIGINT (Ctrl-C) exits without confirmation once this instance's
  broadcast has been delivered.
- The build ignores a `qmake6` found only through `PATH` and refuses Qt
  modules mixed from different installations.

Additional 1.0.0 audit decisions and corrections:

- Phoebus XML remains an interchange format with documented conversion limits.
  Keep the original `.alhConfig` as the authoritative configuration.
- Phoebus imports keep every line within legacy ALH's 498-byte line limit:
  long descriptions are truncated, long commands and display URLs are kept
  as guidance text, and long guidance lines are wrapped, each with a note.
  Display URLs containing shell quoting characters are also kept as guidance
  text, because ALH opens guidance URLs through the shell. An import fails if
  a group or channel line would still be too long, if an XInclude target is
  not a regular file, or if a file cannot be read completely.
- Shared Channel Access wakeups remain active when closing one of several windows.
- A rejected ACKPV submission after ACKS is submitted now reports partial
  submission. Runtime Overrides offers explicit **Retry ACKPV**, without
  repeating ACKS or retrying automatically when the connection recovers.
- Rapid opposite NoAckT requests through duplicate rows use the latest accepted
  record setting, so a stale row cannot discard the newer request. Only a
  request that changes NoAckT (Modify Mask NoAckT, the T box in Force Mask, or a
  Force PV) does this; Add, Disable, NoAck, NoLog, the NoAck timer and Reset
  never write a stale row's NoAckT to ACKT. A later change confirmed by a row,
  such as another client or an IOC reboot, takes precedence over an earlier
  accepted request.
- FORCEPV rejects NaN, infinity and numeric overflow in force/reset values.
  Finite values, legacy parsing defaults and `NE` remain supported.
- Enabling local NoAckT leaves an existing cleared transient awaiting
  acknowledgement until it is acknowledged or a later alarm transition, which
  uses the new setting, replaces it.
- A failed Channel Access alarm monitor update raises `READ`/`ERROR`, or the
  more specific connection/access status when that loss is known, until fresh
  data arrives. Like a disconnection, it writes an alarm-log record, runs
  `ERROR` `$SEVRCOMMAND` commands and any `$STATCOMMAND READ` command, and in
  global active mode writes `$SEVRPV` 4. ALH reported the error but kept the
  previous alarm state.
- Send Message rejects reserved reload/stop-logging control prefixes,
  including the looser forms legacy ALH receivers act on. Explicit controls and
  the legacy broadcast wire format remain supported. Broadcast text is sent as
  one line: pasted line breaks and other control characters become spaces.
- Unavailable `pva://` outputs are reported once per outage, as Channel Access
  outputs are. A `$SEVRPV` written before its output first connects after a
  load is written on connection and reported only if the output does not
  connect within one second or connects without write access.
- As in ALH, a NUL byte in a configuration file ends only its own line.
- With `-T`, or `-L` with an explicit `-m`, an existing alarm log holding more
  records than the limit is rewritten at the first alarm to keep only its
  newest records (ALH appends). Copy the file first, or use `-m 0`, when
  switching from ALH.
- With `-xml`, operation records escape `&`, `<`, `>` and `"` in facility
  labels, names and messages, in the operation log and in database queue
  records. ALH wrote this text unchanged; text-format logs are unchanged.

Requirements: EPICS 7 Base with its pvAccess and pvData libraries, and Qt 5.15
or newer or Qt 6 with Widgets, Network, PrintSupport and Multimedia.
