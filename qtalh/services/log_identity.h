#pragma once
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <sys/xattr.h>
#endif
#endif

namespace alh {
namespace logIdentity {
inline QByteArray identity(QFile& file) {
  if (!file.isOpen()) return {};
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), &info))
    return {};
  const auto inode = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  return QByteArray::number(info.dwVolumeSerialNumber) + ':' + QByteArray::number(inode);
#else
  struct stat info{};
  if (::fstat(file.handle(), &info) != 0) return {};
  return QByteArray::number(quint64(info.st_dev)) + ':' + QByteArray::number(quint64(info.st_ino));
#endif
}
inline QByteArray revision(QFile& file) {
#ifdef Q_OS_WIN
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())), &info)) return {};
  return QByteArray::number((quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow) + ':' +
      QByteArray::number((quint64(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
#else
  struct stat info{};
  if (::fstat(file.handle(), &info) != 0) return {};
#ifdef Q_OS_MACOS
  const auto modified = info.st_mtimespec;
#else
  const auto modified = info.st_mtim;
#endif
  return QByteArray::number(qint64(info.st_size)) + ':' + QByteArray::number(qint64(modified.tv_sec)) + ':' +
      QByteArray::number(qint64(modified.tv_nsec));
#endif
}
// The registry crosses the GUI writer/background reader boundary. Store only
// path hints, never QFile objects or live descriptors owned by another thread.
inline QMutex mutex;
inline QHash<QByteArray, QString> locations;
inline QHash<QByteArray, std::weak_ptr<void>> liveLocations;
inline constexpr char attribute[] = "user.qtalh.checkpoint";
inline QByteArray readLocation(QFile& file) {
  QByteArray bytes(8192, 0);
#ifdef Q_OS_WIN
  // NTFS alternate streams, like POSIX xattrs, belong to the file identity and
  // are visible through every hard link without changing the legacy log bytes.
  QFile stream(file.fileName() + ":qtalh-checkpoint");
  if (!stream.open(QIODevice::ReadOnly) || stream.size() > bytes.size()) return {};
  return stream.readAll();
#elif defined(Q_OS_MACOS)
  const auto count = ::fgetxattr(file.handle(), attribute, bytes.data(), bytes.size(), 0, 0);
#elif defined(Q_OS_LINUX)
  const auto count = ::fgetxattr(file.handle(), attribute, bytes.data(), bytes.size());
#else
  Q_UNUSED(file);
  return {};
#endif
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
  if (count < 0 || count >= bytes.size()) return {};
  bytes.resize(int(count));
  return bytes;
#endif
}
inline void writeLocation(QFile& file, const QString& path) {
  const auto bytes = path.toUtf8();
  if (bytes.size() >= 8192 || readLocation(file) == bytes) return;
#ifdef Q_OS_WIN
  QFile stream(file.fileName() + ":qtalh-checkpoint");
  if (stream.open(QIODevice::WriteOnly)) stream.write(bytes);
#elif defined(Q_OS_MACOS)
  ::fsetxattr(file.handle(), attribute, bytes.constData(), bytes.size(), 0, 0);
#elif defined(Q_OS_LINUX)
  ::fsetxattr(file.handle(), attribute, bytes.constData(), bytes.size(), 0);
#else
  Q_UNUSED(file);
#endif
  // Locator metadata is best effort: filesystems without xattrs/streams still
  // use ordinary sidecars and the in-process identity registry. A failure here
  // must never prevent alarm delivery or replace the checkpoint error policy.
}
inline QString location(QFile& file) {
  const auto key = identity(file);
  const auto stored = readLocation(file);
  const QByteArray prefix = "ALHLOC02\n" + key + '\n';
  // The locator belongs to the open inode, not its current pathname. Bind it
  // to that identity so copying xattrs to a replacement cannot borrow the old
  // file's checkpoint. Older plain-path locators still need path validation.
  if (!key.isEmpty() && stored.startsWith(prefix))
    return QString::fromUtf8(stored.mid(prefix.size()));
  QStringList candidates;
  if (!key.isEmpty()) {
    QMutexLocker guard(&mutex);
    const auto known = locations.value(key);
    if (!known.isEmpty()) {
      // Without xattrs, an active writer supplies a lifetime-checked hint.
      // Once it closes, validate the path again to avoid inode-reuse collisions.
      if (!liveLocations.value(key).expired()) return known;
      candidates << known;
    }
  }
  if (!stored.startsWith("ALHLOC02\n") && !stored.isEmpty())
    candidates << QString::fromUtf8(stored);
  for (const auto& path : candidates) {
    QFile original(path);
    if (!key.isEmpty() && original.open(QIODevice::ReadOnly) && identity(original) == key)
      return path;
  }
  const QFileInfo info(file);
  const auto canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
inline QString checkpointPath(QFile& file) {
  // A renamed file and its replacement may both still be written. Separate
  // their sidecars even though their locators have the same original basename.
  return location(file) + ".qtalh-position." + QString::fromLatin1(identity(file).toHex());
}
inline QString readableCheckpointPath(QFile& file) {
  const auto current = checkpointPath(file);
  if (QFileInfo::exists(current)) return current;
  // Upgrade existing logs on the next write. Recovery/readers validate the
  // legacy cursor against the log contents before accepting it.
  return location(file) + ".qtalh-position";
}
inline void remember(QFile& file, const QString& path, const std::shared_ptr<void>& lifetime) {
  const auto key = identity(file);
  if (key.isEmpty()) return;
  {
    QMutexLocker guard(&mutex);
    locations.insert(key, path);
    liveLocations.insert(key, lifetime);
  }
  // Persist once per writer, not per alarm. The identity-qualified locator
  // survives a rename and can also be used by readers in other processes.
  writeLocation(file, QString::fromUtf8("ALHLOC02\n" + key + '\n') + path);
}
} // namespace logIdentity
} // namespace alh
