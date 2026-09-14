#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QStringList>
#include <functional>
class QObject;
class QThread;
namespace alh {
struct LogSearch {
  QString path, contains;
  QDateTime from, to;
  int maximumRecords = 100000;
  int maximumBytes = 10 * 1024 * 1024;
};
struct LogSearchResult {
  QString text;
  QStringList errors;
  int records = 0, skipped = 0;
  bool truncated = false, cancelled = false;
};
// Reads the named log and its .yyyy-MM-dd siblings, without modifying any file.
LogSearchResult searchLogs(const LogSearch&, const std::function<bool()>& cancelled = {});
using LogSearchReader = std::function<LogSearchResult(const LogSearch&, const std::function<bool()>&)>;
// The worker outlives the receiver if I/O stalls. Destroying the receiver requests
// cancellation and disconnects delivery; completion always releases the worker.
QThread* startLogSearch(QObject* receiver, LogSearch,
                       std::function<void(const LogSearchResult&)> completed,
                       LogSearchReader reader = searchLogs);

// Each poll runs in a worker. The GUI retains only this cursor and a bounded tail.
constexpr int LiveLogMaximumRecords = 1000;
constexpr int LiveLogMaximumBytes = 256 * 1024;
struct LiveLogCursor {
  QString path;
  qint64 offset = -1;
  QDateTime modified;
  QByteArray identity, anchor, checkpoint, partial;
  // Only a checkpoint matched against the full log can anchor ring deltas.
  quint64 validatedSequence = 0;
};
struct LiveLogUpdate {
  LiveLogCursor cursor;
  QStringList lines;
  QString error;
  bool reset = false, limited = false, retry = false;
  qint64 bytesRead = 0;
};
LiveLogUpdate readLiveLog(const LiveLogCursor&, const QString& path,
                         const std::function<bool()>& cancelled = {});
} // namespace alh
