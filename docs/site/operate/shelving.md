# Shelve alarms temporarily

Shelving temporarily removes selected channels from **this runtime's active
alarm presentation and sound**. Monitoring and logging continue, and alarms
return automatically when their shelves expire.

## Start a shelf

1. Right-click a channel or group in either alarm pane and choose **Shelve this
   channel…** or **Shelve this group…**. The dialog targets the item you right-clicked.
   You can also select an item and choose **Action → Shelve Alarms…**.
2. Choose **15 minutes**, **30 minutes**, **1 hour**, **4 hours**, **8 hours**, or
   **Custom minutes**, **Custom hours**, or **Custom days**. Enter a whole number
   in the selected unit, up to **365 days** (8,760 hours or 525,600 minutes).
   One hour is selected by default.
3. Enter your **Username** (up to 120 characters, without spaces or control
   characters) and a single-line **Reason** (up to 240 characters), then click **Shelve**.
   Both fields are required. Username is entered by the operator; QtALH does not
   authenticate it.

The timer starts when you apply the action. A group shelves only its currently
unshelved descendants; existing shelves keep their own deadlines and reasons.
The dialog shows the affected count and retains its displayed target when the
main selection changes. Choosing a different target through the context menu
replaces the open shelving dialog with a fresh dialog for that target.

Unfiltered views show **[Shelved]** beside affected channels and shelved counts
on their parent groups. Active and unacknowledged filters exclude those channels.
Shelved channels do not contribute to alarm counts or sound, including the
compact facility indicator.

## Review, change, or end shelves

Click **Shelved: N** or choose **View → Shelved Alarms…**. The list shows each
channel's full path, underlying alarm severity, outstanding acknowledgement
severity, expiration in local time, countdown, reason, value, and username.
The channel tooltip also shows who set the current shelf.

- Select entries and click **Unshelve Selected** to restore them immediately.
- Select one entry and click **Change Shelf…** to replace its duration and reason.
  Enter your username again; the previous operator's name is not prefilled.
  The shelf shows the username of the latest operator to set or change it, and
  the operation log records the previous username when a shelf is changed.
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

Successful reloads in the same runtime retain shelves, their usernames, and their original
deadlines for uniquely matching full channel paths. Removed, renamed, moved, or
ambiguous entries lose their shelves; new channels start unshelved. Failed
reloads preserve the current state. Local outstanding latches on matched shelved
channels survive reload; global acknowledgement state follows fresh IOC updates.

Shelves are not exported by Save As, written to configuration files, or restored
in a new runtime. Stopping the runtime clears them. Hiding the main window while
the compact runtime remains open leaves them active. Expiration uses the system
wall clock and is checked on the next engine tick, including after suspend.

The operator log records usernames, reasons, and deadlines for shelving, changes, early
unshelving, expiry, and reload-related removal. Existing logging-disable options
still apply; persistent attribution requires operation logging to be enabled. See [alarm logs](/operate/logs) and
[silencing and filters](/operate/silence-filters) for related workflows.
