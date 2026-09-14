# Read alarm logs

Use historical browsers to find alarm transitions and operator changes across current and dated files.

## Search a time interval

1. Open the alarm-log or operator-log browser from **View**.
2. Set **From** and **To** using local time. The entire final minute is included.
3. Enter optional **With** text. Matching is a case-sensitive substring search.
4. Start **Search**. Use **Stop** to cancel a long-running search.
5. Review results in chronological order and read any search warnings.

The browser searches the selected current log and its `.yyyy-MM-dd` siblings. It recognizes modern ALH, legacy ctime, and XML-like timestamps. The With filter searches complete records, including continuation lines in older multiline logs. Results stop at **100,000 records or 10 MiB**, with an explicit message when truncated. Unreadable files and unrecognized timestamps are reported.

## Choose the right record

| View | Use it for |
| --- | --- |
| Alarm log | Alarm status/severity transitions and global acknowledgement changes. Value-only updates do not create alarm records. |
| Operator log | Operator changes, actions, and runtime errors. This file appends independently of the bounded alarm ring. |
| Current alarm history | The ten most recent in-memory history entries, separate from the files. |
| Live log-file viewer | Viewing the current file separately from a historical search. |

Choose **View → Alarm Log File** or **Operation Log File** for a live tail of up to
**1,000 records or 256 KiB**. The viewer follows changes to the log destination
and daily rollover with `-T`. A valid alarm checkpoint preserves insertion order
after the ring wraps, including records with identical timestamps. Without a
matching checkpoint, the viewer scans the file and retains the newest timestamps
before applying its limits; equal timestamps retain physical order. Use the
historical browser to search older records.

## Read current history

Choose **View → Current Alarm History** for the most recent in-memory entries.
The reading area is resizable and uses a fixed-width font to align the columns.
In a non-Motif style, the [font-size shortcuts](/operate/appearance#adjust-the-font-while-running)
also work while this dialog has focus.

![Fusion current alarm history showing a synthetic HIHI/MAJOR transition](/images/fusion-history.png)

## Understand circular files

The default alarm file holds 2000 records unless `-L` is used, which defaults to unlimited retention as in ALH. An explicit `-m count` overrides either default (`0` means unlimited). After wrapping, physical line order is not chronological. The historical browser sorts recognized timestamps; events sharing one-second timestamps may still have ambiguous relative order.

PV values and descriptions containing line breaks appear as `\n` and `\r` in text logs, or `&#10;` and `&#13;` in XML logs. Each new alarm remains one physical line. Bounded-log recovery also repairs older multiline records when their record boundaries can be recognized.

For retention limits, dated files, checkpoints, destination changes, and multiple writers, see [Logging & shared operation](/configure/logging).
