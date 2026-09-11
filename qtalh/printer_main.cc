// Qt Core/Network port of ALH printer.c. See ../LICENSE.
#include "services/ipc.h"
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <sys/msg.h>
namespace {
volatile sig_atomic_t stopped = 0;
void stop(int) {
  stopped = 1;
}
} // namespace
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  auto a = app.arguments();
  bool portOk = false, keyOk = false;
  int port = a.value(2).toInt(&portOk), key = a.value(3).toInt(&keyOk);
  if (a.size() != 5 || !portOk || port < 1 || port > 65535 || !keyOk || key <= 0 ||
      !QStringList({"bw", "bw_bold", "oki_bold", "hp_color"}).contains(a.value(4))) {
    QTextStream(stderr) << "usage:qtalh_printer TCPName TCPport Key ColorModel\nColor "
                           "model={bw,bw_bold,oki_bold,hp_color}\n";
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
  QTcpSocket socket;
  QByteArray pending;
  QObject::connect(&socket, &QTcpSocket::connected, &app, [&] { socket.write(pending); });
  QObject::connect(&socket, &QTcpSocket::bytesWritten, &app, [&](qint64) {
    if (socket.bytesToWrite() == 0) {
      pending.clear();
      socket.disconnectFromHost();
    }
  });
  auto sendNext = [&] {
    if (stopped) {
      app.quit();
      return;
    }
    if (socket.state() != QAbstractSocket::UnconnectedState)
      return;
    if (pending.isEmpty()) {
      QString error;
      auto msg = alh::receiveQueue(id, &error);
      if (!error.isEmpty())
        QTextStream(stderr) << error << '\n';
      if (!msg.isEmpty())
        try {
          pending = alh::printerRecord(msg, a[4]);
        } catch (const std::exception& e) {
          QTextStream(stderr) << e.what() << '\n';
        }
    }
    if (!pending.isEmpty() && socket.state() == QAbstractSocket::UnconnectedState) {
      socket.connectToHost(a[1], port);
    }
  };
  QObject::connect(&timer, &QTimer::timeout, &app, sendNext);
  QObject::connect(&socket, &QTcpSocket::disconnected, &app, [&] {
    // Continue immediately after delivery; polling is only needed while idle
    // or retrying a failed connection. Keep at most one record in flight.
    if (pending.isEmpty())
      QTimer::singleShot(0, &app, sendNext);
  });
  timer.start();
  return app.exec();
}
