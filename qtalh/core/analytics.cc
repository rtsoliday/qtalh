#include "analytics.h"
#include <QDateTime>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
namespace alh {
namespace {
int flags(const ChannelUpdate& s) {
  return (s.shelved ? 1 : 0) | (s.disabled ? 2 : 0) | (s.noAck ? 4 : 0) | (s.cancelled ? 8 : 0);
}
bool selected(int f, int filter) { return filter == 0 || (filter == 1 ? f == 0 : f != 0); }
int histogramBin(qint64 ms) {
  const qint64 limits[] = {1000, 5000, 15000, 60000, 300000, 900000, 3600000, 14400000};
  int i = 0;
  while (i < 8 && ms > limits[i])
    ++i;
  return i;
}
void add(AnalyticsStats& a, const AnalyticsStats& b) {
  a.activations += b.activations;
  a.activeMs += b.activeMs;
  a.observedMs += b.observedMs;
  a.ackCount += b.ackCount;
  a.ackSumMs += b.ackSumMs;
  a.ackMaxMs = qMax(a.ackMaxMs, b.ackMaxMs);
  a.lastActivationUtc = qMax(a.lastActivationUtc, b.lastActivationUtc);
  a.gaps += b.gaps;
  a.incomplete += b.incomplete;
  a.peak = qMax(a.peak, b.peak);
  for (int i = 0; i < AnalyticsHistogramBins; ++i)
    a.histogram[i] += b.histogram[i];
}
void interval(AnalyticsStats& s, int severity, qint64 duration) {
  duration = qMax<qint64>(0, duration);
  s.observedMs += duration;
  if (severity > 0) {
    s.activeMs += duration;
    s.peak = qMax(s.peak, severity);
  }
}
void point(AnalyticsStats& s, const AnalyticsRecord& r) {
  if (r.activation) {
    ++s.activations;
    s.lastActivationUtc = r.utc;
  }
  s.peak = qMax(s.peak, r.peak);
  if (r.ackMs >= 0) {
    ++s.ackCount;
    s.ackSumMs += r.ackMs;
    s.ackMaxMs = qMax(s.ackMaxMs, r.ackMs);
    ++s.histogram[histogramBin(r.ackMs)];
  }
  s.gaps += r.gap;
  s.incomplete += r.incomplete;
}
QString stamp(qint64 utc) {
  return utc ? QDateTime::fromMSecsSinceEpoch(utc, QTimeZone(0)).toUTC().toString(Qt::ISODateWithMs)
             : QString();
}
QString number(qint64 ms) { return QString::number(ms / 1000.0, 'f', 3); }
QString field(QString value) {
  int i = 0;
  while (i < value.size() && value[i].isSpace())
    ++i;
  if (i < value.size() && QString("=+-@").contains(value[i]))
    value.prepend('\'');
  value.replace('"', "\"\"");
  return '"' + value + '"';
}
} // namespace
QString analyticsSuppression(int f) {
  QStringList parts;
  if (f & 1)
    parts << "Shelved";
  if (f & 2)
    parts << "Disabled";
  if (f & 4)
    parts << "NoAck";
  if (f & 8)
    parts << "Cancelled";
  return parts.isEmpty() ? "None" : parts.join(", ");
}
QStringList analyticsHistogramLabels() {
  return {"0–1 s",     ">1–5 s",     ">5–15 s", ">15–60 s", ">1–5 min",
          ">5–15 min", ">15–60 min", ">1–4 h",  ">4 h"};
}
AlarmAnalytics::AlarmAnalytics(qint64 now, qint64 utc) { reset(now, utc); }
void AlarmAnalytics::reset(qint64 now, qint64 utc) {
  auto next = data.generation + 1;
  data = {};
  data.generation = next;
  data.session = QUuid::createUuid().toString(QUuid::WithoutBraces);
  data.started = data.coverage = now;
  data.startedUtc = utc;
  index.clear();
}
void AlarmAnalytics::append(int i, AnalyticsRecord r) {
  r.channel = i;
  auto& c = data.channels[i];
  if (r.priorAvailable)
    interval(c.totals[r.priorFlags != 0], r.priorSeverity, r.time - r.from);
  point(c.totals[r.flags != 0], r);
  data.records.push_back(r);
  trim(r.time);
}
void AlarmAnalytics::trim(qint64 now) {
  qint64 cutoff = qMax(data.started, now - AnalyticsDay);
  while (!data.records.empty() &&
         (data.records.front().time < cutoff || recordCount() > maximumRecords ||
          recordBytes() > maximumBytes)) {
    data.coverage = qMax(data.coverage, data.records.front().time + 1);
    data.records.pop_front();
  }
  data.coverage = qMax(data.coverage, cutoff);
}
void AlarmAnalytics::invalidate(int i, qint64 now, qint64 utc) {
  auto& c = data.channels[i];
  if (!c.available)
    return;
  AnalyticsRecord r;
  r.from = c.last;
  r.time = now;
  r.utc = utc;
  r.priorAvailable = true;
  r.priorSeverity = c.severity;
  r.priorFlags = r.flags = c.flags;
  r.gap = true;
  r.incomplete = c.unack > 0;
  append(i, r);
  c.available = false;
  c.ackUnknown = c.standingUnknown = true;
  c.last = now;
  c.lastUtc = utc;
}
void AlarmAnalytics::gap(qint64 now, qint64 utc) {
  for (int i = 0; i < data.channels.size(); ++i)
    invalidate(i, now, utc);
  ++data.generation;
}
void AlarmAnalytics::reconcile(const QVector<ChannelUpdate>& all, qint64 now, qint64 utc) {
  gap(now, utc);
  for (auto& c : data.channels)
    c.removed = true;
  QHash<QString, int> counts;
  for (const auto& s : all)
    ++counts[s.identity];
  QSet<QString> seen;
  for (const auto& s : all) {
    if (seen.contains(s.identity))
      continue;
    seen << s.identity;
    QString key = s.identity;
    bool ambiguous = counts[key] != 1;
    int i = index.value(key, -1);
    if (i >= 0 && ambiguous) {
      data.channels[i].removed = true;
      index.remove(key);
      i = -1;
    }
    if (i < 0) {
      AnalyticsChannel c;
      c.identity = key;
      c.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
      c.path = s.path;
      c.pv = s.pv;
      c.ancestors = s.ancestors;
      c.ambiguous = ambiguous;
      c.last = now;
      c.lastUtc = utc;
      i = data.channels.size();
      data.channels << c;
      if (!ambiguous)
        index[key] = i;
    }
    auto& c = data.channels[i];
    c.removed = false;
    c.available = false;
    c.initialized = false;
    c.flags = flags(s);
    c.ackUnknown = c.standingUnknown = true;
  }
  trim(now);
}
void AlarmAnalytics::observe(const AlarmObservation& o, qint64 now, qint64 utc) {
  const auto& s = o.after;
  int i = index.value(s.identity, -1);
  if (i < 0)
    return;
  auto& c = data.channels[i];
  if (c.removed || c.ambiguous)
    return;
  bool available = s.initialized && s.available && !s.cancelled;
  int f = flags(s);
  bool changed = c.available != available || c.severity != s.severity || c.unack != s.unack ||
                 c.flags != f || !c.initialized;
  if (!changed) {
    c.lastUtc = utc;
    return;
  }
  AnalyticsRecord r;
  r.from = c.last;
  r.time = now;
  r.utc = utc;
  r.priorAvailable = c.available;
  r.priorSeverity = c.severity;
  r.priorFlags = c.flags;
  r.flags = f;
  r.peak = available ? s.severity : 0;
  bool processed = o.cause != ObservationCause::Suppression;
  r.activation = c.available && available && c.severity == 0 && s.severity > 0 && processed;
  bool confirmed = o.cause == ObservationCause::LocalAcknowledgement ||
                   o.cause == ObservationCause::RequestedAcknowledgement ||
                   o.cause == ObservationCause::ExternalAcknowledgement;
  if (c.available && c.unack > 0 && (!available || s.unack == 0)) {
    if (confirmed && available && c.available && !c.ackUnknown)
      r.ackMs = qMax<qint64>(0, now - c.ackSince);
    else
      r.incomplete = true;
  }
  r.gap = s.initialized && !available && (c.available || !c.initialized);
  if (available) {
    if (!c.available) {
      c.standingSince = c.ackSince = now;
      c.standingUnknown = c.ackUnknown = true;
    } else {
      if (c.severity == 0 && s.severity > 0) {
        c.standingSince = now;
        c.standingUnknown = !processed;
      }
      if (c.unack == 0 && s.unack > 0) {
        c.ackSince = now;
        c.ackUnknown = !processed;
      }
    }
  }
  append(i, r);
  c.available = available;
  c.initialized = s.initialized;
  c.severity = s.severity;
  c.unack = s.unack;
  c.flags = f;
  c.last = now;
  c.lastUtc = utc;
}
AnalyticsSnapshot AlarmAnalytics::snapshot(qint64 now, qint64 utc) const {
  auto copy = data;
  copy.configuration = configuration;
  copy.now = now;
  copy.nowUtc = utc;
  return copy;
}
AnalyticsReport AlarmAnalytics::query(const AnalyticsSnapshot& s, const AnalyticsQuery& q) {
  AnalyticsReport out;
  out.configuration = s.configuration;
  out.session = s.session;
  out.generation = s.generation;
  out.query = q;
  out.from = q.rangeMs ? qMax(s.started, s.now - q.rangeMs) : s.started;
  out.to = s.now;
  out.coverage = qMax(out.from, s.coverage);
  out.partial = out.coverage > out.from;
  out.toUtc = s.nowUtc;
  out.fromUtc = s.startedUtc + (out.from - s.started);
  out.coverageUtc = s.startedUtc + (out.coverage - s.started);
  out.trend.fill(0, 120);
  QVector<int> rowIndex(s.channels.size(), -1);
  for (int i = 0; i < s.channels.size(); ++i) {
    const auto& c = s.channels[i];
    if (!q.scope.isEmpty() && !c.ancestors.contains(q.scope))
      continue;
    if (!q.search.isEmpty() && !c.path.contains(q.search, Qt::CaseInsensitive) &&
        !c.pv.contains(q.search, Qt::CaseInsensitive))
      continue;
    AnalyticsRow r;
    r.channel = c;
    if (!q.rangeMs) {
      if (q.suppression != 2)
        add(r.stats, c.totals[0]);
      if (q.suppression != 1)
        add(r.stats, c.totals[1]);
    }
    if (c.available && selected(c.flags, q.suppression)) {
      if (c.severity > 0)
        r.standingMs = qMax<qint64>(0, s.now - c.standingSince);
      if (c.unack > 0)
        r.outstandingMs = qMax<qint64>(0, s.now - c.ackSince);
      interval(r.stats, c.severity, s.now - qMax(c.last, q.rangeMs ? out.coverage : s.started));
    }
    rowIndex[i] = out.rows.size();
    out.rows << r;
  }
  qint64 width = qMax<qint64>(1, out.to - out.coverage);
  for (const auto& rec : s.records) {
    int i = rowIndex.value(rec.channel, -1);
    if (i < 0)
      continue;
    auto& r = out.rows[i];
    if (q.rangeMs && rec.priorAvailable && selected(rec.priorFlags, q.suppression)) {
      qint64 a = qMax(rec.from, out.coverage), b = qMin(rec.time, out.to);
      if (b > a)
        interval(r.stats, rec.priorSeverity, b - a);
    }
    if (rec.time < out.coverage || rec.time > out.to)
      continue;
    if (selected(rec.flags, q.suppression)) {
      if (q.rangeMs)
        point(r.stats, rec);
      if (rec.activation) {
        r.activationTimes << rec.time;
        r.activationUtc << rec.utc;
        int bucket = qBound(0, int((rec.time - out.coverage) * 120 / width), 119);
        ++out.trend[bucket];
      }
    }
  }
  for (auto& r : out.rows) {
    int left = 0;
    for (int right = 0; right < r.activationTimes.size(); ++right) {
      while (r.activationTimes[right] - r.activationTimes[left] > qint64(qMax(1, q.chatterSeconds)) * 1000)
        ++left;
      r.chatterMaximum = qMax(r.chatterMaximum, right - left + 1);
      if (r.activationTimes[right] >= s.now - qint64(qMax(1, q.chatterSeconds)) * 1000)
        ++r.currentActivations;
    }
    r.chattering = r.currentActivations >= q.chatterCount;
    out.totalActivations += r.stats.activations;
    out.gaps += r.stats.gaps;
    out.incomplete += r.stats.incomplete;
    for (int i = 0; i < AnalyticsHistogramBins; ++i)
      out.histogram[i] += r.stats.histogram[i];
  }
  return out;
}
QByteArray analyticsCsv(const AnalyticsReport& r, int tab, bool chart) {
  QByteArray bytes;
  auto line = [&](const QStringList& values) {
    QStringList escaped;
    for (const auto& v : values)
      escaped << field(v);
    bytes += escaped.join(',').toUtf8() + "\r\n";
  };
  line({"QtALH session analytics", "configuration", r.configuration, "session", r.session});
  line({"requested_from_utc", stamp(r.fromUtc), "to_utc", stamp(r.toUtc),
        "detail_coverage_from_utc", stamp(r.coverageUtc), "partial_detail",
        r.partial ? "true" : "false"});
  line({"scope", r.query.scope, "search", r.query.search, "suppression",
        QString::number(r.query.suppression), "chatter_count",
        QString::number(r.query.chatterCount), "chatter_window_seconds",
        QString::number(r.query.chatterSeconds)});
  if (chart && tab == 0) {
    line({"series", "channel_or_interval_start_utc", "activations"});
    auto rows = r.rows;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return a.stats.activations > b.stats.activations;
    });
    int count = 0;
    for (const auto& row : rows) {
      if (!row.stats.activations)
        continue;
      line({"top_offenders", row.channel.path, QString::number(row.stats.activations)});
      if (++count == 10)
        break;
    }
    for (int i = 0; i < r.trend.size(); ++i)
      line({"trend", stamp(r.coverageUtc + (r.to - r.coverage) * i / r.trend.size()),
            QString::number(r.trend[i])});
    return bytes;
  }
  if (chart && tab == 2) {
    line({"path", "observed_standing_seconds", "lower_bound"});
    auto rows = r.rows;
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.standingMs > b.standingMs; });
    int count = 0;
    for (const auto& row : rows) {
      if (row.standingMs < 0)
        continue;
      line({row.channel.path, number(row.standingMs),
            row.channel.standingUnknown ? "true" : "false"});
      if (++count == 10)
        break;
    }
    return bytes;
  }
  if (chart && tab == 3) {
    line({"acknowledgement_time_bin", "completed_samples"});
    auto labels = analyticsHistogramLabels();
    for (int i = 0; i < AnalyticsHistogramBins; ++i)
      line({labels[i], QString::number(r.histogram[i])});
    return bytes;
  }
  if (chart && tab == 1) {
    line({"path", "activation_utc"});
    for (const auto& row : r.rows)
      for (auto time : row.activationUtc)
        line({row.channel.path, stamp(time)});
    return bytes;
  }
  line({"path",
        "channel_instance",
        "pv",
        "activations",
        "activation_share_percent",
        "peak_severity",
        "last_activation_utc",
        "standing_seconds",
        "standing_lower_bound",
        "active_seconds",
        "observed_seconds",
        "ack_samples",
        "ack_mean_seconds",
        "ack_max_seconds",
        "outstanding_seconds",
        "outstanding_incomplete",
        "chatter_maximum",
        "chattering_now",
        "suppression",
        "available",
        "removed",
        "ambiguous",
        "gaps",
        "incomplete_samples",
        "last_observation_utc"});
  for (const auto& row : r.rows) {
    if (tab == 2 && row.standingMs < 0)
      continue;
    const auto& c = row.channel;
    const auto& s = row.stats;
    line({c.path,
          c.key,
          c.pv,
          QString::number(s.activations),
          QString::number(r.totalActivations ? 100.0 * s.activations / r.totalActivations : 0, 'f',
                          2),
          QString::number(s.peak),
          stamp(s.lastActivationUtc),
          row.standingMs < 0 ? QString() : number(row.standingMs),
          c.standingUnknown ? "true" : "false",
          number(s.activeMs),
          number(s.observedMs),
          QString::number(s.ackCount),
          s.ackCount ? number(s.ackSumMs / s.ackCount) : QString(),
          s.ackCount ? number(s.ackMaxMs) : QString(),
          row.outstandingMs < 0 ? QString() : number(row.outstandingMs),
          c.ackUnknown ? "true" : "false",
          QString::number(row.chatterMaximum),
          row.chattering ? "true" : "false",
          analyticsSuppression(c.flags),
          c.available ? "true" : "false",
          c.removed ? "true" : "false",
          c.ambiguous ? "true" : "false",
          QString::number(s.gaps),
          QString::number(s.incomplete),
          stamp(c.lastUtc)});
  }
  return bytes;
}
} // namespace alh
