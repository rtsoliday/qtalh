#pragma once
#include "options.h"
#include <QFile>
#include <QObject>
#include <QTimer>
namespace alh {
struct AlarmLogFile;
class Logging : public QObject {
public:
  Logging(Options options, QString facility, QObject* parent = nullptr);
  ~Logging() override;
  std::function<void(const QString&)> error, message;
  std::function<void()> reload;
  void alarm(Node*, const State&, qint64);
  void operation(Node*, const QString&);
  void acknowledgement(Node*);
  bool sendBroadcast(const QString&, int suppressMinutes = 0, bool reloadFacility = false);
  void setAlarmFile(const QString&);
  void setOpmodFile(const QString&);
  QString alarmPath() const;
  QString opmodPath() const;
  bool commandsAllowed() const {
    return QDateTime::currentMSecsSinceEpoch() >= suppressUntil;
  }
  bool isMaster() const {
    return master;
  }

private:
  Options options;
  QString facility, lastBroadcast;
  std::shared_ptr<AlarmLogFile> alarmFile;
  QString openedAlarmPath;
  std::unique_ptr<QFile> opmodFile = std::make_unique<QFile>();
  int lockFd = -1, broadcastFd = -1;
  bool master = true;
  QTimer timer;
  qint64 lastLockCheck = 0, suppressUntil = 0, broadcastUnlock = 0;
  qint64 cachedSecond = 0;
  QString cachedTimestamp;
  QString stamp(qint64);
  void tick();
  void finishBroadcast();
  void openFiles();
  void record(bool, const QString&, qint64, int = 0);
  QString dated(const QString&) const;
};
} // namespace alh
