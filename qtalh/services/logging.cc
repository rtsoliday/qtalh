#include "core/diagnostics.h"
// ALH logging and interoperable Linux file locking/broadcast services.
#include "logging.h"
#include "log_identity.h"
#include "log_recovery.h"
#include "ipc.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QTextStream>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QPointer>
#include <future>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif
namespace alh {
namespace {
QString identityToken(QString value, const QString& fallback) {
  value = value.trimmed();
  value.replace(QRegularExpression("\\s+"), "_");
  return value.isEmpty() ? fallback : value;
}
QString operatorIdentity() {
#ifdef Q_OS_WIN
  wchar_t name[257];
  DWORD size = sizeof(name) / sizeof(name[0]);
  if (GetUserNameW(name, &size))
    return identityToken(QString::fromWCharArray(name), "unknown_user");
#else
  struct passwd entry{}, *found = nullptr;
  QByteArray buffer(16384, 0);
  int status;
  while ((status = getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &found)) == ERANGE &&
         buffer.size() < 1024 * 1024)
    buffer.resize(buffer.size() * 2);
  if (status == 0 && found)
    return identityToken(QString::fromLocal8Bit(found->pw_name), "unknown_user");
#endif
  return "unknown_user";
}
QString displayIdentity() {
  return identityToken(qEnvironmentVariable("DISPLAY"), "unknown_display");
}
// Log framing is one physical line per record. Keep ordinary legacy bytes
// unchanged; XML character references preserve embedded control characters.
template<typename String>
String singleLine(String text, bool xml) {
  text.replace("\r", xml ? "&#13;" : "\\r");
  text.replace("\n", xml ? "&#10;" : "\\n");
  return text;
}

}
// All windows writing the same file share both the descriptor and ring position.
// Writes are serialized on the GUI thread. Recovery opens a separate read-only
// QFile on a worker; incoming records are spooled until the scan has completed.
struct AlarmLogFile : std::enable_shared_from_this<AlarmLogFile> {
  QFile file;
  std::deque<QByteArray> records;
  int limit = -1, nextRecord = 0;
  qint64 nextOffset = 0;
  bool master = false;
  QString positionError, checkpointLocation;
  QByteArray digest = QByteArray(32, 0);
#ifdef Q_OS_WIN
  QFile positionFile;
#else
  int positionFd = -1;
  bool appendMode = false;
#endif
  // A disk-backed FIFO avoids either blocking CA during a large scan or
  // accumulating an unbounded alarm burst in RAM. Small/settled rings still
  // write synchronously. Destruction/shutdown drains accepted records.
  std::future<logRecovery::Result> recovery;
  int recoveringMaximum = 0, recoveryPollMs = 10;
  static constexpr int RetryRecovery = -2;
  bool recoveryRewrite = false, scheduled = false;
  QTemporaryFile pending{QDir::tempPath() + "/qtalh-alarm-spool.XXXXXX"};
  qint64 pendingOffset = 0, pendingEnd = 0;
  QString spoolReadError;
  std::function<void(const QString&)> asyncError;
  quint64 positionSequence = 0;
  int nextPositionSlot = 1;
  // Two 96-byte slots: magic/version, sequence, count, next, ring fingerprint,
  // then SHA-256 of the preceding 64 bytes. Integers are big endian.
  static constexpr int PositionSlotSize = 96;
  ~AlarmLogFile() {
    asyncError = {};
    finishPending();
#ifndef Q_OS_WIN
    if (positionFd >= 0)
      ::close(positionFd);
#endif
  }

  void includeRecord(int index, const QByteArray& record) {
    // Include the physical slot in each hash so reordering invalidates metadata.
    auto hash = QCryptographicHash::hash(QByteArray::number(index) + ':' + record,
                                         QCryptographicHash::Sha256);
    for (int i = 0; i < digest.size(); ++i)
      digest[i] = char(digest.at(i) ^ hash.at(i));
  }
  void rebuildFingerprint() {
    digest.fill(0);
    for (int i = 0; i < int(records.size()); ++i)
      includeRecord(i, records[i]);
  }

  QString positionPath() const {
    return checkpointLocation;
  }
  bool positionFailure(const QString& reason) {
    positionError = "Cannot save alarm log position: " + reason;
#ifdef Q_OS_WIN
    positionFile.close();
#else
    if (positionFd >= 0) {
      ::close(positionFd);
      positionFd = -1;
    }
#endif
    return false;
  }
  bool savePosition() {
    if (positionSequence == std::numeric_limits<quint64>::max())
      return positionFailure("checkpoint sequence exhausted");
    // Count every flushed alarm, including those whose checkpoint fails.
    // Readers use sequence differences as record counts. Slot selection is
    // independent: failures keep retrying the same slot, preserving the last
    // good checkpoint even across several consecutive failed writes.
    const auto sequence = ++positionSequence;
#ifdef Q_OS_WIN
    if (!positionFile.isOpen()) {
      positionFile.setFileName(positionPath());
      const QFileInfo info(positionPath());
      if (info.isSymLink() || (info.exists() && !info.isFile()))
        return positionFailure("metadata is not a regular file");
      if (!positionFile.open(QIODevice::ReadWrite) || !positionFile.resize(2 * PositionSlotSize))
        return positionFailure(positionFile.errorString());
    }
#else
    if (positionFd < 0) {
      struct stat info{};
      if (::fstat(file.handle(), &info) != 0)
        return positionFailure(QString::fromLocal8Bit(strerror(errno)));
      const auto permissions = info.st_mode & 0777;
      positionFd = ::open(QFile::encodeName(positionPath()).constData(),
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, permissions);
      if (positionFd < 0)
        return positionFailure(QString::fromLocal8Bit(strerror(errno)));
      if (::fstat(positionFd, &info) != 0)
        return positionFailure(QString::fromLocal8Bit(strerror(errno)));
      if (!S_ISREG(info.st_mode))
        return positionFailure("metadata is not a regular file");
      // An existing group-writable checkpoint may belong to another writer.
      // Avoid requiring ownership merely to reapply unchanged permissions.
      if (((info.st_mode & 0777) != permissions && ::fchmod(positionFd, permissions) != 0) ||
          ::ftruncate(positionFd, 2 * PositionSlotSize) != 0)
        return positionFailure(QString::fromLocal8Bit(strerror(errno)));
    }
#endif
    QByteArray slot(PositionSlotSize, 0);
    std::memcpy(slot.data(), "ALHPOS01", 8);
    qToBigEndian<quint64>(sequence, slot.data() + 8);
    qToBigEndian<quint64>(records.size(), slot.data() + 16);
    qToBigEndian<quint64>(nextRecord, slot.data() + 24);
    std::memcpy(slot.data() + 32, digest.constData(), 32);
    const auto checksum = QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256);
    std::memcpy(slot.data() + 64, checksum.constData(), 32);
    // The alarm record is already flushed. Keep one previous checkpoint intact
    // and reject torn/stale slots on recovery. Match the log's flush-only
    // durability: pwrite reaches the kernel without adding an fsync per event.
    const qint64 offset = nextPositionSlot * PositionSlotSize;
#ifdef Q_OS_WIN
    if (!positionFile.seek(offset) || positionFile.write(slot) != slot.size() ||
        !positionFile.flush())
      return positionFailure(positionFile.errorString());
#else
    qint64 written = 0;
    while (written < slot.size()) {
      const auto count = ::pwrite(positionFd, slot.constData() + written,
                                  slot.size() - written, offset + written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        return positionFailure(count == 0 ? QString("short checkpoint write")
                                         : QString::fromLocal8Bit(strerror(errno)));
      written += count;
    }
#endif
    nextPositionSlot = 1 - nextPositionSlot;
    return true;
  }

  void reportAsyncError() {
    const auto reason = positionError.isEmpty() ? "Log write failed: " + file.errorString() : positionError;
    if (asyncError) asyncError(reason);
    else qWarning("%s", qPrintable(reason));
  }
  void schedule() {
    if (scheduled) return;
    scheduled = true;
    QTimer::singleShot(recoveryPollMs, QCoreApplication::instance(), [self = shared_from_this()] {
      self->scheduled = false;
      self->drain(false);
      if (self->pending.isOpen()) self->schedule();
    });
  }
  bool enqueue(const QByteArray& bytes, int maximum) {
    if (!pending.isOpen() && (!pending.open() || pending.fileName().isEmpty())) {
      positionError = "Cannot spool alarms during log recovery: " + pending.errorString();
      return false;
    }
    QByteArray header(8, 0);
    qToBigEndian<quint32>(maximum, header.data());
    qToBigEndian<quint32>(bytes.size(), header.data() + 4);
    const auto end = pendingEnd;
    if (pending.size() != end) {
      positionError = "Cannot append to incomplete alarm spool " + pending.fileName();
      return false;
    }
    if (!pending.seek(end) || pending.write(header) != header.size() ||
        pending.write(bytes) != bytes.size() || !pending.flush()) {
      positionError = "Cannot spool alarms during log recovery: " + pending.errorString();
      pending.resize(end); // Discard a torn FIFO entry without damaging earlier records.
      return false;
    }
    pendingEnd += header.size() + bytes.size();
    schedule();
    return true;
  }
  void drain(bool wait) {
    QElapsedTimer budget; budget.start();
    auto readFailure = [&](const QString& reason) {
      recoveryPollMs = 1000;
      positionError = QString("Cannot read alarm spool %1 at byte %2 for log %3: %4")
          .arg(pending.fileName()).arg(pendingOffset).arg(file.fileName(), reason);
      if (spoolReadError != positionError) {
        spoolReadError = positionError;
        reportAsyncError();
      }
    };
    // Only the successfully flushed FIFO length proves exhaustion. A failed
    // size query or an externally truncated spool must not look like EOF.
    while (pending.isOpen() && pendingOffset < pendingEnd) {
      if (pendingEnd - pendingOffset < 8) { readFailure("Incomplete committed header"); return; }
      if (!pending.seek(pendingOffset)) { readFailure(pending.errorString()); return; }
      const auto header = pending.read(8);
      if (header.size() != 8) { readFailure("Incomplete header: " + pending.errorString()); return; }
      const auto maximum = qFromBigEndian<quint32>(header.constData());
      const auto length = qFromBigEndian<quint32>(header.constData() + 4);
      if (maximum > quint32(std::numeric_limits<int>::max()) ||
          length > quint64(pendingEnd - pendingOffset - 8)) {
        readFailure("Invalid record length or retention limit"); return;
      }
      const auto bytes = pending.read(length);
      if (quint32(bytes.size()) != length) { readFailure("Incomplete record: " + pending.errorString()); return; }
      spoolReadError.clear();
      const int result = writeNow(bytes, int(maximum), wait);
      if (result == RetryRecovery) {
        recoveryPollMs = 1000;
        reportAsyncError();
        return; // Recovery has not written this entry: retain it for a fresh scan.
      }
      if (result < 0) return; // The FIFO entry remains until recovery finishes.
      recoveryPollMs = 10;
      pendingOffset += 8 + length;
      if (!result) reportAsyncError();
      if (!wait && budget.elapsed() >= 5) return;
    }
    if (pending.isOpen()) {
      pending.close(); pending.remove(); pending.setAutoRemove(true); pendingOffset = pendingEnd = 0;
    }
  }
  void finishPending() {
    drain(true);
    // A failed background snapshot may already be stale. Retry synchronously
    // once at shutdown, but never hang shutdown on a persistently unreadable log.
    if (pending.isOpen()) drain(true);
    if (pending.isOpen() && pending.autoRemove()) {
      pending.setAutoRemove(false);
      positionError = QString("Unwritten alarms retained in spool %1 at byte %2 for log %3")
          .arg(pending.fileName()).arg(pendingOffset).arg(file.fileName());
      reportAsyncError();
    }
  }
  bool write(const QByteArray& bytes, int maximum) {
    if (pending.isOpen()) return enqueue(bytes, maximum);
    const int result = writeNow(bytes, maximum, false);
    if (result == RetryRecovery) {
      recoveryPollMs = 1000;
      reportAsyncError();
    }
    return result < 0 ? enqueue(bytes, maximum) : result != 0;
  }
  bool setAppendMode(bool enabled) {
#ifndef Q_OS_WIN
    if (appendMode == enabled) return true;
    if (!file.flush()) return false;
    const auto flags = ::fcntl(file.handle(), F_GETFL);
    if (flags < 0 || ::fcntl(file.handle(), F_SETFL, enabled ? flags | O_APPEND : flags & ~O_APPEND) < 0) {
      positionError = "Cannot change alarm log append mode: " + QString::fromLocal8Bit(std::strerror(errno));
      return false;
    }
    appendMode = enabled;
#else
    Q_UNUSED(enabled);
#endif
    return true;
  }
  bool appendRecord(const QByteArray& bytes) {
    if (!file.flush() || !setAppendMode(true)) return false;
    // EOF selection and the whole record write must be one native append:
    // seek(size) followed by write lets independent ALH/qtalh writers overwrite
    // each other. Use the existing handle so renaming the log cannot redirect it.
#ifdef Q_OS_WIN
    if (quint64(bytes.size()) > std::numeric_limits<DWORD>::max()) {
      positionError = "Alarm log record is too large to append";
      return false;
    }
    OVERLAPPED offset{};
    offset.Offset = offset.OffsetHigh = 0xFFFFFFFF;
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), bytes.constData(),
                   DWORD(bytes.size()), &written, &offset)) {
      positionError = QString("Cannot append alarm log: Windows error %1").arg(GetLastError());
      return false;
    }
#else
    ssize_t written;
    do { written = ::write(file.handle(), bytes.constData(), bytes.size()); }
    while (written < 0 && errno == EINTR);
    if (written < 0) {
      positionError = "Cannot append alarm log: " + QString::fromLocal8Bit(std::strerror(errno));
      return false;
    }
#endif
    if (quint64(written) != quint64(bytes.size())) {
      positionError = "Incomplete alarm log append";
      return false;
    }
    return true;
  }
  // RetryRecovery is a pre-write failure that can safely be retried; -1 waits
  // for a scan, 0 is a write/checkpoint failure (possibly committed), 1 is success.
  int writeNow(const QByteArray& bytes, int maximum, bool wait) {
    positionError.clear();
    if (recovery.valid()) {
      if (!wait && recovery.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return -1;
      auto result = recovery.get();
      if (!result.error.isEmpty()) { positionError = result.error; return RetryRecovery; }
      records = std::move(result.records);
      positionSequence = result.sequence; nextPositionSlot = result.nextSlot;
      limit = recoveringMaximum;
      nextRecord = int(records.size()) == limit ? 0 : int(records.size());
      rebuildFingerprint(); recoveryRewrite = true;
    }
    // A shared file can switch between unlimited and bounded windows. Disable
    // native append before any ring seek/rewrite, including recovered records.
    if ((maximum || recoveryRewrite) && !setAppendMode(false)) return 0;
    if (!maximum) {
      // Finish a recovered ring before switching to unlimited append mode.
      // This also preserves order if windows sharing a file choose different limits.
      if (recoveryRewrite) {
        if (!file.seek(0)) return 0;
        for (const auto& record : records) if (file.write(record) != record.size()) return 0;
        if (!file.resize(file.pos()) || !file.flush()) return 0;
        recoveryRewrite = false;
      }
      limit = -1; records.clear();
      return appendRecord(bytes);
    }
    if (limit != maximum) {
      const auto position = logIdentity::readableCheckpointPath(file);
      const auto sequence = positionSequence;
      const auto slot = nextPositionSlot;
      // Duplicate the open handle before dispatching a worker. A pathname may
      // already name a replacement (or disappear) before the first alarm.
      // Duplicates share the file offset: while scanning, all new alarms go to
      // the FIFO; recovery always seeks/re-writes the ring before normal writes.
#ifdef Q_OS_WIN
      const int descriptor = _dup(file.handle());
      auto closeDescriptor = [](int* fd) { _close(*fd); delete fd; };
#else
      const int descriptor = ::fcntl(file.handle(), F_DUPFD_CLOEXEC, 0);
      auto closeDescriptor = [](int* fd) { ::close(*fd); delete fd; };
#endif
      if (descriptor < 0) {
        positionError = "Cannot duplicate alarm log for recovery: " + QString::fromLocal8Bit(strerror(errno));
        return RetryRecovery;
      }
      auto handle = std::shared_ptr<int>(new int(descriptor), closeDescriptor);
      auto scan = [=] {
        QFile input;
        if (!input.open(*handle, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
          logRecovery::Result result; result.error = "Cannot read open alarm log: " + input.errorString(); return result;
        }
        return logRecovery::read(input, position, maximum, sequence, slot);
      };
      recoveringMaximum = maximum;
      // Avoid worker/queue overhead for ordinary small rings, but never scan
      // an old large unlimited log from the Channel Access callback thread.
      if (!wait && file.size() > 1024 * 1024) {
        recovery = std::async(std::launch::async, scan);
        return -1;
      }
      auto result = scan();
      if (!result.error.isEmpty()) { positionError = result.error; return RetryRecovery; }
      records = std::move(result.records);
      positionSequence = result.sequence; nextPositionSlot = result.nextSlot;
      limit = maximum;
      nextRecord = int(records.size()) == maximum ? 0 : int(records.size());
      rebuildFingerprint(); recoveryRewrite = true;
    }
    bool rewrite = recoveryRewrite;
    recoveryRewrite = false;
    if (nextRecord < int(records.size())) {
      rewrite |= records[nextRecord].size() != bytes.size();
      includeRecord(nextRecord, records[nextRecord]);
      records[nextRecord] = bytes;
    } else
      records.push_back(bytes);
    includeRecord(nextRecord, bytes);

    bool ok = true;
    if (rewrite) {
      // Variable-length records (notably XML) must not overwrite part of the
      // following record. Fixed-length records retain the inexpensive ring write.
      ok = file.seek(0);
      for (const auto& record : records)
        if (ok)
          ok = file.write(record) == record.size();
      if (ok)
        ok = file.resize(file.pos());
      nextOffset = 0;
      for (int i = 0; i <= nextRecord; ++i)
        nextOffset += records[i].size();
    } else {
      ok = file.seek(nextOffset) && file.write(bytes) == bytes.size();
      nextOffset = file.pos();
    }
    ok = ok && file.flush();
    if (!ok)
      limit = -1; // Re-read the actual file before retrying after a write error.
    if (++nextRecord == maximum) {
      nextRecord = 0;
      nextOffset = 0;
    }
    // Bind the cursor to the actual contents so stale metadata is ignored after
    // a legacy writer, truncation, or an interrupted write changes the log.
    return ok && savePosition();
  }
};
namespace {
QHash<QByteArray, std::weak_ptr<AlarmLogFile>> alarmFiles;
std::shared_ptr<AlarmLogFile> acquireAlarmFile(const QString& name, bool truncate) {
  QFileInfo info(name);
  auto path = info.canonicalFilePath();
  if (path.isEmpty())
    path = info.absoluteFilePath();
  for (auto it = alarmFiles.begin(); it != alarmFiles.end();) {
    if (it.value().expired())
      it = alarmFiles.erase(it);
    else
      ++it;
  }
  auto shared = std::make_shared<AlarmLogFile>();
  shared->file.setFileName(path);
  // Open the selected path before looking up a writer: the previous file may
  // have been renamed/replaced. Identity keys also keep that older live writer
  // available through its other aliases without confusing it with the new file.
  // Open without truncation so sharing a live file cannot erase its records.
  if (shared->file.open(QIODevice::ReadWrite)) {
    const auto identity = logIdentity::identity(shared->file);
    if (!identity.isEmpty())
      if (auto existing = alarmFiles.value(identity).lock())
        if (existing->file.isOpen()) return existing;
    // Hard links have different canonical paths but one file identity. Resolve
    // and remember the sidecar association before any write, including reopen
    // through a different alias after the previous writer has exited.
    logIdentity::remember(shared->file, logIdentity::location(shared->file), shared);
    shared->checkpointLocation = logIdentity::checkpointPath(shared->file);
    if (truncate && !shared->file.resize(0))
      shared->file.close();
    if (shared->file.isOpen() && !identity.isEmpty())
      alarmFiles.insert(identity, shared);
  }
  std::weak_ptr<AlarmLogFile> weak = shared;
  QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                   QCoreApplication::instance(), [weak] {
    if (auto file = weak.lock()) file->finishPending();
  });
  return shared;
}

struct SharedLock {
#ifdef Q_OS_WIN
  DWORD device;
  quint64 inode;
  bool locked = false;
#else
  dev_t device;
  ino_t inode;
#endif
  int users = 1;
  const void* broadcaster = nullptr;
};
QHash<int, SharedLock> sharedLocks;
int acquireLock(const QString& name, QString* failure = nullptr) {
#ifdef Q_OS_WIN
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (failure) *failure = QString("Windows error %1").arg(GetLastError());
    return -1;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) {
    if (failure) *failure = QString("Windows error %1").arg(GetLastError());
    CloseHandle(handle);
    return -1;
  }
  const quint64 inode = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  for (auto it = sharedLocks.begin(); it != sharedLocks.end(); ++it)
    if (it->device == info.dwVolumeSerialNumber && it->inode == inode) {
      CloseHandle(handle);
      ++it->users;
      return it.key();
    }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
  if (fd < 0) {
    if (failure) *failure = QString::fromLocal8Bit(strerror(errno));
    CloseHandle(handle);
    return -1;
  }
  sharedLocks.insert(fd, {info.dwVolumeSerialNumber, inode});
#else
  auto path = QFile::encodeName(name);
  struct stat info{};
  // Closing ANY descriptor for an inode releases this process's POSIX locks.
  // Resolve aliases before opening, and retain the descriptor as the handle so
  // a later rename or symlink change cannot redirect release/ownership checks.
  if (::stat(path.constData(), &info) == 0)
    for (auto it = sharedLocks.begin(); it != sharedLocks.end(); ++it)
      if (it->device == info.st_dev && it->inode == info.st_ino) {
        ++it->users;
        return it.key();
      }
  int fd = ::open(path.constData(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    if (failure) *failure = QString::fromLocal8Bit(strerror(errno));
    return -1;
  }
  if (::fstat(fd, &info) != 0) {
    if (failure) *failure = QString::fromLocal8Bit(strerror(errno));
    ::close(fd);
    return -1;
  }
  sharedLocks.insert(fd, {info.st_dev, info.st_ino});
#endif
  return fd;
}
bool setLock(int fd, bool acquire) {
#ifdef Q_OS_WIN
  auto it = sharedLocks.find(fd);
  if (it == sharedLocks.end())
    return false;
  if (it->locked == acquire)
    return true;
  OVERLAPPED offset{};
  auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  const bool ok = acquire
      ? LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0, MAXDWORD, MAXDWORD, &offset) != 0
      : UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &offset) != 0;
  if (ok)
    it->locked = acquire;
  return ok;
#else
  return lockf(fd, acquire ? F_TLOCK : F_ULOCK, 0) == 0;
#endif
}
void releaseLock(int fd) {
  auto it = sharedLocks.find(fd);
  if (it == sharedLocks.end())
    return;
  if (!--it->users) {
#ifdef Q_OS_WIN
    _close(fd);
#else
    ::close(fd);
#endif
    sharedLocks.erase(it);
  }
}
// A closing window must not erase a message before other processes poll it.
// Keep its descriptor and exclusive ownership until the remaining delivery
// interval expires. If the entire application exits first, release the lock
// without erasing the payload, as with a sender process that terminated.
void retainBroadcast(int fd, const QString& path, int remaining) {
  auto delivery = new QTimer(QCoreApplication::instance());
  delivery->setSingleShot(true);
  delivery->setTimerType(Qt::PreciseTimer);
  auto& shared = sharedLocks[fd];
  ++shared.users;
  shared.broadcaster = delivery;
  QObject::connect(delivery, &QTimer::timeout, delivery, [fd, path, delivery] {
    auto it = sharedLocks.find(fd);
    if (it != sharedLocks.end() && it->broadcaster == delivery) {
      QFile file(path);
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.close();
    }
    delete delivery;
  });
  QObject::connect(delivery, &QObject::destroyed, [fd, delivery] {
    auto it = sharedLocks.find(fd);
    if (it != sharedLocks.end() && it->broadcaster == delivery) {
      it->broadcaster = nullptr;
      setLock(fd, false);
    }
    releaseLock(fd);
  });
  delivery->start(remaining);
}
QString padded(const QString& s, int n) {
  return s.leftJustified(n, ' ');
}
} // namespace
QString Logging::stamp(qint64 time) {
  const qint64 second = time / 1000 - (time % 1000 < 0 ? 1 : 0);
  if (cachedTimestamp.isEmpty() || second != cachedSecond) {
    cachedSecond = second;
    cachedTimestamp = QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(time),
                                           "dd-MMM-yyyy HH:mm:ss");
  }
  return cachedTimestamp;
}
Logging::Logging(Options o, QString f, QObject* p, LoggingClock c)
    : QObject(p), options(o), facility(f), facilityLabel(f), clock(std::move(c)) {
  if (options.lockFile.isEmpty())
    options.lockFile = options.config;
  if (options.lock && !options.editor) {
    lockFd = acquireLock(options.lockFile + ".LOCK");
    master = false;
  }
  if (options.broadcast && !options.editor)
    ensureBroadcastLock();
  openFiles();
  // The editor audits configuration operations, but must not truncate alarm
  // logs, compete for runtime locks, or receive facility broadcasts.
  if (options.editor) return;
  timer.setInterval(2000);
  connect(&timer, &QTimer::timeout, this, [this] { tick(); });
  timer.start();
  // Establish ownership immediately, but allow the caller to install message
  // and reload handlers before consuming a pending broadcast.
  tick(false);
  QTimer::singleShot(0, this, [this] { tick(); });
}
Logging::~Logging() {
  // Window members may already be torn down. Late flush failures still reach
  // stderr through reportAsyncError(), but must not call back into that window.
  error = {};
  for (const auto& pending : pendingAlarmFiles)
    if (auto file = pending.lock()) file->finishPending();
  if (alarmFile) alarmFile->finishPending();
  if (broadcastPending()) {
    const auto owner = sharedLocks.constFind(broadcastFd);
    if (owner != sharedLocks.cend() && owner->broadcaster == this)
      retainBroadcast(broadcastFd, options.config + ".MESS",
                      int(qBound<qint64>(1, broadcastUnlock - clock.monotonic(), 60000)));
    broadcastUnlock = 0;
  } else finishBroadcast();
  if (options.lock && !options.editor)
    releaseLock(lockFd);
  if (options.broadcast && !options.editor)
    releaseLock(broadcastFd);
}
QString Logging::dated(const QString& p) const {
  return p + (options.dated ? QDate::currentDate().toString(".yyyy-MM-dd") : QString());
}
QString Logging::alarmPath() const {
  return dated(options.alarmFile);
}
QString Logging::opmodPath() const {
  return dated(options.opmodFile);
}
void Logging::reportOpenError(bool alarm, const QString& failure) {
  auto& previous = alarm ? alarmOpenError : opmodOpenError;
  if (previous == failure) return;
  previous = failure;
  // Opening can happen in the constructor, before the window has installed its
  // callback or built its message area. Deliver once the event loop is ready.
  QTimer::singleShot(0, this, [this, alarm, failure] {
    if ((alarm ? alarmOpenError : opmodOpenError) != failure) return;
    if (error) error(failure);
    else qWarning("%s", qPrintable(failure));
  });
}
void Logging::openFiles() {
  const auto alarmDestination = alarmPath(), operationDestination = opmodPath();
  debugLog(options.debug, "logging", QString("alarm=%1 opmod=%2 enabled=%3")
      .arg(alarmDestination, operationDestination).arg(!options.noLog));
  if (options.noLog) {
    openedAlarmPath = alarmDestination;
    alarmFile.reset();
    opmodFile->close();
    opmodFile->setFileName(operationDestination);
    return;
  }
  // Retry failed destinations independently. A failed operation log must not
  // reopen/truncate a healthy alarm log or reset its circular write position.
  if (!options.editor &&
      (openedAlarmPath != alarmDestination || !alarmFile || !alarmFile->file.isOpen())) {
    openedAlarmPath = alarmDestination;
    alarmFile = acquireAlarmFile(openedAlarmPath, !options.dated && !options.lock);
    if (!alarmFile->file.isOpen())
      reportOpenError(true, "Cannot open alarm log " + openedAlarmPath + ": " + alarmFile->file.errorString());
    else alarmOpenError.clear();
  }
  if (opmodFile->fileName() != operationDestination || !opmodFile->isOpen()) {
    opmodFile->close();
    opmodFile->setFileName(operationDestination);
    if (!opmodFile->open(QIODevice::WriteOnly | QIODevice::Append))
      reportOpenError(false, "Cannot open operation log " + operationDestination + ": " + opmodFile->errorString());
    else opmodOpenError.clear();
  }
}
void Logging::setAlarmFile(const QString& p) {
  debugLog(options.debug, "logging", "select alarm file " + p);
  const auto path = dated(p);
  std::shared_ptr<AlarmLogFile> next;
  if (!options.noLog && !options.editor) {
    next = acquireAlarmFile(path, !options.dated && !options.lock);
    if (!next->file.isOpen()) {
      if (error)
        error(next->file.errorString());
      return;
    }
  }
  options.alarmFile = p;
  openedAlarmPath = path;
  alarmFile = std::move(next);
  alarmOpenError.clear();
  operation(nullptr, "Setup Alarm Log File : " + p);
}
void Logging::setOpmodFile(const QString& p) {
  debugLog(options.debug, "logging", "select operation file " + p);
  auto next = std::make_unique<QFile>(dated(p));
  if (!options.noLog && !next->open(QIODevice::WriteOnly | QIODevice::Append)) {
    if (error)
      error(next->errorString());
    return;
  }
  // Record a successful handoff in the old operation log, as ALH does, so an
  // operator following that log can find its successor. Failed opens stay silent.
  operation(nullptr, "Setup OpMod File : " + p);
  options.opmodFile = p;
  opmodFile = std::move(next);
  opmodOpenError.clear();
}
void Logging::alarm(Node* n, const State& s, qint64 time) {
  QString body;
  QString transient = s.mask[AckT] ? "noackT" : "ackT";
  if (options.xml) {
    auto tag = [](QString k, QString v) {
      return '<' + k + '>' + v.toHtmlEscaped() + "</" + k + '>';
    };
    body = tag("pv", n->name) + ' ';
    if (options.engine.description)
      body += tag("desc", s.description) + ' ';
    body += tag("value", s.value) + ' ' + tag("status", statusName(s.status)) + ' ' +
            tag("severity", severityName(s.severity));
    if (options.engine.global)
      body +=
          ' ' + tag("status-noack", severityName(s.unack)) + ' ' + tag("severity-noack", transient);
  } else {
    body = padded(n->name, 28) + ' ';
    if (options.engine.description)
      body += padded(s.description, 28) + ' ' + padded(s.value.left(40), 40) + ' ';
    body += padded(statusName(s.status), 12) + ' ' + padded(severityName(s.severity), 16);
    if (options.engine.global)
      body += ' ' + padded(severityName(s.unack), 12) + ' ' + padded(transient, 5);
    if (!options.engine.description)
      body += ' ' + padded(s.value.left(40), 40);
  }
  record(true, body, time, 1);
}
void Logging::acknowledgement(Node* n) {
  // Legacy alLog2DBAckChan uses code 6 for each channel, including group acks.
  if (options.databaseKey)
    record(false, "Ack Channel--- " + padded(n->name, 28), clock.wall(), 6);
}
void Logging::operation(Node* n, const QString& s, OperationKind kind) {
  int code = 0;
  switch (kind) {
  case OperationKind::AckChannel: code = 5; break;
  case OperationKind::AckGroup: code = 6; break;
  case OperationKind::ChangeMask: code = 7; break;
  case OperationKind::ChangeGroupMask: code = 8; break;
  case OperationKind::ForceMask: code = 9; break;
  case OperationKind::ForceGroupMask: code = 10; break;
  case OperationKind::MaskChannel:
    // ALH emits this additional per-channel audit only with database logging.
    if (options.databaseKey && n)
      record(false, "Group Mask ID --- " + padded(n->name, 28), clock.wall(), 8);
    return;
  case OperationKind::Other: break;
  }
  record(false, facilityLabel + ": " + (n ? n->name : QString()) + ":  " + s,
         clock.wall(), code);
}
void Logging::record(bool alarm, const QString& rawBody, qint64 time, int code) {
  // TODO: Possible inherited ALH bug, deferred for a later change: operation
  // labels, names and messages are plain text, but XML entries embed them without
  // escaping '&' or '<'. For example, "RF & vacuum" produces malformed XML.
  // Escape operation text separately from the alarm body's existing XML markup.
  const auto body = singleLine(rawBody, options.xml);
  if (alarm && (options.editor || (options.lock && !master) || !commandsAllowed()))
    return;
  if ((!options.editor && alarmPath() != openedAlarmPath) || opmodPath() != opmodFile->fileName() ||
      (!options.noLog && ((!options.editor && (!alarmFile || !alarmFile->file.isOpen())) || !opmodFile->isOpen())))
    openFiles();
  QString timestamp = stamp(time), line;
  // Legacy helpers receive the same timestamp representation as the file body.
  const auto queueTimestamp = options.xml
      ? "<date>" + timestamp.left(11) + "</date> <time>" + timestamp.mid(12) + "</time>"
      : timestamp;
  if (options.xml)
    line = "<entry>" + queueTimestamp + ' ' + body + "</entry>\n";
  else
    line = padded(timestamp, 20) + " : " + body + '\n';
  auto bytes = line.toLocal8Bit();
  if (!options.noLog) {
    auto destination = alarmFile;
    if (alarm && options.dated) {
      const auto eventPath = options.alarmFile + QDateTime::fromMSecsSinceEpoch(time).date().toString(".yyyy-MM-dd");
      if (eventPath != openedAlarmPath) {
        destination = acquireAlarmFile(eventPath, false);
        if (!destination->file.isOpen() && error)
          error("Cannot open dated alarm log: " + destination->file.errorString());
      }
    }
    auto& f = alarm ? destination->file : *opmodFile;
    if (f.isOpen()) {
      if (alarm) {
        const QPointer<Logging> owner(this);
        destination->asyncError = [owner](const QString& reason) {
          if (owner && owner->error) owner->error(reason);
          else qWarning("%s", qPrintable(reason));
        };
      }
      bool ok = alarm ? destination->write(bytes, options.maxRecords)
                      : f.write(bytes) == bytes.size() && f.flush();
      if (alarm && destination->pending.isOpen()) {
        pendingAlarmFiles.erase(std::remove_if(pendingAlarmFiles.begin(), pendingAlarmFiles.end(),
            [](const auto& file) { return file.expired(); }), pendingAlarmFiles.end());
        const bool known = std::any_of(pendingAlarmFiles.cbegin(), pendingAlarmFiles.cend(),
            [&](const auto& file) { return file.lock() == destination; });
        if (!known) pendingAlarmFiles.push_back(destination);
      }
      if (!ok && error)
        error(alarm && !destination->positionError.isEmpty()
                  ? destination->positionError
                  : "Log write failed: " + f.errorString());
    }
  }
  if (options.printerKey && alarm)
    deliverQueue(true, QString("1 %1 %2 %3").arg(code + 1).arg(queueTimestamp, body).toLocal8Bit());
  if (options.databaseKey && code) {
    QString msg = QString("%1 %2 %3  %4 %5 %6 %7 %8")
                      .arg(alarm ? 1 : 2)
                      .arg(code)
                      .arg(identityToken(facility, "unknown_facility"), operatorIdentity(),
                           identityToken(QHostInfo::localHostName(), "unknown_host"),
                           displayIdentity(), queueTimestamp, body);
    deliverQueue(false, msg.toLocal8Bit());
  }
}
void Logging::setFacility(const QString& identifier, const QString& label) {
  // ALH assigned its database applicationName only once. Keep the active root
  // identity current while preserving files, ownership and older loss batches.
  facility = identifier;
  facilityLabel = label;
}
QByteArray Logging::queueLossRecord(bool printer, const QString& identifier, const QueueLoss& loss) {
  auto token = [](QString text, int maximum) {
    text.replace(QRegularExpression("\\s+"), "_");
    if (text.toLocal8Bit().size() > maximum) {
      while (text.toLocal8Bit().size() >= maximum) text.chop(1);
      text += '~'; // Explicitly mark shortened diagnostic context.
    }
    return text.isEmpty() ? QString("-") : text;
  };
  const auto timestamp = stamp(clock.wall());
  const auto date = options.xml
      ? "<date>" + timestamp.left(11) + "</date> <time>" + timestamp.mid(12) + "</time>"
      : timestamp;
  // Code 4 is the legacy message-loss record (printer transport uses code+1).
  // Millisecond epochs keep the interval unambiguous and the summary short.
  // Only diagnostic header tokens are bounded here; ordinary records retain
  // their full identifiers. Even maximum-width counts/times fit the legacy
  // receive buffer, and sendQueueResult still validates the complete record.
  const auto body = QString("MQ lost %1 messages from_ms=%2 to_ms=%3")
      .arg(loss.count).arg(loss.first).arg(loss.last);
  const auto header = printer ? QString("1 5 ")
      : QString("1 4 %1  %2 %3 %4 ").arg(token(identifier, 32),
          token(operatorIdentity(), 12), token(identityToken(QHostInfo::localHostName(), "unknown_host"), 20),
          token(displayIdentity(), 8));
  // Legacy loss records terminate the printer diagnostic with a newline.
  return (header + date + ' ' + body + '\n').toLocal8Bit();
}
bool Logging::reportQueueLoss(bool printer) {
  auto& losses = printer ? printerLosses : databaseLosses;
  if (losses.isEmpty()) return true;
  auto first = losses.begin();
  const auto record = queueLossRecord(printer, first.key(), first.value());
  const auto result = sendQueueResult(printer ? options.printerKey : options.databaseKey, record);
  // A failed summary is not another lost alarm. Retain the entire interval and
  // erase it only after successful submission; do not recurse through record().
  if (result.status != QueueSendStatus::Sent) return false;
  losses.erase(first);
  return true;
}
void Logging::deliverQueue(bool printer, const QByteArray& record) {
  reportQueueLoss(printer);
  // A summary may need more room than a normal record. Its failure must not
  // block useful delivery or invent a Full result for an unattempted record.
  const auto result = sendQueueResult(printer ? options.printerKey : options.databaseKey, record);
  if (result.status == QueueSendStatus::Sent) return;
  auto& loss = (printer ? printerLosses : databaseLosses)[facility];
  const auto time = clock.wall();
  if (!loss.count) loss.first = time;
  loss.last = time;
  ++loss.count; // Count original records exactly once, unlike ALH's lostCount+1.
  if (error) error((printer ? "Printer queue: " : "Database queue: ") + result.error);
}
void Logging::tick(bool receiveBroadcast) {
  // One nonblocking attempt per destination every ordinary timer tick also
  // reports recovery when the IOC produces no further alarms.
  reportQueueLoss(true);
  reportQueueLoss(false);
  if (!options.noLog && (!alarmFile || !alarmFile->file.isOpen() || !opmodFile->isOpen()))
    openFiles();
  const auto now = clock.wall(), elapsed = clock.monotonic();
  if (options.lock && (!nextLockCheck || elapsed >= nextLockCheck)) {
    nextLockCheck = elapsed + 20000;
    // A missing directory or temporary open failure must not strand this
    // instance in Slave forever. Reuse the inode-aware descriptor registry:
    // opening/closing an alias directly can release another window's lock.
    // Stay Slave until both opening the file and acquiring its lock succeed.
    if (lockFd < 0)
      lockFd = acquireLock(options.lockFile + ".LOCK");
    if (lockFd >= 0) {
      bool wasMaster = master;
      master = setLock(lockFd, true);
      if (wasMaster != master)
        debugLog(options.debug, "logging", master ? "became master" : "became slave");
      if (alarmFile) {
        if (master && !alarmFile->master)
          alarmFile->limit = -1; // Re-read records written by the previous process.
        alarmFile->master = master;
      }
    } else if (error)
      error("Cannot open logging lock file");
  }
  if (suppressUntil && elapsed >= suppressUntil) {
    suppressUntil = 0;
    record(true, "Stop log finish", now, 3);
  }
  if (broadcastUnlock && elapsed >= broadcastUnlock)
    finishBroadcast();
  if (!options.broadcast) return;
  ensureBroadcastLock();
  if (!receiveBroadcast) return;
  QFile f(options.config + ".MESS");
  if (!f.open(QIODevice::ReadOnly))
    return;
  auto lines = QString::fromLocal8Bit(f.readAll()).split('\n');
  // Legacy senders publish in place. Do not consume an ID until the message,
  // date and sender lines have arrived, or a partial reload is lost forever.
  // The final sender line has no terminating newline in ALH's wire format.
  if (f.error() != QFileDevice::NoError || lines.size() < 4 ||
      !lines[2].startsWith("Date is ") || !lines[3].startsWith("FROM: ") ||
      lines[3].mid(6).trimmed().isEmpty() || lines[0].isEmpty() || lines[0] == lastBroadcast)
    return;
  lastBroadcast = lines[0];
  debugLog(options.debug, "broadcast", "received " + lastBroadcast);
  auto text = lines.mid(1).join('\n');
  if (message)
    message(text);
  // Possible inherited ALH protocol bug, deferred: ordinary Send Message text
  // uses the same body as control messages. A reserved RELOAD_FACILITY: or
  // stop-logging prefix therefore executes that action, even when the sender
  // requested only a message. Later reject/escape reserved prefixes for plain
  // messages while retaining interoperability with legacy control messages.
  if (lines[1].startsWith("RELOAD_FACILITY:")) {
    if (reload)
      reload();
    return;
  }
  auto match =
      QRegularExpression("^(\\d+) MIN  ALH  WILL  NOT  SAVE ALARM LOG!!!!").match(lines[1]);
  if (match.hasMatch()) {
    int minutes = match.captured(1).toInt();
    if (minutes > 0 && minutes <= 10) {
      record(true, QString("Stop log start  during %1 min").arg(minutes), now, 3);
      suppressUntil = elapsed + minutes * 60000;
    }
  }
}
bool Logging::ensureBroadcastLock() {
  if (broadcastFd >= 0) return true;
  const auto path = options.config + ".MESSLOCK";
  QString reason;
  broadcastFd = acquireLock(path, &reason);
  if (broadcastFd >= 0) {
    broadcastOpenError.clear();
    return true;
  }
  const auto failure = "Cannot open broadcast lock file " + path + ": " + reason;
  if (failure != broadcastOpenError) {
    broadcastOpenError = failure;
    // Startup runs before the window installs its error handler.
    QTimer::singleShot(0, this, [this, failure] {
      if (broadcastOpenError != failure) return;
      if (error) error(failure);
      else qWarning("%s", qPrintable(failure));
    });
  }
  return false;
}
void Logging::finishBroadcast() {
  if (!broadcastUnlock)
    return;
  broadcastUnlock = 0;
  auto it = sharedLocks.find(broadcastFd);
  if (it == sharedLocks.end() || it->broadcaster != this)
    return;
  QFile file(options.config + ".MESS");
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    file.close();
  it->broadcaster = nullptr;
  if (broadcastFd >= 0)
    setLock(broadcastFd, false);
}
bool Logging::sendBroadcast(const QString& text, int minutes, bool reloadFacility) {
  debugLog(options.debug, "broadcast", QString("send reload=%1 suppressMinutes=%2")
      .arg(reloadFacility).arg(minutes));
  if (!options.broadcast || minutes < 0 || minutes > 10)
    return false;
  if (!ensureBroadcastLock()) return false;
  auto& shared = sharedLocks[broadcastFd];
  // POSIX locks are process-wide: another window (or this same sender) can
  // acquire our lock again. Keep a local owner until the delivery window ends.
  if (shared.broadcaster || !setLock(broadcastFd, true)) {
    if (error)
      error("Message broadcast is busy");
    return false;
  }
  const auto time = clock.wall();
  QString body =
      reloadFacility ? "RELOAD_FACILITY: " + text
      : minutes ? QString("%1 MIN  ALH  WILL  NOT  SAVE ALARM LOG!!!! %2").arg(minutes).arg(text)
                : text;
  QFile f(options.config + ".MESS");
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    setLock(broadcastFd, false);
    return false;
  }
  // Legacy readers treat the first line as an opaque ID in a 30-byte buffer.
  static qint64 lastMessageTime = 0;
  lastMessageTime = qMax(time, lastMessageTime + 1);
  QString id = QString::number(lastMessageTime) + '-' + QString::number(QCoreApplication::applicationPid());
  QString data = id + '\n' + body + "\nDate is " +
                 QLocale::c().toString(QDateTime::currentDateTime(), "ddd MMM d HH:mm:ss yyyy") +
                 "\nFROM: User=" + qEnvironmentVariable("USER") +
                 " host=" + QHostInfo::localHostName() +
                 " display=" + qEnvironmentVariable("DISPLAY");
  auto bytes = data.toLocal8Bit();
  bool ok = f.write(bytes) == bytes.size() && f.flush();
  f.close();
  if (!ok) {
    setLock(broadcastFd, false);
    return false;
  }
  shared.broadcaster = this;
  broadcastUnlock = clock.monotonic() + 60000;
  tick();
  return ok;
}
} // namespace alh
