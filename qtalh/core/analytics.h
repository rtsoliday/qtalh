#pragma once
#include "engine.h"
#include <deque>
namespace alh {
constexpr qint64 AnalyticsDay = 86400000;
constexpr int AnalyticsHistogramBins = 9;
struct AnalyticsStats {
  qint64 activations = 0, activeMs = 0, observedMs = 0, ackCount = 0, ackSumMs = 0, ackMaxMs = 0;
  qint64 lastActivationUtc = 0, gaps = 0, incomplete = 0;
  int peak = 0;
  std::array<qint64, AnalyticsHistogramBins> histogram{};
};
struct AnalyticsChannel {
  QString identity, key, path, pv;
  QStringList ancestors;
  bool removed = false, ambiguous = false, available = false, initialized = false;
  bool standingUnknown = true, ackUnknown = true;
  int severity = 0, unack = 0, flags = 0;
  qint64 last = 0, lastUtc = 0, standingSince = 0, ackSince = 0;
  std::array<AnalyticsStats, 2> totals;
};
// Interval [from,time) belongs to the state before this transition. Point events
// belong to time. No PV strings or values are duplicated in the history buffer.
struct AnalyticsRecord {
  int channel = 0, priorSeverity = 0, priorFlags = 0, flags = 0, peak = 0;
  qint64 from = 0, time = 0, utc = 0, ackMs = -1;
  bool priorAvailable = false, activation = false, gap = false, incomplete = false;
};
struct AnalyticsSnapshot {
  QString configuration, session;
  quint64 generation = 0;
  qint64 started = 0, startedUtc = 0, now = 0, nowUtc = 0, coverage = 0;
  QVector<AnalyticsChannel> channels;
  std::deque<AnalyticsRecord> records;
};
struct AnalyticsQuery {
  qint64 rangeMs = 3600000; // zero means session summaries
  QString scope, search;
  int suppression = 0; // 0 all, 1 unsuppressed, 2 suppressed
  int chatterCount = 5, chatterSeconds = 60;
};
struct AnalyticsRow {
  AnalyticsChannel channel;
  AnalyticsStats stats;
  qint64 standingMs = -1, outstandingMs = -1;
  int chatterMaximum = 0, currentActivations = 0;
  bool chattering = false;
  QVector<qint64> activationTimes, activationUtc;
};
struct AnalyticsReport {
  QString configuration, session;
  quint64 generation = 0;
  AnalyticsQuery query;
  qint64 from = 0, to = 0, coverage = 0, fromUtc = 0, toUtc = 0, coverageUtc = 0;
  bool partial = false;
  QVector<AnalyticsRow> rows;
  QVector<qint64> trend;
  std::array<qint64, AnalyticsHistogramBins> histogram{};
  qint64 totalActivations = 0, gaps = 0, incomplete = 0;
};
class AlarmAnalytics {
public:
  explicit AlarmAnalytics(qint64 now = 0, qint64 utc = 0);
  void reset(qint64 now, qint64 utc);
  void reconcile(const QVector<ChannelUpdate>&, qint64 now, qint64 utc);
  void observe(const AlarmObservation&, qint64 now, qint64 utc);
  void gap(qint64 now, qint64 utc);
  void trim(qint64 now);
  AnalyticsSnapshot snapshot(qint64 now, qint64 utc) const;
  static AnalyticsReport query(const AnalyticsSnapshot&, const AnalyticsQuery&);
  int recordCount() const { return int(data.records.size()); }
  size_t recordBytes() const { return data.records.size() * sizeof(AnalyticsRecord); }
  int maximumRecords = 250000;
  size_t maximumBytes = 64 * 1024 * 1024;
  QString configuration;
  quint64 generation() const { return data.generation; }

private:
  AnalyticsSnapshot data;
  QHash<QString, int> index;
  void append(int, AnalyticsRecord);
  void invalidate(int, qint64, qint64);
};
QString analyticsSuppression(int flags);
QStringList analyticsHistogramLabels();
// CSV snapshots include provenance, coverage, units and explicit incomplete flags.
QByteArray analyticsCsv(const AnalyticsReport&, int tab, bool chart = false);
} // namespace alh
