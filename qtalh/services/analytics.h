#pragma once
#include "core/analytics.h"
#include <QSet>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <optional>
namespace alh {
class AnalyticsService : public QObject {
public:
  explicit AnalyticsService(QObject* parent = nullptr);
  ~AnalyticsService() override;
  AlarmAnalytics analytics;
  std::function<qint64()> monotonicNow, utcNow;
  std::function<void(AnalyticsReport)> reportReady;
  void attach(Engine*, const QString& configuration);
  void detach();
  void reset();
  void poll();
  void request(const AnalyticsQuery&);

private:
  static constexpr int IdlePoll = 1000, ReportPoll = 100;
  void checkClock();
  struct Work {
    std::atomic<bool> busy{false};
    std::mutex mutex;
    std::optional<AnalyticsReport> result;
  };
  std::shared_ptr<Work> work = std::make_shared<Work>();
  QElapsedTimer clock;
  QTimer timer;
  Engine* engine = nullptr;
  AlarmSubscription observer;
  QString configuration;
  // Identities of channels whose configured mask has Disable or Cancel.
  QSet<QString> excluded;
  QVector<ChannelUpdate> included() const;
  qint64 lastMono = 0, lastUtc = 0, lastRequest = -1000;
};
} // namespace alh
