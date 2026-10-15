# Phoebus XML import & export

QtALH reads and writes the alarm configuration XML used by the Phoebus alarm
server and NALMS, so a facility can move between systems or run both.

## In the application

- **File → Export Phoebus XML…** writes the current configuration (configured
  masks, not runtime changes).
- **File → Import Phoebus XML…** converts a Phoebus file and opens it in the
  configuration tool. Review it, then **Save As** an `.alhConfig` file.

When some settings have no equivalent, a message lists each kind with example
paths. Nothing else is changed.

## From the command line

```sh
qtalh --to-phoebus facility.xml facility.alhConfig
qtalh --from-phoebus facility.alhConfig facility.xml
```

The input is the usual configuration argument (an `.alhConfig` file, or a
Phoebus XML file for `--from-phoebus`). No display or PV connection is needed.
Conversion notes, such as settings without an equivalent, are reported on standard error.

## Mapping

| ALH | Phoebus |
| --- | --- |
| Root group | `<config name>` |
| `GROUP` | `<component>` |
| `CHANNEL` | `<pv>` |
| `$ALIAS` | `<description>` |
| Mask Cancel or Disable | `<enabled>false</enabled>` |
| Mask NoAckT (T) | `<latching>false</latching>` |
| `$ALARMCOUNTFILTER -1 s` | `<delay>s</delay>` |
| `$ALARMCOUNTFILTER n s` | `<delay>s</delay>` and `<count>n</count>` |
| `$GUIDANCE` text | `<guidance>` |
| `$GUIDANCE` URL | `<display>` |
| `$COMMAND` (single or `!label!command!` menu) | `<command>` entries |

Imported Phoebus displays that are web or file URLs become guidance URLs; other
displays (for example `.bob` files) are kept as guidance text. A disable-until
date imports as Disable; use [timed shelving](/operate/shelving) instead.
XInclude files are expanded, including shorthand ID selectors in `xpointer`.
Unresolved or unsupported selectors fail the import. Commands containing ALH
menu delimiters (`!`) or line breaks are retained as guidance text with a
conversion note. Delay and count must be nonnegative integers; counts are limited
to ALH's maximum of 1,000,000.

Count filters with a positive count use the same delay and count parameters,
but ALH also delays clearing; Phoebus delays only alarm onset. Conversions report
this difference for review.

Names with spaces are converted to underscores, and a component that repeats an ancestor's name is renamed, as ALH requires.

PV names are copied unchanged in both directions. A Phoebus `pva://` PV is
monitored over [pvAccess](/reference/configuration#pvaccess-channels), with
acknowledgement kept in the running QtALH instance.

Not converted: group aliases (group names are retained), NoAck (A) and NoLog (L)
masks, `$SEVRPV`, `$ACKPV`,
`$SEVRCOMMAND`, `$STATCOMMAND`, `$FORCEPV`, `$BEEPSEVR`, `$BEEPSEVERITY`,
`$HEARTBEATPV`, and `MASTER_ONLY` (exported); Phoebus enabling filters,
automated actions (use [notifications](/operate/notifications)) and annunciation
(imported).
