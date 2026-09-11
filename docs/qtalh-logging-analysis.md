# QtALH logging investigation and optimization

The current implementation uses persistent binary checkpoints. The investigation
below describes the earlier implementation; see [implemented changes](#implemented-changes).

Investigation on 2026-09-11, using the same i7-4770/Linux/Qt 5.15.3/EPICS 7.0.8
setup as the [CPU comparison](qtalh-performance.md). Application code was not
changed during that initial investigation. All PVs belonged to an isolated
loopback IOC; logs were on `/tmp` (XFS).

**The alarm data is the same in the tested workload. The main extra cost is
QtALH's per-record filesystem work for circular-log position metadata.**

## Do the applications write more alarm records?

Both applications monitored the same 1,000 channels simultaneously. A shared IOC
gate enabled and then stopped their normal/MAJOR transitions. Unlimited logging
(`-m 0`) retained the complete output for comparison; initial startup output was
excluded. Each application's capture contained:

- 11,000 records spanning all 1,000 PVs.
- Exactly 123 bytes per record: 1,353,000 bytes in total.
- 5,000 MAJOR records and 6,000 NO_ALARM records.
- Identical ordered per-PV sequences of status, severity, and value.
- Byte-identical record payloads after excluding timestamps and allowing
  interleaving between PVs. Timestamp values were excluded because each client
  timestamps its own callback receipt.

In the circular runs, both final alarm files held exactly 2,000 records and
246,000 bytes. A separate syscall trace showed only 123-byte alarm-file writes
in both applications. QtALH was not duplicating records or rewriting the entire
ring in this fixed-width workload.

[Record comparison results](benchmarks/alh-vs-qtalh-log-content-2026-09-11.json).

## Isolation experiment: circular versus unlimited logging

Logging stayed enabled in every run. The only logging setting changed was
`-m 2000` versus `-m 0`. Each application had a fresh log directory; the IOC
nominally generated 2,000 transitions/second. Results are medians of three
10-second process-CPU measurements after three seconds of warmup. 100% means
one CPU core. Instrumented trace/profile runs were separate from these trials.

| Logging mode | ALH CPU | QtALH CPU |
| --- | ---: | ---: |
| Circular, 2,000 records | 3.10% | 31.97% |
| Unlimited, append only | 3.00% | 4.60% |

QtALH CPU fell by about 86% when the circular bookkeeping path was bypassed,
while it continued recording every transition. Unlimited logging changes
retention, so this is a diagnostic experiment rather than an equivalent
replacement for bounded logging.

The circular QtALH trials spent approximately 1.01 seconds in userspace and
2.16 seconds in the kernel per 10-second measurement; ALH spent about 0.13 and
0.19 seconds respectively. Process-wide write-call counts were approximately
40,541 versus 20,840, with approximately 5.02 MB versus 2.54 MB of requested
write data per measurement. These process counters include UI/internal traffic;
the file-specific trace below separates the alarm and metadata writes.

[All CPU/I/O trials](benchmarks/alh-vs-qtalh-logging-2026-09-11.csv).

## What the earlier QtALH did for each circular alarm record

In the investigated version, `AlarmLogFile::write` wrote and flushed the
ordinary alarm record, updated hashes for the overwritten/new record, and called
`savePosition()`. That function serialized a small JSON document containing the
record count, next ring slot, format version, and content fingerprint into
`alarm.log.qtalh-position`.

On this Qt/XFS setup the trace shows this additional sequence for each record:

1. `openat(..., O_TMPFILE)` creates a temporary inode.
2. `fchmod` applies the log file permissions.
3. `write` writes 123–126 bytes of JSON metadata.
4. `linkat` gives the temporary inode a filename.
5. `rename` atomically replaces the previous position file.
6. `lseek`, `close`, and a cleanup `unlink` complete the temporary-file lifecycle.
   The unlink returns ENOENT because the file has already been renamed.

Thus about 2,000 metadata files are created and replaced every second at the
benchmark rate. The additional metadata bytes roughly double logical file
writes, but the repeated inode and directory operations are much more expensive
than simply extending or overwriting the existing alarm file. The sidecar is
only about 125 bytes on disk at any one time; it does not grow with the log.

The trace recorded 4,241 QtALH alarm writes and 4,242 complete metadata-update
sequences (attachment caught one sequence already in progress). ALH recorded
6,000 alarm writes with no metadata-update sequence. Tracing slows execution,
so these counts establish operations per record, not comparative throughput.
All threads were traced; an initial main-thread-only trace was discarded because
it did not capture the file writes.

Neither trace contained `fsync` or `fdatasync`. Both implementations flush their
record writes; the cost measured here is not an extra per-record durable-sync
operation. `/proc` also attributed about 83 MB of writeback to QtALH per trial,
with nearly all of it canceled when replaced temporary files were removed;
that counter must not be read as 83 MB physically written to the storage device.

[Syscall counts and write sizes](benchmarks/alh-vs-qtalh-log-syscalls-2026-09-11.json).

## CPU profile confirms the dominant cost

A separate ten-second `perf` sample of circular QtALH logging attributed:

| Function/path | Inclusive share of CPU samples |
| --- | ---: |
| `AlarmLogFile::savePosition` | 74.05% |
| `rename` | 25.91% |
| `QTemporaryFile::open` | 12.35% |
| `QTemporaryFile::fileName` | 10.09% |
| `QTemporaryFile` destruction | 6.90% |
| JSON serialization | 3.03% |
| `AlarmLogFile::includeRecord` (hash updates) | 2.25% |

These are inclusive percentages: child operations overlap their callers and
must not be added. They identify file-position persistence as the main target;
hashing and formatting are smaller contributors.

[Selected profiler output](benchmarks/qtalh-logging-profile-2026-09-11.txt).

## Why the extra metadata exists

QtALH persists the exact ring position, including when several records have the
same timestamp. Its fingerprint rejects stale position metadata after an
external writer, truncation, or interrupted update changes the alarm file.
The earlier JSON implementation used atomic replacement for a complete position
document. The current binary checkpoints validate each slot with a checksum.
Recovery behavior is covered in `qtalh/tests/test_helpers.cc`.

Legacy [`filePrintf`](../alh/alLog.c) maintains a fixed-length circular cursor
in memory and writes and flushes alarm records directly; it does not maintain this
per-record metadata file. QtALH also handles variable-length ring records by
rewriting the ring when a replacement's size changes. Such configurations can
add further I/O, but this did not occur in the fixed-width benchmark.

The primary optimization target was therefore the way QtALH persists circular
position metadata while preserving its recovery behavior. Reducing the number
of alarm records was not necessary to address the measured CPU gap.

## Implemented changes

The shared writer keeps the 192-byte position file open. After each flushed
alarm record, a 96-byte `pwrite` updates the alternate checkpoint slot on Unix
(Windows uses `QFile::seek`/`write`/`flush`). Per-record
temporary-file creation, permission changes, linking, renaming, cleanup, and JSON
serialization are removed. Log timestamps are cached within their existing
one-second precision, including older event times and clock reversals.

Each slot contains an 8-byte ALHPOS01 marker, three big-endian 64-bit integers
(sequence, record count, next circular slot), a 32-byte ring fingerprint, and a
32-byte SHA-256 checksum over the preceding 64 bytes. Sequence parity selects the
physical slot. Recovery selects the newest intact checkpoint matching the log.
Torn, truncated, and stale checkpoints are rejected; a valid other slot is usable.
On Unix, short writes and EINTR are retried. Other errors are reported; a later record can
reopen and repair metadata. Retention and flush-only durability are unchanged.

The earlier experimental QtALH JSON metadata format is unsupported.
Plain ALH logs remain supported. As before, interruption between the separate
log and checkpoint writes can require timestamp recovery when neither slot
matches. Equal timestamps alone cannot reconstruct the exact cursor. This does
not claim an atomic transaction across both files or stronger power-loss durability.

At the 2026-09-11 Linux Qt 5 implementation checkpoint, all 231 checks passed:
92 core, 43 UI, 29 IOC, 63 helpers, and 4 visual checks,
including setup/cleanup and data rows. New tests cover corruption, truncation,
stale fingerprints, invalid cursors, equal timestamps, inode/size reuse, forced
short writes with RLIMIT_FSIZE, recovery after errors, and timestamp boundaries.
Focused Valgrind checks found no memory errors or definite, indirect, or possible leaks.

### Measured result

Fresh X11/local-IOC trials retained the 2,000-record circular limit and all alarm
logging. Each comparison used three 10-second trials, 1,000 channels, nominally
2,000 transitions/second, fresh log files, and alternating execution order.

| Comparison | Baseline CPU | Persistent-checkpoint QtALH CPU |
| --- | ---: | ---: |
| Previous QtALH versus new QtALH | 31.4691% | 3.8978% |
| Legacy ALH versus new QtALH, separate run | 3.0968% | 4.5954% |

The primary before/after comparison shows an 87.6% CPU reduction. Percentages
represent one CPU core and are medians; differences between the two QtALH sample
sets illustrate measurement variability. Every logged trial verified normal and
MAJOR records. See [before/after trials](benchmarks/qtalh-logging-checkpoints-2026-09-11.csv)
and [legacy comparison trials](benchmarks/alh-vs-qtalh-checkpoints-2026-09-11.csv).

A final steady-state trace counted 5,246 alarm writes and exactly 5,246 checkpoint
writes, each 96 bytes, with zero metadata open/close/link/rename/unlink operations
inside the traced window. See [checkpoint syscall counts](benchmarks/qtalh-checkpoint-syscalls-2026-09-11.json).
Checkpoint reopen also avoids reapplying unchanged permissions, allowing an
existing group-writable metadata file to remain usable by another authorized writer.
