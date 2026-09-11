// Alarm state port from ALH alLib.c/acknowledge.c/force.c. See ../../LICENSE.
#pragma once
#include "model.h"
#include <QSet>
namespace alh {
struct Event {
  int status = 0, severity = 0, acks = 0, ackt = 1;
  QString value;
};
// Severity outputs retain the latest failed value until the PV becomes writable.
enum class WriteKind { Value, Acknowledge, AckTransient, Severity };
struct PvService {
  virtual ~PvService() = default;
  virtual void monitor(const QString&, std::function<void(Event)> callback,
                       const void* owner = nullptr) = 0;
  // Read-only text values, independent of global alarm acknowledgement rights.
  virtual void text(const QString&, std::function<void(QString)> callback) = 0;
  virtual void number(const QString&, std::function<void(double)> callback,
                      const void* owner = nullptr) = 0;
  virtual void cancelNumbers(const void*) = 0;
  virtual void prepare(const QString&) = 0;
  virtual bool canWrite(const QString&) const = 0;
  virtual bool put(const QString&, double, WriteKind = WriteKind::Value) = 0;
  virtual void cancel(const QString&, const void* owner = nullptr) = 0;
  virtual void clear() = 0;
};
struct State {
  int severity = 4, status = 0, unack = 0, beep = 0;
  // Keep IOC acknowledgement history separate from operator mask/latch changes.
  int observedAcks = 0, observedAckT = -1;
  Mask mask;
  QString value, description;
  bool initialized = false, forceDisabled = false;
  qint64 noAckUntil = 0, filterUntil = 0, filterStarted = 0;
  double forceCurrent = -999;
  Event pending;
  QVector<qint64> edges;
  int edgeIndex = 0;
  std::array<int, 5> counts{}, unackCounts{}, maskCounts{}, beepCounts{};
  int filterCount = 0, filterSeconds = 0, beepThreshold = 1;
};
struct EngineOptions {
  bool global = false, passive = false, caputAckT = false, description = false, debug = false;
};
class Engine {
public:
  explicit Engine(Document& document, EngineOptions options = {}, PvService* pv = nullptr);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  std::function<qint64()> now = [] { return QDateTime::currentMSecsSinceEpoch(); };
  std::function<void(Node*, const State&, qint64)> alarmLog;
  std::function<void(Node*, const QString&)> operation;
  std::function<void(Node*)> acknowledgement;
  std::function<void(const QString&)> command;
  std::function<void(const QString&)> error;
  std::function<void()> changed;
  QVector<QString> history;
  EngineOptions options;
  const State& state(Node* n) const;
  State& mutableState(Node* n) {
    // A caller may retain this reference and change deadlines later.
    externalStateAccess = true;
    return states[n];
  }
  Document& document;
  void start();
  void stop();
  void event(Node*, Event);
  void tick();
  void tickHeartbeat();
  int heartbeatDelay() const;
  void acknowledge(Node*);
  void setMask(Node*, Mask, bool automatic = false);
  void resetMask(Node*);
  void noAck(Node*, bool enabled);
  void cancelNoAckTimer(Node*);
  void forceValue(Node*, double);
  void setForceDisabled(Node*, bool);
  void configureForce(Node*, const QVector<Directive>&, bool disabled);
  void setBeep(Node*, int);
  bool audible() const;
  bool silenceForever = false, silenceCurrent = false;
  qint64 silenceUntil = 0;

private:
  QHash<Node*, State> states;
  qint64 nextStateDeadline = 0;
  bool externalStateAccess = false;
  void noteDeadline(qint64);
  qint64 historySecond = 0;
  QString historyTimestamp;
  PvService* pv;
  qint64 heartbeatDue = 0, heartbeatInterval = 0;
  struct PendingAckT {
    Node* source;
    bool mask;
  };
  QHash<Node*, PendingAckT> pendingForceAckT;
  void applyMask(Node*, Mask, bool automatic, Node* forceSource);
  void discardForceWrites(Node*);
  void retryForceWrites();
  bool running = false;
  void process(Node*, Event, qint64);
  void propagate(Node*, const State& before);
  void severityCommands(Node*, int, int);
  void rebuildBeep();
  void monitor(Node*);
  void startForce(Node*);
  void logOperation(Node*, const QString&);
  void eachChannel(Node*, const std::function<void(Node*)>&);
};
} // namespace alh
