#include "services/channel_access.h"
#include "core/notifications.h"
#include "core/analytics.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QtTest>
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
    qint64 time = 1000; e.now = [&] { return time; };
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
    e.now = [&] { return time; };
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
    QStringList numericErrors;
    second->error = [&](QString s) { numericErrors << s; };
    second->number(prefix + "force", [](double) {});
    first->monitor(prefix + "alarm", [&](Event) { ++callbacks1; });
    second->monitor(prefix + "alarm", [&](Event e) {
      ++callbacks2;
      last = e.severity;
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
    startIoc();
    QTRY_COMPARE_WITH_TIMEOUT(last, 0, 20000);
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
    // Reusing a cancelled numeric channel must re-arm its connection check.
    const auto directives = d.root->directives;
    e.configureForce(d.root.get(), directives, false);
    QTRY_VERIFY(errors.size() > count);
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
