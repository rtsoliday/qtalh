// Bounded compatibility codec for legacy ALH's native-long System V records.
#pragma once
#include <QByteArray>
#include <QString>
namespace alh {
QByteArray encodeQueue(const QByteArray&);
QByteArray decodeQueue(const QByteArray&, int payloadSize);
enum class QueueSendStatus { Sent, Full, Oversized, Failed };
struct QueueSendResult {
  QueueSendStatus status = QueueSendStatus::Sent;
  QString error;
};
QueueSendResult sendQueueResult(int key, const QByteArray&);
// Compatibility convenience API; logging uses the structured result below it.
bool sendQueue(int key, const QByteArray&, QString* error = nullptr);
QByteArray receiveQueue(int id, QString* error = nullptr);
QByteArray printerRecord(const QByteArray&, const QString& color);
} // namespace alh
