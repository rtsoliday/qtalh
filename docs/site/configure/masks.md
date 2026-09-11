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

Canonical configuration order is `CDATL`. For example, `-D--L` enables Disable and NoLog; `-----` clears all five bits.

## Configure a Force PV

To apply a mask automatically when another PV reaches a value, add a `$FORCEPV` directive to the target group or channel:

```text
GROUP NULL Facility
CHANNEL Facility example:pressure -----
$FORCEPV example:maintenance -D--- 1 0
```

When `example:maintenance` becomes 1, Disable is applied. When it becomes 0, the configured channel mask is restored. A group Force PV acts on descendant channels. Use `NE` as the reset value to restore the mask when the force value is left.

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

Validate configuration changes before activating them. For argument limits and retry behavior, see the [configuration reference](/reference/configuration) and [compatibility inventory](/understand/compatibility).
