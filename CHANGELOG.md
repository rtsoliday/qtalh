# QtALH release notes

`qtalh --version` and Help → About report the QtALH version (`QTALH_VERSION`
in `qtalh/core/version.h`) and the Motif ALH release it is based on. Each
release is tagged `v<version>`.

## 1.0.0 (unreleased)

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
- Phoebus alarm XML import and export (`--to-phoebus`, `--from-phoebus`).
  The File menu commands are hidden unless `QTALH_PHOEBUS_MENU` is set.
- Timed shelving of channels and groups with automatic re-enable. Shelving
  requires a username, and the operation log records it.
- Personal notification subscriptions with escalation, and session alarm
  analytics.
- Commands starting with `medm` launch `qtedm` from `PATH` instead.
- New `--help`, `--version` and `--validate` conveniences. Unknown switches
  and Xt resource overrides are errors. A path attached to `-a`, `-o`, `-f`,
  `-l` or `-p` (`-p/usr/share/sounds/freedesktop/stereo/bell.oga`) is
  accepted; ALH exits on that form.
- As in ALH, extra tokens are ignored on `GROUP`, `CHANNEL`, `INCLUDE`,
  `$HEARTBEATPV` and `$ALARMCOUNTFILTER` lines (for example
  `$ALARMCOUNTFILTER -1 3 -----`), and a group may repeat an ancestor's name.
  Saves and editor changes that would reload such a hierarchy differently are
  refused with the group to rename.
- Timed silence (Silence N minutes) turns the status area blue, and the alarm
  panes too unless `-maskcolor` is given, as in ALH.
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
