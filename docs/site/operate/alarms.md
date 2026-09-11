<script setup>
import { withBase } from 'vitepress'
</script>

# Monitor & acknowledge alarms

Use the main window to find a channel, inspect its state, and acknowledge the alarms you have reviewed.

## Open a facility

Start QtALH with your configuration and the main window visible:

```sh
bin/Linux-x86_64/qtalh -mainwindow /path/to/facility.alhConfig
```

The compact facility window remains available. Its indication summarizes the facility; clicking the facility button opens the main window. Closing the main window hides it while monitoring continues. Close the compact window or choose File Exit/Close and confirm to stop the runtime.

## Navigate the hierarchy

1. Select a group in the left tree to show its contents in the right pane.
2. Select a channel to make it the target of an action.
3. Double-click a group in the right pane to open its contents. The arrow control expands or collapses its branch in the tree.
4. Use the View expansion actions to open a level, a branch, or the whole hierarchy.

Group indications summarize their eligible descendant channels. Cancelled and disabled channels do not contribute active severity in the same way as monitored, enabled channels. [Mask reference](/reference/configuration) explains the individual settings.

<figure>
  <img :src="withBase('/images/main-window.png')" alt="QtALH runtime showing a facility tree at left and channel controls at right" loading="lazy" />
  <figcaption>Repository test fixture. ① Left: facility hierarchy. ② Right: the selected group's channels and action controls. ③ Bottom: runtime status and alarm controls. Labels and font rendering vary by platform.</figcaption>
</figure>

## Interpret the indication

| State | Meaning |
| --- | --- |
| NO_ALARM | The monitored record is currently normal. |
| MINOR / yellow | Minor alarm severity. |
| MAJOR / red | Major alarm severity. |
| INVALID / white | Invalid record severity. |
| ERROR / white | QtALH has a connection/access error or is waiting for an initial usable event. |

An alarm can be **currently normal and still unacknowledged** after a transient. Inspect both the current state and acknowledgement indication; [Alarm lifecycle](/understand/alarm-lifecycle) explains how they differ.

## Acknowledge a channel or group

1. Select the channel, or select a parent group to act on its descendant channels.
2. Read the guidance and inspect the equipment condition as appropriate.
3. Choose **Action → Acknowledge Alarm** or press **Ctrl+A**.
4. Confirm the acknowledgement indication clears or reflects the remaining alarms.

Passive mode prevents acknowledgement. In global mode, the IOC must allow the write; failed acknowledgements remain visible. Acknowledging an active alarm does not clear the underlying alarm condition.

## Open guidance or a related process

- **Ctrl+G** opens configured guidance: inline text, a local file, or a URL.
- **Ctrl+P** runs the configured related process or opens its command menu.
- Properties and action dialogs follow the selected group/channel. Selecting another node changes their target; background updates preserve unfinished edits where supported.

Related processes run actual configured commands on your host. Their syntax depends on the operating system.

Continue with [silence and filters](/operate/silence-filters) or [historical logs](/operate/logs).
