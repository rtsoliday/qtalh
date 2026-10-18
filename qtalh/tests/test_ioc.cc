#include "ca_repeater.h"
#include "services/pv_router.h"
#include "core/notifications.h"
#include "core/analytics.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QNetworkDatagram>
#include <QTcpServer>
#include <QThread>
#include <QtTest>
#include <cmath>
namespace alh {
// Exercise a failed subscription callback after a healthy sample without an
// IOC-specific failure trigger. Inject its arguments privately, then use the
// normal copied callback queue and dispatch path; no production injection API.
struct ChannelAccessTestAccess {
  static int channelCount(const ChannelAccess& ca) { return int(ca.channels.size()); }
  static void stopPollingTimer(ChannelAccess& ca) { ca.timer.stop(); }
  static bool failAlarm(ChannelAccess& ca, const QString& name, int status) {
    for (auto c : ca.channelsByName.value(name))
      if (c->alarm && !c->cancelled) {
        event_handler_args args{};
        args.usr = c;
        args.chid = c->id;
        args.type = DBR_STSACK_STRING;
        args.count = 1;
        args.status = status;
        ChannelAccess::update(args);
        ca.dispatch();
        return true;
      }
    return false;
  }
  static bool recheckRights(ChannelAccess& ca, const QString& name) {
    for (auto c : ca.channelsByName.value(name))
      if (c->alarm && !c->cancelled) {
        ca.rightsChanged(c);
        return true;
      }
    return false;
  }
  static bool disconnect(ChannelAccess& ca, const QString& name) {
    for (auto c : ca.channelsByName.value(name))
      if (c->alarm && !c->cancelled) {
        connection_handler_args args{};
        args.chid = c->id;
        args.op = CA_OP_CONN_DOWN;
        ChannelAccess::connection(args);
        ca.dispatch();
        return true;
      }
    return false;
  }
};
} // namespace alh
using namespace alh;
class IocTests : public QObject {
  Q_OBJECT
  TestCaRepeater repeater;
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
    ioc.setProcessChannelMode(QProcess::MergedChannels);
    // softIocPVA serves the same records over Channel Access and pvAccess.
    ioc.start(QString(EPICS_TEST_BIN) + "/softIocPVA",
              {"-m", "P=" + prefix, "-a", security.filePath("access.acf"), "-d", database});
    QVERIFY(ioc.waitForStarted());
    // Process creation precedes database initialization, especially on Windows.
    // Do not spend one-shot read deadlines waiting for the test server to start.
    QByteArray startup;
    QTRY_VERIFY_WITH_TIMEOUT((startup += ioc.readAllStandardOutput())
                                .contains("iocRun: All initialization complete"), 15000);
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
    // QSRV otherwise listens for PVA searches on host interfaces, not loopback.
    qputenv("EPICS_PVA_AUTO_ADDR_LIST", "NO");
    qputenv("EPICS_PVA_ADDR_LIST", "127.0.0.1");
    qputenv("EPICS_PVA_SERVER_PORT", QByteArray::number(port + 2));
    qputenv("EPICS_PVA_BROADCAST_PORT", QByteArray::number(port + 3));
    qputenv("EPICS_PVAS_INTF_ADDR_LIST", "127.0.0.1");
    for (const auto& setting : isolatedEpicsSettings(port))
      qputenv(setting.first.constData(), setting.second);
    QVERIFY(repeater.start(EPICS_TEST_BIN, port + 1));
    ioc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    startIoc();
  }

  void readStringsForAlarmLimits() {
    ChannelAccess ca;
    QHash<QString, QString> values;
    bool done = false;
    // Present fields arrive as strings; a nonexistent field is simply omitted.
    ca.readStrings({prefix + "alarm.RTYP", prefix + "alarm.HIHI", prefix + "alarm.HHSV",
                    prefix + "alarm.NOSUCHFIELD"},
                   [&](QHash<QString, QString> result) { values = result; done = true; }, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(values.value(prefix + "alarm.RTYP"), QString("ai"));
    QCOMPARE(values.value(prefix + "alarm.HIHI").toDouble(), 10.0);
    QCOMPARE(values.value(prefix + "alarm.HHSV"), QString("MAJOR"));
    QVERIFY(!values.contains(prefix + "alarm.NOSUCHFIELD"));
    // A read pending when the context is cleared never calls back.
    bool late = false;
    ca.readStrings({prefix + "missing:pv"}, [&](QHash<QString, QString>) { late = true; }, 200);
    ca.clear();
    QTest::qWait(400);
    QVERIFY(!late);
  }
  void inheritedSiteSettingsStayOffTheHarness() {
    // A site shell may name CA name servers, beacon destinations or server
    // ports that override the private settings above. A child run of this
    // suite must contact none of them and still reach its own IOC.
    QTcpServer nameServer;
    QVERIFY(nameServer.listen(QHostAddress::LocalHost));
    int nameServerConnections = 0;
    connect(&nameServer, &QTcpServer::newConnection, [&] {
      while (auto socket = nameServer.nextPendingConnection()) { ++nameServerConnections; socket->deleteLater(); }
    });
    QUdpSocket site;
    QVERIFY(site.bind(QHostAddress(QHostAddress::LocalHost)));
    int siteDatagrams = 0;
    connect(&site, &QUdpSocket::readyRead, [&] {
      while (site.hasPendingDatagrams()) { site.receiveDatagram(); ++siteDatagrams; }
    });
    auto freePort = [] { QUdpSocket probe; probe.bind(QHostAddress(QHostAddress::LocalHost)); return QString::number(probe.localPort()); };
    const auto sitePort = QString::number(site.localPort());
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("EPICS_CA_NAME_SERVERS", "127.0.0.1:" + QString::number(nameServer.serverPort()));
    environment.insert("EPICS_CAS_SERVER_PORT", freePort());
    environment.insert("EPICS_CAS_BEACON_PORT", sitePort);
    environment.insert("EPICS_PVAS_BROADCAST_PORT", freePort());
    environment.insert("EPICS_PVAS_BEACON_ADDR_LIST", "127.0.0.1:" + sitePort);
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), {"readStringsForAlarmLimits", "pvaMonitorAndWrites"});
    QVERIFY(child.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, 60000);
    QTest::qWait(200);
    const bool passed = child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
    QVERIFY2(passed && nameServerConnections == 0 && siteDatagrams == 0,
             qPrintable(QString("%1 name server connections, %2 site beacons, child %3\n%4")
                            .arg(nameServerConnections).arg(siteDatagrams)
                            .arg(passed ? "passed" : "failed", QString::fromLocal8Bit(child.readAll()))));
  }
  void oneShotReadDoesNotCountReconnectTwice() {
    ChannelAccess ca;
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n");
    Engine engine(doc, {}, &ca); auto n = doc.channels()[0];
    engine.start(); QTRY_VERIFY(engine.state(n).initialized);
    bool done = false;
    ca.readStrings({prefix + "alarm.RTYP", prefix + "missing:record"},
                   [&](QHash<QString, QString>) { done = true; }, 30000);
    QTest::qWait(300); // Read the available field before reconnecting it.
    QVERIFY(!done);
    ioc.kill(); QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE(engine.state(n).severity, 4);
    startIoc(); QTRY_VERIFY_WITH_TIMEOUT(engine.state(n).severity < 4, 15000);
    QTest::qWait(300);
    // Re-reading RTYP must not decrement the missing record's pending count.
    QVERIFY(!done);
    ca.clear();
  }

  void caCallbacksRunOnQtThread() {
    // Non-preemptive CA runs callbacks on its own threads while ca_poll() waits.
    // Engine, logging and widget code reached from them must still run on the
    // Qt thread, including disconnect diagnostics and one-shot reads.
    ChannelAccess ca({true});
    const auto qtThread = QThread::currentThread();
    int calls = 0, foreign = 0;
    auto note = [&] { ++calls; if (QThread::currentThread() != qtThread) ++foreign; };
    int severity = -1;
    QString description;
    double value = 0;
    bool unavailable = false, read = false;
    ca.error = [&](const QString&) { note(); };
    ca.monitor(prefix + "alarm", [&](Event e) { note(); severity = e.severity; });
    ca.number(prefix + "alarm", [&](double v) { note(); unavailable = std::isnan(v); value = v; });
    ca.text(prefix + "alarm.DESC", [&](QString s) { note(); description = s; });
    QTRY_VERIFY(severity >= 0 && severity < 4);
    ca.readStrings({prefix + "alarm.RTYP"}, [&](QHash<QString, QString>) { note(); read = true; }, 5000);
    QTRY_VERIFY(read);
    ioc.kill(); QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE(severity, 4);
    QTRY_VERIFY(unavailable);
    startIoc();
    QTRY_VERIFY_WITH_TIMEOUT(severity >= 0 && severity < 4 && !unavailable, 15000);
    QVERIFY(calls > 0);
    QCOMPARE(foreign, 0);
    Q_UNUSED(value);
    Q_UNUSED(description);
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
  void unavailableSeverityOutputReportedOncePerOutage() {
    // Each diagnostic adds an opmod record and a popup line. A chattering alarm
    // must not repeat it for every SEVRPV write while the output is unavailable;
    // ALH skips disconnected outputs and reports lost write access per event.
    const auto missing = prefix + "missing_severity", output = prefix + "protected";
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + prefix + "alarm\n$SEVRPV " + missing +
                         "\nCHANNEL root " + prefix + "typeAlarm\n$SEVRPV " + output + "\n");
    ChannelAccess ca({true});
    Engine e(d, {true}, &ca);
    QStringList errors;
    ca.error = [&](QString s) { errors << s; };
    e.start();
    auto chatter = d.channels()[0], guarded = d.channels()[1];
    ChannelAccess driver({true});
    driver.prepare(prefix + "permit");
    double value = -9;
    driver.number(output, [&](double v) { value = v; });
    QTRY_VERIFY(e.state(chatter).initialized && e.state(guarded).initialized);
    QTRY_COMPARE(value, 0.0);
    const auto toggle = [&](Node* n, int cycles) {
      for (int i = 0; i < cycles; ++i) {
        QVERIFY(ca.put(n->name, 20));
        QTRY_COMPARE(e.state(n).severity, 2);
        QVERIFY(ca.put(n->name, 0));
        QTRY_COMPARE(e.state(n).severity, 0);
      }
    };
    const auto unavailable = [&](const QString& name) {
      return errors.count("PV not connected or not writable: " + name);
    };
    toggle(chatter, 5);
    // Reported once, when the never-connected output misses its connection deadline.
    QTRY_COMPARE(unavailable(missing), 1);
    toggle(chatter, 2);
    QCOMPARE(unavailable(missing), 1);
    // Lost write access: one diagnostic, then a new one after recovery.
    for (int outage = 1; outage <= 2; ++outage) {
      QTRY_VERIFY(driver.put(prefix + "permit", 0));
      QTRY_VERIFY(!ca.canWrite(output));
      toggle(guarded, 3);
      QVERIFY(ca.put(guarded->name, 20));
      QTRY_COMPARE(e.state(guarded).severity, 2);
      QCOMPARE(unavailable(output), outage);
      QCOMPARE(value, 0.0);
      QVERIFY(driver.put(prefix + "permit", 1));
      QTRY_COMPARE(value, 2.0); // The pending severity is still replayed.
      QVERIFY(ca.put(guarded->name, 0));
      QTRY_COMPARE(value, 0.0);
    }
    QCOMPARE(unavailable(missing), 1);
    QCOMPARE(unavailable(output), 2);
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
  void failedAlarmMonitorRequiresFreshSample_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<int>("status");
    QTest::newRow("local-failed-read") << false << int(ECA_GETFAIL);
    QTest::newRow("global-failed-read") << true << int(ECA_GETFAIL);
    QTest::newRow("local-missing-payload") << false << int(ECA_NORMAL);
    QTest::newRow("global-missing-payload") << true << int(ECA_NORMAL);
  }
  void failedAlarmMonitorRequiresFreshSample() {
    QFETCH(bool, global); QFETCH(int, status);
    const auto name = prefix + "alarm";
    ChannelAccess driver;
    driver.prepare(name);
    QTRY_VERIFY(driver.put(name, 0));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + "\n");
    ChannelAccess ca({global}); Engine e(d, {global}, &ca);
    auto n = d.channels()[0];
    QStringList errors;
    ca.error = [&](const QString& text) { errors << text; };
    e.start();
    QTRY_COMPARE(e.state(n).severity, 0);
    QTest::qWait(100); // Drain the initial healthy monitor and access events.
    QVERIFY(e.channelUpdate(n).available);
    QVERIFY(ChannelAccessTestAccess::failAlarm(ca, name, status));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors[0].contains("Monitor " + name));
    QCOMPARE(severityName(e.state(n).severity), QString("ERROR"));
    QCOMPARE(statusName(e.state(n).status), QString("READ"));
    QCOMPARE(e.state(n).unack, 4);
    QVERIFY(!e.channelUpdate(n).available);
    // A rights notification without new data must not replay the old cache.
    QVERIFY(ChannelAccessTestAccess::recheckRights(ca, name));
    QCOMPARE(e.state(n).severity, 4);
    QVERIFY(!e.channelUpdate(n).available);
    QTest::qWait(100);
    QCOMPARE(e.state(n).severity, 4);
    QVERIFY(driver.put(name, 20));
    QTRY_COMPARE(e.state(n).severity, 2);
    QVERIFY(e.channelUpdate(n).available);
    QVERIFY(driver.put(name, 0));
    QTRY_COMPARE(e.state(n).severity, 0);
    QVERIFY(e.channelUpdate(n).available);
  }
  void failedAlarmMonitorSideEffects_data() {
    QTest::addColumn<bool>("global");
    QTest::newRow("local") << false;
    QTest::newRow("global") << true;
  }
  void failedAlarmMonitorSideEffects() {
    // Legacy ALH's alCaNewAlarmEvent (alh/alCA.c) only
    // prints errMsg for a failed alarm monitor callback and returns. QtALH
    // (F-713) turns it into the record status READ with severity ERROR.
    // Record the operator-visible side effects of that event.
    QFETCH(bool, global);
    const auto name = prefix + "alarm", output = prefix + "severity";
    ChannelAccess driver;
    driver.prepare(name);
    driver.prepare(output);
    QTRY_VERIFY(driver.put(name, 0));
    QTRY_VERIFY(driver.put(output, -1));
    double sevrpv = -1;
    driver.number(output, [&](double v) { sevrpv = v; });
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name +
        "\n$STATCOMMAND READ read-status-command\n$SEVRCOMMAND UP_ERROR up-error\n$SEVRPV " +
        output + "\n");
    ChannelAccess ca({global}); Engine e(d, {global}, &ca);
    auto n = d.channels()[0];
    QStringList commands, logged;
    e.command = [&](QString command) { commands << command; };
    e.alarmLog = [&](Node*, const State& s, qint64) {
      logged << statusName(s.status) + "/" + severityName(s.severity);
    };
    e.start();
    QTRY_COMPARE(e.state(n).severity, 0);
    QTest::qWait(150); // Drain the initial healthy monitor and access events.
    if (global) QTRY_COMPARE(sevrpv, 0.0);
    commands.clear(); logged.clear();
    QVERIFY(ChannelAccessTestAccess::failAlarm(ca, name, ECA_GETFAIL));
    QTest::qWait(300);
    QCOMPARE(statusName(e.state(n).status), QString("READ"));
    QCOMPARE(severityName(e.state(n).severity), QString("ERROR"));
    QVERIFY(commands.contains("read-status-command"));
    QVERIFY(commands.contains("up-error"));
    QCOMPARE(logged, QStringList({"READ/ERROR"}));
    if (global) QCOMPARE(sevrpv, 4.0);
    else QCOMPARE(sevrpv, -1.0);
  }
  void failedAlarmMonitorDiagnosticCanRemoveOwner_data() {
    QTest::addColumn<QString>("action");
    for (const auto& action : {"clear", "cancel", "replace", "destroy"})
      QTest::newRow(action) << QString(action);
  }
  void failedAlarmMonitorDiagnosticCanRemoveOwner() {
    QFETCH(QString, action);
    const auto name = prefix + "alarm";
    auto ca = std::make_unique<ChannelAccess>();
    int events = 0, replacementErrors = 0, diagnostics = 0;
    ca->monitor(name, [&](Event) { ++events; });
    QTRY_VERIFY(events > 0);
    QTest::qWait(100);
    const auto before = events;
    ca->error = [&](const QString&) {
      ++diagnostics;
      if (action == "clear") ca->clear();
      else if (action == "cancel") ca->cancel(name);
      else if (action == "destroy") ca.reset();
      else {
        ca->cancel(name);
        ca->monitor(name, [&](Event event) { if (event.severity == 4) ++replacementErrors; });
      }
    };
    QVERIFY(ChannelAccessTestAccess::failAlarm(*ca, name, ECA_GETFAIL));
    QCOMPARE(diagnostics, 1);
    QCOMPARE(events, before);
    QCOMPARE(replacementErrors, 0);
    QTest::qWait(100);
    QCOMPARE(replacementErrors, 0);
  }
  void numericDiagnosticCanDestroyService() {
    auto ca = std::make_unique<ChannelAccess>();
    qint64 elapsed = 1000;
    ca->monotonicNow = [&] { return elapsed; };
    int diagnostics = 0, numbers = 0;
    ca->number(prefix + "missing_destroy_numeric", [&](double) { ++numbers; });
    ca->error = [&](const QString&) {
      ++diagnostics;
      ca.reset();
    };
    elapsed += 1001;
    ca->poll();
    QVERIFY(!ca);
    QCOMPARE(diagnostics, 1);
    QCOMPARE(numbers, 0);
    QTest::qWait(100);
    QCOMPARE(numbers, 0);
  }
  void creationDiagnosticCanRemoveOwner_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("destroy");
    for (const auto& kind : {"alarm", "numeric", "text", "prepared"})
      for (bool destroy : {false, true})
        QTest::newRow(qPrintable(QString("%1-%2").arg(kind, destroy ? "destroy" : "clear")))
            << QString(kind) << destroy;
  }
  void creationDiagnosticCanRemoveOwner() {
    QFETCH(QString, kind); QFETCH(bool, destroy);
    auto ca = std::make_unique<ChannelAccess>();
    int diagnostics = 0, callbacks = 0;
    ca->error = [&](const QString&) {
      ++diagnostics;
      if (destroy) ca.reset();
      else ca->clear();
    };
    // CA rejects an empty name synchronously, before it can search the network.
    if (kind == "alarm") ca->monitor({}, [&](Event) { ++callbacks; });
    else if (kind == "numeric") ca->number({}, [&](double) { ++callbacks; });
    else if (kind == "text") ca->text({}, [&](QString) { ++callbacks; });
    else ca->prepare({});
    QCOMPARE(diagnostics, 1);
    if (destroy) QVERIFY(!ca);
    else QCOMPARE(ChannelAccessTestAccess::channelCount(*ca), 0);
    QTest::qWait(20);
    QCOMPARE(callbacks, 0);
  }
  void alarmCallbackCanDestroyService_data() {
    QTest::addColumn<bool>("disconnect");
    QTest::newRow("disconnect") << true;
    QTest::newRow("access-rights") << false;
  }
  void severityOutputConnectingAtStartIsNotReported() {
    // The first $SEVRPV write for a new configuration can precede the
    // output's connection. Like ALH, do not report it: the value is replayed
    // on connection. A missing output is reported once, at its connection
    // deadline (one second), not at the first alarm event; an output that
    // connects without write access is reported once when it connects.
    const auto name = prefix + "alarm";
    for (const QString kind : {"writable", "missing", "readonly"}) {
      const bool missing = kind == "missing";
      const auto output = prefix + (missing ? "missingSeverity" : kind == "readonly" ? "readonly" : "severity");
      // A disabled channel writes $SEVRPV -1 in start(), before any output connects.
      const QString mask = kind == "readonly" ? " -D---" : "";
      auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + mask + "\n$SEVRPV " + output + "\n");
      ChannelAccess ca({true}); Engine e(d, {true}, &ca); auto n = d.channels()[0];
      QElapsedTimer started;
      QVector<qint64> reports;
      ca.error = [&](const QString& s) {
        if (s == "PV not connected or not writable: " + output) reports << started.elapsed();
      };
      started.start();
      e.start();
      if (kind != "readonly") QTRY_VERIFY(e.state(n).initialized);
      QTest::qWait(1500);
      if (kind == "writable") {
        QVERIFY(reports.isEmpty());
        continue;
      }
      QCOMPARE(reports.size(), 1);
      if (missing) QVERIFY2(reports[0] >= 900, qPrintable(QString::number(reports[0])));
    }
  }
  void freshAlarmCallbackCanDestroyService() {
    // Fresh monitor updates must call a copy of the callback, as the
    // disconnect and access paths do: a closure that destroys the adapter
    // and then reads its own captures must not run from freed storage.
    // (Detected by AddressSanitizer or valgrind.)
    const auto name = prefix + "alarm";
    ChannelAccess driver;
    driver.prepare(name);
    auto ca = std::make_unique<ChannelAccess>();
    int events = 0;
    bool destroy = false;
    const QString tail(64, 'a');
    ca->monitor(name, [&, tail](Event) {
      ++events;
      if (destroy) {
        ca.reset();
        events += tail.size();
      }
    });
    QTRY_VERIFY(events > 0);
    QTest::qWait(200);
    destroy = true;
    QTRY_VERIFY(driver.put(name, 20));
    QTRY_VERIFY(!ca);
    QVERIFY(driver.put(name, 0));
    QTest::qWait(200);
  }
  void freshNumericCallbackCanCancelItself() {
    const auto name = prefix + "force";
    ChannelAccess driver;
    driver.prepare(name);
    QTRY_VERIFY(driver.put(name, 0));
    ChannelAccess ca;
    int owner = 0, values = 0;
    bool cancel = false;
    const QString tail(64, 'b');
    ca.number(name, [&, tail](double) {
      ++values;
      if (cancel) {
        ca.cancelNumbers(&owner);
        values += tail.size();
      }
    }, &owner);
    QTRY_VERIFY(values > 0);
    QTest::qWait(200);
    cancel = true;
    const int before = values;
    QVERIFY(driver.put(name, 3));
    QTRY_VERIFY(values > before);
    QVERIFY(driver.put(name, 0));
    QTest::qWait(200);
    QCOMPARE(values, before + 1 + tail.size());
  }
  void alarmCallbackCanDestroyService() {
    QFETCH(bool, disconnect);
    const auto name = prefix + "alarm";
    auto ca = std::make_unique<ChannelAccess>();
    int events = 0;
    bool destroy = false;
    ca->monitor(name, [&](Event) {
      ++events;
      if (destroy) ca.reset();
    });
    QTRY_VERIFY(events > 0);
    QTest::qWait(100);
    const auto before = events;
    destroy = true;
    // Exercise copied CA callback dispatch without disrupting the owned IOC.
    QVERIFY(disconnect ? ChannelAccessTestAccess::disconnect(*ca, name)
                       : ChannelAccessTestAccess::recheckRights(*ca, name));
    QVERIFY(!ca);
    QCOMPARE(events, before + 1);
    QTest::qWait(100);
    QCOMPARE(events, before + 1);
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
  // A failed alarm subscription event must keep a known, more specific
  // access/connection diagnosis (cabefe4). The IOC itself sends such a failed
  // event (ECA_NORDACCESS) after read access is denied, so exercise both the
  // real server ordering and a deterministic injected failure.
  void deniedReadKeepsNoReadAccess_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("deniedAtStart");
    QTest::newRow("local-revoked") << false << false;
    QTest::newRow("global-revoked") << true << false;
    QTest::newRow("local-denied-at-start") << false << true;
    QTest::newRow("global-denied-at-start") << true << true;
  }
  void deniedReadKeepsNoReadAccess() {
    QFETCH(bool, global); QFETCH(bool, deniedAtStart);
    const auto name = prefix + "readAlarm";
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + "\n");
    EngineOptions options; options.global = global;
    ChannelAccess ca(options); Engine e(d, options, &ca); auto n = d.channels()[0];
    ChannelAccess driver; driver.prepare(prefix + "readPermit");
    double control = -1;
    driver.number(prefix + "readAlarmSet", [&](double v) { control = v; });
    QTRY_VERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_VERIFY(driver.put(prefix + "readAlarmSet", 0));
    QTRY_COMPARE(control, 0.0);
    QStringList statuses;
    auto observation = e.observe([&](const AlarmObservation& o) {
      statuses << statusName(o.after.status);
    });
    if (deniedAtStart) {
      QVERIFY(driver.put(prefix + "readPermit", 0));
      QTest::qWait(200); // The access-security input has been processed.
      e.start();
    } else {
      e.start();
      QTRY_VERIFY(e.state(n).initialized);
      QTRY_COMPARE(e.state(n).severity, 0);
      QTest::qWait(100);
      QVERIFY(driver.put(prefix + "readPermit", 0));
    }
    QTRY_COMPARE(e.state(n).severity, 4);
    // Let the IOC's denied/failed subscription event arrive after the rights callback.
    QTest::qWait(1500);
    const auto finalStatus = statusName(e.state(n).status);
    QVERIFY(driver.put(prefix + "readPermit", 1));
    QCOMPARE(finalStatus, QString("NO_READ_ACCESS"));
    QVERIFY2(!statuses.contains("READ"), qPrintable(statuses.join(',')));
    QTRY_COMPARE(e.state(n).severity, 0);
  }
  void injectedFailureKeepsAccessDiagnosis_data() {
    QTest::addColumn<QString>("record");
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::addColumn<int>("failure");
    QTest::addColumn<QString>("expected");
    QTest::newRow("no-read-local") << "readAlarm" << false << false << int(ECA_NORDACCESS) << "NO_READ_ACCESS";
    QTest::newRow("no-read-global") << "readAlarm" << true << false << int(ECA_NORDACCESS) << "NO_READ_ACCESS";
    QTest::newRow("no-write-global") << "protected" << true << false << int(ECA_GETFAIL) << "NO_WRITE_ACCESS";
    QTest::newRow("no-write-passive-global") << "protected" << true << true << int(ECA_GETFAIL) << "READ";
    QTest::newRow("no-write-local") << "protected" << false << false << int(ECA_GETFAIL) << "READ";
    QTest::newRow("never-connected") << "missing_failed_monitor" << false << false << int(ECA_GETFAIL) << "NOT_CONNECTED";
  }
  void injectedFailureKeepsAccessDiagnosis() {
    QFETCH(QString, record); QFETCH(bool, global); QFETCH(bool, passive);
    QFETCH(int, failure); QFETCH(QString, expected);
    const auto name = prefix + record;
    const bool read = record == "readAlarm", write = record == "protected";
    ChannelAccess driver, observer;
    driver.prepare(prefix + "readPermit"); driver.prepare(prefix + "permit");
    driver.prepare(prefix + "readAlarmSet");
    QTRY_VERIFY(driver.put(prefix + "readPermit", 1));
    QTRY_VERIFY(driver.put(prefix + "permit", 1));
    if (read) QTRY_VERIFY(driver.put(prefix + "readAlarmSet", 0));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + name + "\n");
    EngineOptions options; options.global = global; options.passive = passive;
    ChannelAccess ca(options); Engine e(d, options, &ca); auto n = d.channels()[0];
    e.start();
    if (read) {
      QTRY_COMPARE(e.state(n).severity, 0);
      QTest::qWait(100);
      QVERIFY(driver.put(prefix + "readPermit", 0));
      QTRY_COMPARE(statusName(e.state(n).status), QString("NO_READ_ACCESS"));
    } else if (write) {
      QTRY_COMPARE(e.state(n).severity, 0);
      observer.prepare(name);
      QTRY_VERIFY(observer.canWrite(name));
      QVERIFY(driver.put(prefix + "permit", 0));
      QTRY_VERIFY(!observer.canWrite(name));
      if (global && !passive)
        QTRY_COMPARE(statusName(e.state(n).status), QString("NO_WRITE_ACCESS"));
    } else {
      QTRY_COMPARE_WITH_TIMEOUT(statusName(e.state(n).status), QString("NOT_CONNECTED"), 5000);
    }
    QTest::qWait(300); // Drain server-originated access and monitor events.
    QVERIFY(ChannelAccessTestAccess::failAlarm(ca, name, failure));
    const auto status = statusName(e.state(n).status);
    const auto severity = e.state(n).severity;
    QVERIFY(driver.put(prefix + "readPermit", 1));
    QVERIFY(driver.put(prefix + "permit", 1));
    QCOMPARE(severity, 4);
    QCOMPARE(status, expected);
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
  void sharedContextUsesOneSocketNotifier() {
    ChannelAccess first, second;
    int callbacks = 0;
    second.monitor(prefix + "alarm", [&](Event) { ++callbacks; });
    QTRY_VERIFY(callbacks > 0);
    const auto firstNotifiers = first.findChildren<QSocketNotifier*>();
    const auto secondNotifiers = second.findChildren<QSocketNotifier*>();
    QSet<qintptr> descriptors;
    for (auto notifier : firstNotifiers + secondNotifiers)
      if (notifier->isEnabled()) descriptors.insert(notifier->socket());
    QVERIFY(!descriptors.isEmpty());
    QCOMPARE(firstNotifiers.size() + secondNotifiers.size(), descriptors.size());
  }
  void sharedContextNotifierHandoff_data() {
    QTest::addColumn<bool>("destroyFirst");
    QTest::newRow("owner") << true;
    QTest::newRow("other-adapter") << false;
  }
  void sharedContextNotifierHandoff() {
    QFETCH(bool, destroyFirst);
    auto first = std::make_unique<ChannelAccess>();
    auto second = std::make_unique<ChannelAccess>();
    auto survivor = destroyFirst ? second.get() : first.get();
    int last = -1;
    survivor->monitor(prefix + "alarm", [&](Event event) { last = event.severity; });
    QTRY_VERIFY(survivor->canWrite(prefix + "alarm"));
    QVERIFY(survivor->put(prefix + "alarm", 0));
    QTRY_COMPARE(last, 0);
    ChannelAccessTestAccess::stopPollingTimer(*first);
    ChannelAccessTestAccess::stopPollingTimer(*second);
    if (destroyFirst) first.reset();
    else second.reset();
    // Event delivery must keep working without the 100 ms fallback timer,
    // whether the notifier's original owner or a later adapter was closed.
    QVERIFY(survivor->put(prefix + "alarm", 20));
    QTRY_COMPARE_WITH_TIMEOUT(last, 2, 3000);
    QVERIFY(survivor->put(prefix + "alarm", 0));
    QTRY_COMPARE_WITH_TIMEOUT(last, 0, 3000);
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
  void deadlineCallbacksMayChangeChannels_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("alarms");
    QTest::addColumn<int>("numbers");
    QTest::newRow("none") << "none" << 3 << 1;
    QTest::newRow("alarm adds channels") << "add" << 3 << 1;
    QTest::newRow("alarm clears service") << "clear" << 1 << 0;
    QTest::newRow("input cancels itself") << "cancel" << 3 << 1;
    QTest::newRow("diagnostic cancels input") << "error" << 3 << 0;
  }
  void deadlineCallbacksMayChangeChannels() {
    // Engine callbacks run from poll()'s connection-deadline loop and may
    // add, cancel or remove channels through the public interface.
    QFETCH(QString, mode); QFETCH(int, alarms); QFETCH(int, numbers);
    ChannelAccess ca;
    qint64 elapsed = 1000;
    ca.monotonicNow = [&] { return elapsed; };
    int fired = 0, received = 0, added = 0, owner = 0;
    ca.error = [&](const QString&) {
      if (mode == "error") ca.cancelNumbers(&owner);
    };
    std::function<void(Event)> alarm = [&](Event) {
      ++fired;
      if (mode == "add" && !added)
        for (; added < 64; ++added)
          ca.monitor(prefix + "missing_reentry_added" + QString::number(added), [](Event) {});
      if (mode == "clear") ca.clear();
    };
    for (int i = 0; i < 3; ++i)
      ca.monitor(prefix + "missing_reentry" + QString::number(i), alarm);
    ca.number(prefix + "missing_reentry_input", [&](double v) {
      if (!std::isfinite(v)) ++received;
      if (mode == "cancel") ca.cancelNumbers(&owner);
    }, &owner);
    elapsed += 5000; // Every connection deadline has expired.
    ca.poll();
    QCOMPARE(fired, alarms);
    QCOMPARE(received, numbers);
    ca.poll();
    QCOMPARE(fired, alarms);
    QCOMPARE(received, numbers);
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
    int unreadableSubscriptions = 0;
    ca.error = [&](const QString& message) {
      if (message == "No read access for PV: " + input) ++unreadableSubscriptions;
    };
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
    // The observer and engine use independent CA subscriptions. Wait until
    // both have seen the gap before restoring access; their callbacks can
    // arrive in separate event-loop iterations.
    QTRY_COMPARE(unreadableSubscriptions, 2);
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
        parseConfig("GROUP NULL root\n$SEVRPV " + prefix + "severity\nCHANNEL root " + prefix + "unused C----\n");
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
  void asynchronousWriteRejectionsAreReported() {
    // The IOC accepts the request and rejects the put afterwards. ALH reports
    // such CA exceptions (alCaException); CA's default handler only prints them
    // and aborts the process for error-severity replies.
    ChannelAccess ca;
    QStringList errors;
    ca.error = [&](const QString& message) { errors << message; };
    const auto name = prefix + "locked";
    ca.prepare(name);
    QTRY_VERIFY(ca.canWrite(name));
    QVERIFY(ca.put(name, 1, WriteKind::Heartbeat));
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY2(errors[0].contains(ca_message(ECA_PUTFAIL)) && errors[0].contains(name),
             qPrintable(errors[0]));
    // As in ALH, a burst ends with one notice instead of flooding diagnostics.
    for (int value = 0; value < 40; ++value)
      QVERIFY(ca.put(name, value));
    QTRY_COMPARE(errors.size(), 26);
    QTest::qWait(300);
    QCOMPARE(errors.size(), 26);
    QVERIFY2(errors.last().contains("Too many exceptions"), qPrintable(errors.last()));
  }
  void passiveWritesAllKinds_data() {
    QTest::addColumn<bool>("pva"); QTest::addColumn<int>("kind"); QTest::addColumn<QString>("record");
    const QStringList kinds = {"value", "acknowledge", "ackt", "severity", "heartbeat", "ackpv"};
    const QStringList records = {"alarm", "alarm", "alarm", "severity", "heartbeat", "ack"};
    for (bool pva : {false, true})
      for (int kind = 0; kind < kinds.size(); ++kind)
        QTest::newRow(qPrintable(QString(pva ? "pva-" : "ca-") + kinds[kind])) << pva << kind << records[kind];
  }
  void passiveWritesAllKinds() {
    // The passive services refuse every write kind themselves, independent of Engine.
    QFETCH(bool, pva); QFETCH(int, kind); QFETCH(QString, record);
    const auto write = WriteKind(kind);
    const auto target = prefix + record, name = (pva ? "pva://" : "") + target;
    ChannelAccess observer; // A writable client of the IOC record itself.
    Event ioc; ioc.severity = -1; double value = NAN;
    observer.monitor(target, [&](Event e) { ioc = e; });
    observer.number(target, [&](double v) { value = v; });
    observer.prepare(target);
    QTRY_VERIFY(ioc.severity >= 0 && !std::isnan(value) && observer.canWrite(target));
    if (write == WriteKind::Acknowledge) { QVERIFY(observer.put(target, 20)); QTRY_COMPARE(ioc.acks, 2); }
    PvRouter passive({true, true, true, false});
    Event seen; seen.severity = -1;
    passive.monitor(name, [&](Event e) { seen = e; });
    QTRY_VERIFY(seen.severity >= 0 && (write != WriteKind::Acknowledge || seen.acks == 2));
    const auto seenBefore = seen, iocBefore = ioc; const double valueBefore = value;
    const double v = write == WriteKind::Acknowledge ? 2 : write == WriteKind::AckTransient ? !seen.ackt
                   : value == 3 ? 4 : 3;
    const bool writable = passive.canWrite(name), written = passive.put(name, v, write);
    QTest::qWait(200);
    const auto seenAfter = seen, iocAfter = ioc; const double valueAfter = value;
    if (record == "alarm") { // Leave the shared record quiet and acknowledged.
      QVERIFY(observer.put(target, 0));
      QVERIFY(observer.put(target, iocBefore.ackt, WriteKind::AckTransient));
      QVERIFY(observer.put(target, 3, WriteKind::Acknowledge));
      QTRY_VERIFY(ioc.severity == 0 && ioc.acks == 0 && ioc.ackt == iocBefore.ackt);
    }
    QVERIFY(!writable); QVERIFY(!written);
    QCOMPARE(valueAfter, valueBefore);
    QCOMPARE(iocAfter.acks, iocBefore.acks); QCOMPARE(iocAfter.ackt, iocBefore.ackt);
    // pva:// acknowledgement state is local to the passive runtime.
    QCOMPARE(seenAfter.acks, seenBefore.acks); QCOMPARE(seenAfter.ackt, seenBefore.ackt);
  }
  void passiveRuntimeLeavesIocUnchanged() {
    // Passive counterpart of alarmAndWrites, with startup and runtime ACKT settings.
    auto d = parseConfig("GROUP NULL passive\n$HEARTBEATPV " + prefix + "heartbeat 0.1 7\n"
                         "CHANNEL passive " + prefix + "alarm ---T-\n$ACKPV " + prefix + "ack 3\n"
                         "$SEVRPV " + prefix + "severity\n");
    ChannelAccess observer;
    Event record; record.severity = -1;
    QMap<QString, double> outputs;
    observer.monitor(prefix + "alarm", [&](Event e) { record = e; });
    observer.prepare(prefix + "alarm");
    for (const QString output : {"ack", "severity", "heartbeat"}) {
      observer.number(prefix + output, [&outputs, output](double v) { outputs[output] = v; });
      observer.prepare(prefix + output);
      QTRY_VERIFY(observer.canWrite(prefix + output));
      QVERIFY(observer.put(prefix + output, -5));
      QTRY_COMPARE(outputs.value(output), -5.0);
    }
    QTRY_VERIFY(record.severity == 0 && observer.canWrite(prefix + "alarm"));
    QVERIFY(observer.put(prefix + "alarm", 1, WriteKind::AckTransient)); QTRY_COMPARE(record.ackt, 1);
    EngineOptions options{true, true, true, false};
    PvRouter pv(options); pv.configureInitialAckT(d);
    Engine e(d, options, &pv); auto n = d.channels()[0];
    QStringList errors; e.error = [&](const QString& s) { errors << s; };
    e.start(); QTRY_VERIFY(e.state(n).initialized);
    QVERIFY(observer.put(prefix + "alarm", 20));
    QTRY_COMPARE(e.state(n).unack, 2); QTRY_COMPARE(record.acks, 2);
    e.acknowledge(n);
    e.setMask(n, Mask::parse("-----")); e.setMask(n, Mask::parse("---T-"));
    QTest::qWait(150); e.tick(); e.tickHeartbeat(); QTest::qWait(200);
    const auto after = record; const auto written = outputs; const int unack = e.state(n).unack;
    QVERIFY(observer.put(prefix + "alarm", 0));
    QVERIFY(observer.put(prefix + "alarm", 1, WriteKind::AckTransient));
    QVERIFY(observer.put(prefix + "alarm", 3, WriteKind::Acknowledge));
    for (const QString output : {"ack", "severity", "heartbeat"}) QVERIFY(observer.put(prefix + output, 0));
    QTRY_VERIFY(record.severity == 0 && record.acks == 0 && record.ackt == 1);
    QCOMPARE(after.acks, 2); QCOMPARE(after.ackt, 1);
    QCOMPARE(written.value("ack"), -5.0); QCOMPARE(written.value("severity"), -5.0);
    QCOMPARE(written.value("heartbeat"), -5.0);
    QCOMPARE(unack, 2);
    QVERIFY(errors.contains("You can't acknowledge alarms in passive mode."));
  }
  void pvaMonitorAndWrites() {
    PvAccess pva;
    QStringList errors;
    pva.error = [&](QString s) { errors << s; };
    const auto name = "pva://" + prefix + "pvaAlarm";
    Event last;
    last.severity = -1;
    pva.monitor(name, [&](Event e) { last = e; });
    pva.prepare(name);
    QTRY_VERIFY_WITH_TIMEOUT(last.severity == 0 && pva.canWrite(name), 5000);
    // QSRV reports the record's STAT name; values use the record precision.
    QVERIFY(pva.put(name, 20));
    QTRY_COMPARE(last.severity, 2);
    QCOMPARE(last.status, 3); // HIHI
    QCOMPARE(last.value, QString("20.0"));
    QCOMPARE(last.acks, 2);
    QVERIFY(pva.put(name, 7));
    QTRY_COMPARE(last.status, 4); // HIGH
    QCOMPARE(last.severity, 1);
    QCOMPARE(last.acks, 2); // ACKT=YES keeps the higher unacknowledged severity.
    QVERIFY(pva.put(name, 0));
    QTRY_COMPARE(last.severity, 0);
    // Enumerated values display their state strings and accept index writes.
    const auto enumName = "pva://" + prefix + "pvaEnum";
    Event state;
    state.severity = -1;
    double index = -1;
    pva.monitor(enumName, [&](Event e) { state = e; });
    pva.number(enumName, [&](double v) { index = v; });
    pva.prepare(enumName);
    QTRY_COMPARE(state.value, QString("Off"));
    QTRY_VERIFY(pva.put(enumName, 1));
    QTRY_COMPARE(state.value, QString("On"));
    QCOMPARE(state.severity, 1);
    QCOMPARE(state.status, 7); // STATE
    QTRY_COMPARE(index, 1.0);
    QVERIFY(pva.put(enumName, 0));
    QTRY_COMPARE(state.value, QString("Off"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
  }
  void pvaMissingAndReconnect() {
    PvRouter pv({true});
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pva://" + prefix + "pvaAlarm\n"
                         "CHANNEL root pva://" + prefix + "missing:pv\n");
    Engine engine(d, {true}, &pv);
    auto n = d.channels()[0], missing = d.channels()[1];
    QStringList errors;
    pv.error = [&](QString s) { errors << s; };
    engine.start();
    QTRY_VERIFY_WITH_TIMEOUT(engine.state(n).initialized && engine.state(n).severity == 0, 5000);
    // A PVA channel that never connects reports ERROR, as Channel Access.
    QTRY_VERIFY(engine.state(missing).initialized);
    QCOMPARE(engine.state(missing).severity, 4);
    QCOMPARE(statusName(engine.state(missing).status), QString("NOT_CONNECTED"));
    ioc.kill(); QVERIFY(ioc.waitForFinished());
    QTRY_COMPARE_WITH_TIMEOUT(engine.state(n).severity, 4, 15000);
    QVERIFY(!pv.canWrite(n->name));
    startIoc();
    QTRY_VERIFY_WITH_TIMEOUT(engine.state(n).severity == 0, 20000);
    QTRY_VERIFY(pv.canWrite(n->name));
    QVERIFY(pv.put(n->name, 20));
    QTRY_COMPARE(engine.state(n).severity, 2);
    QVERIFY(pv.put(n->name, 0));
    QTRY_COMPARE(engine.state(n).severity, 0);
  }
  void pvaGlobalAcknowledgementIsLocal() {
    // QSRV rejects ACKS/ACKT writes, so pva:// acknowledgement stays in this
    // runtime; the record's own ACKS remains for Channel Access clients.
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pva://" + prefix + "pvaAlarm\n"
                         "$SEVRPV pva://" + prefix + "pvaSeverity\n");
    PvRouter pv({true});
    pv.configureInitialAckT(d);
    Engine engine(d, {true}, &pv);
    auto n = d.channels()[0];
    engine.start();
    double severity = -9;
    PvAccess observer;
    observer.number("pva://" + prefix + "pvaSeverity", [&](double v) { severity = v; });
    QTRY_VERIFY_WITH_TIMEOUT(engine.state(n).initialized && engine.state(n).severity == 0, 5000);
    QVERIFY(pv.put(n->name, 20));
    QTRY_COMPARE(engine.state(n).unack, 2);
    QTRY_COMPARE(severity, 2.0);
    engine.acknowledge(n);
    QTRY_COMPARE(engine.state(n).unack, 0);
    QCOMPARE(engine.state(n).severity, 2);
    ChannelAccess ca;
    QHash<QString, QString> fields;
    bool done = false;
    ca.readStrings({prefix + "pvaAlarm.ACKS"}, [&](QHash<QString, QString> v) { fields = v; done = true; });
    QTRY_VERIFY(done);
    QCOMPARE(fields.value(prefix + "pvaAlarm.ACKS"), QString("MAJOR"));
    // Disabling transient acknowledgement lowers ACKS to the current severity.
    QVERIFY(pv.put(n->name, 7));
    QTRY_COMPARE(engine.state(n).severity, 1);
    QCOMPARE(engine.state(n).unack, 1);
    engine.modifyMask(n, AckT, 1);
    QTRY_VERIFY(engine.state(n).mask[AckT]);
    QVERIFY(pv.put(n->name, 0));
    QTRY_COMPARE(engine.state(n).severity, 0);
    QTRY_COMPARE(engine.state(n).unack, 0);
    QTRY_COMPARE(severity, 0.0);
    // Configured transient masks seed the runtime ACKT of a new router.
    auto t = parseConfig("GROUP NULL root\nCHANNEL root pva://" + prefix + "pvaAlarm ---T-\n");
    PvRouter seeded({true});
    seeded.configureInitialAckT(t);
    Engine transient(t, {true}, &seeded);
    auto c = t.channels()[0];
    transient.start();
    QTRY_VERIFY_WITH_TIMEOUT(transient.state(c).initialized && transient.state(c).severity == 0, 5000);
    QVERIFY(transient.state(c).mask[AckT]);
    QVERIFY(seeded.put(c->name, 20));
    QTRY_COMPARE(transient.state(c).unack, 2);
    QVERIFY(seeded.put(c->name, 0));
    QTRY_COMPARE(transient.state(c).unack, 0);
  }
  void pvaDescriptionsReadsAndForce() {
    PvRouter pv;
    QHash<QString, QString> values;
    bool done = false;
    const auto record = "pva://" + prefix + "pvaAlarm";
    pv.readStrings({record + ".RTYP", record + ".HHSV", record + ".NOSUCHFIELD",
                    prefix + "alarm.RTYP"},
                   [&](QHash<QString, QString> v) { values = v; done = true; }, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(values.value(record + ".RTYP"), QString("ai"));
    QCOMPARE(values.value(record + ".HHSV"), QString("MAJOR"));
    QCOMPARE(values.value(prefix + "alarm.RTYP"), QString("ai"));
    QVERIFY(!values.contains(record + ".NOSUCHFIELD"));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + record + "\n$FORCEPV pva://" + prefix +
                         "pvaForce -D--- 1 0\n");
    EngineOptions options;
    options.description = true;
    Engine engine(d, options, &pv);
    auto n = d.channels()[0];
    engine.start();
    QTRY_COMPARE_WITH_TIMEOUT(engine.state(n).description, QString("QtALH PVA alarm"), 5000);
    PvAccess driver;
    driver.prepare("pva://" + prefix + "pvaForce");
    QTRY_VERIFY(driver.put("pva://" + prefix + "pvaForce", 1));
    QTRY_VERIFY(engine.state(n).mask[Disable]);
    QVERIFY(driver.put("pva://" + prefix + "pvaForce", 0));
    QTRY_VERIFY(!engine.state(n).mask[Disable]);
  }
  void pvaPassiveWrites() {
    PvAccess pva({true, true, false, false});
    int events = 0;
    pva.monitor("pva://" + prefix + "pvaAlarm", [&](Event) { ++events; });
    QTRY_VERIFY(events > 0);
    QVERIFY(!pva.canWrite("pva://" + prefix + "pvaAlarm"));
    QVERIFY(!pva.put("pva://" + prefix + "pvaAlarm", 99));
    QVERIFY(!pva.put("pva://" + prefix + "pvaAlarm", 2, WriteKind::Acknowledge));
  }
  void pvaDuplicateObservers() {
    PvAccess pv({true});
    const auto name = "pva://" + prefix + "pvaAlarm";
    int first = 0, second = 0;
    Event a, b;
    a.severity = b.severity = -1;
    pv.monitor(name, [&](Event e) { a = e; }, &first);
    QTRY_COMPARE(a.severity, 0);
    pv.monitor(name, [&](Event e) { b = e; }, &second);
    QTRY_COMPARE(b.severity, 0);
    QVERIFY(pv.put(name, 20));
    QTRY_COMPARE(a.acks, 2);
    QTRY_COMPARE(b.acks, 2);
    QVERIFY(pv.put(name, 2, WriteKind::Acknowledge));
    QTRY_COMPARE(a.acks, 0);
    QTRY_COMPARE(b.acks, 0);
    pv.cancel(name, &first);
    QVERIFY(pv.put(name, 0));
    QTRY_COMPARE(b.severity, 0);
    QCOMPARE(a.severity, 2);
    pv.monitor(name, [&](Event e) { a = e; }, &first);
    QTRY_COMPARE(a.severity, 0);
    pv.cancel(name, &first);
    pv.cancel(name, &second);
    pv.monitor(name, [&](Event e) { b = e; }, &second);
    QVERIFY(pv.put(name, 7));
    QTRY_COMPARE(b.severity, 1);
    QCOMPARE(a.severity, 0);
    QVERIFY(pv.put(name, 0));
    QTRY_COMPARE(b.severity, 0);
  }
  void cleanupTestCase() {
    ioc.kill();
    ioc.waitForFinished();
    QFile output(QString(TEST_OUTPUT) + "/ioc.log");
    if (output.open(QIODevice::WriteOnly))
      output.write(ioc.readAllStandardOutput());
  }
};
QTEST_GUILESS_MAIN(IocTests)
#include "test_ioc.moc"
