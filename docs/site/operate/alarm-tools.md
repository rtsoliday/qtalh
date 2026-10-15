# Active alarms, search & details

These runtime tools add views for finding and understanding alarms. They work
with every appearance, including `-style motif`, and do not change how the alarm
tree, acknowledgement or logging behave.

## Active Alarms

Choose **View → Active Alarms…** (**Ctrl+L**), or right-click the compact
facility window. The table lists every channel that is in alarm or still awaits
acknowledgement, highest severity first and then most recent change:

| Column | Meaning |
| --- | --- |
| Ack | Unacknowledged severity letter, in its alarm color |
| Severity | Current severity, in its alarm color |
| Changed | Local time of the latest severity or status change (tooltip: date) |
| Channel, Status, Value | Alias or PV name, EPICS status and value |
| Group | Path of the channel's group |

Click a heading to sort by that column. Type in the filter to match channel,
status, value or group text; check **Unacknowledged only** to hide acknowledged
alarms. Select rows and click **Acknowledge**, or use **Guidance**,
**Details…** and **Show in Tree** for one row. Double-click a row to select it
in the main window. Rows update as alarms change and keep your selection.
Shelved alarms are counted below the table but not listed; see
[timed shelving](/operate/shelving).

## Find Alarm

Choose **View → Find Alarm…** (**Ctrl+F**), available in the runtime and the
configuration tool. Type part of a group or channel name, alias, PV or path; the
search is case-insensitive and updates as you type. Use the arrow keys to choose
a match and press **Enter** (or double-click) to select it in the tree and
contents panes. A match hidden by the display filter is reported instead.

## Alarm Details

Select a group or channel and choose **View → Alarm Details…** (**Ctrl+D**).
The window follows the selection, like Properties, and updates every second:

- severity, status, value, unacknowledged severity and time of the last change;
- mask, beep severity and count filter, marked where they differ from the
  configuration, and the Force PV state;
- **Why**: a plain explanation, for example *The value 2.1e-6 is at or above
  the HIHI limit of 1e-6, configured as MAJOR*, or that the channel is shelved,
  masked, disconnected or held by a count filter;
- **Alarm limits from the IOC**: read once from the record (HIHI/HIGH/LOW/LOLO
  and their severities for analog records, ZSV/OSV/COSV for binary records, and
  the alarming states of multi-bit records).

For a group, Details shows its alarm counts and how many channels are in alarm.

## Runtime Overrides

**View → Runtime Overrides…** lists everything changed in this runtime that is
still in effect: masks that differ from the configuration, changed beep
severities (including the facility-wide ALH threshold), disabled Force PVs, NoAck
timers and shelves, with current and configured values. Select rows and click **Restore Selected** to return them to
the configuration; each change is recorded in the operation log. Use it at shift
handover to find forgotten disabled alarms.

## Alarm Summary Panel

**View → Alarm Summary Panel…** shows one tile per top-level group (or the
facility when it has no subgroups) with its counts in alarm and unacknowledged.
When the facility also has channels directly under its root, a facility tile
includes all channels. Totals always count each channel once.
Tiles use the alarm colors and blink while alarms are unacknowledged, like the
compact facility window. Click a tile to show that group in the main window. The
panel suits a wall display and is rebuilt after a facility reload.
