// Qt Core port of ALH alh_DB.c. RPC argument 2 is the RPC program number.
#include "services/ipc.h"
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <cstdarg>
#include <rpc/rpc.h>
#include <sys/msg.h>
namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) {
  stopped = 1;
}
#ifdef __APPLE__
bool_t voidResult(XDR*, void*, unsigned int) {
#else
bool_t voidResult(XDR*, ...) {
#endif
  return TRUE;
}
#ifdef __APPLE__
bool_t encode(XDR* x, void* data, unsigned int) {
#else
bool_t encode(XDR* x, ...) {
  // TI-RPC passes the data pointer as the first variadic argument.
  va_list args;
  va_start(args, x);
  void* data = va_arg(args, void*);
  va_end(args);
#endif
  auto s = static_cast<char**>(data);
  return xdr_string(x, s, 8192);
}
} // namespace
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  auto a = app.arguments();
  bool programOk = false, keyOk = false;
  unsigned long program = a.value(2).toULong(&programOk);
  int key = a.value(3).toInt(&keyOk);
  if (a.size() != 4 || !programOk || !program || !keyOk || key <= 0) {
    QTextStream(stderr) << "usage:qtalh_DB TCPName TCPport Key\n";
    return 1;
  }
  int id = msgget(key, 0600 | IPC_CREAT);
  if (id < 0) {
    perror("msgget");
    return 1;
  }
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  QTimer timer;
  timer.setInterval(50);
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &app, [&] {
    if (stopped) {
      app.quit();
      return;
    }
    QString error;
    // Known issue inherited from ALH: receiveQueue removes the record before
    // RPC delivery. A clnt_create or clnt_call failure below loses that record.
    // A future delivery change should retain pending records and define how to
    // handle ambiguous timeouts (the server may already have accepted them);
    // blindly retrying could duplicate database entries. Behavior is unchanged.
    auto record = alh::receiveQueue(id, &error);
    if (!error.isEmpty())
      QTextStream(stderr) << error << '\n';
    if (record.isEmpty()) {
      timer.start(50);
      return;
    }
    // Yield between records, but only wait when the queue is empty.
    timer.start(0);
    auto host = a[1].toLocal8Bit();
#ifdef __APPLE__
    // The macOS Sun RPC implementation accepts tcp/udp, not TI-RPC's netpath.
    char protocol[] = "tcp";
#else
    char protocol[] = "netpath";
#endif
    char diagnostic[] = "qtalh_DB";
    CLIENT* client = clnt_create(host.data(), program, 1, protocol);
    if (!client) {
      clnt_pcreateerror(diagnostic);
      return;
    }
    char* text = record.data();
    timeval timeout{10, 0};
    char result = 0;
    if (clnt_call(client, 1, encode, reinterpret_cast<caddr_t>(&text),
                  voidResult, &result, timeout) != RPC_SUCCESS)
      clnt_perror(client, diagnostic);
    clnt_destroy(client);
  });
  timer.start();
  return app.exec();
}
