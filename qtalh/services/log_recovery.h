#pragma once
// Streaming recovery for the legacy circular log. This reader owns its QFile;
// large scans run on a worker while the GUI spools new alarms separately.
#include "log_identity.h"
#include "log_timestamp.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QtEndian>
#include <deque>
#include <functional>
#include <limits>

namespace alh {
namespace logRecovery {
inline bool recordStart(const QByteArray& line) {
  static const QRegularExpression header(
      "^(?:[0-9]{1,2}-[A-Za-z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} : |"
      "[A-Za-z]{3} [A-Za-z]{3} +[0-9]{1,2} [0-9]{2}:[0-9]{2}:[0-9]{2} [0-9]{4}|"
      "<entry><date>)");
  return header.match(QString::fromLatin1(line.left(64))).hasMatch();
}
struct Result {
  std::deque<QByteArray> records;
  quint64 sequence = 0;
  int nextSlot = 1;
  QString error;
};
struct Slot {
  quint64 sequence = 0, count = 0, next = 0;
  QByteArray digest;
  qint64 offset = 0;
};
inline QByteArray checkpoint(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 192) return {};
  return file.read(192);
}
// Hash logical records without retaining them. The second pass retains only
// the requested tail, so memory depends on retention plus the largest record,
// rather than on the size of an old unlimited log. Chunk physical lines too:
// even a very long legacy continuation need not be buffered during hashing.
inline bool scan(QFile& file, qint64 begin, qint64 end, bool retain,
                 const std::function<void(quint64, qint64, qint64, const QByteArray&,
                                          const QByteArray&, QByteArray)>& visit,
                 const std::function<bool()>& stopped = {}) {
  if (!file.seek(begin)) return false;
  quint64 index = 0;
  qint64 offset = begin;
  bool active = false, lineStart = true, xml = false;
  QByteArray header, raw, ending;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  auto finish = [&](qint64 endOffset) {
    visit(index++, offset, endOffset - offset, header,
          retain ? QByteArray() : hash.result(), std::move(raw));
    raw.clear(); header.clear(); ending.clear(); hash.reset();
  };
  while (file.pos() < end) {
    if (stopped && stopped()) return true;
    const auto start = file.pos();
    const auto chunk = file.readLine(qMin(qint64(64 * 1024), end - start) + 1);
    if (chunk.isEmpty() || file.error() != QFileDevice::NoError) return false;
    if (!active || (lineStart && recordStart(chunk))) {
      if (active) finish(start);
      if (stopped && stopped()) return true;
      active = true; offset = start; xml = chunk.startsWith("<entry>");
      if (!retain) hash.addData(QByteArray::number(index) + ':');
    }
    if (header.size() < 256) header += chunk.left(256 - header.size());
    if (retain) raw += chunk;
    else hash.addData(chunk);
    lineStart = chunk.endsWith('\n');
    // XML has an explicit terminator. Text following it is not a continuation
    // of that entry, and should still be reported as unrecognized by searches.
    if (xml) {
      ending = (ending + chunk).right(10);
      if (ending.endsWith("</entry>\n") || ending.endsWith("</entry>\r\n") ||
          (file.pos() == end && ending.endsWith("</entry>"))) {
        finish(file.pos()); active = false;
      }
    }
  }
  if (active && !(stopped && stopped())) finish(end);
  return file.error() == QFileDevice::NoError;
}
inline Result read(QFile& file, const QString& positionPath,
                   int maximum, quint64 priorSequence, int priorSlot) {
  Result result; result.sequence = priorSequence; result.nextSlot = priorSlot;
  const auto size = file.size();
  const auto revision = logIdentity::revision(file);
  if (revision.isEmpty()) { result.error = "Cannot stat alarm log for recovery"; return result; }
  const auto metadata = checkpoint(positionPath);
  Slot positions[2];
  for (int i = 0; i < 2; ++i) {
    const auto bytes = metadata.mid(i * 96, 96);
    if (bytes.size() != 96 || bytes.left(8) != "ALHPOS01" ||
        QCryptographicHash::hash(bytes.left(64), QCryptographicHash::Sha256) != bytes.mid(64)) continue;
    auto& slot = positions[i];
    slot.sequence = qFromBigEndian<quint64>(bytes.constData() + 8);
    slot.count = qFromBigEndian<quint64>(bytes.constData() + 16);
    slot.next = qFromBigEndian<quint64>(bytes.constData() + 24);
    slot.digest = bytes.mid(32, 32);
    if (slot.next > slot.count) { slot = {}; continue; }
    if (slot.sequence > result.sequence) {
      result.sequence = slot.sequence; result.nextSlot = 1 - i;
    }
  }
  // Legacy/interrupted logs may lack a matching checkpoint. Fall back to the
  // oldest timestamp; equal timestamps alone cannot reveal circular order.
  quint64 count = 0;
  QByteArray digest(32, 0);
  QDateTime earliest;
  qint64 oldest = 0;
  const bool scanned = scan(file, 0, size, false,
      [&](quint64 index, qint64 offset, qint64, const QByteArray& header, const QByteArray& hash, QByteArray) {
    count = index + 1;
    for (auto& slot : positions) if (index == slot.next) slot.offset = offset;
    for (int i = 0; i < 32; ++i) digest[i] = char(digest.at(i) ^ hash.at(i));
    const auto time = logTimestamp(QString::fromLocal8Bit(header));
    if (time.isValid() && (!earliest.isValid() || time < earliest)) {
      earliest = time; oldest = offset;
    }
  });
  if (!scanned) { result.error = "Cannot read alarm log for recovery: " + file.errorString(); return result; }
  int selected = -1;
  for (int i = 0; i < 2; ++i)
    if (positions[i].sequence && positions[i].count == count && positions[i].digest == digest &&
        (selected < 0 || positions[i].sequence > positions[selected].sequence)) selected = i;
  if (selected >= 0) {
    oldest = positions[selected].next == count ? 0 : positions[selected].offset;
    // Preserve the matching slot even when a newer slot is structurally valid
    // but stale. A failed next write must not destroy the recoverable cursor.
    result.nextSlot = 1 - selected;
  } else if (result.sequence) {
    result.sequence += qMin(count + 1, std::numeric_limits<quint64>::max() - result.sequence);
  }
  auto retain = [&](quint64, qint64, qint64, const QByteArray&, const QByteArray&, QByteArray raw) {
    if (raw.endsWith('\n')) raw.chop(1);
    const bool xml = raw.startsWith("<entry>");
    raw.replace("\r", xml ? "&#13;" : "\\r");
    raw.replace("\n", xml ? "&#10;" : "\\n");
    raw += '\n';
    result.records.push_back(std::move(raw));
    if (int(result.records.size()) > maximum) result.records.pop_front();
  };
  if (!scan(file, oldest, size, true, retain) || !scan(file, 0, oldest, true, retain))
    result.error = "Cannot read alarm log for recovery: " + file.errorString();
  // Renaming/replacing the path is harmless: the scan owns a handle to the
  // original file. Reject actual content changes, using handle-based metadata.
  if (logIdentity::revision(file) != revision || checkpoint(positionPath) != metadata)
    result.error = "Alarm log changed during recovery";
  return result;
}
// Path-based entry point for offline recovery; live writers pass an already
// open handle so rotation cannot redirect or strand their scans.
inline Result read(const QString& path, const QByteArray& identity, const QString& positionPath,
                   int maximum, quint64 priorSequence, int priorSlot) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || logIdentity::identity(file) != identity) {
    Result result; result.error = "Cannot open original alarm log for recovery: " + path; return result;
  }
  return read(file, positionPath, maximum, priorSequence, priorSlot);
}
} // namespace logRecovery
} // namespace alh
