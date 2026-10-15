// Qt pvAccess adapter for "pva://" channels. See ../../LICENSE.
#pragma once
#include "core/engine.h"
#include <QObject>
#include <QTimer>
#include <deque>
#include <memory>
namespace epics { namespace pvAccess { class ChannelProvider; } }
namespace alh {
// Channels named "pva://NAME" use pvAccess; every other name uses Channel Access.
inline bool isPvaName(const QString& name) {
  return name.startsWith(QLatin1String("pva://"), Qt::CaseInsensitive);
}
// PVA alarms carry no ACKS/ACKT, and QSRV rejects writes to those record
// fields. PvAccess keeps acknowledgement state for each "pva://" name in this
// runtime instead, applying the IOC's ACKS/ACKT rules to the monitored alarm.
class PvAccess final : public QObject, public PvService {
public:
  explicit PvAccess(EngineOptions options = {}, QObject* parent = nullptr);
  ~PvAccess() override;
  std::function<void(const QString&)> error;
  std::function<qint64()> monotonicNow = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  void monitor(const QString&, std::function<void(Event)>, const void* owner = nullptr) override;
  void text(const QString&, std::function<void(QString)>) override;
  void number(const QString&, std::function<void(double)>, const void* owner = nullptr) override;
  void cancelNumbers(const void*) override;
  void prepare(const QString&) override;
  bool canWrite(const QString&) const override;
  bool put(const QString&, double, WriteKind = WriteKind::Value) override;
  void cancel(const QString&, const void* owner = nullptr) override;
  void clear() override;
  // One-shot string reads, as ChannelAccess::readStrings.
  void readStrings(const QStringList& names, std::function<void(QHash<QString, QString>)> done,
                   int timeoutMs = 1500);
  // Seed each record's runtime ACKT from the configuration's transient masks.
  void setInitialAckT(const QString&, bool);
  void configureInitialAckT(const Document&);

  // Values extracted on pvAccess threads; delivered on the Qt thread.
  struct Sample {
    int severity = 0, status = 0, alarmStatus = 0;
    QString text, alarmMessage;
    double number = 0;
    bool numeric = false, valueChanged = true;
  };
  struct Inbox;

private:
  struct Operations;
  struct AlarmObserver {
    std::function<void(Event)> callback;
    quint64 revision = 0;
    bool cancelled = false, delivered = false;
  };
  struct Channel {
    quint64 id = 0;
    QString name;
    const void* tag = nullptr;
    std::function<void(Event)> alarm;
    QHash<const void*, AlarmObserver> observers;
    std::function<void(double)> numeric;
    std::function<void(QString)> text;
    bool cancelled = false, prepared = false, connected = false;
    qint64 connectionDeadline = 0;
    QString numericError;
    Event last;
    bool haveLast = false, haveObservation = false;
    std::shared_ptr<Operations> ops;
    bool putBusy = false;
    std::deque<double> puts;
  };
  struct Record {
    int status = 0, severity = 0, acks = 0, ackt = 1, alarmStatus = 0;
    QString alarmMessage;
  };
  struct Read {
    quint64 id = 0;
    QHash<QString, QString> values;
    std::vector<std::shared_ptr<Operations>> items;
    std::function<void(QHash<QString, QString>)> done;
    int pending = 0;
  };
  EngineOptions options;
  std::shared_ptr<Inbox> inbox;
  std::shared_ptr<epics::pvAccess::ChannelProvider> provider;
  std::vector<std::unique_ptr<Channel>> channels;
  QHash<quint64, Channel*> channelsById;
  QHash<QString, QVector<Channel*>> channelsByName;
  QHash<QString, Record> records; // Keyed by the full "pva://" name.
  QHash<QString, double> pendingSeverity;
  QSet<QString> changedRecords;
  std::vector<std::unique_ptr<Read>> reads;
  QHash<quint64, Read*> readsByItem;
  quint64 nextId = 0;
  qint64 nextDeadline = 0;
  QTimer deadlineTimer;
  bool connectProvider();
  Channel* add(const QString&);
  void subscribe(Channel*);
  void ensurePut(Channel*);
  void sendPut(Channel*);
  void destroy(Channel*);
  void report(const QString&);
  void numericUnavailable(Channel*, const QString&);
  void noteDeadline(const Channel*);
  void checkDeadlines();
  void changeRecord(const QString&);
  void publishRecords();
  void deliverAlarm(quint64 id, Event);
  void finishRead(quint64 id);
  void drain();
  void connected(Channel*);
  void disconnected(Channel*);
  void monitorFailed(Channel*, const QString&);
  void sample(Channel*, const Sample&);
};
} // namespace alh
