#include "core/diagnostics.h"
// Alarm transition behavior ported from ALH alLib.c. See ../../LICENSE.
#include "engine.h"
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <alarm.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <postfix.h>
namespace alh {
namespace {
QStringList fields(QString s) {
  return s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}
int highest(const std::array<int, 5>& a) {
  for (int i = 4; i > 0; --i)
    if (a[i] > 0)
      return i;
  return 0;
}
int effective(const State& s) {
  return s.mask[Disable] || s.mask[Cancel] ? 0 : s.severity;
}
int unacknowledged(const State& s) {
  return s.mask[Disable] || s.mask[Cancel] || s.mask[Ack] ? 0 : s.unack;
}
QString acknowledgementIdentity(const QString& name) {
  // EPICS resolves the record before the first dot. DBR_PUT_ACKT changes
  // that record's ACKT regardless of the addressed field (including .VAL).
  // Keep the original channel name for the actual write/access-rights check.
  return name.section('.', 0, 0);
}
QString identity(const Node* n) {
  QString result;
  for (; n; n = n->parent)
    result.prepend(QString(n->group ? "G%1:%2" : "C%1:%2").arg(n->name.size()).arg(n->name));
  return result;
}
int beep(const State& s) {
  int n = unacknowledged(s);
  return n >= s.beepThreshold ? n : 0;
}
} // namespace
Engine::Engine(Document& d, EngineOptions o, PvService* p) : options(o), document(d), pv(p) {
  for (auto n : d.nodes()) {
    State s;
    s.mask = n->mask;
    s.severity = n->group || s.mask[Cancel] || s.mask[Disable] ? 0 : 4;
    s.beepThreshold = severityValue(n->option("BEEPSEVR", "MINOR"));
    s.pending.severity = ErrorSeverity; // ALH count filter's initial input baseline.
    auto f = fields(n->option("ALARMCOUNTFILTER"));
    if (f.size() == 2) {
      s.filterCount = f[0].toInt();
      s.filterSeconds = f[1].toInt();
      if (s.filterCount > 0)
        s.edges.fill(0, 2 * s.filterCount);
    }
    states.insert(n, s);
  }
  for (auto n : d.channels()) {
    auto& s = states[n];
    for (auto p = n->parent; p; p = p->parent) {
      auto& g = states[p];
      g.counts[effective(s)]++;
      for (int i = 0; i < 5; ++i)
        if (s.mask[i])
          g.maskCounts[i]++;
      g.severity = highest(g.counts);
    }
  }
  rebuildBeep();
  rebuildPresentation();
}

Engine::~Engine() {
  stop();
}
const State& Engine::state(Node* n) const {
  auto i = states.constFind(n);
  if (i == states.cend())
    throw ParseError("Unknown alarm node");
  return i.value();
}
void Engine::eachChannel(Node* n, const std::function<void(Node*)>& fn) {
  if (!n)
    return;
  if (!n->group)
    fn(n);
  else {
    // ALH stores direct channels separately and processes them before subgroups.
    // Preserve that order even when declarations are interleaved in the file:
    // intermediate group severities can dispatch commands during mask changes.
    for (auto& c : n->children)
      if (!c->group)
        fn(c.get());
    for (auto& c : n->children)
      if (c->group)
        eachChannel(c.get(), fn);
  }
}
void Engine::monitor(Node* n) {
  if (!pv || n->group)
    return;
  if (states[n].mask[Cancel]) {
    // Cancel suppresses monitoring, but global ACKT still needs a connection.
    if (options.global && !options.passive) pv->prepare(n->name);
    return;
  }
  pv->monitor(n->name, [this, n](Event e) { event(n, e); }, n);
}
void Engine::start() {
  if (running)
    return;
  running = true;
  if (!pv)
    return;
  for (auto n : document.nodes()) {
    const auto severity = n->option("SEVRPV");
    if (!severity.isEmpty() && severity != "-") {
      pv->prepare(severity);
      // Disabled channels may never produce an alarm transition (or be cancelled).
      if (!n->group && states[n].mask[Disable] && options.global && !options.passive)
        pv->put(severity, -1, WriteKind::Severity);
    }
    monitor(n);
    const auto ack = fields(n->option("ACKPV"));
    if (!ack.isEmpty())
      pv->prepare(ack[0]);
    if (!n->group && options.description)
      pv->text(n->name.section('.', 0, 0) + ".DESC",
               [this, n](QString value) {
                 states[n].description = value;
                 if (changed) changed();
               });
    startForce(n);
  }
  // Groups without enabled monitors will never receive a severity transition.
  // Publish their settled state after constant force calculations have run.
  QSet<Node*> awaitingEvent;
  for (auto n : document.channels())
    if (!states[n].mask[Cancel] && !states[n].mask[Disable])
      for (auto parent = n->parent; parent; parent = parent->parent)
        awaitingEvent.insert(parent);
  if (options.global && !options.passive)
    for (auto n : document.nodes()) {
      auto name = n->option("SEVRPV");
      if (n->group && !awaitingEvent.contains(n) && !states[n].initialized && !name.isEmpty() &&
          name != "-")
        pv->put(name, states[n].severity, WriteKind::Severity);
    }
  auto heartbeat = fields(document.root->option("HEARTBEATPV"));
  if (!heartbeat.isEmpty()) {
    pv->prepare(heartbeat[0]);
    heartbeatInterval =
        qMax<qint64>(1, qRound64(1000 * (heartbeat.size() > 1 ? heartbeat[1].toDouble() : 1)));
    heartbeatDue = monotonicNow() + heartbeatInterval;
  }
}
struct Engine::ForceRuntime {
  QString source, expression;
  QByteArray compiled;
  std::array<double, CALCPERFORM_NARGS> values{};
  // calcPerform may assign A through F. ALH compared callbacks against that
  // mutable workspace, losing real changes or accepting duplicate inputs.
  std::array<double, 6> received{};
  std::array<QString, 6> inputs;
  std::array<bool, 6> available{};
  // Stable, per-input owners let editing A leave B's subscription untouched.
  std::array<unsigned, 6> generation{};
  bool resultValid = false;
  double result = -999;
};
void Engine::stopForce(Node* n) {
  auto runtime = forces.take(n);
  if (runtime && pv)
    for (int i = 0; i < 6; ++i)
      pv->cancelNumbers(&runtime->generation[i]);
}
bool Engine::startForce(Node* n) {
  if (!pv)
    return true;
  const auto source = fields(n->option("FORCEPV")).value(0);
  auto runtime = forces.value(n);
  if (source.isEmpty()) {
    stopForce(n);
    return true;
  }
  bool fresh = !runtime || runtime->source != source;
  if (fresh) {
    stopForce(n);
    runtime = std::make_shared<ForceRuntime>();
    runtime->source = source;
    forces.insert(n, runtime);
  }
  std::weak_ptr<ForceRuntime> weak = runtime;
  if (source != "CALC") {
    if (fresh)
      pv->number(source, [this, n, weak](double v) {
        if (weak.expired()) return;
        forceValue(n, v);
      }, &runtime->generation[0]);
    return fresh;
  }
  const auto expression = n->option("FORCEPV_CALC");
  bool calculationChanged = fresh || runtime->expression != expression;
  if (calculationChanged) {
    // Remember failed edits too, so restoring the previous expression recompiles.
    runtime->expression = expression;
    auto encoded = expression.toLocal8Bit();
    runtime->compiled = QByteArray(INFIX_TO_POSTFIX_SIZE(encoded.size() + 1), 0);
    short err = 0;
    if (postfix(encoded.constData(), runtime->compiled.data(), &err)) {
      runtime->compiled.clear();
      runtime->resultValid = false;
      if (error) error("Invalid force calculation for " + n->name);
    }
  }
  QVector<int> subscriptions;
  for (int i = 0; i < 6; ++i) {
    const auto name = n->option("FORCEPV_CALC_" + QString(QChar('A' + i)));
    if (!fresh && runtime->inputs[i] == name) continue;
    calculationChanged = true;
    if (!fresh) pv->cancelNumbers(&runtime->generation[i]);
    ++runtime->generation[i];
    runtime->inputs[i] = name;
    bool constant = false;
    double value = name.toDouble(&constant);
    runtime->values[i] = name.isEmpty() ? 0 : constant ? value
        : std::numeric_limits<double>::quiet_NaN();
    runtime->received[i] = runtime->values[i];
    runtime->available[i] = name.isEmpty() || (constant && std::isfinite(value));
    if (!name.isEmpty() && !constant) subscriptions << i;
  }
  // Preserve variables assigned by stateful expressions, even across expression
  // edits. Only explicitly changed input slots are reset. An unchanged Apply
  // neither resets variables (the old Qt bug) nor advances them (ALH's behavior).
  if (calculationChanged) {
    runtime->resultValid = false;
    evaluateForce(n);
  }
  for (int i : subscriptions) {
    const auto generation = runtime->generation[i];
    pv->number(runtime->inputs[i], [this, n, i, weak, generation](double v) {
      auto current = weak.lock();
      if (!current || current->generation[i] != generation) return;
      // An outage invalidates replay without overwriting calculation variables.
      // Recovery must process even the same value that preceded the outage.
      if (!std::isfinite(v)) {
        current->available[i] = false;
        current->resultValid = false;
        return;
      }
      if (current->available[i] && current->received[i] == v) return;
      current->available[i] = true;
      current->received[i] = v;
      // Update only this input slot. Reloading all received values here would
      // erase intentional stateful assignments made by earlier evaluations.
      current->values[i] = v;
      evaluateForce(n);
    }, &runtime->generation[i]);
  }
  return calculationChanged;
}
void Engine::evaluateForce(Node* n) {
  auto runtime = forces.value(n);
  if (!runtime) return;
  runtime->resultValid = false;
  if (runtime->compiled.isEmpty()) return;
  // Availability tracks finite external samples/constants, not the mutable
  // CALC workspace. An expression may store NaN/Inf in a temporary variable
  // and repair it on the next evaluation; rejecting that workspace here would
  // leave a force mask stuck. forceValue still rejects nonfinite final results.
  for (int i = 0; i < 6; ++i)
    if (!runtime->available[i]) return;
  // Possible inherited ALH bug, deferred: calcPerform reads VAL from this
  // result argument, so resetting it makes VAL+1 return 1 on every evaluation.
  // Revisit preserving the previous successful result and its initialization
  // semantics separately; retain legacy behavior for now.
  double result = 0;
  if (!calcPerform(runtime->values.data(), &result, runtime->compiled.constData()))
    forceValue(n, result);
}
void Engine::reapplyForce(Node* n) {
  const auto runtime = forces.value(n);
  if (runtime && !runtime->resultValid) {
    // Reapply the first valid result after recovery even if its number has not
    // changed. Never substitute the pre-outage cached result for fresh data.
    states[n].forceCurrent = -999;
    return;
  }
  const double value = runtime ? runtime->result : states[n].forceCurrent;
  states[n].forceCurrent = -999;
  forceValue(n, value);
}
void Engine::stop() {
  heartbeatDue = 0;
  heartbeatUnavailable = false;
  pendingForceAckT.clear();
  if (running && pv)
    pv->clear();
  forces.clear();
  running = false;
}
void Engine::logOperation(Node* n, const QString& s, OperationKind kind) {
  if (options.debug) debugLog(true, "operation", (n ? n->name + ": " : QString()) + s);
  if (operation)
    operation(n, s, kind);
  if (changed)
    changed();
}
void Engine::severityCommands(Node* n, int prev, int current) {
  if (current == prev)
    return;
  for (auto d : n->directives)
    if (d.key == "SEVRCOMMAND") {
      auto f = fields(d.value);
      if (f.size() < 2)
        continue;
      QString t = f[0];
      bool down = t.startsWith("DOWN_");
      bool up = t.startsWith("UP_");
      QString target = t.mid(down ? 5 : 3);
      if (!up && !down)
        continue;
      if ((down && current >= prev) || (up && current <= prev))
        continue;
      bool match = target == "ANY" || target == severityName(current) ||
                   (up && target == "ALARM" && prev == 0);
      if (match && command)
        command(d.value.mid(t.size()).trimmed());
    }
}
void Engine::propagate(Node* n, const State& before, ObservationCause cause,
                       bool publishGroupSeverity) {
  auto& s = states[n];
  updatePresentation(n, before);
  // Disabled monitors and acknowledgement-only updates must not establish the
  // group's command baseline from siblings still awaiting their first event.
  const bool severityEvent = effective(before) != effective(s) ||
      (!before.initialized && s.initialized && !s.mask[Disable] && !s.mask[Cancel]);
  int oldBeep = beep(before), newBeep = beep(s);
  for (auto p = n->parent; p; p = p->parent) {
    auto& g = states[p];
    // Initial ERROR counts describe channels still awaiting their first event.
    // Publish the first aggregate state even when those counts stay ERROR.
    bool first = !g.initialized;
    int previous = first ? 0 : g.severity;
    // Possible inherited ALH bug, deferred: startup Cancel/Disable can mark
    // this group initialized without publishing its SEVRPV. If a remaining
    // channel's first event leaves the aggregate at ERROR, no initial output
    // is sent. Track output publication separately in a later change; retain
    // the current initialization and Cancel/Disable behavior for now.
    if (severityEvent) g.initialized = true;
    g.counts[effective(before)]--;
    g.counts[effective(s)]++;
    int old = unacknowledged(before), cur = unacknowledged(s);
    if (old)
      g.unackCounts[old]--;
    if (cur)
      g.unackCounts[cur]++;
    if (oldBeep)
      g.beepCounts[oldBeep]--;
    if (newBeep)
      g.beepCounts[newBeep]++;
    oldBeep = oldBeep >= g.beepThreshold ? oldBeep : 0;
    newBeep = newBeep >= g.beepThreshold ? newBeep : 0;
    for (int i = 0; i < 5; ++i)
      g.maskCounts[i] += int(s.mask[i]) - int(before.mask[i]);
    g.severity = highest(g.counts);
    g.unack = highest(g.unackCounts);
    g.beep = highest(g.beepCounts);
    if (g.beep < g.beepThreshold)
      g.beep = 0;
    if (severityEvent) severityCommands(p, previous, g.severity);
    // ALH's Cancel/Disable paths update group state and commands without
    // publishing group SEVRPVs. Monitors and cached Add/Enable alarms do publish.
    if (publishGroupSeverity && severityEvent && (first || previous != g.severity) &&
        options.global && !options.passive && pv) {
      auto name = p->option("SEVRPV");
      if (!name.isEmpty() && name != "-")
        pv->put(name, g.severity, WriteKind::Severity);
    }
  }
  s.beep = beep(s);
  publish(n, before, cause);
  if (changed)
    changed();
}
void Engine::process(Node* n, Event e, qint64 time, int monitorSeverity) {
  if (options.debug)
    debugLog(true, "alarm", QString("%1 status=%2 severity=%3 ACKS=%4 ACKT=%5 value=%6")
        .arg(n->name, statusName(e.status), severityName(e.severity))
        .arg(e.acks).arg(e.ackt).arg(e.value));
  auto& s = states[n];
  State before = s;
  e.severity = qBound(0, e.severity, 4);
  e.acks = qBound(0, e.acks, 4);
  s.observedSeverity = monitorSeverity < 0 ? e.severity : qBound(0, monitorSeverity, 4);
  s.awaitingObservation = false;
  s.value = e.value;
  if (options.global) {
    if (e.ackt >= 0)
      s.mask[AckT] = !e.ackt;
    // Fix an ALH bug: synthetic connection/access errors carry ACKS=0, which
    // used to silence ERROR and hide it in the unacknowledged filter. Latch
    // these errors locally without replacing the last IOC acknowledgement.
    if (s.observedSeverity == ErrorSeverity) {
      if (!before.initialized || before.observedSeverity != ErrorSeverity || before.status != e.status)
        s.communicationUnack = true;
    } else {
      s.iocUnack = e.acks;
      if (s.mask[AckT]) s.communicationUnack = false;
    }
    s.unack = qMax(s.iocUnack, s.communicationUnack ? ErrorSeverity : 0);
  }
  bool first = !before.initialized;
  bool alarmChanged = s.status != e.status || s.severity != e.severity;
  bool transition = first || alarmChanged;
  s.status = e.status;
  s.severity = e.severity;
  s.initialized = true;
  // Value-only monitors update the displayed value, not the alarm log. Global
  // acknowledgement and transient-setting changes are still alarm records.
  bool acknowledgementChanged = options.global &&
      ((s.observedSeverity < ErrorSeverity && e.acks != s.observedAcks) ||
       (e.ackt >= 0 && e.ackt != s.observedAckT));
  if (s.observedSeverity < ErrorSeverity) s.observedAcks = e.acks;
  if (e.ackt >= 0)
    s.observedAckT = e.ackt;
  // Only the initial ERROR/NO_ALARM baseline is a startup connection. A
  // disabled channel, or one explicitly added after Cancel, starts at zero.
  const bool initialConnection = !before.initialized && before.status == 0 && before.severity == 4;
  if (!s.mask[Log] && !initialConnection && (alarmChanged || acknowledgementChanged))
    if (alarmLog)
      alarmLog(n, s, time);
  if (transition && !s.mask[Disable]) {
    addHistory(n, time);
    // ALH dispatches startup channel commands from its initial ERROR severity.
    severityCommands(n, before.severity, s.severity);
    if (s.status != before.status)
      for (auto d : n->directives)
        if (d.key == "STATCOMMAND") {
          auto f = fields(d.value);
          if (!f.isEmpty() && f[0] == statusName(s.status) && command)
            command(d.value.mid(f[0].size()).trimmed());
        }
    if ((first || s.severity != before.severity) && options.global && !options.passive && pv) {
      auto name = n->option("SEVRPV");
      if (!name.isEmpty() && name != "-")
        pv->put(name, s.severity, WriteKind::Severity);
    }
    if (!s.mask[Ack] && !options.global && (s.severity >= s.unack || s.mask[AckT]))
      s.unack = s.severity;
  }
  auto cause = ObservationCause::Processed;
  // A recovery filter may retain the alarm severity after the IOC has cleared.
  // Use the monitor state to distinguish automatic clears from acknowledgement.
  if (options.global && before.initialized && !before.awaitingObservation &&
      before.observedSeverity < 4 && s.observedSeverity < 4 &&
      before.iocUnack > 0 && s.iocUnack == 0 && !s.mask[Ack] && !s.mask[Disable] &&
      (s.observedSeverity > 0 || (before.observedSeverity == 0 && !s.mask[AckT])))
    cause = requestedAcknowledgements.contains(n) ? ObservationCause::RequestedAcknowledgement
                                                : ObservationCause::ExternalAcknowledgement;
  if (!s.unack || s.severity == 4) requestedAcknowledgements.remove(n);
  propagate(n, before, cause, true);
}
void Engine::addHistory(Node* n, qint64 time) {
  const auto& s = states[n];
  // Reuse the timestamp within a second, including delayed filter events.
  const qint64 second = time / 1000 - (time % 1000 < 0 ? 1 : 0);
  if (historyTimestamp.isEmpty() || second != historySecond) {
    historySecond = second;
    historyTimestamp = QDateTime::fromMSecsSinceEpoch(time).toString("dd-MMM-yyyy HH:mm:ss");
  }
  history.prepend(historyTimestamp + " " + n->name + " " + statusName(s.status) + " " +
                  severityName(s.severity) + " " + s.value);
  while (history.size() > 10)
    history.removeLast();
}
void Engine::event(Node* n, Event e) {
  qint64 time = now();
  const auto elapsed = monotonicNow();
  // CA callbacks can arrive after a filter expires but before the UI's next
  // tick. Dispatch due timers before replacing the saved event: otherwise a
  // recovery can erase an alarm that lasted longer than its configured delay.
  // Use the shared deadline order, including earlier NoAck expirations and
  // sibling filters whose commands may cancel this channel.
  processDeadlines(time, elapsed);
  auto& s = states[n];
  if (s.mask[Cancel])
    return;
  Event previous = s.pending;
  s.pending = e;
  // A normal-to-ERROR transition can be held by the display's alarm filter.
  // Invalidate observation coverage immediately; only processing a fresh state
  // may establish a baseline again, including after a short filtered outage.
  if (e.severity >= ErrorSeverity && s.initialized && s.severity == 0 &&
      s.filterSeconds && !s.awaitingObservation) {
    State before = s;
    s.awaitingObservation = true;
    requestedAcknowledgements.remove(n);
    publish(n, before, ObservationCause::Suppression);
  }
  // Only ALH's initial ERROR baseline bypasses filtering. Initially cancelled
  // or disabled channels start at NO_ALARM and must respect the configured delay.
  if ((!s.initialized && s.status == 0 && s.severity == ErrorSeverity) || s.filterSeconds == 0) {
    process(n, e, time);
    return;
  }
  auto reset = [&] {
    s.filterUntil = 0;
    s.edgeIndex = 0;
    std::fill(s.edges.begin(), s.edges.end(), 0);
  };
  if (s.severity == 0 && e.severity == 0) {
    if (e.acks == 0)
      process(n, e, time);
    else {
      // Keep ALH's suppression of unregistered transient ACKS, but do not
      // discard a fresh value or leave observation coverage in an outage.
      // This refresh has no alarm, acknowledgement, command or output effects.
      State before = s;
      s.value = e.value;
      s.observedSeverity = e.severity;
      s.awaitingObservation = false;
      publish(n, before, ObservationCause::Processed);
      if (changed) changed();
    }
    s.filterUntil = 0;
  }
  if (s.severity != 0 && e.severity != 0) {
    process(n, e, time);
    s.filterUntil = 0;
  }
  if (s.severity == 0 && e.severity != 0 && !s.filterUntil) {
    s.filterUntil = elapsed + 1000LL * s.filterSeconds;
    noteDeadline(s.filterUntil);
    s.filterStarted = time;
  }
  if (s.severity != 0 && e.severity == 0) {
    if (s.filterCount == -1) {
      reset();
      process(n, e, time);
    } else {
      Event held = e;
      held.status = s.status;
      held.severity = s.severity;
      process(n, held, time, e.severity);
      if (!s.filterUntil) {
        s.filterUntil = elapsed + 1000LL * s.filterSeconds;
        noteDeadline(s.filterUntil);
        s.filterStarted = time;
      }
    }
  }
  if (s.filterCount > 0 && ((previous.severity == 0) != (e.severity == 0))) {
    qint64 old = s.edges[s.edgeIndex];
    if (old && elapsed - old <= 1000LL * s.filterSeconds) {
      reset();
      process(n, e, time);
    } else {
      s.edges[s.edgeIndex] = elapsed;
      s.edgeIndex = (s.edgeIndex + 1) % s.edges.size();
    }
  }
}
void Engine::acknowledge(Node* n) {
  if (options.passive) {
    if (error)
      error("You can't acknowledge alarms in passive mode.");
    return;
  }
  if (!state(n).unack) return;
  const auto summary = QString(options.global ? "Global" : "Local") + " Ack " +
      (n->group ? "Group" : "Channel") + " (" + severityName(state(n).unack) + ") " +
      severityName(state(n).severity).leftJustified(16, ' ');
  bool summaryLogged = false;
  auto recordAccepted = [&] {
    if (!summaryLogged)
      logOperation(n, summary, n->group ? OperationKind::AckGroup : OperationKind::AckChannel);
    summaryLogged = true;
  };
  if (!options.global) recordAccepted();
  std::function<void(Node*)> ack = [&](Node* c) {
    if (c->group) {
      // ALH visits direct channels before subgroups, and only descends into
      // groups with outstanding alarms. ACKPV writes can depend on this order.
      if (states[c].unack) {
        for (auto& child : c->children)
          if (!child->group) ack(child.get());
        for (auto& child : c->children)
          if (child->group) ack(child.get());
      }
      return;
    }
    auto& s = states[c];
    if (s.shelf.until || !s.unack)
      return;
    if (options.global) {
      if (s.communicationUnack) {
        State old = s;
        s.communicationUnack = false;
        s.unack = s.iocUnack;
        // Communication errors belong to this runtime, not the IOC/ACKPV.
        propagate(c, old, ObservationCause::LocalAcknowledgement);
        logOperation(c, "Local acknowledgement of communication error");
      }
      if (s.observedSeverity == ErrorSeverity || !s.iocUnack) return;
      // Fix an ALH bug: ackChan emitted a success record and wrote ACKPV even
      // when the ACKS submission failed. Only accepted requests have those
      // side effects; the IOC monitor still confirms the actual acknowledgement.
      if (!pv || !pv->put(c->name, s.iocUnack, WriteKind::Acknowledge)) {
        logOperation(c, "Global acknowledgement failed: ACKS request was not submitted");
        return;
      }
      requestedAcknowledgements.insert(c);
      recordAccepted();
      if (acknowledgement) acknowledgement(c);
      auto a = fields(c->option("ACKPV"));
      // Possible inherited ALH bug, deferred: ACKS may succeed while this
      // separate ACKPV write fails. Once IOC confirmation clears ACKS, another
      // Acknowledge cannot retry the missing action. Revisit partial-completion
      // reporting and an independent, explicit ACKPV retry at a later date;
      // automatically replaying arbitrary ACKPV actions needs separate policy.
      if (a.size() == 2)
        pv->put(a[0], a[1].toDouble(), WriteKind::AckValue);
    } else {
      if (acknowledgement) acknowledgement(c);
      State old = s;
      s.unack = 0;
      propagate(c, old, ObservationCause::LocalAcknowledgement);
    }
  };
  ack(n);
}
void Engine::setMask(Node* n, Mask requested, bool automatic) {
  const bool failed = applyMask(n, requested, automatic, automatic ? n : nullptr);
  logOperation(n, QString(automatic ? "Force Mask " : "Change Mask ") + requested.text() +
      (failed ? " (ACKT write failed; previous setting retained)" : ""),
      automatic ? (n->group ? OperationKind::ForceGroupMask : OperationKind::ForceMask)
                : (n->group ? OperationKind::ChangeGroupMask : OperationKind::ChangeMask));
}
bool Engine::applyMask(Node* n, Mask requested, bool automatic, Node* forceSource) {
  bool ackTFailed = false;
  eachChannel(n, [&](Node* c) {
    auto& s = states[c];
    if (s.mask[Cancel] && !s.mask[Disable] && !requested[Cancel] &&
        requested[Disable] && s.severity > 0) {
      // A cancelled channel can retain an alarm observed while disabled.
      // ALH applies Add before Disable: expose that cached alarm with the old
      // settings, including its logs, commands and severity output, then apply
      // the remaining mask changes. Propagate both states to balance aggregates.
      auto added = s.mask;
      added[Cancel] = false;
      applyMask(c, added, false, nullptr);
    }
    State old = s;
    Mask mask = requested;
    // A newer eligible mask request supersedes the PV's older automatic write,
    // including requests through another row, no-ops and failed manual writes.
    // Manual failures retain explicit-retry behavior; they never resurrect an
    // older force. Traversal order supplies deterministic group precedence.
    quint64 request = 0;
    if (options.global && !options.passive) {
      request = ++nextAckTRequest;
      pendingForceAckT.remove(acknowledgementIdentity(c->name));
    }
    if (options.passive && (!automatic || options.global))
      mask[AckT] = old.mask[AckT];
    s.mask = mask;
    // ALH applies Enable/NoAck side effects before changing ACKT and NoLog.
    // In particular, a cached alarm is logged with the previous settings.
    s.mask[AckT] = old.mask[AckT];
    s.mask[Log] = old.mask[Log];
    if (mask[Cancel] || mask[Disable] || mask[Ack]) requestedAcknowledgements.remove(c);
    if (mask[Cancel]) {
      s.awaitingObservation = true;
      s.filterUntil = s.filterStarted = 0;
      s.edgeIndex = 0;
      std::fill(s.edges.begin(), s.edges.end(), 0);
      s.pending = {};
    }
    if (mask[Cancel] && !old.mask[Cancel]) {
      // ALH applies Cancel before changing Disable, using the previous setting.
      if (!old.mask[Disable]) {
        s.severity = 0;
        s.status = 0;
        s.unack = 0;
        s.iocUnack = 0;
        s.communicationUnack = false;
      }
    }
    if (!mask[Cancel] && old.mask[Cancel])
      monitor(c);
    if (!options.global) {
      if (mask[Ack] || mask[Disable])
        s.unack = 0;
      else if (!mask[Cancel] && (old.mask[Ack] || old.mask[Disable] || old.mask[Cancel]))
        s.unack = s.severity;
    }
    const bool restoredAlarm = (old.mask[Disable] || old.mask[Cancel]) &&
                              !mask[Disable] && !mask[Cancel] && s.severity > 0;
    if (restoredAlarm) {
      // ALH processes a cached alarm again on Enable/Add. Record that exposure
      // without treating it as a fresh IOC observation or propagating twice.
      const auto time = now();
      if (!s.mask[Log] && alarmLog)
        alarmLog(c, s, time);
      addHistory(c, time);
      severityCommands(c, 0, s.severity);
    }
    if (options.global && !options.passive && pv) {
      // Clear transients suppressed while disabled or NoAck before exposing
      // them again. Keep the latch if the IOC acknowledgement cannot be sent.
      // Enabling clears a suppressed transient even if this same request sets
      // NoAck. ALH handles Disable before Ack; testing only the final mask would
      // skip its ACKS/ACKPV writes in a D -> A change.
      const bool enabled = old.mask[Disable] && !mask[Disable];
      const bool ackRestored = old.mask[Ack] && !mask[Ack] && !mask[Disable];
      if ((enabled || ackRestored) && !mask[Cancel] &&
          s.severity == 0 && s.unack > 0 &&
          (!s.iocUnack || pv->put(c->name, s.iocUnack, WriteKind::Acknowledge))) {
        if (s.iocUnack) {
          if (acknowledgement) acknowledgement(c);
          auto a = fields(c->option("ACKPV"));
          // As with operator acknowledgement above, a failed ACKPV submission
          // is currently not retained after ACKS succeeds (deferred ALH bug).
          if (a.size() == 2)
            pv->put(a[0], a[1].toDouble(), WriteKind::AckValue);
        }
        s.unack = s.iocUnack = 0;
        s.communicationUnack = false;
        logOperation(c, "Auto ack of transient alarms on enable");
      }
      if (!mask[Cancel] && (mask[Disable] != old.mask[Disable] || restoredAlarm)) {
        auto name = c->option("SEVRPV");
        if (!name.isEmpty() && name != "-")
          pv->put(name, mask[Disable] ? -1 : s.severity, WriteKind::Severity);
      }
    }
    // Restore a cancelled channel before writing, and defer cancellation until
    // afterward. Failed writes leave the displayed setting unchanged/retryable.
    // Before an IOC monitor arrives, the configured bit is only a placeholder.
    // An automatic force must write (or defer) even if that bit already matches.
    // Possible inherited ALH bug, deferred: after a successful ACKT submission
    // through one row, another row for the same record still has its old mask
    // until the IOC confirms it. A later request matching that stale mask is
    // skipped here, leaving the earlier setting applied. Track successful
    // outstanding requests per record when deciding whether a write is a
    // no-op in a later change; retain current runtime mask behavior for now.
    if (options.global && !options.passive &&
        (mask[AckT] != old.mask[AckT] || (automatic && old.observedAckT < 0)) &&
        (!pv || !pv->put(c->name, !mask[AckT], WriteKind::AckTransient))) {
      if (automatic && !options.passive)
        pendingForceAckT.insert(acknowledgementIdentity(c->name), {forceSource, c, mask[AckT], request});
      mask[AckT] = old.mask[AckT];
      ackTFailed = true;
    }
    // Possible inherited ALH bug, deferred: enabling local NoAckT changes the
    // bit without reconciling an already latched transient. A MAJOR -> normal
    // transition followed by NoAckT can remain unacknowledged and audible,
    // even through normal value-only updates. Revisit clearing/reducing that
    // local latch and its aggregates when the setting changes; retain behavior
    // for now, as in alh/alLib.c's alChangeChanMask.
    s.mask[AckT] = mask[AckT];
    if (mask[Cancel] && !old.mask[Cancel] && pv)
      pv->cancel(c->name, c);
    // Keep global writes available after monitoring is cancelled, including a
    // startup ACKT setting still waiting for write access.
    if (pv && (pendingForceAckT.contains(acknowledgementIdentity(c->name)) ||
               (mask[Cancel] && options.global && !options.passive)))
      pv->prepare(c->name);
    s.mask[Log] = mask[Log];
    propagate(c, old, ObservationCause::Suppression, restoredAlarm);
  });
  return ackTFailed;
}
void Engine::discardForceWrites(Node* source) {
  for (auto i = pendingForceAckT.begin(); i != pendingForceAckT.end();)
    if (i->source == source)
      i = pendingForceAckT.erase(i);
    else
      ++i;
}
void Engine::retryForceWrites() {
  if (!pv || !options.global || options.passive)
    return;
  auto pendingWrites = pendingForceAckT.values();
  std::sort(pendingWrites.begin(), pendingWrites.end(), [](const PendingAckT& a, const PendingAckT& b) {
    return a.request < b.request;
  });
  for (const auto& pending : pendingWrites) {
    auto c = pending.channel;
    auto current = pendingForceAckT.constFind(acknowledgementIdentity(c->name));
    // A preceding callback may have superseded/cancelled a snapshotted request.
    if (current == pendingForceAckT.cend() || current->request != pending.request)
      continue;
    if (!pv->canWrite(c->name) || !pv->put(c->name, !pending.mask, WriteKind::AckTransient))
      continue;
    current = pendingForceAckT.constFind(acknowledgementIdentity(c->name));
    if (current == pendingForceAckT.cend() || current->request != pending.request)
      continue;
    pendingForceAckT.remove(acknowledgementIdentity(c->name));
    State before = states[c];
    // Preserve per-row display behavior: update the requesting row here; other
    // monitored rows learn the shared ACKT value from their IOC confirmations.
    states[c].mask[AckT] = pending.mask;
    propagate(c, before);
    logOperation(c, "Applied deferred force ACKT setting");
  }
}
void Engine::resetMask(Node* n, bool automatic) {
  bool failed = false;
  eachChannel(n, [&](Node* c) { failed |= applyMask(c, c->mask, automatic, automatic ? n : nullptr); });
  logOperation(n, QString(automatic ? "Force PV Reset Mask" : "Reset Mask") +
      (failed ? " (ACKT write failed; previous setting retained)" : ""),
      n->group ? OperationKind::ChangeGroupMask : OperationKind::ChangeMask);
}
void Engine::modifyMask(Node* n, int bit, int choice) {
  if (bit < 0 || bit >= 5 || choice < 0 || choice > 2)
    throw ParseError("Invalid mask action");
  bool failed = false;
  eachChannel(n, [&](Node* c) {
    auto mask = states[c].mask;
    mask[bit] = choice == 2 ? c->mask[bit] : choice == 1;
    failed |= applyMask(c, mask, false, nullptr);
    logOperation(c, "Group Mask ID --- " + c->name, OperationKind::MaskChannel);
  });
  // The legacy Modify Mask menu uses code 7 even for group actions.
  const QStringList bits = {"Cancel", "Disable", "NoAck", "NoAckT", "NoLog"};
  const QStringList choices = {"Off", "On", "Reset"};
  logOperation(n, QString("Modify Mask %1 %2").arg(bits[bit], choices[choice]) +
      (failed ? " (ACKT write failed; previous setting retained)" : ""), OperationKind::ChangeMask);
}
void Engine::cancelNoAckTimer(Node* n) {
  const bool active = states[n].noAckUntil != 0;
  states[n].noAckUntil = 0;
  if (active && changed) changed();
  for (auto& child : n->children)
    cancelNoAckTimer(child.get());
}
void Engine::noAck(Node* n, bool enabled) {
  std::function<void(Node*)> apply = [&](Node* c) {
    if (enabled)
      states[c].noAckUntil = 0;
    else if (c != n && states[c].noAckUntil)
      return;
    if (c->group) {
      // Match ALH's traversal without flattening independently timed branches.
      for (auto& child : c->children)
        if (!child->group) apply(child.get());
      for (auto& child : c->children)
        if (child->group) apply(child.get());
    } else {
      auto m = states[c].mask;
      m[Ack] = enabled ? true : c->mask[Ack];
      applyMask(c, m, false, nullptr);
      logOperation(c, "Group Mask ID --- " + c->name, OperationKind::MaskChannel);
    }
  };
  states[n].noAckUntil = 0;
  apply(n);
  states[n].noAckUntil = enabled ? monotonicNow() + 3600000 : 0;
  noteDeadline(states[n].noAckUntil);
  logOperation(n, enabled ? "Set NoAck and start NoAck one hour timer"
                          : "Reset Ack mask and cancel NoAck one hour timer");
}
void Engine::forceValue(Node* n, double value) {
  // Keep a valid result for settings-only edits without re-running a stateful
  // CALC. Outages invalidate replay, but retain forceCurrent for NE edge history.
  if (auto runtime = forces.value(n)) {
    runtime->resultValid = std::isfinite(value);
    if (runtime->resultValid) runtime->result = value;
  }
  // A disconnect is not a force value; retain the last valid value for NE reset.
  if (!std::isfinite(value))
    return;
  auto& forceState = states[n];
  double previous = forceState.forceCurrent;
  forceState.forceCurrent = value;
  // Intentionally retain ALH's -999 sentinel collision: a first/reinitialized
  // sample of -999 is treated as unchanged, even when it is the force value.
  if (forceState.forceDisabled || value == previous)
    return;
  auto f = fields(n->option("FORCEPV"));
  if (f.size() < 2)
    return;
  double forced = f.size() > 2 ? f[2].toDouble() : 1, reset = f.size() > 3 ? f[3].toDouble() : 0;
  bool ne = (f.size() > 3 && f[3] == "NE") || reset == forced;
  // Only CALC results use legacy float comparison; scalar PVs are doubles.
  // Use that same comparison for the previous forced value in an NE reset.
  // ALH compares the previous value as a double there, which can leave the
  // force mask stuck after a result matched the force value only by rounding.
  auto equal = [&](double a, double b) { return f[0] == "CALC" ? float(a) == float(b) : a == b; };
  if (equal(value, forced))
    setMask(n, Mask::parse(f[1]), true);
  else if ((ne && (previous == -999 || equal(previous, forced)) && !equal(value, forced)) ||
           (!ne && equal(value, reset)))
    resetMask(n, true);
}
void Engine::configureForce(Node* n, const QVector<Directive>& directives, bool disabled) {
  // Copy before modifying the node: callers may pass n->directives itself.
  QVector<Directive> replacement, previous;
  for (const auto& d : directives)
    if (d.key.startsWith("FORCEPV")) replacement.push_back(d);
  for (const auto& d : n->directives)
    if (d.key.startsWith("FORCEPV")) previous.push_back(d);
  auto effectiveOptions = [](const QVector<Directive>& values) {
    QHash<QString, QString> result;
    // The editor emits a fixed order; match Node::option's first-value lookup
    // so reordering equivalent directives cannot trigger a force action.
    for (const auto& d : values) if (!result.contains(d.key)) result.insert(d.key, d.value);
    return result;
  };
  const bool same = effectiveOptions(previous) == effectiveOptions(replacement);
  if (same) {
    if (states[n].forceDisabled != disabled) setForceDisabled(n, disabled);
    return; // In particular, do not discard a pending automatic ACKT write.
  }
  discardForceWrites(n);
  for (int i = n->directives.size() - 1; i >= 0; --i)
    if (n->directives[i].key.startsWith("FORCEPV")) n->directives.removeAt(i);
  n->directives += replacement;
  states[n].forceCurrent = -999;
  states[n].forceDisabled = disabled;
  if (!startForce(n)) reapplyForce(n);
  // ALH audits Force PV Apply with codes 9/10 even without a mask transition.
  // Other/code 0 would retain the file record but omit database delivery.
  logOperation(n, "Change Force PV " + n->option("FORCEPV"),
               n->group ? OperationKind::ForceGroupMask : OperationKind::ForceMask);
}
void Engine::setForceDisabled(Node* n, bool disabled) {
  if (states[n].forceDisabled == disabled) return;
  if (disabled) discardForceWrites(n);
  states[n].forceDisabled = disabled;
  if (!disabled) reapplyForce(n);
  // Enable/disable-only Apply uses the same legacy Force PV audit codes.
  logOperation(n, disabled ? "Disable Force PV" : "Enable Force PV",
               n->group ? OperationKind::ForceGroupMask : OperationKind::ForceMask);
}
void Engine::rebuildBeep() {
  const auto nodes = document.nodes();
  for (auto n : nodes) {
    states[n].highestBeepThreshold = states[n].beepThreshold;
    if (n->group)
      states[n].beepCounts.fill(0);
  }
  // Include subgroup thresholds even when they have no channels. Rebuilding
  // from the leaves also removes obsolete maxima when a setting is lowered.
  for (auto i = nodes.crbegin(); i != nodes.crend(); ++i)
    if (auto parent = (*i)->parent)
      states[parent].highestBeepThreshold =
          qMax(states[parent].highestBeepThreshold, states[*i].highestBeepThreshold);
  for (auto n : document.channels()) {
    int b = beep(states[n]);
    states[n].beep = b;
    for (auto p = n->parent; p; p = p->parent) {
      auto& s = states[p];
      if (b)
        s.beepCounts[b]++;
      if (b < s.beepThreshold)
        b = 0;
    }
  }
  for (auto n : document.nodes())
    if (n->group) {
      auto& s = states[n];
      s.beep = highest(s.beepCounts);
      if (s.beep < s.beepThreshold)
        s.beep = 0;
    }
}
void Engine::setBeep(Node* n, int v) {
  states[n].beepThreshold = v;
  n->setOption("BEEPSEVR", severityName(v));
  rebuildBeep();
  rebuildPresentation();
  logOperation(n, "Set beep severity " + severityName(v));
}
void Engine::setSilenceCurrent(bool enabled) {
  if (silenceCurrent == enabled) return;
  silenceCurrent = enabled;
  logOperation(nullptr, enabled ? "Silence Current set to TRUE" : "Silence Current set to FALSE");
}
void Engine::setSilenceForever(bool enabled) {
  if (silenceForever == enabled) return;
  silenceForever = enabled;
  logOperation(nullptr, enabled ? "Silence Forever set to TRUE" : "Silence Forever set to FALSE");
}
void Engine::setSilenceUntil(qint64 deadline) {
  const bool wasEnabled = silenceUntil != 0;
  silenceUntil = deadline;
  if (wasEnabled != (deadline != 0))
    logOperation(nullptr, deadline ? "Silence Selected Minutes set to TRUE"
                                   : "Silence Selected Minutes set to FALSE");
}
bool Engine::audible() const {
  if (silenceForever || silenceCurrent || monotonicNow() < silenceUntil || !document.root)
    return false;
  return presentation(document.root.get()).beep >= qMax(1, document.beepSeverity);
}

Presentation Engine::presentation(Node* n) const {
  const auto& s = state(n);
  if (n->group) return s.presentation;
  Presentation result;
  result.shelved = s.shelf.until != 0;
  if (!result.shelved) {
    result.severity = effective(s);
    result.unack = unacknowledged(s);
    result.beep = beep(s);
    result.counts[result.severity] = 1;
  }
  return result;
}
void Engine::updatePresentation(Node* n, const State& before, bool notify) {
  const auto& after = states[n];
  int oldB = before.shelf.until ? 0 : beep(before);
  int newB = after.shelf.until ? 0 : beep(after);
  for (auto p = n->parent; p; p = p->parent) {
    auto& parent = states[p];
    auto& g = parent.presentation;
    if (!before.shelf.until) {
      --g.counts[effective(before)];
      if (unacknowledged(before)) --g.unackCounts[unacknowledged(before)];
    }
    if (!after.shelf.until) {
      ++g.counts[effective(after)];
      if (unacknowledged(after)) ++g.unackCounts[unacknowledged(after)];
    }
    g.shelved += int(after.shelf.until != 0) - int(before.shelf.until != 0);
    if (oldB) --g.beepCounts[oldB];
    if (newB) ++g.beepCounts[newB];
    g.severity = highest(g.counts);
    g.unack = highest(g.unackCounts);
    g.beep = highest(g.beepCounts);
    if (g.beep < parent.beepThreshold) g.beep = 0;
    if (oldB < parent.beepThreshold) oldB = 0;
    if (newB < parent.beepThreshold) newB = 0;
  }
  // ALH resets current-alarm silence on any changed audible unacknowledged
  // severity, including a downward transition with NoAckT.
  if (notify && newB != oldB && newB >= document.beepSeverity) setSilenceCurrent(false);
}
void Engine::rebuildPresentation() {
  for (auto n : document.nodes()) states[n].presentation = {};
  // A shelved placeholder contributes no counts. Balance its shelf count before
  // adding each real channel through the same incremental aggregation path.
  State absent;
  absent.shelf.until = 1;
  for (auto n : document.channels()) {
    for (auto p = n->parent; p; p = p->parent) ++states[p].presentation.shelved;
    updatePresentation(n, absent, false);
  }
}
AlarmSubscription Engine::observe(std::function<void(const AlarmObservation&)> callback) {
  auto lifetime = std::make_shared<int>(0);
  observers.push_back({lifetime, std::move(callback)});
  return lifetime;
}
void Engine::publish(Node* n, const State& before, ObservationCause cause) {
  observers.erase(std::remove_if(observers.begin(), observers.end(),
      [](const Observer& o) { return o.lifetime.expired(); }), observers.end());
  if (!channelUpdated && observers.isEmpty()) return;
  auto after = channelUpdate(n);
  if (channelUpdated) channelUpdated(after);
  if (observers.isEmpty()) return;
  AlarmObservation observation{snapshot(n, before), after, cause};
  const auto listeners = observers;
  for (const auto& listener : listeners)
    if (!listener.lifetime.expired()) listener.callback(observation);
}
QString Engine::nodeIdentity(const Node* n) { return identity(n); }
ChannelUpdate Engine::channelUpdate(Node* n) const { return snapshot(n, state(n)); }
ChannelUpdate Engine::snapshot(Node* n, const State& s) const {
  ChannelUpdate u;
  u.identity = identity(n); u.path = channelPath(n); u.pv = n->name; u.value = s.value;
  for (auto p = n; p; p = p->parent) u.ancestors << identity(p);
  u.severity = s.severity; u.status = s.status; u.unack = s.unack;
  u.initialized = s.initialized;
  u.suppressed = s.mask[Cancel] || s.mask[Disable] || s.mask[Ack] || s.shelf.until;
  u.cancelled = s.mask[Cancel]; u.disabled = s.mask[Disable];
  u.noAck = s.mask[Ack]; u.shelved = s.shelf.until != 0;
  u.available = s.initialized && !s.awaitingObservation && s.severity < 4 && !u.cancelled;
  u.communicationError = options.global && s.observedSeverity == ErrorSeverity;
  u.observedAt = now();
  return u;
}
void Engine::restoreLocalAcknowledgements(const QHash<QString, int>& saved) {
  if (options.global || saved.isEmpty()) return;
  QHash<QString, int> counts;
  for (auto n : document.channels()) ++counts[identity(n)];
  for (auto n : document.channels()) {
    auto& s = states[n]; const auto key = identity(n);
    if (counts[key] == 1 && saved.contains(key) && !s.mask[Ack] && !s.mask[Disable] && !s.mask[Cancel])
      s.unack = qBound(0, saved[key], 4);
  }
  for (auto n : document.nodes()) if (n->group) states[n].unackCounts.fill(0);
  for (auto n : document.channels()) {
    const int u = unacknowledged(states[n]);
    if (u) for (auto p = n->parent; p; p = p->parent) ++states[p].unackCounts[u];
  }
  for (auto n : document.nodes()) if (n->group) states[n].unack = highest(states[n].unackCounts);
  rebuildBeep(); rebuildPresentation();
}
QString Engine::channelPath(const Node* n) {
  QStringList parts;
  for (; n; n = n->parent) {
    QString part = n->name;
    part.replace("\\", "\\\\");
    part.replace("/", "\\/");
    parts.prepend(part);
  }
  return "/" + parts.join('/');
}
bool Engine::validShelfUsername(const QString& username) {
  const auto text = username.trimmed();
  return !text.isEmpty() && text.size() <= 120 &&
      !text.contains(QRegularExpression("[\\s\\x{0000}-\\x{001f}\\x{007f}\\x{2028}\\x{2029}]"));
}
int Engine::shelve(Node* n, int minutes, const QString& reason, const QString& username, bool replace) {
  const QString user = username.trimmed();
  if (!validShelfUsername(user))
    throw ParseError("Shelving requires a username (1–120 characters, without spaces or control characters).");
  const QString text = reason.trimmed();
  if (minutes < 1 || minutes > MaximumShelfMinutes || text.isEmpty() || text.size() > 240 ||
      text.contains(QRegularExpression("[\\r\\n\\x{2028}\\x{2029}]")))
    throw ParseError("Shelving requires a duration from 1 minute to 365 days and a single-line reason (1–240 characters).");
  if (!n || (replace && n->group)) throw ParseError("Select one channel to change its shelf.");
  state(n); // Validate ownership before traversing.
  const qint64 time = now();
  const qint64 duration = qint64(minutes) * 60000;
  if (time > std::numeric_limits<qint64>::max() - duration)
    throw ParseError("Shelving deadline is out of range.");
  int count = 0;
  eachChannel(n, [&](Node* c) {
    auto& s = states[c];
    if (s.shelf.until && s.shelf.until <= time) clearShelf(c, "Shelf expired");
    if (s.shelf.until && !replace) return;
    State before = s;
    s.shelf = {time + duration, text, user};
    updatePresentation(c, before);
    publish(c, before, ObservationCause::Suppression);
    noteShelfDeadline(s.shelf.until);
    logOperation(c, QString(before.shelf.until ? "Change shelf " : "Shelve ") + channelPath(c) +
        " until=" + QDateTime::fromMSecsSinceEpoch(s.shelf.until).toString(Qt::ISODateWithMs) +
        " username=" + user +
        (before.shelf.until ? " previous_username=" + before.shelf.username : QString()) +
        " reason=" + text);
    ++count;
  });
  if (count && changed) changed();
  return count;
}
void Engine::clearShelf(Node* n, const QString& action) {
  auto& s = states[n];
  if (!s.shelf.until) return;
  State before = s;
  s.shelf = {};
  updatePresentation(n, before);
  publish(n, before, ObservationCause::Suppression);
  logOperation(n, action + " " + channelPath(n) + " until=" +
      QDateTime::fromMSecsSinceEpoch(before.shelf.until).toString(Qt::ISODateWithMs) +
      " shelved_by=" + before.shelf.username + " reason=" + before.shelf.reason);
  if (changed) changed();
}
void Engine::unshelve(Node* n) {
  if (!n) return;
  state(n);
  eachChannel(n, [&](Node* c) { clearShelf(c, "Unshelve"); });
}
QVector<ShelfSnapshot> Engine::shelves() const {
  QHash<QString, int> counts;
  const auto channels = document.channels();
  for (auto n : channels) ++counts[identity(n)];
  QVector<ShelfSnapshot> result;
  for (auto n : channels) {
    const auto& s = state(n);
    if (s.shelf.until)
      result.push_back({identity(n), channelPath(n), s.shelf, s.unack, counts[identity(n)] == 1});
  }
  return result;
}
void Engine::restoreShelves(const QVector<ShelfSnapshot>& saved) {
  QHash<QString, QVector<Node*>> targets;
  for (auto n : document.channels()) targets[identity(n)].push_back(n);
  for (const auto& entry : saved) {
    const auto matches = targets.value(entry.identity);
    if (!entry.unique || matches.size() != 1) {
      logOperation(document.root.get(), "Drop shelf on reload " + entry.path +
          " (missing or ambiguous identity) until=" +
          QDateTime::fromMSecsSinceEpoch(entry.shelf.until).toString(Qt::ISODateWithMs) +
          " shelved_by=" + entry.shelf.username + " reason=" + entry.shelf.reason);
      continue;
    }
    auto n = matches.front();
    auto& s = states[n];
    State before = s;
    s.shelf = entry.shelf;
    if (!options.global && !s.mask[Ack] && !s.mask[Disable]) s.unack = entry.localUnack;
    updatePresentation(n, before, false);
    if (s.shelf.until <= now()) clearShelf(n, "Shelf expired during reload");
    else noteShelfDeadline(s.shelf.until);
  }
  for (auto n : document.nodes()) if (n->group) states[n].unackCounts.fill(0);
  for (auto n : document.channels()) {
    const int u = unacknowledged(states[n]);
    if (u) for (auto p = n->parent; p; p = p->parent) ++states[p].unackCounts[u];
  }
  for (auto n : document.nodes()) if (n->group) states[n].unack = highest(states[n].unackCounts);
  rebuildBeep();
  rebuildPresentation();
  if (changed) changed();
}

void Engine::noteDeadline(qint64 deadline) {
  if (deadline && (!nextStateDeadline || deadline < nextStateDeadline))
    nextStateDeadline = deadline;
}
void Engine::noteShelfDeadline(qint64 deadline) {
  if (deadline && (!nextShelfDeadline || deadline < nextShelfDeadline))
    nextShelfDeadline = deadline;
}
void Engine::tick() {
  retryForceWrites();
  processDeadlines(now(), monotonicNow());
  tickHeartbeat();
}
void Engine::processDeadlines(qint64 t, qint64 elapsed) {
  // A timer callback can synchronously deliver another event. Do not nest a
  // second timer dispatch inside a partially propagated alarm transition.
  if (dispatchingDeadlines) return;
  QScopedValueRollback<bool> dispatching(dispatchingDeadlines, true);
  if (silenceUntil && elapsed >= silenceUntil) setSilenceUntil(0);
  // Scan only when a deadline can have elapsed. Cancelled timers may cause
  // one harmless extra scan. Distinct interval deadlines run in time order
  // so a delayed refresh preserves their intermediate transitions.
  if (externalStateAccess || (nextStateDeadline && elapsed >= nextStateDeadline) ||
      (nextShelfDeadline && t >= nextShelfDeadline)) {
    nextStateDeadline = nextShelfDeadline = 0;
    struct Timeout { Node* node; qint64 deadline; bool filter; };
    QVector<Timeout> expired;
    const auto nodes = document.nodes();
    for (auto n : nodes) {
      const auto& s = states[n];
      if (!s.mask[Cancel] && s.filterUntil && elapsed >= s.filterUntil)
        expired.push_back({n, s.filterUntil, true});
      if (s.noAckUntil && elapsed >= s.noAckUntil)
        expired.push_back({n, s.noAckUntil, false});
    }
    // Equal millisecond deadlines intentionally retain document order instead
    // of reconstructing ALH's Xt registration order. Treat them as simultaneous:
    // the settled group severity must be the highest alarm (MAJOR when MINOR
    // and MAJOR coincide). Separate intermediate command transitions for ties
    // are order-dependent and are intentionally not guaranteed.
    std::stable_sort(expired.begin(), expired.end(), [](const Timeout& a, const Timeout& b) {
      return a.deadline < b.deadline;
    });
    for (const auto& timeout : expired) {
      auto n = timeout.node;
      auto& s = states[n];
      // Earlier callbacks can cancel or replace a later timer (e.g. Force PV
      // masks or a timed NoAck reset). Never dispatch its stale snapshot.
      if (timeout.filter && !s.mask[Cancel] && s.filterUntil == timeout.deadline) {
        s.filterUntil = 0;
        s.edgeIndex = 0;
        std::fill(s.edges.begin(), s.edges.end(), 0);
        process(n, s.pending, s.filterStarted);
      } else if (!timeout.filter && s.noAckUntil == timeout.deadline) {
        noAck(n, false);
        logOperation(n, "Set Ack after expiration of NoAck one hour timer");
      }
    }
    // Shelves have absolute wall-clock expirations, separate from interval
    // ordering. Recompute deadlines after all callbacks have changed state.
    for (auto n : nodes) {
      auto& s = states[n];
      if (s.shelf.until && t >= s.shelf.until)
        clearShelf(n, "Shelf expired");
      noteShelfDeadline(s.shelf.until);
      if (!s.mask[Cancel])
        noteDeadline(s.filterUntil);
      noteDeadline(s.noAckUntil);
    }
  }
}
int Engine::heartbeatDelay() const {
  if (!heartbeatDue || !options.global || options.passive || !pv ||
      !pv->canWrite(fields(document.root->option("HEARTBEATPV"))[0]))
    return -1;
  return int(qBound(qint64(0), heartbeatDue - monotonicNow(), qint64(std::numeric_limits<int>::max())));
}
void Engine::tickHeartbeat() {
  auto t = monotonicNow();
  if (heartbeatDue && t >= heartbeatDue) {
    auto h = fields(document.root->option("HEARTBEATPV"));
    if (pv && options.global && !options.passive) {
      if (pv->canWrite(h[0])) {
        heartbeatUnavailable = false;
        pv->put(h[0], h.size() > 2 ? h[2].toDouble() : 1, WriteKind::Heartbeat);
      } else if (!heartbeatUnavailable) {
        heartbeatUnavailable = true;
        if (error) error("Heartbeat PV not connected or writable: " + h[0]);
      }
    }
    // Keep the original cadence; skip missed beats rather than sending a burst.
    heartbeatDue += ((t - heartbeatDue) / heartbeatInterval + 1) * heartbeatInterval;
  }
}
} // namespace alh
