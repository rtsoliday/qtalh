# Silence & display filters

Change what you hear or see without changing which channels QtALH monitors.

## Silence alarm audio

Use **Setup** to choose a silence interval or **Silence Forever**. The available intervals are 5, 10, 15, 30, and 60 minutes; the default interval is 30 minutes. Starting with `-s` enables Silence Forever, which can be changed in the running application.

Silence does not acknowledge an alarm, stop monitoring, or disable logging. Use acknowledgement separately after reviewing the alarm.

## Choose which severities can sound

Set the facility beep threshold in Setup. Node-specific thresholds are available through **Beep Severity** (**Ctrl+B**) and `$BEEPSEVR`; ancestor thresholds also apply. `$BEEPSEVERITY` sets the facility's configured threshold, defaulting to MINOR.

With no sound file, QtALH uses the system beep. `-p /path/to/alarm.ogg` selects a local audio file. Decoding depends on the installed Qt media backend. If no sound is heard, check silence state, outstanding acknowledgements, thresholds, file path, backend, and output device.

## Filter the displayed channels

Choose **Setup → Display Filter**:

| Filter | Rows shown |
| --- | --- |
| No filter | All rows in the selected hierarchy. |
| Active Alarms Only | Active or unacknowledged alarms. |
| Unacknowledged Alarms Only | Alarms with outstanding acknowledgement. |

The command-line equivalents are `-filter no`, `-filter active`, and `-filter unack`. Filtering changes display membership; it does not cancel subscriptions or suppress logging. A cleared transient may remain in either alarm filter until acknowledged.

## Temporarily remove acknowledgement requirements

Timed NoAck (**Ctrl+N**) is different from silence. It changes acknowledgement behavior and is shown as `H` in the mask summary. Use the [mask reference](/reference/configuration) before changing NoAck, NoAckT, Disable, or Cancel settings.
