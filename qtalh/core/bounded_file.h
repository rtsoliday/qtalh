#pragma once
#include <QFile>
#include <QtGlobal>
#include <algorithm>

namespace alh {
constexpr qint64 MaximumConfigurationBytes = 32LL * 1024 * 1024;
constexpr qint64 MaximumBroadcastBytes = 64LL * 1024;

enum class BoundedReadResult { Complete, TooLarge, ReadError };

// Charge each file to the caller's remaining budget. Checking size first
// avoids allocating a known oversized regular file; bounded reads also cover
// pipes and files that grow after the size check.
inline BoundedReadResult readBoundedFile(QFile& file, QByteArray& data, qint64& remaining) {
  data.clear();
  if (file.size() > remaining)
    return BoundedReadResult::TooLarge;
  for (;;) {
    const auto chunk = file.read(std::min(qint64(64 * 1024), remaining + 1));
    if (file.error() != QFileDevice::NoError)
      return BoundedReadResult::ReadError;
    if (chunk.isEmpty())
      return BoundedReadResult::Complete;
    if (chunk.size() > remaining)
      return BoundedReadResult::TooLarge;
    data += chunk;
    remaining -= chunk.size();
  }
}
} // namespace alh
