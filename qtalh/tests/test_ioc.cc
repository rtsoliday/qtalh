#include "services/channel_access.h"
#include "core/notifications.h"
#include "core/analytics.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QtTest>
#include <cmath>
using namespace alh;
class IocTests : public QObject {
  Q_OBJECT
  QProcess ioc;
  QString prefix;
  QTemporaryDir security;
  QString database = QFileInfo("tests/ioc.db").absoluteFilePath();
  void startIoc() {
    QFile input("tests/ioc.acf");
    QVERIFY(input.open(QIODevice::ReadOnly));
    QFile output(security.filePath("access.acf"));
    QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
    output.write(input.readAll().replace("$(P)", prefix.toLatin1()));
    output.close();
    ioc.start(QString(EPICS_TEST_BIN) + "/softIoc",
              {"-m", "P=" + prefix, "-a", security.filePath("access.acf"), "-d", database});
    QVERIFY(ioc.waitForStarted());
  }
private slots:
  void initTestCase() {
    prefix = "qtalh_test_" + QString::number(QCoreApplication::applicationPid()) + ":";
    int port = 22000 + QCoreApplication::applicationPid() % 15000;
    qputenv("EPICS_CA_AUTO_ADDR_LIST", "NO");
    qputenv("EPICS_CA_ADDR_LIST", ("127.0.0.1:" + QString::number(port)).toLatin1());
    qputenv("EPICS_CA_SERVER_PORT", QByteArray::number(port));
    qputenv("EPICS_CA_REPEATER_PORT", QByteArray::number(port + 1));
    qputenv("EPICS_CAS_INTF_ADDR_LIST", "127.0.0.1");
    qputenv("EPICS_CAS_BEACON_ADDR_LIST", ("127.0.0.1:" + QString::number(port + 1)).toLatin1());
    ioc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    startIoc();
  }

  void analyticsConfirmedAcknowledgementsAndGaps() {
    auto doc=parseConfig("GROUP NULL analytics\nCHANNEL analytics "+prefix+"alarm\nCHANNEL analytics "+prefix+"protected\n");
    ChannelAccess ca({true});Engine engine(doc,{true},&ca);auto n=doc.channels()[0];auto protectedNode=doc.channels()[1];
    QElapsedTimer clock;clock.start();AlarmAnalytics analytics(0,QDateTime::currentMSecsSinceEpoch());
    QVector<ChannelUpdate> initial;for(auto c:doc.channels())initial<<engine.channelUpdate(c);
    analytics.reconcile(initial,0,QDateTime::currentMSecsSinceEpoch());
    ObservationCause lastAck=ObservationCause::Processed;
    auto observer=engine.observe([&](const AlarmObservation&o){analytics.observe(o,clock.elapsed(),QDateTime::currentMSecsSinceEpoch());if(o.cause==ObservationCause::RequestedAcknowledgement||o.cause==ObservationCause::ExternalAcknowledgement)lastAck=o.cause;});
    engine.start();QTRY_VERIFY(engine.state(n).initialized);
    ChannelAccess driver;driver.prepare(prefix+"permit");QTRY_VERIFY(driver.put(prefix+"permit",1));
    QVERIFY(ca.put(n->name,0));QVERIFY(ca.put(n->name,1,WriteKind::AckTransient));QVERIFY(ca.put(n->name,3,WriteKind::Acknowledge));
    QTRY_COMPARE(engine.state(n).unack,0);QTRY_VERIFY(engine.channelUpdate(protectedNode).available);
    auto result=[&]{AnalyticsQuery q;q.rangeMs=0;q.search=n->name;return AlarmAnalytics::query(analytics.snapshot(clock.elapsed(),QDateTime::currentMSecsSinceEpoch()),q).rows[0];};
    QVERIFY(ca.put(n->name,20));QTRY_COMPARE(engine.state(n).unack,2);engine.acknowledge(n);
    QTRY_COMPARE(engine.state(n).unack,0);QCOMPARE(lastAck,ObservationCause::RequestedAcknowledgement);
    QCOMPARE(result().stats.ackCount,qint64(1));QVERIFY(result().standingMs>=0);
    QVERIFY(ca.put(n->name,0));QTRY_COMPARE(engine.state(n).severity,0);
    QVERIFY(ca.put(n->name,20));QTRY_COMPARE(engine.state(n).unack,2);
    QVERIFY(ca.put(n->name,3,WriteKind::Acknowledge));QTRY_COMPARE(engine.state(n).unack,0);
    QCOMPARE(lastAck,ObservationCause::ExternalAcknowledgement);QCOMPARE(result().stats.ackCount,qint64(2));
    QVERIFY(driver.put(prefix+"permit",0));QTRY_VERIFY(!engine.channelUpdate(protectedNode).available);
    QVERIFY(driver.put(prefix+"permit",1));QTRY_VERIFY(engine.channelUpdate(protectedNode).available);
    AnalyticsQuery q;q.rangeMs=0;q.search=protectedNode->name;
    auto gap=AlarmAnalytics::query(analytics.snapshot(clock.elapsed(),QDateTime::currentMSecsSinceEpoch()),q).rows[0];
    QVERIFY(gap.stats.gaps>=1);QCOMPARE(gap.stats.activations,qint64(0));
    QVERIFY(ca.put(n->name,0));QTRY_COMPARE(engine.state(n).severity,0);
  }

  void notificationsLeaveIocAcknowledgementsAndOutputsUnchanged() {
    auto d = parseConfig("GROUP NULL notify\nCHANNEL notify " + prefix + "alarm\n$SEVRPV " + prefix + "severity\n");
    ChannelAccess ca({true}); Engine engine(d, {true}, &ca);
    auto n = d.channels()[0];
    NotificationSettings settings;
    NotificationDestination destination; destination.id = "test"; destination.name = "Test";
    destination.kind = "webhook"; destination.url = "http://127.0.0.1:1/test";
    settings.destinations << destination;
    NotificationSubscription rule; rule.id = "rule"; rule.name = "IOC notification";
    rule.configuration = "/ioc-test.alh"; rule.stages = {{"initial", 0, {"test"}}};
    settings.subscriptions << rule;
    NotificationPolicy policy; policy.configure(settings, rule.configuration); policy.enable(true);
    engine.channelUpdated = [&](const ChannelUpdate& update) { policy.observe(update, 0); };
    engine.start(); QTRY_VERIFY(engine.state(n).initialized);
    QVERIFY(ca.put(n->name, 0));
    QVERIFY(ca.put(n->name, 1, WriteKind::AckTransient));
    QVERIFY(ca.put(n->name, 3, WriteKind::Acknowledge));
    QTRY_COMPARE(engine.state(n).unack, 0);
    ChannelAccess observer; double output = -1;
    observer.number(prefix + "severity", [&](double v) { output = v; });
    QVERIFY(ca.put(n->name, 20));
    QTRY_COMPARE(engine.state(n).unack, 2); QTRY_COMPARE(output, 2.0);
    policy.tick(0); policy.tick(10000); NotificationEnvelope envelope;
    QVERIFY(policy.take(envelope, 10000)); envelope.attempts = 1;
    policy.attempted(envelope, 10000); policy.completed(envelope, true);
    QTest::qWait(100);
    QCOMPARE(engine.state(n).unack, 2); QCOMPARE(output, 2.0);
    QVERIFY(!engine.state(n).mask[AckT]);
    engine.shelve(n, 1, "Notification suppression", "tester");
    QVERIFY(policy.activeChannels().isEmpty());
    QCOMPARE(engine.state(n).unack, 2); QCOMPARE(output, 2.0);
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(engine.state(n).severity, 0);
    QVERIFY(ca.put(n->name, 3, WriteKind::Acknowledge));
    QTRY_COMPARE(engine.state(n).unack, 0);
  }

  void shelvingKeepsIocStateAndOutputs() {
    auto d = parseConfig("GROUP NULL shelfTest\nCHANNEL shelfTest " + prefix + "alarm\n$SEVRPV " + prefix + "severity\n");
    auto peerDoc = parseConfig("GROUP NULL peer\nCHANNEL peer " + prefix + "alarm\n");
    ChannelAccess ca({true}), peerCa({true});
    Engine e(d, {true}, &ca), peer(peerDoc, {true}, &peerCa);
    auto n = d.channels()[0], other = peerDoc.channels()[0];
    qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    e.start(); peer.start();
    QTRY_VERIFY(e.state(n).initialized && peer.state(other).initialized);
    QVERIFY(ca.put(n->name, 0));
    QVERIFY(ca.put(n->name, 1, WriteKind::AckTransient));
    QVERIFY(ca.put(n->name, 3, WriteKind::Acknowledge));
    QTRY_COMPARE(e.state(n).severity, 0); QTRY_COMPARE(e.state(n).unack, 0);
    ChannelAccess observer; double output = -99;
    observer.number(prefix + "severity", [&](double value) { output = value; });
    int records = 0; e.alarmLog = [&](Node*, const State&, qint64) { ++records; };
    e.shelve(n, 1, "IOC integration", "tester");
    QVERIFY(ca.put(n->name, 20));
    QTRY_COMPARE(e.state(n).severity, 2); QTRY_COMPARE(peer.state(other).severity, 2);
    QTRY_COMPARE(e.state(n).unack, 2); QTRY_COMPARE(output, 2.0);
    QCOMPARE(e.presentation(d.root.get()).severity, 0);
    QCOMPARE(peer.presentation(peerDoc.root.get()).severity, 2);
    QVERIFY(!e.audible()); QVERIFY(records > 0);
    e.acknowledge(d.root.get());
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(e.state(n).severity, 0); QTRY_COMPARE(output, 0.0);
    QCOMPARE(e.state(n).unack, 2);
    QVERIFY(!e.state(n).mask[AckT]);
    time += 60000; e.tick();
    QCOMPARE(e.presentation(d.root.get()).unack, 2); QVERIFY(e.audible());
    e.shelve(n, 1, "Other operator acknowledges", "tester"); peer.acknowledge(other);
    QTRY_COMPARE(e.state(n).unack, 0);
    e.unshelve(n); QVERIFY(!e.audible());
  }

  void legacyOutputTypes_data() {
    QTest::addColumn<int>("ackValue");
    QTest::newRow("positive") << 3; QTest::newRow("signed-short") << -1;
  }
  void legacyOutputTypes() {
    QFETCH(int, ackValue);
    auto d = parseConfig("GROUP NULL outputTypes\n$HEARTBEATPV " + prefix +
        "heartbeatString 0.1 -7\nCHANNEL outputTypes " + prefix + "typeAlarm\n$SEVRPV " +
        prefix + "severityString\n$ACKPV " + prefix + "ackString " + QString::number(ackValue) + "\n");
    ChannelAccess ca({true}), observer;
    Engine e(d, {true}, &ca); auto n = d.channels()[0];
    QString severity, acknowledgement, heartbeat;
    observer.text(prefix + "severityString", [&](QString s) { severity = s; });
    observer.text(prefix + "ackString", [&](QString s) { acknowledgement = s; });
    observer.text(prefix + "heartbeatString", [&](QString s) { heartbeat = s; });
    e.start(); QTRY_VERIFY(e.state(n).initialized);
    QVERIFY(ca.put(n->name, 0)); QTRY_COMPARE(e.state(n).severity, 0);
    QVERIFY(ca.put(n->name, 3, WriteKind::Acknowledge)); QTRY_COMPARE(e.state(n).unack, 0);
    QVERIFY(ca.put(n->name, 20)); QTRY_COMPARE(e.state(n).unack, 2);
    QTRY_COMPARE(severity, QString("2"));
    e.acknowledge(n);
    QTRY_COMPARE(acknowledgement, QString::number(static_cast<unsigned short>(ackValue)));
    QTRY_VERIFY(([&] { e.tickHeartbeat(); return heartbeat == "-7"; })());
    e.setMask(n, Mask::parse("D")); QTRY_COMPARE(severity, QString("-1"));
    // Ordinary numeric writes still preserve fractional values.
    QVERIFY(ca.put(n->name, 1.25)); QTRY_COMPARE(e.state(n).value.toDouble(), 1.25);
  }
  void alarmAndWrites() {
    auto d = parseConfig("GROUP NULL integration\n$HEARTBEATPV " + prefix +
                         "heartbeat 0.1 7\nCHANNEL integration " + prefix + "alarm\n$ACKPV " +
                         prefix + "ack 3\n$SEVRPV " + prefix + "severity\n$FORCEPV " + prefix +
                         "force -D--- 1 NE\n");
    ChannelAccess ca({true, false, false, false});
    Engine e(d, {true, false, false, false}, &ca);
    QStringList errors;
    ca.error = [&](QString s) { errors << s; };
    auto n = d.channels()[0];
    e.start();
    ChannelAccess observer;
    double ack = -1, severity = -9, beat = -1;
    observer.number(prefix + "ack", [&](double v) { ack = v; });
    observer.number(prefix + "severity", [&](double v) { severity = v; });
    observer.number(prefix + "heartbeat", [&](double v) { beat = v; });
    observer.prepare(prefix + "alarm");
    observer.prepare(prefix + "force");
    QTRY_VERIFY_WITH_TIMEOUT(e.state(n).initialized && e.state(n).severity == 0, 15000);
    QTRY_VERIFY(ack >= 0);
    QTRY_VERIFY(ca.put(prefix + "alarm", 20));
    QTRY_COMPARE(e.state(n).severity, 2);
    QTRY_COMPARE(e.state(n).unack, 2);
    QTRY_COMPARE(severity, 2.0);
    e.acknowledge(n);
    QTRY_COMPARE(e.state(n).unack, 0);
    QTRY_COMPARE(ack, 3.0);
    QTest::qWait(120);
    e.tick();
    QTRY_COMPARE(beat, 7.0);
    QTRY_VERIFY(observer.put(prefix + "force", 1));
    QTRY_VERIFY(e.state(n).mask[Disable]);
    QTRY_COMPARE(severity, -1.0);
    QVERIFY(observer.put(prefix + "force", 0));
    QTRY_VERIFY(!e.state(n).mask[Disable]);
    QVERIFY(observer.put(prefix + "alarm", 0));
    QTRY_COMPARE(e.state(n).severity, 0);
  }
  void globalLoggingIgnoresValueOnlyMonitors() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    auto n = d.channels()[0];
    int records = 0;
    e.alarmLog = [&](Node*, const State&, qint64) { ++records; };
    e.start();
    QTRY_VERIFY(e.state(n).initialized);
    QVERIFY(ca.put(n->name, 0));
    QVERIFY(ca.put(n->name, 3, WriteKind::Acknowledge));
    QVERIFY(ca.put(n->name, 1, WriteKind::AckTransient));
    QTRY_COMPARE(e.state(n).severity, 0);
    QTRY_COMPARE(e.state(n).unack, 0);
    QTRY_VERIFY(!e.state(n).mask[AckT]);
    records = 0;
    for (int value = 1; value <= 3; ++value) {
      QVERIFY(ca.put(n->name, value));
      QTRY_COMPARE(e.state(n).value.toDouble(), double(value));
      QCOMPARE(records, 0);
    }
    QVERIFY(ca.put(n->name, 6));
    QTRY_COMPARE(e.state(n).severity, 1);
    QCOMPARE(records, 1);
    QVERIFY(ca.put(n->name, 7));
    QTRY_COMPARE(e.state(n).value.toDouble(), 7.0);
    QCOMPARE(records, 1);
    e.acknowledge(n);
    QTRY_COMPARE(e.state(n).unack, 0);
    QCOMPARE(records, 2);
    QVERIFY(ca.put(n->name, 0, WriteKind::AckTransient));
    QTRY_VERIFY(e.state(n).mask[AckT]);
    QCOMPARE(records, 3);
    e.resetMask(n); // Also log IOC confirmation of an operator mask change.
    QTRY_VERIFY(!e.state(n).mask[AckT]);
    QTRY_COMPARE(records, 4);
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(e.state(n).severity, 0);
  }
  void noAckExpiryAcknowledgesIocTransient() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n$ACKPV " + prefix +
                         "ack 7\n");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0];
    e.start();
    ChannelAccess observer;
    int acks = -1;
    double ackPv = -1;
    observer.monitor(n->name, [&](Event event) { acks = event.acks; });
    observer.number(prefix + "ack", [&](double value) { ackPv = value; });
    QTRY_VERIFY(e.state(n).initialized && acks >= 0 && ackPv >= 0);
    e.noAck(d.root.get(), true);
    QVERIFY(ca.put(n->name, 20));
    QTRY_COMPARE(e.state(n).severity, 2);
    QTRY_COMPARE(acks, 2);
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(e.state(n).severity, 0);
    QCOMPARE(e.state(n).unack, 2);
    QVERIFY(!e.audible());
    time += 3600000;
    e.tick();
    QTRY_COMPARE(acks, 0);
    QTRY_COMPARE(ackPv, 7.0);
    QCOMPARE(e.state(n).unack, 0);
    QVERIFY(!e.audible());
  }
  void severityWriteRecoveryAfterConnect() {
    ChannelAccess ca({true});
    auto name = prefix + "severity";
    // No channel exists yet, so neither attempt can be submitted to CA.
    QVERIFY(!ca.put(name, 1, WriteKind::Severity));
    QVERIFY(!ca.put(name, 2, WriteKind::Severity));
    ca.prepare(name);
    ChannelAccess observer;
    int status = -1;
    double value = -9;
    observer.monitor(name, [&](Event event) {
      status = event.severity;
      value = event.value.toDouble();
    });
    QTRY_COMPARE(value, 2.0);
    ioc.kill();
    QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE_WITH_TIMEOUT(status, 4, 15000);
    QVERIFY(!ca.put(name, 3, WriteKind::Severity));
    QVERIFY(!ca.put(name, -1, WriteKind::Severity));
    startIoc();
    QTRY_COMPARE_WITH_TIMEOUT(value, -1.0, 20000);
    // Clearing the service discards pending outputs from the old document.
    auto fresh = prefix + "heartbeat";
    QVERIFY(!ca.put(fresh, 9, WriteKind::Severity));
    ca.clear();
    ca.prepare(fresh);
    double freshValue = -9;
    observer.number(fresh, [&](double v) { freshValue = v; });
    QTRY_COMPARE(freshValue, 0.0);
    QTest::qWait(200);
    QCOMPARE(freshValue, 0.0);
  }
  void severityWriteRecoveryAfterAccessChange() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n$SEVRPV " + prefix +
                         "protected\n");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    e.start();
    auto n = d.channels()[0];
    ChannelAccess driver({true});
    driver.prepare(prefix + "permit");
    int outputStatus = -1;
    double output = -9;
    driver.monitor(prefix + "protected", [&](Event event) { outputStatus = event.status; });
    driver.number(prefix + "protected", [&](double value) { output = value; });
    QTRY_VERIFY(e.state(n).initialized);
    QTRY_VERIFY(driver.put(prefix + "protected", 0));
    QTRY_COMPARE(output, 0.0);
    QTRY_VERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(outputStatus), QString("NO_WRITE_ACCESS"));
    QVERIFY(ca.put(n->name, 20));
    QTRY_COMPARE(e.state(n).severity, 2);
    QVERIFY(ca.put(n->name, 6));
    QTRY_COMPARE(e.state(n).severity, 1);
    QCOMPARE(output, 0.0);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_COMPARE(output, 1.0); // Latest severity, without another input transition.
    QCOMPARE(e.state(n).severity, 1);
    // An ordinary operator write must not be delayed and replayed later.
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(outputStatus), QString("NO_WRITE_ACCESS"));
    QVERIFY(!ca.put(prefix + "protected", 99));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_COMPARE(outputStatus, 0);
    QTest::qWait(200);
    QCOMPARE(output, 1.0);
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(e.state(n).severity, 0);
  }
  void accessRights() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "readonly");
    ChannelAccess ca({true, false, false, false});
    Engine e(d, {true, false, false, false}, &ca);
    e.start();
    auto n = d.channels()[0];
    QTRY_VERIFY_WITH_TIMEOUT(e.state(n).initialized, 10000);
    QVERIFY(!ca.put(prefix + "readonly", 1));
  }
  void readAccessRecoveryRequiresFreshAlarm_data() {
    QTest::addColumn<bool>("global");
    QTest::newRow("local") << false;
    QTest::newRow("global") << true;
  }
  void readAccessRecoveryRequiresFreshAlarm() {
    QFETCH(bool, global);
    const auto name = prefix + "readAlarm";
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name +
        "\n$SEVRCOMMAND DOWN_MAJOR stale-major\n$SEVRCOMMAND DOWN_NO_ALARM clear\n");
    ChannelAccess ca({global}); Engine e(d, {global}, &ca); auto n = d.channels()[0];
    ChannelAccess driver; driver.prepare(prefix + "readPermit");
    double control = -1;
    driver.number(prefix + "readAlarmSet", [&](double v) { control = v; });
    QTRY_VERIFY(driver.put(prefix + "readPermit", 1));
    e.start(); QTRY_VERIFY(e.state(n).initialized);
    QTRY_VERIFY(driver.put(prefix + "readAlarmSet", 20));
    QTRY_COMPARE(e.state(n).severity, 2);
    QVERIFY(driver.put(prefix + "readPermit", 0));
    QTRY_COMPARE(e.state(n).severity, 4);
    QCOMPARE(e.state(n).unack, 4); QVERIFY(e.audible());
    QVERIFY(driver.put(prefix + "readAlarmSet", 0));
    QTRY_COMPARE(control, 0.0); // The internal PP link has cleared the unreadable record.
    QStringList commands; QVector<int> observed;
    e.command = [&](QString command) { commands << command; };
    auto subscription = e.observe([&](const AlarmObservation& o) { observed << o.after.severity; });
    QVERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_COMPARE(e.state(n).severity, 0);
    QTest::qWait(100);
    QVERIFY(!observed.contains(2));
    QCOMPARE(commands, QStringList({"clear"}));
  }
  void uncancelWithoutReadAccess_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("filtered");
    QTest::newRow("local") << false << false << false;
    QTest::newRow("global") << true << false << false;
    QTest::newRow("passive-global") << true << true << false;
    QTest::newRow("filtered-local") << false << false << true;
    QTest::newRow("filtered-global") << true << false << true;
    QTest::newRow("filtered-passive-global") << true << true << true;
  }
  void uncancelWithoutReadAccess() {
    QFETCH(bool, global); QFETCH(bool, passive); QFETCH(bool, filtered);
    const auto name = prefix + "readAlarm";
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name +
        (filtered ? "\n$ALARMCOUNTFILTER -1 1" : "") +
        "\nGROUP root other\nCHANNEL other " + name + "\n");
    EngineOptions options; options.global = global; options.passive = passive;
    ChannelAccess ca(options); Engine e(d, options, &ca);
    qint64 elapsed = 100000;
    e.monotonicNow = [&] { return elapsed; };
    auto first = d.channels()[0], second = d.channels()[1];
    ChannelAccess driver; driver.prepare(prefix + "readPermit");
    double control = -1;
    driver.number(prefix + "readAlarmSet", [&](double v) { control = v; });
    QTRY_VERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_VERIFY(driver.put(prefix + "readAlarmSet", 20));
    e.start();
    QTRY_COMPARE(e.state(first).severity, 2);
    QTRY_COMPARE(e.state(second).severity, 2);
    e.modifyMask(first, Cancel, 1);
    QCOMPARE(e.state(first).severity, 0);
    QVERIFY(driver.put(prefix + "readPermit", 0));
    // The second owner proves the access callback has arrived while the first
    // was cancelled; Add must discover the denial without another rights change.
    QTRY_COMPARE(e.state(second).severity, 4);
    e.modifyMask(first, Cancel, 0);
    QCOMPARE(e.state(first).severity, 0); // No reentrant event inside applyMask.
    if (filtered) {
      QTRY_VERIFY(e.state(first).filterUntil != 0);
      QVERIFY(!e.channelUpdate(first).available);
      elapsed += 1000;
      e.tick();
    }
    QTRY_COMPARE(e.state(first).severity, 4);
    QCOMPARE(statusName(e.state(first).status), QString("NO_READ_ACCESS"));
    QCOMPARE(e.state(first).unack, 4);
    QCOMPARE(e.state(d.root.get()).counts[4], 2);
    QCOMPARE(e.state(d.root.get()).unackCounts[4], 2);
    QVERIFY(!e.channelUpdate(first).available);
    e.acknowledge(first);
    QCOMPARE(e.state(first).unack, passive ? 4 : 0);
    QCOMPARE(e.state(d.root.get()).unackCounts[4], passive ? 2 : 1);

    QVERIFY(driver.put(prefix + "readAlarmSet", 0));
    QTRY_COMPARE(control, 0.0);
    QVector<int> recovered;
    auto observation = e.observe([&](const AlarmObservation& o) { recovered << o.after.severity; });
    QVERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_COMPARE(e.state(first).severity, 0);
    QTRY_COMPARE(e.state(second).severity, 0);
    QVERIFY(!recovered.contains(2)); // Never replay the pre-Cancel MAJOR.
    QCOMPARE(e.state(d.root.get()).counts[4], 0);
    QCOMPARE(e.state(d.root.get()).counts[0], 2);

    // A later Cancel or stop must discard a deferred access check.
    e.modifyMask(first, Cancel, 1);
    QVERIFY(driver.put(prefix + "readPermit", 0));
    QTRY_COMPARE(e.state(second).severity, 4);
    e.modifyMask(first, Cancel, 0);
    e.modifyMask(first, Cancel, 1);
    ca.poll();
    QCOMPARE(e.state(first).severity, 0);
    e.modifyMask(first, Cancel, 0);
    e.stop();
    const int before = recovered.size();
    ca.poll();
    QCOMPARE(recovered.size(), before);
    QVERIFY(driver.put(prefix + "readPermit", 1));
  }
  void uncancelWithoutWriteAccess_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::newRow("local") << false << false;
    QTest::newRow("global") << true << false;
    QTest::newRow("passive-global") << true << true;
  }
  void uncancelWithoutWriteAccess() {
    QFETCH(bool, global); QFETCH(bool, passive);
    EngineOptions options; options.global = global; options.passive = passive;
    ChannelAccess ca(options), observer, driver;
    driver.prepare(prefix + "permit");
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    int severity = -1, status = -1;
    ca.monitor(prefix + "protected", [&](Event e) { severity = e.severity; status = e.status; });
    QTRY_COMPARE(severity, 0);
    ca.cancel(prefix + "protected");
    observer.prepare(prefix + "protected");
    QTRY_VERIFY(observer.canWrite(prefix + "protected"));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_VERIFY(!observer.canWrite(prefix + "protected"));
    severity = -1;
    ca.monitor(prefix + "protected", [&](Event e) { severity = e.severity; status = e.status; });
    QCOMPARE(severity, -1);
    QTRY_COMPARE(severity, global && !passive ? 4 : 0);
    if (global && !passive) QCOMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_COMPARE(severity, 0);
  }
  void changingAccessRights() {
    ChannelAccess ca({true, false, false, false});
    ca.prepare(prefix + "permit");
    int status = -1;
    ca.monitor(prefix + "protected", [&](Event event) { status = event.status; });
    QTRY_VERIFY(ca.put(prefix + "protected", 0));
    QTRY_COMPARE(status, 0);
    QTRY_VERIFY(ca.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    QVERIFY(!ca.put(prefix + "protected", 7));
    QVERIFY(ca.put(prefix + "permit", 1));
    QTRY_COMPARE(status, 0);
    QVERIFY(ca.put(prefix + "protected", 7));
  }
  void calculationInputs() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix +
                         "alarm\n$FORCEPV CALC -D--- 1 NE\n$FORCEPV_CALC A>B\n$FORCEPV_CALC_A " +
                         prefix + "force\n$FORCEPV_CALC_B 0.5");
    ChannelAccess ca;
    Engine e(d, {}, &ca);
    e.start();
    ChannelAccess driver;
    driver.prepare(prefix + "force");
    QTRY_VERIFY(driver.put(prefix + "force", 0));
    QTRY_VERIFY(e.state(d.channels()[0]).initialized);
    QVERIFY(driver.put(prefix + "force", 1));
    QTRY_VERIFY(e.state(d.channels()[0]).mask[Disable]);
    QVERIFY(driver.put(prefix + "force", 0));
    QTRY_VERIFY(!e.state(d.channels()[0]).mask[Disable]);
  }
  void twoWindowsAndReconnect() {
    auto first = std::make_unique<ChannelAccess>();
    auto second = std::make_unique<ChannelAccess>();
    int callbacks1 = 0, callbacks2 = 0, last = -1;
    QVector<int> recovered;
    QStringList numericErrors;
    second->error = [&](QString s) { numericErrors << s; };
    second->number(prefix + "force", [](double) {});
    first->monitor(prefix + "alarm", [&](Event) { ++callbacks1; });
    second->monitor(prefix + "alarm", [&](Event e) {
      ++callbacks2;
      last = e.severity;
      recovered << last;
    });
    QTRY_VERIFY(callbacks1 > 0 && callbacks2 > 0);
    first.reset();
    int prior = callbacks2;
    QVERIFY(second->put(prefix + "alarm", 20));
    QTRY_VERIFY(callbacks2 > prior);
    QTRY_COMPARE(last, 2);
    ioc.kill();
    QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE_WITH_TIMEOUT(last, 4, 15000);
    QTRY_VERIFY(numericErrors.contains("PV not connected: " + prefix + "force"));
    recovered.clear();
    startIoc();
    QTRY_COMPARE_WITH_TIMEOUT(last, 0, 20000);
    QVERIFY(!recovered.contains(2)); // Never replay the pre-disconnect MAJOR.
    second.reset();
  }
  void uncancelWhileDisconnected() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm");
    ChannelAccess ca;
    Engine e(d, {}, &ca);
    auto n = d.channels()[0];
    e.start();
    QTRY_VERIFY_WITH_TIMEOUT(e.state(n).initialized && e.state(n).severity == 0, 15000);
    e.setMask(n, Mask::parse("C----"));
    ChannelAccess observer;
    int observed = -1;
    observer.monitor(prefix + "alarm", [&](Event event) { observed = event.severity; });
    QTRY_COMPARE(observed, 0);
    ioc.kill();
    QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE_WITH_TIMEOUT(observed, 4, 15000);
    e.resetMask(n);
    QTRY_COMPARE_WITH_TIMEOUT(e.state(n).severity, 4, 5000);
    QCOMPARE(statusName(e.state(n).status), QString("NOT_CONNECTED"));
    startIoc();
    QTRY_COMPARE_WITH_TIMEOUT(e.state(n).severity, 0, 20000);
  }
  void duplicateSubscriptions() {
    ChannelAccess ca;
    int owner1 = 1, owner2 = 2, first = 0, second = 0;
    ca.monitor(prefix + "alarm", [&](Event) { ++first; }, &owner1);
    ca.monitor(prefix + "alarm", [&](Event) { ++second; }, &owner2);
    QTRY_VERIFY(first && second);
    ca.cancel(prefix + "alarm", &owner1);
    int prior1 = first, prior2 = second;
    QVERIFY(ca.put(prefix + "alarm", 20));
    QTRY_VERIFY(second > prior2);
    QCOMPARE(first, prior1);
    ca.monitor(prefix + "alarm", [&](Event) { ++first; }, &owner1);
    QTRY_VERIFY(first > prior1);
    QVERIFY(ca.put(prefix + "alarm", 0));
  }
  void repeatedMonitorReplacesCallback_data() {
    QTest::addColumn<bool>("tagged");
    QTest::newRow("default-owner") << false;
    QTest::newRow("explicit-owner") << true;
  }
  void repeatedMonitorReplacesCallback() {
    QFETCH(bool, tagged);
    ChannelAccess ca;
    int owner = 1, oldCalls = 0, newCalls = 0;
    QString lastValue;
    const void* tag = tagged ? &owner : nullptr;
    ca.monitor(prefix + "alarm", [&](Event e) { ++oldCalls; lastValue = e.value; }, tag);
    QTRY_VERIFY(oldCalls > 0);
    QVERIFY(ca.put(prefix + "alarm", 0));
    QTRY_COMPARE(lastValue, QString("0"));
    const auto before = oldCalls;
    ca.monitor(prefix + "alarm", [&](Event e) { if (e.value == "27") ++newCalls; }, tag);
    QVERIFY(ca.put(prefix + "alarm", 27));
    QTRY_VERIFY(newCalls > 0);
    QTest::qWait(200);
    QCOMPARE(oldCalls, before);
    QCOMPARE(newCalls, 1);
    QVERIFY(ca.put(prefix + "alarm", 0));
  }
  void startupForceHasOneSubscription() {
    auto d = parseConfig("GROUP NULL root\n$FORCEPV CALC ----- 1 0\n"
        "$FORCEPV_CALC 1\nCHANNEL root " + prefix + "alarm C\n");
    ChannelAccess ca;
    Engine e(d, {}, &ca);
    auto n = d.channels()[0];
    int events = 0;
    auto observer = e.observe([&](const AlarmObservation& update) {
      if (update.cause == ObservationCause::Processed && update.after.value == "29") ++events;
    });
    e.start();
    QTRY_VERIFY(e.state(n).initialized);
    QVERIFY(!e.state(n).mask[Cancel]);
    QVERIFY(ca.put(n->name, 0));
    QTRY_COMPARE(e.state(n).value, QString("0"));
    events = 0;
    QVERIFY(ca.put(n->name, 29));
    QTRY_VERIFY(events > 0);
    QTest::qWait(200);
    QCOMPARE(events, 1);
    // Cancel/Add still restores exactly one monitor for this owner.
    e.setMask(n, Mask::parse("C"));
    e.setMask(n, {});
    QTRY_VERIFY(events > 1);
    QTest::qWait(200);
    QCOMPARE(events, 2);
    QVERIFY(ca.put(n->name, 0));
  }
  void connectionTimeoutUsesMonotonicTime_data() {
    QTest::addColumn<qint64>("wallShift");
    QTest::newRow("backward") << qint64(-3600000);
    QTest::newRow("forward") << qint64(3600000);
  }
  void connectionTimeoutUsesMonotonicTime() {
    QFETCH(qint64, wallShift);
    ChannelAccess ca;
    qint64 elapsed = 1000, wall = QDateTime::currentMSecsSinceEpoch();
    ca.monotonicNow = [&] { return elapsed; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "missing_clock\n");
    Engine e(d, {}, &ca); e.now = [&] { return wall; }; e.start();
    auto n = d.channels()[0]; wall += wallShift;
    elapsed += 999; ca.poll(); QVERIFY(!e.state(n).initialized);
    ++elapsed; ca.poll(); QVERIFY(e.state(n).initialized);
    QCOMPARE(e.state(n).unack, 4); QVERIFY(e.audible());
    QCOMPARE(statusName(e.state(n).status), QString("NOT_CONNECTED"));
    // Reusing a cancelled monitor must arm a new interval on the same clock.
    e.setMask(n, Mask::parse("C")); elapsed += 100;
    e.setMask(n, {}); wall -= 2 * wallShift;
    elapsed += 999; ca.poll(); QCOMPARE(e.state(n).unack, 0);
    ++elapsed; ca.poll(); QCOMPARE(e.state(n).unack, 4);
    // Missing numeric Force PV inputs use the same deadline.
    int unavailable = 0;
    ca.number(prefix + "missing_numeric_clock", [&](double v) {
      if (!std::isfinite(v)) ++unavailable;
    });
    elapsed += 999; ca.poll(); QCOMPARE(unavailable, 0);
    ++elapsed; ca.poll(); QCOMPARE(unavailable, 1);
    ca.poll(); QCOMPARE(unavailable, 1);
  }
  void missingChannel() {
    ChannelAccess ca;
    Event last;
    int events = 0;
    ca.monitor(prefix + "does_not_exist", [&](Event e) {
      last = e;
      ++events;
    });
    QTRY_VERIFY(events > 0);
    QCOMPARE(last.severity, 4);
    QCOMPARE(statusName(last.status), QString("NOT_CONNECTED"));
  }
  void numericCancellationReleasesOwnedInputs() {
    ChannelAccess ca;
    int owner1 = 1, owner2 = 2, alarms = 0, numbers = 0;
    ca.monitor(prefix + "force", [&](Event) { ++alarms; }, &owner2);
    ca.number(prefix + "force", [&](double) { ++numbers; }, &owner2);
    QTRY_VERIFY(alarms && numbers);
    for (int i = 0; i < 20; ++i) {
      auto lifetime = std::make_shared<int>(i);
      std::weak_ptr<int> weak = lifetime;
      int callbacks = 0;
      const auto name = prefix + (i == 0 ? QString("force") : "obsolete_" + QString::number(i));
      ca.number(name, [lifetime, &callbacks](double) { ++callbacks; }, &owner1);
      if (i == 0) QTRY_VERIFY(callbacks > 0); // Remove a live input shared with another owner.
      lifetime.reset();
      QVERIFY(!weak.expired());
      ca.cancelNumbers(&owner1);
      QVERIFY(weak.expired());
    }
    int oldAlarms = alarms, oldNumbers = numbers;
    QVERIFY(ca.put(prefix + "force", 42));
    QTRY_VERIFY(alarms > oldAlarms && numbers > oldNumbers);
    // A write target prepared through a numeric connection must survive removal
    // of that input, without retaining its callback or affecting other owners.
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    int preparedEvents = 0;
    ca.number(prefix + "severity", [lifetime, &preparedEvents](double) { ++preparedEvents; }, &owner1);
    ca.prepare(prefix + "severity");
    QTRY_VERIFY(preparedEvents > 0 && ca.canWrite(prefix + "severity"));
    lifetime.reset();
    ca.cancelNumbers(&owner1);
    QVERIFY(weak.expired());
    QVERIFY(ca.put(prefix + "severity", 2, WriteKind::Severity));
    QVERIFY(ca.put(prefix + "force", 0));
  }
  void statefulForceIgnoresAlarmOnlyInputs() {
    const auto input = prefix + "statefulInput";
    ChannelAccess driver;
    driver.prepare(prefix + "readPermit");
    driver.prepare(input); driver.prepare(input + ".HIGH"); driver.prepare(input + ".PROC");
    Event observed; observed.severity = -1;
    driver.monitor(input, [&](Event event) { observed = event; });
    QTRY_VERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_VERIFY(driver.canWrite(input + ".PROC") && driver.canWrite(input + ".HIGH"));
    QTRY_COMPARE(observed.severity, 0);
    // Process first so the IOC's value-monitor baseline is initialized. Merely
    // loading VAL=5 can leave its previous posted value at zero until processing.
    QTRY_VERIFY(driver.put(input, 4)); QTRY_COMPARE(observed.value.toDouble(), 4.0);
    QVERIFY(driver.put(input, 5)); QTRY_COMPARE(observed.value.toDouble(), 5.0);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n"
        "$FORCEPV CALC D 1 0\n$FORCEPV_CALC A:=A+1;B:=B+1;B%2\n$FORCEPV_CALC_A " + input + "\n");
    ChannelAccess ca; Engine e(d, {}, &ca); auto n = d.channels()[0];
    int validInputs = 0, unavailable = 0;
    ca.number(input, [&](double v) { if (std::isfinite(v)) ++validInputs; else ++unavailable; });
    e.start(); QTRY_COMPARE(e.state(n).forceCurrent, 1.0);
    QTRY_COMPARE(validInputs, 1);
    // Change alarm status while VAL remains 5. Numeric subscriptions must stay quiet.
    QVERIFY(driver.put(input + ".HIGH", 4)); QVERIFY(driver.put(input + ".PROC", 1));
    QTRY_COMPARE(observed.severity, 1);
    QTest::qWait(200);
    QCOMPARE(validInputs, 1); QCOMPARE(e.state(n).forceCurrent, 1.0);
    QVERIFY(e.state(n).mask[Disable]);
    // A is now 6 in the CALC workspace, but a received 6 is a genuine new value.
    QVERIFY(driver.put(input, 6)); QTRY_COMPARE(e.state(n).forceCurrent, 0.0);
    QTRY_COMPARE(validInputs, 2); QVERIFY(!e.state(n).mask[Disable]);
    QVERIFY(driver.put(prefix + "readPermit", 0)); QTRY_VERIFY(unavailable > 0);
    QCOMPARE(e.state(n).forceCurrent, 0.0);
    QVERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_COMPARE(e.state(n).forceCurrent, 1.0); // Same 6 after a gap must evaluate once.
    QTRY_COMPARE(validInputs, 3); QTest::qWait(200);
    QCOMPARE(e.state(n).forceCurrent, 1.0); QCOMPARE(validInputs, 3);
  }
  void duplicatePvManualAckTSupersedesDeferredForce() {
    const auto name = prefix + "protected";
    ChannelAccess driver;
    driver.prepare(prefix + "permit");
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + "\nCHANNEL root " + name + "\n");
    ChannelAccess ca({true}); Engine e(d, {true}, &ca); e.start();
    auto a = d.channels()[0], b = d.channels()[1];
    QTRY_VERIFY(ca.canWrite(name)); QVERIFY(ca.put(name, 1, WriteKind::AckTransient));
    QTRY_COMPARE(e.state(a).observedAckT, 1); QTRY_COMPARE(e.state(b).observedAckT, 1);
    QVERIFY(driver.put(prefix + "permit", 0)); QTRY_VERIFY(!ca.canWrite(name));
    e.setMask(a, Mask::parse("T"), true); // Only this row owns the old failed force.
    QVERIFY(driver.put(prefix + "permit", 1)); QTRY_VERIFY(ca.canWrite(name));
    e.setMask(b, Mask::parse("T")); e.setMask(b, Mask::parse("-"));
    e.tick();
    QTest::qWait(250); // Allow confirmations of all submitted writes, including a bad retry.
    QCOMPARE(e.state(a).observedAckT, 1); QCOMPARE(e.state(b).observedAckT, 1);
    QVERIFY(!e.state(a).mask[AckT]); QVERIFY(!e.state(b).mask[AckT]);
    e.tick(); QTest::qWait(100); QCOMPARE(e.state(a).observedAckT, 1);
  }

  void forceCalculationApplyKeepsLiveInputs() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n"
        "$FORCEPV CALC D 2 NE\n$FORCEPV_CALC B:=B+1;B\n"
        "$FORCEPV_CALC_A " + prefix + "force\n$FORCEPV_CALC_C " + prefix + "heartbeat\n");
    auto n = d.channels()[0]; ChannelAccess ca; Engine e(d, {}, &ca); e.start();
    QTRY_COMPARE(e.state(n).forceCurrent, 1.0);
    e.configureForce(n, n->directives, false);
    QTest::qWait(250); QCOMPARE(e.state(n).forceCurrent, 1.0);
    QVERIFY(ca.put(prefix + "force", 101)); QTRY_COMPARE(e.state(n).forceCurrent, 2.0);
    QVERIFY(e.state(n).mask[Disable]);
    e.configureForce(n, n->directives, true); e.configureForce(n, n->directives, false);
    QTest::qWait(250); QCOMPARE(e.state(n).forceCurrent, 2.0);
    auto directives = n->directives;
    for (auto& directive : directives)
      if (directive.key == "FORCEPV_CALC_A") directive.value = prefix + "severity";
    e.configureForce(n, directives, false);
    QTRY_COMPARE(e.state(n).forceCurrent, 3.0); // B and the already connected C survive.
    QVERIFY(!e.state(n).mask[Disable]);
    ChannelAccess driver; driver.prepare(prefix + "force");
    QTRY_VERIFY(driver.canWrite(prefix + "force")); QVERIFY(driver.put(prefix + "force", 102));
    QTest::qWait(250); QCOMPARE(e.state(n).forceCurrent, 3.0); // Obsolete A is gone.
    QVERIFY(ca.put(prefix + "heartbeat", 103)); QTRY_COMPARE(e.state(n).forceCurrent, 4.0);
    QVERIFY(driver.put(prefix + "force", 0)); QVERIFY(ca.put(prefix + "heartbeat", 0));
  }
  void missingForceReportsFailure() {
    auto name = prefix + "missing_force";
    auto d = parseConfig("GROUP NULL root\n$FORCEPV " + name + " -D--- 1 NE\n");
    ChannelAccess ca;
    Engine e(d, {}, &ca);
    QStringList errors;
    ca.error = [&](QString s) { errors << s; };
    e.start();
    QTRY_VERIFY(errors.join('\n').contains("PV not connected: " + name));
    const auto count = errors.size();
    QTest::qWait(1200);
    QCOMPARE(errors.size(), count);
    // Unchanged Apply retains the subscription and its outage diagnostic.
    auto directives = d.root->directives;
    e.configureForce(d.root.get(), directives, false);
    QTest::qWait(1200); QCOMPARE(errors.size(), count);
    // A genuinely changed input gets a new connection deadline.
    directives[0].value = name + "_replacement -D--- 1 NE";
    e.configureForce(d.root.get(), directives, false);
    QTRY_VERIFY(errors.join('\n').contains("PV not connected: " + name + "_replacement"));
  }
  void forceReadAccessFailureAndRecovery() {
    auto name = prefix + "readprotected";
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n$FORCEPV " + name +
                         " -D--- 1 NE\n");
    ChannelAccess ca;
    Engine e(d, {}, &ca);
    QStringList errors;
    ca.error = [&](QString s) { errors << s; };
    e.start();
    ChannelAccess driver;
    driver.prepare(prefix + "readPermit");
    driver.prepare(name);
    auto n = d.channels()[0];
    QTRY_VERIFY(e.state(n).initialized);
    QTRY_VERIFY(driver.put(name, 1));
    QTRY_VERIFY(e.state(n).mask[Disable]);
    for (int outage = 0; outage < 2; ++outage) {
      QVERIFY(driver.put(prefix + "readPermit", 0));
      QTRY_COMPARE(errors.count("No read access for PV: " + name), outage + 1);
      QVERIFY(e.state(n).mask[Disable]); // A failed input must not reset the mask.
      QTest::qWait(200);
      QVERIFY(e.state(n).mask[Disable]);
      QVERIFY(driver.put(prefix + "readPermit", 1));
      QTRY_VERIFY(driver.put(name, 0));
      QTRY_VERIFY(!e.state(n).mask[Disable]);
      QVERIFY(driver.put(name, 1));
      QTRY_VERIFY(e.state(n).mask[Disable]);
    }
    QVERIFY(driver.put(name, 0));
    QTRY_VERIFY(!e.state(n).mask[Disable]);
  }
  void repeatedPvStartupAckT_data() {
    QTest::addColumn<bool>("rootT"); QTest::addColumn<bool>("branchFirst");
    QTest::addColumn<bool>("cancelled");
    QTest::addColumn<QString>("rootField"); QTest::addColumn<QString>("branchField");
    const QVector<QPair<QString, QString>> fields{{"", ""}, {"", ".VAL"},
                                                {".VAL", ""}, {".VAL", ".DESC"}};
    for (bool rootT : {false, true}) for (bool branchFirst : {false, true})
      for (bool cancelled : {false, true}) for (const auto& pair : fields)
        QTest::newRow(qPrintable(QString("T%1-branchFirst%2-cancelled%3-root%4-branch%5")
            .arg(rootT).arg(branchFirst).arg(cancelled)
            .arg(pair.first.isEmpty() ? "bare" : pair.first.mid(1),
                 pair.second.isEmpty() ? "bare" : pair.second.mid(1))))
            << rootT << branchFirst << cancelled << pair.first << pair.second;
  }
  void repeatedPvStartupAckT() {
    QFETCH(bool, rootT); QFETCH(bool, branchFirst); QFETCH(bool, cancelled);
    QFETCH(QString, rootField); QFETCH(QString, branchField);
    const auto name = prefix + "alarm";
    ChannelAccess driver; driver.prepare(name);
    double transient = -1;
    driver.number(name + ".ACKT", [&](double v) { transient = v; });
    QTRY_VERIFY(driver.canWrite(name));
    const QString direct = "CHANNEL root " + name + rootField + " " +
        (cancelled ? "C" : "-") + (rootT ? "T" : "-") + "\n";
    const QString branch = "GROUP root branch\nGROUP branch leaf\nCHANNEL leaf " + name + branchField +
        (rootT ? " -----\n" : " ---T-\n");
    auto original = parseConfig("GROUP NULL root\n" +
        (branchFirst ? branch + direct : direct + branch));
    for (bool roundTrip : {false, true}) {
      // Start opposite to the expected final value, so this requires an IOC write.
      QVERIFY(driver.put(name, rootT, WriteKind::AckTransient));
      QTRY_COMPARE(transient, double(rootT));
      auto d = roundTrip ? parseConfig(writeConfig(original)) : original;
      EngineOptions options{true, false, true}; ChannelAccess ca(options);
      ca.configureInitialAckT(d);
      Engine e(d, options, &ca); e.start();
      // ALH visits the leaf first and the direct root channel last, even if
      // declaration order is reversed or the root channel is cancelled.
      QTRY_COMPARE(transient, double(!rootT));
      for (auto n : d.channels())
        if (!n->mask[Cancel]) QTRY_COMPARE(e.state(n).observedAckT, int(!rootT));
      // A later IOC setting must not be overwritten by duplicate subscriptions.
      QVERIFY(driver.put(name, rootT, WriteKind::AckTransient));
      QTRY_COMPARE(transient, double(rootT));
      QTest::qWait(150); QCOMPARE(transient, double(rootT));
    }
  }
  void recordFieldStartupAckTAccessRecovery_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("configured");
    QTest::newRow("enable-transients") << true << false << true;
    QTest::newRow("disable-transients") << true << false << false;
    QTest::newRow("passive") << true << true << false;
    QTest::newRow("local") << false << false << false;
  }
  void recordFieldStartupAckTAccessRecovery() {
    QFETCH(bool, global); QFETCH(bool, passive); QFETCH(bool, configured);
    const auto name = prefix + "protected";
    ChannelAccess driver({true}); driver.prepare(prefix + "permit");
    int status = -1; double transient = -1;
    driver.monitor(name, [&](Event event) { status = event.status; });
    driver.number(name + ".ACKT", [&](double value) { transient = value; });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(name, !configured, WriteKind::AckTransient));
    QTRY_COMPARE(transient, double(!configured));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name +
        (configured ? " -----\n" : " ---T-\n") +
        "GROUP root branch\nCHANNEL branch " + name + ".VAL" +
        (configured ? " ---T-\n" : " -----\n"));
    EngineOptions options{global, passive, true}; ChannelAccess ca(options);
    ca.configureInitialAckT(d);
    // An auxiliary description connection must not apply the startup setting.
    bool described = false;
    ca.text(name + ".DESC", [&](QString) { described = true; });
    QTRY_VERIFY(described); QTest::qWait(100);
    QCOMPARE(transient, double(!configured));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    bool observed = false;
    ca.monitor(name + ".VAL", [&](Event) { observed = true; });
    QTRY_VERIFY(observed);
    QCOMPARE(transient, double(!configured));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    const bool active = global && !passive;
    QTest::qWait(100);
    QTRY_COMPARE(transient, double(active ? configured : !configured));
    QVERIFY(driver.put(name, !configured, WriteKind::AckTransient));
    QTRY_COMPARE(transient, double(!configured));
    // The other configured name connects only after the completed startup
    // write and an operator change. It must not replay the startup value.
    bool late = false;
    ca.monitor(name, [&](Event) { late = true; });
    QTRY_VERIFY(late); QTest::qWait(100);
    QCOMPARE(transient, double(!configured));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    QTest::qWait(100); QCOMPARE(transient, double(!configured));
  }
  void cancelledAckTransientStartup_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("passive");
    QTest::newRow("global") << true << false;
    QTest::newRow("passive") << true << true;
    QTest::newRow("local") << false << false;
  }
  void cancelledAckTransientStartup() {
    QFETCH(bool, global); QFETCH(bool, passive);
    ChannelAccess driver({true});
    const auto name = prefix + "protected";
    driver.prepare(prefix + "permit");
    int status = -1; double transient = -1;
    driver.monitor(name, [&](Event event) { status = event.status; });
    driver.number(name + ".ACKT", [&](double v) { transient = v; });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(name, 1, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 1.0);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    EngineOptions options{global, passive, true}; ChannelAccess ca(options);
    ca.setInitialAckT(name, false);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + " C--T-\n");
    Engine e(d, options, &ca); auto n = d.channels()[0]; e.start();
    QTest::qWait(150); QCOMPARE(transient, 1.0); QVERIFY(!e.state(n).initialized);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    QTest::qWait(150);
    QTRY_COMPARE(transient, global && !passive ? 0.0 : 1.0);
    QVERIFY(!e.state(n).initialized); // No alarm subscription was installed.
    QVERIFY(driver.put(name, 1, WriteKind::AckTransient)); QTRY_COMPARE(transient, 1.0);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    QTest::qWait(150); QCOMPARE(transient, 1.0);
    if (global && !passive) {
      // Adding monitoring after startup must not replay the already completed write.
      e.setMask(n, Mask::parse("---T-"));
      QTRY_VERIFY(e.state(n).initialized); QTest::qWait(150); QCOMPARE(transient, 1.0);
    }
  }

  void cancelWhileStartupAckTWaitsForAccess() {
    ChannelAccess driver({true});
    const auto name = prefix + "protected";
    driver.prepare(prefix + "permit");
    int status = -1; double transient = -1;
    driver.monitor(name, [&](Event event) { status = event.status; });
    driver.number(name + ".ACKT", [&](double v) { transient = v; });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(name, 1, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 1.0);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    EngineOptions options{true, false, true}; ChannelAccess ca(options);
    ca.setInitialAckT(name, false);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + " ---T-\n");
    Engine e(d, options, &ca); auto n = d.channels()[0]; e.start();
    QTRY_VERIFY(e.state(n).initialized);
    e.setMask(n, Mask::parse("C--T-"));
    QCOMPARE(transient, 1.0);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_COMPARE(transient, 0.0);
    QCOMPARE(e.state(n).severity, 0); // Recovery has not resumed alarm monitoring.
    QVERIFY(driver.put(name, 1, WriteKind::AckTransient));
  }
  void ackTransientStartup() {
    ChannelAccess driver;
    driver.prepare(prefix + "alarm");
    QTRY_VERIFY(driver.put(prefix + "alarm", 1, WriteKind::AckTransient));
    ChannelAccess ca({true, false, true, false});
    ca.setInitialAckT(prefix + "alarm", false);
    int transient = -1;
    ca.monitor(prefix + "alarm", [&](Event e) { transient = e.ackt; });
    QTRY_COMPARE(transient, 0);
    QVERIFY(driver.put(prefix + "alarm", 1, WriteKind::AckTransient));
  }
  void ackTransientStartupAccessRecovery_data() {
    QTest::addColumn<bool>("configured");
    QTest::addColumn<bool>("passive");
    QTest::newRow("enable-transients") << true << false;
    QTest::newRow("disable-transients") << false << false;
    QTest::newRow("passive") << false << true;
  }
  void ackTransientStartupAccessRecovery() {
    QFETCH(bool, configured);
    QFETCH(bool, passive);
    ChannelAccess driver({true});
    driver.prepare(prefix + "permit");
    int status = -1;
    double transient = -1;
    driver.monitor(prefix + "protected", [&](Event event) { status = event.status; });
    driver.number(prefix + "protected.ACKT", [&](double value) { transient = value; });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(prefix + "protected", !configured, WriteKind::AckTransient));
    QTRY_COMPARE(transient, double(!configured));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    ChannelAccess ca({true, passive, true});
    ca.setInitialAckT(prefix + "protected", configured);
    int events = 0;
    ca.monitor(prefix + "protected", [&](Event) { ++events; });
    QTRY_VERIFY(events > 0);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    if (passive)
      QTest::qWait(200);
    QTRY_COMPARE(transient, double(passive ? !configured : configured));
    // A successfully applied startup setting must not replay after later edits.
    QVERIFY(driver.put(prefix + "protected", !configured, WriteKind::AckTransient));
    QTRY_COMPARE(transient, double(!configured));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(status), QString("NO_WRITE_ACCESS"));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(status) != "NO_WRITE_ACCESS");
    QTest::qWait(200);
    QCOMPARE(transient, double(!configured));
  }
  void automaticAckTRecovery_data() {
    QTest::addColumn<bool>("cancelled");
    QTest::addColumn<bool>("defaultMatches");
    QTest::newRow("monitored") << false << false;
    QTest::newRow("cancelled") << true << false;
    QTest::newRow("monitored-matching-default") << false << true;
    QTest::newRow("cancelled-matching-default") << true << true;
  }
  void automaticAckTRecovery() {
    QFETCH(bool, cancelled);
    QFETCH(bool, defaultMatches);
    ChannelAccess driver;
    driver.prepare(prefix + "permit");
    int transient = -1;
    driver.monitor(prefix + "protected", [&](Event event) { transient = event.ackt; });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(prefix + "protected", 0, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 0);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_VERIFY(!driver.canWrite(prefix + "protected"));
    auto d =
        parseConfig("GROUP NULL root\n$FORCEPV CALC " + QString(cancelled ? "C----" : "-----") +
                    " 1 NE\n$FORCEPV_CALC 1\nCHANNEL root " + prefix + "protected " +
                    QString(defaultMatches ? "-----" : "---T-") + "\n");
    EngineOptions options{true, false, !defaultMatches};
    ChannelAccess ca(options);
    ca.setInitialAckT(prefix + "protected", defaultMatches);
    Engine e(d, options, &ca);
    e.start(); // Constant force precedes alarm connection, with no write access.
    auto n = d.channels()[0];
    QTimer timer;
    connect(&timer, &QTimer::timeout, [&] { e.tick(); });
    timer.start(20);
    QTest::qWait(200);
    QCOMPARE(e.state(n).mask[AckT], !defaultMatches);
    QCOMPARE(e.state(n).mask[Cancel], cancelled);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_COMPARE(transient, 1);
    QTRY_VERIFY(!e.state(n).mask[AckT]);
    // A successful force write must not keep overwriting later IOC settings.
    QVERIFY(driver.put(prefix + "protected", 0, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 0);
    QTest::qWait(100);
    QCOMPARE(transient, 0);
    QVERIFY(driver.put(prefix + "protected", 1, WriteKind::AckTransient));
  }
  void recordFieldRequestSupersedesForce_data() {
    QTest::addColumn<QString>("field"); QTest::addColumn<bool>("failedManual");
    for (const auto field : {".VAL", ".DESC"}) for (bool failed : {false, true})
      QTest::newRow(qPrintable(QString("%1-failed%2").arg(field).arg(failed))) << QString(field) << failed;
  }
  void recordFieldRequestSupersedesForce() {
    QFETCH(QString, field); QFETCH(bool, failedManual);
    const auto name = prefix + "protected";
    ChannelAccess driver; driver.prepare(prefix + "permit"); driver.prepare(name);
    int transient = -1;
    driver.number(name + ".ACKT", [&](double v) { transient = int(v); });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(name, 1, WriteKind::AckTransient)); QTRY_COMPARE(transient, 1);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + "\nCHANNEL root " + name + field + "\n");
    ChannelAccess ca({true}); Engine e(d, {true}, &ca); e.start();
    auto a = d.channels()[0], b = d.channels()[1];
    QTRY_COMPARE(e.state(a).observedAckT, 1); QTRY_COMPARE(e.state(b).observedAckT, 1);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_VERIFY(!ca.canWrite(a->name) && !ca.canWrite(b->name));
    e.setMask(a, Mask::parse("T"), true);
    e.setMask(b, Mask::parse(failedManual ? "T" : "-"));
    QVERIFY(!e.state(a).mask[AckT]); QVERIFY(!e.state(b).mask[AckT]);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(ca.canWrite(a->name) && ca.canWrite(b->name));
    e.tick(); QTest::qWait(200); e.tick();
    QCOMPARE(transient, 1); // The old force must not reappear through the bare record.
    if (failedManual) {
      e.setMask(b, Mask::parse("T")); QTRY_COMPARE(transient, 0);
      e.setMask(b, Mask::parse("-")); QTRY_COMPARE(transient, 1);
    }
  }
  void failedOperatorAckTWrite() {
    ChannelAccess driver({true});
    driver.prepare(prefix + "permit");
    int transient = -1;
    driver.monitor(prefix + "protected", [&](Event event) {
      if (event.ackt >= 0)
        transient = event.ackt;
    });
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(driver.put(prefix + "protected", 1, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 1);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "protected");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    e.start();
    auto n = d.channels()[0];
    QTRY_VERIFY(e.state(n).initialized);
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(e.state(n).status), QString("NO_WRITE_ACCESS"));
    QStringList errors;
    ca.error = [&](const QString& message) { errors << message; };
    e.setMask(n, Mask::parse("---T-"));
    QVERIFY(!errors.isEmpty());
    QVERIFY(!e.state(n).mask[AckT]);
    QCOMPARE(transient, 1);
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(e.state(n).status) != "NO_WRITE_ACCESS");
    e.setMask(n, Mask::parse("---T-"));
    QTRY_COMPARE(transient, 0);
    QVERIFY(e.state(n).mask[AckT]);
    QVERIFY(driver.put(prefix + "protected", 1, WriteKind::AckTransient));
    QTRY_COMPARE(transient, 1);
  }
  void readOnlyDescriptions() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "readonly\n");
    EngineOptions options{true, false, true, true};
    ChannelAccess ca(options);
    Engine e(d, options, &ca);
    e.start();
    auto n = d.channels()[0];
    QTRY_COMPARE(e.state(n).description, QString("Read-only alarm description"));
    QTRY_COMPARE(statusName(e.state(n).status), QString("NO_WRITE_ACCESS"));
  }
  void descriptionAccessRecovery() {
    ChannelAccess driver;
    driver.prepare(prefix + "permit");
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "protected\n");
    EngineOptions options{true, false, false, true};
    ChannelAccess ca(options);
    Engine e(d, options, &ca);
    e.start();
    auto n = d.channels()[0];
    QTRY_COMPARE(e.state(n).description, QString("Protected alarm description"));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_COMPARE(statusName(e.state(n).status), QString("NO_WRITE_ACCESS"));
    QCOMPARE(e.state(n).description, QString("Protected alarm description"));
    QVERIFY(driver.put(prefix + "permit", 1));
    QTRY_VERIFY(statusName(e.state(n).status) != "NO_WRITE_ACCESS");
    QCOMPARE(e.state(n).description, QString("Protected alarm description"));
  }
  void cancelledGroupSeverityOutput() {
    ChannelAccess driver;
    double actual = -9;
    driver.number(prefix + "severity", [&](double value) { actual = value; });
    QTRY_VERIFY(driver.put(prefix + "severity", 2));
    QTRY_COMPARE(actual, 2.0);
    auto d =
        parseConfig("GROUP NULL root\n$SEVRPV " + prefix + "severity\nCHANNEL root unused C----\n");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    e.start();
    QTRY_COMPARE(actual, 0.0);
    QCOMPARE(e.state(d.root.get()).severity, 0);
  }
  void passiveWrites() {
    ChannelAccess ca({true, true, false, false});
    int events = 0;
    ca.monitor(prefix + "alarm", [&](Event) { ++events; });
    QTRY_VERIFY(events > 0);
    QVERIFY(!ca.put(prefix + "alarm", 99));
  }
  void cleanupTestCase() {
    ioc.kill();
    ioc.waitForFinished();
    QFile output(QString(TEST_OUTPUT) + "/ioc.log");
    if (output.open(QIODevice::WriteOnly))
      output.write(ioc.readAllStandardOutput() + ioc.readAllStandardError());
  }
};
QTEST_GUILESS_MAIN(IocTests)
#include "test_ioc.moc"
