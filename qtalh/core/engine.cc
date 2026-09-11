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
void Engine::propagate(Node* n, const State& before) {
  auto& s = states[n];
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
  if (newBeep > oldBeep && newBeep >= document.beepSeverity)
    silenceCurrent = false;
  if (changed)
    changed();
}
void Engine::process(Node* n, Event e, qint64 time) {
  if (options.debug)
    debugLog(true, "alarm", QString("%1 status=%2 severity=%3 ACKS=%4 ACKT=%5 value=%6")
        .arg(n->name, statusName(e.status), severityName(e.severity))
        .arg(e.acks).arg(e.ackt).arg(e.value));
  auto& s = states[n];
  State before = s;
  e.severity = qBound(0, e.severity, 4);
  e.acks = qBound(0, e.acks, 4);
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
  propagate(n, before);
}
void Engine::event(Node* n, Event e) {
  auto& s = states[n];
  if (s.mask[Cancel])
    return;
  qint64 time = now();
  Event previous = s.pending;
  s.pending = e;
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
      process(n, held, time);
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
    if (!unacknowledged(s))
      return;
    if (acknowledgement)
      acknowledgement(c);
    if (options.global && pv) {
      pv->put(c->name, s.unack, WriteKind::Acknowledge);
      auto a = fields(c->option("ACKPV"));
      if (a.size() == 2)
        pv->put(a[0], a[1].toDouble());
    } else {
      State old = s;
      s.unack = 0;
      propagate(c, old);
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
    if (options.global)
      s.mask[AckT] = old.mask[AckT];
    if (mask[Cancel]) {
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
  logOperation(n, "Set beep severity " + severityName(v));
}
bool Engine::audible() const {
  if (silenceForever || silenceCurrent || now() < silenceUntil || !document.root)
    return false;
  return state(document.root.get()).beep >= qMax(1, document.beepSeverity);
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
