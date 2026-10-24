# Silence & display filters

Change what you hear or see without changing which channels QtALH monitors.

## Silence alarm audio

Use **Setup** to choose a silence interval or **Silence Forever**. The available intervals are 5, 10, 15, 30, and 60 minutes; the default interval is 30 minutes. Starting with `-s` enables Silence Forever, which can be changed in the running application.

The right side of the status area groups the timed-silence and current-alarm
checkboxes with **Silence Forever** status and **ALH Beep Severity**. All four
lines align to the right, as they do in Motif. Timed silence and **Silence Forever**
turn the footer blue; the alarm panes also turn blue unless `-maskcolor` is set.
**Silence Forever: On** has a bold amber background in every appearance.
See the [main-window screenshots](/operate/appearance).

Silence does not acknowledge an alarm, stop monitoring, or disable logging. Use acknowledgement separately after reviewing the alarm.

## Choose which severities can sound

Set the facility beep threshold in Setup. Node-specific thresholds are available through **Beep Severity** (**Ctrl+B**) and `$BEEPSEVR`; ancestor thresholds also apply. `$BEEPSEVERITY` sets the facility's configured threshold, defaulting to MINOR.

The footer's **ALH Beep Severity** indication has an amber background whenever the
facility threshold differs from MINOR. Audible alarms repeat once every two seconds;
a custom sound that is still playing is allowed to finish before another playback.

With no sound file, QtALH uses the system beep. `-p /path/to/alarm.ogg` selects a local audio file. Decoding depends on the installed Qt media backend. If no sound is heard, check silence state, outstanding acknowledgements, thresholds, file path, backend, and output device.

## Filter the displayed channels

Choose **Setup → Display Filter**:

| Filter | Rows shown |
| --- | --- |
| No filter | All rows in the selected hierarchy. |
| Active Alarms Only | Active or unacknowledged alarms. |
| Unacknowledged Alarms Only | Alarms with outstanding acknowledgement. |

The command-line equivalents are `-filter no`, `-filter active`, and `-filter unack`. Filtering changes display membership; it does not cancel subscriptions or suppress logging. A cleared transient may remain in either alarm filter until acknowledged.

An amber **Display Filter** indication remains visible in the footer while either
alarm-only filter is selected. Choosing **No filter** clears this indication.

## Temporarily remove acknowledgement requirements

Timed NoAck (**Action → NoAck for One Hour…**, Ctrl+N) is different from silence. Its Modify NoAck Mask dialog starts or ends the one-hour timer for the selection. It changes acknowledgement behavior and is shown as `H` in the mask summary. In global active mode, when the hour expires, an alarm that cleared meanwhile is acknowledged automatically: QtALH writes the IOC ACKS and any `$ACKPV`. Use the [mask reference](/reference/configuration) before changing NoAck, NoAckT, Disable, or Cancel settings.

## Temporarily shelve alarms

When enabled with `USE_SHELVING=1`, use [timed shelving](/operate/shelving) to exclude selected channels from active
counts, alarm lists, and sound while preserving normal monitoring, logging, and
acknowledgement state. Shelves require a reason and expire automatically; they
are separate from timed NoAck and silencing.
