# Masks & automatic forcing

Masks control monitoring, participation in alarm summaries, acknowledgement requirements, and alarm logging. Choose the setting that matches the operator's intended action.

## Select a mask setting

| Goal | Setting | What continues |
| --- | --- | --- |
| Stop a channel's alarm subscription | Cancel (`C`) | Other channels continue monitoring. |
| Exclude a channel from active alarm handling | Disable (`D`) | Its subscription and alarm-file records continue unless separately cancelled/suppressed. |
| Remove acknowledgement requirements | NoAck (`A`) | Monitoring continues. |
| Do not latch cleared transients | NoAckT (`T`) | Active alarms remain visible. |
| Stop a channel's alarm records | NoLog (`L`) | Monitoring and display continue. |

Use **Modify Mask** (**Ctrl+S**) or **Force Mask** (**Ctrl+M**) for the selected node. Acting on a group applies the change to descendant channels. In passive mode, restricted operations remain unavailable. Global transient-setting writes require IOC write access; failures retain the previous displayed bit and can be retried explicitly.

In global active mode (`-global` without `-S`), mask changes also write to the IOC. Changing NoAckT writes the record's ACKT. Removing Disable or NoAck from a channel whose cleared transient alarm is still unacknowledged writes ACKS and the configured `$ACKPV`, which acknowledges the alarm for every client. Changing Disable, or removing Cancel so that monitoring resumes, writes the configured `$SEVRPV`. The same writes happen when a Force PV applies or resets a mask, and when **Restore Selected** in [Runtime Overrides](/operate/alarm-tools#runtime-overrides) restores a mask, NoAck timer or Force PV. Local mode makes none of these writes.

In global mode, Disable and NoAck do not protect a channel's IOC acknowledgement. A group acknowledgement still writes ACKS, and any `$ACKPV`, for such channels in the groups it enters; see [Acknowledge a channel or group](/operate/alarms#acknowledge-a-channel-or-group).

In local mode, enabling NoAckT leaves a cleared transient that was already
latched awaiting acknowledgement. The new setting applies to subsequent alarm
transitions; it does not clear the earlier event, but a later transition
replaces it.

Canonical configuration order is `CDATL`. For example, `-D--L` enables Disable and NoLog; `-----` clears all five bits.

**Modify Mask Settings** changes one field immediately and preserves the other
fields. Each row's **Reset** uses that channel's configured value. **Force Mask**
lets you choose all five fields, then **Apply** sets the mask on the selected
channel or every descendant channel in a group. **Reset** restores each channel's
configured mask. **Close** (or **Dismiss** in the classic appearance) closes the
dialog without undoing changes already applied.

## Configure a Force PV

To apply a mask automatically when another PV reaches a value, add a `$FORCEPV` directive to the target group or channel:

```text
GROUP NULL Facility
CHANNEL Facility example:pressure -----
$FORCEPV example:maintenance -D--- 1 0
```

When `example:maintenance` becomes 1, Disable is applied. When it becomes 0, the configured channel mask is restored. A group Force PV acts on descendant channels. Use `NE` as the reset value to restore the mask when the force value is left.

The first value counts as a change: if the Force PV is already at its reset value, the configured masks are restored at startup. In global active mode (`-global` without `-S`), forcing and resetting can write to the IOC with nobody acknowledging:

- The resulting NoAckT setting is written to each channel's ACKT when it differs from the current ACKT, and always before that ACKT has been read. This happens without `-caputackt` and can change an ACKT that other clients share.
- When Disable or NoAck is removed, an alarm that has already cleared but is still unacknowledged is acknowledged automatically: QtALH writes the IOC ACKS and the channel's `$ACKPV`. Expiry of NoAck for One Hour and Runtime Overrides **Restore Selected** do the same.

Open **Force Process Variable** (**Ctrl+V**) for the selected node. In the modern
appearance the mask choices use two columns and the CALC expression and inputs
follow the force and reset values, so the form normally fits without scrolling;
Apply, Cancel and Close stay visible.

![Modern Force PV dialog showing the enable setting, mask, force/reset values and CALC inputs](/images/modern-force-pv.png)

The screenshot shows the repository's synthetic CALC example with Disable selected
and an `NE` reset, rather than the literal-PV example above.

## Use an expression

A calculated Force PV requires an expression. Inputs A–F can be constants or PV names:

```text
GROUP NULL Facility
CHANNEL Facility example:pressure -----
$FORCEPV CALC -D--- 1 0
$FORCEPV_CALC A>0
$FORCEPV_CALC_A example:maintenance
```

Evaluation waits until all named PV inputs are available. Constants can evaluate immediately; omitted inputs are zero. The main-window footer shows the facility-wide count of disabled Force PVs.

An input is a constant only when the whole token is a number, such as `0` or `-2`. A name beginning with a digit, such as `1ID:gate`, is a PV; legacy ALH treated it as the constant 1, and treated a loaded `0` as a PV name.

Validate configuration changes before activating them. For argument limits and retry behavior, see the [configuration reference](/reference/configuration) and [compatibility inventory](/understand/compatibility).
