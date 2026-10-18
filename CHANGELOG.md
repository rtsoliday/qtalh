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
- The build ignores a `qmake6` found only through `PATH`, refuses Qt from an
  active Conda environment unless `QTALH_ALLOW_CONDA=1`, and refuses Qt
  modules mixed from different installations.

Additional 1.0.0 audit decisions and corrections:

- Phoebus XML remains an interchange format with documented conversion limits.
  Keep the original `.alhConfig` as the authoritative configuration.
- Phoebus imports keep every line within legacy ALH's 498-byte line limit:
  long descriptions are truncated, long commands and display URLs are kept
  as guidance text, and long guidance lines are wrapped, each with a note.
- Shared Channel Access wakeups remain active when closing one of several windows.
- A rejected ACKPV submission after ACKS is submitted now reports partial
  submission. Runtime Overrides offers explicit **Retry ACKPV**, without
  repeating ACKS or retrying automatically when the connection recovers.
- Rapid opposite NoAckT requests through duplicate rows use the latest accepted
  record setting, so a stale row cannot discard the newer request. Only a
  request that changes NoAckT (Modify Mask NoAckT, the T box in Force Mask, or a
  Force PV) does this; Add, Disable, NoAck, NoLog, the NoAck timer and Reset
  never write a stale row's NoAckT to ACKT.
- FORCEPV rejects NaN, infinity and numeric overflow in force/reset values.
  Finite values, legacy parsing defaults and `NE` remain supported.
- Enabling local NoAckT leaves an existing cleared transient awaiting
  acknowledgement until it is acknowledged or a later alarm transition, which
  uses the new setting, replaces it.
- Failed CA alarm reads invalidate the observation until fresh data arrives.
  Known connection/access errors retain their more specific status.
- Send Message rejects reserved reload/stop-logging control prefixes. Explicit
  controls and the legacy broadcast wire format remain supported.
- With `-xml`, operation records escape `&`, `<`, `>` and `"` in facility
  labels, names and messages, in the operation log and in database queue
  records. ALH wrote this text unchanged; text-format logs are unchanged.

Requirements: EPICS 7 Base with its pvAccess and pvData libraries, and Qt 5.15
or newer or Qt 6 with Widgets, Network, PrintSupport and Multimedia.
