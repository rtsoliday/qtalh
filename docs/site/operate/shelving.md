# Shelve alarms temporarily

Shelving temporarily removes selected channels from **this runtime's active
alarm presentation and sound**. Monitoring and logging continue, and alarms
return automatically when their shelves expire.

## Start a shelf

1. Select a channel or group and choose **Action → Shelve Alarms…**.
2. Choose **15 minutes**, **30 minutes**, **1 hour**, **4 hours**, **8 hours**, or
   custom minutes from **1 to 1,440**. One hour is selected by default.
3. Enter a single-line reason, up to 240 characters, and click **Shelve**.

The timer starts when you apply the action. A group shelves only its currently
unshelved descendants; existing shelves keep their own deadlines and reasons.
The dialog shows the affected count and retains its displayed target when the
main selection changes.

Unfiltered views show **[Shelved]** beside affected channels and shelved counts
on their parent groups. Active and unacknowledged filters exclude those channels.
Shelved channels do not contribute to alarm counts or sound, including the
compact facility indicator.

## Review, change, or end shelves

Click **Shelved: N** or choose **View → Shelved Alarms…**. The list shows each
channel's full path, underlying alarm severity, outstanding acknowledgement
severity, expiration in local time, countdown, reason, and value.

- Select entries and click **Unshelve Selected** to restore them immediately.
- Select one entry and click **Change Shelf…** to replace its duration and reason.
- **Action → Unshelve Alarms** clears every shelf under the selected group,
  including shelves applied individually.

At expiry, the current alarm presentation returns automatically. A cleared
transient can reappear if it still requires acknowledgement. Existing silence
settings and beep thresholds apply. Unshelve a channel before acknowledging it;
bulk acknowledgement skips shelved channels.

## What shelving changes

| Operation | Alarm counts and active lists | Acknowledgement state | Sound |
| --- | --- | --- | --- |
| Shelving | Temporarily excludes selected channels | Preserved under normal alarm/mask rules | Suppressed for selected channels |
| Timed NoAck | Uses the existing NoAck mask behavior | Changes acknowledgement requirements | Follows NoAck behavior |
| Silence | Unchanged | Unchanged | Suppressed according to the selected silence option |

Shelving does not change masks or write ACKS/ACKT. Command hooks and severity
output PVs continue to reflect the underlying alarm state. Monitoring, count
filtering, and alarm logging continue normally. Other clients are unaffected,
even in global mode. Changes from Force PVs, manual masks, and other clients'
acknowledgements still apply; expiry does not override Disable or Cancel.

## Reloads and runtime lifetime

Successful reloads in the same runtime retain shelves and their original
deadlines for uniquely matching full channel paths. Removed, renamed, moved, or
ambiguous entries lose their shelves; new channels start unshelved. Failed
reloads preserve the current state. Local outstanding latches on matched shelved
channels survive reload; global acknowledgement state follows fresh IOC updates.

Shelves are not exported by Save As, written to configuration files, or restored
in a new runtime. Stopping the runtime clears them. Hiding the main window while
the compact runtime remains open leaves them active. Expiration uses the system
wall clock and is checked on the next engine tick, including after suspend.

The operator log records reasons and deadlines for shelving, changes, early
unshelving, expiry, and reload-related removal. Existing logging-disable options
still apply. See [alarm logs](/operate/logs) and
[silencing and filters](/operate/silence-filters) for related workflows.
