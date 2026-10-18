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

Requirements: EPICS 7 Base with its pvAccess and pvData libraries, and Qt 5.15
or newer or Qt 6 with Widgets, Network, PrintSupport and Multimedia.
