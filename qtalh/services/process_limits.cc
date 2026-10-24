// See process_limits.h.
#include "services/process_limits.h"
#include <QDir>
#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif
#ifdef Q_OS_MACOS
#include <climits>
#endif
namespace alh {
namespace {
#ifndef Q_OS_WIN
// Written once in main() before threads start; read-only afterwards.
rlim_t inheritedSoft = RLIM_INFINITY;
bool raised = false;
#endif
} // namespace
bool raiseOpenFileLimit() {
#ifdef Q_OS_WIN
  return false;
#else
  struct rlimit limit {};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= limit.rlim_max) return false;
  const auto inherited = limit.rlim_cur;
  limit.rlim_cur = limit.rlim_max;
#ifdef Q_OS_MACOS
  // macOS rejects an unlimited soft limit; its kernel ceiling is OPEN_MAX.
  if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > OPEN_MAX) limit.rlim_cur = OPEN_MAX;
#endif
  if (limit.rlim_cur <= inherited || setrlimit(RLIMIT_NOFILE, &limit) != 0) return false;
  inheritedSoft = inherited;
  raised = true;
  return true;
#endif
}
qint64 inheritedOpenFileLimit() {
#ifdef Q_OS_WIN
  return -1;
#else
  return raised ? qint64(inheritedSoft) : -1;
#endif
}
void restoreInheritedOpenFileLimit() {
#ifndef Q_OS_WIN
  if (!raised) return;
  struct rlimit limit {};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return;
  limit.rlim_cur = inheritedSoft;
  setrlimit(RLIMIT_NOFILE, &limit);
#endif
}
QString inheritedLimitShellPrefix() {
  const auto inherited = inheritedOpenFileLimit();
  return inherited < 0 ? QString() : QString("ulimit -S -n %1 2>/dev/null; ").arg(inherited);
}
OpenFiles openFiles() {
  OpenFiles result;
#ifndef Q_OS_WIN
  struct rlimit limit {};
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
    result.limit = qint64(limit.rlim_cur);
#ifdef Q_OS_LINUX
  const QDir descriptors("/proc/self/fd");
#else
  const QDir descriptors("/dev/fd");
#endif
  // The listing itself holds one descriptor while it reads the directory.
  if (descriptors.exists())
    result.open = qMax<qint64>(0, descriptors.entryList(QDir::AllEntries | QDir::System |
                                                       QDir::NoDotAndDotDot).size() - 1);
#endif
  return result;
}
QString OpenFileMonitor::check(const OpenFiles& files) {
  if (files.open < 0 || files.limit <= 0) return {};
  if (warned) {
    if (files.open * 10 < files.limit * 7) warned = false;
    return {};
  }
  if (files.open * 5 < files.limit * 4) return {};
  warned = true;
  return QString("Open files are near this process's limit: %1 of %2 descriptors in use. "
                 "Ask the system administrator to raise the open-file limit (ulimit -n).")
      .arg(files.open).arg(files.limit);
}
ChildProcess::ChildProcess(QObject* parent) : QProcess(parent) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && !defined(Q_OS_WIN)
  setChildProcessModifier(restoreInheritedOpenFileLimit);
#endif
}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void ChildProcess::setupChildProcess() { restoreInheritedOpenFileLimit(); }
#endif
} // namespace alh
