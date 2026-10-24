# Architecture

QtALH separates configuration and alarm state from Channel Access, pvAccess, file services, and widgets. This allows the alarm algorithms to be exercised with deterministic PV callbacks and clocks.

| Layer | Location | Responsibility |
| --- | --- | --- |
| Core | `qtalh/core/` | Document tree, legacy text parsing/writing, Phoebus XML conversion, masks, alarm state, filtering, acknowledgement, and aggregation. |
| Services | `qtalh/services/` | Channel Access, pvAccess, PV routing, CLI options, logging, log search, locking, broadcasts, and helper protocols. |
| UI | `qtalh/ui/` | Item models, custom row controls, runtime/editor windows, dialogs, operator tools, and the bundled help. |
| Entry points | `qtalh/main.cc`, `printer_main.cc`, `database_main.cc` | Main application and Unix queue helpers. |
| Tests | `qtalh/tests/` | Core, UI, IOC, helper, visual, and benchmark fixtures. |

Core uses Qt Core and EPICS calculation routines, with no widget, Channel Access, or pvAccess dependency. `PvService` and clock/log/command callbacks are the test seams.

## Alarm update flow

A CA or pvAccess subscription delivers an event to the engine. The engine updates channel state, propagates group counts, and invokes eligible log/command/output callbacks. The UI refreshes its models separately, preserving transition processing even when drawing is deferred.

Channel Access uses a shared non-preemptive context, socket notifiers, and a 100 ms fallback poll. Callbacks are serialized on the GUI thread, polling is guarded against reentrancy, and subscriptions are cleared before model destruction. Ownership distinguishes subscriptions for separate rows using the same PV.

`PvRouter` sends names starting with `pva://` to the pvAccess client and all others to Channel Access, behind the same `PvService` interface. pvAccess callbacks arrive on EPICS threads; their values are queued and delivered on the GUI thread, matching the CA model. Because PVA alarms have no ACKS/ACKT, the pvAccess client keeps acknowledgement state for each `pva://` record in the runtime.

The PVA inbox holds at most 4,096 updates or 4 MiB of accounted payload and
delivers at most 256 updates per GUI turn. Received strings (value text,
enumeration choices, alarm messages and status text) are truncated to 4,096
characters, so a single update cannot exceed the budget. Overflow reports lost
observations, publishes a communication gap, and recreates connections in small
batches to obtain fresh state. Stale callbacks are rejected by channel/operation
identity.
Each output has at most 128 pending writes and a 30-second setup/completion
deadline. Consecutive pending severity or heartbeat writes retain the latest
value; ordered writes are rejected visibly when full. A timeout cancels pending
writes and reports uncertain acceptance of an in-flight write.

Analytics and notifications subscribe independently through lifetime-managed
engine observers. Observations carry before/after state, availability, suppression
flags, and acknowledgement causes. The compatibility logging callbacks remain
separate; a global acknowledgement request is not an analytics confirmation.

Session analytics retain compact transitions in a bounded rolling buffer and
incremental session summaries. The GUI requests immutable report snapshots at
most once per second. A single background job per runtime aggregates each
snapshot; generation checks discard results invalidated by reset or reload.
Virtual table models and QPainter charts share the resulting report.

See [Alarm analytics](/operate/analytics) for metric definitions and coverage limits.

## Timers and background work

The engine's 200 ms tick handles due filter/NoAck work. Heartbeats use a separate precise timer and skip missed beats after event-loop delays. UI status/blink work is separated from full model refreshes. Historical log searches run in a cancellable worker thread with bounded results.

## Documents and runtime windows

Parsing and insertion are transactional. Save uses `QSaveFile`. The parser keeps each statement's preceding comments, its text as written and the INCLUDE file that declared it; writers emit unchanged statements as read and the rest in legacy form, either as separate files (`writeConfigFiles`) or expanded. Editor round trips use `ConfigText::Edit` text, which marks INCLUDE file boundaries. Every save reads its text back first and refuses one that would load differently. Editor activation copies the current configuration into an independent runtime. Runtime Save As exports masks while retaining the original monitoring/reload/lock identity.

## Platform boundaries

The main Qt application and portable file services support Linux, macOS, and Windows. Motif ALH and System V/Sun RPC helpers remain Linux/macOS only. Unix helpers use native-ABI queue records; Windows file locks do not provide POSIX interoperability.

[Compatibility](/understand/compatibility) records the implemented behavior and remaining acceptance work. [Tests & contributions](/develop/testing) explains how to validate a change.
