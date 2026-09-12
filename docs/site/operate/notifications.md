# Notification subscriptions and escalation

QtALH can send grouped alarm notifications through a local sendmail-compatible
program or a generic JSON webhook. Subscriptions are personal settings associated
with the saved configuration's canonical filename and full group/channel paths.
They do not alter the ALH configuration file.

## Set up notifications

1. Open **Setup → Notifications…** in a runtime window. With no destinations saved,
   the dialog opens on **Destinations**; otherwise it opens on **Subscriptions**.
2. In **Destinations**, add an email or webhook destination. If you choose
   **Add subscription** before creating a destination, QtALH opens the destination
   editor first and then continues to the subscription editor.
3. In **Subscriptions**, add a subscription. Select the whole configuration or a
   group/channel, optionally narrow it with a PV wildcard, and check the match count.
4. Set the minimum unacknowledged severity (default **MAJOR**), repeat suppression
   (default **1,800 seconds**), and stages. The first stage defaults to **60 seconds**.
   Each stage has its own recipients/destinations and an increasing delay measured
   from the start of the same unacknowledged episode. For example, notify operators
   at 60 seconds and escalate to a supervisor at 600 seconds.
5. Use **Preview message** to inspect up to 100 matching channels without delivery.
   Click **Save and Apply**, then select a destination and click **Send Test** to
   submit explicitly synthetic data. Tests work while automatic notifications are paused.
6. Select **Enable notifications in this runtime** when ready.

PV wildcards match the entire PV name. `*` matches any number of characters and
`?` matches one character, including `/` and `\`. Character classes such as
`[a-c]` and `[!x]` are supported; separators entered literally match themselves.

Every runtime starts **paused**, even when saved subscriptions are enabled.
Multiple enabled runtimes operate independently and can send duplicate messages.
Notifications are available in local, global, and passive runtime modes, but not
in the configuration editor. Closing the runtime stops its notification work.

## Episode and suppression rules

Only initialized channels with outstanding acknowledgement severity at or above
the subscription threshold start an episode. The initial ERROR placeholder does
not qualify. A recovered transient that remains latched continues its episode.
Severity changes update the message's state without restarting its timer.

Each stage sends once to each selected destination. There are no repeating
reminders within an episode. Acknowledgement or a nonlatched clear cancels pending
alarm submissions and retries. Already submitted or in-flight messages cannot be recalled.

Shelving, Cancel, Disable, and NoAck suppress notifications and cancel pending
alarm work without resolution messages. Unshelving or removing suppression starts
a fresh initial delay, while retaining repeat suppression. Silence, display
filters, and NoLog do not suppress notifications. Notification processing never
acknowledges an alarm or writes a mask or output PV.

Optional resolution messages are off by default. When enabled, they go only to
destinations that accepted/submitted an alarm for that episode. They distinguish
an acknowledged alarm that remained active from a cleared alarm. Their state and
observation time describe the end of that episode, even if another alarm has
started before delivery or a retry.

Repeat suppression is tracked separately for each subscription, channel, stage,
and destination, starting with the first submission attempt. A recurring episode
must satisfy both its stage delay and this cooldown. Pausing clears pending work
and episodes but retains cooldowns until the runtime closes.

## Delivery and limits

Due alarms for the same subscription, stage, and destination are collected for
10 seconds. Envelopes contain at most 100 alarms and 256 KiB of JSON; large batches
are split. Value snapshots are limited to 4,096 characters. State and acknowledgement
are checked again before each attempt; queued value snapshots are retained.

There is at most one attempt per destination every 10 seconds, four simultaneous
attempts per runtime, and 1,000 queued envelopes. Due work remains pending when
capacity is exhausted, with throttling shown in Activity. A stalled event loop
resumes overdue stages through the same batching and throttling rules.

Email uses an asynchronous local process, with separate executable and argument
fields (default arguments `-i` and `-t`). No shell is used. Messages use UTF-8 MIME
on standard input. Install/configure a sendmail-compatible mailer; on Windows,
provide a compatible executable explicitly. Exit 0 means **submitted to the
mailer**, not delivered to an inbox. Exit 75 retries after 30 and then 120 seconds;
other exits and the 30-second timeout are terminal. Timeout acceptance is uncertain.

Webhooks POST versioned JSON over HTTPS with TLS verification and no redirects.
HTTP is permitted only for localhost/loopback testing. Configure a direct URL or
an environment variable containing the URL; bearer tokens must come from an
environment variable. Use environment references for secret URLs as well.

HTTP 2xx is accepted. Network failures/timeouts, HTTP 429, and HTTP 5xx retry after
30 and then 120 seconds (three attempts total). `Retry-After` can extend the delay
up to 600 seconds. Other HTTP responses are terminal. Requests time out after
30 seconds, and responses are limited to 64 KiB. An uncertain HTTP attempt may
have been accepted: receivers should deduplicate using the stable `messageId`
and `X-QtALH-Message-ID` header. Explicit test submissions make one attempt.

The JSON contains `version`, `messageId`, `configuration`, `subscription`,
`subscriptionId`, `kind` (`alarm`, `resolution`, or `test`), `stage`, UTC `timestamp`,
and an `alarms` array with episode ID, channel, full path, value, severity, status,
unacknowledged severity, and observation time. Generic JSON is not a Slack or
Teams-specific payload.

## Settings, reload, and activity

Settings live in `notifications.json` under Qt's per-user `AppConfigLocation`
(typically `$XDG_CONFIG_HOME/EPICS/qtalh`, or `~/.config/EPICS/qtalh`, on Linux).
Only destinations and subscriptions are saved. Episodes, pending work, cooldowns,
and the runtime enable switch are not persisted.

**Save and Apply** writes atomically and checks for another runtime's changes
under a file lock. On conflict, your edits are not saved: review them, use
**Reload saved settings**, and reapply them. Other runtimes apply shared-file
changes only when they explicitly reload. A corrupt or unsupported settings file
is reported and is not overwritten; repair it or restore a valid copy before
reloading.

A successful reload of the same alarm configuration preserves unchanged
subscriptions and uniquely matched channel episodes, including local latches
needed by those episodes. Dispatch waits for fresh observations. Global latches
follow the IOC. Removed/ambiguous channels and edited/disabled subscriptions lose
pending work. Failed configuration reloads leave notifications unchanged.

**Activity** retains the latest 1,000 records, including queued, submitting,
accepted/submitted, retry, cancelled, failed, and throttled events. URLs, tokens,
and mailer output are excluded. Notification operations also use the operation
log unless logging is disabled. Saved credentials are environment references;
mailer arguments and directly entered URLs are ordinary personal settings.
