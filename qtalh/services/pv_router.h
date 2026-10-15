// Routes "pva://" names to pvAccess and all others to Channel Access. See ../../LICENSE.
#pragma once
#include "services/channel_access.h"
#include "services/pv_access.h"
namespace alh {
class PvRouter final : public QObject, public PvService {
public:
  explicit PvRouter(EngineOptions options = {}, QObject* parent = nullptr);
  std::function<void(const QString&)> error;
  ChannelAccess ca;
  PvAccess pva;
  void monitor(const QString&, std::function<void(Event)>, const void* owner = nullptr) override;
  void text(const QString&, std::function<void(QString)>) override;
  void number(const QString&, std::function<void(double)>, const void* owner = nullptr) override;
  void cancelNumbers(const void*) override;
  void prepare(const QString&) override;
  bool canWrite(const QString&) const override;
  bool put(const QString&, double, WriteKind = WriteKind::Value) override;
  void cancel(const QString&, const void* owner = nullptr) override;
  void clear() override;
  // Reads both protocols; done receives the merged result once.
  void readStrings(const QStringList& names, std::function<void(QHash<QString, QString>)> done,
                   int timeoutMs = 1500);
  void configureInitialAckT(const Document&);

private:
  PvService& route(const QString& name) {
    return isPvaName(name) ? static_cast<PvService&>(pva) : ca;
  }
};
} // namespace alh
