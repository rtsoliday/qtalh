#include "services/sdds_export.h"
#include "ui/window.h"
#include "ui/appearance.h"
#include "test_compat.h"
#include <QtWidgets>
#include <QTemporaryDir>
#include <cstdlib>
#include <cmath>
#include <stdexcept>
#ifdef QTALH_HAVE_SDDS
#include <SDDS.h>
#endif

using namespace alh;
#ifdef QTALH_HAVE_SDDS
class Reader {
public:
  SDDS_DATASET data{};
  explicit Reader(const QString& path) {
    auto name = QFile::encodeName(path);
    if (!SDDS_InitializeInput(&data, name.data())) throw std::runtime_error("Opening exported SDDS file failed");
    if (SDDS_ReadPage(&data) <= 0) { SDDS_Terminate(&data); throw std::runtime_error("Reading exported SDDS page failed"); }
  }
  ~Reader() { SDDS_Terminate(&data); }
  int rows() { return int(SDDS_CountRowsOfInterest(&data)); }
  int type(const char* name) {
    QByteArray n(name); const int index = SDDS_GetColumnIndex(&data, n.data());
    if (index < 0) throw std::runtime_error("Missing column");
    return data.layout.column_definition[index].type;
  }
  QString parameter(const char* name) {
    QByteArray n(name); char* value = SDDS_GetParameterAsString(&data, n.data(), nullptr);
    if (!value) throw std::runtime_error("Missing parameter");
    const auto text = QString::fromUtf8(value); std::free(value); return text;
  }
  template<class T> QVector<T> column(const char* name) {
    QByteArray n(name); auto* values = static_cast<T*>(SDDS_GetColumn(&data, n.data()));
    if (!values) throw std::runtime_error("Missing numeric column");
    QVector<T> result; for (int i = 0; i < rows(); ++i) result << values[i];
    std::free(values); return result;
  }
  QStringList exact(const char* name) {
    if (type(name) == SDDS_STRING) return strings(name);
    QStringList result; for (auto n : column<int64_t>(name)) result << QString::number(qlonglong(n));
    return result;
  }
  QStringList strings(const char* name) {
    QByteArray n(name); auto** values = static_cast<char**>(SDDS_GetColumn(&data, n.data()));
    if (!values) throw std::runtime_error("Missing string column");
    QStringList result;
    for (int i = 0; i < rows(); ++i) { result << QString::fromUtf8(values[i]); std::free(values[i]); }
    std::free(values); return result;
  }
  QStringList array(const char* name) {
    QByteArray n(name); auto* value = SDDS_GetArray(&data, n.data(), nullptr);
    if (!value) throw std::runtime_error("Missing metadata array");
    QStringList result; auto** strings = static_cast<char**>(value->data);
    for (int i = 0; i < value->elements; ++i) result << QString::fromUtf8(strings[i]);
    SDDS_FreeArray(value); return result;
  }
};
#endif


static Options runtimeOptions(bool editor = false) {
  Options options; options.editor = editor; options.silent = true;
  options.noLog = true; options.noErrorPopup = true; options.mainWindow = true;
  return options;
}
static Document document() {
  return parseConfig("GROUP NULL root\nGROUP root first\nCHANNEL first pv:a\n"
                     "GROUP root second\nCHANNEL second pv:a\nCHANNEL second pv:b\n");
}
static AnalyticsReport report() {
  AnalyticsReport r;
  r.configuration = QString::fromUtf8("config \"µ\".alhConfig"); r.session = "session";
  r.generation = 18446744073709551615ULL;
  r.from = 0; r.coverage = 2000; r.to = 6000;
  r.fromUtc = -12345; r.coverageUtc = -10345; r.toUtc = -6345;
  r.query.scope = "scope"; r.query.search = "pv:"; r.query.suppression = 2;
  r.query.chatterCount = 7; r.query.chatterSeconds = 20;
  r.partial = true; r.metadataTruncated = true; r.gaps = 2; r.incomplete = 3;
  AnalyticsRow a;
  a.channel.path = "/root/first/pv:a"; a.channel.key = "first-instance"; a.channel.pv = "pv:a";
  a.channel.available = a.channel.initialized = true; a.channel.severity = 2; a.channel.unack = 1;
  a.channel.flags = 4; a.channel.lastUtc = -7654;
  a.stats.activations = 9007199254740993LL; a.stats.peak = 3;
  a.stats.activeMs = 1234; a.stats.observedMs = 4567;
  a.stats.lastActivationUtc = -8765; a.stats.ackCount = 2; a.stats.ackSumMs = 5000; a.stats.ackMaxMs = 4000;
  a.stats.gaps = 1; a.stats.incomplete = 2;
  a.standingMs = 1250; a.outstandingMs = 2500; a.chatterMaximum = 9;
  a.currentActivations = 7; a.chattering = true; a.activationUtc = {-10000, -9000};
  AnalyticsRow b;
  b.channel.path = "/root/second/pv:b"; b.channel.key = "second-instance"; b.channel.pv = "pv:b";
  b.channel.removed = b.channel.ambiguous = true; b.stats.activations = 2;
  b.activationUtc = {-8000};
  r.rows = {b, a}; r.totalActivations = a.stats.activations + 2;
  r.trend = {1, 2}; r.histogram[0] = a.stats.activations; r.histogram[8] = 2;
  return r;
}
#ifdef QTALH_HAVE_SDDS
static void selectFileLater(QObject* owner, const QString& path, std::function<void()> before = {}) {
  QTimer::singleShot(0, owner, [owner, path, before] {
    auto dialog = owner->findChild<QDialog*>("fileSelectionDialog"); QVERIFY(dialog);
    if (before) before();
    if (auto files = qobject_cast<QFileDialog*>(dialog)) {
      files->selectFile(path); QMetaObject::invokeMethod(files, "accept");
    } else {
      auto selection = dialog->findChild<QLineEdit*>("fileSelection"); QVERIFY(selection);
      selection->setText(path); QMetaObject::invokeMethod(selection, "returnPressed");
    }
  });
}
#endif

class SddsTests : public QObject {
  Q_OBJECT
  QTemporaryDir settings;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("QtALH-SddsTests");
    QCoreApplication::setApplicationName("QtALH-SddsTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    initializeAppearance(qEnvironmentVariable("QTALH_TEST_STYLE"));
  }
  void availability() {
    auto w = std::make_unique<Window>(document(), runtimeOptions(), false);
    auto action = w->findChild<QAction*>("exportAlarmSnapshotSdds");
    QCOMPARE(action != nullptr, sddsAvailable());
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Alarm Analytics...") a->trigger();
    auto dialog = w->findChild<QDialog*>("analyticsDialog"); QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QPushButton*>("analyticsExportTableSdds") != nullptr, sddsAvailable());
    QCOMPARE(dialog->findChild<QPushButton*>("analyticsExportChartSdds") != nullptr, sddsAvailable());
    auto editor = std::make_unique<Window>(document(), runtimeOptions(true), false);
    QVERIFY(!editor->findChild<QAction*>("exportAlarmSnapshotSdds"));
#ifndef QTALH_HAVE_SDDS
    QVERIFY(!sddsAvailable());
    QTemporaryDir dir; auto path = dir.filePath("unsupported.sdds");
    QVERIFY_EXCEPTION_THROWN(w->saveAlarmSnapshot(path), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(saveAnalyticsSdds(path, report(), 0), std::runtime_error);
    QVERIFY(!QFile::exists(path));
#endif
  }
  void snapshot() {
#ifndef QTALH_HAVE_SDDS
    QSKIP("SDDS not available");
#else
    QTemporaryDir dir; auto d = document(); d.filename = QString::fromUtf8("µ.alhConfig");
    Engine engine(d); auto nodes = d.channels();
    auto& s = engine.mutableState(nodes[0]);
    s.initialized = true; s.awaitingObservation = false; s.value = QString::fromUtf8("3.14 µ\n\"value\"");
    s.description = QString::fromUtf8("description µ"); s.status = 3; s.severity = 2;
    s.unack = 2; s.presentation.unack = 0; s.changedAt = -12345;
    s.mask[Ack] = true; s.mask[AckT] = true; s.mask[Log] = true;
    s.shelf = {1700000123456LL, "maintenance", "operator"};
    auto path = dir.filePath(QString::fromUtf8("snapshot µ.sdds"));
    saveAlarmSnapshotSdds(path, engine);
    Reader r(path); QCOMPARE(r.rows(), 3); QCOMPARE(r.data.layout.data_mode.mode, SDDS_BINARY);
    QCOMPARE(r.parameter("Configuration"), d.filename);
    QCOMPARE(r.parameter("ExportKind"), QString("AlarmSnapshot"));
    QVERIFY(r.parameter("TimestampKind").contains("not IOC"));
    QCOMPARE(r.strings("PV"), QStringList({"pv:a", "pv:a", "pv:b"}));
    QVERIFY(r.strings("Path")[0] != r.strings("Path")[1]);
    QCOMPARE(r.strings("ValueText")[0], s.value); QCOMPARE(r.strings("Description")[0], s.description);
    QCOMPARE(r.column<int32_t>("Status")[0], 3); QCOMPARE(r.column<int32_t>("Severity")[0], 2);
    QCOMPARE(r.column<int32_t>("UnacknowledgedSeverity")[0], 0);
    QCOMPARE(r.column<int32_t>("UnderlyingUnacknowledgedSeverity")[0], 2);
    QCOMPARE(r.column<double>("ChangedTime")[0], -12.345);
    QCOMPARE(r.exact("ChangedMilliseconds")[0], QString("-12345"));
    QVERIFY(std::isnan(r.column<double>("ChangedTime")[1]));
    QCOMPARE(r.exact("ShelfUntilMilliseconds")[0], QString("1700000123456"));
    QCOMPARE(r.column<int32_t>("NoAck")[0], 1); QCOMPARE(r.column<int32_t>("NoLog")[0], 1);
    QCOMPARE(r.column<int32_t>("NoAckTransient")[0], 1); QCOMPARE(r.column<int32_t>("Shelved")[0], 1);
    QCOMPARE(engine.state(nodes[0]).changedAt, qint64(-12345)); QCOMPARE(engine.history.size(), 0);
    auto empty = parseConfig("GROUP NULL empty\n"); Engine emptyEngine(empty);
    saveAlarmSnapshotSdds(dir.filePath("empty.sdds"), emptyEngine);
    Reader er(dir.filePath("empty.sdds")); QCOMPARE(er.rows(), 0);
#endif
  }
  void analyticsTable() {
#ifndef QTALH_HAVE_SDDS
    QSKIP("SDDS not available");
#else
    QTemporaryDir dir; auto input = report(); auto path = dir.filePath("table.sdds");
    saveAnalyticsSdds(path, input, 0);
    Reader r(path); QCOMPARE(r.rows(), 2); QCOMPARE(r.data.layout.data_mode.mode, SDDS_BINARY);
    QCOMPARE(r.parameter("Configuration"), input.configuration);
    QCOMPARE(r.parameter("Generation"), QString("18446744073709551615"));
    QCOMPARE(r.parameter("Scope"), input.query.scope); QCOMPARE(r.parameter("Search"), input.query.search);
    QCOMPARE(r.parameter("SuppressionFilter"), QString("2"));
    QCOMPARE(r.parameter("ChatterCount"), QString("7")); QCOMPARE(r.parameter("ChatterWindowSeconds"), QString("20"));
    QCOMPARE(r.parameter("RequestedFromMilliseconds"), QString("-12345"));
    QCOMPARE(r.parameter("DetailCoverageMilliseconds"), QString("-10345"));
    QCOMPARE(r.parameter("PartialDetail"), QString("1"));
    QCOMPARE(r.parameter("RemovedChannelHistoryPruned"), QString("1"));
    QCOMPARE(r.parameter("CoverageGaps"), QString("2")); QCOMPARE(r.parameter("DurationClock"), QString("monotonic"));
    QCOMPARE(r.strings("ChannelInstance"), QStringList({"second-instance", "first-instance"}));
    QCOMPARE(r.exact("Activations"), QStringList({"2", "9007199254740993"}));
#ifdef SDDS_LONG64
    QCOMPARE(r.type("Activations"), SDDS_LONG64);
#else
    QCOMPARE(r.type("Activations"), SDDS_STRING);
#endif
    QVERIFY(std::isnan(r.column<double>("StandingSeconds")[0]));
    QCOMPARE(r.column<double>("StandingSeconds")[1], 1.25);
    QCOMPARE(r.column<int32_t>("StandingLowerBound")[1], 1);
    QCOMPARE(r.column<double>("AckMeanSeconds")[1], 2.5);
    QVERIFY(std::isnan(r.column<double>("AckMeanSeconds")[0]));
    QCOMPARE(r.exact("LastActivationMilliseconds")[1], QString("-8765"));
    QCOMPARE(r.column<double>("LastActivationTime")[1], -8.765);
    QCOMPARE(r.column<int32_t>("Removed")[0], 1); QCOMPARE(r.column<int32_t>("Ambiguous")[0], 1);
    saveAnalyticsSdds(dir.filePath("standing.sdds"), input, 2);
    Reader standing(dir.filePath("standing.sdds")); QCOMPARE(standing.rows(), 1);
    input.rows.clear(); saveAnalyticsSdds(dir.filePath("empty.sdds"), input, 3);
    Reader empty(dir.filePath("empty.sdds")); QCOMPARE(empty.rows(), 0);
#endif
  }
  void analyticsCharts() {
#ifndef QTALH_HAVE_SDDS
    QSKIP("SDDS not available");
#else
    QTemporaryDir dir; auto input = report();
    for (int tab = 0; tab < 4; ++tab) {
      const auto path = dir.filePath(QString::number(tab) + ".sdds");
      saveAnalyticsSdds(path, input, tab, true); Reader r(path);
      QCOMPARE(r.parameter("ExportKind"), QString("AnalyticsChart"));
      if (tab == 0) {
        QCOMPARE(r.rows(), 4);
        QCOMPARE(r.strings("Series"), QStringList({"top_offenders", "top_offenders", "trend", "trend"}));
        QCOMPARE(r.exact("Count")[0], QString("9007199254740993"));
        QCOMPARE(r.exact("TimeMilliseconds").mid(2), QStringList({"-10345", "-8345"}));
        QVERIFY(std::isnan(r.column<double>("Time")[0]));
      } else if (tab == 1) {
        QCOMPARE(r.rows(), 3);
        QCOMPARE(r.exact("TimeMilliseconds"), QStringList({"-8000", "-10000", "-9000"}));
        QCOMPARE(r.column<int32_t>("TimeKnown"), QVector<int32_t>({1, 1, 1}));
      } else if (tab == 2) {
        QCOMPARE(r.rows(), 1); QCOMPARE(r.column<double>("Value")[0], 1.25);
        QCOMPARE(r.column<int32_t>("LowerBound")[0], 1); QCOMPARE(r.column<int32_t>("CountKnown")[0], 0);
      } else {
        QCOMPARE(r.rows(), AnalyticsHistogramBins);
        QCOMPARE(r.strings("Label"), analyticsHistogramLabels());
        QCOMPARE(r.exact("Count")[0], QString("9007199254740993"));
      }
    }
    input.rows.clear(); input.trend.clear();
    for (int tab = 0; tab < 3; ++tab) {
      const auto path = dir.filePath("empty" + QString::number(tab) + ".sdds");
      saveAnalyticsSdds(path, input, tab, true); Reader r(path); QCOMPARE(r.rows(), 0);
    }
#endif
  }
  void failuresPreserveDestination() {
#ifndef QTALH_HAVE_SDDS
    QSKIP("SDDS not available");
#else
    QTemporaryDir dir; const auto path = dir.filePath("keep.sdds");
    { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write("original"), qint64(8)); }
    auto input = report();
    QVERIFY_EXCEPTION_THROWN(saveAnalyticsSdds(path, input, 4), std::runtime_error);
    input.rows[0].channel.pv = QString("invalid") + QChar(0);
    QVERIFY_EXCEPTION_THROWN(saveAnalyticsSdds(path, input, 0), std::runtime_error);
    QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), QByteArray("original"));
    try {
      saveAnalyticsSdds(dir.filePath("missing/output.sdds"), report(), 0);
      QFAIL("Saving to a nonexistent directory must fail");
    } catch (const std::exception& e) {
      QVERIFY(QString::fromUtf8(e.what()).contains("missing/output.sdds"));
    }
#endif
  }
  void uiExports() {
#ifndef QTALH_HAVE_SDDS
    QSKIP("SDDS not available");
#else
    QTemporaryDir dir; qint64 now = 1000;
    auto w = std::make_unique<Window>(document(), runtimeOptions(), false); w->show();
    auto service = w->alarmAnalytics();
    service->monotonicNow = [&] { return now; }; service->utcNow = [&] { return 1700000000000LL + now; };
    service->reset();
    const auto snapshot = dir.filePath("snapshot.sdds"); selectFileLater(w.get(), snapshot);
    w->findChild<QAction*>("exportAlarmSnapshotSdds")->trigger();
    { Reader r(snapshot); QCOMPARE(r.rows(), 3); }
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Alarm Analytics...") a->trigger();
    auto dialog = w->findChild<QDialog*>("analyticsDialog"); QVERIFY(dialog);
    auto table = dialog->findChild<QTableView*>("analyticsTable0");
    QTRY_COMPARE(table->model()->rowCount(), 3);
    // A report for the dialog's default query, which shows unsuppressed activity.
    auto input = report(); input.query = AnalyticsQuery{}; input.query.suppression = 1;
    service->reportReady(input); table->sortByColumn(0, Qt::DescendingOrder);
    QCOMPARE(table->model()->rowCount(), 2);
    const auto first = table->model()->index(0, 0).data(Qt::UserRole).toString();
    const auto output = dir.filePath("table.sdds");
    selectFileLater(dialog, output, [=] { auto refreshed = input; refreshed.rows.clear(); service->reportReady(refreshed); });
    dialog->findChild<QPushButton*>("analyticsExportTableSdds")->click();
    { Reader r(output); QCOMPARE(r.rows(), 2); QCOMPARE(r.strings("ChannelInstance")[0], first); }
    service->reportReady(input);
    dialog->findChild<QTabWidget*>("analyticsTabs")->setCurrentIndex(1);
    auto chatter = dialog->findChild<QTableView*>("analyticsTable1");
    QVERIFY(chatter->model()->rowCount() > 0);
    chatter->setCurrentIndex(chatter->model()->index(0, 0));
    auto key = chatter->currentIndex().data(Qt::UserRole).toString();
    const auto chart = dir.filePath("chart.sdds"); selectFileLater(dialog, chart);
    dialog->findChild<QPushButton*>("analyticsExportChartSdds")->click();
    Reader r(chart); QVERIFY(r.rows() > 0);
    for (const auto& exported : r.strings("ChannelInstance")) QCOMPARE(exported, key);
    dialog->findChild<QTabWidget*>("analyticsTabs")->setCurrentIndex(0);
    dialog->findChild<QLineEdit*>("analyticsSearch")->setText("pv:b");
    now += 1000;
    QTRY_COMPARE(table->model()->rowCount(), 1);
    const auto filtered = dir.filePath("filtered.sdds"); selectFileLater(dialog, filtered);
    dialog->findChild<QPushButton*>("analyticsExportTableSdds")->click();
    Reader filteredReader(filtered); QCOMPARE(filteredReader.rows(), 1);
    QCOMPARE(filteredReader.strings("PV"), QStringList({"pv:b"}));
    QCOMPARE(filteredReader.parameter("Search"), QString("pv:b"));
#endif
  }
};
QTEST_MAIN(SddsTests)
#include "test_sdds.moc"
