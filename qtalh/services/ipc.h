// Bounded compatibility codec for legacy ALH's native-long System V records.
#pragma once
#include <QByteArray>
#include <QString>
namespace alh {
QByteArray encodeQueue(const QByteArray&);
QByteArray decodeQueue(const QByteArray&, int payloadSize);
bool sendQueue(int key, const QByteArray&, QString* error = nullptr);
QByteArray receiveQueue(int id, QString* error = nullptr);
QByteArray printerRecord(const QByteArray&, const QString& color);
} // namespace alh
