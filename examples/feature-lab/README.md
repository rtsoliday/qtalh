# Interactive QtALH feature lab

This lab runs a four-channel EPICS IOC, QtALH, and a webhook receiver on this
computer. Everything uses loopback. No email account or external webhook service
is needed. Python 3, QtALH, and EPICS Base are already available on this Linux host.

## Start

From a terminal in your desktop session:

```sh
cd /home/SOLIDAY/github/qtalh
python3 examples/feature-lab/run.py
```

Keep that terminal open. QtALH opens in Fusion appearance. Enter the commands
below at the terminal's `lab>` prompt. Use the QtALH window for acknowledgement,
shelving, configuration, and analytics. Type `help` for all commands and `quit`
to stop the lab's processes. Closing just the QtALH window leaves the IOC and
receiver running; `restart` opens QtALH again.

The default CA ports are 15068/15069 and the webhook port is 18080. If a port is
occupied, stop the other lab or choose unused ports, for example:

```sh
python3 examples/feature-lab/run.py --port 15168 --webhook-port 18081
```

Use the webhook URL printed by the launcher. The script checks ports before
starting and does not stop unrelated processes. All lab commands write only
the `qtalh_lab:` PVs on this loopback IOC.

## Try alarms and acknowledgement

Select **Process** in the QtALH tree. Enter each command separately:

| Terminal command | Expected result |
| --- | --- |
| `minor` | Pressure becomes MINOR. |
| `major` | Pressure becomes MAJOR. |
| `normal` | Current pressure alarm clears; outstanding acknowledgement remains latched. |

Select the pressure channel and use **Action → Acknowledge Alarm** or **Ctrl+A**.
Acknowledgement clears the outstanding indication, not the process value.
To test acknowledging an active alarm, enter `major`, acknowledge it while red,
then enter `normal`.

The lab starts with alarm sound enabled and uses the included alarm audio file.
An eligible unacknowledged alarm plays the sound repeatedly. Use the silence
controls in QtALH to stop it, or launch with `--silent` for quiet testing.
Alarm and operation logging remain enabled.

## Test notification setup and delivery

1. Open **Setup → Notifications…**. First-time setup opens on **Destinations**.
2. **Add destination**: name `Local test receiver`, type **webhook**, URL
   `http://127.0.0.1:18080/notifications` (or the URL printed by the launcher).
   Leave both environment-variable fields empty. Click **OK**, then **Save and Apply**.
3. Select the destination and click **Send Test**. The terminal prints
   `Notification received: test`. Check the **Activity** tab as well.
4. Under **Subscriptions**, click **Add subscription**. Enter name `Lab major alarms`,
   keep **Subscription enabled** checked, use **Whole configuration**, wildcard
   `*`, and minimum severity **MAJOR**. Set repeat suppression to **30 seconds**
   for this exercise. The match count should be four.
5. Click **Add stage**, set **5 seconds**, and **check the checkbox** beside
   `Local test receiver`. Click **OK**. Optionally add a second stage at **20 seconds**,
   checking the same destination, to exercise escalation. Delays are measured from
   the start of the alarm episode, so the second stage is at 20 seconds total.
6. Optionally check **Send resolution to destinations that accepted an alarm**.
   Click **OK**, then **Save and Apply**, then check **Enable notifications in this runtime**.
7. In the terminal, enter `clear`, acknowledge all outstanding alarms in QtALH,
   then enter `major`. Leave it unacknowledged. Expect the first message after
   roughly **15 seconds** (5-second delay plus 10-second batching), and the second
   around **30 seconds** if configured. The terminal reports each received message.
8. Acknowledge pressure. Pending stages are cancelled; a resolution message is
   sent if enabled and an alarm message was already accepted. For another episode,
   enter `normal` followed by `major`. Repeat suppression can postpone that episode's
   delivery until the 30-second cooldown expires.

Each stage sends once per episode; repeat suppression is not a periodic reminder.
Acknowledging before delivery, or applying shelving/Disable/NoAck/Cancel, suppresses
pending notifications. Audio silence does not suppress them.

The receiver saves every accepted JSON message to `.state/webhooks.jsonl`, one
per line. It is a capture receiver, not a simulation of email or Slack/Teams delivery.

## Test timed shelving

1. Enter `maintenance`; select the maintenance channel under **Diagnostics**.
2. Choose **Action → Shelve Alarms…**, choose **Custom minutes**, enter **1**, and
   enter your username and a reason such as `Lab maintenance`. Apply it.
3. Inspect **Action → Shelved Alarms…** and watch the countdown.
4. Leave the PV in alarm. After one minute, the shelf expires automatically.
   Shelving does not clear the PV or acknowledge it. Notification eligibility
   resumes with a fresh delay after unshelving.
5. Enter `set maintenance 0` and acknowledge it to finish.

For longer shelves, choose **Custom hours** or **Custom days**. The maximum
duration is 365 days; shelves still end when the runtime closes.

The separate **NoAck for One Hour** action is not a one-minute shelf.

## Test analytics

Open **View → Alarm Analytics…**. Keep the default last-hour range or choose
**Since Session Start**. Refreshes may take about a second to appear.

| Exercise | What to inspect |
| --- | --- |
| Enter `normal`, `major`, `normal` several times | Frequent offenders counts normal-to-alarm activations. `minor` then `major` within one active alarm does not add an activation. |
| Enter `chatter` | Six activation/recovery cycles in about 12 seconds. The chatter channel meets the default five-activations/60-second threshold. Its current flag expires as activations leave the detection window. |
| Enter `standing`, then acknowledge temperature while it remains active | Standing duration continues through acknowledgement. `set temperature 0` ends it. |
| Clear and acknowledge pressure, enter `major`, wait several seconds, then acknowledge it | A completed acknowledgement-time sample appears. |
| Clear and acknowledge pressure, enter `major`, then `normal`, wait, then acknowledge | Recovered but latched pressure remains pending until acknowledged. |
| Enter `gap` | IOC disconnect/reconnect is recorded as a coverage gap, not new process alarm activity. All IOC values restart normal. |

Try scope, channel search, suppression filters, chatter thresholds, details, and
CSV export. Select the intended channel for its chatter timeline. **Reset Analytics**
clears analytics only. It establishes fresh baselines; an already-active alarm's
onset becomes unknown until a subsequent observed recovery and activation.

## Try restart, appearances, and modes

The `restart` command keeps the IOC running and reopens QtALH. Saved subscriptions
and destinations remain; notifications start paused and must be re-enabled.
Pending deliveries, escalation progress, cooldowns, local shelves, and analytics
are session-only. Lab PV values stay as they were because the IOC is still running.

Quit the lab before changing startup options:

```sh
python3 examples/feature-lab/run.py --style motif
python3 examples/feature-lab/run.py --mode global
python3 examples/feature-lab/run.py --mode passive
```

Local mode is the default. Global mode uses the IOC's acknowledgement fields;
passive mode observes global acknowledgement state without writing it. In local
mode, the IOC ACKS displayed by `status` is separate from QtALH's local acknowledgement.

## Files and verification

Lab settings are isolated from your usual QtALH settings:

- Saved notifications: `.state/config/EPICS/qtalh/notifications.json`
- Captured notifications: `.state/webhooks.jsonl`
- Process diagnostics: `.state/ioc.log`, `.state/qtalh.log`, `.state/repeater.log`
- Alarm and operation log files: in `.state/` (the lab's working directory).

Paths above are relative to `examples/feature-lab/`. `.state/` is ignored by Git.
It persists across lab launches. The launcher does not create or enable notification
subscriptions for you; the setup exercise uses the actual application form.

With the interactive lab stopped, run a headless connectivity check:

```sh
python3 examples/feature-lab/run.py --smoke-test
```

It validates the configuration, checks MINOR/MAJOR/normal on all four PVs, posts
one synthetic notification to the local receiver, and cleans up its processes.
This is an IOC/receiver check; it does not exercise QtALH's notification sender.
The automated application suites are documented in [the test guide](../../qtalh/tests/README.md).
