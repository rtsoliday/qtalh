#include "services/analytics.h"
#include "services/notifications.h"
#include "test_compat.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
using namespace alh;
namespace {
ChannelUpdate sample(QString id = "pv", int severity = 0, int unack = 0) {
  ChannelUpdate s;
  s.identity = id;
  s.path = "/root/" + id;
  s.pv = id;
  s.ancestors = QStringList{"root", id};
  s.initialized = s.available = true;
  s.severity = severity;
  s.unack = unack;
  return s;
}
AnalyticsQuery session() {
  AnalyticsQuery q;
  q.rangeMs = 0;
  return q;
}
void update(AlarmAnalytics& a, ChannelUpdate s, qint64 now,
            ObservationCause cause = ObservationCause::Processed) {
  a.observe({{}, s, cause}, now, 1700000000000 + now);
}
AnalyticsReport report(AlarmAnalytics& a, qint64 now, AnalyticsQuery q = session()) {
  return AlarmAnalytics::query(a.snapshot(now, 1700000000000 + now), q);
}
struct Driver : PvService {
  bool succeeds = true;
  int writes = 0;
  std::function<void(Event)> callback;
  void monitor(const QString&, std::function<void(Event)> cb, const void*) override {
    callback = cb;
  }
  void text(const QString&, std::function<void(QString)>) override {}
  void number(const QString&, std::function<void(double)>, const void*) override {}
  void cancelNumbers(const void*) override {}
  void prepare(const QString&) override {}
  bool canWrite(const QString&) const override { return succeeds; }
  bool put(const QString&, double, WriteKind) override {
    ++writes;
    return succeeds;
  }
  void cancel(const QString&, const void*) override {}
  void clear() override {}
};
} // namespace
class AnalyticsTests : public QObject {
  Q_OBJECT
private slots:
  void crossingsAndChatter() {
    AlarmAnalytics a(0, 1700000000000);
    auto s = sample();
    a.reconcile({s}, 0, 1700000000000);
    update(a, s, 0);
    for (int i = 0; i < 5; ++i) {
      update(a, sample("pv", 2, 2), 1000 + i * 10000);
      update(a, sample("pv", 3, 3), 2000 + i * 10000);
      update(a, s, 3000 + i * 10000);
    }
    auto r = report(a, 50000);
    QCOMPARE(r.rows[0].stats.activations, qint64(5));
    QCOMPARE(r.rows[0].stats.peak, 3);
    QCOMPARE(r.rows[0].chatterMaximum, 5);
    QVERIFY(r.rows[0].chattering);
    QCOMPARE(report(a, 110000).rows[0].currentActivations, 0);
  }
  void chatterBoundaryAndQueryChange() {
    AlarmAnalytics a;
    auto normal = sample();
    a.reconcile({normal}, 0, 0);
    update(a, normal, 0);
    update(a, sample("pv", 2, 2), 1000);
    update(a, normal, 2000);
    update(a, sample("pv", 2, 2), 61000);
    auto q = session();
    q.chatterCount = 2;
    auto r = report(a, 61000, q);
    QVERIFY(r.rows[0].chattering);
    QCOMPARE(r.rows[0].chatterMaximum, 2);
    q.chatterSeconds = 59;
    r = report(a, 61000, q);
    QVERIFY(!r.rows[0].chattering);
    QCOMPARE(r.rows[0].chatterMaximum, 1);
    q.rangeMs = 1000;
    QCOMPARE(report(a, 62001, q).rows[0].stats.activations, qint64(0));
  }
  void initialUnavailableAndCancelledBaseline() {
    AlarmAnalytics a;
    auto s = sample();
    a.reconcile({s}, 0, 0);
    s.available = false;
    s.severity = 4;
    update(a, s, 0);
    QCOMPARE(report(a, 1000).rows[0].stats.gaps, qint64(1));
    update(a, sample("pv", 2, 2), 2000);
    QCOMPARE(report(a, 3000).rows[0].stats.activations, qint64(0));
    s = sample();
    s.cancelled = true;
    s.available = false;
    update(a, s, 4000, ObservationCause::Suppression);
    update(a, sample("pv", 2, 2), 5000, ObservationCause::Suppression);
    auto r = report(a, 6000);
    QCOMPARE(r.rows[0].stats.activations, qint64(0));
    QVERIFY(r.rows[0].channel.standingUnknown);
  }
  void standingAcknowledgementAndTransient() {
    AlarmAnalytics a;
    auto s = sample();
    a.reconcile({s}, 0, 0);
    update(a, s, 0);
    update(a, sample("pv", 2, 2), 1000);
    update(a, sample("pv", 2, 0), 5000, ObservationCause::LocalAcknowledgement);
    auto r = report(a, 7000);
    QCOMPARE(r.rows[0].standingMs, qint64(6000));
    QCOMPARE(r.rows[0].stats.ackCount, qint64(1));
    QCOMPARE(r.rows[0].stats.ackSumMs, qint64(4000));
    update(a, sample("pv", 0, 0), 8000);
    update(a, sample("pv", 2, 2), 10000);
    update(a, sample("pv", 0, 2), 11000);
    update(a, sample("pv", 0, 0), 15000, ObservationCause::LocalAcknowledgement);
    r = report(a, 16000);
    QCOMPARE(r.rows[0].stats.ackCount, qint64(2));
    QCOMPARE(r.rows[0].stats.ackMaxMs, qint64(5000));
    QCOMPARE(r.rows[0].standingMs, qint64(-1));
    QCOMPARE(r.rows[0].stats.activeMs, qint64(8000));
  }
  void unknownAndAutomaticSamples() {
    AlarmAnalytics a;
    auto s = sample("pv", 2, 2);
    a.reconcile({s}, 0, 0);
    update(a, s, 0);
    QVERIFY(report(a, 1000).rows[0].channel.standingUnknown);
    update(a, sample(), 2000, ObservationCause::LocalAcknowledgement);
    update(a, s, 3000);
    update(a, sample(), 4000);
    auto r = report(a, 5000);
    QCOMPARE(r.rows[0].stats.ackCount, qint64(0));
    QCOMPARE(r.rows[0].stats.incomplete, qint64(2));
    QCOMPARE(r.rows[0].stats.activations, qint64(1));
  }
  void suppressionAndGap() {
    AlarmAnalytics a;
    auto s = sample();
    a.reconcile({s}, 0, 0);
    update(a, s, 0);
    update(a, sample("pv", 2, 2), 1000);
    s = sample("pv", 2, 2);
    s.shelved = s.suppressed = true;
    update(a, s, 2000, ObservationCause::Suppression);
    s.severity = 0;
    update(a, s, 3000);
    s.severity = 2;
    update(a, s, 4000);
    s.shelved = s.suppressed = false;
    update(a, s, 5000, ObservationCause::Suppression);
    auto r = report(a, 6000);
    QCOMPARE(r.rows[0].stats.activations, qint64(2));
    auto q = session();
    q.suppression = 2;
    QCOMPARE(report(a, 6000, q).rows[0].stats.activeMs, qint64(2000));
    s.available = false;
    s.severity = 4;
    update(a, s, 7000);
    update(a, sample("pv", 2, 2), 9000);
    r = report(a, 10000);
    QCOMPARE(r.rows[0].stats.activations, qint64(2));
    QCOMPARE(r.rows[0].stats.gaps, qint64(1));
    QVERIFY(r.rows[0].channel.standingUnknown);
    QCOMPARE(r.rows[0].standingMs, qint64(1000));
  }
  void rollingIntervalsAndEviction() {
    AlarmAnalytics a;
    auto s = sample();
    a.reconcile({s}, 0, 0);
    update(a, s, 0);
    update(a, sample("pv", 2, 2), 1000);
    update(a, sample(), 5000);
    AnalyticsQuery q;
    q.rangeMs = 3000;
    auto r = report(a, 6000, q);
    QCOMPARE(r.rows[0].stats.activeMs, qint64(2000));
    QCOMPARE(r.rows[0].stats.activations, qint64(0));
    a.maximumRecords = 2;
    for (int i = 0; i < 10; ++i)
      update(a, sample("pv", i % 2 ? 2 : 0, i % 2 ? 2 : 0), 7000 + i * 1000);
    QVERIFY(a.recordCount() <= 2);
    r = report(a, 18000);
    QCOMPARE(r.rows[0].stats.activations, qint64(6));
    QVERIFY(r.partial);
    a.maximumBytes = sizeof(AnalyticsRecord);
    a.trim(18000);
    QVERIFY(a.recordCount() <= 1);
    a.trim(AnalyticsDay + 18000);
    QCOMPARE(a.recordCount(), 0);
    QCOMPARE(report(a, AnalyticsDay + 18000).rows[0].stats.activations, qint64(6));
  }
  void reloadRemovedDuplicatesAndReset() {
    AlarmAnalytics a;
    auto s = sample();
    a.reconcile({s, sample("removed")}, 0, 0);
    update(a, s, 0);
    update(a, sample("pv", 2, 2), 1000);
    a.reconcile({s}, 2000, 2000);
    update(a, sample("pv", 2, 2), 3000);
    auto r = report(a, 4000);
    QCOMPARE(r.rows[0].stats.activations, qint64(1));
    QVERIFY(r.rows[0].channel.standingUnknown);
    QCOMPARE(r.rows[0].stats.incomplete, qint64(1));
    QVERIFY(r.rows[1].channel.removed);
    a.reconcile({s, s}, 5000, 5000);
    update(a, sample("pv", 2, 2), 6000);
    r = report(a, 7000);
    QVERIFY(r.rows[0].channel.removed);
    QVERIFY(r.rows.last().channel.ambiguous);
    a.reset(8000, 8000);
    QCOMPARE(report(a, 9000).rows.size(), 0);
  }
  void scopesAndCsv() {
    AlarmAnalytics a;
    auto s = sample("=unsafe,\"quoted\"");
    a.reconcile({s, sample("other")}, 0, 0);
    update(a, s, 0);
    s.severity = s.unack = 2;
    update(a, s, 1000);
    auto q = session();
    q.search = "unsafe";
    auto r = report(a, 2000, q);
    QCOMPARE(r.rows.size(), 1);
    auto csv = analyticsCsv(r, 0);
    QVERIFY(csv.contains("'\x3dunsafe"));
    QVERIFY(csv.contains("\"\"quoted\"\""));
    QVERIFY(csv.contains("partial_detail"));
    QVERIFY(csv.contains("active_seconds"));
    QVERIFY(!analyticsCsv(r, 0, true).isEmpty());
    q.scope = "missing";
    QVERIFY(report(a, 2000, q).rows.isEmpty());
  }
  void independentObserversAndAcknowledgementCauses() {
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Driver driver;
    Engine e(doc, {true}, &driver);
    auto n = doc.channels()[0];
    QVector<ObservationCause> causes;
    int other = 0;
    auto a = e.observe([&](const AlarmObservation& o) { causes << o.cause; });
    auto b = e.observe([&](const AlarmObservation&) { ++other; });
    e.event(n, {0, 0, 0, 1, "0"});
    e.event(n, {3, 2, 2, 1, "2"});
    driver.succeeds = false;
    e.acknowledge(n);
    QCOMPARE(causes.last(), ObservationCause::Processed);
    driver.succeeds = true;
    e.acknowledge(n);
    e.event(n, {3, 2, 0, 1, "2"});
    QCOMPARE(causes.last(), ObservationCause::RequestedAcknowledgement);
    e.event(n, {3, 2, 2, 1, "2"});
    e.event(n, {3, 2, 0, 1, "2"});
    QCOMPARE(causes.last(), ObservationCause::ExternalAcknowledgement);
    int before = other;
    b.reset();
    e.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(other, before);
    QVERIFY(a);
  }
  void filteredRecoveryAcknowledgements_data() {
    QTest::addColumn<bool>("latched");
    QTest::addColumn<bool>("requested");
    QTest::newRow("automatic-clear") << false << false;
    QTest::newRow("ambiguous-request-and-clear") << false << true;
    QTest::newRow("external-latched-ack") << true << false;
    QTest::newRow("requested-latched-ack") << true << true;
  }
  void filteredRecoveryAcknowledgements() {
    QFETCH(bool, latched);
    QFETCH(bool, requested);
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 2 5\n");
    Driver driver;
    Engine e(doc, {true}, &driver);
    auto n = doc.channels()[0];
    qint64 now = 0;
    e.now = e.monotonicNow = [&] { return now; };
    AlarmAnalytics a;
    a.reconcile({e.channelUpdate(n)}, now, now);
    ObservationCause cause{};
    auto observer = e.observe([&](const AlarmObservation& o) {
      cause = o.cause;
      a.observe(o, now, now);
    });
    e.event(n, {0, 0, 0, int(latched), "normal"});
    now = 1000;
    e.event(n, {3, 2, 2, int(latched), "alarm"});
    now = 6000;
    e.tick();
    QCOMPARE(e.state(n).unack, 2);
    if (requested && !latched) e.acknowledge(n);
    now = 7000;
    e.event(n, {0, 0, latched ? 2 : 0, int(latched), "recovered"});
    QCOMPARE(e.state(n).severity, 2); // Recovery is still held by the filter.
    QCOMPARE(cause, ObservationCause::Processed);
    QCOMPARE(report(a, now).rows[0].stats.ackCount, qint64(0));
    if (latched) {
      if (requested) e.acknowledge(n);
      now = 8000;
      e.event(n, {0, 0, 0, 1, "confirmed"});
      QCOMPARE(cause, requested ? ObservationCause::RequestedAcknowledgement
                                : ObservationCause::ExternalAcknowledgement);
      QCOMPARE(report(a, now).rows[0].stats.ackCount, qint64(1));
      QCOMPARE(report(a, now).rows[0].stats.ackSumMs, qint64(2000));
    } else {
      QCOMPARE(report(a, now).rows[0].stats.incomplete, qint64(1));
    }
    now = 12000;
    e.tick();
    QCOMPARE(e.state(n).severity, 0);
    QCOMPARE(report(a, now).rows[0].stats.ackCount, qint64(latched));
  }
  void filteredConnectionGap_data() {
    QTest::addColumn<int>("recoverySeverity");
    QTest::newRow("normal-recovery") << 0;
    QTest::newRow("alarm-recovery") << 2;
  }
  void filteredConnectionGap() {
    QFETCH(int, recoverySeverity);
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER -1 30\n");
    Engine e(doc);
    auto n = doc.channels()[0];
    qint64 now = 0;
    e.now = e.monotonicNow = [&] { return now; };
    AlarmAnalytics a;
    a.reconcile({e.channelUpdate(n)}, 0, 0);
    auto observer = e.observe([&](const AlarmObservation& o) { a.observe(o, now, now); });
    e.event(n, {0, 0, 0, 1, "normal"});
    now = 1000;
    e.event(n, {22, 4, 0, -1, "0"});
    QCOMPARE(e.state(n).severity, 0); // Preserve the display's legacy alarm filter.
    QVERIFY(!e.channelUpdate(n).available);
    e.shelve(n, 1, "outage", "tester");
    e.unshelve(n);
    QVERIFY(!e.channelUpdate(n).available);
    now = 10000;
    e.event(n, {recoverySeverity ? 3 : 0, recoverySeverity, recoverySeverity, 1, "recovered"});
    if (recoverySeverity) {
      QVERIFY(!e.channelUpdate(n).available);
      now = 31000;
      e.tick();
    }
    QVERIFY(e.channelUpdate(n).available);
    auto r = report(a, now + 1000);
    QCOMPARE(r.rows[0].stats.gaps, qint64(1));
    QCOMPARE(r.rows[0].stats.observedMs, qint64(2000));
    QCOMPARE(r.totalActivations, qint64(0));
    if (recoverySeverity) {
      QCOMPARE(r.rows[0].standingMs, qint64(1000));
      QVERIFY(r.rows[0].channel.standingUnknown);
    }
  }
  void filteredTransientGapEndsOnRecovery() {
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 0 1\n");
    Engine e(doc, {true}); auto n = doc.channels()[0];
    qint64 now = 0; e.now = e.monotonicNow = [&] { return now; };
    AlarmAnalytics a;
    a.reconcile({e.channelUpdate(n)}, now, now);
    auto observer = e.observe([&](const AlarmObservation& o) { a.observe(o, now, now); });
    e.event(n, {3, 2, 2, 1, "alarm"});
    now = 1000; e.event(n, {0, 0, 2, 1, "clear"});
    now = 2000; e.tick();
    now = 3000; e.event(n, {22, 4, 0, -1, "error"});
    now = 3100; e.event(n, {0, 0, 2, 1, "recovered"});
    auto r = report(a, 5000);
    QCOMPARE(r.rows[0].stats.gaps, qint64(1));
    QCOMPARE(r.rows[0].stats.observedMs, qint64(4900));
    QCOMPARE(r.rows[0].stats.ackCount, qint64(0));
    QVERIFY(e.channelUpdate(n).available);
  }
  void cancelAwaitsFreshObservation_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("filtered");
    QTest::newRow("local") << false << false;
    QTest::newRow("global") << true << false;
    QTest::newRow("local-filtered") << false << true;
    QTest::newRow("global-filtered") << true << true;
  }
  void cancelAwaitsFreshObservation() {
    QFETCH(bool, global);
    QFETCH(bool, filtered);
    auto doc = parseConfig(QString("GROUP NULL root\nCHANNEL root pv\n") +
                           (filtered ? "$ALARMCOUNTFILTER 2 5\n" : ""));
    Engine e(doc, {global});
    auto n = doc.channels()[0];
    qint64 now = 0;
    e.now = e.monotonicNow = [&] { return now; };
    AlarmAnalytics a;
    a.reconcile({e.channelUpdate(n)}, 0, 0);
    auto observer = e.observe([&](const AlarmObservation& o) { a.observe(o, now, now); });
    e.event(n, {3, 2, 2, 1, "already active"});
    now = 1000;
    e.setMask(n, Mask::parse("C"));
    QVERIFY(!e.channelUpdate(n).available);
    now = 2000;
    e.resetMask(n);
    QVERIFY(!e.channelUpdate(n).available);
    QVERIFY(!report(a, 2500).rows[0].channel.available);
    QCOMPARE(report(a, 2500).rows[0].stats.observedMs, qint64(1000));
    // Other presentation operations cannot establish a monitoring baseline.
    e.shelve(n, 1, "awaiting reconnect", "tester");
    e.unshelve(n);
    QVERIFY(!e.channelUpdate(n).available);
    now = 3000;
    e.event(n, {3, 2, 2, 1, "same standing alarm"});
    if (filtered) {
      QVERIFY(!e.channelUpdate(n).available);
      now = 8000;
      e.tick();
    }
    QVERIFY(e.channelUpdate(n).available);
    const auto r = report(a, now + 1000);
    QCOMPARE(r.totalActivations, qint64(0));
    QCOMPARE(r.rows[0].stats.observedMs, qint64(2000));
    QCOMPARE(r.rows[0].standingMs, qint64(1000));
    QVERIFY(r.rows[0].channel.standingUnknown);
    QVERIFY(r.rows[0].channel.ackUnknown);
  }
  void localAckAndAutoClearCauses() {
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine e(doc);
    auto n = doc.channels()[0];
    ObservationCause cause{};
    auto token = e.observe([&](const AlarmObservation& o) { cause = o.cause; });
    e.event(n, {0, 0, 0, 1, "0"});
    e.event(n, {3, 2, 2, 1, "2"});
    e.acknowledge(n);
    QCOMPARE(cause, ObservationCause::LocalAcknowledgement);
    e.setMask(n, Mask::parse("--A--"));
    QCOMPARE(cause, ObservationCause::Suppression);
  }
  void notificationsAndAsyncReset() {
    QTemporaryDir dir;
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine e(doc);
    AnalyticsService service;
    NotificationService notifications(nullptr, dir.filePath("settings.json"));
    qint64 now = 0;
    service.monotonicNow = [&] { return now; };
    service.utcNow = [&] { return 1700000000000 + now; };
    service.attach(&e, "/test.alh");
    notifications.attach(&e, doc.channels(), "/test.alh");
    service.reset();
    e.event(doc.channels()[0], {0, 0, 0, 1, "0"});
    now = 1000;
    e.event(doc.channels()[0], {3, 2, 2, 1, "2"});
    notifications.enable(true);
    notifications.enable(false);
    now = 2000;
    e.acknowledge(doc.channels()[0]);
    auto r = report(service.analytics, 3000);
    QCOMPARE(r.rows[0].stats.activations, qint64(1));
    QCOMPARE(r.rows[0].stats.ackCount, qint64(1));
    int results = 0;
    service.reportReady = [&](AnalyticsReport) { ++results; };
    service.request(session());
    service.reset();
    QTest::qWait(200);
    QCOMPARE(results, 0);
    now = 4000;
    service.request(session());
    QTRY_COMPARE(results, 1);
    now = 20000;
    service.poll();
    QVERIFY(!report(service.analytics, now).rows[0].channel.available);
  }
  void tenThousandChannelsAndPressure() {
    AlarmAnalytics a;
    QVector<ChannelUpdate> all;
    for (int i = 0; i < 10000; ++i)
      all << sample(QString::number(i));
    a.reconcile(all, 0, 0);
    QElapsedTimer timer;
    timer.start();
    for (auto s : all)
      update(a, s, 0);
    for (int round = 1; round <= 30; ++round)
      for (auto s : all) {
        s.severity = s.unack = round % 2 ? 2 : 0;
        update(a, s, round * 1000);
      }
    QVERIFY(a.recordCount() <= 250000);
    QVERIFY(a.recordBytes() <= 64 * 1024 * 1024);
    auto r = report(a, 31000);
    QCOMPARE(r.totalActivations, qint64(150000));
    QVERIFY(r.partial);
    qInfo() << "10,000 channels / 300,000 transitions, ms:" << timer.elapsed();
    QVERIFY(timer.elapsed() < 10000);
  }
};
QTEST_GUILESS_MAIN(AnalyticsTests)
#include "test_analytics.moc"
