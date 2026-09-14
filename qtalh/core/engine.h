// Alarm state port from ALH alLib.c/acknowledge.c/force.c. See ../../LICENSE.
#pragma once
#include "model.h"
#include <QSet>
#include <chrono>
namespace alh {
struct Event {
  int status = 0, severity = 0, acks = 0, ackt = 1;
  QString value;
};
// Severity outputs retain the latest failed value until the PV becomes writable.
enum class WriteKind { Value, Acknowledge, AckTransient, Severity, Heartbeat, AckValue };
enum class OperationKind { Other, AckChannel, AckGroup, ChangeMask, ChangeGroupMask,
                           ForceMask, ForceGroupMask, MaskChannel };
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
// Per-runtime suppression. Never serialized into the ALH configuration or written to PVs.
struct Shelf {
  qint64 until = 0;
  QString reason, username;
};
// Operator-facing aggregates; the original State fields still drive automation.
struct Presentation {
  int severity = 0, unack = 0, beep = 0, shelved = 0;
  std::array<int, 5> counts{}, unackCounts{}, beepCounts{};
};
struct ShelfSnapshot {
  QString identity, path;
  Shelf shelf;
  int localUnack = 0;
  bool unique = true;
};
struct State {
  int severity = 4, status = 0, unack = 0, beep = 0;
  // Keep IOC acknowledgement history separate from operator mask/latch changes.
  int observedAcks = 0, observedAckT = -1, observedSeverity = 4;
  // Global IOC acknowledgements and locally acknowledged communication errors
  // are independent; unack is their combined operator-facing severity.
  int iocUnack = 0;
  bool communicationUnack = false;
  Mask mask;
  Shelf shelf;
  Presentation presentation;
  QString value, description;
  bool initialized = false, forceDisabled = false;
  // Cancel and filtered monitoring failures invalidate observation coverage.
  bool awaitingObservation = true;
  // Deadlines/edges use monotonic time; filterStarted is the event wall timestamp.
  qint64 noAckUntil = 0, filterUntil = 0, filterStarted = 0;
  // Intentional ALH compatibility: -999 is the initial/reinitialization sentinel.
  // A first value of -999 is therefore ignored, even when configured to force.
  double forceCurrent = -999;
  Event pending;
  QVector<qint64> edges;
  int edgeIndex = 0;
  std::array<int, 5> counts{}, unackCounts{}, maskCounts{}, beepCounts{};
  int filterCount = 0, filterSeconds = 0, beepThreshold = 1;
  // Highest configured threshold in this node and its descendants (row indicator).
  int highestBeepThreshold = 1;
};
struct ChannelUpdate {
  QString identity, path, pv, value;
  QStringList ancestors;
  int severity = 0, status = 0, unack = 0;
  bool initialized = false, suppressed = false;
  bool available = false, cancelled = false, disabled = false, noAck = false, shelved = false;
  // Global monitoring is unavailable; its local error latch is not IOC ACKS.
  bool communicationError = false;
  qint64 observedAt = 0;
};
enum class ObservationCause { Processed, Suppression, LocalAcknowledgement, RequestedAcknowledgement, ExternalAcknowledgement };
struct AlarmObservation {
  ChannelUpdate before, after;
  ObservationCause cause = ObservationCause::Processed;
};
using AlarmSubscription = std::shared_ptr<void>;
struct EngineOptions {
  bool global = false, passive = false, caputAckT = false, description = false, debug = false;
};
class Engine {
public:
  explicit Engine(Document& document, EngineOptions options = {}, PvService* pv = nullptr);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  // Wall time is for event timestamps and absolute shelf expirations.
  std::function<qint64()> now = [] { return QDateTime::currentMSecsSinceEpoch(); };
  // Interval timers must survive system clock corrections. Tests can inject
  // this clock independently of wall time; its origin is shared across engines.
  std::function<qint64()> monotonicNow = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  std::function<void(Node*, const State&, qint64)> alarmLog;
  std::function<void(Node*, const QString&, OperationKind)> operation;
  std::function<void(Node*)> acknowledgement;
  std::function<void(const QString&)> command;
  std::function<void(const QString&)> error;
  std::function<void()> changed;
  // Compatibility callback; independent observers use observe().
  std::function<void(const ChannelUpdate&)> channelUpdated;
  AlarmSubscription observe(std::function<void(const AlarmObservation&)>);
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
  Presentation presentation(Node*) const;
  static QString channelPath(const Node*);
  static QString nodeIdentity(const Node*);
  ChannelUpdate channelUpdate(Node*) const;
  void restoreLocalAcknowledgements(const QHash<QString, int>&);
  static constexpr int MaximumShelfMinutes = 365 * 24 * 60;
  // Group operations skip existing shelves; replacement is channel-only.
  static bool validShelfUsername(const QString&);
  int shelve(Node*, int minutes, const QString& reason, const QString& username, bool replace = false);
  void unshelve(Node*);
  QVector<ShelfSnapshot> shelves() const;
  void restoreShelves(const QVector<ShelfSnapshot>&);
  void setMask(Node*, Mask, bool automatic = false);
  void resetMask(Node*, bool automatic = false);
  void modifyMask(Node*, int bit, int choice);
  void noAck(Node*, bool enabled);
  void cancelNoAckTimer(Node*);
  void forceValue(Node*, double);
  void setForceDisabled(Node*, bool);
  void configureForce(Node*, const QVector<Directive>&, bool disabled);
  void setBeep(Node*, int);
  bool audible() const;
  void setSilenceCurrent(bool);
  void setSilenceForever(bool);
  void setSilenceUntil(qint64);
  bool silenceForever = false, silenceCurrent = false;
  qint64 silenceUntil = 0;

private:
  QHash<Node*, State> states;
  struct Observer { std::weak_ptr<void> lifetime; std::function<void(const AlarmObservation&)> callback; };
  QVector<Observer> observers;
  QSet<Node*> requestedAcknowledgements;
  ChannelUpdate snapshot(Node*, const State&) const;
  void publish(Node*, const State&, ObservationCause);
  qint64 nextStateDeadline = 0, nextShelfDeadline = 0;
  bool externalStateAccess = false;
  bool dispatchingDeadlines = false;
  void processDeadlines(qint64 wall, qint64 elapsed);
  void noteDeadline(qint64);
  void noteShelfDeadline(qint64);
  qint64 historySecond = 0;
  QString historyTimestamp;
  PvService* pv;
  qint64 heartbeatDue = 0, heartbeatInterval = 0;
  bool heartbeatUnavailable = false;
  struct PendingAckT {
    Node* source;
    Node* channel;
    bool mask;
    quint64 request;
  };
  // ACKT belongs to the IOC record, shared by its bare name and all fields.
  // CA does not expose the identity behind unrelated server-side record aliases.
  QHash<QString, PendingAckT> pendingForceAckT;
  quint64 nextAckTRequest = 0;
  struct ForceRuntime;
  // Editable directives do not own live CALC variables or subscription lifetimes.
  QHash<Node*, std::shared_ptr<ForceRuntime>> forces;
  bool applyMask(Node*, Mask, bool automatic, Node* forceSource);
  void discardForceWrites(Node*);
  void retryForceWrites();
  bool running = false;
  void process(Node*, Event, qint64, int monitorSeverity = -1);
  void addHistory(Node*, qint64);
  void propagate(Node*, const State& before, ObservationCause cause = ObservationCause::Suppression,
                 bool publishGroupSeverity = false);
  void updatePresentation(Node*, const State& before, bool notify = true);
  void rebuildPresentation();
  void clearShelf(Node*, const QString& action);
  void severityCommands(Node*, int, int);
  void rebuildBeep();
  void monitor(Node*);
  bool startForce(Node*);
  void stopForce(Node*);
  void evaluateForce(Node*);
  void reapplyForce(Node*);
  void logOperation(Node*, const QString&, OperationKind = OperationKind::Other);
  void eachChannel(Node*, const std::function<void(Node*)>&);
};
} // namespace alh
