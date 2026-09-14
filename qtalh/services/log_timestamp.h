#pragma once
#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>

namespace alh {
// Browsing and circular-log recovery must recognize the same ALH timestamps.
inline QDateTime logTimestamp(const QString& line) {
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
} // namespace alh
