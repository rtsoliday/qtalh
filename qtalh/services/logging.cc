#include "core/diagnostics.h"
// ALH logging and interoperable Linux file locking/broadcast services.
#include "logging.h"
#include "ipc.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHostInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QTextStream>
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
#endif
namespace alh {
// All windows writing the same file share both the descriptor and ring position.
// CA and logging callbacks run on the GUI thread, so writes are serialized.
struct AlarmLogFile {
  QFile file;
  std::deque<QByteArray> records;
  int limit = -1, nextRecord = 0;
  qint64 nextOffset = 0;
  bool master = false;
  QString positionError;
  QByteArray digest = QByteArray(32, 0);
#ifdef Q_OS_WIN
  QFile positionFile;
#else
  int positionFd = -1;
#endif
  quint64 positionSequence = 0;
  // Two 96-byte slots: magic/version, sequence, count, next, ring fingerprint,
  // then SHA-256 of the preceding 64 bytes. Integers are big endian.
  static constexpr int PositionSlotSize = 96;
  ~AlarmLogFile() {
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
    return file.fileName() + ".qtalh-position";
  }
  int readPosition() {
    QFile position(positionPath());
    int next = -1;
    quint64 best = 0;
    if (position.open(QIODevice::ReadOnly) && position.size() >= PositionSlotSize &&
        position.size() <= 2 * PositionSlotSize) {
      const auto bytes = position.readAll();
      for (int i = 0; i < 2; ++i) {
        const auto slot = bytes.mid(i * PositionSlotSize, PositionSlotSize);
        if (slot.size() != PositionSlotSize || slot.left(8) != "ALHPOS01" ||
            QCryptographicHash::hash(slot.left(64), QCryptographicHash::Sha256) != slot.mid(64))
          continue;
        const auto sequence = qFromBigEndian<quint64>(slot.constData() + 8);
        const auto count = qFromBigEndian<quint64>(slot.constData() + 16);
        const auto cursor = qFromBigEndian<quint64>(slot.constData() + 24);
        if (!sequence || cursor > count)
          continue;
        // Advance beyond even stale slots, so they cannot later outrank a new
        // checkpoint if identical ring contents recur.
        positionSequence = qMax(positionSequence, sequence);
        if (sequence > best && count == records.size() &&
            slot.mid(32, 32) == digest) {
          best = sequence;
          next = int(cursor);
        }
      }
    }
    if (next >= 0)
      return next;
    // ALH logs or interrupted checkpoints may have no valid cursor. Recover the oldest
    // timestamp where possible; equal timestamps cannot reveal their ring order.
    int oldest = 0;
    QDateTime earliest;
    for (int i = 0; i < int(records.size()); ++i) {
      auto text = QString::fromLocal8Bit(records[i]);
      QString stamp = text.left(20);
      if (text.startsWith("<entry>")) {
        auto match =
            QRegularExpression("^<entry><date>([^<]+)</date> <time>([^<]+)</time>").match(text);
        stamp = match.captured(1) + ' ' + match.captured(2);
      }
      auto time = QLocale::c().toDateTime(stamp, "dd-MMM-yyyy HH:mm:ss");
      if (time.isValid() && (!earliest.isValid() || time < earliest)) {
        earliest = time;
        oldest = i;
      }
    }
    return oldest;
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
    const auto sequence = positionSequence + 1;
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
    const qint64 offset = (sequence % 2) * PositionSlotSize;
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
    positionSequence = sequence;
    return true;
  }

  bool write(const QByteArray& bytes, int maximum) {
    positionError.clear();
    if (!maximum) {
      limit = -1;
      records.clear();
      return file.seek(file.size()) && file.write(bytes) == bytes.size() && file.flush();
    }
    bool rewrite = false;
    if (limit != maximum) {
      records.clear();
      if (!file.seek(0))
        return false;
      while (!file.atEnd()) {
        auto line = file.readLine();
        if (file.error() != QFileDevice::NoError)
          return false;
        records.push_back(line);
      }
      rebuildFingerprint();
      // Normalize the previous ring to insertion order before trimming or
      // changing its capacity. The sidecar also handles identical timestamps.
      int next = readPosition();
      if (next > 0 && next < int(records.size())) {
        std::rotate(records.begin(), records.begin() + next, records.end());
        rewrite = true;
      }
      while (int(records.size()) > maximum) {
        records.pop_front();
        rewrite = true;
      }
      if (rewrite)
        rebuildFingerprint();
      limit = maximum;
      nextRecord = int(records.size()) == maximum ? 0 : int(records.size());
      nextOffset = nextRecord ? file.size() : 0;
    }
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
QHash<QString, std::weak_ptr<AlarmLogFile>> alarmFiles;
std::shared_ptr<AlarmLogFile> acquireAlarmFile(const QString& name, bool truncate) {
  QFileInfo info(name);
  auto path = info.canonicalFilePath();
  if (path.isEmpty())
    path = info.absoluteFilePath();
  if (auto shared = alarmFiles.value(path).lock())
    return shared;
  for (auto it = alarmFiles.begin(); it != alarmFiles.end();) {
    if (it.value().expired())
      it = alarmFiles.erase(it);
    else
      ++it;
  }
  auto shared = std::make_shared<AlarmLogFile>();
  shared->file.setFileName(path);
  if (shared->file.open(QIODevice::ReadWrite |
                        (truncate ? QIODevice::Truncate : QIODevice::NotOpen))) {
    // A new file had no canonical path before open. Register its resolved name
    // now so later opens through symlinks share the descriptor and ring cursor.
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty())
      path = canonical;
  }
  alarmFiles.insert(path, shared);
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
  const Logging* broadcaster = nullptr;
};
QHash<int, SharedLock> sharedLocks;
int acquireLock(const QString& name) {
#ifdef Q_OS_WIN
  HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return -1;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) {
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
  if (fd < 0)
    return -1;
  if (::fstat(fd, &info) != 0) {
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
Logging::Logging(Options o, QString f, QObject* p) : QObject(p), options(o), facility(f) {
  if (options.lockFile.isEmpty())
    options.lockFile = options.config;
  if (options.lock) {
    lockFd = acquireLock(options.lockFile + ".LOCK");
    master = false;
  }
  if (options.broadcast)
    broadcastFd = acquireLock(options.config + ".MESSLOCK");
  openFiles();
  timer.setInterval(2000);
  connect(&timer, &QTimer::timeout, this, [this] { tick(); });
  timer.start();
  tick();
}
Logging::~Logging() {
  finishBroadcast();
  if (options.lock)
    releaseLock(lockFd);
  if (options.broadcast)
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
void Logging::openFiles() {
  opmodFile->close();
  openedAlarmPath = alarmPath();
  opmodFile->setFileName(opmodPath());
  debugLog(options.debug, "logging", QString("alarm=%1 opmod=%2 enabled=%3")
      .arg(openedAlarmPath, opmodPath()).arg(!options.noLog));
  if (options.noLog) {
    alarmFile.reset();
    return;
  }
  alarmFile = acquireAlarmFile(openedAlarmPath, !options.dated && !options.lock);
  if (!alarmFile->file.isOpen()) {
    if (error)
      error(alarmFile->file.errorString());
    else
      qWarning("%s", qPrintable(alarmFile->file.errorString()));
  }
  if (!opmodFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
    if (error)
      error(opmodFile->errorString());
  }
}
void Logging::setAlarmFile(const QString& p) {
  debugLog(options.debug, "logging", "select alarm file " + p);
  const auto path = dated(p);
  std::shared_ptr<AlarmLogFile> next;
  if (!options.noLog) {
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
}
void Logging::setOpmodFile(const QString& p) {
  debugLog(options.debug, "logging", "select operation file " + p);
  auto next = std::make_unique<QFile>(dated(p));
  if (!options.noLog && !next->open(QIODevice::WriteOnly | QIODevice::Append)) {
    if (error)
      error(next->errorString());
    return;
  }
  options.opmodFile = p;
  opmodFile = std::move(next);
}
void Logging::alarm(Node* n, const State& s, qint64 time) {
  QString body;
  QString transient = s.mask[AckT] ? "NO" : "YES";
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
    record(false, "Ack Channel--- " + padded(n->name, 28), QDateTime::currentMSecsSinceEpoch(), 6);
}
void Logging::operation(Node* n, const QString& s) {
  // Free-text shelving reasons must not turn a local presentation action into
  // a legacy database acknowledgement or mask-change message.
  const bool shelving = s.startsWith("Shelve ") || s.startsWith("Change shelf ") ||
      s.startsWith("Unshelve ") || s.startsWith("Shelf expired") || s.startsWith("Drop shelf on reload ");
  record(false, facility + ": " + (n ? n->name : QString()) + ":  " + s,
         QDateTime::currentMSecsSinceEpoch(),
         (shelving || s.startsWith("Notification ")) ? 0
         : s.contains("Ack Group")     ? 6
         : s.contains("Ack Channel")   ? 5
         : s.startsWith("Change Mask") ? 7
                                       : 0);
}
void Logging::record(bool alarm, const QString& body, qint64 time, int code) {
  if (alarm && ((options.lock && !master) || QDateTime::currentMSecsSinceEpoch() < suppressUntil))
    return;
  if (alarmPath() != openedAlarmPath || opmodPath() != opmodFile->fileName())
    openFiles();
  QString timestamp = stamp(time), line;
  if (options.xml)
    line = "<entry><date>" + timestamp.left(11) + "</date> <time>" + timestamp.mid(12) +
           "</time> " + body + "</entry>\n";
  else
    line = padded(timestamp, 20) + " : " + body + '\n';
  auto bytes = line.toLocal8Bit();
  if (!options.noLog) {
    auto& f = alarm ? alarmFile->file : *opmodFile;
    if (f.isOpen()) {
      bool ok = alarm ? alarmFile->write(bytes, options.maxRecords)
                      : f.write(bytes) == bytes.size() && f.flush();
      if (!ok && error)
        error(alarm && !alarmFile->positionError.isEmpty()
                  ? alarmFile->positionError
                  : "Log write failed: " + f.errorString());
    }
  }
  QString problem;
  if (options.printerKey && alarm &&
      !sendQueue(options.printerKey,
                 QString("1 %1 %2 %3").arg(code + 1).arg(timestamp, body).toLocal8Bit(), &problem))
    if (error)
      error("Printer queue: " + problem);
  if (options.databaseKey && code) {
    QString msg = QString("%1 %2 %3  %4 %5 %6 %7 %8")
                      .arg(alarm ? 1 : 2)
                      .arg(code)
                      .arg(facility, qEnvironmentVariable("USER"), QHostInfo::localHostName(),
                           qEnvironmentVariable("DISPLAY"), timestamp, body);
    if (!sendQueue(options.databaseKey, msg.toLocal8Bit(), &problem) && error)
      error("Database queue: " + problem);
  }
}
void Logging::tick() {
  auto now = QDateTime::currentMSecsSinceEpoch();
  if (options.lock && now - lastLockCheck >= 20000) {
    lastLockCheck = now;
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
  if (suppressUntil && now >= suppressUntil) {
    suppressUntil = 0;
    record(true, "Stop log finish", now, 3);
  }
  if (broadcastUnlock && now >= broadcastUnlock)
    finishBroadcast();
  if (!options.broadcast)
    return;
  QFile f(options.config + ".MESS");
  if (!f.open(QIODevice::ReadOnly))
    return;
  auto lines = QString::fromLocal8Bit(f.readAll()).split('\n');
  if (lines.size() < 2 || lines[0].isEmpty() || lines[0] == lastBroadcast)
    return;
  lastBroadcast = lines[0];
  debugLog(options.debug, "broadcast", "received " + lastBroadcast);
  auto text = lines.mid(1).join('\n');
  if (message)
    message(text);
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
      suppressUntil = now + minutes * 60000;
    }
  }
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
  if (!options.broadcast || broadcastFd < 0 || minutes < 0 || minutes > 10)
    return false;
  auto& shared = sharedLocks[broadcastFd];
  // POSIX locks are process-wide: another window (or this same sender) can
  // acquire our lock again. Keep a local owner until the delivery window ends.
  if (shared.broadcaster || !setLock(broadcastFd, true)) {
    if (error)
      error("Message broadcast is busy");
    return false;
  }
  auto time = QDateTime::currentMSecsSinceEpoch();
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
  broadcastUnlock = time + 60000;
  tick();
  return ok;
}
} // namespace alh
