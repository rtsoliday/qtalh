# QtALH logging implementation and performance

## Measured result

The current [CPU comparison](qtalh-performance.md), measured on 2026-09-14,
uses 1,000 channels, nominally 2,000 normal/MAJOR transitions per second, global
mode, X11 rendering, and five alternating 30-second logged trials per application.
Each trial uses a fresh 2,000-record circular alarm log.

| Application | Median process CPU |
| --- | ---: |
| ALH 1.2.35 | 3.2666% |
| QtALH | 4.7666% |

QtALH uses 45.9% more CPU in this workload, a difference of 1.5000 percentage
points of one core. Every logged trial retained both normal and MAJOR records
for all 1,000 channels. The comparison measures the complete application; it
does not attribute the difference to a particular function or persistence step.
The benchmark page provides raw results, host details, limitations, and reproduction.

## Implemented changes

The shared circular-log writer keeps a 192-byte position file open. After a
flushed alarm record, a 96-byte write updates the alternate checkpoint slot.
Unix uses `pwrite`; Windows uses `QFile::seek`/`write`/`flush`. Log timestamps
are cached within their existing one-second precision, including older event
times and clock reversals.

Each slot contains an eight-byte ALHPOS01 marker, three big-endian 64-bit integers
(sequence, record count, next circular slot), a 32-byte ring fingerprint, and a
32-byte SHA-256 checksum over the preceding 64 bytes. Sequence parity selects
the physical slot. Recovery selects the newest intact checkpoint matching the
log. Torn, truncated, and stale checkpoints are rejected; another valid slot
remains usable. On Unix, short writes and EINTR are retried. Other errors are
reported, and later writes can repair the metadata.

The metadata preserves the exact ring position when several records have the
same timestamp. A stale fingerprint detects changes such as an external writer
or truncation. Plain ALH logs remain supported; equal timestamps alone cannot
reconstruct an exact cursor when no matching checkpoint exists. The experimental
JSON metadata format is unsupported.

Legacy [`filePrintf`](../alh/alLog.c) keeps a fixed-length circular cursor in
memory and writes and flushes alarm records directly. QtALH also handles
variable-length ring records and maintains recovery metadata. These are
implementation differences, not a measured attribution of the current CPU gap.

Flushing does not add a per-record `fsync` or promise stronger power-loss
durability. Log and checkpoint writes are separate operations, not an atomic
transaction across both files. Recovery and failure handling are covered by
[`test_helpers.cc`](../qtalh/tests/test_helpers.cc), including checkpoint
corruption, stale metadata, equal timestamps, and failed writes. See the
[logging guide](qtalh-user-guide.md#logging-and-shared-operation) for retention,
recovery, shared operation, and helper behavior.

## Disabled destinations

`Logging::alarm` skips record formatting when file logging is disabled and
neither printer nor database output is configured. `-D` alone does not disable
helper output. Engine history and alarm observers remain active; configured
outputs use the existing formatting and delivery paths.
