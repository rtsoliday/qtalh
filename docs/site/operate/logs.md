# Read alarm logs

Use historical browsers to find alarm transitions and operator changes across current and dated files.

## Search a time interval

1. Open the alarm-log or operator-log browser from **View**.
2. Set **From** and **To** using local time. The entire final minute is included.
3. Enter optional **With** text. Matching is a case-sensitive substring search.
4. Start **Search**. Use **Stop** to cancel a long-running search.
5. Review results in chronological order and read any search warnings.

The browser searches the selected current log and its `.yyyy-MM-dd` siblings. It recognizes modern ALH, legacy ctime, and XML-like timestamps. Results stop at **100,000 records or 10 MiB**, with an explicit message when truncated. Unreadable files and unrecognized timestamps are reported.

## Choose the right record

| View | Use it for |
| --- | --- |
| Alarm log | Alarm status/severity transitions and global acknowledgement changes. Value-only updates do not create alarm records. |
| Operator log | Operator changes and actions. This file appends independently of the bounded alarm ring. |
| Current alarm history | The ten most recent in-memory history entries, separate from the files. |
| Live log-file viewer | Viewing the current file separately from a historical search. |

## Understand circular files

The default alarm file holds 2000 records. After wrapping, physical line order is not chronological. The historical browser sorts recognized timestamps; events sharing one-second timestamps may still have ambiguous relative order.

For retention limits, dated files, checkpoints, destination changes, and multiple writers, see [Logging & shared operation](/configure/logging).
