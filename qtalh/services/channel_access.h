// Qt Channel Access adapter. See ../../LICENSE.
#pragma once
#include "core/engine.h"
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <cadef.h>
namespace alh {
class ChannelAccess final : public QObject, public PvService {
public:
  explicit ChannelAccess(EngineOptions options = {}, QObject* parent = nullptr);
  ~ChannelAccess() override;
  std::function<void(const QString&)> error;
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
  void setInitialAckT(const QString&, bool);

private:
  struct Channel {
    ChannelAccess* owner = nullptr;
    const void* tag = nullptr;
    QString name, numericError;
    chid id = nullptr;
    evid subscription = nullptr;
    std::function<void(Event)> alarm;
    std::function<void(double)> numeric;
    std::function<void(QString)> text;
    bool cancelled = false, initialAckWritten = false;
    qint64 connectionDeadline = 0;
    Event last;
    bool haveLast = false;
  };
  EngineOptions options;
  std::vector<std::unique_ptr<Channel>> channels;
  QHash<QString, QVector<Channel*>> channelsByName;
  QHash<int, QSocketNotifier*> sockets;
  QHash<QString, bool> initialAckT;
  QHash<QString, double> pendingSeverity;
  QTimer timer;
  bool active = false, clearing = false;
  Channel* add(const QString&);
  void subscribe(Channel*);
  void report(int, const QString&);
  void numericUnavailable(Channel*, const QString&);
  static void connection(connection_handler_args);
  static void access(access_rights_handler_args);
  static void update(event_handler_args);
  static void fd(void*, int, int);
};
} // namespace alh
