// Qt Channel Access adapter. See ../../LICENSE.
#pragma once
#include "core/engine.h"
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <cadef.h>
#include <mutex>
namespace alh {
class ChannelAccess final : public QObject, public PvService {
public:
  explicit ChannelAccess(EngineOptions options = {}, QObject* parent = nullptr);
  ~ChannelAccess() override;
  std::function<void(const QString&)> error;
  // Connection timeouts are intervals, independent of event wall timestamps.
  std::function<qint64()> monotonicNow = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  // Repeated requests for one owner/PV replace its callback and reuse its monitor.
  void monitor(const QString&, std::function<void(Event)>, const void* owner = nullptr) override;
  void text(const QString&, std::function<void(QString)>) override;
  void number(const QString&, std::function<void(double)>, const void* owner = nullptr) override;
  void cancelNumbers(const void*) override;
  void prepare(const QString&) override;
  bool canWrite(const QString&) const override;
  bool put(const QString&, double, WriteKind = WriteKind::Value) override;
  void cancel(const QString&, const void* owner = nullptr) override;
  void clear() override;
  void poll();
  // One-shot DBR_STRING reads for display-only details such as alarm limits.
  // Each name maps to its value; a missing field, unreachable PV or timeout
  // leaves the name out. No diagnostics are reported.
  void readStrings(const QStringList& names, std::function<void(QHash<QString, QString>)> done,
                   int timeoutMs = 1500);
  void setInitialAckT(const QString&, bool);
  // Configure before connecting. All fields of a record share ALH's
  // subgroup-first startup precedence and one successful ACKT submission.
  void configureInitialAckT(const Document&);

private:
  friend struct ChannelAccessTestAccess;
  struct Channel {
    ChannelAccess* owner = nullptr;
    // Identifies the channel in queued callbacks; epoch changes when its
    // subscription is cleared, so queued events from that subscription are dropped.
    quint64 serial = 0, epoch = 0;
    const void* tag = nullptr;
    QString name, numericError;
    chid id = nullptr;
    evid subscription = nullptr;
    std::function<void(Event)> alarm;
    // Force PV and CALC inputs of every owner reading this PV share the
    // channel and its monitor. An owner of null always adds another input.
    struct Number {
      const void* tag;
      std::function<void(double)> callback;
      quint64 id; // Unique per input, as owners may be null.
    };
    std::vector<Number> numbers;
    // The latest value, for an input added after the monitor's first event.
    double number = 0;
    bool haveNumber = false;
    std::function<void(QString)> text;
    bool cancelled = false, prepared = false;
    bool recheckAccess = false;
    qint64 connectionDeadline = 0;
    Event last;
    bool haveLast = false;
  };
  EngineOptions options;
  std::vector<std::unique_ptr<Channel>> channels;
  QHash<QString, QVector<Channel*>> channelsByName;
  QHash<quint64, Channel*> channelsBySerial;
  quint64 nextSerial = 0;
  QHash<int, QSocketNotifier*> sockets;
  QHash<QString, bool> initialAckT; // Keyed by record name.
  QSet<QString> initialAckTargets; // Configured CA names, retaining their fields.
  QSet<QString> initialAckWritten;
  QHash<QString, double> pendingSeverity;
  QSet<QString> writeUnavailable; // Reported once until writable or a write succeeds.
  QTimer timer;
  bool active = false, clearing = false;
  // poll() scans channels only for pending access rechecks or connection deadlines.
  bool recheckPending = false;
  qint64 nextDeadline = 0;
  void noteDeadline(const Channel*);
  void forget(Channel*);
  Channel* add(const QString&);
  void subscribe(Channel*);
  void report(int, const QString&);
  void numericUnavailable(Channel*, const QString&);
  void deliverNumber(Channel*, double);
  void replayNumber(Channel*, quint64 input);
  struct Read;
  struct ReadItem {
    ChannelAccess* owner = nullptr;
    Read* read = nullptr;
    QString name;
    chid id = nullptr;
    bool requested = false;
  };
  struct Read {
    QHash<QString, QString> values;
    std::vector<std::unique_ptr<ReadItem>> items;
    std::function<void(QHash<QString, QString>)> done;
    int pending = 0;
    quint64 id = 0;
  };
  std::vector<std::unique_ptr<Read>> reads;
  quint64 nextRead = 0;
  void finishRead(quint64 id);
  // Non-preemptive CA runs callbacks on its own threads while poll() waits in
  // ca_poll(). They only copy their arguments here; dispatch() applies them on
  // the Qt thread after ca_poll() returns, so Engine and UI code never run on CA threads.
  struct Callback {
    // NumberReplay gives a channel's latest value to a newly added input.
    enum Kind { Connection, Access, Update, ReadConnection, ReadValue, NumberReplay } kind = Connection;
    quint64 channel = 0, epoch = 0, read = 0;
    ReadItem* item = nullptr;
    quint64 input = 0;
    long op = 0;
    int status = 0;
    long type = 0;
    bool data = false;
    Event event;
    double number = 0;
    QString text;
  };
  std::mutex callbackLock;
  std::vector<Callback> callbacks;
  void defer(Callback);
  void dispatch();
  void connected(Channel*, long op);
  void rightsChanged(Channel*);
  void updated(Channel*, const Callback&);
  void readConnected(ReadItem*, long op);
  void readCompleted(ReadItem*, const Callback&);
  static void readConnection(connection_handler_args);
  static void readValue(event_handler_args);
  static void connection(connection_handler_args);
  static void access(access_rights_handler_args);
  static void update(event_handler_args);
  static void fd(void*, int, int);
};
} // namespace alh
