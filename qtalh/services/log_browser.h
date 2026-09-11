#pragma once
#include <QDateTime>
#include <QStringList>
#include <functional>
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
} // namespace alh
