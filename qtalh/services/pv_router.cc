#include "pv_router.h"
namespace alh {
PvRouter::PvRouter(EngineOptions options, QObject* parent)
    : QObject(parent), ca(options), pva(options) {
  ca.error = pva.error = [this](const QString& s) {
    if (error) error(s);
  };
}
void PvRouter::monitor(const QString& n, std::function<void(Event)> callback, const void* owner) {
  route(n).monitor(n, std::move(callback), owner);
}
void PvRouter::text(const QString& n, std::function<void(QString)> callback) {
  route(n).text(n, std::move(callback));
}
void PvRouter::number(const QString& n, std::function<void(double)> callback, const void* owner) {
  route(n).number(n, std::move(callback), owner);
}
void PvRouter::cancelNumbers(const void* owner) {
  ca.cancelNumbers(owner);
  pva.cancelNumbers(owner);
}
void PvRouter::prepare(const QString& n) { route(n).prepare(n); }
bool PvRouter::canWrite(const QString& n) const {
  return isPvaName(n) ? pva.canWrite(n) : ca.canWrite(n);
}
bool PvRouter::put(const QString& n, double v, WriteKind kind) { return route(n).put(n, v, kind); }
void PvRouter::cancel(const QString& n, const void* owner) { route(n).cancel(n, owner); }
void PvRouter::clear() {
  ca.clear();
  pva.clear();
}
void PvRouter::readStrings(const QStringList& names,
                           std::function<void(QHash<QString, QString>)> done, int timeoutMs) {
  QStringList caNames, pvaNames;
  for (const auto& name : names)
    (isPvaName(name) ? pvaNames : caNames) << name;
  if (pvaNames.isEmpty())
    return ca.readStrings(caNames, std::move(done), timeoutMs);
  if (caNames.isEmpty())
    return pva.readStrings(pvaNames, std::move(done), timeoutMs);
  struct Merge {
    QHash<QString, QString> values;
    int pending = 2;
    std::function<void(QHash<QString, QString>)> done;
  };
  auto merge = std::make_shared<Merge>();
  merge->done = std::move(done);
  auto part = [merge](QHash<QString, QString> values) {
    for (auto it = values.cbegin(); it != values.cend(); ++it)
      merge->values.insert(it.key(), it.value());
    if (--merge->pending == 0 && merge->done)
      merge->done(merge->values);
  };
  ca.readStrings(caNames, part, timeoutMs);
  pva.readStrings(pvaNames, part, timeoutMs);
}
void PvRouter::configureInitialAckT(const Document& document) {
  ca.configureInitialAckT(document);
  pva.configureInitialAckT(document);
}
} // namespace alh
