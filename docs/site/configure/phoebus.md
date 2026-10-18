# Phoebus XML import & export

QtALH reads and writes the alarm configuration XML used by the Phoebus alarm
server and NALMS, so a facility can move between systems or run both.
For QtALH 1.0.0, these conversions preserve the settings each format supports.
Keep the original `.alhConfig` as the authoritative configuration: exporting
to XML and importing it again can lose ALH-only settings, including Force PVs,
beep thresholds, and group aliases. Review the conversion notes before using
the converted configuration.

## In the application

- **File → Export Phoebus XML…** writes the current configuration (configured
  masks, not runtime changes).
- **File → Import Phoebus XML…** converts a Phoebus file and opens it in the
  configuration tool. Review it, then **Save As** an `.alhConfig` file.
  Imported commands become related processes: clicking **P** during review
  runs them, so hover over **P** to preview them instead.

When some settings have no equivalent, a message lists each kind with example
paths. A few changes are not reported:

- Cancel exports as `<enabled>false</enabled>`, which imports as Disable, so a
  Cancel mask returns as Disable after a round trip.
- Elements QtALH does not recognize, including misspelled ones, are ignored on
  import together with their contents.
- Characters that XML 1.0 cannot hold, such as form feeds and other control
  characters, are left out on export.
- Leading and trailing white space in each imported element's text is removed.

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
Include targets must be regular files; symbolic links to regular files are
accepted. Devices, FIFOs, and directories are rejected before opening them.
A read error in the XML file or any XInclude rejects the import, even if the
bytes read so far form a complete XML element.
Unresolved or unsupported selectors fail the import. Commands containing ALH
menu delimiters (`!`) or line breaks are retained as guidance text with a
conversion note. Guidance lines whose first non-blank character is `$` are
written as `\$`, because ALH ends a guidance block at any such line and reads the
rest as configuration. Delay and count must be nonnegative integers; counts are limited
to ALH's maximum of 1,000,000.

Command text is copied unchanged, but the two systems run it differently.
Phoebus starts the program directly, without a shell: it replaces `$(macro)`
references from its `macros` preference, splits the text at spaces (honoring
double quotes only), and runs it in its `command_directory`. QtALH, like ALH,
runs the text with `/bin/sh -c` (`cmd.exe` on Windows) in its own working
directory, so shell characters such as `>`, `;`, `|`, `'` and `$(...)` take
effect. Conversions do not flag such commands; review commands that contain
shell characters, `$(...)` macros, or relative paths after import or export.

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
