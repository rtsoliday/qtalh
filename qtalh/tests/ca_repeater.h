// Test-owned Channel Access repeater for a private EPICS_CA_REPEATER_PORT.
#pragma once
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QUdpSocket>
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
