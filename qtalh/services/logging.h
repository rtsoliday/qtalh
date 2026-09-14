#pragma once
#include "options.h"
#include <QFile>
#include <QObject>
#include <QTimer>
#include <QMap>
#include <QVector>
#include <chrono>
namespace alh {
struct AlarmLogFile;
struct LoggingClock {
  std::function<qint64()> wall = [] { return QDateTime::currentMSecsSinceEpoch(); };
  std::function<qint64()> monotonic = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
};
class Logging : public QObject {
public:
  Logging(Options options, QString facility, QObject* parent = nullptr, LoggingClock clock = {});
  ~Logging() override;
  std::function<void(const QString&)> error, message;
  std::function<void()> reload;
  void alarm(Node*, const State&, qint64);
  void operation(Node*, const QString&, OperationKind = OperationKind::Other);
  void acknowledgement(Node*);
  // Normal runtime exit waits for the legacy delivery interval. Destruction
  // during window/application teardown still preserves the pending payload.
  bool broadcastPending() const { return broadcastUnlock && clock.monotonic() < broadcastUnlock; }
  bool sendBroadcast(const QString&, int suppressMinutes = 0, bool reloadFacility = false);
  void setAlarmFile(const QString&);
  void setOpmodFile(const QString&);
  // Update validated reload metadata without reopening files or releasing locks.
  void setFacility(const QString& identifier, const QString& label);
  void setFacilityLabel(const QString& label) { facilityLabel = label; }
  // Setters and file choosers use basenames; resolved paths include today's
  // suffix under -T and must not be fed back into the setters unchanged.
  QString alarmBasePath() const { return options.alarmFile; }
  QString opmodBasePath() const { return options.opmodFile; }
  QString alarmPath() const;
  QString opmodPath() const;
  bool commandsAllowed() const {
    return !suppressUntil || clock.monotonic() >= suppressUntil;
  }
  bool isMaster() const {
    return master;
  }

private:
  Options options;
  QString facility, facilityLabel, lastBroadcast;
  struct QueueLoss {
    quint64 count = 0;
    qint64 first = 0, last = 0;
  };
  // Separate destinations and facility identities: neither an unrelated queue's
  // recovery nor a root rename may consume or mislabel another loss interval.
  QMap<QString, QueueLoss> printerLosses, databaseLosses;
  void deliverQueue(bool printer, const QByteArray&);
  bool reportQueueLoss(bool printer);
  QByteArray queueLossRecord(bool printer, const QString&, const QueueLoss&);
  LoggingClock clock;
  std::shared_ptr<AlarmLogFile> alarmFile;
  // Remember queued dated/previous destinations until teardown has flushed them,
  // even if the user selects another file while its recovery worker is running.
  QVector<std::weak_ptr<AlarmLogFile>> pendingAlarmFiles;
  QString openedAlarmPath;
  std::unique_ptr<QFile> opmodFile = std::make_unique<QFile>();
  int lockFd = -1, broadcastFd = -1;
  bool master = true;
  QTimer timer;
  // Interval deadlines use monotonic time; timestamps and message IDs use wall time.
  qint64 nextLockCheck = 0, suppressUntil = 0, broadcastUnlock = 0;
  qint64 cachedSecond = 0;
  QString cachedTimestamp;
  QString stamp(qint64);
  void tick(bool receiveBroadcast = true);
  void reportOpenError(bool alarm, const QString&);
  QString alarmOpenError, opmodOpenError;
  bool ensureBroadcastLock();
  QString broadcastOpenError;
  void finishBroadcast();
  void openFiles();
  void record(bool, const QString&, qint64, int = 0);
  QString dated(const QString&) const;
};
} // namespace alh
