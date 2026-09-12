# Architecture

QtALH separates configuration and alarm state from Channel Access, file services, and widgets. This allows the alarm algorithms to be exercised with deterministic PV callbacks and clocks.

| Layer | Location | Responsibility |
| --- | --- | --- |
| Core | `qtalh/core/` | Document tree, legacy text parsing/writing, masks, alarm state, filtering, acknowledgement, and aggregation. |
| Services | `qtalh/services/` | Channel Access, CLI options, logging, log search, locking, broadcasts, and helper protocols. |
| UI | `qtalh/ui/` | Item models, custom row controls, runtime/editor windows, dialogs, and operator workflows. |
| Entry points | `qtalh/main.cc`, `printer_main.cc`, `database_main.cc` | Main application and Unix queue helpers. |
| Tests | `qtalh/tests/` | Core, UI, IOC, helper, visual, and benchmark fixtures. |

Core uses Qt Core and EPICS calculation routines, with no widget or Channel Access dependency. `PvService` and clock/log/command callbacks are the test seams.

## Alarm update flow

A CA subscription delivers an event to the engine. The engine updates channel state, propagates group counts, and invokes eligible log/command/output callbacks. The UI refreshes its models separately, preserving transition processing even when drawing is deferred.

Channel Access uses a shared non-preemptive context, socket notifiers, and a 100 ms fallback poll. Callbacks are serialized on the GUI thread, polling is guarded against reentrancy, and subscriptions are cleared before model destruction. Ownership distinguishes subscriptions for separate rows using the same PV.

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

Parsing and insertion are transactional. Save uses `QSaveFile`; includes are expanded and formatting is normalized. Editor activation copies the current configuration into an independent runtime. Runtime Save As exports masks while retaining the original monitoring/reload/lock identity.

## Platform boundaries

The main Qt application and portable file services support Linux, macOS, and Windows. Motif ALH and System V/Sun RPC helpers remain Linux/macOS only. Unix helpers use native-ABI queue records; Windows file locks do not provide POSIX interoperability.

[Compatibility](/understand/compatibility) records the implemented behavior and remaining acceptance work. [Tests & contributions](/develop/testing) explains how to validate a change.
