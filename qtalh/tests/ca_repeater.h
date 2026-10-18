// Test-owned Channel Access repeater for a private EPICS_CA_REPEATER_PORT.
#pragma once
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QUdpSocket>
// Site settings that take precedence over a harness's loopback address lists
// and private ports (port: CA server, port + 1: repeater and beacons, port + 2
// and port + 3: pvAccess server and broadcast). An empty name server list is
// no list, so an inherited shell cannot send searches or beacons elsewhere.
inline QList<QPair<QByteArray, QByteArray>> isolatedEpicsSettings(int port) {
  return {{"EPICS_CA_NAME_SERVERS", ""},
          {"EPICS_PVA_NAME_SERVERS", ""},
          {"EPICS_CAS_SERVER_PORT", QByteArray::number(port)},
          {"EPICS_CAS_BEACON_PORT", QByteArray::number(port + 1)},
          {"EPICS_CAS_AUTO_BEACON_ADDR_LIST", "NO"},
          {"EPICS_PVAS_SERVER_PORT", QByteArray::number(port + 2)},
          {"EPICS_PVAS_BROADCAST_PORT", QByteArray::number(port + 3)},
          {"EPICS_PVAS_BEACON_ADDR_LIST", "127.0.0.1"},
          {"EPICS_PVAS_AUTO_BEACON_ADDR_LIST", "NO"}};
}
// A CA client that finds no repeater on its port starts a detached caRepeater.
// That process outlives the test and keeps inherited output pipes open, so a
// piped `make test` never finishes. Start one here, before any CA client (test
// process, IOC or legacy ALH), and stop it when the test ends.
struct TestCaRepeater {
  QProcess process;
  bool start(const QString& bin, quint16 port) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("EPICS_CA_REPEATER_PORT", QString::number(port));
    process.setProcessEnvironment(environment);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    // A repeater exits if the port is taken, including briefly by the probe.
    for (int attempt = 0; attempt < 5; ++attempt) {
      process.start(bin + "/caRepeater", QStringList());
      if (!process.waitForStarted())
        return false;
      QElapsedTimer elapsed;
      elapsed.start();
      while (process.state() == QProcess::Running && elapsed.elapsed() < 5000) {
        QThread::msleep(20);
        QUdpSocket probe;
        if (!probe.bind(QHostAddress::AnyIPv4, port, QUdpSocket::DontShareAddress))
          return process.state() == QProcess::Running;
      }
      if (process.state() == QProcess::Running)
        return false;
      process.waitForFinished(1000);
    }
    return false;
  }
  ~TestCaRepeater() {
    if (process.state() == QProcess::NotRunning)
      return;
    process.kill();
    process.waitForFinished(3000);
  }
};
