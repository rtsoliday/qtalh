// Open-file headroom. Large configurations open many CA/pvAccess sockets, so
// QtALH raises its soft descriptor limit at startup. Child processes get the
// inherited limit back, because older programs may still use select().
#pragma once
#include <QProcess>
#include <QString>
namespace alh {
struct OpenFiles {
  qint64 open = -1, limit = -1; // -1 where the platform cannot report them
};
// Call first in main(), before any thread starts. Returns false if unchanged.
bool raiseOpenFileLimit();
// The soft limit inherited at startup, or -1 when it was not changed.
qint64 inheritedOpenFileLimit();
// Restores the inherited soft limit in a forked child before exec. It makes
// one system call, so it is safe between fork() and exec().
void restoreInheritedOpenFileLimit();
// Prefix for a /bin/sh -c command that restores the inherited soft limit.
QString inheritedLimitShellPrefix();
OpenFiles openFiles();
// Warns once when use reaches 80% of the limit, again only after it has
// fallen below 70%. Returns the warning text, or an empty string.
class OpenFileMonitor {
public:
  QString check(const OpenFiles&);

private:
  bool warned = false;
};
// A QProcess whose child starts with the inherited open-file limit.
class ChildProcess : public QProcess {
public:
  explicit ChildProcess(QObject* parent = nullptr);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
protected:
  void setupChildProcess() override;
#endif
};
} // namespace alh
