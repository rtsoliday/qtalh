# Active alarms, search & details

With optional SDDS support, **File → Export Alarm Snapshot (SDDS)…** saves all
configured channels in one binary page, regardless of the current view filter.
The snapshot includes values as text, hierarchy, alarm and acknowledgement state,
configured/runtime masks, shelving and observation availability. It changes no
alarm state and writes no PVs. Timestamps describe QtALH processing, not IOC time.
See the [SDDS export conventions](/operate/analytics#binary-sdds) for time and
integer encoding.

These runtime tools add views for finding and understanding alarms. They work
with every appearance, including `-style motif`, and do not change how the alarm
tree, acknowledgement or logging behave. Their **Acknowledge** and **Restore
Selected** buttons act like the matching main-window commands, including the
IOC writes those commands make in global active mode, and **Retry ACKPV**
writes a previously refused `$ACKPV` value; see
[Runtime Overrides](#runtime-overrides).

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
**Details…** and **Show in Tree** for one row. When the main window's display
filter hides that row's channel (for example, an acknowledged alarm under
**Unacknowledged Alarms Only**), both report this in the message area
instead of selecting it, and Details… opens nothing. Double-click a row to
select it in the main window. Rows update as alarms change and keep your selection; a
facility reload closes the window.
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
  the unacknowledged severity is the one the main window shows, and one hidden
  by the channel's mask (such as the IOC's in global mode with NoAck) is noted
  as underlying;
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

Restoring is an operator action, not only a display change. A Mask row returns
the channel to its configured mask, as **Reset** in Force Mask does; a NoAck row
ends **NoAck for One Hour**; a Force PV row enables the Force PV again, which
reapplies its current value. In global active mode (`-global` without `-S`) a
restore makes the same IOC writes as those commands:

- ACKT, when the NoAckT (`T`) bit changes;
- ACKS and the configured `$ACKPV`, when removing Disable or NoAck exposes a
  cleared transient alarm that is still unacknowledged. The alarm is then
  acknowledged for every ALH and Channel Access client, and the operation log
  records *Auto ack of transient alarms on enable*;
- the configured `$SEVRPV`, when Disable changes or removing Cancel resumes
  monitoring.

Mask rows also list masks applied by an active Force PV, such as a maintenance
interlock. Restoring one overrides the Force PV until its value next changes.
Check **Alarm Details** for the Force PV state before restoring masks in bulk.
In local mode a restore writes nothing to the IOC; in passive mode (`-S`)
**Restore Selected** is unavailable. Restoring beep severities and shelves never
writes to the IOC.

Failed `$ACKPV` submissions appear as **ACKPV action pending**, with the captured
PV, value and number of pending actions. QtALH reports when ACKS was submitted
but the separate ACKPV action was not. Correct the connection or write-access
problem, select the row and click **Retry ACKPV**. Each click submits one
pending ACKPV action per selected row; it never repeats ACKS. Restore Selected
does not act on pending ACKPV rows. Recovery alone never retries them.
A retry does not check the channel's current alarm: it writes the captured
value even if a newer alarm is unacknowledged or the channel is Disabled.
Only a rejected submission is retryable; accepted asynchronous writes with an
uncertain IOC outcome are not replayed. Pending actions are runtime only and
are discarded by channel Cancel or configuration reload. Retry requires global
active mode and is unavailable in passive mode.

## Alarm Summary Panel

**View → Alarm Summary Panel…** shows one tile per top-level group (or the
facility when it has no subgroups) with its counts in alarm and unacknowledged.
When the facility also has channels directly under its root, a facility tile
includes all channels. Totals always count each channel once.
Tiles use the alarm colors and blink while alarms are unacknowledged, like the
compact facility window. Click a tile to show that group in the main window. The
panel suits a wall display and is rebuilt after a facility reload.
