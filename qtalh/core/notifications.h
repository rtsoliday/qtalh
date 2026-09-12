// Personal notification policy. No network, process, or widget dependencies.
#pragma once
#include "engine.h"
#include <QJsonObject>
#include <QRegularExpression>
namespace alh {
struct NotificationDestination {
  QString id, name, kind = "email", program, sender, url, urlEnvironment, bearerEnvironment;
  QStringList arguments = {"-i", "-t"}, recipients;
};
struct NotificationStage {
  QString id;
  int delaySeconds = 60;
  QStringList destinations;
};
struct NotificationSubscription {
  QString id, name, configuration, scope, wildcard = "*";
  int minimumSeverity = 2, cooldownSeconds = 1800;
  bool enabled = true, resolution = false;
  QVector<NotificationStage> stages;
};
struct NotificationSettings {
  QVector<NotificationDestination> destinations;
  QVector<NotificationSubscription> subscriptions;
};
QString notificationId();
QString notificationConfiguration(const QString&);
QJsonObject notificationJson(const NotificationSettings&);
NotificationSettings parseNotifications(const QByteArray&);
void validateNotifications(const NotificationSettings&);
struct NotificationAlarm {
  QString episode;
  ChannelUpdate state;
};
struct NotificationEnvelope {
  QString id, subscription, subscriptionName, configuration, stage, destination, kind = "alarm";
  QVector<NotificationAlarm> alarms;
  int attempts = 0;
  qint64 ready = 0;
  bool test = false;
};
QByteArray notificationPayload(const NotificationEnvelope&);
QString notificationText(const NotificationEnvelope&);
class NotificationPolicy {
public:
  std::function<void(const QString&)> activity;
  void configure(const NotificationSettings&, const QString& configuration);
  void enable(bool);
  bool enabled() const { return enabled_; }
  void observe(const ChannelUpdate&, qint64 monotonicNow);
  void reconcile(const QVector<ChannelUpdate>&); // Successful reload, await fresh observations.
  void tick(qint64 monotonicNow, int queueLimit = 1000);
  bool take(NotificationEnvelope&, qint64 monotonicNow);
  bool revalidate(NotificationEnvelope&);
  void attempted(const NotificationEnvelope&, qint64 monotonicNow);
  void completed(const NotificationEnvelope&, bool accepted);
  void retry(NotificationEnvelope);
  QSet<QString> activeChannels() const;
  int pending() const { return queue.size(); }
  bool throttled() const { return throttled_; }
  bool destinationReady(const QString& id, qint64 now) const {
    return destinationNext.value(id, 0) <= now;
  }
  const NotificationSettings& settings() const { return settings_; }
  int matchCount(const NotificationSubscription&, const QVector<ChannelUpdate>&) const;

private:
  struct Episode {
    QString id, subscription;
    ChannelUpdate state;
    qint64 started = 0;
    bool active = true, resolved = false, waiting = false;
    QSet<QString> queued, done, accepted, resolutionDone, resolutionQueued;
  };
  NotificationSettings settings_;
  QString configuration_;
  bool enabled_ = false, throttled_ = false;
  QHash<QString, ChannelUpdate> channels;
  QHash<QString, Episode> episodes;
  QHash<QString, QString> current;
  QHash<QString, qint64> cooldown, batchDeadline, destinationNext;
  QVector<NotificationEnvelope> queue;
  QHash<QString, QVector<QString>> matches;
  const NotificationSubscription* subscription(const QString&) const;
  bool match(const NotificationSubscription&, const ChannelUpdate&) const;
  void end(Episode&, bool resolved);
  void collect();
};
} // namespace alh
