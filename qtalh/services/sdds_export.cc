// Optional read-only SDDS exports. See ../../LICENSE.
#include "sdds_export.h"
#include <stdexcept>
#ifdef QTALH_HAVE_SDDS
#include <SDDS.h>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>
#endif

namespace alh {
bool sddsAvailable() {
#ifdef QTALH_HAVE_SDDS
  return true;
#else
  return false;
#endif
}

#ifdef QTALH_HAVE_SDDS
namespace {
#ifdef SDDS_LONG64
constexpr int countType = SDDS_LONG64;
#else
constexpr int countType = SDDS_STRING;
#endif
const double missing = std::numeric_limits<double>::quiet_NaN();
double timeSeconds(qint64 ms) { return ms ? double(ms) / 1000 : missing; }
double durationSeconds(qint64 ms) { return ms >= 0 ? double(ms) / 1000 : missing; }
QVariant count(qint64 n) { return QVariant::fromValue<qlonglong>(n); }
QByteArray utf8(const QString& text) {
  if (text.contains(QChar(0))) throw std::runtime_error("SDDS strings cannot contain NUL characters");
  return text.toUtf8();
}
[[noreturn]] void failed(const QString& operation) {
  int32_t count = 0;
  char** errors = SDDS_GetErrorMessages(&count, SDDS_ALL_GetErrorMessages);
  QString message = operation;
  if (errors) {
    for (int32_t i = 0; i < count; ++i) {
      message += ": " + QString::fromUtf8(errors[i]); std::free(errors[i]);
    }
    std::free(errors);
  }
  SDDS_ClearErrors();
  throw std::runtime_error(message.toStdString());
}
enum class Kind { Text, Integer, Count, Real };
struct Field { const char* name; Kind kind; const char* units = ""; };
using Row = QVariantList;
struct Table {
  std::vector<Field> fields;
  QVector<Row> rows;
};
struct Output {
  SDDS_DATASET data{};
  bool initialized = false;
  std::vector<std::pair<int32_t, QByteArray>> textParameters;
  bool close() {
    bool ok = true;
    // SDDS_Terminate ignores fclose errors. Detach the ordinary binary stream
    // and check its close before freeing the SDDS page/layout buffers.
    if (data.layout.fp) { ok = std::fclose(data.layout.fp) == 0; data.layout.fp = nullptr; }
    if (initialized) { initialized = false; if (!SDDS_Terminate(&data)) ok = false; }
    return ok;
  }
  ~Output() { close(); }
  void parameter(const char* name, int type, const QString& value, const char* units = "") {
    // Owned writable strings work with both current and older EPICS headers.
    QByteArray n(name), u(units), text = utf8(value);
    auto index = SDDS_DefineParameter(&data, n.data(), nullptr, u.data(), nullptr, nullptr,
                                      type, type == SDDS_STRING ? nullptr : text.data());
    if (index < 0) failed("Defining SDDS parameter");
    // Binary string parameters preserve empty text, quotes and Unicode without
    // relying on legacy readers' namelist escape interpretation.
    if (type == SDDS_STRING) textParameters.emplace_back(index, std::move(text));
  }
  void text(const char* name, const QString& value) { parameter(name, SDDS_STRING, value); }
  void integer(const char* name, int value) { parameter(name, SDDS_LONG, QString::number(value)); }
  void exact(const char* name, qint64 value, const char* units = "") { parameter(name, countType, QString::number(value), units); }
  void real(const char* name, double value, const char* units = "") { parameter(name, SDDS_DOUBLE, QString::number(value, 'g', 17), units); }
  void timestamp(const char* name, const char* exactName, qint64 ms) {
    real(name, timeSeconds(ms), "seconds"); exact(exactName, ms, "milliseconds");
  }
  void table(const Table& table) {
    for (const auto& field : table.fields) {
      QByteArray name(field.name), units(field.units);
      const int type = field.kind == Kind::Text ? SDDS_STRING : field.kind == Kind::Integer ? SDDS_LONG : field.kind == Kind::Count ? countType : SDDS_DOUBLE;
      if (SDDS_DefineColumn(&data, name.data(), nullptr, units.data(), nullptr, nullptr, type, 0) < 0)
        failed("Defining SDDS column");
    }
    for (const auto& row : table.rows)
      if (row.size() != int(table.fields.size())) throw std::runtime_error("Invalid SDDS export row");
    const int32_t rows = static_cast<int32_t>(table.rows.size());
    if (!SDDS_WriteLayout(&data) || !SDDS_StartPage(&data, rows)) failed("Writing SDDS layout");
    for (auto& parameter : textParameters)
      if (!SDDS_SetParameters(&data, SDDS_SET_BY_INDEX | SDDS_PASS_BY_VALUE,
                              parameter.first, parameter.second.data(), int32_t{-1})) failed("Writing SDDS parameter");
    if (!rows) return; // Older SDDS libraries reject empty SetColumn calls.
    for (int32_t column = 0; column < int32_t(table.fields.size()); ++column) {
      const auto kind = table.fields[column].kind;
      int32_t ok = 0;
      if (kind == Kind::Text || (kind == Kind::Count && countType == SDDS_STRING)) {
        std::vector<QByteArray> text;
        for (const auto& row : table.rows) text.push_back(utf8(row[column].toString()));
        std::vector<char*> pointers; for (auto& value : text) pointers.push_back(value.data());
        ok = SDDS_SetColumn(&data, SDDS_SET_BY_INDEX, pointers.data(), rows, column);
      } else if (kind == Kind::Integer) {
        std::vector<int32_t> values; for (const auto& row : table.rows) values.push_back(row[column].toInt());
        ok = SDDS_SetColumn(&data, SDDS_SET_BY_INDEX, values.data(), rows, column);
      } else if (kind == Kind::Count) {
        std::vector<int64_t> values; for (const auto& row : table.rows) values.push_back(row[column].toLongLong());
        ok = SDDS_SetColumn(&data, SDDS_SET_BY_INDEX, values.data(), rows, column);
      } else {
        std::vector<double> values; for (const auto& row : table.rows) values.push_back(row[column].toDouble());
        ok = SDDS_SetColumn(&data, SDDS_SET_BY_INDEX, values.data(), rows, column);
      }
      if (!ok) failed("Writing SDDS column");
    }
  }
};

template<class Build> void save(const QString& path, Build build) {
  SDDS_ClearErrors();
  try {
    // Always stage ordinary binary data, irrespective of the chosen suffix.
    QTemporaryFile temporary;
    if (!temporary.open()) throw std::runtime_error(temporary.errorString().toStdString());
    temporary.close();
#ifdef _WIN32
    auto filename = temporary.fileName().toLocal8Bit();
#else
    auto filename = QFile::encodeName(temporary.fileName());
#endif
    Output out; out.initialized = true;
    char description[] = "QtALH data export", contents[] = "Alarm state and analytics";
    if (!SDDS_InitializeOutput(&out.data, SDDS_BINARY, 1, description, contents, filename.data())) failed("Initializing SDDS output");
    out.text("TimestampKind", "QtALH observation/processing UTC; not IOC time");
    out.timestamp("ExportTime", "ExportMilliseconds", QDateTime::currentMSecsSinceEpoch());
    build(out);
    if (!SDDS_WritePage(&out.data)) failed("Writing SDDS page");
    if (!out.close()) failed("Closing SDDS output");
    QSaveFile destination(path);
    if (!temporary.open() || !destination.open(QIODevice::WriteOnly))
      throw std::runtime_error((!temporary.isOpen() ? temporary.errorString() : destination.errorString()).toStdString());
    while (!temporary.atEnd()) {
      const auto block = temporary.read(64 * 1024);
      if (block.isEmpty() || destination.write(block) != block.size())
        throw std::runtime_error((temporary.error() != QFile::NoError ? temporary.errorString() : destination.errorString()).toStdString());
    }
    if (!destination.commit()) throw std::runtime_error(destination.errorString().toStdString());
  } catch (const std::exception& e) {
    SDDS_ClearErrors();
    throw std::runtime_error(("Cannot save " + path + ": " + QString::fromUtf8(e.what())).toStdString());
  }
}
void provenance(Output& out, const AnalyticsReport& r, int tab, bool chart) {
  static const char* titles[] = {"Frequent offenders", "Chattering alarms", "Standing alarms", "Acknowledgement times"};
  out.text("ExportKind", chart ? "AnalyticsChart" : "AnalyticsTable");
  out.text("Configuration", r.configuration); out.text("Session", r.session);
  out.text("Generation", QString::number(r.generation));
  out.integer("Tab", tab); out.text("TabName", titles[tab]);
  out.timestamp("RequestedFromTime", "RequestedFromMilliseconds", r.fromUtc);
  out.timestamp("RequestedToTime", "RequestedToMilliseconds", r.toUtc);
  out.timestamp("DetailCoverageTime", "DetailCoverageMilliseconds", r.coverageUtc);
  out.exact("RequestedRangeMilliseconds", r.query.rangeMs, "milliseconds");
  out.text("Scope", r.query.scope); out.text("Search", r.query.search);
  out.integer("SuppressionFilter", r.query.suppression);
  out.integer("ChatterCount", r.query.chatterCount); out.integer("ChatterWindowSeconds", r.query.chatterSeconds);
  out.integer("PartialDetail", r.partial); out.integer("RemovedChannelHistoryPruned", r.metadataTruncated);
  out.exact("TotalActivations", r.totalActivations); out.exact("CoverageGaps", r.gaps); out.exact("IncompleteSamples", r.incomplete);
  out.text("DurationClock", "monotonic");
}
Table analyticsTable(const AnalyticsReport& r, int tab) {
  using K = Kind;
  Table table{{{"Path", K::Text}, {"ChannelInstance", K::Text}, {"PV", K::Text},
      {"Activations", K::Count}, {"ActivationSharePercent", K::Real, "%"}, {"PeakSeverity", K::Integer},
      {"LastActivationTime", K::Real, "seconds"}, {"LastActivationMilliseconds", K::Count, "milliseconds"},
      {"StandingSeconds", K::Real, "seconds"}, {"StandingLowerBound", K::Integer},
      {"ActiveSeconds", K::Real, "seconds"}, {"ObservedSeconds", K::Real, "seconds"},
      {"AckSamples", K::Count}, {"AckMeanSeconds", K::Real, "seconds"}, {"AckMaxSeconds", K::Real, "seconds"},
      {"OutstandingSeconds", K::Real, "seconds"}, {"OutstandingLowerBound", K::Integer},
      {"IncompleteSamples", K::Count}, {"ChatterMaximum", K::Integer}, {"CurrentActivations", K::Integer},
      {"ChatteringNow", K::Integer}, {"Suppression", K::Text}, {"SuppressionFlags", K::Integer},
      {"Available", K::Integer}, {"Removed", K::Integer}, {"Ambiguous", K::Integer}, {"Initialized", K::Integer},
      {"Severity", K::Integer}, {"UnacknowledgedSeverity", K::Integer}, {"Gaps", K::Count},
      {"LastObservationTime", K::Real, "seconds"}, {"LastObservationMilliseconds", K::Count, "milliseconds"}}, {}};
  for (const auto& row : r.rows) {
    if (tab == 2 && row.standingMs < 0) continue;
    const auto& c = row.channel; const auto& s = row.stats;
    table.rows << Row{c.path, c.key, c.pv, count(s.activations),
        r.totalActivations ? 100.0 * s.activations / r.totalActivations : 0, s.peak,
        timeSeconds(s.lastActivationUtc), count(s.lastActivationUtc), durationSeconds(row.standingMs), c.standingUnknown,
        durationSeconds(s.activeMs), durationSeconds(s.observedMs), count(s.ackCount),
        s.ackCount ? double(s.ackSumMs) / s.ackCount / 1000 : missing,
        s.ackCount ? durationSeconds(s.ackMaxMs) : missing, durationSeconds(row.outstandingMs), c.ackUnknown,
        count(s.incomplete), row.chatterMaximum, row.currentActivations, row.chattering,
        analyticsSuppression(c.flags), c.flags, c.available, c.removed, c.ambiguous, c.initialized,
        c.severity, c.unack, count(s.gaps), timeSeconds(c.lastUtc), count(c.lastUtc)};
  }
  return table;
}
Table analyticsChart(const AnalyticsReport& r, int tab) {
  using K = Kind;
  Table table{{{"Series", K::Text}, {"Label", K::Text}, {"Path", K::Text}, {"ChannelInstance", K::Text}, {"PV", K::Text},
      {"Time", K::Real, "seconds"}, {"TimeMilliseconds", K::Count, "milliseconds"}, {"TimeKnown", K::Integer},
      {"Value", K::Real}, {"Units", K::Text}, {"Count", K::Count}, {"CountKnown", K::Integer},
      {"LowerBound", K::Integer}}, {}};
  auto point = [&](const QString& series, const QString& label, const AnalyticsRow* row,
                   qint64 utc, double value, const QString& units, qint64 n, bool numericCount, bool lower = false) {
    table.rows << Row{series, label, row ? row->channel.path : QString(), row ? row->channel.key : QString(),
        row ? row->channel.pv : QString(), timeSeconds(utc), count(utc), utc != 0, value, units, count(n), numericCount, lower};
  };
  if (tab == 0 || tab == 2) {
    auto rows = r.rows;
    std::sort(rows.begin(), rows.end(), [tab](const auto& a, const auto& b) {
      return tab == 2 ? a.standingMs > b.standingMs : a.stats.activations > b.stats.activations;
    });
    int n = 0;
    for (const auto& row : rows) {
      if (tab == 0 ? !row.stats.activations : row.standingMs < 0) continue;
      point(tab == 0 ? "top_offenders" : "standing", row.channel.path, &row, 0,
            tab == 0 ? double(row.stats.activations) : durationSeconds(row.standingMs),
            tab == 0 ? "activations" : "seconds", tab == 0 ? row.stats.activations : 0,
            tab == 0, tab == 2 && row.channel.standingUnknown);
      if (++n == 10) break;
    }
    if (tab == 0) {
      const qint64 span = r.to - r.coverage;
      for (int i = 0; i < r.trend.size(); ++i) {
        // Same interval starts as the existing CSV export. Long-double
        // multiplication avoids overflow for long session spans.
        const qint64 offset = qint64(static_cast<long double>(span) * i / r.trend.size());
        point("trend", {}, nullptr, r.coverageUtc + offset, double(r.trend[i]), "activations", r.trend[i], true);
      }
    }
  } else if (tab == 1) {
    for (const auto& row : r.rows)
      for (auto utc : row.activationUtc) point("activation_timeline", row.channel.path, &row, utc, 1, "activations", 1, true);
  } else {
    auto labels = analyticsHistogramLabels();
    for (int i = 0; i < AnalyticsHistogramBins; ++i)
      point("acknowledgement_histogram", labels[i], nullptr, 0, double(r.histogram[i]), "samples", r.histogram[i], true);
  }
  return table;
}
} // namespace
#endif

void saveAlarmSnapshotSdds(const QString& path, const Engine& engine) {
#ifndef QTALH_HAVE_SDDS
  (void)path; (void)engine;
  throw std::runtime_error("SDDS export is unavailable in this build");
#else
  save(path, [&](Output& out) {
    using K = Kind;
    out.text("ExportKind", "AlarmSnapshot"); out.text("Configuration", engine.document.filename);
    out.text("Scope", "All configured channels");
    out.integer("GlobalMode", engine.options.global); out.integer("PassiveMode", engine.options.passive);
    Table table{{{"PV", K::Text}, {"Path", K::Text}, {"Identity", K::Text}, {"GroupPath", K::Text},
        {"Label", K::Text}, {"Description", K::Text}, {"ValueText", K::Text},
        {"Status", K::Integer}, {"StatusName", K::Text}, {"Severity", K::Integer}, {"SeverityName", K::Text},
        {"UnacknowledgedSeverity", K::Integer}, {"UnderlyingUnacknowledgedSeverity", K::Integer},
        {"ChangedTime", K::Real, "seconds"}, {"ChangedMilliseconds", K::Count, "milliseconds"},
        {"Initialized", K::Integer}, {"Available", K::Integer}, {"AwaitingObservation", K::Integer},
        {"CommunicationError", K::Integer}, {"ConfiguredMask", K::Text}, {"Mask", K::Text},
        {"Cancelled", K::Integer}, {"Disabled", K::Integer}, {"NoAck", K::Integer},
        {"NoAckTransient", K::Integer}, {"NoLog", K::Integer}, {"Shelved", K::Integer},
        {"ShelfUntilTime", K::Real, "seconds"}, {"ShelfUntilMilliseconds", K::Count, "milliseconds"},
        {"ShelfReason", K::Text}, {"ShelfOperator", K::Text}}, {}};
    for (auto* node : engine.document.channels()) {
      const auto& s = engine.state(node); const auto channel = engine.channelUpdate(node);
      table.rows << Row{node->name, channel.path, channel.identity, node->parent ? Engine::channelPath(node->parent) : QString("/"),
          node->label(), s.description, s.value, s.status, statusName(s.status), s.severity, severityName(s.severity),
          s.presentation.unack, s.unack, timeSeconds(s.changedAt), count(s.changedAt), s.initialized, channel.available,
          s.awaitingObservation, channel.communicationError, node->mask.text(), s.mask.text(),
          channel.cancelled, channel.disabled, channel.noAck, s.mask[AckT], s.mask[Log], channel.shelved,
          timeSeconds(s.shelf.until), count(s.shelf.until), s.shelf.reason, s.shelf.username};
    }
    out.table(table);
  });
#endif
}

void saveAnalyticsSdds(const QString& path, const AnalyticsReport& report, int tab, bool chart) {
#ifndef QTALH_HAVE_SDDS
  (void)path; (void)report; (void)tab; (void)chart;
  throw std::runtime_error("SDDS export is unavailable in this build");
#else
  if (tab < 0 || tab > 3) throw std::runtime_error("Invalid analytics tab");
  save(path, [&](Output& out) { provenance(out, report, tab, chart); out.table(chart ? analyticsChart(report, tab) : analyticsTable(report, tab)); });
#endif
}
} // namespace alh
