#include "analytics.h"
#include <QFileInfo>
#include <QRunnable>
#include <QThreadPool>
namespace alh {
AnalyticsService::AnalyticsService(QObject* parent) : QObject(parent) {
  clock.start();
  monotonicNow = [this] { return clock.elapsed(); };
  utcNow = [] { return QDateTime::currentMSecsSinceEpoch(); };
  lastMono = monotonicNow();
  lastUtc = utcNow();
  analytics.reset(lastMono, lastUtc);
  timer.setInterval(100);
  connect(&timer, &QTimer::timeout, this, [this] { poll(); });
  timer.start();
}
AnalyticsService::~AnalyticsService() { detach(); }
void AnalyticsService::detach() {
  if (engine)
    analytics.gap(monotonicNow(), utcNow());
  observer.reset();
  engine = nullptr;
}
void AnalyticsService::attach(Engine* e, const QString& file) {
  detach();
  engine = e;
  QFileInfo info(file);
  auto canonical = file.isEmpty()                       ? QString()
                   : info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                        : info.canonicalFilePath();
  if (canonical != configuration) {
    configuration = canonical;
    analytics.reset(monotonicNow(), utcNow());
  }
  analytics.configuration = configuration;
  QVector<ChannelUpdate> all;
  for (auto n : e->document.channels())
    all << e->channelUpdate(n);
  analytics.reconcile(all, monotonicNow(), utcNow());
  observer = e->observe([this](const AlarmObservation& o) {
    checkClock();
    analytics.observe(o, monotonicNow(), utcNow());
  });
  lastMono = monotonicNow();
  lastUtc = utcNow();
}
void AnalyticsService::reset() {
  analytics.reset(monotonicNow(), utcNow());
  if (!engine)
    return;
  QVector<ChannelUpdate> all;
  for (auto n : engine->document.channels())
    all << engine->channelUpdate(n);
  analytics.reconcile(all, monotonicNow(), utcNow());
  for (const auto& s : all)
    analytics.observe({{}, s, ObservationCause::Suppression}, monotonicNow(), utcNow());
  lastRequest = -1000;
  lastMono = monotonicNow();
  lastUtc = utcNow();
}
void AnalyticsService::checkClock() {
  auto mono = monotonicNow(), utc = utcNow();
  if (mono - lastMono > 5000 || qAbs((utc - lastUtc) - (mono - lastMono)) > 2500)
    analytics.gap(lastMono, lastUtc);
  lastMono = mono;
  lastUtc = utc;
}
void AnalyticsService::poll() {
  checkClock();
  analytics.trim(monotonicNow());
  std::optional<AnalyticsReport> result;
  {
    std::lock_guard<std::mutex> lock(work->mutex);
    if (work->result) {
      result = std::move(work->result);
      work->result.reset();
    }
  }
  if (result && result->generation == analytics.generation() && reportReady)
    reportReady(std::move(*result));
}
void AnalyticsService::request(const AnalyticsQuery& q) {
  auto now = monotonicNow();
  if (now - lastRequest < 1000 || work->busy.exchange(true))
    return;
  lastRequest = now;
  auto snapshot = analytics.snapshot(now, utcNow());
  auto state = work;
  QThreadPool::globalInstance()->start(
      QRunnable::create([state, snapshot = std::move(snapshot), q] {
        auto result = AlarmAnalytics::query(snapshot, q);
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->result = std::move(result);
        }
        state->busy = false;
      }));
}
} // namespace alh
