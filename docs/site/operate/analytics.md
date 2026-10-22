# Alarm analytics for the current session

Open **View → Alarm Analytics…** to inspect frequent offenders, chattering alarms,
standing durations, and acknowledgement response times. Collection starts when
this runtime opens, even if the analytics window is closed or notifications are
paused. Analytics use processed alarms, after QtALH alarm filtering.

Analytics are held in memory for this runtime only. Closing QtALH loses the
session. There is no shared collector, database, or import of old ALH logs.
The configuration editor does not collect analytics.

## Read the dashboard

Choose **Last 15 minutes**, **Last hour** (the default), **Last 8 hours**,
**Last 24 hours**, or **Since Session Start**. Select a group/channel scope or
search for part of a channel name or full path. The suppression filter selects
all observed activity, activity while unsuppressed, or activity while suppressed.
Historical counts and durations use the suppression state at the time of each
observation; suppression labels in rows describe the channel's current state.

Tables support sorting and selection. **Channel details** or a double-click opens
the selected channel's measurements, coverage, and incomplete-sample information.
The four tabs are:

- **Frequent offenders:** Observed normal-to-alarm activations, each channel's
  share of activations, peak severity, last activation, and coverage gaps. Changes
  between alarm severities do not create another activation. Charts show the ten
  largest activation counts and an activation trend over retained history.
- **Chattering alarms:** The default is five activations within 60 seconds.
  Adjust the count and window to recalculate chatter from retained observations.
  The table shows the current detection result, recent activation count, and
  maximum count in a detection window within the report's detail coverage. Select
  a channel to see its activation timeline.
- **Standing alarms:** Currently active process alarms, ordered by observed
  standing duration. Acknowledgement does not end standing time; recovery does.
  The table also shows accumulated active time and observed time in the selected
  range. The chart shows the ten longest currently observed standing durations.
- **Acknowledgement times:** Completed sample count, mean and maximum confirmed
  response time, outstanding age, and incomplete samples. The histogram bins are
  0–1 seconds, >1–5 seconds, >5–15 seconds, >15–60 seconds, >1–5 minutes, >5–15
  minutes, >15–60 minutes, >1–4 hours, and >4 hours.

A channel already active when first observed has an unknown true onset. Its
standing age is labeled **At least**: this means continuously observed duration,
not an estimate of how long the alarm existed before observation. Outstanding
acknowledgements with unknown onset use the same lower-bound label.

## Acknowledgements and coverage gaps

A response sample starts at an observed transition into an unacknowledged state.
Local acknowledgement completes the sample after the local state changes.
Global acknowledgement requires IOC confirmation; making a request or attempting
a failed write does not complete a sample. The engine distinguishes locally
requested confirmation from externally observed acknowledgement without claiming
an operator identity. A recovered alarm that remains latched continues waiting
for acknowledgement.

Automatic clears, masking, unknown onset, and interrupted measurements do not
enter response-time statistics. A clear coincident with an ambiguous nonlatched
IOC recovery is excluded rather than assumed to be an operator acknowledgement.
Completed samples belong to the range containing their confirmation time; their
observed onset may precede that range.

Shelved, disabled, and NoAck channels continue contributing observed process
activity, with suppression labels and filters. Mask changes and unshelving do
not create activations. Silence, display filters, and NoLog do not affect
collection. Analytics never acknowledge alarms, modify masks or shelves, send
notifications, or write output PVs.

Connection/access failures and Cancel create coverage gaps. They do not count as
process-alarm activations or chatter. Recovery establishes a fresh baseline;
QtALH does not invent the events or continuous duration that might have occurred
while observation was unavailable. Synthetic initial ERROR placeholders are not
process alarms. The dashboard reports diagnostic gaps and channel availability.

Elapsed measurements use a monotonic clock. Detected clock discontinuities,
suspend, or long event-loop stalls interrupt measurements conservatively. Fresh
observations are needed before continuous measurement resumes. UTC timestamps
identify observations and exports; changing the wall clock does not directly
change measured response durations.

## History limits and reloads

The detail buffer retains at most 24 hours, 250,000 compact records, or 64 MiB,
whichever limit is reached first. It stores state transitions, not PV values.
Session activation totals, active/observed durations, acknowledgement totals,
means, maxima, and histogram counts survive detail eviction.

A **PARTIAL DETAIL** notice identifies the start of available history. Rolling
reports, timelines, and recalculated chatter use only that coverage. Missing
history is never represented as a measured zero. Since-session summaries remain
available, while their activation timelines can cover a shorter retained period.
Rows with no process activity can still show unavailable or ambiguous channels.

Reloading the same saved configuration preserves summaries for uniquely matched
full hierarchy identities. It interrupts open measurements and waits for fresh
observations. Removed channels retain their history with a Removed label for
up to 24 hours, subject to a limit of 1,000 removed channels and a 4 MiB metadata
budget. Current configuration channels are retained in full. Evicting removed
channels also discards their session totals and detail; a **PARTIAL SESSION**
notice and the CSV `removed_channel_history_pruned` flag identify this loss.
Repeated reloads reuse unchanged ambiguous entries; their alarm histories are
not merged. A failed configuration reload leaves the
session unchanged. Opening another configuration, or reloading a symbolic link
retargeted to another file, starts a new session.

**Reset Analytics** clears only analytics and establishes new observation
baselines. Alarm, shelving, acknowledgement, and notification state remain intact.
The range, search, and chatter controls are temporary dashboard settings.

## Export

Use **Export table CSV…** for the current sorted table, or **Export chart CSV…**
for plotted series. Exports use UTF-8, quote text safely, and protect text fields
that spreadsheet software could interpret as formulas. They include the
configuration, session identifier, scope/filter choices, requested and available
coverage, units, and incomplete-measurement flags.

The offender chart export includes both top-offender and trend series. The
chatter chart export contains the selected channel's retained activation times.
The standing chart export contains its top ten durations and lower-bound flags;
the acknowledgement chart export contains the response histogram. CSV is an
explicit report export, not persistent collection or an importable ALH file.

### Binary SDDS

Builds with SDDS support also offer **Export table SDDS…** and **Export chart
SDDS…**. Each writes one binary page. The table uses the current tab's filtered
rows in their displayed sort order; the chart uses the plotted series and, for
chatter, the selected channel (or the chart's highest-activation fallback).
The report and selection are captured when the export button is pressed, so
refreshes while choosing a file do not change the saved data. After changing a
filter, wait for its report to appear before exporting.

Table columns include path, channel instance and PV, activation count/share,
peak severity, standing/active/observed durations, acknowledgement samples and
durations, chatter statistics, suppression, availability, removed/ambiguous
channel flags, gaps and incomplete samples. Unknown durations are NaN. Separate
`StandingLowerBound` and `OutstandingLowerBound` flags retain uncertainty about
when an interval began. Standing tables include only standing alarms.

Chart rows have `Series`, `Label`, channel identifiers, `Time`, `Value`, `Units`,
exact `Count`, and `TimeKnown`, `CountKnown`, `LowerBound` flags:

| Tab | Series and values |
| --- | --- |
| Frequent offenders | `top_offenders`: top ten activation counts; `trend`: retained trend-bin counts at each bin's start |
| Chattering alarms | `activation_timeline`: selected channel's retained activation times, one activation per row |
| Standing alarms | `standing`: top ten standing durations in seconds, with lower-bound flags |
| Acknowledgement times | `acknowledgement_histogram`: sample counts for the nine labelled response-time bins |

Parameters record configuration, session/generation, tab, scope/search/suppression
filters, chatter settings, requested and available coverage, partial-detail and
pruned-history flags, and totals for gaps and incomplete samples. Exporting cannot
recover discarded history. Wall times are QtALH observation/processing Unix epoch
seconds, accompanied by exact milliseconds; they are not IOC timestamps. Elapsed
durations come from the monotonic clock. Unknown wall times use NaN/zero.

Counts and millisecond timestamps use signed 64-bit SDDS integers when supported.
With older SDDS libraries they use exact decimal strings instead, avoiding lost
precision above 2^53; use `Count` for exact chart counts. Text is UTF-8. Saves
replace the destination atomically, preserving an existing file on failure.
These controls are omitted silently when compatible SDDS libraries are unavailable.
