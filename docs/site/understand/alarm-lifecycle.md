# Alarm lifecycle & modes

An alarm's **current severity** describes the condition now. Its **unacknowledged severity** records what the operator still needs to acknowledge. Keeping these separate preserves visibility of short-lived alarms.

## A typical local alarm

1. The PV connects and reports NO_ALARM.
2. Its severity rises to MAJOR. QtALH updates the row, parent summaries, and eligible alarm actions.
3. The condition clears. Current severity returns to NO_ALARM.
4. With the default transient mask, the outstanding MAJOR acknowledgement remains.
5. The operator acknowledges it. The outstanding indication clears.

An acknowledgement can also occur while the alarm is still active. It does not write a new process value or correct the equipment condition. NoAck and NoAckT masks change acknowledgement requirements and transient retention.

## Choose who owns acknowledgement

| Mode | Acknowledgement | Configured output PVs |
| --- | --- | --- |
| Local (default) | Maintained in this QtALH runtime. | Heartbeat, severity, and acknowledgement PVs are not written. |
| Global (`-global`) | Uses IOC ACKS/ACKT fields. | Active mode can write configured outputs when permitted. |
| Passive (`-S`, with either local or global) | Operator acknowledgement is disabled; no CA writes. | Writes are suppressed. |

`-caputackt` applies configured ACKT settings at startup in global active mode. Access-right failures remain visible; severity-output and automatic Force PV ACKT retries have specific behavior described in [Compatibility](/understand/compatibility).

## Separate the controls

- **Silence (`-s`)** controls sound.
- **Disable file logging (`-D`)** controls alarm and operation file writes; configured helper queues retain their behavior.
- **Passive (`-S`)** controls CA writes and operator acknowledgement.
- **Display filters** control which rows appear.

These controls do not disable configured shell commands. Commands can run for configured severity/status transitions, and related-process actions run on request. `MASTER_ONLY` and stop-logging broadcasts impose additional command restrictions.

## What a group represents

Groups aggregate eligible descendant channel severity, acknowledgement, and mask counts. Cancel and Disable exclude channels from active summaries; NoAck removes their acknowledgement requirements. A parent's alarm can remain elevated because a different descendant still has an alarm or outstanding acknowledgement.

Before the first usable event, an unconnected channel uses ERROR as a placeholder. Connection and access changes can also produce ERROR. Value-only updates refresh the displayed value without creating another alarm record.
