#include "log_browser.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>
namespace alh {
namespace {
QDateTime timestamp(const QString& line) {
  static const QRegularExpression modern("^(\\d{1,2}-[A-Za-z]{3}-\\d{4} \\d{2}:\\d{2}:\\d{2})");
  static const QRegularExpression old("^([A-Za-z]{3} [A-Za-z]{3} +\\d{1,2} \\d{2}:\\d{2}:\\d{2} \\d{4})");
  static const QRegularExpression xml("^<entry><date>([^<]+)</date>\\s*<time>([^<]+)</time>");
  auto match = modern.match(line);
  if (match.hasMatch())
    return QLocale::c().toDateTime(match.captured(1), "d-MMM-yyyy HH:mm:ss");
  match = old.match(line);
  if (match.hasMatch())
    return QLocale::c().toDateTime(match.captured(1).simplified(), "ddd MMM d HH:mm:ss yyyy");
  match = xml.match(line);
  if (match.hasMatch())
    return QLocale::c().toDateTime(match.captured(1) + ' ' + match.captured(2),
                                  "d-MMM-yyyy HH:mm:ss");
  return {};
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
  for (const auto& file : directory.entryInfoList(QDir::Files, QDir::Name)) {
    if (cancelled && cancelled()) { result.cancelled = true; break; }
    const auto name = file.fileName();
    if (name != base) {
      if (!name.startsWith(base + '.')) continue;
      const auto date = QDate::fromString(name.mid(base.size() + 1), "yyyy-MM-dd");
      if (!date.isValid() || date.toString("yyyy-MM-dd") != name.mid(base.size() + 1) ||
          date < request.from.date() || date > request.to.date()) continue;
    }
    QFile input(file.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly)) {
      result.errors << file.fileName() + ": " + input.errorString();
      continue;
    }
    while (!input.atEnd()) {
      if (cancelled && cancelled()) { result.cancelled = true; break; }
      const auto raw = input.readLine(1024 * 1024);
      if (raw.isEmpty() && input.error() != QFileDevice::NoError) {
        result.errors << file.fileName() + ": " + input.errorString();
        break;
      }
      const auto line = QString::fromLocal8Bit(raw);
      const auto time = timestamp(line);
      if (!time.isValid()) { ++result.skipped; continue; }
      if (time < request.from || time > request.to ||
          !line.contains(request.contains, Qt::CaseSensitive)) continue;
      if (records.size() >= request.maximumRecords || bytes + raw.size() > request.maximumBytes) {
        result.truncated = true;
        break;
      }
      records.push_back({time, line.endsWith('\n') ? line : line + '\n'});
      bytes += raw.size();
    }
    if (result.truncated || result.cancelled) break;
  }
  std::stable_sort(records.begin(), records.end(),
                   [](const Record& a, const Record& b) { return a.time < b.time; });
  for (const auto& record : records) result.text += record.text;
  result.records = records.size();
  return result;
}
} // namespace alh
