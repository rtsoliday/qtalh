#include "services/ipc.h"
#include "services/logging.h"
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
#ifndef Q_OS_WIN
#include <netinet/in.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>
#include <sys/msg.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <csignal>
#include <unistd.h>
#endif
using namespace alh;
namespace {
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
bool_t stringXdr(XDR* x, void* data, ...) {
#endif
  auto p = static_cast<char**>(data);
  return xdr_string(x, p, 8192);
}
#ifdef __APPLE__
bool_t voidXdr(XDR*, void*, unsigned int) {
#else
bool_t voidXdr(XDR*, void*, ...) {
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
  void failedLogFileChange_data() {
    QTest::addColumn<bool>("dated");
    QTest::newRow("plain") << false;
    QTest::newRow("dated") << true;
  }
  void failedLogFileChange() {
    QFETCH(bool, dated);
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
    log.alarm(n, state, 1000);
    log.operation(n, "before-failure");
    log.setAlarmFile(dir.filePath("missing/alarm"));
    log.setOpmodFile(dir.filePath("missing/opmod"));
    QCOMPARE(errors.size(), 2);
    QCOMPARE(log.alarmPath(), alarmPath);
    QCOMPARE(log.opmodPath(), opmodPath);
    state.value = "after-failure";
    log.alarm(n, state, 2000);
    log.operation(n, "after-failure");
    for (auto path : {alarmPath, opmodPath}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      auto contents = file.readAll();
      QVERIFY(contents.contains("before-failure"));
      QVERIFY(contents.contains("after-failure"));
    }
    log.setAlarmFile(dir.filePath("new-alarm"));
    log.setOpmodFile(dir.filePath("new-opmod"));
    QVERIFY(log.alarmPath() != alarmPath);
    QVERIFY(log.opmodPath() != opmodPath);
    state.value = "after-success";
    log.alarm(n, state, 3000);
    log.operation(n, "after-success");
    for (auto path : {log.alarmPath(), log.opmodPath()}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      QVERIFY(file.readAll().contains("after-success"));
    }
  }
#ifndef Q_OS_WIN
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
    e.operation = [&](Node* n, const QString& s) { log.operation(n, s); };
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
      for (const auto& alias : {"direct", "directory", "canonical", "file"}) {
#ifdef Q_OS_WIN
        // QFile::link creates shortcuts on Windows, not filesystem symlinks.
        if (QByteArray(alias) != "direct")
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
    Logging second(o, "second");
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
      log.alarm(n, s, 1000);
    }
    {
      Logging log(o, "second");
      for (int i = 0; i < 10; ++i) {
        s.value = QString("record-%1-").arg(i) + QString(i, 'x');
        log.alarm(n, s, 2000 + i);
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
      log.alarm(d.channels()[0], state, sameTime ? 1000 : 1000 * i);
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
      QVERIFY(QFile::remove(o.alarmFile + ".qtalh-position"));
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
    QVERIFY(QFile::exists(o.alarmFile + ".qtalh-position"));
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
    QFile position(o.alarmFile + ".qtalh-position");
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
    const auto path = QFile::encodeName(o.alarmFile + ".qtalh-position");
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
    QFile metadata(o.alarmFile + ".qtalh-position");
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
      state.value = "record-3";
      log.alarm(d.channels()[0], state, 1000);
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
    QVERIFY(file.readAll().contains("record-3"));
    error.clear();
    state.value = "record-4";
    log.alarm(d.channels()[0], state, 1000);
    QVERIFY(error.isEmpty());
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const auto repaired = metadata.readAll();
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
    QVERIFY(QDir().mkdir(o.alarmFile + ".qtalh-position"));
    Logging log(o, "root");
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
    auto first = std::make_unique<Logging>(o, "root");
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
    first.reset();
    QVERIFY(second.sendBroadcast("reload", 0, true));
    QCOMPARE(reloads, 1);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto next = file.readAll();
    QVERIFY(next.left(next.indexOf('\n')) != original.left(original.indexOf('\n')));
    QVERIFY(next.indexOf('\n') < 29); // Fits legacy messID[30].
  }
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
#ifdef Q_OS_WIN
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().size() == 3 && app.arguments()[1] == "--lock-probe") {
    Options o;
    o.noLog = true;
    o.lock = true;
    o.lockFile = app.arguments()[2];
    Logging log(o, "probe");
    return log.isMaster() ? 0 : 1;
  }
  HelperTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#else
QTEST_GUILESS_MAIN(HelperTests)
#endif
#include "test_helpers.moc"
