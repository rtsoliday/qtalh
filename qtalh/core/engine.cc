#include "core/diagnostics.h"
// Alarm transition behavior ported from ALH alLib.c. See ../../LICENSE.
#include "engine.h"
#include <QRegularExpression>
#include <alarm.h>
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
    s.severity = n->group || s.mask[Cancel] ? 0 : 4;
    s.beepThreshold = severityValue(n->option("BEEPSEVR", "MINOR"));
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
  else
    for (auto& c : n->children)
      eachChannel(c.get(), fn);
}
void Engine::monitor(Node* n) {
  if (!pv || n->group)
    return;
  if (states[n].mask[Cancel])
    return;
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
  // Groups without monitored descendants will never receive a startup event.
  // Publish their settled state after constant force calculations have run.
  QSet<Node*> awaitingEvent;
  for (auto n : document.channels())
    if (!states[n].mask[Cancel])
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
    heartbeatDue = now() + heartbeatInterval;
  }
}
void Engine::startForce(Node* n) {
  if (!pv)
    return;
  const auto force = fields(n->option("FORCEPV"));
  if (!force.isEmpty()) {
    if (force[0] != "CALC")
      pv->number(force[0], [this, n](double v) { forceValue(n, v); }, n);
    else {
      auto values = std::make_shared<std::array<double, CALCPERFORM_NARGS>>();
      for (int i = 0; i < 6; ++i) {
        auto name = n->option("FORCEPV_CALC_" + QString(QChar('A' + i)));
        bool constant = false;
        double value = name.toDouble(&constant);
        (*values)[i] = name.isEmpty() ? 0
                       : constant     ? value
                                      : std::numeric_limits<double>::quiet_NaN();
      }
      auto expression = n->option("FORCEPV_CALC").toLocal8Bit();
      auto compiled = std::make_shared<QByteArray>(INFIX_TO_POSTFIX_SIZE(expression.size() + 1), 0);
      short err = 0;
      if (postfix(expression.constData(), compiled->data(), &err)) {
        if (error)
          error("Invalid force calculation for " + n->name);
        return;
      }
      bool allReady = true;
      for (int i = 0; i < 6; ++i)
        allReady &= std::isfinite((*values)[i]);
      if (allReady) {
        double result = 0;
        if (!calcPerform(values->data(), &result, compiled->constData()))
          forceValue(n, result);
      }
      for (int i = 0; i < 6; ++i) {
        auto name = n->option("FORCEPV_CALC_" + QString(QChar('A' + i)));
        bool constant = false;
        name.toDouble(&constant);
        if (!name.isEmpty() && !constant)
          pv->number(
              name,
              [this, n, i, values, compiled](double v) {
                (*values)[i] = v;
                for (int j = 0; j < 6; ++j)
                  if (!std::isfinite((*values)[j]))
                    return;
                double result = 0;
                if (!calcPerform(values->data(), &result, compiled->constData()))
                  forceValue(n, result);
              },
              n);
      }
    }
  }
}
void Engine::stop() {
  heartbeatDue = 0;
  pendingForceAckT.clear();
  if (running && pv)
    pv->clear();
  running = false;
}
void Engine::logOperation(Node* n, const QString& s) {
  if (options.debug) debugLog(true, "operation", (n ? n->name + ": " : QString()) + s);
  if (operation)
    operation(n, s);
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
void Engine::propagate(Node* n, const State& before, ObservationCause cause) {
  auto& s = states[n];
  updatePresentation(n, before);
  int oldBeep = beep(before), newBeep = beep(s);
  for (auto p = n->parent; p; p = p->parent) {
    auto& g = states[p];
    // Initial ERROR counts describe channels still awaiting their first event.
    // Publish the first aggregate state even when those counts stay ERROR.
    bool first = !g.initialized;
    int previous = first ? 0 : g.severity;
    g.initialized = true;
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
    severityCommands(p, previous, g.severity);
    if ((first || previous != g.severity) && options.global && !options.passive && pv) {
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
    s.unack = e.acks;
    if (e.ackt >= 0)
      s.mask[AckT] = !e.ackt;
  }
  bool first = !before.initialized;
  bool transition = first || s.status != e.status || s.severity != e.severity;
  s.status = e.status;
  s.severity = e.severity;
  s.initialized = true;
  // Value-only monitors update the displayed value, not the alarm log. Global
  // acknowledgement and transient-setting changes are still alarm records.
  bool acknowledgementChanged =
      options.global && (e.acks != s.observedAcks || (e.ackt >= 0 && e.ackt != s.observedAckT));
  s.observedAcks = e.acks;
  if (e.ackt >= 0)
    s.observedAckT = e.ackt;
  if (!s.mask[Log] && before.initialized && (transition || acknowledgementChanged))
    if (alarmLog)
      alarmLog(n, s, time);
  if (transition && !s.mask[Disable]) {
    // History has one-second precision. Reuse its timestamp within that second,
    // including bursts and delayed filter events with an older timestamp.
    const qint64 second = time / 1000 - (time % 1000 < 0 ? 1 : 0);
    if (historyTimestamp.isEmpty() || second != historySecond) {
      historySecond = second;
      historyTimestamp = QDateTime::fromMSecsSinceEpoch(time).toString("dd-MMM-yyyy HH:mm:ss");
    }
    history.prepend(historyTimestamp + " " +
                    n->name + " " + statusName(s.status) + " " + severityName(s.severity) + " " +
                    s.value);
    while (history.size() > 10)
      history.removeLast();
    // The initial ERROR is a placeholder, not a previously reported alarm.
    severityCommands(n, first ? 0 : before.severity, s.severity);
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
      before.unack > 0 && s.unack == 0 && !s.mask[Ack] && !s.mask[Disable] &&
      (s.observedSeverity > 0 || (before.observedSeverity == 0 && !s.mask[AckT])))
    cause = requestedAcknowledgements.contains(n) ? ObservationCause::RequestedAcknowledgement
                                                : ObservationCause::ExternalAcknowledgement;
  if (!s.unack || s.severity == 4) requestedAcknowledgements.remove(n);
  propagate(n, before, cause);
}
void Engine::event(Node* n, Event e) {
  auto& s = states[n];
  if (s.mask[Cancel])
    return;
  qint64 time = now();
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
  if (!s.initialized || s.filterSeconds == 0) {
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
    s.filterUntil = 0;
  }
  if (s.severity != 0 && e.severity != 0) {
    process(n, e, time);
    s.filterUntil = 0;
  }
  if (s.severity == 0 && e.severity != 0 && !s.filterUntil) {
    s.filterUntil = time + 1000LL * s.filterSeconds;
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
        s.filterUntil = time + 1000LL * s.filterSeconds;
        noteDeadline(s.filterUntil);
        s.filterStarted = time;
      }
    }
  }
  if (s.filterCount > 0 && ((previous.severity == 0) != (e.severity == 0))) {
    qint64 old = s.edges[s.edgeIndex];
    if (old && time - old <= 1000LL * s.filterSeconds) {
      reset();
      process(n, e, time);
    } else {
      s.edges[s.edgeIndex] = time;
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
  logOperation(n, QString(options.global ? "Global" : "Local") + " Ack " +
                      (n->group ? "Group" : "Channel") + " (" + severityName(state(n).unack) + ")");
  eachChannel(n, [&](Node* c) {
    auto& s = states[c];
    if (s.shelf.until || !unacknowledged(s))
      return;
    if (acknowledgement)
      acknowledgement(c);
    if (options.global && pv) {
      if (pv->put(c->name, s.unack, WriteKind::Acknowledge)) requestedAcknowledgements.insert(c);
      auto a = fields(c->option("ACKPV"));
      if (a.size() == 2)
        pv->put(a[0], a[1].toDouble());
    } else {
      State old = s;
      s.unack = 0;
      propagate(c, old, ObservationCause::LocalAcknowledgement);
    }
  });
}
void Engine::setMask(Node* n, Mask requested, bool automatic) {
  applyMask(n, requested, automatic, automatic ? n : nullptr);
}
void Engine::applyMask(Node* n, Mask requested, bool automatic, Node* forceSource) {
  bool ackTFailed = false;
  eachChannel(n, [&](Node* c) {
    auto& s = states[c];
    State old = s;
    Mask mask = requested;
    // A newer mask request supersedes any deferred force setting for this channel.
    pendingForceAckT.remove(c);
    if (options.passive && (!automatic || options.global))
      mask[AckT] = old.mask[AckT];
    s.mask = mask;
    if (mask[Cancel] || mask[Disable] || mask[Ack]) requestedAcknowledgements.remove(c);
    if (options.global)
      s.mask[AckT] = old.mask[AckT];
    if (mask[Cancel]) {
      s.awaitingObservation = true;
      s.filterUntil = s.filterStarted = 0;
      s.edgeIndex = 0;
      std::fill(s.edges.begin(), s.edges.end(), 0);
      s.pending = {};
    }
    if (mask[Cancel] && !old.mask[Cancel]) {
      if (!mask[Disable]) {
        s.severity = 0;
        s.status = 0;
        s.unack = 0;
      }
    }
    if (!mask[Cancel] && old.mask[Cancel])
      monitor(c);
    // Restore a cancelled channel before writing, and defer cancellation until
    // afterward. Failed writes leave the displayed setting unchanged/retryable.
    // Before an IOC monitor arrives, the configured bit is only a placeholder.
    // An automatic force must write (or defer) even if that bit already matches.
    if (options.global && !options.passive &&
        (mask[AckT] != old.mask[AckT] || (automatic && old.observedAckT < 0)) &&
        (!pv || !pv->put(c->name, !mask[AckT], WriteKind::AckTransient))) {
      if (automatic && !options.passive)
        pendingForceAckT.insert(c, {forceSource, mask[AckT]});
      mask[AckT] = old.mask[AckT];
      ackTFailed = true;
    }
    s.mask[AckT] = mask[AckT];
    if (mask[Cancel] && !old.mask[Cancel] && pv)
      pv->cancel(c->name, c);
    // A cancelled alarm still needs a write-only connection for a pending ACKT.
    if (pendingForceAckT.contains(c) && pv)
      pv->prepare(c->name);
    if (!options.global) {
      if (mask[Ack] || mask[Disable])
        s.unack = 0;
      else if (old.mask[Ack] || old.mask[Disable])
        s.unack = s.severity;
    }
    if (old.mask[Disable] && !mask[Disable] && !mask[Cancel])
      severityCommands(c, 0, s.severity);
    if (options.global && !options.passive && pv) {
      // Clear transients suppressed while disabled or NoAck before exposing
      // them again. Keep the latch if the IOC acknowledgement cannot be sent.
      if ((old.mask[Ack] || old.mask[Disable]) && !mask[Ack] && !mask[Disable] && !mask[Cancel] &&
          s.severity == 0 && s.unack > 0 && pv->put(c->name, s.unack, WriteKind::Acknowledge)) {
        if (acknowledgement)
          acknowledgement(c);
        s.unack = 0;
        auto a = fields(c->option("ACKPV"));
        if (a.size() == 2)
          pv->put(a[0], a[1].toDouble());
        logOperation(c, "Auto ack of transient alarms on enable");
      }
      if (mask[Disable] != old.mask[Disable]) {
        auto name = c->option("SEVRPV");
        if (!name.isEmpty() && name != "-")
          pv->put(name, mask[Disable] ? -1 : s.severity, WriteKind::Severity);
      }
    }
    propagate(c, old);
  });
  logOperation(n, "Change Mask " + requested.text() +
                      (ackTFailed ? " (ACKT write failed; previous setting retained)" : ""));
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
  for (auto c : pendingForceAckT.keys()) {
    const auto pending = pendingForceAckT.value(c);
    if (!pv->canWrite(c->name) || !pv->put(c->name, !pending.mask, WriteKind::AckTransient))
      continue;
    pendingForceAckT.remove(c);
    State before = states[c];
    states[c].mask[AckT] = pending.mask;
    propagate(c, before);
    logOperation(c, "Applied deferred force ACKT setting");
  }
}
void Engine::resetMask(Node* n) {
  eachChannel(n, [&](Node* c) { setMask(c, c->mask); });
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
      for (auto& child : c->children)
        apply(child.get());
    } else {
      auto m = states[c].mask;
      m[Ack] = enabled ? true : c->mask[Ack];
      setMask(c, m);
    }
  };
  states[n].noAckUntil = 0;
  apply(n);
  states[n].noAckUntil = enabled ? now() + 3600000 : 0;
  noteDeadline(states[n].noAckUntil);
  logOperation(n, enabled ? "Set NoAck and start NoAck one hour timer"
                          : "Reset Ack mask and cancel NoAck one hour timer");
}
void Engine::forceValue(Node* n, double value) {
  // A disconnect is not a force value; retain the last valid value for NE reset.
  if (!std::isfinite(value))
    return;
  auto& forceState = states[n];
  double previous = forceState.forceCurrent;
  forceState.forceCurrent = value;
  if (forceState.forceDisabled || value == previous)
    return;
  auto f = fields(n->option("FORCEPV"));
  if (f.size() < 2)
    return;
  double forced = f.size() > 2 ? f[2].toDouble() : 1, reset = f.size() > 3 ? f[3].toDouble() : 0;
  bool ne = (f.size() > 3 && f[3] == "NE") || reset == forced;
  // Only CALC results use legacy float comparison; scalar PVs are doubles.
  auto equal = [&](double a, double b) { return f[0] == "CALC" ? float(a) == float(b) : a == b; };
  if (equal(value, forced))
    setMask(n, Mask::parse(f[1]), true);
  else if ((ne && (previous == -999 || equal(previous, forced)) && !equal(value, forced)) ||
           (!ne && equal(value, reset)))
    eachChannel(n, [&](Node* c) { applyMask(c, c->mask, true, n); });
}
void Engine::configureForce(Node* n, const QVector<Directive>& directives, bool disabled) {
  discardForceWrites(n);
  if (pv)
    pv->cancelNumbers(n);
  for (int i = n->directives.size() - 1; i >= 0; --i)
    if (n->directives[i].key.startsWith("FORCEPV"))
      n->directives.removeAt(i);
  for (const auto& d : directives)
    if (d.key.startsWith("FORCEPV"))
      n->directives.push_back(d);
  states[n].forceCurrent = -999;
  states[n].forceDisabled = disabled;
  startForce(n);
  logOperation(n, "Change Force PV " + n->option("FORCEPV"));
}
void Engine::setForceDisabled(Node* n, bool disabled) {
  if (disabled)
    discardForceWrites(n);
  states[n].forceDisabled = disabled;
  if (!disabled) {
    double v = states[n].forceCurrent;
    states[n].forceCurrent = -999;
    forceValue(n, v);
  }
  logOperation(n, disabled ? "Disable Force PV" : "Enable Force PV");
}
void Engine::rebuildBeep() {
  for (auto n : document.nodes())
    if (n->group)
      states[n].beepCounts.fill(0);
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
bool Engine::audible() const {
  if (silenceForever || silenceCurrent || now() < silenceUntil || !document.root)
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
  if (notify && newB > oldB && newB >= document.beepSeverity) silenceCurrent = false;
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
    noteDeadline(s.shelf.until);
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
    else noteDeadline(s.shelf.until);
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
void Engine::tick() {
  retryForceWrites();
  auto t = now();
  // Keep the existing expiry order and cadence, but scan only when a deadline
  // can have elapsed. Cancelled timers may cause one harmless extra scan.
  if (externalStateAccess || (nextStateDeadline && t >= nextStateDeadline)) {
    nextStateDeadline = 0;
    for (auto n : document.nodes()) {
      auto& s = states[n];
      if (!s.mask[Cancel] && s.filterUntil && t >= s.filterUntil) {
        s.filterUntil = 0;
        s.edgeIndex = 0;
        std::fill(s.edges.begin(), s.edges.end(), 0);
        process(n, s.pending, s.filterStarted);
      }
      if (s.shelf.until && t >= s.shelf.until)
        clearShelf(n, "Shelf expired");
      noteDeadline(s.shelf.until);
      if (s.noAckUntil && t >= s.noAckUntil) {
        noAck(n, false);
        logOperation(n, "Set Ack after expiration of NoAck one hour timer");
      }
      if (!s.mask[Cancel])
        noteDeadline(s.filterUntil);
      noteDeadline(s.noAckUntil);
    }
  }
  tickHeartbeat();
}
int Engine::heartbeatDelay() const {
  if (!heartbeatDue || !options.global || options.passive)
    return -1;
  return int(qBound(qint64(0), heartbeatDue - now(), qint64(std::numeric_limits<int>::max())));
}
void Engine::tickHeartbeat() {
  auto t = now();
  if (heartbeatDue && t >= heartbeatDue) {
    auto h = fields(document.root->option("HEARTBEATPV"));
    if (pv && options.global && !options.passive)
      pv->put(h[0], h.size() > 2 ? h[2].toDouble() : 1);
    // Keep the original cadence; skip missed beats rather than sending a burst.
    heartbeatDue += ((t - heartbeatDue) / heartbeatInterval + 1) * heartbeatInterval;
  }
}
} // namespace alh
