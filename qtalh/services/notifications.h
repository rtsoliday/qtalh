#pragma once
#include "core/notifications.h"
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QTimer>
namespace alh {
// Personal settings are atomically replaced under a lock with optimistic conflict detection.
class NotificationStore {
public:
  explicit NotificationStore(QString path = {});
  NotificationSettings load();
  void save(const NotificationSettings&);
  QString path() const { return path_; }

private:
  QString path_;
  QByteArray revision;
  bool loaded = false;
};
class NotificationService : public QObject {
public:
  explicit NotificationService(QObject* parent = nullptr, QString settingsPath = {});
  ~NotificationService() override;
  NotificationStore store;
  NotificationPolicy policy;
  QStringList activity;
  QString settingsError;
  std::function<void(const QString&)> operation;
  // Injectable clock and timeout for isolated transport tests.
  std::function<qint64()> monotonicNow;
  int transportTimeoutMs = 30000;
  void attach(Engine*, const QVector<Node*>&, const QString& configuration);
  void detach();
  void enable(bool);
  void apply(const NotificationSettings&);
  void reload();
  void sendTest(const QString& destination);
  void pump();
  QString status() const;
  QVector<ChannelUpdate> channels() const;
  static QByteArray mailMessage(const NotificationDestination&, const NotificationEnvelope&);
  static QString validateWebhook(const NotificationDestination&);

private:
  QElapsedTimer clock;
  QTimer timer;
  QNetworkAccessManager network;
  Engine* engine = nullptr;
  AlarmSubscription observer;
  QVector<Node*> nodes;
  QString configuration;
  QSet<QString> unique;
  int inFlight = 0, failed = 0;
  void watch();
  void record(const QString&);
  void dispatch(NotificationEnvelope);
  void finish(NotificationEnvelope, bool accepted, bool retryable, const QString&,
              int retryAfter = 0);
};
} // namespace alh
