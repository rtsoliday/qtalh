#include "log_browser.h"
#include "log_identity.h"
#include "log_recovery.h"
#include "log_timestamp.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QtEndian>
#include <QThread>
#include <memory>
#include <map>
#include <algorithm>
namespace alh {
namespace {
QByteArray logCheckpoint(const QString& path) {
  // Canonical paths resolve symlinks, but not hard links. Use the writer's
  // file-identity association so all aliases read the same circular cursor.
  QFile log(path);
  if (!log.open(QIODevice::ReadOnly)) return {};
  QFile file(logIdentity::readableCheckpointPath(log));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 192) return {};
  return file.read(192);
}
struct LogPosition {
  quint64 sequence = 0, count = 0, next = 0;
  QByteArray digest;
};
LogPosition logPosition(const QByteArray& data) {
  LogPosition result;
  for (int i = 0; i < 2; ++i) {
    auto slot = data.mid(i * 96, 96);
    if (slot.size() != 96 || slot.left(8) != "ALHPOS01" ||
        QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256) != slot.mid(64)) continue;
    auto sequence = qFromBigEndian<quint64>(slot.constData() + 8);
    auto count = qFromBigEndian<quint64>(slot.constData() + 16);
    auto next = qFromBigEndian<quint64>(slot.constData() + 24);
    if (sequence > result.sequence && next <= count)
      result = {sequence, count, next, slot.mid(32, 32)};
  }
  return result;
}
// Return the oldest physical record only when a checkpoint matches the entire
// file. Hash logical records in bounded chunks, including legacy continuations.
qint64 searchStart(QFile& input, const std::function<bool()>& cancelled) {
  const auto path = input.fileName();
  const auto checkpoint = logCheckpoint(path);
  const LogPosition positions[] = {logPosition(checkpoint.left(96)), logPosition(checkpoint.mid(96))};
  if (!positions[0].sequence && !positions[1].sequence) return 0;
  const auto size = input.size();
  const auto modified = QFileInfo(input).lastModified();
  qint64 offsets[2]{};
  quint64 count = 0;
  QByteArray digest(32, 0);
  const bool scanned = logRecovery::scan(input, 0, size, false,
      [&](quint64 index, qint64 offset, qint64, const QByteArray&, const QByteArray& hash, QByteArray) {
    for (int i = 0; i < 2; ++i)
      if (index == positions[i].next) offsets[i] = offset;
    for (int i = 0; i < digest.size(); ++i) digest[i] = char(digest.at(i) ^ hash.at(i));
    count = index + 1;
  }, cancelled);
  if (!scanned || (cancelled && cancelled())) return 0;
  if (logCheckpoint(path) != checkpoint || QFileInfo(input).size() != size ||
      QFileInfo(input).lastModified() != modified) return 0;
  quint64 newest = 0;
  qint64 offset = 0;
  for (int i = 0; i < 2; ++i)
    if (positions[i].sequence > newest && positions[i].count == count && positions[i].digest == digest) {
      newest = positions[i].sequence;
      offset = positions[i].next == count ? 0 : offsets[i];
    }
  return offset;
}

}
LogSearchResult searchLogs(const LogSearch& request, const std::function<bool()>& cancelled) {
  LogSearchResult result;
  if (!request.from.isValid() || !request.to.isValid() || request.from > request.to ||
      request.maximumRecords <= 0 || request.maximumBytes <= 0) {
    result.errors << "Invalid search interval or result limit.";
    return result;
  }
  QFileInfo source(request.path);
  QString base = source.fileName();
  static const QRegularExpression suffix("\\.(\\d{4}-\\d{2}-\\d{2})$");
  auto match = suffix.match(base);
  if (match.hasMatch() && QDate::fromString(match.captured(1), "yyyy-MM-dd").isValid())
    base.truncate(match.capturedStart());
  QDir directory(source.absolutePath());
  if (!directory.exists()) {
    result.errors << "Log directory does not exist: " + directory.path();
    return result;
  }
  struct Record { QDateTime time; QString text; };
  QVector<Record> records;
  int bytes = 0;
  QFileInfoList files;
  // The selected file is authoritative, even when hidden or its basename
  // happens to end in a date unrelated to the records being searched.
  // A missing current daily file is normal; still discover its older siblings.
  if (source.exists()) files << source;
  for (const auto& file : directory.entryInfoList(QDir::Files | QDir::Hidden, QDir::Name)) {
    if (cancelled && cancelled()) { result.cancelled = true; return result; }
    if (file.absoluteFilePath() == source.absoluteFilePath()) continue;
    const auto name = file.fileName();
    if (name != base) {
      if (!name.startsWith(base + '.')) continue;
      const auto date = QDate::fromString(name.mid(base.size() + 1), "yyyy-MM-dd");
      if (!date.isValid() || date.toString("yyyy-MM-dd") != name.mid(base.size() + 1) ||
          date < request.from.date() || date > request.to.date()) continue;
    }
    files << file;
  }
  for (const auto& file : files) {
    if (cancelled && cancelled()) { result.cancelled = true; break; }
    QFile input(file.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly)) {
      result.errors << file.fileName() + ": " + input.errorString();
      continue;
    }
    const auto start = searchStart(input, cancelled);
    const qint64 size = input.size();
    const qint64 begins[] = {start, 0}, ends[] = {size, start};
    auto stopped = [&] {
      if (cancelled && cancelled()) result.cancelled = true;
      return result.truncated || result.cancelled;
    };
    // Apply filters to complete logical records; a match may occur only in a
    // continuation. Oversized records are searched in chunks without retaining
    // them, so the result budget also bounds record buffering.
    auto visit = [&](quint64, qint64 offset, qint64 length, const QByteArray& header,
                     const QByteArray&, QByteArray) {
      const auto time = logTimestamp(QString::fromLocal8Bit(header));
      if (!time.isValid()) { ++result.skipped; return; }
      if (time < request.from || time > request.to) return;
      const auto resume = input.pos();
      if (!input.seek(offset)) return;
      QByteArray raw;
      bool matches = request.contains.isEmpty();
      if (length <= request.maximumBytes) {
        raw = input.read(length);
        matches = QString::fromLocal8Bit(raw).contains(request.contains, Qt::CaseSensitive);
      } else {
        const auto needle = request.contains.toLocal8Bit();
        QByteArray overlap;
        for (qint64 remaining = length; remaining > 0 && !matches && !stopped();) {
          const auto chunk = input.read(qMin(qint64(64 * 1024), remaining));
          if (chunk.isEmpty()) break;
          remaining -= chunk.size();
          overlap += chunk;
          matches = overlap.contains(needle);
          overlap = overlap.right(needle.size() - 1);
        }
      }
      input.seek(resume);
      if (!matches || stopped() || input.error() != QFileDevice::NoError) return;
      if (records.size() >= request.maximumRecords || length > request.maximumBytes - bytes) {
        result.truncated = true;
        return;
      }
      const auto text = QString::fromLocal8Bit(raw);
      records.push_back({time, text.endsWith('\n') ? text : text + '\n'});
      bytes += raw.size();
    };
    // Traverse the ring before applying limits; stable_sort preserves insertion
    // order for timestamp ties when the checkpoint is valid.
    for (int range = 0; range < 2 && !stopped(); ++range) {
      if (!logRecovery::scan(input, begins[range], ends[range], false, visit, stopped)) {
        result.errors << file.fileName() + ": " + input.errorString();
        break;
      }
    }
    if (cancelled && cancelled()) result.cancelled = true;
    if (result.truncated || result.cancelled) break;
  }
  std::stable_sort(records.begin(), records.end(),
                   [](const Record& a, const Record& b) { return a.time < b.time; });
  for (const auto& record : records) result.text += record.text;
  result.records = records.size();
  return result;
}
QThread* startLogSearch(QObject* receiver, LogSearch request,
                       std::function<void(const LogSearchResult&)> completed,
                       LogSearchReader reader) {
  auto result = std::make_shared<LogSearchResult>();
  auto job = QThread::create([request = std::move(request), reader = std::move(reader), result] {
    *result = reader(request, [] { return QThread::currentThread()->isInterruptionRequested(); });
  });
  QObject::connect(receiver, &QObject::destroyed, job, [job] { job->requestInterruption(); });
  QObject::connect(job, &QThread::finished, job, &QObject::deleteLater);
  QObject::connect(job, &QThread::finished, receiver,
                   [result, completed = std::move(completed)] { completed(*result); });
  job->start();
  return job;
}

// The writer's checkpoint identifies insertion order even when every record has
// the same timestamp. Validate it against the file before using its ring cursor.
namespace {
struct LogTail {
  QList<QByteArray> rows;
  int bytes = 0;
  bool limited = false;
  void add(QByteArray row) {
    if (row.size() > LiveLogMaximumBytes) { limited = true; return; }
    bytes += row.size(); rows.push_back(std::move(row));
    while (rows.size() > LiveLogMaximumRecords || bytes > LiveLogMaximumBytes) {
      bytes -= rows.front().size(); rows.removeFirst(); limited = true;
    }
  }
};
struct TimestampTail {
  std::map<std::pair<QDateTime, quint64>, QByteArray> rows;
  quint64 ordinal = 0;
  QDateTime recordTime;
  int bytes = 0;
  bool limited = false;
  void add(const QByteArray& row) {
    if (row.size() > LiveLogMaximumBytes) { limited = true; return; }
    const auto time = logTimestamp(QString::fromLocal8Bit(row));
    if (time.isValid()) recordTime = time;
    // Keep legacy continuation lines adjacent to their timestamped header.
    rows.emplace(std::make_pair(recordTime, ordinal++), row);
    bytes += row.size();
    while (rows.size() > LiveLogMaximumRecords || bytes > LiveLogMaximumBytes) {
      bytes -= rows.begin()->second.size(); rows.erase(rows.begin()); limited = true;
    }
  }
};

}
LiveLogUpdate readLiveLog(const LiveLogCursor& previous, const QString& path,
                         const std::function<bool()>& cancelled) {
  LiveLogUpdate result;
  result.cursor = previous.path == path ? previous : LiveLogCursor();
  auto& cursor = result.cursor;
  cursor.path = path;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
  const auto identity = logIdentity::identity(file);
  if (identity.isEmpty() || identity != cursor.identity) {
    // A replacement can retain size, mtime and the tail anchor. No byte offset,
    // partial record or validated ring sequence belongs to the new file.
    cursor = LiveLogCursor();
    cursor.path = path;
    cursor.identity = identity;
  }
  const auto size = file.size();
  const auto modified = QFileInfo(file).lastModified();
  const auto checkpoint = logCheckpoint(path);
  auto position = logPosition(checkpoint);
  auto read = [&](qint64 bytes) {
    auto data = file.read(bytes); result.bytesRead += data.size(); return data;
  };
  bool append = cursor.offset >= 0 && size >= cursor.offset;
  if (append && !cursor.anchor.isEmpty()) {
    file.seek(cursor.offset - cursor.anchor.size());
    append = read(cursor.anchor.size()) == cursor.anchor;
  }
  const auto oldPosition = logPosition(cursor.checkpoint);
  if (append && checkpoint != cursor.checkpoint && position.sequence && oldPosition.sequence &&
      position.count <= oldPosition.count) append = false;
  // In-place writes do not necessarily touch the tail. A ring checkpoint or
  // mtime change at unchanged size requires a new ordered snapshot.
  if (append && size == cursor.offset &&
      (checkpoint != cursor.checkpoint || modified != cursor.modified)) append = false;
  if (append && size == cursor.offset) return result;
  result.reset = !append || size - cursor.offset > LiveLogMaximumBytes;
  LogTail tail;
  const auto previousSequence = cursor.validatedSequence;
  cursor.validatedSequence = 0;
  if (result.reset) {
    const LogPosition positions[] = {logPosition(checkpoint.left(96)), logPosition(checkpoint.mid(96))};
    // Retain bounded tails for each possible cursor while hashing the file once.
    LogTail beforeSlots[2], afterSlots[2];
    TimestampTail chronological;
    cursor.partial.clear();
    QByteArray digest(32, 0);
    quint64 count = 0;
    bool complete = true;
    file.seek(0);
    while (!file.atEnd()) {
      if (cancelled && cancelled()) { result.retry = true; return result; }
      auto row = file.readLine(LiveLogMaximumBytes + 1);
      result.bytesRead += row.size();
      if (row.isEmpty() && file.error() != QFileDevice::NoError) {
        result.error = file.errorString(); return result;
      }
      if (!row.endsWith('\n') && !file.atEnd()) {
        complete = false;
        do { row = file.readLine(LiveLogMaximumBytes + 1); result.bytesRead += row.size(); }
        while (!row.endsWith('\n') && !file.atEnd() && !(cancelled && cancelled()));
        tail.limited = true;
        ++count;
        continue;
      }
      if (!row.endsWith('\n')) { cursor.partial = row; complete = false; break; }
      chronological.add(row);
      auto hash = QCryptographicHash::hash(QByteArray::number(count) + ':' + row, QCryptographicHash::Sha256);
      for (int i = 0; i < digest.size(); ++i) digest[i] = char(digest.at(i) ^ hash.at(i));
      for (int i = 0; i < 2; ++i)
        (count < positions[i].next ? beforeSlots[i] : afterSlots[i]).add(row);
      ++count;
    }
    if (logCheckpoint(path) != checkpoint || QFileInfo(path).size() != size ||
        QFileInfo(path).lastModified() != modified) {
      result.retry = true; return result; // writer changed the snapshot while it was read
    }
    int selected = -1;
    for (int i = 0; i < 2; ++i)
      if (complete && positions[i].sequence && count == positions[i].count &&
          digest == positions[i].digest &&
          (selected < 0 || positions[i].sequence > positions[selected].sequence))
        selected = i;
    auto& before = beforeSlots[selected < 0 ? 0 : selected];
    auto& after = afterSlots[selected < 0 ? 0 : selected];
    if (selected >= 0) {
      position = positions[selected];
      cursor.validatedSequence = position.sequence;
      for (const auto& row : after.rows) tail.add(row);
      for (const auto& row : before.rows) tail.add(row);
      if (cursor.offset >= 0 && previousSequence && position.sequence > previousSequence) {
        // Writer sequences count flushed records, not successful checkpoints;
        // gaps therefore include alarms saved during metadata write failures.
        const auto added = position.sequence - previousSequence;
        if (added <= quint64(tail.rows.size())) {
          while (quint64(tail.rows.size()) > added) tail.rows.removeFirst();
          result.reset = false;
        } else tail.limited = true; // more records arrived than the retained ring contains
      }
    } else {
      // Consider every physical slot before limiting the chronological tail.
      // A legacy ring's newest records can be near the beginning of the file.
      for (const auto& entry : chronological.rows) tail.add(entry.second);
      tail.limited |= chronological.limited;
    }
    tail.limited |= before.limited || after.limited;
    cursor.offset = file.pos();
  } else {
    file.seek(cursor.offset);
    auto data = cursor.partial + read(size - cursor.offset);
    auto rows = data.split('\n');
    cursor.partial = rows.takeLast();
    // Do not retain an unbounded unterminated external record between polls.
    if (cursor.partial.size() > LiveLogMaximumBytes) cursor.partial.clear();
    for (auto row : rows) tail.add(row + '\n');
    cursor.offset = file.pos();
  }
  if (cancelled && cancelled()) { result.retry = true; return result; }
  if (file.error() != QFileDevice::NoError) { result.error = file.errorString(); return result; }
  file.seek(qMax(qint64(0), cursor.offset - 128));
  cursor.anchor = read(qMin(qint64(128), cursor.offset));
  cursor.checkpoint = checkpoint;
  cursor.modified = modified;
  result.limited = tail.limited;
  for (auto row : tail.rows) {
    if (row.endsWith('\n')) row.chop(1);
    if (row.endsWith('\r')) row.chop(1);
    result.lines << QString::fromLocal8Bit(row);
  }
  return result;
}
} // namespace alh
