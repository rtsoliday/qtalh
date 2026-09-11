#pragma once
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <cstdio>
namespace alh {
inline void debugLog(bool enabled, const QString& category, const QString& message) {
  if (!enabled) return;
  // Explicit stderr output also works when Qt routes its own messages to journald.
  const auto line = QString("qtalh debug %1 [%2] %3\n")
                        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), category,
                             QString(message).replace('\n', "\\n")).toLocal8Bit();
  static QMutex mutex;
  QMutexLocker lock(&mutex);
  std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
  std::fflush(stderr);
}
} // namespace alh
