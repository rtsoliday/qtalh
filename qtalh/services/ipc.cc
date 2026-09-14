#include "ipc.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#ifndef Q_OS_WIN
#include <sys/msg.h>
#endif
namespace alh {
QByteArray encodeQueue(const QByteArray& record) {
  // Legacy ALH passes text as msgp: its first sizeof(long) bytes ARE mtype.
  // msgsz is strlen(text), so a zero-padded tail avoids the legacy overread.
  if (record.size() < int(sizeof(long)) || record.size() > 250 - int(sizeof(long)))
    throw std::runtime_error("ALH queue record must fit the legacy 250-byte receive buffer");
  QByteArray bytes = record;
  bytes.append(QByteArray(sizeof(long), 0));
  long type = 0;
  std::memcpy(&type, bytes.constData(), sizeof(type));
  if (type <= 0)
    throw std::runtime_error("Legacy ALH queue type is not positive");
  return bytes;
}
QByteArray decodeQueue(const QByteArray& bytes, int payloadSize) {
  if (payloadSize < 0 || bytes.size() < int(sizeof(long)) + payloadSize)
    return {};
  QByteArray result = bytes.left(sizeof(long) + payloadSize);
  int end = result.indexOf('\0');
  if (end >= 0)
    result.truncate(end);
  return result;
}
QueueSendResult sendQueueResult(int key, const QByteArray& record) {
#ifdef Q_OS_WIN
  Q_UNUSED(key);
  Q_UNUSED(record);
  return {QueueSendStatus::Failed, "System V printer/database queues are unavailable on Windows"};
#else
  if (record.size() > 250 - int(sizeof(long)))
    return {QueueSendStatus::Oversized, "ALH queue record must fit the legacy 250-byte receive buffer"};
  try {
    auto bytes = encodeQueue(record);
    int id = msgget(key, 0600 | IPC_CREAT);
    if (id < 0 || msgsnd(id, bytes.constData(), record.size(), IPC_NOWAIT) < 0) {
      const int failure = errno;
      return {failure == EAGAIN ? QueueSendStatus::Full : QueueSendStatus::Failed,
              QString::fromLocal8Bit(strerror(failure))};
    }
    return {};
  } catch (const std::exception& e) {
    return {QueueSendStatus::Failed, QString::fromLocal8Bit(e.what())};
  }
#endif
}
bool sendQueue(int key, const QByteArray& record, QString* error) {
  const auto result = sendQueueResult(key, record);
  if (error) *error = result.error;
  return result.status == QueueSendStatus::Sent;
}
QByteArray receiveQueue(int id, QString* error) {
#ifdef Q_OS_WIN
  Q_UNUSED(id);
  if (error)
    *error = "System V printer/database queues are unavailable on Windows";
  return {};
#else
  QByteArray bytes(8193 + sizeof(long), 0);
  int n = msgrcv(id, bytes.data(), 8193, 0, IPC_NOWAIT | MSG_NOERROR);
  if (n < 0) {
    if (errno != ENOMSG && errno != EINTR && error)
      *error = strerror(errno);
    return {};
  }
  if (n > 8192) {
    if (error)
      *error = "Oversized ALH queue record discarded";
    return {};
  }
  return decodeQueue(bytes, n);
#endif
}
QByteArray printerRecord(const QByteArray& record, const QString& color) {
  // printer.c receives "1 <type> <timestamp> ..." and skips the first two bytes.
  // Possible inherited ALH protocol bug, deferred: Logging::alarm sends code 1
  // as printer type 2, but this decoder recognizes only type 1 as a normal
  // alarm. Normal records therefore get MAJOR styling and no appended newline.
  // Reconcile record types, field offsets and line termination in a future
  // change, including mixed legacy/Qt senders and helpers. Preserve bytes now.
  QByteArray msg = record.mid(2);
  int type = msg.left(msg.indexOf(' ')).toInt();
  if (type == 0)
    throw std::runtime_error("Invalid printer record type");
  int sev = 2;
  if (type == 1) {
    if (msg.size() < 112)
      throw std::runtime_error("Truncated legacy printer record");
    if (msg.mid(52).startsWith("NO_ALARM"))
      sev = 0;
    else if (msg.mid(52).startsWith("MINOR"))
      sev = 1;
    else if (msg.mid(52).startsWith("INVALID"))
      sev = 3;
  }
  QByteArray start, end;
  if (color == "bw_bold") {
    if (sev >= 2)
      start = "\033[1m";
    end = "\033[0m";
  } else if (color == "oki_bold") {
    if (sev >= 2)
      start = "\033H";
    end = "\033I";
  } else if (color == "hp_color") {
    if (sev > 0)
      start = sev == 1 ? "\033&v3S" : "\033&v1S";
    end = "\033E";
  } else if (color != "bw")
    throw std::runtime_error("Unknown printer color model");
  if (type > 1)
    return start + msg.mid(2) + end;
  int blank = msg.indexOf(' ', 23);
  if (blank < 0)
    throw std::runtime_error("Missing printer PV delimiter");
  return start + msg.mid(2, 20) + msg.mid(22, blank - 22) + msg.mid(51, 6) + msg.mid(64, 6) +
         msg.mid(111) + end + '\n';
}
} // namespace alh
