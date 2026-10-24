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
  // Replaces the personal settings; the configuration file's are kept.
  void apply(const NotificationSettings&);
  void reload();
  // Settings defined by the open configuration file's $NOTIFY directives. They
  // apply with the personal ones but are never saved to the personal store.
  void setConfiguration(const Document&);
  const NotificationSettings& personal() const { return personal_; }
  const NotificationSettings& configured() const { return configured_; }
  void sendTest(const QString& destination);
  void pump();
  QString status() const;
  // The canonical configuration this runtime's subscriptions belong to.
  QString configurationPath() const { return configuration; }
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
  NotificationSettings personal_, configured_;
  NotificationSettings merged() const;
  int inFlight = 0, failed = 0;
  void watch();
  // The pump runs only while notifications are enabled or deliveries remain;
  // a paused runtime (the startup state) has no periodic wakeups.
  void schedulePump();
  void record(const QString&);
  void dispatch(NotificationEnvelope);
  void finish(NotificationEnvelope, bool accepted, bool retryable, const QString&,
              int retryAfter = 0);
};
} // namespace alh
