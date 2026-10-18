#include "services/notifications.h"
#include "test_compat.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif
using namespace alh;
namespace {
NotificationSettings settings() {
  NotificationSettings s;
  NotificationDestination d;
  d.id = "ops";
  d.name = "Operators";
  d.kind = "webhook";
  d.url = "http://127.0.0.1:1/test";
  s.destinations << d;
  NotificationSubscription r;
  r.id = "rule";
  r.name = "Major alarms";
  r.configuration = notificationConfiguration("/test.alh");
  r.stages = {{"initial", 60, {"ops"}}, {"escalate", 600, {"ops"}}};
  s.subscriptions << r;
  return s;
}
ChannelUpdate alarm(QString id = "pv") {
  ChannelUpdate s;
  s.identity = id;
  s.pv = id;
  s.path = "Root/" + id;
  s.ancestors = QStringList{id, "root"};
  s.severity = s.unack = 2;
  s.initialized = s.available = true;
  s.observedAt = 1700000000000;
  return s;
}
void configure(NotificationPolicy& p, NotificationSettings s = settings()) {
  p.configure(s, notificationConfiguration("/test.alh"));
  p.enable(true);
}
struct Receiver : QTcpServer {
  QVector<QByteArray> requests;
  QVector<int> codes;
  QByteArray extra;
  bool hold = false;
  int connections = 0;
  Receiver() {
    listen(QHostAddress::LocalHost);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto socket = nextPendingConnection();
        ++connections;
        auto bytes = std::make_shared<QByteArray>();
        auto done = std::make_shared<bool>(false);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes, done] {
          *bytes += socket->readAll();
          if (*done)
            return;
          int split = bytes->indexOf("\r\n\r\n");
          if (split < 0)
            return;
          auto headers = bytes->left(split);
          int length = 0;
          for (const auto& line : headers.split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes->size() < split + 4 + length)
            return;
          *done = true;
          requests << *bytes;
          if (hold)
            return;
          int code = codes.isEmpty() ? 204 : codes.takeFirst();
          socket->write("HTTP/1.1 " + QByteArray::number(code) +
                        " Result\r\nContent-Length: 0\r\nConnection: close\r\n" + extra + "\r\n");
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString url() const {
    return QString("http://127.0.0.1:%1/hook?secret=do-not-log").arg(serverPort());
  }
};
} // namespace
class NotificationTests : public QObject {
  Q_OBJECT
private slots:
  void delaysStagesOnce() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    NotificationEnvelope e;
    p.tick(59999);
    QVERIFY(!p.take(e, 59999));
    p.tick(60000);
    p.tick(69999);
    QVERIFY(!p.take(e, 69999));
    p.tick(70000);
    QVERIFY(p.take(e, 70000));
    QCOMPARE(e.stage, QString("initial"));
    e.attempts = 1;
    p.attempted(e, 70000);
    p.completed(e, true);
    a.severity = a.unack = 3;
    p.observe(a, 80000);
    p.tick(590000);
    QVERIFY(!p.take(e, 590000));
    p.tick(600000);
    p.tick(610000);
    QVERIFY(p.take(e, 610000));
    QCOMPARE(e.stage, QString("escalate"));
    p.completed(e, true);
    p.tick(9999999);
    QVERIFY(!p.take(e, 9999999));
  }
  void eligibilityAndCancellation() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    a.initialized = false;
    p.observe(a, 0);
    p.tick(100000);
    QCOMPARE(p.activeChannels().size(), 0);
    a.initialized = true;
    p.observe(a, 100000);
    a.severity = 0;
    p.observe(a, 110000);
    p.tick(160000);
    p.tick(170000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 170000));
    QCOMPARE(e.alarms.first().state.severity, 0);
    a.unack = 0;
    p.observe(a, 170001);
    QVERIFY(!p.revalidate(e));
    p.completed(e, false);
    QCOMPARE(p.activeChannels().size(), 0);
  }
  void shelvingFreshDelay() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    a.suppressed = true;
    p.observe(a, 65000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(!p.take(e, 70000));
    a.suppressed = false;
    p.observe(a, 80000);
    p.tick(139999);
    QVERIFY(!p.take(e, 139999));
    p.tick(140000);
    p.tick(150000);
    QVERIFY(p.take(e, 150000));
  }
  void cooldownAcrossEpisodesAndPause() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    e.attempts = 1;
    p.attempted(e, 70000);
    p.completed(e, true);
    p.enable(false);
    p.enable(true);
    p.observe(a, 100000);
    p.tick(160000);
    p.tick(170000);
    QVERIFY(!p.take(e, 170000));
    p.tick(1870000);
    p.tick(1880000);
    QVERIFY(p.take(e, 1880000));
  }
  void expiredAndObsoleteDeadlinesAreReleased() {
    NotificationPolicy p;
    auto s = settings();
    configure(p, s);
    NotificationEnvelope e;
    e.subscription = "rule"; e.stage = "initial"; e.destination = "ops"; e.attempts = 1;
    for (int i = 0; i < 1000; ++i) {
      e.alarms = {{QString::number(i), alarm("pv" + QString::number(i))}};
      p.attempted(e, i);
    }
    QCOMPARE(p.retainedDeadlines(), 1001);
    p.enable(false);
    p.tick(10001); // Destination deadline remains until the latest attempt expires.
    QCOMPARE(p.retainedDeadlines(), 1001);
    p.tick(11000);
    QCOMPARE(p.retainedDeadlines(), 1000);
    p.tick(1801000);
    QCOMPARE(p.retainedDeadlines(), 0);
    p.attempted(e, 2000000);
    QCOMPARE(p.retainedDeadlines(), 2);
    configure(p, s); // Unchanged settings preserve an active cooldown.
    QCOMPARE(p.retainedDeadlines(), 2);
    p.reconcile({e.alarms[0].state});
    QCOMPARE(p.retainedDeadlines(), 2);
    p.reconcile({alarm("replacement")});
    QCOMPARE(p.retainedDeadlines(), 1);
    e.alarms = {{"replacement", alarm("replacement")}};
    p.attempted(e, 2000001);
    QCOMPARE(p.retainedDeadlines(), 2);
    s.subscriptions.clear();
    p.configure(s, notificationConfiguration("/test.alh"));
    QCOMPARE(p.retainedDeadlines(), 1);
    s.destinations.clear();
    p.configure(s, notificationConfiguration("/test.alh"));
    QCOMPARE(p.retainedDeadlines(), 0);
  }
  void resolutionDestinationsAndLateAcceptance() {
    auto s = settings();
    s.subscriptions[0].resolution = true;
    NotificationPolicy p;
    configure(p, s);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    a.unack = 0;
    p.observe(a, 70001);
    p.completed(e, true);
    p.tick(70002);
    p.tick(80002);
    QVERIFY(p.take(e, 80002));
    QCOMPARE(e.kind, QString("resolution"));
    QVERIFY(notificationText(e).contains("alarm was still active"));
    p.completed(e, true);
    p.tick(90002);
    QVERIFY(!p.take(e, 90002));
  }
  void resolutionKeepsEndedEpisode_data() {
    QTest::addColumn<int>("severity");
    QTest::addColumn<bool>("reload");
    QTest::newRow("cleared") << 0 << false;
    QTest::newRow("acknowledged-active") << 2 << false;
    QTest::newRow("cleared-reload") << 0 << true;
    QTest::newRow("acknowledged-active-reload") << 2 << true;
  }
  void resolutionKeepsEndedEpisode() {
    QFETCH(int, severity);
    QFETCH(bool, reload);
    auto s = settings();
    s.subscriptions[0].resolution = true;
    NotificationPolicy p;
    configure(p, s);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    const auto episode = e.alarms[0].episode;
    p.completed(e, true);
    a.severity = severity;
    a.unack = 0;
    a.value = "resolved value";
    a.observedAt = 71000;
    p.observe(a, 71000);
    p.tick(71000);
    auto recurring = alarm();
    recurring.severity = recurring.unack = 3;
    recurring.value = "new alarm";
    recurring.observedAt = 72000;
    p.observe(recurring, 72000); // Recurs during the resolution batch window.
    p.tick(81000);
    QVERIFY(p.take(e, 81000));
    QCOMPARE(e.kind, QString("resolution"));
    const auto message = e.id;
    e.ready = 90000;
    p.retry(e);
    if (reload) {
      auto initial = recurring;
      initial.initialized = false;
      p.reconcile({initial});
      QVERIFY(!p.take(e, 90000));
      p.observe(recurring, 90001);
    }
    QVERIFY(p.take(e, 90001));
    QCOMPARE(e.id, message);
    QCOMPARE(e.alarms[0].episode, episode);
    QCOMPARE(e.alarms[0].state.unack, 0);
    QCOMPARE(e.alarms[0].state.severity, severity);
    QCOMPARE(e.alarms[0].state.value, QString("resolved value"));
    QCOMPARE(e.alarms[0].state.observedAt, qint64(71000));
    QVERIFY(notificationText(e).contains(severity ? "alarm was still active" : "Alarm cleared"));
    p.completed(e, true);
    p.tick(132000);
    p.tick(142000);
    QVERIFY(p.take(e, 142000));
    QCOMPARE(e.kind, QString("alarm"));
    QVERIFY(e.alarms[0].episode != episode);
    QCOMPARE(e.alarms[0].state.unack, 3);
  }
  void resolutionOffAndSuppressed() {
    for (bool suppressed : {false, true}) {
      NotificationPolicy p;
      auto s = settings();
      s.subscriptions[0].resolution = suppressed;
      configure(p, s);
      auto a = alarm();
      p.observe(a, 0);
      p.tick(60000);
      p.tick(70000);
      NotificationEnvelope e;
      QVERIFY(p.take(e, 70000));
      p.completed(e, true);
      a.unack = 0;
      a.suppressed = suppressed;
      p.observe(a, 71000);
      p.tick(80000);
      p.tick(90000);
      QVERIFY(!p.take(e, 90000));
    }
  }
  void reloadAndEditedRules() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    a.initialized = false;
    p.reconcile({a});
    NotificationEnvelope e;
    QVERIFY(!p.take(e, 90000));
    a.initialized = true;
    p.observe(a, 90001);
    QVERIFY(p.take(e, 90001));
    p.completed(e, true);
    auto s = settings();
    s.subscriptions[0].name = "Edited";
    p.configure(s, notificationConfiguration("/test.alh"));
    p.observe(a, 100000);
    p.tick(159999);
    QVERIFY(!p.take(e, 159999));
    p.tick(160000);
    p.tick(170000);
    QVERIFY(p.take(e, 170000));
    QCOMPARE(e.subscriptionName, QString("Edited"));
  }
  void editsAndConfigurationSwitchKeepCooldown() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    e.attempts = 1;
    p.attempted(e, 70000); // Initial stage is suppressed until 1870000.
    p.completed(e, true);
    auto s = settings();
    s.subscriptions[0].name = "Edited";
    p.configure(s, notificationConfiguration("/test.alh"));
    p.observe(a, 100000); // Edited subscriptions restart their episodes...
    p.tick(160000);
    p.tick(170000);
    QVERIFY(!p.take(e, 170000)); // ...but keep repeat suppression.
    s.destinations[0].name = "Operators (edited)";
    p.configure(s, notificationConfiguration("/test.alh"));
    p.observe(a, 200000);
    p.tick(260000);
    p.tick(270000);
    QVERIFY(!p.take(e, 270000));
    p.enable(false);
    p.configure(s, notificationConfiguration("/other.alh"));
    p.configure(s, notificationConfiguration("/test.alh"));
    p.enable(true);
    p.observe(a, 300000);
    p.tick(360000);
    p.tick(370000);
    QVERIFY(!p.take(e, 370000));
    p.tick(900000);
    p.tick(910000);
    QVERIFY(p.take(e, 910000)); // Other stages are not suppressed.
    QCOMPARE(e.stage, QString("escalate"));
    p.completed(e, true);
    p.tick(1870000);
    p.tick(1880000);
    QVERIFY(p.take(e, 1880000));
    QCOMPARE(e.stage, QString("initial"));
    QCOMPARE(e.subscriptionName, QString("Edited"));
  }
  void pvWildcards_data() {
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("pv");
    QTest::addColumn<bool>("matches");
    QTest::newRow("default-slash") << "*" << "system/pv" << true;
    QTest::newRow("multiple-separators") << "system*" << "system/area/pv" << true;
    QTest::newRow("question-slash") << "system?pv" << "system/pv" << true;
    QTest::newRow("literal-slash") << "system/*" << "system/pv" << true;
    QTest::newRow("anchored") << "system/*" << "other/system/pv" << false;
    QTest::newRow("literal-backslash") << "system\\*" << "system\\pv" << true;
    QTest::newRow("distinct-separators") << "system\\*" << "system/pv" << false;
    QTest::newRow("range") << "system/[a-c]?" << "system/b1" << true;
    QTest::newRow("excluded-range") << "system/[a-c]?" << "system/d1" << false;
    QTest::newRow("negated-class") << "[!x]*" << "/pv" << true;
    QTest::newRow("excluded-class") << "[!x]*" << "x/pv" << false;
    QTest::newRow("literal-class-slash") << "system[/]pv" << "system/pv" << true;
    QTest::newRow("literal-bracket") << "pv[]]" << "pv]" << true;
    QTest::newRow("literal-regex") << "pv.+(1)" << "pv.+(1)" << true;
    QTest::newRow("no-regex-expansion") << "pv.+(1)" << "pvXYZ1" << false;
  }
  void pvWildcards() {
    QFETCH(QString, pattern);
    QFETCH(QString, pv);
    QFETCH(bool, matches);
    auto s = settings();
    s.subscriptions[0].wildcard = pattern;
    NotificationPolicy p;
    configure(p, s);
    auto a = alarm(pv);
    QCOMPARE(p.matchCount(s.subscriptions[0], {a}), int(matches));
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QCOMPARE(p.take(e, 70000), matches);
    if (matches) QCOMPARE(e.alarms[0].state.pv, pv);
  }
  void scopesAndDuplicateReload() {
    NotificationPolicy p;
    auto s = settings();
    s.subscriptions[0].scope = "root";
    s.subscriptions[0].wildcard = "vac:*";
    configure(p, s);
    QCOMPARE(p.matchCount(s.subscriptions[0], {alarm("vac:1"), alarm("power:1")}), 1);
    auto a = alarm("vac:1");
    p.observe(a, 0);
    p.reconcile({a, a});
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(!p.take(e, 70000));
  }
  void retryIdentityAndRevalidation() {
    NotificationPolicy p;
    configure(p);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    auto id = e.id;
    e.attempts = 1;
    p.attempted(e, 70000);
    e.ready = 100000;
    p.retry(e);
    QVERIFY(!p.take(e, 99999));
    QVERIFY(p.take(e, 100000));
    QCOMPARE(e.id, id);
    a.unack = 0;
    p.observe(a, 100001);
    QVERIFY(!p.revalidate(e));
  }
  void batchingAndTenThousandChannels() {
    NotificationPolicy p;
    auto s = settings();
    s.subscriptions[0].stages.resize(1);
    configure(p, s);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 10000; ++i)
      p.observe(alarm(QString("pv%1").arg(i)), 0);
    p.tick(60000);
    p.tick(70000);
    QCOMPARE(p.pending(), 100);
    NotificationEnvelope e;
    int count = 0;
    while (p.take(e, 70000)) {
      QVERIFY(e.alarms.size() <= 100);
      QVERIFY(notificationPayload(e).size() <= 256 * 1024);
      count += e.alarms.size();
      p.completed(e, true);
    }
    QCOMPARE(count, 10000);
    qInfo() << "10,000 notification channels, ms:" << timer.elapsed();
    QVERIFY(timer.elapsed() < 10000);
  }
  void queuePressureRetainsDueWork() {
    NotificationPolicy p;
    auto s = settings();
    s.subscriptions[0].stages.resize(1);
    configure(p, s);
    for (int i = 0; i < 250; ++i)
      p.observe(alarm(QString::number(i)), 0);
    p.tick(60000, 1);
    p.tick(70000, 1);
    QCOMPARE(p.pending(), 1);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    QCOMPARE(e.alarms.size(), 100);
    p.completed(e, true);
    p.tick(70001, 1);
    QCOMPARE(p.pending(), 1);
    QVERIFY(p.take(e, 70001));
    QCOMPARE(e.alarms.size(), 100);
    p.completed(e, true);
    p.tick(70002, 1);
    QVERIFY(p.take(e, 70002));
    QCOMPARE(e.alarms.size(), 50);
  }
  void settingsRoundTripConflictCorruption() {
    QTemporaryDir dir;
    NotificationStore a(dir.filePath("settings.json")), b(a.path());
    a.load();
    b.load();
    a.save(settings());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, b.save(settings()));
    auto loaded = b.load();
    QCOMPARE(loaded.subscriptions.size(), 1);
    b.save(loaded);
    QFile f(a.path());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("broken");
    f.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, a.load());
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, a.save(settings()));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, a.load());
  }
  void headerAndUrlSafety() {
    auto s = settings();
    auto d = s.destinations[0];
    d.kind = "email";
    d.program = "sendmail";
    d.sender = "operator@example.invalid";
    d.recipients = QStringList{"user@example.invalid"};
    NotificationEnvelope e;
    e.kind = "test";
    e.subscriptionName = "name\r\nBcc: bad";
    auto message = NotificationService::mailMessage(d, e);
    QVERIFY(!message.contains("\r\nBcc:"));
    d.sender += "\nBcc: bad";
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, NotificationService::mailMessage(d, e));
    d = s.destinations[0];
    d.url = "http://example.invalid";
    QVERIFY(!NotificationService::validateWebhook(d).isEmpty());
    d.url = "https://user:secret@example.invalid";
    QVERIFY(!NotificationService::validateWebhook(d).isEmpty());
    d.url = "https://example.invalid/hook";
    QVERIFY(NotificationService::validateWebhook(d).isEmpty());
    d.bearerEnvironment = "QTALH_NOTIFICATION_TEST_MISSING";
    qunsetenv("QTALH_NOTIFICATION_TEST_MISSING");
    QVERIFY(!NotificationService::validateWebhook(d).isEmpty());
  }
  void webhookRetryAndSecrets() {
    QTemporaryDir dir;
    Receiver receiver;
    receiver.codes = {503, 204};
    receiver.extra = "Retry-After: 40\r\n";
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc, {});
    NotificationService service(nullptr, dir.filePath("settings.json"));
    qint64 now = 0;
    service.monotonicNow = [&] { return now; };
    service.attach(&engine, doc.channels(), "/test.alh");
    auto s = settings();
    s.destinations[0].url = receiver.url();
    s.subscriptions[0].stages.resize(1);
    service.apply(s);
    service.enable(true);
    engine.event(doc.channels()[0], {3, 2, 2, 1, "value"});
    now = 60000;
    service.pump();
    now = 70000;
    service.pump();
    QTRY_COMPARE(receiver.requests.size(), 1);
    QTRY_VERIFY(service.activity.join('\n').contains("Queued retry"));
    now = 109999;
    service.pump();
    QCOMPARE(receiver.requests.size(), 1);
    now = 110000;
    service.pump();
    QTRY_COMPARE(receiver.requests.size(), 2);
    QTRY_VERIFY(service.activity.join('\n').contains("Accepted/submitted"));
    auto payload = [](const QByteArray& request) {
      return QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
    };
    QCOMPARE(payload(receiver.requests[0])["messageId"],
             payload(receiver.requests[1])["messageId"]);
    QVERIFY(!service.activity.join('\n').contains("do-not-log"));
    QCOMPARE(engine.state(doc.channels()[0]).unack, 2);
  }
  void redirectAndTimeout() {
    for (bool timeout : {false, true}) {
      QTemporaryDir dir;
      Receiver receiver;
      receiver.codes = {302};
      receiver.extra = "Location: http://127.0.0.1:1/never\r\n";
      receiver.hold = timeout;
      NotificationService service(nullptr, dir.filePath("settings.json"));
      service.transportTimeoutMs = 100;
      auto s = settings();
      s.destinations[0].url = receiver.url();
      service.apply(s);
      service.sendTest("ops");
      QTRY_COMPARE(receiver.requests.size(), 1);
      QTRY_VERIFY(service.activity.join('\n').contains("Failed:"));
      QCOMPARE(receiver.connections, 1);
    }
  }
  void mailSubmission() {
    QTemporaryDir dir;
    NotificationService service(nullptr, dir.filePath("settings.json"));
    auto s = settings();
    auto& d = s.destinations[0];
    d.kind = "email";
    d.program = QCoreApplication::applicationFilePath();
    d.arguments = QStringList{"--fake-mail", dir.filePath("message.eml"), "0"};
    d.sender = "qtalh@example.invalid";
    d.recipients = QStringList{"ops@example.invalid"};
    service.apply(s);
    service.sendTest("ops");
    QTRY_VERIFY(service.activity.join('\n').contains("Accepted/submitted"));
    QFile f(dir.filePath("message.eml"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto mail = f.readAll();
    QVERIFY(mail.startsWith("From: qtalh@example.invalid\r\n"));
    auto body = QByteArray::fromBase64(mail.mid(mail.indexOf("\r\n\r\n") + 4));
    QVERIFY(body.contains("QTALH:TEST"));
    QVERIFY(!service.activity.join('\n').contains("mailer-secret"));
  }
  void mailDateHeaderIsRfc5322() {
    // RFC 5322 day and month names are English whatever the host locale; Qt 5
    // localizes "ddd"/"MMM" with QLocale::system(), so also run under German.
    if (qEnvironmentVariableIsEmpty("QTALH_TEST_MAIL_LOCALE_CHILD")) {
      QProcess child;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert("QTALH_TEST_MAIL_LOCALE_CHILD", "1");
      for (const char* name : {"LC_ALL", "LC_TIME", "LANG"})
        environment.insert(name, "de_DE.UTF-8");
      child.setProcessEnvironment(environment);
      child.start(QCoreApplication::applicationFilePath(), {"mailDateHeaderIsRfc5322"});
      QVERIFY(child.waitForStarted());
      if (!child.waitForFinished(30000)) {
        child.kill();
        child.waitForFinished();
        QFAIL("Localized mail Date header test timed out");
      }
      const auto output = child.readAllStandardOutput() + child.readAllStandardError();
      QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
               output.constData());
    }
    QTemporaryDir dir;
    NotificationService service(nullptr, dir.filePath("settings.json"));
    auto s = settings();
    auto& d = s.destinations[0];
    d.kind = "email";
    d.program = QCoreApplication::applicationFilePath();
    d.arguments = QStringList{"--fake-mail", dir.filePath("message.eml"), "0"};
    d.sender = "qtalh@example.invalid";
    d.recipients = QStringList{"ops@example.invalid"};
    service.apply(s);
    service.sendTest("ops");
    QTRY_VERIFY(service.activity.join('\n').contains("Accepted/submitted"));
    QFile f(dir.filePath("message.eml"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto mail = f.readAll();
    int start = mail.indexOf("\r\nDate: ");
    QVERIFY(start >= 0);
    start += 2;
    auto date = QString::fromLatin1(mail.mid(start, mail.indexOf("\r\n", start) - start));
    QRegularExpression rfc5322("^Date: (Mon|Tue|Wed|Thu|Fri|Sat|Sun), \\d\\d "
                               "(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
                               "\\d{4} \\d\\d:\\d\\d:\\d\\d \\+0000$");
    QVERIFY2(rfc5322.match(date).hasMatch(), qPrintable(date));
  }
  void mailTemporaryFailureRetries() {
    QTemporaryDir dir;
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc, {});
    NotificationService service(nullptr, dir.filePath("settings.json"));
    qint64 now = 0;
    service.monotonicNow = [&] { return now; };
    service.attach(&engine, doc.channels(), "/test.alh");
    auto s = settings();
    s.subscriptions[0].stages.resize(1);
    auto& d = s.destinations[0];
    d.kind = "email";
    d.program = QCoreApplication::applicationFilePath();
    d.arguments = QStringList{"--fake-mail", dir.filePath("message"), "75"};
    d.sender = "qtalh@example.invalid";
    d.recipients = QStringList{"ops@example.invalid"};
    service.apply(s);
    service.enable(true);
    engine.event(doc.channels()[0], {3, 2, 2, 1, "value"});
    now = 60000;
    service.pump();
    now = 70000;
    service.pump();
    QTRY_VERIFY(service.activity.join('\n').contains("Queued retry"));
    now = 100000;
    service.pump();
    QTRY_VERIFY(service.activity.join('\n').contains("attempt 2"));
    QTRY_COMPARE(service.policy.pending(), 1);
    now = 220000;
    service.pump();
    QTRY_VERIFY(service.activity.join('\n').contains("Failed:"));
    QCOMPARE(service.activity.join('\n').count("Submitting:"), 3);
    QCOMPARE(service.policy.pending(), 0);
    QVERIFY(!service.activity.join('\n').contains("mailer-secret"));
  }
  void connectionFailureDoesNotResolve_data() {
    QTest::addColumn<int>("status");
    QTest::newRow("disconnect") << 22;
    QTest::newRow("read-access") << 23;
    QTest::newRow("write-access") << 24;
  }
  void connectionFailureDoesNotResolve() {
    QFETCH(int, status);
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc, {true});
    auto n = doc.channels()[0];
    auto s = settings();
    s.subscriptions[0].resolution = true;
    NotificationPolicy p;
    configure(p, s);
    qint64 now = 0;
    auto observer = engine.observe([&](const AlarmObservation& o) { p.observe(o.after, now); });
    engine.event(n, {0, 0, 0, 1, "normal"});
    engine.event(n, {3, 2, 2, 1, "alarm"});
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    p.completed(e, true);
    now = 71000;
    engine.event(n, {status, 4, 0, -1, "0"});
    QVERIFY(!engine.channelUpdate(n).available);
    p.tick(71000);
    p.tick(81000);
    QVERIFY(!p.take(e, 81000));
    QCOMPARE(p.activeChannels().size(), 1);
    QCOMPARE(engine.state(n).unack, 4);
    p.tick(600000); p.tick(610000);
    QVERIFY(!p.take(e, 610000)); // ERROR must not bypass the access-gap pause.
    now = 611000;
    engine.event(n, {3, 2, 2, 1, "still in alarm"});
    // Recovery resumes the original escalation schedule, not a new episode.
    p.tick(611000);
    p.tick(621000);
    QVERIFY(p.take(e, 621000));
    QCOMPARE(e.stage, QString("escalate"));
    p.completed(e, true);
    now = 622000;
    engine.event(n, {3, 2, 0, 1, "acknowledged"});
    p.tick(622000); p.tick(632000);
    QVERIFY(!p.take(e, 632000)); // The separate communication error is still unacknowledged.
    now = 633000; engine.acknowledge(n);
    p.tick(633000); p.tick(643000);
    QVERIFY(p.take(e, 643000));
    QCOMPARE(e.kind, QString("resolution"));
  }
  void resolutionSuppressedBeforeDelivery_data() {
    QTest::addColumn<bool>("pumpWhileSuppressed");
    QTest::newRow("pumped") << true;
    QTest::newRow("busy-transports") << false;
  }
  void resolutionSuppressedBeforeDelivery() {
    QFETCH(bool, pumpWhileSuppressed);
    auto s = settings();
    s.subscriptions[0].resolution = true;
    NotificationPolicy p;
    configure(p, s);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    p.completed(e, true);
    a.unack = 0;
    p.observe(a, 70001);
    p.tick(70002);
    p.tick(80002);
    a.suppressed = true;
    p.observe(a, 80003);
    if (pumpWhileSuppressed) {
      QVERIFY(!p.take(e, 80004));
      p.tick(80005);
    }
    a.suppressed = false;
    p.observe(a, 90000);
    p.tick(90000);
    p.tick(100000);
    QVERIFY(!p.take(e, 100000));
  }
  void staleAlarmRetryKeepsQueuedResolution() {
    auto s = settings();
    s.subscriptions[0].resolution = true;
    NotificationPolicy p;
    configure(p, s);
    auto a = alarm();
    p.observe(a, 0);
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    QVERIFY(p.take(e, 70000));
    e.attempts = 1;
    p.attempted(e, 70000);
    p.completed(e, true);
    p.tick(600000);
    p.tick(610000);
    NotificationEnvelope escalation;
    QVERIFY(p.take(escalation, 610000));
    QCOMPARE(escalation.stage, QString("escalate"));
    escalation.attempts = 1;
    p.attempted(escalation, 610000); // Still in flight when the alarm is acknowledged.
    a.unack = 0;
    p.observe(a, 611000);
    p.tick(611000);
    p.tick(621000);
    NotificationEnvelope resolution;
    QVERIFY(p.take(resolution, 621000));
    QCOMPARE(resolution.kind, QString("resolution"));
    resolution.attempts = 1;
    p.attempted(resolution, 621000); // Also in flight.
    // A retryable escalation failure revalidates (now stale) and completes.
    QVERIFY(!p.revalidate(escalation));
    p.completed(escalation, false);
    p.tick(622000);
    p.tick(632000);
    QVERIFY(!p.take(e, 632000)); // No second resolution with a new message id.
    p.completed(resolution, true);
    p.tick(642000);
    QVERIFY(!p.take(e, 642000));
    QCOMPARE(p.pending(), 0);
  }
  void transportConcurrencyAndShutdown() {
    QTemporaryDir dir;
    Receiver receiver;
    receiver.hold = true;
    auto service = std::make_unique<NotificationService>(nullptr, dir.filePath("settings.json"));
    auto s = settings();
    s.subscriptions.clear();
    s.destinations.clear();
    for (int i = 0; i < 5; ++i) {
      NotificationDestination d;
      d.id = QString::number(i);
      d.name = d.id;
      d.kind = "webhook";
      d.url = receiver.url();
      s.destinations << d;
    }
    service->apply(s);
    for (int i = 0; i < 5; ++i)
      service->sendTest(QString::number(i));
    QTRY_COMPARE(receiver.requests.size(), 4);
    QVERIFY(service->activity.join('\n').contains("Throttled:"));
    service.reset();
    QCoreApplication::processEvents();
  }
  void oversizedValuesSplit() {
    auto s = settings();
    s.subscriptions[0].stages.resize(1);
    NotificationPolicy p;
    configure(p, s);
    for (int i = 0; i < 100; ++i) {
      auto a = alarm(QString::number(i));
      a.value = QString(4096, QChar(1));
      p.observe(a, 0);
    }
    p.tick(60000);
    p.tick(70000);
    NotificationEnvelope e;
    int count = 0;
    while (p.take(e, 70000)) {
      QVERIFY(notificationPayload(e).size() <= 256 * 1024);
      count += e.alarms.size();
      p.completed(e, true);
    }
    QCOMPARE(count, 100);
  }
  void placeholdersInTextAreLiteral() {
    NotificationEnvelope e;
    auto a = alarm();
    a.path = "Root/Tank%5";
    a.value = "Pump 2%1";
    a.severity = 2;
    a.status = 1;
    a.unack = 2;
    e.alarms << NotificationAlarm{"x", a};
    QVERIFY(notificationText(e).contains(
        "Root/Tank%5 (pv)\n  Value: Pump 2%1; severity: 2; status: 1; unacknowledged severity: 2\n"));
    QTemporaryDir dir;
    Receiver receiver;
    NotificationService service(nullptr, dir.filePath("settings.json"));
    auto s = settings();
    s.destinations[0].name = "Pager%1";
    s.destinations[0].url = receiver.url();
    service.apply(s);
    service.sendTest("ops");
    QTRY_VERIFY(service.activity.join('\n').contains("Accepted/submitted"));
    QVERIFY(service.activity.join('\n').contains(QRegularExpression(
        "Submitting: Pager%1; message [^;]+; attempt 1(\n|$)")));
  }
  void configurationChangeRecordsPause() {
    QTemporaryDir dir;
    for (auto name : {"a.alh", "b.alh"}) {
      QFile f(dir.filePath(name));
      QVERIFY(f.open(QIODevice::WriteOnly));
    }
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc, {});
    NotificationService service(nullptr, dir.filePath("settings.json"));
    QStringList operations;
    service.operation = [&](const QString& s) { operations << s; };
    auto file = dir.filePath("a.alh");
#ifndef Q_OS_WIN
    // Versioned files deployed behind a symlink resolve to a new canonical path on reload.
    file = dir.filePath("current.alh");
    QVERIFY(QFile::link(dir.filePath("a.alh"), file));
#endif
    service.attach(&engine, doc.channels(), file);
    QVERIFY(operations.isEmpty());
    auto s = settings();
    s.subscriptions[0].configuration = notificationConfiguration(file);
    service.apply(s);
    service.enable(true);
    QCOMPARE(service.policy.matchCount(s.subscriptions[0], service.channels()), 1);
    service.attach(&engine, doc.channels(), file);
    QVERIFY(service.policy.enabled());
    const int before = service.activity.size();
    operations.clear();
#ifndef Q_OS_WIN
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(dir.filePath("b.alh"), file));
#else
    file = dir.filePath("b.alh");
#endif
    service.attach(&engine, doc.channels(), file);
    QVERIFY(!service.policy.enabled());
    QCOMPARE(service.policy.matchCount(s.subscriptions[0], service.channels()), 0);
    QCOMPARE(service.activity.size(), before + 1);
    QVERIFY(service.activity.last().contains("Paused"));
    QVERIFY(service.activity.last().contains(notificationConfiguration(dir.filePath("b.alh"))));
    QCOMPARE(operations.size(), 1);
    QVERIFY(operations[0].startsWith("Notification Paused"));
  }
  void engineObserverAndLocalReload() {
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc, {});
    QVector<ChannelUpdate> updates;
    engine.channelUpdated = [&](const ChannelUpdate& s) { updates << s; };
    auto n = doc.channels()[0];
    QVERIFY(!engine.channelUpdate(n).initialized);
    engine.event(n, {3, 2, 2, 1, "v"});
    QCOMPARE(updates.last().unack, 2);
    engine.shelve(n, 1, "test", "tester");
    QVERIFY(updates.last().suppressed);
    engine.unshelve(n);
    QVERIFY(!updates.last().suppressed);
    engine.event(n, {0, 0, 2, 1, "ok"});
    QCOMPARE(updates.last().unack, 2);
    QHash<QString, int> saved{{Engine::nodeIdentity(n), 2}};
    Engine reloaded(doc, {});
    reloaded.restoreLocalAcknowledgements(saved);
    reloaded.event(n, {0, 0, 0, 1, "ok"});
    QCOMPARE(reloaded.state(n).unack, 2);
    engine.acknowledge(n);
    QCOMPARE(updates.last().unack, 0);
  }
};
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().value(1) == "--fake-mail") {
#ifdef Q_OS_WIN
    // Preserve MIME CRLF bytes rather than applying CRT text-mode translation.
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    QFile in, out;
    if (!in.open(stdin, QIODevice::ReadOnly))
      return 1;
    out.setFileName(app.arguments().value(2));
    if (!out.open(QIODevice::WriteOnly))
      return 1;
    out.write(in.readAll());
    fprintf(stderr, "mailer-secret\n");
    return app.arguments().value(3).toInt();
  }
  NotificationTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "test_notifications.moc"
