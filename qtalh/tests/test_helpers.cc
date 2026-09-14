#include "services/ipc.h"
#include "services/logging.h"
#include "services/log_recovery.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <QLocale>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>
#include <cstdio>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <netinet/in.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <csignal>
#include <cstdarg>
#include <unistd.h>
#include <pwd.h>
#endif
using namespace alh;
static QString checkpointFor(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return logIdentity::checkpointPath(file);
}
namespace {
struct AuditPv : PvService {
  bool writable = true;
  QVector<WriteKind> writes;
  void monitor(const QString&, std::function<void(Event)>, const void*) override {}
  void text(const QString&, std::function<void(QString)>) override {}
  void number(const QString&, std::function<void(double)>, const void*) override {}
  void cancelNumbers(const void*) override {}
  void prepare(const QString&) override {}
  bool canWrite(const QString&) const override { return writable; }
  bool put(const QString&, double, WriteKind kind) override {
    writes << kind;
    return writable;
  }
  void cancel(const QString&, const void*) override {}
  void clear() override {}
};
#ifdef Q_OS_MACOS
// macOS defaults to 40 queued messages system-wide and 2048 bytes per queue.
constexpr int BurstRecordCount = 32;
#else
constexpr int BurstRecordCount = 100;
#endif
struct Child : QProcess {
  ~Child() {
    if (state() != NotRunning) {
      kill();
      waitForFinished();
    }
  }
};
#ifndef Q_OS_WIN
struct Queue {
  int id = -1, key = 0;
  Queue() {
    key = 0x51000000 | (getpid() & 0xFFFFFF);
    while ((id = msgget(key, IPC_CREAT | IPC_EXCL | 0600)) < 0 && errno == EEXIST)
      ++key;
  }
  ~Queue() {
    if (id >= 0)
      msgctl(id, IPC_RMID, nullptr);
  }
};
QByteArray receivedRpc;
QVector<QByteArray> receivedRpcRecords;
#ifdef __APPLE__
bool_t stringXdr(XDR* x, void* data, unsigned int) {
#else
bool_t stringXdr(XDR* x, ...) {
  // TI-RPC passes the data pointer as the first variadic argument.
  va_list args;
  va_start(args, x);
  void* data = va_arg(args, void*);
  va_end(args);
#endif
  auto p = static_cast<char**>(data);
  return xdr_string(x, p, 8192);
}
#ifdef __APPLE__
bool_t voidXdr(XDR*, void*, unsigned int) {
#else
bool_t voidXdr(XDR*, ...) {
#endif
  return TRUE;
}
bool_t rpcArgs(SVCXPRT* transport, xdrproc_t codec, caddr_t data, bool release = false) {
#ifdef __APPLE__
  // Apple's C++ xp_ops declarations use (...), whose calling convention on
  // arm64 differs from the fixed arguments used by the C RPC implementation.
  using Operation = bool_t (*)(SVCXPRT*, xdrproc_t, caddr_t);
  auto operation = release ? transport->xp_ops->xp_freeargs : transport->xp_ops->xp_getargs;
  return reinterpret_cast<Operation>(operation)(transport, codec, data);
#else
  return release ? svc_freeargs(transport, codec, data) : svc_getargs(transport, codec, data);
#endif
}
void destroyRpcTransport(SVCXPRT* transport) {
#ifdef __APPLE__
  reinterpret_cast<void (*)(SVCXPRT*)>(transport->xp_ops->xp_destroy)(transport);
#else
  svc_destroy(transport);
#endif
}
void rpcDispatch(svc_req* request, SVCXPRT* transport) {
  if (request->rq_proc == 0) {
    svc_sendreply(transport, voidXdr, nullptr);
    return;
  }
  if (request->rq_proc != 1) {
    svcerr_noproc(transport);
    return;
  }
  char* text = nullptr;
  if (!rpcArgs(transport, stringXdr, reinterpret_cast<caddr_t>(&text))) {
    svcerr_decode(transport);
    return;
  }
  receivedRpc = text ? QByteArray(text) : QByteArray();
  receivedRpcRecords.append(receivedRpc);
  svc_sendreply(transport, voidXdr, nullptr);
  rpcArgs(transport, stringXdr, reinterpret_cast<caddr_t>(&text), true);
}
struct RpcServer {
  SVCXPRT* transport = nullptr;
  unsigned long program = 0;
  QTimer poll;
  RpcServer() {
    program = 0x40000000 | (getpid() & 0xFFFFFF);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    while (pmap_getport(&local, program, 1, IPPROTO_TCP))
      ++program;
    int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0)
      return;
    if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
      ::close(socket);
      return;
    }
    ::listen(socket, 16);
    transport = svctcp_create(socket, 0, 0);
    if (!transport) {
      ::close(socket);
      return;
    }
#ifdef __APPLE__
    // Apple's RPC header declares the dispatch callback without arguments.
    auto dispatch = reinterpret_cast<void (*)()>(rpcDispatch);
#else
    auto dispatch = rpcDispatch;
#endif
    if (!svc_register(transport, program, 1, dispatch, IPPROTO_TCP)) {
      destroyRpcTransport(transport);
      transport = nullptr;
      return;
    }
    QObject::connect(&poll, &QTimer::timeout, [this] {
      Q_UNUSED(this);
      fd_set set = svc_fdset;
      timeval timeout{};
      if (select(FD_SETSIZE, &set, nullptr, nullptr, &timeout) > 0)
        svc_getreqset(&set);
    });
    poll.start(10);
  }
  ~RpcServer() {
    poll.stop();
    if (transport) {
      svc_unregister(program, 1);
      destroyRpcTransport(transport);
    }
  }
};
#endif
} // namespace
class HelperTests : public QObject {
  Q_OBJECT
private slots:
#ifdef Q_OS_WIN
  void windowsLockLifetime() {
    QTemporaryDir dir;
    Options o;
    o.noLog = true;
    o.lock = true;
    o.lockFile = dir.filePath("shared");
    auto probe = [&] {
      Child child;
      child.start(QCoreApplication::applicationFilePath(), {"--lock-probe", o.lockFile});
      if (!child.waitForFinished())
        return -1;
      return child.exitCode();
    };
    auto first = std::make_unique<Logging>(o, "root");
    QVERIFY(first->isMaster());
    QCOMPARE(probe(), 1);
    {
      Logging second(o, "root");
      QVERIFY(second.isMaster());
    }
    QCOMPARE(probe(), 1); // Closing another window must retain the shared lock.
    first.reset();
    QCOMPARE(probe(), 0); // The last owner releases the lock for another process.
  }
#endif
#ifndef Q_OS_WIN
  void printer_data() {
    QTest::addColumn<QString>("binary");
    QTest::addColumn<QString>("color");
    for (auto color : {"bw", "bw_bold", "oki_bold", "hp_color"}) {
      QTest::newRow(qPrintable(QString("Qt-") + color)) << "qtalh_printer" << color;
      QTest::newRow(qPrintable(QString("legacy-") + color)) << "alh_printer" << color;
    }
  }
  void printer() {
    QFETCH(QString, binary);
    QFETCH(QString, color);
    Queue q;
    QVERIFY(q.id >= 0);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray received;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (server.hasPendingConnections()) {
        auto s = server.nextPendingConnection();
        connect(s, &QTcpSocket::readyRead, &server, [&, s] { received += s->readAll(); });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
      }
    });
    Child process;
    process.start(
        QString(TEST_BIN_DIR) + "/" + binary,
        {"127.0.0.1", QString::number(server.serverPort()), QString::number(q.key), color});
    QVERIFY(process.waitForStarted());
    QByteArray message = "1 2 10-Sep-2026 12:00:00 helper integration test";
    QVERIFY(sendQueue(q.key, message));
    QTRY_COMPARE_WITH_TIMEOUT(received, printerRecord(message, color), 10000);
  }
  void rpc_data() {
    QTest::addColumn<QString>("binary");
    QTest::newRow("Qt") << "qtalh_DB";
    QTest::newRow("Motif-era helper") << "alh_DB";
  }
  void rpc() {
    QFETCH(QString, binary);
    RpcServer server;
    QVERIFY2(server.transport,
             "A local rpcbind service must be available for RPC interoperability tests");
    Queue q;
    QVERIFY(q.id >= 0);
    receivedRpc.clear();
    Child process;
    process.start(QString(TEST_BIN_DIR) + "/" + binary,
                  {"127.0.0.1", QString::number(server.program), QString::number(q.key)});
    QVERIFY(process.waitForStarted());
    QByteArray message =
        "1 1 integration  test localhost :0 10-Sep-2026 12:00:00 test:pv HIHI MAJOR 99";
    QVERIFY(sendQueue(q.key, message));
    QTRY_COMPARE_WITH_TIMEOUT(receivedRpc, message, 15000);
  }
  void printerOutageRecovery() {
    Queue queue; QVERIFY(queue.id >= 0);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto port = server.serverPort();
    server.close(); // A refused local connection, with no production destination.
    QByteArray received, diagnostics, expected;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (server.hasPendingConnections()) {
        auto socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, &server, [&, socket] { received += socket->readAll(); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    Child process;
    connect(&process, &QProcess::readyReadStandardError, &process,
            [&] { diagnostics += process.readAllStandardError(); });
    process.start(QString(TEST_BIN_DIR) + "/qtalh_printer",
                  {"127.0.0.1", QString::number(port), QString::number(queue.key), "bw"});
    QVERIFY(process.waitForStarted());
    for (int outage = 1; outage <= 2; ++outage) {
      const auto message = QByteArray("1 2 14-Sep-2026 10:00:00 retry-") + QByteArray::number(outage);
      QVERIFY(sendQueue(queue.key, message));
      QTRY_COMPARE(diagnostics.count("Printer delivery unavailable"), outage);
      QVERIFY(diagnostics.contains("127.0.0.1:" + QByteArray::number(port)));
      QVERIFY(diagnostics.contains("retaining current record"));
      QTest::qWait(1300); // Repeated refusals must not flood stderr or lose the record.
      QCOMPARE(diagnostics.count("Printer delivery unavailable"), outage);
      QCOMPARE(diagnostics.count("Printer delivery resumed"), outage - 1);
      QCOMPARE(process.state(), QProcess::Running);
      QCOMPARE(received, expected);
      QVERIFY(server.listen(QHostAddress::LocalHost, port));
      expected += printerRecord(message, "bw");
      QTRY_COMPARE_WITH_TIMEOUT(received, expected, 5000);
      QTRY_COMPARE(diagnostics.count("Printer delivery resumed"), outage);
      server.close();
    }
    // Healthy delivery still drains subsequent queued records without duplicates.
    QVERIFY(server.listen(QHostAddress::LocalHost, port));
    const QByteArray message("1 2 14-Sep-2026 10:00:00 healthy");
    QVERIFY(sendQueue(queue.key, message)); expected += printerRecord(message, "bw");
    QTRY_COMPARE_WITH_TIMEOUT(received, expected, 5000);
    QCOMPARE(diagnostics.count("Printer delivery unavailable"), 2);
    QCOMPARE(diagnostics.count("Printer delivery resumed"), 2);
  }
  void printerBurst() {
    Queue q;
    QVERIFY(q.id >= 0);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QByteArray received, expected;
    QVector<QByteArray> receivedByConnection;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
      while (server.hasPendingConnections()) {
        auto socket = server.nextPendingConnection();
        int index = receivedByConnection.size();
        receivedByConnection.append(QByteArray());
        // Each record uses a separate TCP stream. Qt may dispatch readyRead
        // for a later connection first; preserve the server's acceptance order.
        connect(socket, &QTcpSocket::readyRead, &server, [&, socket, index] {
          receivedByConnection[index] += socket->readAll();
          received.clear();
          for (const auto& bytes : receivedByConnection)
            received += bytes;
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    for (int i = 0; i < BurstRecordCount; ++i) {
      auto message = QByteArray("1 2 10-Sep-2026 12:00:00 burst ") + QByteArray::number(i);
      QVERIFY(sendQueue(q.key, message));
      expected += printerRecord(message, "bw");
    }
    Child process;
    process.start(
        QString(TEST_BIN_DIR) + "/qtalh_printer",
        {"127.0.0.1", QString::number(server.serverPort()), QString::number(q.key), "bw"});
    QVERIFY(process.waitForStarted());
    // The deadline still rejects a 50 ms delay per record on a healthy sink.
    QTRY_COMPARE_WITH_TIMEOUT(received, expected, BurstRecordCount * 30);
  }
  void rpcBurst() {
    RpcServer server;
    QVERIFY2(server.transport, "A local rpcbind service is required");
    Queue q;
    QVERIFY(q.id >= 0);
    QVector<QByteArray> expected;
    receivedRpcRecords.clear();
    for (int i = 0; i < BurstRecordCount; ++i) {
      auto message = QByteArray("1 1 test burst ") + QByteArray::number(i);
      QVERIFY(sendQueue(q.key, message));
      expected.append(message);
    }
    Child process;
    process.start(QString(TEST_BIN_DIR) + "/qtalh_DB",
                  {"127.0.0.1", QString::number(server.program), QString::number(q.key)});
    QVERIFY(process.waitForStarted());
    QTRY_COMPARE_WITH_TIMEOUT(receivedRpcRecords.size(), expected.size(), BurstRecordCount * 30);
    QCOMPARE(receivedRpcRecords, expected);
  }
  void legacySender() {
    Queue q;
    QVERIFY(q.id >= 0);
    QByteArray text = "1 2 10-Sep-2026 12:00:00 legacy sender";
    // Execute the original msgsnd(msgp=text,msgsz=strlen(text)) layout, with
    // allocated padding so the characterization test itself has no overread.
    QByteArray buffer = text;
    buffer.append(QByteArray(sizeof(long), 0));
    QVERIFY(msgsnd(q.id, buffer.data(), text.size(), 0) == 0);
    QCOMPARE(receiveQueue(q.id), text);
  }
#endif

  void alarmAcknowledgementTokens_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<bool>("global"); QTest::addColumn<bool>("noack");
    for (bool xml : {false, true}) for (bool global : {false, true}) for (bool noack : {false, true})
      QTest::newRow(qPrintable(QString("xml%1-global%2-noack%3").arg(xml).arg(global).arg(noack)))
          << xml << global << noack;
  }
  void alarmAcknowledgementTokens() {
    QFETCH(bool, xml); QFETCH(bool, global); QFETCH(bool, noack);
    QTemporaryDir dir; Options o; o.xml = xml; o.engine.global = global;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
#ifndef Q_OS_WIN
    Queue printer, database; QVERIFY(printer.id >= 0 && database.id >= 0);
    // Full global XML database records exceed the legacy queue's size limit.
    if (!xml) { o.printerKey = printer.key; o.databaseKey = database.key; }
#endif
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Logging log(o, "root"); State state; state.severity = 2; state.unack = 2;
    state.mask[AckT] = noack;
    log.alarm(d.channels()[0], state, QDateTime::currentMSecsSinceEpoch());
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto text = file.readAll();
    const QByteArray token = noack ? "noackT" : "ackT";
    const QByteArray expected = xml ? "<severity-noack>" + token + "</severity-noack>" : " " + token + " ";
    QCOMPARE(text.contains(expected), global);
    QVERIFY(!text.contains(" YES ") && !text.contains(" NO "));
#ifndef Q_OS_WIN
    if (!xml) {
      const auto printed = receiveQueue(printer.id), queued = receiveQueue(database.id);
      QVERIFY(printed.startsWith("1 2 ")); QVERIFY(queued.startsWith("1 1 root  "));
      QCOMPARE(printed.contains(expected), global); QCOMPARE(queued.contains(expected), global);
    }
#endif
  }
#ifndef Q_OS_WIN
  void alarmOutputDestinations_data() {
    QTest::addColumn<int>("destinations"); QTest::addColumn<bool>("xml");
    for (int destinations = 0; destinations < 8; ++destinations)
      for (bool xml : {false, true})
        QTest::newRow(qPrintable(QString("destinations-%1-xml-%2").arg(destinations).arg(xml)))
            << destinations << xml;
  }
  void alarmOutputDestinations() {
    QFETCH(int, destinations); QFETCH(bool, xml);
    QTemporaryDir dir; Queue printer, database;
    QVERIFY(dir.isValid() && printer.id >= 0 && database.id >= 0);
    Options o; o.noLog = !(destinations & 1); o.xml = xml;
    o.printerKey = destinations & 2 ? printer.key : 0;
    o.databaseKey = destinations & 4 ? database.key : 0;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("operations");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Logging log(o, "root"); Engine engine(d); auto n = d.channels()[0];
    qint64 time = 10000; engine.now = [&] { return time; };
    engine.event(n, {0, 0, 0, 1, "0"});
    engine.alarmLog = [&](Node* node, const State& state, qint64 t) { log.alarm(node, state, t); };
    QVector<AlarmObservation> observations;
    auto observer = engine.observe([&](const AlarmObservation& event) { observations << event; });
    const auto historyCount = engine.history.size();
    ++time; engine.event(n, {3, 2, 2, 1, "10"});
    ++time; engine.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(observations.size(), 2);
    QCOMPARE(observations[0].after.severity, 2); QCOMPARE(observations[1].after.severity, 0);
    QCOMPARE(engine.history.size(), historyCount + 2);
    QVERIFY(engine.history.first().contains("NO_ALARM"));
    QVERIFY(engine.history[1].contains("MAJOR"));
    auto verifyQueue = [&](Queue& queue, bool enabled) {
      const auto alarm = receiveQueue(queue.id), normal = receiveQueue(queue.id);
      if (enabled) {
        QVERIFY(alarm.contains("MAJOR") && alarm.contains("pv"));
        QVERIFY(normal.contains("NO_ALARM") && normal.contains("pv"));
        QCOMPARE(alarm.contains("<pv>pv</pv>"), xml);
      } else {
        QVERIFY(alarm.isEmpty() && normal.isEmpty());
      }
      QVERIFY(receiveQueue(queue.id).isEmpty());
    };
    verifyQueue(printer, destinations & 2); verifyQueue(database, destinations & 4);
    QFile file(o.alarmFile);
    if (o.noLog) {
      QVERIFY(!file.exists() && !QFile::exists(o.opmodFile));
    } else {
      QVERIFY(file.open(QIODevice::ReadOnly)); const auto records = file.readAll();
      QCOMPARE(records.count('\n'), 2);
      QVERIFY(records.contains("MAJOR") && records.contains("NO_ALARM"));
      QCOMPARE(records.contains("<pv>pv</pv>"), xml);
    }
  }

  void databaseHeaderIdentity_data() {
    QTest::addColumn<QByteArray>("user"); QTest::addColumn<QByteArray>("display");
    QTest::addColumn<QByteArray>("expectedDisplay");
    QTest::newRow("unset") << QByteArray() << QByteArray() << QByteArray("unknown_display");
    QTest::newRow("empty") << QByteArray("") << QByteArray("") << QByteArray("unknown_display");
    QTest::newRow("spoofed-user") << QByteArray("someone_else") << QByteArray(":99") << QByteArray(":99");
    QTest::newRow("whitespace") << QByteArray(" ") << QByteArray(" \t ") << QByteArray("unknown_display");
  }
  void databaseHeaderIdentity() {
    QFETCH(QByteArray, user); QFETCH(QByteArray, display); QFETCH(QByteArray, expectedDisplay);
    struct Environment {
      QByteArray user = qgetenv("USER"), display = qgetenv("DISPLAY");
      ~Environment() {
        if (user.isNull()) qunsetenv("USER"); else qputenv("USER", user);
        if (display.isNull()) qunsetenv("DISPLAY"); else qputenv("DISPLAY", display);
      }
    } restore;
    if (user.isNull()) qunsetenv("USER"); else qputenv("USER", user);
    if (display.isNull()) qunsetenv("DISPLAY"); else qputenv("DISPLAY", display);
    const auto account = getpwuid(geteuid()); QVERIFY(account);
    const QByteArray expectedUser(account->pw_name);
    Queue queue; QVERIFY(queue.id >= 0);
    Options o; o.noLog = true; o.databaseKey = queue.key;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Logging log(o, "root"); State state; state.severity = 2;
    log.alarm(d.channels()[0], state, QDateTime(QDate(2026, 9, 13), QTime(12, 0)).toMSecsSinceEpoch());
    log.operation(d.channels()[0], "acknowledged", OperationKind::AckChannel);
    for (int i = 0; i < 2; ++i) {
      const auto message = receiveQueue(queue.id);
      const auto fields = message.simplified().split(' ');
      QVERIFY2(fields.size() >= 9, message.constData());
      QCOMPARE(fields[0], QByteArray(i ? "2" : "1"));
      QCOMPARE(fields[2], QByteArray("root")); QCOMPARE(fields[3], expectedUser);
      QVERIFY(!fields[4].isEmpty()); QCOMPARE(fields[5], expectedDisplay);
      QVERIFY(QDate::fromString(QString::fromLatin1(fields[6]), "dd-MMM-yyyy").isValid());
      QVERIFY(QTime::fromString(QString::fromLatin1(fields[7]), "HH:mm:ss").isValid());
    }
  }
#endif

  void operationFacilityAlias_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("text") << false; QTest::newRow("xml") << true;
  }
  void operationFacilityAlias() {
    QFETCH(bool, xml);
    QTemporaryDir dir; Options o; o.xml = xml;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
#ifndef Q_OS_WIN
    Queue queue; QVERIFY(queue.id >= 0); o.databaseKey = queue.key;
#endif
    auto d = parseConfig("GROUP NULL internal\nGROUP internal branch\nCHANNEL branch pv\n$ALIAS Channel label\n");
    Logging log(o, d.root->name);
    for (const auto& alias : {QString(), QString("Operator Facility"), QString("New Facility"), QString()}) {
      d.root->setOption("ALIAS", alias);
      log.setFacilityLabel(d.root->label());
      const auto label = d.root->label().toLocal8Bit();
      log.operation(d.channels()[0], "Change Mask -D---", OperationKind::ChangeMask);
      log.operation(d.root.get(), "Group acknowledgement", OperationKind::AckGroup);
      log.operation(nullptr, "Silence Current set to TRUE");
      QFile file(o.opmodFile); QVERIFY(file.open(QIODevice::ReadOnly));
      const auto lines = file.readAll().split('\n');
      QVERIFY(lines[lines.size() - 4].contains(label + ": pv:  Change Mask -D---"));
      QVERIFY(lines[lines.size() - 3].contains(label + ": internal:  Group acknowledgement"));
      QVERIFY(lines[lines.size() - 2].contains(label + ": :  Silence Current set to TRUE"));
#ifndef Q_OS_WIN
      const auto channel = receiveQueue(queue.id), group = receiveQueue(queue.id);
      QVERIFY(channel.startsWith("2 7 internal  "));
      QVERIFY(group.startsWith("2 6 internal  "));
      QVERIFY(channel.contains(label + ": pv:  Change Mask -D---"));
      QVERIFY(group.contains(label + ": internal:  Group acknowledgement"));
      QVERIFY(receiveQueue(queue.id).isEmpty());
#endif
    }
  }
  void shelvingOperationLogs() {
    QTemporaryDir dir;
    Options o; o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod"); o.maxRecords = 0;
#ifndef Q_OS_WIN
    Queue queue; QVERIFY(queue.id >= 0); o.databaseKey = queue.key;
#endif
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch pv\n");
    qint64 time = 1000;
    auto run = [&] {
      Logging log(o, "root"); Engine engine(d); engine.now = engine.monotonicNow = [&] { return time; };
      engine.operation = [&](Node* n, const QString& text, OperationKind kind) { log.operation(n, text, kind); };
      auto n = d.channels()[0];
      engine.shelve(n, 1, "Investigating Ack Channel behavior", "tester");
      engine.shelve(n, 2, "Investigating Ack Group behavior", "tester", true);
      engine.unshelve(n);
      engine.shelve(n, 1, "Maintenance complete", "tester");
      time += 60000; engine.tick();
      log.operation(n, "Notification Queued: Ack Group and Ack Channel subscription");
    };
    run();
    QFile file(o.opmodFile); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto text = file.readAll(); file.close();
    QCOMPARE(text.count('\n'), 6);
    QVERIFY(text.contains("Notification Queued: Ack Group and Ack Channel subscription"));
    QVERIFY(text.contains("Shelve /root/branch/pv until="));
    QVERIFY(text.contains("reason=Investigating Ack Channel behavior"));
    QVERIFY(text.contains("Change shelf /root/branch/pv until="));
    QVERIFY(text.contains("username=tester"));
    QVERIFY(text.contains("previous_username=tester"));
    QVERIFY(text.contains("shelved_by=tester"));
    QVERIFY(text.contains("Unshelve /root/branch/pv until="));
    QVERIFY(text.contains("Shelf expired /root/branch/pv until="));
#ifndef Q_OS_WIN
    struct { long type; char bytes[1024]; } message{};
    QCOMPARE(msgrcv(queue.id, &message, sizeof(message.bytes), 0, IPC_NOWAIT), ssize_t(-1));
    QCOMPARE(errno, ENOMSG); // Shelf reasons never emit legacy DB acknowledgement messages.
#endif
    o.noLog = true; run();
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), text);
  }

  void concurrentUnlimitedLogging() {
    QTemporaryDir dir;
    const auto time = QDateTime::currentMSecsSinceEpoch();
    Child first, second;
    int worker = 0;
    for (auto* child : {&first, &second})
      child->start(QCoreApplication::applicationFilePath(),
                   {"--append-probe", dir.path(), QString::number(worker++), QString::number(time)});
    for (auto* child : {&first, &second}) {
      QVERIFY(child->waitForStarted());
      if (!child->canReadLine()) QVERIFY(child->waitForReadyRead());
      QCOMPARE(child->readLine().trimmed(), QByteArray("ready"));
    }
    // Release independent writers together; a seek followed by write loses rows.
    for (auto* child : {&first, &second}) {
      QCOMPARE(child->write("\n"), qint64(1));
      QVERIFY(child->waitForBytesWritten());
    }
    for (auto* child : {&first, &second}) {
      if (child->state() != QProcess::NotRunning) QVERIFY(child->waitForFinished());
      QCOMPARE(child->exitStatus(), QProcess::NormalExit);
      QCOMPARE(child->exitCode(), 0);
    }
    QFile file(dir.filePath("alarm") + QDateTime::fromMSecsSinceEpoch(time).date().toString(".yyyy-MM-dd"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto rows = file.readAll().split('\n');
    QCOMPARE(rows.takeLast(), QByteArray());
    QCOMPARE(rows.size(), 40000);
    QSet<QByteArray> values;
    for (const auto& row : rows) values.insert(row.trimmed().split(' ').last());
    QCOMPARE(values.size(), 40000);
    for (int id = 0; id < 2; ++id) for (int i = 0; i < 20000; ++i)
      QVERIFY(values.contains(QByteArray::number(id) + ':' + QByteArray::number(i)));
  }

  void sharedLogLimitTransitions() {
    QTemporaryDir dir;
    Options o; o.dated = true; o.maxRecords = 2;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    Logging bounded(o, "root");
    o.maxRecords = 0;
    Logging unlimited(o, "root");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state;
    const auto time = QDateTime::currentMSecsSinceEpoch();
    auto write = [&](Logging& log, const QString& value) {
      state.value = value; log.alarm(d.channels()[0], state, time);
    };
    write(bounded, "A"); write(bounded, "B"); write(unlimited, "C"); write(bounded, "D");
    auto values = [&] {
      QFile file(bounded.alarmPath());
      if (!file.open(QIODevice::ReadOnly)) return QSet<QByteArray>();
      QSet<QByteArray> result;
      while (!file.atEnd()) result.insert(file.readLine().trimmed().split(' ').last());
      return result;
    };
    QCOMPARE(values(), (QSet<QByteArray>{"C", "D"}));
    write(unlimited, "E"); write(bounded, "F");
    QCOMPARE(values(), (QSet<QByteArray>{"E", "F"}));
  }

  void logging() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 0;
    auto d = parseConfig("GROUP NULL test\nCHANNEL test pv");
    State s;
    s.severity = 2;
    s.status = 3;
    s.value = "99";
    {
      Logging log(o, "test");
      log.alarm(d.channels()[0], s, 1000);
      log.operation(d.channels()[0], "Ack Channel");
    }
    QFile alarm(o.alarmFile);
    QVERIFY(alarm.open(QIODevice::ReadOnly));
    auto text = alarm.readAll();
    QVERIFY(text.contains("pv"));
    QVERIFY(text.contains("HIHI"));
    QVERIFY(text.contains("MAJOR"));
    o.noLog = true;
    {
      Logging log(o, "test");
      log.alarm(d.channels()[0], s, 2000);
    }
    alarm.seek(0);
    QCOMPARE(alarm.readAll(), text);
  }
#ifndef Q_OS_WIN
  // POSIX allows replacing a pathname while the original log is still open.
  void renamedLogRecovery_data() {
    QTest::addColumn<bool>("large"); QTest::addColumn<bool>("duringRecovery");
    QTest::newRow("default-before-first-alarm") << false << false;
    QTest::newRow("large-before-first-alarm") << true << false;
    QTest::newRow("large-during-worker-scan") << true << true;
  }
  void renamedLogRecovery() {
    QFETCH(bool, large); QFETCH(bool, duringRecovery);
    QTemporaryDir dir;
    Options o; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    if (large) {
      o.lock = true; o.maxRecords = 3;
      QFile seed(o.alarmFile); QVERIFY(seed.open(QIODevice::WriteOnly));
      const QByteArray row = "13-Sep-2026 12:00:00 : " + QByteArray(200, 'x') + '\n';
      for (int i = 0; i < 10000; ++i) QCOMPARE(seed.write(row), qint64(row.size()));
    }
    Logging log(o, "root"); QStringList errors;
    log.error = [&](const QString& error) { errors << error; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    auto write = [&](const QString& value) { state.value = value; log.alarm(d.channels()[0], state, 1000); };
    if (duringRecovery) write("before-rename");
    const auto renamed = dir.filePath("renamed");
    QVERIFY(QFile::rename(o.alarmFile, renamed));
    QFile replacement(o.alarmFile); QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.close();
    write("after-rename-1"); write("after-rename-2");
    QByteArray records;
    auto complete = [&] {
      QFile file(renamed); if (!file.open(QIODevice::ReadOnly)) return false;
      records = file.readAll(); return records.contains("after-rename-2");
    };
    QTRY_VERIFY_WITH_TIMEOUT(complete(), 20000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(records.contains("after-rename-1"));
    if (duringRecovery) QVERIFY(records.contains("before-rename"));
    QCOMPARE(records.count('\n'), large ? 3 : 2);
    QCOMPARE(QFileInfo(o.alarmFile).size(), qint64(0));
    // Explicitly selecting the replacement switches destinations, as before.
    log.setAlarmFile(o.alarmFile); write("replacement-alarm");
    QVERIFY(replacement.open(QIODevice::ReadOnly));
    QVERIFY(replacement.readAll().contains("replacement-alarm"));
    QVERIFY(errors.isEmpty());
  }
  void replacedAlarmFile_data() {
    QTest::addColumn<int>("maximum");
    QTest::newRow("unlimited") << 0;
    QTest::newRow("bounded") << 3;
  }
  void replacedAlarmFile() {
    QFETCH(int, maximum);
    QTemporaryDir dir;
    Options o; o.lock = true; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    o.maxRecords = maximum;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.value = "before-replacement";
    Logging log(o, "root");
    log.alarm(d.channels()[0], state, 1000);
    const auto oldPath = o.alarmFile + ".old";
    QVERIFY(QFile::rename(o.alarmFile, oldPath));
    QFile replacement(o.alarmFile);
    QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.close();
    state.value = "while-renamed";
    log.alarm(d.channels()[0], state, 1500);
    log.setAlarmFile(o.alarmFile);
    state.value = "after-replacement";
    log.alarm(d.channels()[0], state, 2000);
    QVERIFY(replacement.open(QIODevice::ReadOnly));
    const auto contents = replacement.readAll();
    QVERIFY(contents.contains("after-replacement"));
    QVERIFY(!contents.contains("before-replacement"));
    QVERIFY(!contents.contains("while-renamed"));
    QFile old(oldPath); QVERIFY(old.open(QIODevice::ReadOnly));
    const auto previous = old.readAll();
    QVERIFY(previous.contains("before-replacement"));
    QVERIFY(previous.contains("while-renamed"));
    QVERIFY(!previous.contains("after-replacement"));
  }
#endif
  void legacyCtimeRingRecovery_data() {
    QTest::addColumn<bool>("stale"); QTest::addColumn<int>("day");
    for (bool stale : {false, true}) for (int day : {3, 13})
      QTest::newRow(qPrintable(QString("stale-%1-day-%2").arg(stale).arg(day))) << stale << day;
  }
  void legacyCtimeRingRecovery() {
    QFETCH(bool, stale); QFETCH(int, day);
    QTemporaryDir dir;
    const auto path = dir.filePath("alarm"), metadataPath = path + ".qtalh-position";
    QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
    for (int second : {3, 1, 2}) {
      const auto time = QDateTime(QDate(2026, 9, day), QTime(12, 0, second));
      const auto stamp = QLocale::c().toString(time, "ddd MMM ") +
          QString::number(day).rightJustified(2, ' ') + QLocale::c().toString(time, " HH:mm:ss yyyy");
      file.write((stamp + QString(" record-%1\n").arg(second)).toLatin1());
    }
    QVERIFY(file.flush());
    if (stale) {
      // A structurally valid checkpoint for different contents must fall back
      // to timestamps, just like a legacy file with no checkpoint at all.
      QByteArray slot(96, 0); slot.replace(0, 8, "ALHPOS01");
      qToBigEndian<quint64>(5, slot.data() + 8);
      qToBigEndian<quint64>(3, slot.data() + 16);
      slot.replace(64, 32, QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256));
      QFile metadata(metadataPath); QVERIFY(metadata.open(QIODevice::WriteOnly));
      QCOMPARE(metadata.write(slot), qint64(slot.size()));
    }
    const auto recovered = logRecovery::read(path, logIdentity::identity(file), metadataPath, 2, 0, 1);
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(int(recovered.records.size()), 2);
    QVERIFY(recovered.records[0].contains("record-2"));
    QVERIFY(recovered.records[1].contains("record-3"));
    file.close();
    Options o; o.lock = true; o.config = dir.filePath("config");
    o.alarmFile = path; o.opmodFile = dir.filePath("opmod"); o.maxRecords = 2;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.value = "record-4";
    { Logging log(o, "root"); log.alarm(d.channels()[0], state,
        QDateTime(QDate(2026, 9, day), QTime(12, 0, 4)).toMSecsSinceEpoch()); }
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto saved = file.readAll();
    QCOMPARE(saved.count('\n'), 2);
    QVERIFY(saved.contains("record-3")); QVERIFY(saved.contains("record-4"));
    QVERIFY(!saved.contains("record-1")); QVERIFY(!saved.contains("record-2"));
  }
  void failedLogFileChange_data() {
    QTest::addColumn<bool>("dated");
    QTest::newRow("plain") << false;
    QTest::newRow("dated") << true;
  }
  void failedLogFileChange() {
    QFETCH(bool, dated);
    const auto today = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.dated = dated;
    o.maxRecords = 0;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    auto n = d.channels()[0];
    Logging log(o, "root");
    QStringList errors;
    log.error = [&](QString s) { errors << s; };
    auto alarmPath = log.alarmPath(), opmodPath = log.opmodPath();
    State state;
    state.value = "before-failure";
    log.alarm(n, state, today + 1000);
    log.operation(n, "before-failure");
    log.setAlarmFile(dir.filePath("missing/alarm"));
    log.setOpmodFile(dir.filePath("missing/opmod"));
    QCOMPARE(errors.size(), 2);
    QCOMPARE(log.alarmPath(), alarmPath);
    QCOMPARE(log.opmodPath(), opmodPath);
    QCOMPARE(log.alarmBasePath(), o.alarmFile);
    QCOMPARE(log.opmodBasePath(), o.opmodFile);
    state.value = "after-failure";
    log.alarm(n, state, today + 2000);
    log.operation(n, "after-failure");
    for (auto path : {alarmPath, opmodPath}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      auto contents = file.readAll();
      QVERIFY(contents.contains("before-failure"));
      QVERIFY(contents.contains("after-failure"));
      QVERIFY(!contents.contains("Setup")); // Rejected destinations are not audited as changes.
    }
    log.setAlarmFile(dir.filePath("new-alarm"));
    log.setOpmodFile(dir.filePath("new-opmod"));
    QVERIFY(log.alarmPath() != alarmPath);
    QVERIFY(log.opmodPath() != opmodPath);
    QFile previous(opmodPath); QVERIFY(previous.open(QIODevice::ReadOnly));
    const auto handoff = previous.readAll();
    QCOMPARE(handoff.count("Setup Alarm Log File : " + dir.filePath("new-alarm").toLocal8Bit()), 1);
    QCOMPARE(handoff.count("Setup OpMod File : " + dir.filePath("new-opmod").toLocal8Bit()), 1);
    QFile current(log.opmodPath()); QVERIFY(current.open(QIODevice::ReadOnly));
    QVERIFY(current.readAll().isEmpty()); // The handoff belongs to the old log.
    state.value = "after-success";
    log.alarm(n, state, today + 3000);
    log.operation(n, "after-success");
    for (auto path : {log.alarmPath(), log.opmodPath()}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      QVERIFY(file.readAll().contains("after-success"));
    }
  }
#ifndef Q_OS_WIN
  void queueStructuredResults() {
    Queue queue; QVERIFY(queue.id >= 0);
    QCOMPARE(sendQueueResult(queue.key, "1 1 root test").status, QueueSendStatus::Sent);
    QCOMPARE(receiveQueue(queue.id), QByteArray("1 1 root test"));
    QCOMPARE(sendQueueResult(queue.key, QByteArray(251, 'x')).status, QueueSendStatus::Oversized);
    QCOMPARE(sendQueueResult(queue.key, "bad").status, QueueSendStatus::Failed);
    struct msqid_ds settings{}; QVERIFY(msgctl(queue.id, IPC_STAT, &settings) == 0);
    settings.msg_qbytes = 64; QVERIFY(msgctl(queue.id, IPC_SET, &settings) == 0);
    QCOMPARE(sendQueueResult(queue.key, QByteArray("1 1 ") + QByteArray(60, 'x')).status, QueueSendStatus::Sent);
    QCOMPARE(sendQueueResult(queue.key, "1 1 root test").status, QueueSendStatus::Full);
  }
  void queueLossRecovery_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("plain") << false; QTest::newRow("xml") << true;
  }
  void queueLossRecovery() {
    QFETCH(bool, xml);
    Queue queue; QVERIFY(queue.id >= 0);
    struct msqid_ds settings{}; QVERIFY(msgctl(queue.id, IPC_STAT, &settings) == 0);
    settings.msg_qbytes = 250 - sizeof(long); QVERIFY(msgctl(queue.id, IPC_SET, &settings) == 0);
    const auto filler = QByteArray("1 1 ") + QByteArray(settings.msg_qbytes - 4, 'x');
    QVERIFY(sendQueue(queue.key, filler));
    QTemporaryDir dir; Options o; o.noLog = true; o.xml = xml; o.databaseKey = queue.key;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    qint64 time = 1000; LoggingClock clock; clock.wall = [&] { return time; };
    Logging log(o, "root", nullptr, clock);
    QStringList errors; log.error = [&](const QString& text) { errors << text; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2; state.status = 3; state.value = "2";
    log.alarm(d.channels()[0], state, time);
    time = 2000; log.operation(d.channels()[0], "mask", OperationKind::ChangeMask);
    QCOMPARE(errors.size(), 2);
    // Repeated full-queue attempts must not count the summary as a lost alarm.
    QTest::qWait(4200);
    QCOMPARE(receiveQueue(queue.id), filler);
    QByteArray summary;
    QTRY_VERIFY(([&] { if (summary.isEmpty()) summary = receiveQueue(queue.id); return !summary.isEmpty(); })()); // No new alarm is needed.
    QVERIFY2(summary.startsWith("1 4 root  "), summary.constData());
    QVERIFY(summary.endsWith("MQ lost 2 messages from_ms=1000 to_ms=2000\n"));
    QVERIFY(summary.size() <= 250 - int(sizeof(long)));
    QCOMPARE(summary.contains("<date>"), xml);
    QTest::qWait(2200); QVERIFY(receiveQueue(queue.id).isEmpty());
    log.alarm(d.channels()[0], state, time);
    QVERIFY(receiveQueue(queue.id).startsWith("1 1 root  "));
    QVERIFY(receiveQueue(queue.id).isEmpty());
  }
  void queueLossDestinationsAndReload() {
    Queue printer, database; QVERIFY(printer.id >= 0); QVERIFY(database.id >= 0);
    struct msqid_ds settings{}; QVERIFY(msgctl(database.id, IPC_STAT, &settings) == 0);
    settings.msg_qbytes = 250 - sizeof(long); QVERIFY(msgctl(database.id, IPC_SET, &settings) == 0);
    const auto filler = QByteArray("1 1 ") + QByteArray(settings.msg_qbytes - 4, 'x');
    QVERIFY(sendQueue(database.key, filler));
    QTemporaryDir dir; Options o; o.noLog = true; o.printerKey = printer.key; o.databaseKey = database.key;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    qint64 time = 1000; LoggingClock clock; clock.wall = [&] { return time; };
    Logging log(o, "oldroot", nullptr, clock); log.error = [](const QString&) {};
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2; state.status = 3;
    log.alarm(d.channels()[0], state, time);
    QVERIFY(receiveQueue(printer.id).startsWith("1 2 "));
    log.setFacility("newroot", "New Facility"); time = 2000;
    log.alarm(d.channels()[0], state, time);
    QVERIFY(receiveQueue(printer.id).startsWith("1 2 "));
    QCOMPARE(receiveQueue(database.id), filler);
    QList<QByteArray> summaries;
    QTRY_VERIFY(([&] { auto next = receiveQueue(database.id); if (!next.isEmpty()) summaries << next;
                      return summaries.size() == 2; })());
    for (const auto& summary : summaries) {
      QVERIFY(summary.contains("MQ lost 1 messages"));
      if (summary.startsWith("1 4 oldroot  ")) QVERIFY(summary.endsWith("from_ms=1000 to_ms=1000\n"));
      else { QVERIFY(summary.startsWith("1 4 newroot  ")); QVERIFY(summary.endsWith("from_ms=2000 to_ms=2000\n")); }
    }
    QVERIFY(summaries[0].left(15) != summaries[1].left(15));
    QVERIFY(receiveQueue(printer.id).isEmpty()); // No cross-destination loss summary.
    log.alarm(d.channels()[0], state, time);
    QVERIFY(receiveQueue(database.id).startsWith("1 1 newroot  "));
    QVERIFY(receiveQueue(printer.id).startsWith("1 2 "));
  }
  void pendingQueueSummaryDoesNotBlockSmallerRecords() {
    Queue queue; QVERIFY(queue.id >= 0);
    QTemporaryDir dir; Options o; o.noLog = true; o.databaseKey = queue.key;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    Logging log(o, "root"); log.error = [](const QString&) {};
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); auto n = d.channels()[0];
    log.operation(n, "x", OperationKind::ChangeMask);
    auto ordinary = receiveQueue(queue.id); QVERIFY(!ordinary.isEmpty());
    struct msqid_ds settings{}; QVERIFY(msgctl(queue.id, IPC_STAT, &settings) == 0);
    const auto capacity = settings.msg_qbytes;
    settings.msg_qbytes = ordinary.size(); QVERIFY(msgctl(queue.id, IPC_SET, &settings) == 0);
    log.operation(n, QString(300, 'x'), OperationKind::ChangeMask); // One oversized original.
    log.operation(n, "x", OperationKind::ChangeMask); // Smaller than the pending summary.
    QVERIFY(receiveQueue(queue.id).startsWith("2 7 root  "));
    settings.msg_qbytes = capacity; QVERIFY(msgctl(queue.id, IPC_SET, &settings) == 0);
    QByteArray summary; QTRY_VERIFY(([&] { if (summary.isEmpty()) summary = receiveQueue(queue.id); return !summary.isEmpty(); })());
    QVERIFY(summary.contains("MQ lost 1 messages"));
  }
  void oversizedQueueLossSummaryIsBounded() {
    Queue printer, database; QVERIFY(printer.id >= 0); QVERIFY(database.id >= 0);
    QTemporaryDir dir; Options o; o.noLog = true; o.xml = true;
    o.printerKey = printer.key; o.databaseKey = database.key;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    Logging log(o, QString(1000, 'r')); log.error = [](const QString&) {};
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.value = QString(1000, '&'); state.severity = 2;
    log.alarm(d.channels()[0], state, QDateTime::currentMSecsSinceEpoch());
    QByteArray p, db;
    QTRY_VERIFY(([&] { if (p.isEmpty()) p = receiveQueue(printer.id); return !p.isEmpty(); })());
    QTRY_VERIFY(([&] { if (db.isEmpty()) db = receiveQueue(database.id); return !db.isEmpty(); })());
    QVERIFY(p.startsWith("1 5 ")); QVERIFY(db.startsWith("1 4 "));
    for (const auto& record : {p, db}) {
      QVERIFY(record.size() <= 250 - int(sizeof(long)));
      QVERIFY(record.contains("MQ lost 1 messages"));
      QVERIFY(record.endsWith('\n'));
    }
  }
  void queueTimestampFormat_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("plain") << false; QTest::newRow("xml") << true;
  }
  void queueTimestampFormat() {
    QFETCH(bool, xml);
    Queue printer, database; QVERIFY(printer.id >= 0); QVERIFY(database.id >= 0);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.xml = xml; o.printerKey = printer.key; o.databaseKey = database.key;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 34, 56)).toMSecsSinceEpoch();
    LoggingClock clock; clock.wall = [=] { return time; };
    Logging log(o, "root", nullptr, clock);
    QStringList errors; log.error = [&](const QString& text) { errors << text; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2; state.status = 3; state.value = "2";
    log.alarm(d.channels()[0], state, time);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    const QByteArray stamp = xml
        ? "<date>13-Sep-2026</date> <time>12:34:56</time>"
        : "13-Sep-2026 12:34:56";
    const auto body = xml
        ? QByteArray("<pv>pv</pv> <value>2</value> <status>HIHI</status> <severity>MAJOR</severity>")
        : QString("%1 %2 %3 %4").arg("pv", -28).arg("HIHI", -12).arg("MAJOR", -16)
                  .arg("2", -40).toLocal8Bit();
    const auto record = stamp + ' ' + body;
    QCOMPARE(receiveQueue(printer.id), QByteArray("1 2 ") + record);
    const auto db = receiveQueue(database.id);
    QVERIFY2(db.startsWith("1 1 root  ") && db.endsWith(record), db.constData());
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), xml ? "<entry>" + record + "</entry>\n"
                                : stamp + " : " + body + '\n');
    log.operation(d.channels()[0], "ack", OperationKind::AckChannel);
    const auto operation = receiveQueue(database.id);
    QVERIFY2(operation.startsWith("2 5 root  ") &&
             operation.endsWith(stamp + " root: pv:  ack"), operation.constData());
    QVERIFY(receiveQueue(printer.id).isEmpty());
    QVERIFY(receiveQueue(database.id).isEmpty());
  }

  void maskOperationProtocol() {
    Queue q; QVERIFY(q.id >= 0);
    Options o; o.noLog = true; o.databaseKey = q.key;
    auto d = parseConfig("GROUP NULL root\n$FORCEPV gate -D--- 1 0\n"
                         "CHANNEL root first\n$FORCEPV gate -D--- 1 0\nCHANNEL root second\n");
    Engine e(d); Logging log(o, "root");
    e.operation = [&](Node* n, const QString& s, OperationKind kind) { log.operation(n, s, kind); };
    auto root = d.root.get(); auto first = d.channels()[0];
    e.modifyMask(root, Disable, 1);
    auto one = receiveQueue(q.id), two = receiveQueue(q.id), summary = receiveQueue(q.id);
    QVERIFY(one.startsWith("2 8 root ")); QVERIFY(one.contains("Group Mask ID --- first"));
    QVERIFY(two.startsWith("2 8 root ")); QVERIFY(two.contains("Group Mask ID --- second"));
    QVERIFY(summary.startsWith("2 7 root ")); QVERIFY(receiveQueue(q.id).isEmpty());
    e.setMask(root, {}); QVERIFY(receiveQueue(q.id).startsWith("2 8 root "));
    e.resetMask(root); QVERIFY(receiveQueue(q.id).startsWith("2 8 root "));
    e.setMask(first, {}); QVERIFY(receiveQueue(q.id).startsWith("2 7 root "));
    e.forceValue(root, 1); QVERIFY(receiveQueue(q.id).startsWith("2 10 root "));
    e.forceValue(root, 0); QVERIFY(receiveQueue(q.id).startsWith("2 8 root "));
    e.forceValue(first, 1); QVERIFY(receiveQueue(q.id).startsWith("2 9 root "));
    e.forceValue(first, 0); QVERIFY(receiveQueue(q.id).startsWith("2 7 root "));
    log.operation(root, "Ack Group Change Mask arbitrary text");
    QVERIFY(receiveQueue(q.id).isEmpty());
  }

  void forcePvEditAudit_data() {
    QTest::addColumn<bool>("group");
    QTest::addColumn<bool>("noLog");
    for (bool group : {false, true})
      for (bool noLog : {false, true})
        QTest::newRow(qPrintable(QString("%1-%2").arg(group ? "group" : "channel",
                                                    noLog ? "no-files" : "files")))
            << group << noLog;
  }
  void forcePvEditAudit() {
    QFETCH(bool, group);
    QFETCH(bool, noLog);
    Queue q; QVERIFY(q.id >= 0);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.noLog = noLog; o.databaseKey = q.key;
    o.alarmFile = dir.filePath("alarm.log");
    o.opmodFile = dir.filePath("operations.log");
    auto d = parseConfig("GROUP NULL root\n$FORCEPV gate -D--- 1 0\n"
                         "CHANNEL root pv\n$FORCEPV gate -D--- 1 0\n");
    AuditPv pv; Engine e(d, {}, &pv); Logging log(o, "root");
    auto n = group ? d.root.get() : d.channels()[0];
    e.start();
    e.forceValue(n, 3); // Neither old nor edited settings match: no mask action.
    e.operation = [&](Node* node, const QString& text, OperationKind kind) {
      log.operation(node, text, kind);
    };
    const QByteArray prefix = group ? "2 10 root " : "2 9 root ";
    const QVector<Directive> changed = {{"FORCEPV", "gate -D--- 2 0"}};
    auto verifyRecord = [&](const QByteArray& text) {
      const auto record = receiveQueue(q.id);
      QVERIFY2(record.startsWith(prefix), record.constData());
      QVERIFY(record.contains(("root: " + n->name + ":  ").toLocal8Bit() + text));
      QVERIFY(receiveQueue(q.id).isEmpty());
    };
    e.configureForce(n, changed, false);
    verifyRecord("Change Force PV gate -D--- 2 0");
    e.configureForce(n, changed, false); // Unchanged Apply remains a no-op.
    QVERIFY(receiveQueue(q.id).isEmpty());
    e.configureForce(n, changed, true); // Enabled setting is the only change.
    verifyRecord("Disable Force PV");
    e.configureForce(n, changed, true);
    QVERIFY(receiveQueue(q.id).isEmpty());
    e.setForceDisabled(n, false);
    verifyRecord("Enable Force PV");
    e.setForceDisabled(n, false);
    QVERIFY(receiveQueue(q.id).isEmpty());
    QVERIFY(pv.writes.isEmpty());
    for (auto channel : d.channels()) QCOMPARE(e.state(channel).mask.text(), QString("-----"));
    QFile file(o.opmodFile);
    if (noLog) {
      QVERIFY(!file.exists());
    } else {
      QVERIFY(file.open(QIODevice::ReadOnly));
      const auto records = file.readAll();
      QCOMPARE(records.count('\n'), 3);
      QVERIFY(records.contains("Change Force PV gate -D--- 2 0"));
      QVERIFY(records.contains("Disable Force PV"));
      QVERIFY(records.contains("Enable Force PV"));
    }
  }

  void groupAcknowledgementAudit() {
    Queue q;
    QVERIFY(q.id >= 0);
    Options o;
    o.noLog = true;
    o.databaseKey = q.key;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root active\nCHANNEL root normal\n"
                         "CHANNEL root suppressed --A--\nGROUP root branch\nCHANNEL branch child");
    Engine e(d);
    Logging log(o, "root");
    e.operation = [&](Node* n, const QString& s, OperationKind kind) { log.operation(n, s, kind); };
    e.acknowledgement = [&](Node* n) { log.acknowledgement(n); };
    for (auto n : d.channels())
      e.event(n, {3, n->name == "normal" ? 0 : 2, 0, 1, "0"});
    e.acknowledge(d.root.get());
    auto group = receiveQueue(q.id);
    QVERIFY(group.contains("Local Ack Group"));
    auto first = receiveQueue(q.id), second = receiveQueue(q.id);
    QVERIFY(first.startsWith("2 6 root "));
    QVERIFY(first.contains("Ack Channel--- active"));
    QVERIFY(second.startsWith("2 6 root "));
    QVERIFY(second.contains("Ack Channel--- child"));
    QVERIFY(receiveQueue(q.id).isEmpty());
    QCOMPARE(e.state(d.root.get()).unack, 0);
  }
#endif
  void acknowledgementSeverityAudit_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("group");
    QTest::addColumn<bool>("transient"); QTest::addColumn<bool>("accepted");
    for (bool global : {false, true})
      for (bool group : {false, true})
        for (bool transient : {false, true})
          for (bool accepted : {false, true}) {
            if (!global && !accepted) continue;
            QTest::newRow(qPrintable(QString("%1-%2-%3-%4")
                .arg(global ? "global" : "local", group ? "group" : "channel",
                     transient ? "transient" : "active", accepted ? "accepted" : "failed")))
                << global << group << transient << accepted;
          }
  }
  void acknowledgementSeverityAudit() {
    QFETCH(bool, global); QFETCH(bool, group); QFETCH(bool, transient); QFETCH(bool, accepted);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.engine.global = global;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
#ifndef Q_OS_WIN
    Queue queue; QVERIFY(queue.id >= 0); o.databaseKey = queue.key;
#endif
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ACKPV ack-output 7\n");
    AuditPv pv; pv.writable = accepted;
    Engine e(d, o.engine, &pv); Logging log(o, "root");
    e.operation = [&](Node* n, const QString& text, OperationKind kind) { log.operation(n, text, kind); };
    e.acknowledgement = [&](Node* n) { log.acknowledgement(n); };
    auto channel = d.channels()[0], target = group ? d.root.get() : channel;
    e.event(channel, {}); e.event(channel, {3, 2, 2, 1, "2"});
    if (transient) e.event(channel, {0, 0, 2, 1, "0"});
    e.acknowledge(target);
    const auto expected = (QString("root: %1:  %2 Ack %3 (MAJOR) ")
        .arg(target->name, global ? "Global" : "Local", group ? "Group" : "Channel") +
        QString(transient ? "NO_ALARM" : "MAJOR").leftJustified(16, ' ')).toLocal8Bit();
    QFile file(log.opmodPath()); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto first = file.readLine();
    if (!accepted) {
      QVERIFY(first.contains("Global acknowledgement failed: ACKS request was not submitted"));
      QCOMPARE(e.state(channel).unack, 2);
      QCOMPARE(pv.writes, QVector<WriteKind>({WriteKind::Acknowledge}));
#ifndef Q_OS_WIN
      QVERIFY(receiveQueue(queue.id).isEmpty()); // No acknowledgement-success database records.
#endif
      return;
    }
    if (global) QCOMPARE(pv.writes, QVector<WriteKind>({WriteKind::Acknowledge, WriteKind::AckValue}));
    QVERIFY2(first.endsWith(expected + '\n'), first.constData());
#ifndef Q_OS_WIN
    const auto summary = receiveQueue(queue.id);
    QVERIFY(summary.startsWith(group ? "2 6 root " : "2 5 root "));
    QVERIFY2(summary.endsWith(expected), summary.constData());
    const auto channelAudit = receiveQueue(queue.id);
    QVERIFY(channelAudit.startsWith("2 6 root "));
    QVERIFY(channelAudit.contains("Ack Channel--- pv"));
    QVERIFY(receiveQueue(queue.id).isEmpty());
#endif
  }
  void datedAndXml() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.dated = true;
    o.xml = true;
    auto d = parseConfig("GROUP NULL test\nCHANNEL test pv");
    State s;
    s.value = "a<b";
    Logging log(o, "test");
    log.alarm(d.channels()[0], s, QDateTime::currentMSecsSinceEpoch());
    QVERIFY(log.alarmPath().endsWith(QDate::currentDate().toString(".yyyy-MM-dd")));
    QFile f(log.alarmPath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto text = f.readAll();
    QVERIFY(text.contains("<entry>"));
    QVERIFY(text.contains("a&lt;b"));
  }
#ifndef Q_OS_WIN
  void lockHandoff() {
    QTemporaryDir dir;
    Options o;
    o.lock = true;
    o.noLog = true;
    o.lockFile = dir.filePath("shared");
    auto program = "import fcntl,sys; f=open(sys.argv[1],'a+'); fcntl.lockf(f,fcntl.LOCK_EX); "
                   "print('locked',flush=True); sys.stdin.read()";
    Child native;
    native.start("python3", {"-c", program, o.lockFile + ".LOCK"});
    QVERIFY(native.waitForStarted());
    QVERIFY(native.waitForReadyRead());
    QVERIFY(native.readAllStandardOutput().contains("locked"));
    Logging first(o, "test");
    QVERIFY(!first.isMaster());
    native.kill();
    QVERIFY(native.waitForFinished());
    QTRY_VERIFY_WITH_TIMEOUT(first.isMaster(), 25000);
    {
      Logging second(o, "test");
      QVERIFY(second.isMaster());
    }
    // Closing a second window must not release this process's shared lock.
    Child competitor;
    competitor.start("python3", {"-c", program, o.lockFile + ".LOCK"});
    QVERIFY(competitor.waitForStarted());
    QVERIFY(!competitor.waitForReadyRead(200));
    QCOMPARE(competitor.state(), QProcess::Running);
  }
  void aliasedLockLifetime_data() {
    QTest::addColumn<QString>("aliasKind");
    QTest::newRow("directory-symlink") << QString("directory");
    QTest::newRow("file-symlink") << QString("file");
    QTest::newRow("hard-link") << QString("hard");
  }
  void aliasedLockLifetime() {
    QFETCH(QString, aliasKind);
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkdir("real"));
    Options o;
    o.lock = true;
    o.noLog = true;
    o.config = dir.filePath("real/config");
    auto lockPath = o.config + ".LOCK";
    auto first = std::make_unique<Logging>(o, "root");
    QVERIFY(first->isMaster());
    if (aliasKind == "directory") {
      QVERIFY(QFile::link(dir.filePath("real"), dir.filePath("alias")));
      o.config = dir.filePath("alias/config");
    } else {
      o.config = dir.filePath("alias");
      if (aliasKind == "file")
        QVERIFY(QFile::link(lockPath, o.config + ".LOCK"));
      else
        QCOMPARE(::link(QFile::encodeName(lockPath).constData(),
                        QFile::encodeName(o.config + ".LOCK").constData()), 0);
    }
    {
      Logging second(o, "root");
      QVERIFY(second.isMaster());
    }
    QVERIFY(first->isMaster());
    auto acquireExternally = [&] {
      Child competitor;
      competitor.start("python3", {"-c",
                                   "import fcntl,sys; f=open(sys.argv[1],'a+'); "
                                   "fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)",
                                   lockPath});
      if (!competitor.waitForFinished())
        return -1;
      return competitor.exitCode();
    };
    QCOMPARE(acquireExternally(), 1);
    first.reset();
    QCOMPARE(acquireExternally(), 0);
  }
  void changedConfigurationLock_data() {
    QTest::addColumn<bool>("explicitLock");
    QTest::newRow("default-follows-config") << false;
    QTest::newRow("explicit-original-config") << true;
  }
  void changedConfigurationLock() {
    QFETCH(bool, explicitLock);
    QTemporaryDir dir;
    Options o;
    o.lock = true;
    o.noLog = true;
    o.config = dir.filePath("first");
    if (explicitLock)
      o.lockFile = o.config;
    Logging first(o, "first");
    QVERIFY(first.isMaster());
    o.config = dir.filePath("second"); // File -> Open copies options and replaces config.
    Logging second(o, "second");
    QVERIFY(second.isMaster());
    Child competitor;
    competitor.start("python3", {"-c",
                                 "import fcntl,sys; f=open(sys.argv[1],'a+'); "
                                 "fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)",
                                 o.config + ".LOCK"});
    QVERIFY(competitor.waitForFinished());
    QCOMPARE(competitor.exitCode(), explicitLock ? 0 : 1);
  }
#endif
  void failedStartupLogRecovery_data() {
    QTest::addColumn<bool>("alarmFails");
    QTest::newRow("operation") << false;
    QTest::newRow("alarm") << true;
  }
  void failedStartupLogRecovery() {
    QFETCH(bool, alarmFails);
    QTemporaryDir dir; Options o; o.maxRecords = 4;
    o.alarmFile = dir.filePath(alarmFails ? "missing/alarm" : "alarm");
    o.opmodFile = dir.filePath(alarmFails ? "operations" : "missing/operations");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Logging log(o, "root"); QStringList errors;
    log.error = [&](const QString& text) { errors << text; };
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(errors[0].contains(alarmFails ? "alarm log" : "operation log"));
    State s; s.value = "before-recovery";
    log.alarm(d.channels()[0], s, 1000); log.operation(nullptr, "before-recovery");
    QCoreApplication::processEvents(); QCOMPARE(errors.size(), 1);
    QVERIFY(QDir(dir.path()).mkdir("missing"));
    s.value = "after-recovery";
    log.alarm(d.channels()[0], s, 2000); log.operation(nullptr, "after-recovery");
    for (bool alarm : {false, true}) {
      QFile file(alarm ? o.alarmFile : o.opmodFile); QVERIFY(file.open(QIODevice::ReadOnly));
      const auto text = file.readAll(); QVERIFY(text.contains("after-recovery"));
      QCOMPARE(text.contains("before-recovery"), alarm != alarmFails);
    }
    QCoreApplication::processEvents(); QCOMPARE(errors.size(), 1);
  }
  void pendingStartupBroadcast_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("message") << 0; QTest::newRow("reload") << 1; QTest::newRow("stop-logging") << 2;
  }
  void pendingStartupBroadcast() {
    QFETCH(int, mode);
    QTemporaryDir dir; Options o; o.noLog = true; o.broadcast = true;
    o.config = dir.filePath("config");
    Logging sender(o, "root");
    QVERIFY(sender.sendBroadcast("pending startup message", mode == 2 ? 1 : 0, mode == 1));
    Logging receiver(o, "root"); int messages = 0, reloads = 0; QString received;
    receiver.message = [&](const QString& text) { ++messages; received = text; };
    receiver.reload = [&] { ++reloads; };
    QTRY_COMPARE(messages, 1); QVERIFY(received.contains("pending startup message"));
    QCOMPARE(reloads, mode == 1 ? 1 : 0);
    QCOMPARE(receiver.commandsAllowed(), mode != 2);
    QTest::qWait(2200); QCOMPARE(messages, 1); QCOMPARE(reloads, mode == 1 ? 1 : 0);
  }
  void incompleteBroadcast_data() {
    QTest::addColumn<int>("mode"); QTest::addColumn<bool>("missingFrom");
    for (int mode : {0, 1, 2}) for (bool missingFrom : {false, true})
      QTest::newRow(qPrintable(QString("mode-%1-missingFrom-%2").arg(mode).arg(missingFrom)))
          << mode << missingFrom;
  }
  void incompleteBroadcast() {
    QFETCH(int, mode); QFETCH(bool, missingFrom);
    QTemporaryDir dir; Options o; o.noLog = true; o.broadcast = true;
    o.config = dir.filePath("config");
    int polls = 0, messages = 0, reloads = 0;
    LoggingClock clock; clock.wall = [&] { ++polls; return QDateTime::currentMSecsSinceEpoch(); };
    Logging log(o, "root", nullptr, clock); QString received;
    log.message = [&](const QString& text) { ++messages; received = text; };
    log.reload = [&] { ++reloads; };
    const QByteArray body = mode == 1 ? "RELOAD_FACILITY: complete" : mode == 2
        ? "1 MIN  ALH  WILL  NOT  SAVE ALARM LOG!!!! complete" : "complete message";
    const auto complete = "id1\n" + body + "\nDate is test\nFROM: User=test";
    QFile file(o.config + ".MESS"); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(missingFrom ? "id1\n" + body + "\nDate is test\n" : "id1\n" + body.left(4));
    file.close();
    int before = polls; QTRY_VERIFY(polls > before);
    QCOMPARE(messages, 0); QCOMPARE(reloads, 0); QVERIFY(log.commandsAllowed());
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write(complete); file.close();
    QTRY_COMPARE(messages, 1); QCOMPARE(reloads, mode == 1 ? 1 : 0);
    QCOMPARE(received.toLocal8Bit(), complete.mid(4));
    QCOMPARE(log.commandsAllowed(), mode != 2);
    before = polls; QTRY_VERIFY(polls > before);
    QCOMPARE(messages, 1); QCOMPARE(reloads, mode == 1 ? 1 : 0);
  }
  void broadcastLockRecovery_data() {
    QTest::addColumn<bool>("onSend");
    QTest::newRow("timer") << false; QTest::newRow("send") << true;
  }
  void broadcastLockRecovery() {
    QFETCH(bool, onSend);
    QTemporaryDir dir; Options o; o.noLog = true; o.broadcast = true;
    o.config = dir.filePath("missing/config");
    Logging log(o, "root"); QStringList errors;
    log.error = [&](const QString& text) { errors << text; };
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(errors[0].contains(o.config + ".MESSLOCK"));
    QVERIFY(!log.sendBroadcast("still unavailable"));
    QCoreApplication::processEvents(); QCOMPARE(errors.size(), 1);
    QVERIFY(QDir(dir.path()).mkdir("missing"));
    if (!onSend) QTRY_VERIFY(QFileInfo::exists(o.config + ".MESSLOCK"));
    int reloads = 0; log.reload = [&] { ++reloads; };
    QVERIFY(log.sendBroadcast("recovered", 0, true)); QCOMPARE(reloads, 1);
    Logging other(o, "root");
    QVERIFY(!other.sendBroadcast("cannot replace pending message"));
    QFile file(o.config + ".MESS"); QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().contains("RELOAD_FACILITY: recovered"));
  }
  void multilineAlarmRecords_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<bool>("description");
    for (bool xml : {false, true}) for (bool description : {false, true})
      QTest::newRow(qPrintable(QString("xml-%1-description-%2").arg(xml).arg(description))) << xml << description;
  }
  void multilineAlarmRecords() {
    QFETCH(bool, xml); QFETCH(bool, description);
    QTemporaryDir dir; Options o; o.lock = true; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 2; o.xml = xml; o.engine.description = description;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    {
      Logging log(o, "root");
      state.value = "old"; log.alarm(d.channels()[0], state, 1000);
      state.value = "head\ntail\rvalue"; state.description = "desc\nnext\rpart";
      log.alarm(d.channels()[0], state, 2000);
    }
    {
      QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::ReadOnly)); const auto data = file.readAll();
      QCOMPARE(data.count('\n'), 2); QVERIFY(!data.contains('\r'));
      QVERIFY(data.contains(xml ? "head&#10;tail&#13;value" : "head\\ntail\\rvalue"));
      if (description) QVERIFY(data.contains(xml ? "desc&#10;next&#13;part" : "desc\\nnext\\rpart"));
    }
    { Logging log(o, "root"); state.value = "third"; log.alarm(d.channels()[0], state, 3000); }
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::ReadOnly)); const auto data = file.readAll();
    QCOMPARE(data.count('\n'), 2); QVERIFY(!data.contains("old"));
    QVERIFY(data.contains("head")); QVERIFY(data.contains("tail")); QVERIFY(data.contains("third"));
  }
  void legacyMultilineRecovery_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<bool>("checkpoint");
    for (bool xml : {false, true}) for (bool checkpoint : {false, true})
      QTest::newRow(qPrintable(QString("xml-%1-checkpoint-%2").arg(xml).arg(checkpoint))) << xml << checkpoint;
  }
  void legacyMultilineRecovery() {
    QFETCH(bool, xml); QFETCH(bool, checkpoint);
    QTemporaryDir dir; Options o; o.lock = true; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod"); o.maxRecords = 2; o.xml = xml;
    // With a checkpoint, equal timestamps and reversed physical order require
    // matching the original logical-record fingerprint before escaping newlines.
    auto record = [xml](int second, const QByteArray& value) -> QByteArray {
      const auto time = QByteArray("00:00:0") + QByteArray::number(second);
      return xml ? "<entry><date>01-Jan-2026</date> <time>" + time + "</time> <value>" + value + "</value></entry>\n"
                 : "01-Jan-2026 " + time + " : pv NO_ALARM MAJOR " + value + "\n";
    };
    const auto old = record(1, "old"), recent = record(checkpoint ? 1 : 2, "head\ntail\rvalue");
    const QList<QByteArray> records = checkpoint ? QList<QByteArray>{recent, old} : QList<QByteArray>{old, recent};
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::WriteOnly));
    for (const auto& row : records) file.write(row);
    file.close();
    if (checkpoint) {
      QByteArray digest(32, 0);
      for (int i = 0; i < records.size(); ++i) {
        const auto hash = QCryptographicHash::hash(QByteArray::number(i) + ':' + records[i], QCryptographicHash::Sha256);
        for (int j = 0; j < 32; ++j) digest[j] = char(digest.at(j) ^ hash.at(j));
      }
      QByteArray slot(96, 0); slot.replace(0, 8, "ALHPOS01");
      qToBigEndian<quint64>(5, slot.data() + 8); qToBigEndian<quint64>(2, slot.data() + 16);
      qToBigEndian<quint64>(1, slot.data() + 24); slot.replace(32, 32, digest);
      slot.replace(64, 32, QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256));
      QFile metadata(o.alarmFile + ".qtalh-position"); QVERIFY(metadata.open(QIODevice::WriteOnly)); metadata.write(slot);
    }
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state; state.value = "third";
    { Logging log(o, "root"); log.alarm(d.channels()[0], state, QDateTime(QDate(2026, 1, 1), QTime(0, 0, 3)).toMSecsSinceEpoch()); }
    QVERIFY(file.open(QIODevice::ReadOnly)); const auto data = file.readAll(); file.close();
    QCOMPARE(data.count('\n'), 2); QVERIFY(!data.contains("old"));
    QVERIFY(data.contains(xml ? "head&#10;tail&#13;value" : "head\\ntail\\rvalue"));
    QVERIFY(data.contains("third"));
    { Logging log(o, "root"); state.value = "fourth"; log.alarm(d.channels()[0], state, 4000); }
    QVERIFY(file.open(QIODevice::ReadOnly)); const auto after = file.readAll();
    QCOMPARE(after.count('\n'), 2); QVERIFY(!after.contains("head"));
    QVERIFY(after.contains("third")); QVERIFY(after.contains("fourth"));
  }
  void logRotation() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 2;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    Logging log(o, "test");
    State s;
    s.value = "record-one";
    log.alarm(d.channels()[0], s, 1000);
    auto one = QFileInfo(o.alarmFile).size();
    s.value = "record-two";
    log.alarm(d.channels()[0], s, 2000);
    s.value = "record-end";
    log.alarm(d.channels()[0], s, 3000);
    QCOMPARE(QFileInfo(o.alarmFile).size(), 2 * one);
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto text = file.readAll();
    QVERIFY(text.contains("record-end"));
    QVERIFY(!text.contains("record-one"));
  }
  void sharedAlarmFile_data() {
    QTest::addColumn<bool>("locked");
    QTest::addColumn<QString>("alias");
    for (bool locked : {false, true})
      for (const auto& alias : {"direct", "directory", "canonical", "file", "hardlink"}) {
#ifdef Q_OS_WIN
        // QFile::link creates shortcuts on Windows, not filesystem symlinks.
        if (QByteArray(alias) != "direct" && QByteArray(alias) != "hardlink")
          continue;
#endif
        auto row = QByteArray(locked ? "locked-" : "default-") + alias;
        QTest::newRow(row.constData()) << locked << QString(alias);
      }
  }
  void sharedAlarmFile() {
    QFETCH(bool, locked);
    QFETCH(QString, alias);
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkdir("real"));
    Options o;
    auto realPath = dir.filePath("real/alarm");
    o.alarmFile = realPath;
    if (alias == "directory" || alias == "canonical") {
      QVERIFY(QFile::link(dir.filePath("real"), dir.filePath("alias")));
      o.alarmFile = dir.filePath("alias/alarm");
    } else if (alias == "file") {
      // The target does not exist until the first logger opens it.
      QVERIFY(QFile::link(realPath, dir.filePath("alias")));
      o.alarmFile = dir.filePath("alias");
    }
    o.opmodFile = dir.filePath("opmod");
    o.lockFile = dir.filePath("lock");
    o.lock = locked;
    o.maxRecords = 3;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    auto n = d.channels()[0];
    State s;
    QVERIFY(!QFileInfo::exists(realPath));
    auto first = std::make_unique<Logging>(o, "first");
    s.value = "record-one";
    first->alarm(n, s, 1000);
    if (alias == "canonical")
      o.alarmFile = realPath;
    if (alias == "hardlink") {
      o.alarmFile = dir.filePath("hardlink");
#ifdef Q_OS_WIN
      QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(o.alarmFile.utf16()),
                              reinterpret_cast<LPCWSTR>(realPath.utf16()), nullptr));
#else
      QCOMPARE(::link(QFile::encodeName(realPath).constData(),
                      QFile::encodeName(o.alarmFile).constData()), 0);
#endif
    }
    Logging second(o, "second");
    QFile initial(realPath);
    QVERIFY(initial.open(QIODevice::ReadOnly));
    QVERIFY(initial.readAll().contains("record-one"));
    s.value = "record-two";
    second.alarm(n, s, 2000);
    s.value = "record-three";
    first->alarm(n, s, 3000);
    QFile f(o.alarmFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto text = f.readAll();
    QVERIFY(text.contains("record-one"));
    QVERIFY(text.contains("record-two"));
    QVERIFY(text.contains("record-three"));
    s.value = "record-four";
    second.alarm(n, s, 4000);
    first.reset(); // Closing one window must leave the writer usable.
    s.value = "record-five";
    second.alarm(n, s, 5000);
    f.seek(0);
    text = f.readAll();
    QCOMPARE(text.count('\n'), 3);
    QVERIFY(!text.contains("record-one"));
    QVERIFY(!text.contains("record-two"));
    QVERIFY(text.contains("record-three"));
    QVERIFY(text.contains("record-four"));
    QVERIFY(text.contains("record-five"));
    Logging third(o, "third");
    s.value = "record-six";
    third.alarm(n, s, 6000);
    f.seek(0);
    text = f.readAll();
    QCOMPARE(text.count('\n'), 3);
    QVERIFY(!text.contains("record-three"));
    QVERIFY(text.contains("record-four"));
    QVERIFY(text.contains("record-five"));
    QVERIFY(text.contains("record-six"));
  }
  void lockedDefaultPreservesHistory() {
    QTemporaryDir dir;
    auto o = parseOptions({"qtalh", "-L", "-l", dir.path(), dir.filePath("facility")});
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray history;
    for (int i = 0; i < 2005; ++i)
      history += "12-Sep-2026 01:00:00 : historical-" + QByteArray::number(i) + '\n';
    QCOMPARE(file.write(history), qint64(history.size()));
    file.close();
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state;
    state.value = "new-record";
    {
      Logging log(o, "root");
      QVERIFY(log.isMaster());
      log.alarm(d.channels()[0], state, QDateTime::currentMSecsSinceEpoch());
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto text = file.readAll();
    QVERIFY(text.startsWith(history));
    QCOMPARE(text.count('\n'), 2006);
    QVERIFY(text.contains("new-record"));
  }
  void reopenedLogRotation_data() {
    QTest::addColumn<bool>("dated");
    QTest::addColumn<bool>("xml");
    QTest::newRow("locked-text") << false << false;
    QTest::newRow("locked-xml") << false << true;
    QTest::newRow("dated-text") << true << false;
    QTest::newRow("dated-xml") << true << true;
  }
  void reopenedLogRotation() {
    QFETCH(bool, dated);
    QFETCH(bool, xml);
    const auto today = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.lockFile = dir.filePath("lock");
    o.lock = !dated;
    o.dated = dated;
    o.xml = xml;
    o.maxRecords = 2;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    auto n = d.channels()[0];
    State s;
    QString path;
    {
      Logging log(o, "first");
      path = log.alarmPath();
      s.value = "initial";
      log.alarm(n, s, today + 1000);
    }
    {
      Logging log(o, "second");
      for (int i = 0; i < 10; ++i) {
        s.value = QString("record-%1-").arg(i) + QString(i, 'x');
        log.alarm(n, s, today + 2000 + i);
      }
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    auto text = f.readAll();
    QCOMPARE(text.count('\n'), 2);
    QVERIFY(text.contains("record-8-"));
    QVERIFY(text.contains("record-9-"));
    QVERIFY(!text.contains("initial"));
    if (xml) {
      QCOMPARE(text.count("<entry>"), 2);
      QCOMPARE(text.count("</entry>"), 2);
    }
  }
  void wrappedLogRestart_data() {
    QTest::addColumn<bool>("dated");
    QTest::addColumn<bool>("xml");
    QTest::addColumn<bool>("sameTime");
    QTest::newRow("locked-text") << false << false << false;
    QTest::newRow("locked-xml") << false << true << false;
    QTest::newRow("dated-text") << true << false << false;
    QTest::newRow("dated-xml") << true << true << false;
    QTest::newRow("equal-timestamps") << false << false << true;
    QTest::newRow("equal-timestamps-xml") << false << true << true;
  }
  void wrappedLogRestart() {
    QFETCH(bool, dated);
    QFETCH(bool, xml);
    QFETCH(bool, sameTime);
    const auto today = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.lockFile = dir.filePath("lock");
    o.lock = !dated;
    o.dated = dated;
    o.xml = xml;
    o.maxRecords = 3;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    QString path;
    auto write = [&](Logging& log, int i) {
      state.value = QString("record-%1").arg(i);
      log.alarm(d.channels()[0], state, today + (sameTime ? 1000 : 1000 * i));
    };
    {
      Logging log(o, "first");
      path = log.alarmPath();
      for (int i = 1; i <= 4; ++i)
        write(log, i);
    }
    {
      Logging log(o, "second");
      write(log, 5);
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto text = file.readAll();
    QCOMPARE(text.count('\n'), 3);
    QVERIFY(!text.contains("record-2"));
    for (int i = 3; i <= 5; ++i)
      QVERIFY(text.contains(QString("record-%1").arg(i).toLatin1()));
    // Reopening again with a smaller capacity must trim the oldest record.
    o.maxRecords = 2;
    {
      Logging log(o, "third");
      write(log, 6);
    }
    file.seek(0);
    text = file.readAll();
    QCOMPARE(text.count('\n'), 2);
    QVERIFY(text.contains("record-5"));
    QVERIFY(text.contains("record-6"));
  }
  void legacyLogPositionRecovery_data() {
    QTest::addColumn<bool>("stale");
    QTest::newRow("no-metadata") << false;
    QTest::newRow("stale-metadata") << true;
  }
  void legacyLogPositionRecovery() {
    QFETCH(bool, stale);
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.lockFile = dir.filePath("lock");
    o.lock = true;
    o.maxRecords = 3;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    {
      Logging log(o, "first");
      for (int i = 1; i <= 4; ++i) {
        state.value = QString("record-%1").arg(i);
        log.alarm(d.channels()[0], state, i * 1000);
      }
    }
    QFile file(o.alarmFile);
    if (stale) {
      // Simulate an external writer changing the ring without updating metadata.
      QVERIFY(file.open(QIODevice::ReadWrite));
      auto rows = file.readAll().split('\n');
      rows.removeLast();
      std::rotate(rows.begin(), rows.begin() + 1, rows.end());
      file.seek(0);
      file.write(rows.join('\n') + '\n');
      file.close();
    } else {
      QVERIFY(QFile::remove(checkpointFor(o.alarmFile)));
    }
    {
      Logging log(o, "second");
      state.value = "record-5";
      log.alarm(d.channels()[0], state, 5000);
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto text = file.readAll();
    QCOMPARE(text.count('\n'), 3);
    QVERIFY(!text.contains("record-2"));
    QVERIFY(text.contains("record-3"));
    QVERIFY(text.contains("record-4"));
    QVERIFY(text.contains("record-5"));
  }
  void boundedLogBurst() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    QStringList errors;
    Logging log(o, "root");
    log.error = [&](const QString& message) { errors << message; };
    QElapsedTimer elapsed;
    elapsed.start();
    for (int i = 0; i < 1000; ++i) {
      state.value = QString::number(i);
      log.alarm(d.channels()[0], state, 1000);
    }
    qInfo() << "1000 bounded alarm records (ms):" << elapsed.elapsed();
    QVERIFY2(elapsed.elapsed() < 5000, "Alarm logging blocked the event thread for five seconds");
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll().count('\n'), 1000);
    QVERIFY(QFile::exists(checkpointFor(o.alarmFile)));
  }
  void positionSlotRecovery_data() {
    QTest::addColumn<QString>("damage");
    QTest::newRow("intact") << QString("none");
    QTest::newRow("older-slot-torn") << QString("older");
    QTest::newRow("newer-slot-torn-older-matches") << QString("newer");
    QTest::newRow("newer-slot-stale-older-matches") << QString("stale");
    QTest::newRow("record-written-checkpoint-torn") << QString("uncheckpointed");
    QTest::newRow("truncated-metadata") << QString("truncated");
    QTest::newRow("truncated-metadata-older-matches") << QString("truncated-older");
    QTest::newRow("invalid-cursor") << QString("cursor");
  }
  void positionSlotRecovery() {
    QFETCH(QString, damage);
    QTemporaryDir dir;
    Options o;
    o.lock = true;
    o.lockFile = dir.filePath("lock");
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 3;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    const bool fallback = damage == "uncheckpointed" || damage == "truncated" || damage == "cursor";
    QByteArray previousLog;
    {
      Logging log(o, "root");
      for (int i = 1; i <= 5; ++i) {
        state.value = QString("record-%1").arg(i);
        log.alarm(d.channels()[0], state, fallback ? i * 1000 : 1000);
        if (i == 4) {
          QFile f(o.alarmFile);
          QVERIFY(f.open(QIODevice::ReadOnly));
          previousLog = f.readAll();
        }
      }
    }
    QFile position(checkpointFor(o.alarmFile));
    QVERIFY(position.open(QIODevice::ReadWrite));
    auto checkpoints = position.readAll();
    QCOMPARE(checkpoints.size(), 192);
    QCOMPARE(qFromBigEndian<quint64>(checkpoints.constData() + 8), quint64(4));
    QCOMPARE(qFromBigEndian<quint64>(checkpoints.constData() + 96 + 8), quint64(5));
    const bool restorePrevious = damage == "newer" || damage == "stale" || damage == "truncated-older";
    if (restorePrevious) {
      QFile f(o.alarmFile);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(f.write(previousLog), qint64(previousLog.size()));
    }
    if (damage == "older") checkpoints[40] = char(checkpoints.at(40) ^ 1);
    if (damage == "newer" || damage == "uncheckpointed")
      checkpoints[96 + 40] = char(checkpoints.at(96 + 40) ^ 1);
    if (damage == "cursor") {
      qToBigEndian<quint64>(99, checkpoints.data() + 96 + 24);
      checkpoints.replace(96 + 64, 32, QCryptographicHash::hash(checkpoints.mid(96, 64), QCryptographicHash::Sha256));
    }
    if (damage.startsWith("truncated")) checkpoints.chop(1);
    QVERIFY(position.resize(checkpoints.size()));
    QVERIFY(position.seek(0));
    QCOMPARE(position.write(checkpoints), qint64(checkpoints.size()));
    position.close();
    {
      Logging log(o, "root");
      state.value = "record-6";
      log.alarm(d.channels()[0], state, fallback ? 6000 : 1000);
    }
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto result = file.readAll();
    QCOMPARE(result.count('\n'), 3);
    QVERIFY(result.contains("record-4"));
    QVERIFY(result.contains("record-6"));
    QVERIFY(result.contains(restorePrevious ? "record-3" : "record-5"));
    QVERIFY(!result.contains("record-2"));
  }
#ifndef Q_OS_WIN
  void positionFileReused() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 3;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    Logging log(o, "root");
    log.alarm(d.channels()[0], state, 1000);
    const auto path = QFile::encodeName(checkpointFor(o.alarmFile));
    struct stat initial{}, current{};
    QCOMPARE(::stat(path.constData(), &initial), 0);
    for (int i = 2; i <= 10; ++i) {
      state.value = QString::number(i);
      log.alarm(d.channels()[0], state, 1000);
      QCOMPARE(::stat(path.constData(), &current), 0);
      QCOMPARE(current.st_ino, initial.st_ino);
      QCOMPARE(current.st_size, off_t(192));
    }
    QFile metadata(QString::fromLocal8Bit(path));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    auto checkpoints = metadata.readAll();
    for (int i = 0; i < 2; ++i) {
      const auto slot = checkpoints.mid(i * 96, 96);
      QCOMPARE(slot.left(8), QByteArray("ALHPOS01"));
      QCOMPARE(QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256), slot.mid(64));
    }
  }
  void shortPositionWrite() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 1;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    Logging log(o, "root");
    QString error;
    log.error = [&](const QString& text) { error = text; };
    for (int i = 1; i <= 2; ++i) {
      state.value = QString("record-%1").arg(i);
      log.alarm(d.channels()[0], state, 1000);
    }
    QFile metadata(checkpointFor(o.alarmFile));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const auto previous = metadata.readAll();
    metadata.close();
    struct rlimit original{}, restricted{};
    QCOMPARE(getrlimit(RLIMIT_FSIZE, &original), 0);
    restricted = original;
    restricted.rlim_cur = 128; // Alarm record fits; slot at offset 96 gets a short write.
    auto handler = std::signal(SIGXFSZ, SIG_IGN);
    const int limited = setrlimit(RLIMIT_FSIZE, &restricted);
    if (limited == 0) {
      for (int i = 3; i <= 4; ++i) {
        state.value = QString("record-%1").arg(i);
        log.alarm(d.channels()[0], state, 1000);
      }
    }
    const int restored = setrlimit(RLIMIT_FSIZE, &original);
    std::signal(SIGXFSZ, handler);
    QCOMPARE(limited, 0);
    QCOMPARE(restored, 0);
    QVERIFY(error.contains("Cannot save alarm log position"));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const auto damaged = metadata.readAll();
    metadata.close();
    QCOMPARE(damaged.left(96), previous.left(96));
    QVERIFY(QCryptographicHash::hash(damaged.mid(96, 64), QCryptographicHash::Sha256) != damaged.mid(160, 32));
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().contains("record-4"));
    error.clear();
    state.value = "record-5";
    log.alarm(d.channels()[0], state, 1000);
    QVERIFY(error.isEmpty());
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const auto repaired = metadata.readAll();
    // Two failed metadata writes still represent two flushed alarm records.
    // Retrying the same slot preserves the previous successful checkpoint.
    QCOMPARE(repaired.left(96), previous.left(96));
    QCOMPARE(qFromBigEndian<quint64>(repaired.constData() + 96 + 8), quint64(5));
    QCOMPARE(QCryptographicHash::hash(repaired.mid(96, 64), QCryptographicHash::Sha256), repaired.mid(160, 32));
  }
#endif
  void logTimestampCache() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.maxRecords = 0;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    QVector<qint64> times{1000, 1999, 2000, 1500, -1, -1000, -1001};
    {
      Logging log(o, "root");
      for (auto time : times) log.alarm(d.channels()[0], state, time);
    }
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = file.readAll().split('\n');
    QCOMPARE(rows.size(), times.size() + 1);
    for (int i = 0; i < times.size(); ++i)
      QVERIFY(rows[i].startsWith(QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(times[i]),
                                                     "dd-MMM-yyyy HH:mm:ss").toLatin1()));
  }
  void positionWriteFailure() {
    QTemporaryDir dir;
    Options o;
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    Logging log(o, "root");
    QVERIFY(QDir().mkdir(checkpointFor(o.alarmFile)));
    QString error;
    log.error = [&](const QString& message) { error = message; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    State state;
    state.value = "retained";
    log.alarm(d.channels()[0], state, 1000);
    QVERIFY(error.contains("Cannot save alarm log position"));
    QFile file(o.alarmFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().contains("retained"));
    QCOMPARE(QDir(dir.path()).entryList({"alarm.qtalh-position.*"}, QDir::Files).size(), 0);
  }
  void largeLogRecovery_data() {
    QTest::addColumn<bool>("xml");
    QTest::addColumn<bool>("checkpoint");
    QTest::newRow("text-unlimited") << false << false;
    QTest::newRow("text-wrapped-checkpoint") << false << true;
    QTest::newRow("xml-wrapped-checkpoint") << true << true;
  }
  void largeLogRecovery() {
    QFETCH(bool, xml); QFETCH(bool, checkpoint);
    QTemporaryDir dir;
    Options o; o.lock = true; o.xml = xml; o.maxRecords = 8;
    o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::WriteOnly));
    constexpr int count = 100000;
    const int cursor = checkpoint ? 60000 : 0;
    QByteArray digest(32, 0);
    // Equal timestamps require the checkpoint to recover the correct tail.
    // Write the large fixture incrementally so the test does not mask RAM use.
    for (int i = 0; i < count; ++i) {
      const int logical = (i - cursor + count) % count;
      const auto id = QString("old-%1;").arg(logical, 6, 10, QChar('0')).toLatin1();
      const auto row = (xml ? QByteArray("<entry><date>13-Sep-2026</date> <time>12:00:00</time> ")
                           : QByteArray("13-Sep-2026 12:00:00 : ")) +
                       id + QByteArray(180, 'x') + (xml ? "</entry>\n" : "\n");
      QCOMPARE(file.write(row), qint64(row.size()));
      if (checkpoint) {
        const auto hash = QCryptographicHash::hash(QByteArray::number(i) + ':' + row,
                                                  QCryptographicHash::Sha256);
        for (int n = 0; n < 32; ++n) digest[n] = char(digest.at(n) ^ hash.at(n));
      }
    }
    file.close();
    if (checkpoint) {
      QFile position(o.alarmFile + ".qtalh-position");
      QVERIFY(position.open(QIODevice::WriteOnly));
      for (int i = 0; i < 2; ++i) {
        QByteArray slot(64, 0); slot.replace(0, 8, "ALHPOS01");
        qToBigEndian<quint64>(42 + i, slot.data() + 8);
        qToBigEndian<quint64>(count, slot.data() + 16);
        qToBigEndian<quint64>(cursor, slot.data() + 24);
        // The newer slot has a valid checksum but stale contents.
        slot.replace(32, 32, i ? QByteArray(32, 'x') : digest);
        slot += QCryptographicHash::hash(slot, QCryptographicHash::Sha256);
        QCOMPARE(position.write(slot), qint64(96));
      }
    }
    Logging first(o, "root"), second(o, "root");
    QStringList errors;
    first.error = second.error = [&](const QString& message) { errors << message; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    QElapsedTimer elapsed; elapsed.start();
    for (int i = 0; i < 4; ++i) {
      state.value = QString("new-%1;").arg(i);
      (i % 2 ? second : first).alarm(d.channels()[0], state, 1000);
    }
    QVERIFY2(elapsed.elapsed() < 500, "First alarms must not wait for the large-log scan");
    QVERIFY(QFileInfo(o.alarmFile).size() > 1024 * 1024); // Deferred, not already rewritten.
    int ticks = 0;
    QTimer heartbeat; heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; }); heartbeat.start();
    QByteArray records;
    auto complete = [&] {
      if (QFileInfo(o.alarmFile).size() > 16384) return false;
      if (!file.open(QIODevice::ReadOnly)) return false;
      records = file.readAll(); file.close();
      return records.contains("new-3;");
    };
    QTRY_VERIFY_WITH_TIMEOUT(complete(), 20000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(ticks > 0);
    QCOMPARE(records.count('\n'), 8);
    for (int i = count - 4; i < count; ++i)
      QVERIFY(records.contains(QString("old-%1;").arg(i, 6, 10, QChar('0')).toLatin1()));
    for (int i = 0; i < 4; ++i)
      QVERIFY(records.contains(QString("new-%1;").arg(i).toLatin1()));
  }
  void spoolReadFailure_data() {
    QTest::addColumn<int>("damage");
    QTest::addColumn<bool>("closeAfterFailure");
    for (bool close : {false, true})
      for (int damage = 0; damage < 4; ++damage)
        QTest::newRow(qPrintable(QString("%1-%2").arg(close ? "close" : "retry").arg(damage)))
            << damage << close;
  }
  void spoolReadFailure() {
    QFETCH(int, damage); QFETCH(bool, closeAfterFailure);
    QTemporaryDir dir;
    Options o; o.lock = true; o.maxRecords = 3; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray old = "13-Sep-2026 12:00:00 : " + QByteArray(200, 'x') + '\n';
    for (int i = 0; i < 10000; ++i) QCOMPARE(file.write(old), qint64(old.size()));
    file.close();
    QDir temporary(QDir::tempPath());
    const auto previous = temporary.entryList({"qtalh-alarm-spool.*"}, QDir::Files);
    auto log = std::make_unique<Logging>(o, "root");
    QStringList errors;
    log->error = [&](const QString& message) { errors << message; };
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    state.value = "spool-first"; log->alarm(doc.channels()[0], state, 1000);
    state.value = "spool-second"; log->alarm(doc.channels()[0], state, 2000);
    QString path; QByteArray saved;
    temporary.refresh();
    for (const auto& name : temporary.entryList({"qtalh-alarm-spool.*"}, QDir::Files)) {
      if (previous.contains(name)) continue;
      QFile candidate(temporary.filePath(name));
      if (!candidate.open(QIODevice::ReadOnly)) continue;
      const auto bytes = candidate.readAll();
      if (bytes.contains("spool-first") && bytes.contains("spool-second")) {
        path = candidate.fileName(); saved = bytes; break;
      }
    }
    QVERIFY(!path.isEmpty());
    // The first record is intact; inject a missing/short header, short payload,
    // or corrupt length for the second entry before the first drain callback.
    const int second = 8 + qFromBigEndian<quint32>(saved.constData() + 4);
    QByteArray broken = damage == 0 ? saved.left(second) : damage == 1 ? saved.left(second + 4)
                      : damage == 2 ? saved.left(saved.size() - 1) : saved;
    if (damage == 3) qToBigEndian<quint32>(0xffffffff, broken.data() + second + 4);
    auto replaceSpool = [&](const QByteArray& bytes) {
      QFile spool(path); if (!spool.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
      return spool.write(bytes) == bytes.size() && spool.flush();
    };
    QVERIFY(replaceSpool(broken));
    QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors[0].contains("Cannot read alarm spool " + path));
    QVERIFY(errors[0].contains(QString("at byte %1").arg(second)));
    QVERIFY(errors[0].contains(o.alarmFile));
    QVERIFY(QFileInfo::exists(path));
    if (closeAfterFailure) {
      QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Unwritten alarms retained in spool .*"));
      log.reset();
      QVERIFY(QFileInfo::exists(path));
    } else {
      QTest::qWait(1200);
      QCOMPARE(errors.size(), 1); // A persistent failure is reported once.
    }
    QVERIFY(replaceSpool(saved));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 10000);
    QVERIFY(file.open(QIODevice::ReadOnly)); const auto records = file.readAll();
    QCOMPARE(records.count("spool-first"), 1);
    QCOMPARE(records.count("spool-second"), 1);
    QVERIFY(records.indexOf("spool-first") < records.indexOf("spool-second"));
    QCOMPARE(errors.size(), 1);
  }
  void changedLogRecovery_data() {
    QTest::addColumn<bool>("closeAfterFailure");
    QTest::newRow("retry-in-event-loop") << false;
    QTest::newRow("retry-at-close") << true;
  }
  void changedLogRecovery() {
    QFETCH(bool, closeAfterFailure);
    QTemporaryDir dir;
    Options o; o.lock = true; o.maxRecords = 4; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray row = "13-Sep-2026 12:00:00 : " + QByteArray(200, 'x') + '\n';
    for (int i = 0; i < 40000; ++i) QCOMPARE(file.write(row), qint64(row.size()));
    file.close();
    auto log = std::make_unique<Logging>(o, "root");
    QStringList errors;
    // Keep changing the snapshot until recovery rejects it, then allow its
    // retry to succeed. The rejected snapshot must not consume the FIFO head.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
    QTimer change; change.setInterval(1);
    connect(&change, &QTimer::timeout, [&] {
      QCOMPARE(file.write(row), qint64(row.size())); QVERIFY(file.flush());
    });
    log->error = [&](const QString& message) { errors << message; change.stop(); };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    for (int i = 0; i < 3; ++i) {
      state.value = QString("queued-%1;").arg(i);
      log->alarm(d.channels()[0], state, 1000 + i);
    }
    change.start();
    QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 20000);
    QCOMPARE(errors, QStringList{"Alarm log changed during recovery"});
    file.close();
    if (closeAfterFailure) log.reset();
    QByteArray records;
    auto complete = [&] {
      if (QFileInfo(o.alarmFile).size() > 16384 || !file.open(QIODevice::ReadOnly)) return false;
      records = file.readAll(); file.close();
      return records.contains("queued-2;");
    };
    QTRY_VERIFY_WITH_TIMEOUT(complete(), 20000);
    QCOMPARE(records.count('\n'), 4);
    for (int i = 0; i < 3; ++i) QCOMPARE(records.count(QString("queued-%1;").arg(i).toLatin1()), 1);
    QVERIFY(records.indexOf("queued-0;") < records.indexOf("queued-1;"));
    QVERIFY(records.indexOf("queued-1;") < records.indexOf("queued-2;"));
    QCOMPARE(errors.size(), 1);
  }
  void closeDuringLogRecovery_data() {
    QTest::addColumn<bool>("switchFile");
    QTest::addColumn<bool>("failCheckpoint");
    QTest::newRow("current-file") << false << false;
    QTest::newRow("previous-file") << true << false;
    QTest::newRow("teardown-error") << false << true;
  }
  void closeDuringLogRecovery() {
    QFETCH(bool, switchFile); QFETCH(bool, failCheckpoint);
    QTemporaryDir dir;
    Options o; o.lock = true; o.maxRecords = 3; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray row = "13-Sep-2026 12:00:00 : " + QByteArray(200, 'x') + '\n';
    for (int i = 0; i < 10000; ++i) QCOMPARE(file.write(row), qint64(row.size()));
    file.close();
    if (failCheckpoint) {
      QVERIFY(QDir().mkdir(checkpointFor(o.alarmFile)));
      QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Cannot save alarm log position:.*"));
    }
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    bool lateCallback = false;
    {
      Logging log(o, "root");
      log.error = [&](const QString&) { lateCallback = true; };
      state.value = "queued-before-close"; log.alarm(d.channels()[0], state, 1000);
      if (switchFile) {
        log.setAlarmFile(dir.filePath("next"));
        state.value = "next-destination"; log.alarm(d.channels()[0], state, 2000);
      }
      // Deliberately close without allowing the completion timer to run.
    }
    QVERIFY(file.open(QIODevice::ReadOnly)); const auto records = file.readAll();
    QCOMPARE(records.count('\n'), 3);
    QVERIFY(records.contains("queued-before-close"));
    QVERIFY(!records.contains("next-destination"));
    QVERIFY(!lateCallback); // Teardown errors must not access a closing window's members.
  }
  void closingBroadcastSender_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("message") << 0;
    QTest::newRow("stop-logging") << 1;
    QTest::newRow("reload") << 2;
  }
  void closingBroadcastSender() {
    QFETCH(int, mode);
    QTemporaryDir dir;
    Options o; o.config = dir.filePath("config"); o.broadcast = true; o.noLog = true;
    qint64 elapsed = 0;
    LoggingClock clock; clock.monotonic = [&] { return elapsed; };
    auto sender = std::make_unique<Logging>(o, "root", nullptr, clock);
    Logging receiver(o, "root");
    int received = 0, reloads = 0;
    receiver.message = [&](const QString&) { ++received; };
    receiver.reload = [&] { ++reloads; };
    QVERIFY(sender->sendBroadcast("pending delivery", mode == 1 ? 1 : 0, mode == 2));
    QVERIFY(sender->broadcastPending());
    QFile file(o.config + ".MESS"); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto original = file.readAll(); file.close();
    elapsed = 57000; // Keep a real three-second delivery lease after sender teardown.
    sender.reset();
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original); file.close();
    QVERIFY(!receiver.sendBroadcast("overwrite"));
    // The receiver has not polled yet: closing the sender must not lose this message.
    QTRY_COMPARE(received, 1);
    QCOMPARE(reloads, mode == 2 ? 1 : 0);
    QCOMPARE(receiver.commandsAllowed(), mode != 1);
    QTRY_COMPARE(QFileInfo(o.config + ".MESS").size(), qint64(0));
    QVERIFY(receiver.sendBroadcast("next message"));
  }
  void broadcastOwnership_data() {
    QTest::addColumn<bool>("aliased");
    QTest::newRow("same-path") << false;
#ifndef Q_OS_WIN
    QTest::newRow("directory-symlink") << true;
#endif
  }
  void broadcastOwnership() {
    QFETCH(bool, aliased);
    QTemporaryDir dir;
    Options o;
    o.config = dir.filePath("config");
    o.broadcast = true;
    o.noLog = true;
    qint64 elapsed = 0;
    LoggingClock clock; clock.monotonic = [&] { return elapsed; };
    auto first = std::make_unique<Logging>(o, "root", nullptr, clock);
    if (aliased) {
      QVERIFY(QFile::link(dir.path(), dir.filePath("alias")));
      o.config = dir.filePath("alias/config");
    }
    Logging second(o, "root");
    int reloads = 0;
    second.reload = [&] { ++reloads; };
    QVERIFY(first->sendBroadcast("first"));
    QFile file(o.config + ".MESS");
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto original = file.readAll();
    QVERIFY(!first->sendBroadcast("overwrite", 0, true));
    QVERIFY(!second.sendBroadcast("overwrite", 0, true));
    file.seek(0);
    QCOMPARE(file.readAll(), original);
    file.close();
    // Closing an unrelated window must not release the sender's ownership.
    {
      Logging third(o, "root");
    }
    QVERIFY(!second.sendBroadcast("overwrite"));
    elapsed = 60000;
    QTRY_COMPARE(QFileInfo(o.config + ".MESS").size(), qint64(0));
    first.reset();
    QVERIFY(second.sendBroadcast("reload", 0, true));
    QCOMPARE(reloads, 1);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto next = file.readAll();
    QVERIFY(next.left(next.indexOf('\n')) != original.left(original.indexOf('\n')));
    QVERIFY(next.indexOf('\n') < 29); // Fits legacy messID[30].
  }
  void loggingClockChanges_data() {
    QTest::addColumn<qint64>("adjustment");
    QTest::newRow("backward") << qint64(-3600000);
    QTest::newRow("forward") << qint64(3600000);
  }
  void loggingClockChanges() {
    QFETCH(qint64, adjustment);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.broadcast = true; o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    qint64 wall = QDateTime::currentMSecsSinceEpoch(), elapsed = 0;
    int reads = 0;
    LoggingClock clock{[&] { ++reads; return wall; }, [&] { return elapsed; }};
    Logging log(o, "root", nullptr, clock);
    QVERIFY(log.sendBroadcast("clock correction", 1));
    QVERIFY(!log.commandsAllowed());
    wall += adjustment; elapsed = 59999;
    const int before = reads;
    QTRY_VERIFY(reads > before); // Dispatch the real service callback with the injected clocks.
    QVERIFY(!log.commandsAllowed());
    QVERIFY(!log.sendBroadcast("too early"));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.value = "suppressed-record";
    log.alarm(d.channels()[0], state, wall);
    QFile file(o.alarmFile); QVERIFY(file.open(QIODevice::ReadOnly));
    auto records = file.readAll();
    QVERIFY(!records.contains("suppressed-record"));
    QVERIFY(!records.contains("Stop log finish"));
    ++elapsed;
    QVERIFY(log.commandsAllowed());
    QTRY_COMPARE(QFileInfo(o.config + ".MESS").size(), qint64(0));
    state.value = "resumed-record"; log.alarm(d.channels()[0], state, wall);
    file.seek(0); records = file.readAll();
    QCOMPARE(records.count("Stop log finish"), 1);
    QVERIFY(records.contains("resumed-record"));
    // Audit timestamps remain wall time, independent of the interval clock's zero origin.
    const auto stamp = QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(wall),
                                            "dd-MMM-yyyy HH:mm:ss").toLocal8Bit();
    QVERIFY(records.contains(stamp + " : Stop log finish"));
    QVERIFY(log.sendBroadcast("next broadcast"));
  }
#ifndef Q_OS_WIN
  void lockPollingClockChanges_data() { loggingClockChanges_data(); }
  void lockPollingClockChanges() {
    QFETCH(qint64, adjustment);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.lock = true; o.noLog = true; o.lockFile = dir.filePath("lock");
    Child native;
    native.start("python3", {"-c", "import fcntl,sys; f=open(sys.argv[1],'a+'); "
        "fcntl.lockf(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()",
        o.lockFile + ".LOCK"});
    QVERIFY(native.waitForStarted()); QVERIFY(native.waitForReadyRead());
    QVERIFY(native.readAllStandardOutput().contains("locked"));
    qint64 wall = QDateTime::currentMSecsSinceEpoch(), elapsed = 0;
    int reads = 0;
    LoggingClock clock{[&] { ++reads; return wall; }, [&] { return elapsed; }};
    Logging log(o, "root", nullptr, clock); QVERIFY(!log.isMaster());
    native.kill(); QVERIFY(native.waitForFinished());
    wall += adjustment; elapsed = 19999;
    const int before = reads; QTRY_VERIFY(reads > before);
    QVERIFY(!log.isMaster());
    ++elapsed; QTRY_VERIFY(log.isMaster());
  }
#endif

  void lockOpenRecovery() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.lock = true; o.noLog = true; o.lockFile = dir.filePath("missing/lock");
    qint64 elapsed = 0;
    int reads = 0;
    LoggingClock clock{[&] { ++reads; return QDateTime::currentMSecsSinceEpoch(); },
                       [&] { return elapsed; }};
    Logging log(o, "root", nullptr, clock);
    QVERIFY(!log.isMaster());
    elapsed = 20000;
    const int before = reads; QTRY_VERIFY(reads > before);
    QVERIFY(!log.isMaster()); // A repeated failure must never imply ownership.
    QVERIFY(QDir().mkpath(dir.filePath("missing")));
    elapsed = 40000;
    QTRY_VERIFY(log.isMaster());
  }
#ifndef Q_OS_WIN
  void lockOpenRecoveryWithCompetitor() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    Options o; o.lock = true; o.noLog = true; o.lockFile = dir.filePath("missing/lock");
    qint64 elapsed = 0;
    int reads = 0;
    LoggingClock clock{[&] { ++reads; return QDateTime::currentMSecsSinceEpoch(); },
                       [&] { return elapsed; }};
    auto log = std::make_unique<Logging>(o, "root", nullptr, clock);
    QVERIFY(!log->isMaster());
    QVERIFY(QDir().mkpath(dir.filePath("missing")));
    Child native;
    native.start("python3", {"-c", "import fcntl,sys; f=open(sys.argv[1],'a+'); "
        "fcntl.lockf(f,fcntl.LOCK_EX); print('locked',flush=True); sys.stdin.read()",
        o.lockFile + ".LOCK"});
    QVERIFY(native.waitForStarted()); QVERIFY(native.waitForReadyRead());
    QVERIFY(native.readAllStandardOutput().contains("locked"));
    elapsed = 20000;
    const int before = reads; QTRY_VERIFY(reads > before);
    QVERIFY(!log->isMaster()); // Open recovery cannot bypass another master's lock.
    native.kill(); QVERIFY(native.waitForFinished());
    elapsed = 40000;
    QTRY_VERIFY(log->isMaster());
    {
      Logging second(o, "root");
      QVERIFY(second.isMaster());
    }
    auto probe = [&] {
      QProcess process;
      process.start("python3", {"-c", "import fcntl,sys; f=open(sys.argv[1],'a+'); "
          "fcntl.lockf(f,fcntl.LOCK_EX|fcntl.LOCK_NB)", o.lockFile + ".LOCK"});
      if (!process.waitForFinished()) return -1;
      return process.exitCode();
    };
    QVERIFY(probe() > 0); // Closing a peer must not release the recovered lock.
    log.reset();
    QCOMPARE(probe(), 0);
  }
#endif

  void broadcast() {
    QTemporaryDir dir;
    Options o;
    o.config = dir.filePath("test.alhConfig");
    o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("opmod");
    o.broadcast = true;
    o.noLog = true;
    Logging log(o, "test");
    QString message;
    log.message = [&](QString s) { message = s; };
    QVERIFY(log.sendBroadcast("test broadcast"));
    QVERIFY(message.contains("test broadcast"));
    QFile f(o.config + ".MESS");
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.readAll().contains("FROM: User="));
    f.close();
    bool reloaded = false;
    log.reload = [&] { reloaded = true; };
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("100\nRELOAD_FACILITY: test\nDate is test\nFROM: User=test");
    f.close();
    QTRY_VERIFY(reloaded);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("101\n1 MIN  ALH  WILL  NOT  SAVE ALARM LOG!!!! test\nDate is test\nFROM: User=test");
    f.close();
    QTRY_VERIFY(!log.commandsAllowed());
  }
};
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 5 && app.arguments()[1] == "--append-probe") {
    const auto directory = app.arguments()[2], worker = app.arguments()[3];
    const auto time = app.arguments()[4].toLongLong();
    Options o; o.dated = true; o.maxRecords = 0;
    o.alarmFile = directory + "/alarm"; o.opmodFile = directory + "/opmod" + worker;
    Logging log(o, "root");
    bool failed = false;
    log.error = [&](const QString&) { failed = true; };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2; state.status = 3;
    std::puts("ready"); std::fflush(stdout);
    if (std::getchar() != '\n') return 2;
    for (int i = 0; i < 20000; ++i) {
      state.value = worker + ':' + QString::number(i);
      log.alarm(d.channels()[0], state, time);
    }
    return failed ? 1 : 0;
  }
#ifdef Q_OS_WIN
  if (app.arguments().size() == 3 && app.arguments()[1] == "--lock-probe") {
    Options o;
    o.noLog = true;
    o.lock = true;
    o.lockFile = app.arguments()[2];
    Logging log(o, "probe");
    return log.isMaster() ? 0 : 1;
  }
#endif
  HelperTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "test_helpers.moc"
