#include "test_compat.h"
#include "core/engine.h"
#include "core/model.h"
#include "services/ipc.h"
#include "services/options.h"
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <alarm.h>
#include <cstring>
#include <limits>
#ifndef Q_OS_WIN
#include <sys/msg.h>
#endif
using namespace alh;
struct FakePv : PvService {
  struct Write {
    QString name;
    double value;
    WriteKind kind;
  };
  QVector<Write> writes;
  bool writable = true;
  QSet<QString> unwritable;
  QStringList cancelled;
  QHash<QString, std::function<void(Event)>> alarms;
  QHash<QString, std::function<void(double)>> numbers;
  void monitor(const QString& n, std::function<void(Event)> f, const void* = nullptr) override {
    alarms[n] = f;
  }
  void text(const QString& n, std::function<void(QString)> f) override {
    alarms[n] = [f](Event event) { f(event.value); };
  }
  void number(const QString& n, std::function<void(double)> f, const void* = nullptr) override {
    numbers[n] = f;
  }
  void cancelNumbers(const void*) override {}
  void prepare(const QString&) override {}
  bool canWrite(const QString& n) const override {
    return writable && !unwritable.contains(n);
  }
  bool put(const QString& n, double v, WriteKind k = WriteKind::Value) override {
    writes.push_back({n, v, k});
    return writable && !unwritable.contains(n);
  }
  void cancel(const QString& n, const void* = nullptr) override {
    cancelled << n;
    alarms.remove(n);
  }
  void clear() override {
    alarms.clear();
    numbers.clear();
  }
};
struct TrackedForcePv : FakePv {
  struct Input { QString name; const void* owner; std::function<void(double)> callback; };
  QVector<Input> inputs;
  int added = 0, removed = 0;
  void number(const QString& name, std::function<void(double)> callback, const void* owner) override {
    inputs.push_back({name, owner, std::move(callback)}); ++added;
  }
  void cancelNumbers(const void* owner) override {
    for (int i = inputs.size() - 1; i >= 0; --i)
      if (inputs[i].owner == owner) { inputs.removeAt(i); ++removed; }
  }
  void send(const QString& name, double value) {
    const auto callbacks = inputs;
    for (const auto& input : callbacks) if (input.name == name) input.callback(value);
  }
  void clear() override { inputs.clear(); FakePv::clear(); }
};
class CoreTests : public QObject {
  Q_OBJECT
private slots:

  void snapshotMetadataTracksDocumentChanges() {
    auto d = parseConfig("GROUP NULL root\nGROUP root first\nCHANNEL first pv\n"
                         "GROUP root second\nCHANNEL second pv\n");
    Engine e(d);
    auto n = d.channels()[0], other = d.channels()[1];
    auto first = n->parent, second = other->parent;
    auto verify = [&](Node* node) {
      const auto update = e.channelUpdate(node);
      QCOMPARE(update.identity, Engine::nodeIdentity(node));
      QCOMPARE(update.path, Engine::channelPath(node));
      QCOMPARE(update.pv, node->name);
      QStringList ancestors;
      for (auto p = node; p; p = p->parent) ancestors << Engine::nodeIdentity(p);
      QCOMPARE(update.ancestors, ancestors);
    };
    verify(n); verify(other);
    auto retained = e.channelUpdate(n);
    QVERIFY(retained.identity != e.channelUpdate(other).identity);
    // Escaping and lengths must remain correct when any ancestor is renamed.
    d.root->name = QString::fromUtf8("root/\\é");
    verify(n); verify(other);
    first->name = "renamed/group";
    verify(n);
    n->name = "new\\pv/value";
    verify(n);
    // Same Node address, new parent: metadata must follow the new ancestry.
    auto moved = first->children.front();
    first->children.clear(); second->children.push_back(moved); n->parent = second;
    verify(n);
    second->name = "destination";
    verify(n); verify(other);
    // Changing the node kind also changes the identity encoding.
    n->group = true; verify(n); n->group = false; verify(n);
    QCOMPARE(retained.path, QString("/root/first/pv"));
    QCOMPARE(retained.identity, QString("G4:rootG5:firstC2:pv"));
    QCOMPARE(retained.ancestors, QStringList({"G4:rootG5:firstC2:pv", "G4:rootG5:first", "G4:root"}));
    // Returned implicitly shared values are independent of future snapshots.
    auto changedCopy = e.channelUpdate(n); changedCopy.ancestors.clear(); changedCopy.path = "changed";
    verify(n);
  }
  void snapshotMetadataPreservesEventState() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine e(d); auto n = d.channels()[0]; qint64 wall = 10000;
    e.now = [&] { return wall; };
    e.event(n, {0, 0, 0, 1, "initial"});
    QVector<AlarmObservation> observations;
    QVector<ChannelUpdate> compatibility;
    e.channelUpdated = [&](const ChannelUpdate& u) { compatibility << u; };
    auto observer = e.observe([&](const AlarmObservation& o) { observations << o; });
    for (int i = 0; i < 100; ++i) {
      ++wall;
      const int severity = i % 2 ? 0 : 2;
      e.event(n, {severity ? 3 : 0, severity, severity, 1, QString::number(i)});
      QCOMPARE(observations.size(), i + 1);
      QCOMPARE(compatibility.size(), i + 1);
      const auto& o = observations.last();
      QCOMPARE(o.cause, ObservationCause::Processed);
      QCOMPARE(o.before.severity, i ? (i % 2 ? 2 : 0) : 0);
      QCOMPARE(o.before.value, i ? QString::number(i - 1) : QString("initial"));
      QCOMPARE(o.after.severity, severity);
      QCOMPARE(o.after.status, severity ? 3 : 0);
      QCOMPARE(o.after.value, QString::number(i));
      QCOMPARE(o.after.observedAt, wall);
      QCOMPARE(o.before.observedAt, wall);
      QCOMPARE(compatibility.last().value, o.after.value);
      QVERIFY(o.after.initialized && o.after.available);
      QVERIFY(!o.after.suppressed);
    }
    e.acknowledge(n);
    QCOMPARE(observations.last().after.unack, 0);
    e.shelve(n, 1, "maintenance", "tester");
    QVERIFY(observations.last().after.shelved && observations.last().after.suppressed);
    e.unshelve(n);
    QVERIFY(!observations.last().after.shelved);
    e.setMask(n, Mask::parse("D"));
    QVERIFY(observations.last().after.disabled && observations.last().after.suppressed);
    e.setMask(n, Mask::parse("-"));
    QVERIFY(!observations.last().after.disabled);
    // Previously retained observations must not acquire the latest dynamic state.
    QCOMPARE(observations.first().after.value, QString("0"));
    QCOMPARE(observations.first().after.severity, 2);
    QVERIFY(!observations.first().after.shelved);
  }

  void duplicatePvManualRequestSupersedesForce_data() {
    QTest::addColumn<QString>("request");
    QTest::addColumn<QString>("first"); QTest::addColumn<QString>("second");
    for (const auto action : {"success", "failure", "no-op"})
      for (const auto first : {"shared", "shared.VAL"})
        for (const auto second : {"shared", "shared.VAL", "shared.DESC"})
          QTest::newRow(qPrintable(QString("%1-%2-%3").arg(action, first, second)))
              << QString(action) << QString(first) << QString(second);
  }
  void duplicatePvManualRequestSupersedesForce() {
    QFETCH(QString, request); QFETCH(QString, first); QFETCH(QString, second);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root " + first + "\nCHANNEL root " + second + "\n");
    FakePv pv; Engine e(d, {true}, &pv);
    auto a = d.channels()[0], b = d.channels()[1];
    e.event(a, {}); e.event(b, {});
    pv.writable = false;
    e.setMask(a, Mask::parse("T"), true);
    if (request == "success") {
      pv.writable = true;
      e.setMask(b, Mask::parse("T"));
      e.setMask(b, Mask::parse("-"));
      QCOMPARE(pv.writes.last().value, 1.0);
    } else {
      e.setMask(b, Mask::parse(request == "failure" ? "T" : "-"));
    }
    QVERIFY(!e.state(b).mask[AckT]);
    pv.writes.clear(); pv.writable = true; e.tick();
    QVERIFY(pv.writes.isEmpty()); // The other row's old force must never reappear.
    QVERIFY(!e.state(a).mask[AckT]);
    if (request == "failure") {
      e.setMask(b, Mask::parse("T")); // A failed manual request still needs explicit retry.
      QCOMPARE(pv.writes.size(), 1); QCOMPARE(pv.writes[0].value, 0.0);
    }
  }
  void recordFieldForceOwnership() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root shared\nCHANNEL root shared.VAL\nCHANNEL root other.VAL\n");
    FakePv pv; pv.writable = false; Engine e(d, {true}, &pv);
    auto a = d.channels()[0], b = d.channels()[1], other = d.channels()[2];
    e.setMask(a, Mask::parse("-"), true);
    e.setMask(b, Mask::parse("T"), true);
    e.setMask(other, Mask::parse("-"), true);
    e.setForceDisabled(a, true); // The superseded source no longer owns the shared request.
    pv.writes.clear(); pv.writable = true; e.tick();
    QCOMPARE(pv.writes.size(), 2);
    QCOMPARE(pv.writes[0].name, b->name); QCOMPARE(pv.writes[0].value, 0.0);
    QCOMPARE(pv.writes[1].name, other->name); QCOMPARE(pv.writes[1].value, 1.0);
    QVERIFY(e.state(b).mask[AckT]); QVERIFY(!e.state(a).mask[AckT]);
  }
  void duplicatePvForceOwnership() {
    for (bool edit : {false, true}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root shared\nCHANNEL root shared\n");
      FakePv pv; pv.writable = false; Engine e(d, {true}, &pv);
      auto a = d.channels()[0], b = d.channels()[1];
      e.setMask(a, Mask::parse("-"), true);
      e.setMask(b, Mask::parse("T"), true);
      if (edit) e.configureForce(a, {{"FORCEPV", "replacement D 1 0"}}, false);
      else e.setForceDisabled(a, true);
      pv.writes.clear(); pv.writable = true; e.tick();
      QCOMPARE(pv.writes.size(), 1); QCOMPARE(pv.writes[0].value, 0.0);
      QVERIFY(e.state(b).mask[AckT]);
      // Let the IOC confirm both rows, then change it externally. No replay.
      e.event(a, {0, 0, 0, 0, "0"}); e.event(b, {0, 0, 0, 0, "0"});
      e.event(a, {}); e.event(b, {}); e.tick();
      QCOMPARE(pv.writes.size(), 1);
      QVERIFY(!e.state(a).mask[AckT]); QVERIFY(!e.state(b).mask[AckT]);
    }
  }
  void duplicatePvDeferredGroupReset() {
    for (bool saved : {false, true}) for (bool cancelled : {false, true}) {
      const QString cancel = cancelled ? "C" : "";
      auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch shared " + cancel +
          "T\nCHANNEL root shared " + cancel + "\nCHANNEL root separate " + cancel + "T\n");
      if (saved) d = parseConfig(writeConfig(d));
      FakePv pv; pv.writable = false; Engine e(d, {true}, &pv);
      e.resetMask(d.root.get(), true);
      pv.writes.clear(); pv.writable = true; e.tick();
      // Direct channels precede the subgroup, which supplies shared's final bit.
      QCOMPARE(pv.writes.size(), 2);
      QCOMPARE(pv.writes[0].name, QString("separate"));
      QCOMPARE(pv.writes[1].name, QString("shared"));
      QCOMPARE(pv.writes[0].value, 0.0); QCOMPARE(pv.writes[1].value, 0.0);
    }
  }
  void deferredAckTRetryHonorsNewRequestsDuringCallbacks() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root first\nCHANNEL root second\n");
    FakePv pv; pv.writable = false; Engine e(d, {true}, &pv);
    auto a = d.channels()[0], b = d.channels()[1];
    e.event(a, {}); e.event(b, {});
    e.setMask(d.root.get(), Mask::parse("T"), true);
    bool replaced = false;
    e.operation = [&](Node* n, const QString& message, OperationKind) {
      if (n == a && message == "Applied deferred force ACKT setting") {
        replaced = true;
        e.setMask(b, Mask::parse("-")); // Supersede the next snapshotted request.
      }
    };
    pv.writes.clear(); pv.writable = true; e.tick();
    QVERIFY(replaced); QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(pv.writes[0].name, QString("first"));
    QVERIFY(!e.state(b).mask[AckT]);
  }

  void calculationAssignmentsKeepObservedInputsSeparate() {
    for (bool group : {false, true}) for (bool global : {false, true}) {
      const QString force = "$FORCEPV CALC D 1 0\n$FORCEPV_CALC A:=A+1;B:=B+1;B%2\n"
                            "$FORCEPV_CALC_A source\n$FORCEPV_CALC_C other\n$FORCEPV_CALC_D 10\n";
      auto d = parseConfig("GROUP NULL root\n" + (group ? force : QString()) +
          "CHANNEL root pv\n" + (group ? QString() : force));
      FakePv pv; Engine e(d, {global}, &pv); e.start();
      auto n = d.channels()[0], target = group ? d.root.get() : n;
      pv.numbers["source"](5); QCOMPARE(e.state(target).forceCurrent, -999.0);
      pv.numbers["other"](8); QVERIFY(e.state(n).mask[Disable]); // A=6, B=1.
      pv.numbers["source"](5); QVERIFY(e.state(n).mask[Disable]); // Duplicate, not A=6.
      e.configureForce(target, target->directives, false);
      pv.numbers["source"](6); QVERIFY(!e.state(n).mask[Disable]); // Real change, B=2.
      pv.numbers["source"](6); QVERIFY(!e.state(n).mask[Disable]);
      pv.numbers["other"](9); QVERIFY(e.state(n).mask[Disable]); // B=3, retain/increment A=8.
      auto edited = target->directives;
      for (auto& directive : edited)
        if (directive.key == "FORCEPV_CALC") directive.value = "A+D";
      e.configureForce(target, edited, false);
      QCOMPARE(e.state(target).forceCurrent, 18.0); // Do not reload A=6 on expression edits.
      pv.numbers["source"](6); QCOMPARE(e.state(target).forceCurrent, 18.0);
      pv.numbers["source"](std::numeric_limits<double>::quiet_NaN());
      pv.numbers["source"](6); QCOMPARE(e.state(target).forceCurrent, 16.0); // Recovery is fresh.
      pv.numbers["source"](6); QCOMPARE(e.state(target).forceCurrent, 16.0);
    }
  }

  void calculationRecoversNonfiniteWorkspace_data() {
    QTest::addColumn<QString>("nonfinite");
    QTest::newRow("nan") << "SQRT(-1)";
    QTest::newRow("infinity") << "EXP(1000)";
  }
  void calculationRecoversNonfiniteWorkspace() {
    QFETCH(QString, nonfinite);
    for (bool group : {false, true}) for (bool global : {false, true})
      for (bool nonfiniteResult : {false, true}) {
        const QString force = "$FORCEPV CALC D 1 0\n$FORCEPV_CALC B:=A?" + nonfinite +
            ":0;" + (nonfiniteResult ? "B" : "A") + "\n$FORCEPV_CALC_A source\n"
            "$FORCEPV_CALC_C other\n";
        auto d = parseConfig("GROUP NULL root\n" + (group ? force : QString()) +
            "CHANNEL root pv\n" + (group ? QString() : force));
        FakePv pv; Engine e(d, {global}, &pv); e.start();
        auto n = d.channels()[0], target = group ? d.root.get() : n;
        pv.numbers["source"](1);
        QCOMPARE(e.state(target).forceCurrent, -999.0); // Await all named inputs.
        pv.numbers["other"](7);
        if (nonfiniteResult) {
          QCOMPARE(e.state(target).forceCurrent, -999.0);
          QVERIFY(!e.state(n).mask[Disable]); // Never force from a NaN/Inf result.
          e.setMask(n, Mask::parse("D"));
        } else {
          QCOMPARE(e.state(target).forceCurrent, 1.0);
          QVERIFY(e.state(n).mask[Disable]);
        }
        // External input outages must still block evaluation and cached replay.
        pv.numbers["other"](std::numeric_limits<double>::quiet_NaN());
        pv.numbers["source"](0);
        QVERIFY(e.state(n).mask[Disable]);
        pv.numbers["other"](7);
        QCOMPARE(e.state(target).forceCurrent, 0.0);
        QVERIFY(!e.state(n).mask[Disable]); // Re-evaluation repairs B and resets.
      }
  }

  void forceApplyPreservesStatefulCalculation() {
    for (bool group : {false, true}) for (bool global : {false, true}) {
      const QString force = "$FORCEPV CALC D 1 0\n$FORCEPV_CALC B:=B+1;B<2\n"
                            "$FORCEPV_CALC_A source\n";
      auto d = parseConfig("GROUP NULL root\n" + (group ? force : QString()) +
          "CHANNEL root pv\n$SEVRPV output\n$SEVRCOMMAND UP_ANY up\n"
          "$SEVRCOMMAND DOWN_ANY down\n" + (group ? QString() : force));
      auto n = d.channels()[0], target = group ? d.root.get() : n;
      TrackedForcePv pv; Engine e(d, {global}, &pv); e.start();
      e.event(n, {3, 2, 2, 1, "alarm"});
      pv.send("source", 1); QVERIFY(e.state(n).mask[Disable]);
      pv.send("source", 2); QVERIFY(!e.state(n).mask[Disable]);
      int logs = 0, commands = 0, operations = 0;
      e.alarmLog = [&](Node*, const State&, qint64) { ++logs; };
      e.command = [&](const QString&) { ++commands; };
      e.operation = [&](Node*, const QString&, OperationKind) { ++operations; };
      pv.writes.clear(); const auto history = e.history;
      // Passing the node's own vector must also be safe.
      e.configureForce(target, target->directives, false);
      auto reordered = target->directives;
      std::reverse(reordered.begin(), reordered.end());
      e.configureForce(target, reordered, false); // Editor order need not match the file.
      pv.send("source", 2);
      QVERIFY(!e.state(n).mask[Disable]); QCOMPARE(pv.added, 1); QCOMPARE(pv.removed, 0);
      QCOMPARE(logs, 0); QCOMPARE(commands, 0); QCOMPARE(operations, 0);
      QVERIFY(pv.writes.isEmpty()); QCOMPARE(e.history, history);
      // Settings-only edits use the cached result, with no extra B increment.
      e.setForceDisabled(target, true); e.setForceDisabled(target, false);
      auto changed = target->directives;
      for (auto& directive : changed) if (directive.key == "FORCEPV_CALC") directive.value = "B";
      e.configureForce(target, changed, false);
      QCOMPARE(e.state(target).forceCurrent, 2.0);
      QCOMPARE(pv.added, 1); QCOMPARE(pv.removed, 0);
    }
  }
  void forceEditsRetainInputsAndRejectStaleCallbacks() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV CALC D 2 NE\n"
        "$FORCEPV_CALC B:=B+1;B\n$FORCEPV_CALC_A source\n$FORCEPV_CALC_C other\n");
    auto n = d.channels()[0]; TrackedForcePv pv; Engine e(d, {}, &pv); e.start();
    auto oldCallback = pv.inputs[0].callback;
    pv.send("source", 1); QCOMPARE(e.state(n).forceCurrent, -999.0);
    pv.send("other", 9); QCOMPARE(e.state(n).forceCurrent, 1.0);
    pv.send("source", 2); QCOMPARE(e.state(n).forceCurrent, 2.0);
    QVERIFY(e.state(n).mask[Disable]);
    auto changed = n->directives;
    for (auto& directive : changed) if (directive.key == "FORCEPV") directive.value = "CALC A 2 NE";
    e.configureForce(n, changed, false);
    QCOMPARE(e.state(n).forceCurrent, 2.0); QVERIFY(e.state(n).mask[Ack]);
    QVERIFY(!e.state(n).mask[Disable]); QCOMPARE(pv.added, 2); QCOMPARE(pv.removed, 0);
    for (auto& directive : changed) if (directive.key == "FORCEPV_CALC_A") directive.value = "replacement";
    e.configureForce(n, changed, false);
    QCOMPARE(pv.added, 3); QCOMPARE(pv.removed, 1);
    oldCallback(88); QCOMPARE(e.state(n).forceCurrent, -999.0);
    pv.send("replacement", 4); QCOMPARE(e.state(n).forceCurrent, 3.0); // B and C survived.
    QVERIFY(!e.state(n).mask[Ack]);
    pv.send("other", 9); QCOMPARE(e.state(n).forceCurrent, 3.0);
    pv.send("other", std::numeric_limits<double>::quiet_NaN());
    for (auto& directive : changed) if (directive.key == "FORCEPV") directive.value = "CALC D 4 NE";
    e.configureForce(n, changed, false);
    QVERIFY(!e.state(n).mask[Disable]);
    pv.send("replacement", 5); QVERIFY(!e.state(n).mask[Disable]);
    pv.send("other", 9); QCOMPARE(e.state(n).forceCurrent, 4.0);
    QVERIFY(e.state(n).mask[Disable]);
    for (auto& directive : changed) if (directive.key == "FORCEPV_CALC") directive.value = "B:=B+10;B";
    e.configureForce(n, changed, false); QCOMPARE(e.state(n).forceCurrent, 14.0);
    QCOMPARE(pv.added, 3); QCOMPARE(pv.removed, 1);
    for (auto& directive : changed) if (directive.key == "FORCEPV_CALC_C") directive.value = "7";
    e.configureForce(n, changed, false); QCOMPARE(e.state(n).forceCurrent, 24.0);
    QCOMPARE(pv.added, 3); QCOMPARE(pv.removed, 2);
    auto lastCallback = pv.inputs[0].callback;
    e.stop(); lastCallback(100); QCOMPARE(e.state(n).forceCurrent, 24.0);
    QVERIFY(pv.inputs.isEmpty());
  }
  void forceInvalidExpressionRecovery() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    auto n = d.channels()[0]; TrackedForcePv pv; Engine e(d, {}, &pv); e.start();
    int errors = 0; e.error = [&](const QString&) { ++errors; };
    QVector<Directive> changed{{"FORCEPV", "CALC D 2 NE"},
        {"FORCEPV_CALC", "("}, {"FORCEPV_CALC_A", "source"}};
    e.configureForce(n, changed, false); QCOMPARE(errors, 1);
    pv.send("source", 1); QCOMPARE(e.state(n).forceCurrent, -999.0);
    changed[1].value = "B:=B+1;B";
    e.configureForce(n, changed, false); QCOMPARE(e.state(n).forceCurrent, 1.0);
    changed[1].value = "(";
    e.configureForce(n, changed, false); QCOMPARE(errors, 2);
    pv.send("source", 2); QCOMPARE(e.state(n).forceCurrent, -999.0);
    changed[1].value = "B:=B+1;B";
    e.configureForce(n, changed, false); QCOMPARE(e.state(n).forceCurrent, 2.0);
    QVERIFY(e.state(n).mask[Disable]);
    QCOMPARE(pv.added, 1); QCOMPARE(pv.removed, 0);
  }
  void forceEnableWaitsForRecovery() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV gate D 1 NE\n");
    auto n = d.channels()[0]; TrackedForcePv pv; Engine e(d, {}, &pv); e.start();
    pv.send("gate", 1); QVERIFY(e.state(n).mask[Disable]);
    e.configureForce(n, n->directives, true);
    e.resetMask(n); QVERIFY(!e.state(n).mask[Disable]);
    pv.send("gate", std::numeric_limits<double>::quiet_NaN());
    e.configureForce(n, n->directives, false);
    QVERIFY(!e.state(n).mask[Disable]);
    pv.send("gate", 1); QVERIFY(e.state(n).mask[Disable]);
    QCOMPARE(pv.added, 1); QCOMPARE(pv.removed, 0);
  }
  void forceUnchangedApplyRetainsDeferredAckT() {
    auto d = parseConfig("GROUP NULL root\n$FORCEPV CALC T 1 NE\n$FORCEPV_CALC 1\nCHANNEL root pv\n");
    FakePv pv; pv.writable = false; Engine e(d, {true}, &pv); e.start();
    const int attempts = pv.writes.size(); QVERIFY(attempts > 0);
    e.configureForce(d.root.get(), d.root->directives, false);
    QCOMPARE(pv.writes.size(), attempts);
    pv.writable = true; e.tick(); QCOMPARE(pv.writes.size(), attempts + 1);
    QVERIFY(e.state(d.channels()[0]).mask[AckT]);
  }
  void filteredTransientRecovery() {
    for (bool global : {false, true}) for (int count : {-1, 0, 1})
      for (int status : {int(ALARM_NSTATUS), ALARM_NSTATUS + 1, ALARM_NSTATUS + 2}) {
        auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER " +
            QString::number(count) + " 10\n$SEVRPV severity\n"
            "$SEVRCOMMAND UP_ANY up\n$SEVRCOMMAND DOWN_ANY down\n");
        FakePv pv; Engine e(d, {global}, &pv); auto n = d.channels()[0];
        qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
        e.event(n, {3, 2, 2, 1, "alarm"});
        e.event(n, {0, 0, 2, 1, "clear"});
        time += 10000; e.tick();
        QCOMPARE(e.state(n).severity, 0); QCOMPARE(e.state(n).unack, 2);
        QVERIFY(e.channelUpdate(n).available);
        const auto history = e.history;
        int logs = 0, commands = 0, changes = 0;
        e.alarmLog = [&](Node*, const State&, qint64) { ++logs; };
        e.command = [&](QString) { ++commands; };
        e.changed = [&] { ++changes; };
        QVector<AlarmObservation> observations;
        auto observer = e.observe([&](const AlarmObservation& o) { observations << o; });
        pv.writes.clear();
        e.event(n, {status, 4, 0, -1, "error"});
        QVERIFY(!e.channelUpdate(n).available);
        time += 100;
        e.event(n, {0, 0, 2, 1, "recovered"});
        QVERIFY(e.channelUpdate(n).available);
        QCOMPARE(e.state(n).value, QString("recovered"));
        QCOMPARE(e.state(n).filterUntil, qint64(0));
        QCOMPARE(e.state(n).unack, 2);
        QCOMPARE(observations.size(), 2);
        QVERIFY(!observations.last().before.available);
        QVERIFY(observations.last().after.available);
        QCOMPARE(observations.last().cause, ObservationCause::Processed);
        QVERIFY(changes > 0);
        e.event(n, {0, 0, 2, 1, "updated"});
        QCOMPARE(e.state(n).value, QString("updated"));
        time += 20000; e.tick();
        QVERIFY(e.channelUpdate(n).available);
        QCOMPARE(e.state(n).severity, 0); QCOMPARE(e.state(n).unack, 2);
        QCOMPARE(e.history, history); QCOMPARE(logs, 0); QCOMPARE(commands, 0);
        QVERIFY(pv.writes.isEmpty());
      }
  }
  void filteredPulseStillSuppressesTransient() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 0 10\n");
    Engine e(d, {true}); auto n = d.channels()[0];
    qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    e.event(n, {});
    e.event(n, {3, 2, 2, 1, "short alarm"});
    time += 100; e.event(n, {0, 0, 2, 1, "clear"});
    QCOMPARE(e.state(n).value, QString("clear"));
    time += 20000; e.tick();
    QCOMPARE(e.state(n).severity, 0); QCOMPARE(e.state(n).unack, 0);
    QVERIFY(!e.audible());
  }
  void calculationIgnoresDuplicateInputs() {
    for (bool group : {false, true}) {
      const QString force = "$FORCEPV CALC D 1 0\n$FORCEPV_CALC B:=B+1;B%2\n"
                            "$FORCEPV_CALC_A source\n$FORCEPV_CALC_C other\n";
      auto d = parseConfig("GROUP NULL root\n" + (group ? force : QString()) +
          "CHANNEL root pv\n" + (group ? QString() : force));
      FakePv pv; Engine e(d, {}, &pv); auto n = d.channels()[0];
      e.start();
      pv.numbers["source"](5);
      QVERIFY(!e.state(n).mask[Disable]); // Wait for every named input.
      pv.numbers["other"](8);
      QVERIFY(e.state(n).mask[Disable]); // B=1.
      pv.numbers["source"](5);
      QVERIFY(e.state(n).mask[Disable]); // Duplicate callbacks must not advance B.
      pv.numbers["other"](8);
      QVERIFY(e.state(n).mask[Disable]);
      pv.numbers["source"](6);
      QVERIFY(!e.state(n).mask[Disable]); // B=2.
      const auto unavailable = std::numeric_limits<double>::quiet_NaN();
      pv.numbers["source"](unavailable); pv.numbers["other"](unavailable);
      pv.numbers["source"](6);
      QVERIFY(!e.state(n).mask[Disable]); // Still waiting for the other input.
      pv.numbers["other"](8);
      QVERIFY(e.state(n).mask[Disable]); // Same values after a gap must evaluate: B=3.
      pv.numbers["source"](6); pv.numbers["other"](8);
      QVERIFY(e.state(n).mask[Disable]);
    }
  }

  void globalCommunicationErrors() {
    for (int status : {int(ALARM_NSTATUS), ALARM_NSTATUS + 1, ALARM_NSTATUS + 2}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ACKPV ack 7\n");
      FakePv pv; Engine e(d, {true}, &pv); auto n = d.channels()[0];
      int accepted = 0; e.acknowledgement = [&](Node*) { ++accepted; };
      e.event(n, {3, 2, 2, 1, "major"});
      e.event(n, {status, 4, 0, -1, "0"});
      QCOMPARE(e.state(n).unack, 4); QCOMPARE(e.state(d.root.get()).unack, 4);
      QCOMPARE(e.state(n).observedAcks, 2); QVERIFY(e.audible());
      QVERIFY(!e.channelUpdate(n).available);
      e.acknowledge(d.root.get()); // A local error acknowledgement never writes ACKS=4.
      QCOMPARE(e.state(n).unack, 2); QCOMPARE(accepted, 0); QVERIFY(pv.writes.isEmpty());
      e.event(n, {status, 4, 0, -1, "0"});
      QCOMPARE(e.state(n).unack, 2); // Repeated callbacks do not relatch the same error.
      e.event(n, {3, 2, 2, 1, "major"});
      e.event(n, {status, 4, 0, -1, "0"});
      QCOMPARE(e.state(n).unack, 4); // A new outage does relatch it.
      e.event(n, {3, 2, 2, 1, "major"});
      QCOMPARE(e.state(n).unack, 4); // Preserve a transient error with ACKT enabled.
      e.acknowledge(n);
      QCOMPARE(pv.writes.size(), 2); QCOMPARE(pv.writes[0].value, 2.0);
      QCOMPARE(pv.writes[1].name, QString("ack")); QCOMPARE(accepted, 1);
      QCOMPARE(e.state(n).unack, 2); // IOC confirmation is still required.
      e.event(n, {3, 2, 0, 1, "major"});
      QCOMPARE(e.state(n).unack, 0); QVERIFY(!e.audible());
    }
  }
  void globalCommunicationErrorMasksAndRecovery() {
    for (bool noAckT : {false, true}) for (bool passive : {false, true}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root missing " + QString(noAckT ? "T" : "-") + "\n");
      FakePv pv; Engine e(d, {true, passive}, &pv); auto n = d.channels()[0];
      e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
      QCOMPARE(e.state(n).unack, 4); QVERIFY(e.audible());
      e.acknowledge(n); QCOMPARE(e.state(n).unack, passive ? 4 : 0);
      QVERIFY(pv.writes.isEmpty());
      e.event(n, {0, 0, 0, !noAckT, "0"});
      e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
      e.setMask(n, Mask::parse(noAckT ? "DT" : "D")); QVERIFY(!e.audible());
      e.resetMask(n); QVERIFY(e.audible());
      e.shelve(n, 1, "maintenance", "tester"); QVERIFY(!e.audible());
      e.unshelve(n); QVERIFY(e.audible());
      e.event(n, {0, 0, 0, !noAckT, "0"});
      QCOMPARE(e.state(n).unack, noAckT ? 0 : 4);
      e.setMask(n, Mask::parse("C"));
      QCOMPARE(e.state(n).unack, 0); QVERIFY(!e.state(n).communicationUnack);
      e.resetMask(n); e.event(n, {0, 0, 0, !noAckT, "0"});
      QCOMPARE(e.state(n).unack, 0);
    }
  }
  void failedGlobalAcknowledgementHasNoSuccessSideEffects() {
    for (bool group : {false, true}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root failed\n$ACKPV failed-ack 1\n"
          "GROUP root branch\nCHANNEL branch success\n$ACKPV success-ack 2\n");
      FakePv pv; Engine e(d, {true}, &pv);
      auto failed = d.channels()[0], success = d.channels()[1];
      e.event(failed, {3, 2, 2, 1, "20"}); e.event(success, {3, 2, 2, 1, "20"});
      pv.unwritable.insert("failed"); QStringList accepted, failures;
      int summaries = 0;
      e.acknowledgement = [&](Node* n) { accepted << n->name; };
      e.operation = [&](Node*, const QString& text, OperationKind kind) {
        if (text.contains("ACKS request was not submitted")) failures << text;
        if (kind == OperationKind::AckGroup || kind == OperationKind::AckChannel) ++summaries;
      };
      e.acknowledge(group ? d.root.get() : failed);
      QCOMPARE(failures.size(), 1); QCOMPARE(summaries, group ? 1 : 0);
      QCOMPARE(accepted, group ? QStringList({"success"}) : QStringList());
      QCOMPARE(e.state(failed).unack, 2); QCOMPARE(e.state(success).unack, 2);
      QCOMPARE(pv.writes.size(), group ? 3 : 1);
      for (const auto& write : pv.writes) QVERIFY(write.name != "failed-ack");
      pv.unwritable.clear(); pv.writes.clear(); accepted.clear();
      e.acknowledge(failed);
      QCOMPARE(pv.writes.size(), 2); QCOMPARE(pv.writes.last().name, QString("failed-ack"));
      QCOMPARE(accepted, QStringList({"failed"})); QCOMPARE(e.state(failed).unack, 2);
    }
  }
  void topLevelNullParentsMatchIncludes() {
    const QString body = "GROUP NULL branch\n$BEEPSEVR MAJOR\n$FORCEPV gate D 1 0\n"
        "CHANNEL branch direct\nGROUP NULL leaf\nCHANNEL leaf nested\n";
    QTemporaryDir dir; QFile include(dir.filePath("part")); QVERIFY(include.open(QIODevice::WriteOnly));
    include.write(body.toUtf8()); include.close();
    auto direct = parseConfig("GROUP NULL root\n" + body + "CHANNEL root outside\n");
    auto included = parseConfig("GROUP NULL root\nINCLUDE root part\nCHANNEL root outside\n", dir.path());
    QCOMPARE(writeConfig(direct), writeConfig(included));
    QCOMPARE(writeConfig(parseConfig(writeConfig(direct))), writeConfig(direct));
    auto branch = direct.root->children[0].get();
    QCOMPARE(branch->children[1]->name, QString("leaf"));
    Engine e(direct); auto nested = branch->children[1]->children[0].get();
    e.event(nested, {4, 1, 1, 1, "minor"}); QVERIFY(!e.audible());
    e.event(nested, {3, 2, 2, 1, "major"}); QVERIFY(e.audible());
    e.forceValue(branch, 1);
    for (auto n : direct.channels()) QCOMPARE(e.state(n).mask[Disable], n->name != "outside");
  }

  void filterExpiryBeforeIncomingRecovery_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("duration");
    for (bool global : {false, true}) for (int count : {-1, 0, 1})
      for (int duration : {999, 1000, 1050})
        QTest::newRow(qPrintable(QString("global-%1-count-%2-duration-%3")
            .arg(global).arg(count).arg(duration))) << global << count << duration;
  }
  void filterExpiryBeforeIncomingRecovery() {
    QFETCH(bool, global); QFETCH(int, count); QFETCH(int, duration);
    auto d = parseConfig(QString("GROUP NULL root\n$SEVRPV group-output\n"
        "CHANNEL root pv\n$SEVRPV channel-output\n$ALARMCOUNTFILTER %1 1\n"
        "$SEVRCOMMAND UP_MAJOR alarm-command\n").arg(count));
    FakePv pv; Engine e(d, {global}, &pv); auto n = d.channels()[0];
    qint64 elapsed = 10000, wall = 100000;
    e.monotonicNow = [&] { return elapsed; }; e.now = [&] { return wall; };
    e.event(n, {}); pv.writes.clear();
    QStringList commands, values; QVector<int> severities; QVector<qint64> times;
    e.command = [&](QString command) { commands << command; };
    e.alarmLog = [&](Node*, const State& state, qint64 time) {
      severities << state.severity; values << state.value; times << time;
    };
    e.event(n, {3, 2, 2, 1, "major"});
    // No tick between the alarm and recovery. Keep a backward wall-clock jump
    // independent of the monotonic filter deadline and its saved log timestamp.
    elapsed += duration; wall = 50000;
    e.event(n, {0, 0, 2, 1, "recovered"});
    if (duration < 1000) {
      QCOMPARE(e.state(n).unack, 0); QVERIFY(commands.isEmpty());
      QVERIFY(severities.isEmpty()); QVERIFY(pv.writes.isEmpty());
    } else {
      QCOMPARE(e.state(n).unack, 2);
      QCOMPARE(commands, QStringList({"alarm-command"}));
      QCOMPARE(severities, count == -1 ? QVector<int>({2, 0}) : QVector<int>({2}));
      QCOMPARE(values.front(), QString("major")); QCOMPARE(times.front(), qint64(100000));
      QCOMPARE(pv.writes.size(), global ? (count == -1 ? 4 : 2) : 0);
      if (global) {
        QCOMPARE(pv.writes[0].name, QString("channel-output"));
        QCOMPARE(pv.writes[1].name, QString("group-output"));
        QCOMPARE(pv.writes[0].value, 2.0); QCOMPARE(pv.writes[1].value, 2.0);
      }
    }
    elapsed += 1000; wall += 1000; e.tick();
    QCOMPARE(e.state(n).severity, 0); QCOMPARE(e.state(d.root.get()).severity, 0);
    QCOMPARE(e.state(n).unack, duration < 1000 ? 0 : 2);
    QCOMPARE(severities, duration < 1000 ? QVector<int>() : QVector<int>({2, 0}));
    QCOMPARE(commands.size(), duration < 1000 ? 0 : 1);
    const int logged = severities.size(); e.tick(); QCOMPARE(severities.size(), logged);
  }

  void expiredRecoveryBeforeNextAlarm() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 0 1\n"
        "$SEVRCOMMAND DOWN_NO_ALARM clear-command\n$SEVRCOMMAND UP_MAJOR alarm-command\n");
    Engine e(d); auto n = d.channels()[0]; qint64 elapsed = 1000;
    e.now = e.monotonicNow = [&] { return elapsed; };
    e.event(n, {3, 2, 2, 1, "initial"});
    QStringList commands, values;
    e.command = [&](QString command) { commands << command; };
    e.alarmLog = [&](Node*, const State& state, qint64) { values << state.value; };
    e.event(n, {0, 0, 2, 1, "recovered"});
    elapsed = 2050; e.event(n, {3, 2, 2, 1, "next alarm"});
    QCOMPARE(e.state(n).severity, 0);
    QCOMPARE(values, QStringList({"recovered"}));
    QCOMPARE(commands, QStringList({"clear-command"}));
    QCOMPARE(e.state(n).filterUntil, qint64(3050));
    elapsed = 3050; e.tick();
    QCOMPARE(values, QStringList({"recovered", "next alarm"}));
    QCOMPARE(commands, QStringList({"clear-command", "alarm-command"}));
  }

  void incomingEventDispatchesEarlierDeadlines() {
    auto d = parseConfig("GROUP NULL root\n$SEVRCOMMAND UP_MINOR minor-command\n"
        "$SEVRCOMMAND UP_MAJOR major-command\nCHANNEL root later\n$ALARMCOUNTFILTER 0 1\n"
        "CHANNEL root earlier\n$ALARMCOUNTFILTER 0 1\n");
    Engine e(d); auto later = d.channels()[0], earlier = d.channels()[1];
    qint64 elapsed = 1000; e.now = e.monotonicNow = [&] { return elapsed; };
    e.event(later, {}); e.event(earlier, {});
    e.noAck(earlier, true); e.mutableState(earlier).noAckUntil = 1950;
    QStringList commands, records;
    e.command = [&](QString command) { commands << command; };
    e.alarmLog = [&](Node* n, const State& state, qint64) {
      records << n->name; QVERIFY(!state.mask[Ack]);
    };
    e.event(earlier, {4, 1, 1, 1, "minor"});
    elapsed = 1050; e.event(later, {3, 2, 2, 1, "major"});
    elapsed = 2100; e.event(later, {0, 0, 2, 1, "recovered"});
    QCOMPARE(records, QStringList({"earlier", "later"}));
    QCOMPARE(commands, QStringList({"minor-command", "major-command"}));
    QCOMPARE(e.state(earlier).unack, 1); QCOMPARE(e.state(later).unack, 2);
    e.tick(); QCOMPARE(records.size(), 2);
  }

  void earlierDeadlineCanCancelIncomingChannel() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root later\n$ALARMCOUNTFILTER 0 1\n"
        "CHANNEL root earlier\n$ALARMCOUNTFILTER 0 1\n$SEVRCOMMAND UP_MINOR cancel-later\n");
    Engine e(d); auto later = d.channels()[0], earlier = d.channels()[1];
    qint64 elapsed = 1000; e.now = e.monotonicNow = [&] { return elapsed; };
    e.event(later, {}); e.event(earlier, {});
    e.command = [&](QString command) {
      if (command == "cancel-later") e.setMask(later, Mask::parse("C"));
    };
    QStringList records;
    e.alarmLog = [&](Node* n, const State&, qint64) { records << n->name; };
    e.event(earlier, {4, 1, 1, 1, "minor"});
    elapsed = 1050; e.event(later, {3, 2, 2, 1, "major"});
    elapsed = 2100; e.event(later, {3, 2, 2, 1, "new value"});
    QCOMPARE(records, QStringList({"earlier"}));
    QVERIFY(e.state(later).mask[Cancel]); QCOMPARE(e.state(later).severity, 0);
    QCOMPARE(e.state(later).filterUntil, qint64(0));
    e.tick(); QCOMPARE(records.size(), 1);
  }

  void expiredFiltersFollowDeadlines() {
    auto d = parseConfig("GROUP NULL root\n$SEVRPV group-output\n"
        "$SEVRCOMMAND UP_MINOR minor-command\n$SEVRCOMMAND UP_MAJOR major-command\n"
        "CHANNEL root later\n$ALARMCOUNTFILTER 0 1\n"
        "CHANNEL root earlier\n$ALARMCOUNTFILTER 0 1\n");
    FakePv pv; Engine e(d, {true}, &pv);
    qint64 elapsed = 1000; e.now = e.monotonicNow = [&] { return elapsed; };
    auto later = d.channels()[0], earlier = d.channels()[1];
    e.event(later, {}); e.event(earlier, {}); pv.writes.clear();
    QStringList commands, records; QVector<qint64> timestamps;
    e.command = [&](QString command) { commands << command; };
    e.alarmLog = [&](Node* n, const State&, qint64 time) {
      records << n->name; timestamps << time;
    };
    e.event(earlier, {4, 1, 1, 1, "minor"});
    elapsed = 1050; e.event(later, {3, 2, 2, 1, "major"});
    elapsed = 2100; e.tick();
    QCOMPARE(commands, QStringList({"minor-command", "major-command"}));
    QCOMPARE(records, QStringList({"earlier", "later"}));
    QCOMPARE(timestamps, QVector<qint64>({1000, 1050}));
    QCOMPARE(pv.writes.size(), 2);
    QCOMPARE(pv.writes[0].value, 1.0); QCOMPARE(pv.writes[1].value, 2.0);
    QCOMPARE(e.state(d.root.get()).severity, 2);
  }

  void suppressionPreservesGroupSeverityOutput() {
    for (bool group : {false, true}) for (bool automatic : {false, true})
      for (auto mask : {"C", "D", "CD"}) {
        auto d = parseConfig("GROUP NULL root\n$SEVRPV root-output\n"
            "$SEVRCOMMAND DOWN_NO_ALARM root-clear\nGROUP root branch\n"
            "$SEVRPV branch-output\n$SEVRCOMMAND DOWN_NO_ALARM branch-clear\n"
            "CHANNEL branch pv\n$SEVRPV channel-output\n");
        FakePv pv; Engine e(d, {true}, &pv); e.start(); auto n = d.channels()[0];
        e.event(n, {3, 2, 2, 1, "major"}); pv.writes.clear();
        QStringList commands; e.command = [&](QString command) { commands << command; };
        e.setMask(group ? d.root.get() : n, Mask::parse(mask), automatic);
        QCOMPARE(commands, QStringList({"branch-clear", "root-clear"}));
        QCOMPARE(e.state(d.root.get()).severity, 0);
        QCOMPARE(e.state(n->parent).severity, 0);
        QCOMPARE(pv.writes.size(), QString(mask) == "D" ? 1 : 0);
        if (!pv.writes.isEmpty()) {
          QCOMPARE(pv.writes[0].name, QString("channel-output"));
          QCOMPARE(pv.writes[0].value, -1.0);
        }
        e.resetMask(group ? d.root.get() : n, automatic);
        // Enable exposes the cached alarm; Add receives a fresh monitor.
        if (QString(mask) != "D") e.event(n, {3, 2, 2, 1, "major"});
        // CD -> enabled first publishes channel NO_ALARM, then the fresh alarm.
        QCOMPARE(pv.writes.size(), QString(mask) == "C" ? 3 : 4);
        QCOMPARE(pv.writes[pv.writes.size() - 2].name, QString("branch-output"));
        QCOMPARE(pv.writes.last().name, QString("root-output"));
        QCOMPARE(pv.writes.last().value, 2.0);
      }
  }

  void expiredFilterCanBeCancelledByEarlierTimeout() {
    auto d = parseConfig("GROUP NULL root\n"
        "CHANNEL root later\n$ALARMCOUNTFILTER 0 1\n"
        "CHANNEL root earlier\n$ALARMCOUNTFILTER 0 1\n"
        "$SEVRCOMMAND UP_MINOR cancel-later\n");
    Engine e(d); qint64 elapsed = 1000; e.now = e.monotonicNow = [&] { return elapsed; };
    auto later = d.channels()[0], earlier = d.channels()[1];
    e.event(later, {}); e.event(earlier, {});
    e.command = [&](QString command) {
      if (command == "cancel-later") e.setMask(later, Mask::parse("C"));
    };
    QStringList records;
    e.alarmLog = [&](Node* n, const State&, qint64) { records << n->name; };
    e.event(earlier, {4, 1, 1, 1, "minor"});
    elapsed += 50; e.event(later, {3, 2, 2, 1, "major"});
    elapsed += 1100; e.tick();
    QCOMPARE(records, QStringList({"earlier"}));
    QCOMPARE(e.state(later).filterUntil, qint64(0));
    QCOMPARE(e.state(later).severity, 0);
    QCOMPARE(e.state(d.root.get()).severity, 1);
  }

  void noAckExpiresBeforeLaterFilter() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 0 1\n");
    Engine e(d); qint64 elapsed = 1000; e.now = e.monotonicNow = [&] { return elapsed; };
    auto n = d.channels()[0]; e.event(n, {}); e.noAck(n, true);
    // Both deadlines are overdue at tick, but NoAck ended before the alarm.
    elapsed = 3600050; e.event(n, {3, 2, 2, 1, "major"});
    QVector<bool> noAckAtAlarm;
    e.alarmLog = [&](Node*, const State& state, qint64) { noAckAtAlarm << state.mask[Ack]; };
    elapsed = 3601100; e.tick();
    QCOMPARE(noAckAtAlarm, QVector<bool>({false}));
    QCOMPARE(e.state(n).noAckUntil, qint64(0));
    QCOMPARE(e.state(n).unack, 2); QVERIFY(e.audible());
  }

  void includedNullGroupsKeepLegacyParents() {
    QTemporaryDir dir;
    QFile first(dir.filePath("first")), second(dir.filePath("second"));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.write("GROUP NULL branch\n$BEEPSEVR MAJOR\n$FORCEPV gate D 1 0\n"
                "CHANNEL branch direct\nGROUP NULL leaf\nCHANNEL leaf deep\n"
                "INCLUDE leaf second\nGROUP NULL afterInclude\nCHANNEL afterInclude last\n");
    first.close();
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.write("GROUP NULL nested\nCHANNEL nested inner\n"); second.close();
    auto d = parseConfig("GROUP NULL root\nINCLUDE root first\nCHANNEL root outside\n", dir.path());
    QCOMPARE(d.root->children.size(), size_t(2));
    auto branch = d.root->children[0].get();
    QCOMPARE(branch->name, QString("branch"));
    QCOMPARE(branch->children.size(), size_t(2));
    auto leaf = branch->children[1].get();
    QCOMPARE(leaf->name, QString("leaf"));
    QCOMPARE(leaf->children.size(), size_t(3));
    QCOMPARE(leaf->children[1]->name, QString("nested"));
    QCOMPARE(leaf->children[2]->name, QString("afterInclude"));
    const auto saved = writeConfig(d);
    auto restored = parseConfig(saved);
    QCOMPARE(writeConfig(restored), saved);
    auto deep = leaf->children[0].get();
    Engine e(d);
    e.event(deep, {4, 1, 1, 1, "minor"}); QVERIFY(!e.audible());
    e.event(deep, {3, 2, 2, 1, "major"}); QVERIFY(e.audible());
    e.forceValue(branch, 1);
    for (auto channel : d.channels())
      QCOMPARE(e.state(channel).mask[Disable], channel->name != "outside");
  }

  void calcTrailingComments_data() {
    QTest::addColumn<QString>("expression");
    QTest::addColumn<QString>("normalized");
    QTest::addColumn<bool>("forced");
    QTest::newRow("legacy-comment") << "A>0 # beam inhibit" << "A>0" << true;
    QTest::newRow("spaced-expression") << "A > 0 # beam inhibit" << "A > 0" << true;
    QTest::newRow("extra-hash") << "A > 0 # beam # inhibit" << "A > 0" << true;
    QTest::newRow("hash-operator") << "A # B" << "A # B" << false;
    QTest::newRow("hash-operator-comment") << "A # B # beam inhibit" << "A # B" << false;
  }
  void calcTrailingComments() {
    QFETCH(QString, expression); QFETCH(QString, normalized); QFETCH(bool, forced);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV CALC D 1 0\n"
                         "$FORCEPV_CALC " + expression + "\n$FORCEPV_CALC_A 1\n$FORCEPV_CALC_B 1\n");
    auto n = d.channels()[0];
    QCOMPARE(n->option("FORCEPV_CALC"), normalized);
    auto restored = parseConfig(writeConfig(d));
    QCOMPARE(restored.channels()[0]->option("FORCEPV_CALC"), normalized);
    FakePv pv; Engine e(restored, {}, &pv); e.start();
    QCOMPARE(e.state(restored.channels()[0]).mask[Disable], forced);
    QVERIFY_THROWS_EXCEPTION(ParseError,
        parseConfig("GROUP NULL root\n$FORCEPV_CALC A + # invalid expression\n"));
  }

  void addThenDisableCachedAlarm_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("group"); QTest::addColumn<bool>("automatic");
    for (bool global : {false, true}) for (bool passive : {false, true})
      for (bool group : {false, true}) for (bool automatic : {false, true})
        QTest::newRow(qPrintable(QString("global-%1-passive-%2-group-%3-force-%4")
            .arg(global).arg(passive).arg(group).arg(automatic))) << global << passive << group << automatic;
  }
  void addThenDisableCachedAlarm() {
    QFETCH(bool, global); QFETCH(bool, passive); QFETCH(bool, group); QFETCH(bool, automatic);
    for (bool noLog : {false, true}) {
      auto d = parseConfig("GROUP NULL root\n$SEVRPV group-output\n$SEVRCOMMAND UP_MAJOR group-up\n"
                           "$SEVRCOMMAND DOWN_NO_ALARM group-down\nCHANNEL root pv D\n"
                           "$SEVRCOMMAND UP_MAJOR channel-up\n$SEVRPV output\n");
      FakePv pv; Engine e(d, {global, passive}, &pv); e.start();
      auto n = d.channels()[0]; auto root = d.root.get();
      e.event(n, {3, 2, 2, 1, "alarm"});
      e.setMask(n, Mask::parse("CD"));
      e.setMask(n, Mask::parse(noLog ? "CL" : "C"));
      QCOMPARE(e.state(n).severity, 2);
      QStringList commands; QVector<State> logs;
      e.command = [&](QString command) { commands << command; };
      e.alarmLog = [&](Node*, const State& state, qint64) { logs << state; };
      QVector<ObservationCause> causes;
      auto observer = e.observe([&](const AlarmObservation& observation) { causes << observation.cause; });
      pv.writes.clear(); e.history.clear();
      e.setMask(group ? root : n, Mask::parse(noLog ? "D" : "DL"), automatic);
      QCOMPARE(commands, QStringList({"channel-up", "group-up", "group-down"}));
      QCOMPARE(logs.size(), noLog ? 0 : 1);
      if (!logs.isEmpty()) {
        QCOMPARE(logs[0].severity, 2);
        QVERIFY(!logs[0].mask[Cancel]); QVERIFY(!logs[0].mask[Disable]); QVERIFY(!logs[0].mask[Log]);
      }
      QCOMPARE(e.history.size(), 1);
      QCOMPARE(pv.writes.size(), global && !passive ? 3 : 0);
      if (!pv.writes.isEmpty()) {
        QCOMPARE(pv.writes[0].name, QString("output"));
        QCOMPARE(pv.writes[0].kind, WriteKind::Severity); QCOMPARE(pv.writes[0].value, 2.0);
        QCOMPARE(pv.writes[1].name, QString("group-output"));
        QCOMPARE(pv.writes[1].kind, WriteKind::Severity); QCOMPARE(pv.writes[1].value, 2.0);
        QCOMPARE(pv.writes[2].name, QString("output"));
        QCOMPARE(pv.writes[2].kind, WriteKind::Severity); QCOMPARE(pv.writes[2].value, -1.0);
      }
      QCOMPARE(e.state(n).mask.text(), QString(noLog ? "-D---" : "-D--L"));
      QVERIFY(e.state(n).awaitingObservation); // A cached exposure is not a fresh CA observation.
      for (auto cause : causes) QCOMPARE(cause, ObservationCause::Suppression);
      QCOMPARE(e.state(root).severity, 0); QCOMPARE(e.state(root).unack, 0);
      QCOMPARE(e.state(root).counts[0], 1); QCOMPARE(e.state(root).counts[2], 0);
      QCOMPARE(e.state(root).maskCounts[Cancel], 0); QCOMPARE(e.state(root).maskCounts[Disable], 1);
      QCOMPARE(e.presentation(root).severity, 0); QCOMPARE(e.presentation(root).unack, 0);
      QVERIFY(!e.audible());
    }
  }

  void combinedMaskSideEffects() {
    for (bool group : {false, true}) for (bool automatic : {false, true}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root pv D\n"
                           "$ACKPV output 7\n$SEVRPV severity\n");
      FakePv pv; Engine e(d, {true}, &pv); e.start();
      auto n = d.channels()[0];
      e.event(n, {0, 0, 2, 1, "cleared"});
      pv.writes.clear();
      e.setMask(group ? d.root.get() : n, Mask::parse("AT"), automatic);
      QCOMPARE(pv.writes.size(), 4);
      QCOMPARE(pv.writes[0].kind, WriteKind::Acknowledge);
      QCOMPARE(pv.writes[0].value, 2.0);
      QCOMPARE(pv.writes[1].kind, WriteKind::AckValue);
      QCOMPARE(pv.writes[1].value, 7.0);
      QCOMPARE(pv.writes[2].kind, WriteKind::Severity);
      QCOMPARE(pv.writes[2].value, 0.0);
      QCOMPARE(pv.writes[3].kind, WriteKind::AckTransient);
      QCOMPARE(e.state(n).unack, 0);
      QCOMPARE(e.state(n).mask.text(), QString("--AT-"));
    }
  }

  void combinedMaskAcknowledgementRestrictions() {
    for (bool passive : {false, true}) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root pv D\n$ACKPV output 7\n");
      FakePv pv; Engine e(d, {true, passive}, &pv); e.start();
      auto n = d.channels()[0]; e.event(n, {0, 0, 2, 1, "cleared"});
      pv.unwritable.insert("pv"); pv.writes.clear();
      e.setMask(n, Mask::parse("A"));
      QCOMPARE(e.state(n).unack, 2);
      QCOMPARE(pv.writes.size(), passive ? 0 : 1);
      if (!passive) QCOMPARE(pv.writes[0].kind, WriteKind::Acknowledge);
      // A later explicit enable can acknowledge once the channel is writable.
      e.setMask(n, Mask::parse("D"));
      pv.unwritable.clear(); pv.writes.clear(); e.setMask(n, Mask::parse("A"));
      QCOMPARE(e.state(n).unack, passive ? 2 : 0);
      QCOMPARE(pv.writes.size(), passive ? 0 : 2);
    }
  }

  void combinedMaskLogging() {
    for (bool global : {false, true}) for (bool noLog : {false, true}) {
      auto d = parseConfig(QString("GROUP NULL root\nCHANNEL root pv %1\n")
                           .arg(noLog ? "DL" : "D"));
      FakePv pv; Engine e(d, {global}, &pv); e.start();
      auto n = d.channels()[0]; e.event(n, {3, 2, 2, 1, "alarm"});
      QVector<State> logged;
      e.alarmLog = [&](Node*, const State& s, qint64) { logged << s; };
      e.setMask(n, Mask::parse(noLog ? "T" : "TL"));
      QCOMPARE(logged.size(), noLog ? 0 : 1);
      if (!logged.isEmpty()) {
        QCOMPARE(logged[0].severity, 2);
        QVERIFY(!logged[0].mask[Log]);
        QVERIFY(!logged[0].mask[AckT]); // ACKT changes after Enable, too.
      }
      QCOMPARE(e.state(n).mask[Log], !noLog);
      QVERIFY(e.state(n).mask[AckT]);
    }
  }

  void timedNoAckTraversal() {
    const QString config = "GROUP NULL root\nGROUP root branch\n"
        "CHANNEL branch nested\n$ACKPV output 2\n"
        "CHANNEL root direct\n$ACKPV output 1\n";
    for (bool roundTrip : {false, true}) for (bool expire : {false, true}) {
      auto d = parseConfig(config);
      if (roundTrip) d = parseConfig(writeConfig(d));
      FakePv pv; Engine e(d, {true}, &pv); e.start();
      qint64 elapsed = 1000; e.monotonicNow = [&] { return elapsed; };
      for (auto n : d.channels()) e.event(n, {3, 2, 2, 1, "alarm"});
      e.noAck(d.root.get(), true);
      for (auto n : d.channels()) e.event(n, {0, 0, 2, 1, "cleared"});
      pv.writes.clear();
      if (expire) { elapsed += 3600000; e.tick(); }
      else e.noAck(d.root.get(), false);
      QCOMPARE(pv.writes.size(), 4);
      QCOMPARE(pv.writes[0].name, QString("direct"));
      QCOMPARE(pv.writes[1].kind, WriteKind::AckValue);
      QCOMPARE(pv.writes[1].value, 1.0);
      QCOMPARE(pv.writes[2].name, QString("nested"));
      QCOMPARE(pv.writes[3].kind, WriteKind::AckValue);
      QCOMPARE(pv.writes[3].value, 2.0);
    }
  }

  void timedNoAckPreservesDescendantTimers() {
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch nested\n"
                         "CHANNEL root direct\nCHANNEL root independent\n");
    Engine e(d); qint64 elapsed = 1000; e.monotonicNow = [&] { return elapsed; };
    auto root = d.root.get(), branch = root->children[0].get();
    auto direct = root->children[1].get(), independent = root->children[2].get();
    e.noAck(root, true); elapsed += 1000;
    e.noAck(branch, true); e.noAck(independent, true);
    elapsed = 3601000; e.tick();
    QVERIFY(!e.state(direct).mask[Ack]);
    QVERIFY(e.state(branch->children[0].get()).mask[Ack]);
    QVERIFY(e.state(independent).mask[Ack]);
    elapsed += 1000; e.tick();
    for (auto n : d.channels()) QVERIFY(!e.state(n).mask[Ack]);
  }

  void groupAcknowledgementOrder() {
    const QString config = "GROUP NULL root\nGROUP root branch\n"
        "CHANNEL branch nested\n$ACKPV output 2\n"
        "CHANNEL root direct\n$ACKPV output 1\n";
    for (bool roundTrip : {false, true}) {
      auto d = parseConfig(config);
      if (roundTrip) d = parseConfig(writeConfig(d));
      FakePv pv; Engine e(d, {true}, &pv); e.start();
      for (auto n : d.channels()) e.event(n, {3, 2, 2, 1, "alarm"});
      pv.writes.clear();
      e.acknowledge(d.root.get());
      QCOMPARE(pv.writes.size(), 4);
      QCOMPARE(pv.writes[0].name, QString("direct"));
      QCOMPARE(pv.writes[0].kind, WriteKind::Acknowledge);
      QCOMPARE(pv.writes[1].name, QString("output"));
      QCOMPARE(pv.writes[1].kind, WriteKind::AckValue);
      QCOMPARE(pv.writes[1].value, 1.0);
      QCOMPARE(pv.writes[2].name, QString("nested"));
      QCOMPARE(pv.writes[2].kind, WriteKind::Acknowledge);
      QCOMPARE(pv.writes[3].name, QString("output"));
      QCOMPARE(pv.writes[3].kind, WriteKind::AckValue);
      QCOMPARE(pv.writes[3].value, 2.0);
    }
  }

  void groupMaskTraversal_data() {
    QTest::addColumn<int>("action");
    for (int action = 0; action < 5; ++action)
      QTest::newRow(qPrintable(QString::number(action))) << action;
  }
  void groupMaskTraversal() {
    QFETCH(int, action);
    const QString config =
        "GROUP NULL root\n$SEVRCOMMAND DOWN_MINOR unexpected-minor\n"
        "$SEVRCOMMAND DOWN_NO_ALARM clear\n"
        "GROUP root branch\nCHANNEL branch major D\nCHANNEL root minor D\n";
    for (bool roundTrip : {false, true}) {
      auto d = parseConfig(config);
      if (roundTrip) d = parseConfig(writeConfig(d));
      Engine e(d);
      e.setMask(d.root.get(), {});
      for (auto n : d.channels()) e.event(n, {});
      for (auto n : d.channels()) e.event(n, {3, n->name == "major" ? 2 : 1, 0, 1, "alarm"});
      QStringList commands, visited;
      e.command = [&](QString command) { commands << command; };
      e.channelUpdated = [&](const ChannelUpdate& update) { visited << update.pv; };
      if (action < 2) e.setMask(d.root.get(), Mask::parse("D"), action == 1);
      else if (action < 4) e.resetMask(d.root.get(), action == 3);
      else e.modifyMask(d.root.get(), Disable, 1);
      QCOMPARE(visited, QStringList({"minor", "major"}));
      QCOMPARE(commands, QStringList({"clear"}));
      QCOMPARE(e.state(d.root.get()).severity, 0);
    }
  }
  void guidanceTerminatorComments_data() {
    QTest::addColumn<QString>("ending");
    QTest::newRow("comment") << "$END # first channel";
    QTest::newRow("mixed-case") << "  $End\t# first channel";
    QTest::newRow("prefix") << "$END# first channel";
  }
  void guidanceTerminatorComments() {
    QFETCH(QString, ending);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root first\n$GUIDANCE\nFirst help\n" +
        ending + "\nCHANNEL root second\n$GUIDANCE\nSecond help\n$END\n");
    for (int pass = 0; pass < 2; ++pass) {
      QCOMPARE(d.channels().size(), 2);
      QCOMPARE(d.channels()[0]->name, QString("first"));
      QCOMPARE(d.channels()[0]->option("GUIDANCE_TEXT"), QString("First help"));
      QCOMPARE(d.channels()[1]->name, QString("second"));
      QCOMPARE(d.channels()[1]->option("GUIDANCE_TEXT"), QString("Second help"));
      FakePv pv; Engine engine(d, {}, &pv); engine.start();
      QVERIFY(pv.alarms.contains("first")); QVERIFY(pv.alarms.contains("second"));
      engine.stop();
      d = parseConfig(writeConfig(d));
    }
    // Also accept a commented terminator at EOF, without a later block to hide the error.
    QCOMPARE(parseConfig("GROUP NULL root\n$GUIDANCE\nHelp\n" + ending)
                 .root->option("GUIDANCE_TEXT"), QString("Help"));
  }
  void countFilterIntegerBases_data() {
    QTest::addColumn<QString>("arguments");
    QTest::addColumn<int>("count"); QTest::addColumn<int>("seconds");
    QTest::newRow("octal") << "010 010" << 8 << 8;
    QTest::newRow("hexadecimal") << "0x2 0xA" << 2 << 10;
    QTest::newRow("decimal") << "10 10" << 10 << 10;
    QTest::newRow("signed") << "-01 +010" << -1 << 8;
    QTest::newRow("defaults") << "" << 1 << 1;
  }
  void countFilterIntegerBases() {
    QFETCH(QString, arguments); QFETCH(int, count); QFETCH(int, seconds);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER " + arguments + '\n');
    for (int pass = 0; pass < 2; ++pass) {
      QCOMPARE(d.channels()[0]->option("ALARMCOUNTFILTER"), QString("%1 %2").arg(count).arg(seconds));
      Engine engine(d); auto n = d.channels()[0];
      QCOMPARE(engine.state(n).filterCount, count);
      qint64 elapsed = 1000; engine.monotonicNow = [&] { return elapsed; };
      engine.event(n, {}); engine.event(n, {3, 2, 0, 1, "alarm"});
      elapsed += seconds * 1000 - 1; engine.tick();
      QCOMPARE(engine.state(n).severity, 0);
      ++elapsed; engine.tick(); QCOMPARE(engine.state(n).severity, 2);
      d = parseConfig(writeConfig(d));
    }
    for (const auto& bad : {"0xF4241 1", "-0x2 1", "1 -01", "1 0x80000000", "0x 1"}) {
      QVERIFY_THROWS_EXCEPTION(ParseError,
                               parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER " +
                                           QString(bad) + '\n'));
    }
  }

  void fixedArgumentDirectiveComments() {
    auto d = parseConfig(
        "$BEEPSEVERITY MAJOR # facility threshold\nGROUP NULL root\n"
        "$HEARTBEATPV beat 1 7 # watchdog\nCHANNEL root pv\n"
        "$SEVRPV out#put # severity mirror\n$BEEPSEVR INVALID # audio threshold\n"
        "$ACKPV ack 1 # acknowledge\n$ALARMCOUNTFILTER 2 5 # debounce\n"
        "$FORCEPV CALC D 1 NE # gate\n$FORCEPV_CALC A+B\n"
        "$FORCEPV_CALC_A input#pv # input\n$FORCEPV_CALC_B 1 # constant\n"
        "$COMMAND echo '# keep' # shell comment\n$ALIAS Label # keep\n"
        "$SEVRCOMMAND UP_MAJOR echo '# keep'\n$STATCOMMAND HIHI echo '# keep'\n"
        "$GUIDANCE https://example.invalid/help#section # keep\n"
        "$GUIDANCE\n# Keep this guidance line\n$END\n");
    for (int pass = 0; pass < 2; ++pass) {
      auto n = d.channels()[0];
      QCOMPARE(d.beepSeverity, 2);
      QCOMPARE(d.root->option("HEARTBEATPV"), QString("beat 1 7"));
      QCOMPARE(n->option("SEVRPV"), QString("out#put"));
      QCOMPARE(n->option("BEEPSEVR"), QString("INVALID"));
      QCOMPARE(n->option("ACKPV"), QString("ack 1"));
      QCOMPARE(n->option("ALARMCOUNTFILTER"), QString("2 5"));
      QCOMPARE(n->option("FORCEPV"), QString("CALC -D--- 1 NE"));
      QCOMPARE(n->option("FORCEPV_CALC_A"), QString("input#pv"));
      QCOMPARE(n->option("FORCEPV_CALC_B"), QString("1"));
      QCOMPARE(n->option("COMMAND"), QString("echo '# keep' # shell comment"));
      QCOMPARE(n->option("ALIAS"), QString("Label # keep"));
      QCOMPARE(n->option("SEVRCOMMAND"), QString("UP_MAJOR echo '# keep'"));
      QCOMPARE(n->option("STATCOMMAND"), QString("HIHI echo '# keep'"));
      QCOMPARE(n->option("GUIDANCE"), QString("https://example.invalid/help#section # keep"));
      QCOMPARE(n->option("GUIDANCE_TEXT"), QString("# Keep this guidance line"));
      d = parseConfig(writeConfig(d));
    }
    auto defaults = parseConfig("GROUP NULL root\n$HEARTBEATPV #beat # default interval/value\n"
        "CHANNEL root pv\n$FORCEPV #gate # default mask/values\n"
        "$ALARMCOUNTFILTER # default count/seconds\n");
    QCOMPARE(defaults.root->option("HEARTBEATPV"), QString("#beat"));
    QCOMPARE(defaults.channels()[0]->option("FORCEPV"), QString("#gate ----- 1 0"));
    QCOMPARE(defaults.channels()[0]->option("ALARMCOUNTFILTER"), QString("1 1"));
  }
  void singleTokenPvDirectives() {
    for (bool group : {false, true}) {
      const QString directives =
          "$SEVRPV - ignored placeholder\n$SEVRPV output extra tokens\n"
          "$SEVRPV ignored-output extra\n$FORCEPV CALC D 1 0\n"
          "$FORCEPV_CALC A+B+C+D+E+F\n"
          "$FORCEPV_CALC_A input\textra # comment\n"
          "$FORCEPV_CALC_A ignored-input extra\n"
          "$FORCEPV_CALC_B 1 extra\n$FORCEPV_CALC_C 0 extra\n"
          "$FORCEPV_CALC_D -2 extra\n$FORCEPV_CALC_E 2e0 extra\n"
          "$FORCEPV_CALC_F #input extra\n";
      auto d = parseConfig("GROUP NULL root\n" + (group ? directives : QString()) +
          "CHANNEL root pv\n" + (group ? QString() : directives));
      for (int pass = 0; pass < 2; ++pass) {
        auto n = d.channels()[0], target = group ? d.root.get() : n;
        QCOMPARE(target->option("SEVRPV"), QString("output"));
        const QStringList inputs = {"input", "1", "0", "-2", "2e0", "#input"};
        for (int i = 0; i < inputs.size(); ++i)
          QCOMPARE(target->option("FORCEPV_CALC_" + QString(QChar('A' + i))), inputs[i]);
        QVERIFY(!writeConfig(d).contains("extra"));
        FakePv pv; Engine e(d, {true}, &pv); e.start();
        QCOMPARE(pv.numbers.size(), 2);
        QVERIFY(pv.numbers.contains("input")); QVERIFY(pv.numbers.contains("#input"));
        e.event(n, {3, 2, 2, 1, "alarm"});
        QCOMPARE(pv.writes.size(), 1);
        QCOMPARE(pv.writes[0].name, QString("output"));
        QCOMPARE(pv.writes[0].kind, WriteKind::Severity);
        QCOMPARE(pv.writes[0].value, 2.0);
        pv.numbers["input"](0); pv.numbers["#input"](0);
        QCOMPARE(e.state(target).forceCurrent, 1.0);
        QVERIFY(e.state(n).mask[Disable]);
        pv.numbers["input"](-1);
        QCOMPARE(e.state(target).forceCurrent, 0.0);
        QVERIFY(!e.state(n).mask[Disable]);
        d = parseConfig(writeConfig(d));
      }
    }
  }

  void severityPvPlaceholder() {
    auto d = parseConfig("GROUP NULL root\n$SEVRPV -\n$SEVRPV - # placeholder\n"
        "$SEVRPV group-output\n$SEVRPV ignored-group-output\n$SEVRPV -\n"
        "CHANNEL root pv\n$SEVRPV -\n$SEVRPV output\n$SEVRPV ignored-output\n$SEVRPV -\n");
    for (int pass = 0; pass < 2; ++pass) {
      QCOMPARE(d.root->option("SEVRPV"), QString("group-output"));
      auto n = d.channels()[0];
      QCOMPARE(n->option("SEVRPV"), QString("output"));
      FakePv pv; Engine e(d, {true}, &pv); e.start();
      e.event(n, {3, 2, 2, 1, "alarm"});
      QCOMPARE(pv.writes.size(), 2);
      QCOMPARE(pv.writes[0].name, QString("output"));
      QCOMPARE(pv.writes[1].name, QString("group-output"));
      for (const auto& write : pv.writes) {
        QCOMPARE(write.kind, WriteKind::Severity); QCOMPARE(write.value, 2.0);
      }
      d = parseConfig(writeConfig(d));
    }
  }

  void forceResetRoundTrip_data() {
    QTest::addColumn<QString>("reset");
    for (auto value : {"0.000001", "1.2345e-6", "-1.23e-9", "0.12345678912345678", "1e100", "1.23456789e-100"})
      QTest::newRow(value) << QString(value);
  }
  void forceResetRoundTrip() {
    QFETCH(QString, reset);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV gate D 2 " + reset + "\n");
    QTemporaryDir dir;
    for (int pass = 0; pass < 3; ++pass) {
      auto n = d.channels()[0];
      QCOMPARE(n->option("FORCEPV").section(' ', 3, 3).toDouble(), reset.toDouble());
      {
        Engine e(d);
        e.forceValue(n, 2); QVERIFY(e.state(n).mask[Disable]);
        e.forceValue(n, reset.toDouble()); QVERIFY(!e.state(n).mask[Disable]);
      }
      const auto text = writeConfig(d);
      QCOMPARE(writeConfig(parseConfig(text)), text);
      saveConfig(d, dir.filePath("saved"));
      d = loadConfig(dir.filePath("saved"));
    }
  }
  void firstSuppressedMonitorUsesFilter_data() {
    QTest::addColumn<QString>("mask"); QTest::addColumn<bool>("global");
    QTest::addColumn<int>("count");
    for (auto mask : {"C", "D"}) for (bool global : {false, true}) for (int count : {-1, 0, 1})
      QTest::newRow(qPrintable(QString("%1-global%2-count%3").arg(mask).arg(global).arg(count)))
          << QString(mask) << global << count;
  }
  void firstSuppressedMonitorUsesFilter() {
    QFETCH(QString, mask); QFETCH(bool, global); QFETCH(int, count);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv " + mask +
        "\n$ALARMCOUNTFILTER " + QString::number(count) + " 10\n$SEVRCOMMAND UP_MAJOR alarm\n");
    FakePv pv; Engine e(d, {global}, &pv); auto n = d.channels()[0];
    qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    QStringList commands; e.command = [&](QString command) { commands << command; };
    e.start(); e.setMask(n, {});
    e.event(n, {3, 2, 2, 1, "first"});
    QCOMPARE(e.state(n).severity, 0); QCOMPARE(e.state(n).unack, 0);
    QVERIFY(!e.audible()); QVERIFY(commands.isEmpty());
    time += 9999; e.tick(); QCOMPARE(e.state(n).severity, 0);
    ++time; e.tick(); QCOMPARE(e.state(n).severity, 2);
    QCOMPARE(e.state(n).unack, 2); QCOMPARE(commands, QStringList({"alarm"}));
  }
  void initialFilterInputDoesNotInventEdge() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv C\n$ALARMCOUNTFILTER 1 10\n");
    Engine e(d); auto n = d.channels()[0]; qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; }; e.setMask(n, {});
    e.event(n, {3, 2, 0, 1, "short pulse"});
    ++time; e.event(n, {}); // First real in/out edge; initial input was ERROR.
    ++time; e.event(n, {3, 2, 0, 1, "second pulse"});
    QCOMPARE(e.state(n).severity, 0);
    time += 10000; e.tick(); QCOMPARE(e.state(n).severity, 2);
  }
  void intervalClocks_data() {
    QTest::addColumn<qint64>("correction");
    QTest::newRow("backwards") << qint64(-60000);
    QTest::newRow("forwards") << qint64(60000);
  }
  void intervalClocks() {
    QFETCH(qint64, correction);
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV beat 1 7\n"
        "CHANNEL root filtered\n$ALARMCOUNTFILTER 0 1\nCHANNEL root timed\nCHANNEL root speaker\n");
    FakePv pv; Engine e(d, {true}, &pv);
    qint64 wall = 1700000000000LL, elapsed = 1000;
    e.now = [&] { return wall; }; e.monotonicNow = [&] { return elapsed; };
    const auto eventTime = wall;
    qint64 logged = -1; e.alarmLog = [&](Node*, const State&, qint64 stamp) { logged = stamp; };
    auto filtered = d.channels()[0], timed = d.channels()[1];
    e.start(); e.event(filtered, {}); e.event(timed, {});
    e.noAck(timed, true); e.silenceUntil = elapsed + 1000;
    e.event(d.channels()[2], {3, 2, 2, 1, "audible alarm"});
    e.event(filtered, {3, 2, 2, 1, "alarm"});
    wall += correction; e.tick();
    QVERIFY(!e.audible());
    QCOMPARE(e.heartbeatDelay(), 1000); QVERIFY(pv.writes.isEmpty());
    QCOMPARE(e.state(filtered).severity, 0); QVERIFY(e.state(timed).mask[Ack]);
    elapsed += 999; e.tick(); QCOMPARE(e.state(filtered).severity, 0);
    ++elapsed; e.tick();
    QCOMPARE(e.state(filtered).severity, 2); QCOMPARE(logged, eventTime);
    QCOMPARE(pv.writes.size(), 1); QCOMPARE(pv.writes[0].name, QString("beat"));
    QVERIFY(e.audible()); QVERIFY(e.state(timed).mask[Ack]);
    elapsed = 3601000; e.tick(); QVERIFY(!e.state(timed).mask[Ack]);
    QCOMPARE(pv.writes.size(), 2); // Delayed heartbeat skips catch-up bursts.
    QCOMPARE(e.channelUpdate(filtered).observedAt, wall);
  }
  void filterEdgesUseMonotonicTime() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 1 1\n");
    Engine e(d); auto n = d.channels()[0]; qint64 wall = 1700000000000LL, elapsed = 1000;
    e.now = [&] { return wall; }; e.monotonicNow = [&] { return elapsed; };
    e.event(n, {}); e.event(n, {3, 2, 0, 1, "pulse"});
    elapsed += 100; e.event(n, {});
    elapsed += 2000; wall -= 60000;
    e.event(n, {3, 2, 0, 1, "new pulse"});
    QCOMPARE(e.state(n).severity, 0); // Old edges are outside the one-second window.
    elapsed += 1000; e.tick(); QCOMPARE(e.state(n).severity, 2);
  }

  void enableInitiallyDisabledBeforeObservation_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("automatic");
    for (bool global : {false, true}) for (bool automatic : {false, true})
      QTest::newRow(qPrintable(QString("global-%1-force-%2").arg(global).arg(automatic))) << global << automatic;
  }
  void enableInitiallyDisabledBeforeObservation() {
    QFETCH(bool, global); QFETCH(bool, automatic);
    auto d = parseConfig("GROUP NULL root\n$SEVRCOMMAND UP_ERROR group-error\n"
                         "CHANNEL root pv D\n$SEVRCOMMAND UP_ERROR channel-error\n$SEVRPV output\n");
    FakePv pv; Engine e(d, {global}, &pv); auto n = d.channels()[0];
    QStringList commands; e.command = [&](QString command) { commands << command; };
    e.start(); pv.writes.clear();
    QCOMPARE(e.state(n).severity, 0);
    e.setMask(d.root.get(), {}, automatic);
    QCOMPARE(e.state(n).unack, 0);
    QCOMPARE(e.state(d.root.get()).severity, 0);
    QVERIFY(!e.audible()); QVERIFY(commands.isEmpty());
    for (const auto& write : pv.writes)
      if (write.kind == WriteKind::Severity) QCOMPARE(write.value, 0.0);
    // A real monitoring failure must still raise ERROR after the channel is enabled.
    e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QCOMPARE(commands, QStringList({"channel-error", "group-error"}));
    QCOMPARE(e.state(n).severity, 4);
  }
  void cancelledDisableChangesDoNotWriteSeverity_data() {
    QTest::addColumn<bool>("group"); QTest::addColumn<bool>("automatic");
    for (bool group : {false, true}) for (bool automatic : {false, true})
      QTest::newRow(qPrintable(QString("group-%1-force-%2").arg(group).arg(automatic))) << group << automatic;
  }
  void cancelledDisableChangesDoNotWriteSeverity() {
    QFETCH(bool, group); QFETCH(bool, automatic);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$SEVRPV output\n");
    FakePv pv; Engine e(d, {true}, &pv); e.start(); auto n = d.channels()[0];
    auto target = group ? d.root.get() : n;
    e.event(n, {3, 2, 2, 1, "20"});
    e.setMask(target, Mask::parse("D"), automatic);
    QCOMPARE(pv.writes.last().value, -1.0);
    e.setMask(target, Mask::parse("CD"), automatic); pv.writes.clear();
    for (auto mask : {"C", "CD", "C"}) e.setMask(target, Mask::parse(mask), automatic);
    QVERIFY(pv.writes.isEmpty());
    QCOMPARE(e.presentation(n).severity, 0);
    e.setMask(target, {}, automatic);
    QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(pv.writes[0].name, QString("output"));
    QCOMPARE(pv.writes[0].value, 2.0);
  }
  void firstAlarmLoggingAfterSuppression_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<QString>("mask");
    for (bool global : {false, true}) for (auto mask : {"-----", "C", "D", "CL", "DL"})
      QTest::newRow(qPrintable(QString("global-%1-mask-%2").arg(global).arg(mask))) << global << QString(mask);
  }
  void firstAlarmLoggingAfterSuppression() {
    QFETCH(bool, global); QFETCH(QString, mask);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv " + mask + "\n");
    Engine e(d, {global}); auto n = d.channels()[0];
    int records = 0; qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    e.alarmLog = [&](Node* channel, const State& state, qint64 stamp) {
      QCOMPARE(channel, n); QCOMPARE(state.severity, 2); QCOMPARE(stamp, time); ++records;
    };
    auto added = n->mask; added[Cancel] = false;
    if (n->mask[Cancel]) e.setMask(n, added);
    e.event(n, {3, 2, 2, 1, "20"});
    const int expected = (n->mask[Cancel] || n->mask[Disable]) && !n->mask[Log] ? 1 : 0;
    QCOMPARE(records, expected);
    e.event(n, {3, 2, 2, 1, "21"}); // Value-only callback must not duplicate the record.
    QCOMPARE(records, expected);
  }
  void reenableRecordsCachedAlarm_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("noLog"); QTest::addColumn<bool>("cancelled");
    for (int mode = 0; mode < 3; ++mode) for (bool noLog : {false, true}) for (bool cancelled : {false, true})
      QTest::newRow(qPrintable(QString("mode-%1-nolog-%2-cancel-%3").arg(mode).arg(noLog).arg(cancelled)))
          << (mode != 0) << (mode == 2) << noLog << cancelled;
  }
  void reenableRecordsCachedAlarm() {
    QFETCH(bool, global); QFETCH(bool, passive); QFETCH(bool, noLog); QFETCH(bool, cancelled);
    auto d = parseConfig("GROUP NULL root\n$SEVRCOMMAND UP_MAJOR group-alarm\n"
                         "CHANNEL root pv\n$SEVRCOMMAND UP_MAJOR channel-alarm\n"
                         "$STATCOMMAND HIHI status-alarm\n$SEVRPV output\n");
    FakePv pv; Engine e(d, {global, passive}, &pv); auto n = d.channels()[0];
    qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    e.event(n, {}); e.setMask(n, Mask::parse("D")); e.event(n, {3, 2, 2, 1, "20"});
    if (cancelled) { e.setMask(n, Mask::parse("CD")); e.setMask(n, Mask::parse("C")); }
    e.history.clear(); pv.writes.clear();
    int records = 0; QStringList commands;
    e.alarmLog = [&](Node* channel, const State& state, qint64 stamp) {
      QCOMPARE(channel, n); QCOMPARE(state.status, 3); QCOMPARE(state.severity, 2);
      QCOMPARE(state.value, QString("20")); QCOMPARE(stamp, time); ++records;
    };
    e.command = [&](QString command) { commands << command; };
    // Enable logs before this request changes NoLog; later alarms use the new bit.
    time = 2000; e.setMask(d.root.get(), Mask::parse(noLog ? "L" : "-----"));
    QCOMPARE(records, 1);
    QCOMPARE(e.history.size(), 1);
    QVERIFY(e.history[0].endsWith(" pv HIHI MAJOR 20"));
    QVERIFY(e.history[0].startsWith(QDateTime::fromMSecsSinceEpoch(time).toString("dd-MMM-yyyy HH:mm:ss")));
    QCOMPARE(commands, QStringList({"channel-alarm", "group-alarm"}));
    QCOMPARE(e.state(d.root.get()).counts[2], 1);
    QCOMPARE(e.state(d.root.get()).unack, 2);
    QCOMPARE(pv.writes.size(), global && !passive ? 1 : 0);
    e.setMask(n, Mask::parse(noLog ? "L" : "-----"));
    e.event(n, {3, 2, 2, 1, "21"});
    QCOMPARE(records, 1); QCOMPARE(e.history.size(), 1); QCOMPARE(commands.size(), 2);
    QCOMPARE(pv.writes.size(), global && !passive ? 1 : 0);
    e.alarmLog = [&](Node*, const State&, qint64) { ++records; };
    e.event(n, {4, 1, 2, 1, "10"});
    QCOMPARE(records, noLog ? 1 : 2);
  }

  void globalGroupAcknowledgesSuppressedChildren() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root active\nCHANNEL root disabled -D---\n"
                         "CHANNEL root noack --A--\nCHANNEL root shelf\n"
                         "GROUP root suppressed\nCHANNEL suppressed hidden -D---\n");
    FakePv pv; Engine e(d, {true}, &pv);
    for (auto n : d.channels()) e.event(n, {3, 2, 2, 1, "2"});
    e.shelve(d.channels()[3], 10, "maintenance", "tester");
    e.acknowledge(d.root.get());
    QStringList names;
    for (const auto& write : pv.writes)
      if (write.kind == WriteKind::Acknowledge) names << write.name;
    QCOMPARE(names, QStringList({"active", "disabled", "noack"}));
    // Wait for IOC confirmation; suppressed latches must not be cleared optimistically.
    QCOMPARE(e.state(d.channels()[1]).unack, 2);
    for (int i = 0; i < 3; ++i) e.event(d.channels()[i], {3, 2, 0, 1, "2"});
    e.setMask(d.channels()[1], {}); e.setMask(d.channels()[2], {});
    QCOMPARE(e.presentation(d.channels()[1]).unack, 0);
    QCOMPARE(e.presentation(d.channels()[2]).unack, 0);
    QCOMPARE(e.state(d.channels()[3]).unack, 2);
    QCOMPARE(e.state(d.channels()[4]).unack, 2);
  }
  void silenceCurrentDownwardTransitions_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("shelved");
    QTest::newRow("local") << false << false;
    QTest::newRow("global") << true << false;
    QTest::newRow("shelved") << false << true;
  }
  void silenceCurrentDownwardTransitions() {
    QFETCH(bool, global); QFETCH(bool, shelved);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv ---T-\n");
    Engine e(d, {global}); auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 0, "0"}); e.event(n, {3, 2, 2, 0, "2"});
    if (shelved) e.shelve(n, 10, "maintenance", "tester");
    e.silenceCurrent = true;
    e.event(n, {4, 1, 1, 0, "1"});
    QCOMPARE(e.state(n).unack, 1);
    QCOMPARE(e.silenceCurrent, shelved);
    QCOMPARE(e.audible(), !shelved);
    // Value-only changes and clears below the threshold do not end silence.
    e.silenceCurrent = true;
    e.event(n, {4, 1, 1, 0, "1.1"}); QVERIFY(e.silenceCurrent);
    e.event(n, {0, 0, 0, 0, "0"}); QVERIFY(e.silenceCurrent);
  }

  void shelvingLifecycle() {
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch pv\nCHANNEL root other\n");
    Engine e(d);
    qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0]; auto other = d.channels()[1]; auto root = d.root.get();
    QStringList operations; int records = 0;
    e.operation = [&](Node*, const QString& text, OperationKind) { operations << text; };
    e.alarmLog = [&](Node*, const State&, qint64) { ++records; };
    for (auto c : d.channels()) e.event(c, {});
    e.event(n, {3, 1, 0, 1, {}});
    QVERIFY(e.audible());
    QCOMPARE(e.shelve(n, 1, "  Testing hardware  ", "tester"), 1);
    const auto deadline = e.state(n).shelf.until;
    QCOMPARE(e.state(n).shelf.reason, QString("Testing hardware"));
    QCOMPARE(e.state(n).unack, 1);
    QCOMPARE(e.presentation(root).shelved, 1);
    QCOMPARE(e.presentation(root).severity, 0);
    QCOMPARE(e.presentation(root).counts[0], 1); // Shelved is not counted as healthy.
    QVERIFY(!e.audible());
    e.silenceCurrent = true;
    e.event(n, {3, 2, 0, 1, {}});
    QVERIFY(e.silenceCurrent); // A hidden escalation cannot reset current-alarm silence.
    e.event(n, {});
    QCOMPARE(e.state(n).unack, 2);
    QCOMPARE(records, 3);
    e.event(other, {3, 1, 0, 1, {}});
    e.acknowledge(root);
    QCOMPARE(e.state(other).unack, 0);
    QCOMPARE(e.state(n).unack, 2);
    e.acknowledge(n); // Direct acknowledgement also requires unshelving first.
    QCOMPARE(e.state(n).unack, 2);
    time = deadline - 1; e.tick(); QVERIFY(e.state(n).shelf.until);
    time = deadline; e.tick();
    QCOMPARE(e.presentation(root).shelved, 0);
    QCOMPARE(e.presentation(root).unack, 2);
    QCOMPARE(e.state(n).severity, 0);
    QVERIFY(e.audible());
    QVERIFY(operations.last().startsWith("Shelf expired /root/branch/pv"));
    const int logs = operations.size(); e.tick(); QCOMPARE(operations.size(), logs);
    e.acknowledge(n); QCOMPARE(e.state(n).unack, 0);
  }
  void shelvingUsernameAttribution() {
    auto doc = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine engine(doc); qint64 now = 1000; engine.now = engine.monotonicNow = [&] { return now; };
    auto channel = doc.channels()[0];
    QStringList logs;
    engine.operation = [&](Node*, const QString& text, OperationKind) { logs << text; };
    for (const auto& user : QStringList{"", "   ", "two names", "bad\nuser", "bad\tuser",
                                        QString(121, 'x'), QString("bad") + QChar(0)}) {
      QVERIFY_THROWS_EXCEPTION(ParseError, engine.shelve(channel, 1, "reason", user));
      QCOMPARE(engine.state(channel).shelf.until, qint64(0));
    }
    QCOMPARE(engine.shelve(channel, 1, "reason", "  operator_one  "), 1);
    QCOMPARE(engine.state(channel).shelf.username, QString("operator_one"));
    QVERIFY(logs.last().contains(" username=operator_one "));
    engine.shelve(doc.root.get(), 2, "group", "operator_two");
    QCOMPARE(engine.state(channel).shelf.username, QString("operator_one"));
    QCOMPARE(engine.shelve(channel, 3, "extended", "operator_two", true), 1);
    QVERIFY(logs.last().contains("username=operator_two previous_username=operator_one"));
    QCOMPARE(engine.shelves().first().shelf.username, QString("operator_two"));
    auto copy = parseConfig(writeConfig(doc)); Engine restored(copy); restored.now = restored.monotonicNow = [&] { return now; };
    restored.restoreShelves(engine.shelves());
    QCOMPARE(restored.state(copy.channels()[0]).shelf.username, QString("operator_two"));
    now = engine.state(channel).shelf.until; engine.tick();
    QVERIFY(logs.last().contains("shelved_by=operator_two"));
    QVERIFY(engine.state(channel).shelf.username.isEmpty());
  }

  void shelvingValidationAndGroups() {
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch one\nCHANNEL branch two\nCHANNEL root three\n");
    Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    auto channels = d.channels(); auto root = d.root.get();
    for (auto c : channels) e.event(c, {3, 2, 0, 1, {}});
    QVERIFY_THROWS_EXCEPTION(ParseError, e.shelve(root, 0, "reason", "tester"));
    QVERIFY_THROWS_EXCEPTION(ParseError, e.shelve(root, Engine::MaximumShelfMinutes + 1, "reason", "tester"));
    QVERIFY_THROWS_EXCEPTION(ParseError, e.shelve(root, 60, "  ", "tester"));
    QVERIFY_THROWS_EXCEPTION(ParseError, e.shelve(root, 60, "line\nline", "tester"));
    QVERIFY_THROWS_EXCEPTION(ParseError, e.shelve(root, 60, QString(241, 'a'), "tester"));
    QCOMPARE(e.presentation(root).shelved, 0);
    QCOMPARE(e.shelve(channels[0], 15, "individual", "tester"), 1);
    auto first = e.state(channels[0]).shelf.until;
    time += 1000;
    QCOMPARE(e.shelve(root, 60, "group", "tester"), 2);
    QCOMPARE(e.state(channels[0]).shelf.until, first);
    QCOMPARE(e.state(channels[0]).shelf.reason, QString("individual"));
    QCOMPARE(e.presentation(root).shelved, 3);
    QCOMPARE(e.presentation(channels[0]->parent).shelved, 2);
    QCOMPARE(e.shelve(root, 60, "no change", "tester"), 0);
    QCOMPARE(e.shelve(channels[0], 1440, "extended", "tester", true), 1);
    QVERIFY(e.state(channels[0]).shelf.until > first);
    QCOMPARE(e.shelve(channels[0], Engine::MaximumShelfMinutes, "maximum", "tester", true), 1);
    QCOMPARE(e.state(channels[0]).shelf.until, time + qint64(365) * 24 * 60 * 60000);
    e.unshelve(channels[0]->parent);
    QCOMPARE(e.presentation(root).shelved, 1);
    QCOMPARE(e.presentation(root).unack, 2);
    QCOMPARE(e.presentation(root).counts[2], 2);
    e.unshelve(root); QCOMPARE(e.presentation(root).shelved, 0);
    QCOMPARE(e.presentation(root).counts[2], 3);
    // Clock moves back; absolute expiry is retained. A forward jump expires it.
    e.shelve(root, 1, "clock", "tester"); const auto deadline = e.state(channels[0]).shelf.until;
    time -= 100000; e.tick(); QCOMPARE(e.presentation(root).shelved, 3);
    time = deadline + 86400000; e.tick(); QCOMPARE(e.presentation(root).shelved, 0);
  }
  void shelvingPreservesExternalEffects_data() {
    QTest::addColumn<bool>("global"); QTest::addColumn<bool>("passive");
    QTest::newRow("local") << false << false;
    QTest::newRow("global") << true << false;
    QTest::newRow("passive-local") << false << true;
    QTest::newRow("passive-global") << true << true;
  }
  void shelvingPreservesExternalEffects() {
    QFETCH(bool, global); QFETCH(bool, passive);
    const QString config = "GROUP NULL root\n$SEVRPV groupSevr\n$SEVRCOMMAND UP_ANY groupUp\n"
        "$SEVRCOMMAND DOWN_ANY groupDown\nCHANNEL root pv\n$SEVRPV pvSevr\n$ACKPV ack 1\n"
        "$SEVRCOMMAND UP_ANY up\n$SEVRCOMMAND DOWN_ANY down\n$STATCOMMAND HIHI status\n";
    auto a = parseConfig(config), b = parseConfig(config);
    FakePv pa, pb; EngineOptions o; o.global = global; o.passive = passive;
    Engine ea(a, o, &pa), eb(b, o, &pb);
    QStringList ca, cb, la, lb; qint64 time = 1000;
    ea.now = ea.monotonicNow = eb.now = eb.monotonicNow = [&] { return time; };
    ea.command = [&](const QString& text) { ca << text; };
    eb.command = [&](const QString& text) { cb << text; };
    ea.alarmLog = [&](Node*, const State& st, qint64) { la << QString::number(st.severity); };
    eb.alarmLog = [&](Node*, const State& st, qint64) { lb << QString::number(st.severity); };
    ea.start(); eb.start();
    auto na = a.channels()[0], nb = b.channels()[0];
    ea.event(na, {}); eb.event(nb, {});
    ea.shelve(na, 1, "maintenance", "tester");
    for (const Event& ev : {Event{3, 1, 1, 1, {}}, Event{3, 2, 2, 1, {}}, Event{0, 0, 2, 1, {}}, Event{0, 0, 0, 1, {}}, Event{0, 4, 4, 1, {}}, Event{0, 0, 4, 1, {}}}) {
      ea.event(na, ev); eb.event(nb, ev);
      QVERIFY(!ea.audible());
      QCOMPARE(ea.state(na).unack, eb.state(nb).unack);
    }
    time += 60000; ea.tick(); eb.tick();
    QCOMPARE(ca, cb); QCOMPARE(la, lb); QCOMPARE(pa.writes.size(), pb.writes.size());
    for (int i = 0; i < pa.writes.size(); ++i) {
      QCOMPARE(pa.writes[i].name, pb.writes[i].name);
      QCOMPARE(pa.writes[i].value, pb.writes[i].value);
      QCOMPARE(pa.writes[i].kind, pb.writes[i].kind);
    }
    QCOMPARE(ea.state(a.root.get()).severity, eb.state(b.root.get()).severity);
    QCOMPARE(ea.presentation(a.root.get()).unack, eb.presentation(b.root.get()).unack);
    const int count = pa.writes.size(); const int commands = ca.size();
    ea.shelve(na, 1, "again", "tester"); ea.shelve(na, 2, "extend", "tester", true); ea.unshelve(na);
    QCOMPARE(pa.writes.size(), count); QCOMPARE(ca.size(), commands);
  }
  void shelvingMaskAndFilterInteractions() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER -1 2\n"
                         "$FORCEPV gate -D--- 1 0\n");
    Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0]; auto root = d.root.get();
    e.event(n, {}); e.shelve(n, 1, "filter", "tester");
    const auto until = e.state(n).shelf.until;
    e.event(n, {3, 2, 0, 1, {}}); time += 2000; e.tick();
    QCOMPARE(e.state(n).severity, 2); QCOMPARE(e.presentation(root).severity, 0);
    e.noAck(n, true); QCOMPARE(e.state(n).shelf.until, until);
    e.noAck(n, false); QCOMPARE(e.state(n).shelf.until, until);
    e.forceValue(n, 1); QVERIFY(e.state(n).mask[Disable]);
    time = until; e.tick(); QVERIFY(!e.state(n).shelf.until);
    QCOMPARE(e.presentation(root).severity, 0);
    e.forceValue(n, 0); QCOMPARE(e.presentation(root).severity, 2);
    e.shelve(n, 1, "cancel", "tester"); e.setMask(n, Mask::parse("C"));
    time += 60000; e.tick(); QCOMPARE(e.presentation(root).severity, 0);
    e.resetMask(n); e.event(n, {3, 2, 0, 1, {}}); time += 2000; e.tick();
    e.shelve(n, 1, "beep", "tester"); e.setBeep(root, 3); e.unshelve(n);
    QVERIFY(!e.audible()); e.setBeep(root, 1); QVERIFY(e.audible());
    e.shelve(n, 1, "silence", "tester"); e.silenceForever = true;
    time += 60000; e.tick(); QVERIFY(!e.audible());
    e.silenceForever = false; e.silenceUntil = time + 1000; QVERIFY(!e.audible());
    time += 1000; QVERIFY(e.audible());
  }
  void shelvingReloadIdentityAndLatches() {
    const QString original = "GROUP NULL root\nGROUP root a\nCHANNEL a same\nCHANNEL a removed\n"
                             "GROUP root b\nCHANNEL b same\n";
    auto d = parseConfig(original); Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    for (auto n : d.channels()) { e.event(n, {3, 2, 0, 1, {}}); e.event(n, {}); }
    e.shelve(d.root.get(), 10, "work", "tester"); auto saved = e.shelves();
    auto replacement = parseConfig("GROUP NULL root\nGROUP root b\nCHANNEL b same\n"
        "GROUP root a\nCHANNEL a added\nCHANNEL a same\n");
    Engine next(replacement); next.now = next.monotonicNow = [&] { return time; };
    QStringList operations; next.operation = [&](Node*, const QString& text, OperationKind) { operations << text; };
    next.restoreShelves(saved);
    QCOMPARE(next.presentation(replacement.root.get()).shelved, 2);
    QCOMPARE(operations.size(), 1); QVERIFY(operations[0].contains("/root/a/removed"));
    for (auto n : replacement.channels()) next.event(n, {});
    auto channels = replacement.channels();
    QCOMPARE(next.state(channels[0]).unack, 2); QCOMPARE(next.state(channels[2]).unack, 2);
    QCOMPARE(next.state(channels[1]).shelf.until, qint64(0));
    QCOMPARE(next.state(replacement.root.get()).unackCounts[2], 2);
    time += 600000; next.tick();
    QCOMPARE(next.presentation(replacement.root.get()).unackCounts[2], 2);
    next.acknowledge(replacement.root.get());
    QCOMPARE(next.presentation(replacement.root.get()).unack, 0);
    // Expired during reload, latches still return; a new runtime inherits nothing.
    Engine expired(d); expired.now = expired.monotonicNow = [&] { return time; }; expired.restoreShelves(saved);
    for (auto n : d.channels()) expired.event(n, {});
    QCOMPARE(expired.presentation(d.root.get()).shelved, 0);
    QCOMPARE(expired.presentation(d.root.get()).unack, 2);
    Engine fresh(d); QCOMPARE(fresh.presentation(d.root.get()).shelved, 0);
    Engine global(d, {true}); global.now = global.monotonicNow = [&] { return qint64(1000); }; global.restoreShelves(saved);
    for (auto n : d.channels()) global.event(n, {0, 0, 0, 1, {}});
    global.unshelve(d.root.get()); QCOMPARE(global.presentation(d.root.get()).unack, 0);
  }
  void shelvingAmbiguousReload() {
    const QString config = "GROUP NULL root\nCHANNEL root same\nCHANNEL root same\n";
    auto d = parseConfig(config); Engine e(d);
    e.shelve(d.channels()[0], 60, "duplicate", "tester"); auto saved = e.shelves();
    QVERIFY(!saved[0].unique);
    auto single = parseConfig("GROUP NULL root\nCHANNEL root same\n"); Engine next(single);
    next.restoreShelves(saved); QCOMPARE(next.presentation(single.root.get()).shelved, 0);
    next.shelve(single.root.get(), 60, "unique", "tester");
    Engine duplicate(d); duplicate.restoreShelves(next.shelves());
    QCOMPARE(duplicate.presentation(d.root.get()).shelved, 0);
  }
  void shelvingTenThousandChannels() {
    QString config = "GROUP NULL root\n";
    for (int i = 0; i < 10000; ++i) config += QString("CHANNEL root pv%1\n").arg(i);
    auto d = parseConfig(config); Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    QElapsedTimer elapsed; elapsed.start();
    e.shelve(d.root.get(), 1, "large group", "tester");
    for (auto n : d.channels()) e.event(n, {3, 2, 0, 1, {}});
    QCOMPARE(e.presentation(d.root.get()).shelved, 10000);
    QCOMPARE(e.presentation(d.root.get()).severity, 0);
    time += 60000; e.tick();
    QCOMPARE(e.presentation(d.root.get()).counts[2], 10000);
    QCOMPARE(e.presentation(d.root.get()).unackCounts[2], 10000);
    e.acknowledge(d.root.get()); QCOMPARE(e.presentation(d.root.get()).unack, 0);
    qInfo() << "10000-channel shelf, events, expiry and acknowledgement (ms):" << elapsed.elapsed();
  }

  void sampleRoundTrip() {
    auto d = loadConfig("../alh/test.alhConfig");
    QCOMPARE(d.channels().size(), 10);
    auto s = writeConfig(d);
    QCOMPARE(writeConfig(parseConfig(s)), s);
  }
  void directives() {
    auto d = parseConfig(R"($BEEPSEVERITY MAJOR
GROUP NULL root
$HEARTBEATPV beat 0.1 7
$SEVRPV severity
$FORCEPV CALC -D--- 1 NE
$FORCEPV_CALC A+B
$FORCEPV_CALC_A one
$FORCEPV_CALC_B two
$FORCEPV_CALC_C three
$FORCEPV_CALC_D four
$FORCEPV_CALC_E five
$FORCEPV_CALC_F six
$SEVRCOMMAND UP_ALARM echo alarm
$SEVRCOMMAND DOWN_ANY echo clear
$BEEPSEVR INVALID
$GUIDANCE
A line of operator guidance
$END
CHANNEL root test CDATL
$ACKPV ack 5
$ALARMCOUNTFILTER -1 2
$ALIAS friendly
$COMMAND xload
$STATCOMMAND HIHI echo high
$GUIDANCE https://example.invalid/guide
)");
    QCOMPARE(d.beepSeverity, 2);
    QCOMPARE(d.channels()[0]->mask.text(), QString("CDATL"));
    auto s = writeConfig(d);
    QCOMPARE(writeConfig(parseConfig(s)), s);
  }
  void startupSeverityCommands_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<int>("severity");
    for (bool global : {false, true})
      for (int severity : {0, 2, 4})
        QTest::newRow(qPrintable(QString("%1-%2").arg(global ? "global" : "local").arg(severity)))
            << global << severity;
  }
  void startupSeverityCommands() {
    QFETCH(bool, global);
    QFETCH(int, severity);
    const QString config = "GROUP NULL root\nCHANNEL root pv\n"
        "$SEVRCOMMAND UP_ANY up-any\n$SEVRCOMMAND UP_ALARM up-alarm\n"
        "$SEVRCOMMAND UP_MAJOR up-major\n$SEVRCOMMAND UP_ERROR up-error\n"
        "$SEVRCOMMAND DOWN_ANY down-any\n$SEVRCOMMAND DOWN_MAJOR down-major\n"
        "$SEVRCOMMAND DOWN_NO_ALARM down-normal\n";
    auto d = parseConfig(config);
    for (int load = 0; load < 2; ++load) {
      Engine e(d, {global});
      QStringList commands;
      e.command = [&](const QString& command) { commands << command; };
      auto n = d.channels()[0];
      e.event(n, {severity == 4 ? ALARM_NSTATUS : severity == 2 ? 3 : 0, severity, severity, 1, "0"});
      const QStringList expected = severity == 4 ? QStringList()
          : QStringList({"down-any", severity == 2 ? "down-major" : "down-normal"});
      QCOMPARE(commands, expected);
      commands.clear();
      e.event(n, {severity == 4 ? ALARM_NSTATUS : severity == 2 ? 3 : 0, severity, severity, 1, "1"});
      QVERIFY(commands.isEmpty());
      e.event(n, {});
      commands.clear();
      e.event(n, {3, 2, 2, 1, "12"});
      QCOMPARE(commands, QStringList({"up-any", "up-alarm", "up-major"}));
    }
  }
  void repeatedBeepSeverity() {
    auto d = parseConfig("GROUP NULL root\n$BEEPSEVR INVALID\n$BEEPSEVR MINOR\n"
        "CHANNEL root pv\n$BEEPSEVR MAJOR\n$BEEPSEVR MINOR\n"
        "$ALIAS first alias\n$ALIAS second alias\n");
    for (int load = 0; load < 2; ++load) {
      QCOMPARE(d.root->option("BEEPSEVR"), QString("MINOR"));
      auto n = d.channels()[0];
      QCOMPARE(n->option("BEEPSEVR"), QString("MINOR"));
      QCOMPARE(n->label(), QString("first alias"));
      Engine e(d);
      e.event(n, {}); e.event(n, {4, 1, 1, 1, "7"});
      QVERIFY(e.audible());
      d = parseConfig(writeConfig(d));
    }
  }
  void repeatedOutputAndCalculationDirectives() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"
        "$ACKPV old-output 1\n$ACKPV new-output 2\n"
        "$FORCEPV CALC D 1 0\n$FORCEPV_CALC 0\n$FORCEPV_CALC 1\n"
        "$COMMAND first\n$COMMAND second\n");
    for (int load = 0; load < 2; ++load) {
      auto n = d.channels()[0];
      FakePv pv;
      Engine e(d, {true}, &pv);
      e.start();
      QVERIFY(e.state(n).mask[Disable]); // The final CALC expression forces the mask.
      e.setMask(n, {});
      e.event(n, {3, 2, 2, 1, "20"});
      pv.writes.clear();
      e.acknowledge(n);
      QCOMPARE(pv.writes.size(), 2);
      QCOMPARE(pv.writes[1].name, QString("new-output"));
      QCOMPARE(pv.writes[1].value, 2.0);
      QCOMPARE(n->option("COMMAND"), QString("first"));
      d = parseConfig(writeConfig(d));
    }
  }
  void declarationComments() {
    QTemporaryDir dir;
    QFile included(dir.filePath("child.alhConfig"));
    QVERIFY(included.open(QIODevice::WriteOnly));
    included.write("GROUP NULL child # included group\nCHANNEL child nested D # disabled\n");
    included.close();
    auto d = parseConfig("GROUP NULL root # facility\n"
        "CHANNEL root pv ----- # description\n"
        "$COMMAND echo '# keep command text' # and shell comment\n"
        "$GUIDANCE https://example.invalid/help#fragment\n"
        "$GUIDANCE\n# keep guidance text\n$END\n"
        "CHANNEL root unmasked # description without mask\n"
        "CHANNEL root pv#part D # hash within PV name\n"
        "INCLUDE root child.alhConfig # include comment\n", dir.path());
    for (int load = 0; load < 2; ++load) {
      QCOMPARE(d.channels().size(), 4);
      QCOMPARE(d.channels()[0]->mask.text(), QString("-----"));
      QCOMPARE(d.channels()[0]->option("COMMAND"),
               QString("echo '# keep command text' # and shell comment"));
      QCOMPARE(d.channels()[0]->option("GUIDANCE"), QString("https://example.invalid/help#fragment"));
      QCOMPARE(d.channels()[0]->option("GUIDANCE_TEXT"), QString("# keep guidance text"));
      QCOMPARE(d.channels()[1]->mask.text(), QString("-----"));
      QCOMPARE(d.channels()[2]->name, QString("pv#part"));
      QVERIFY(d.channels()[2]->mask[Disable]);
      QVERIFY(d.channels()[3]->mask[Disable]);
      d = parseConfig(writeConfig(d));
    }
    QVERIFY_THROWS_EXCEPTION(ParseError, parseConfig("GROUP NULL root extra # invalid\n"));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseConfig("GROUP NULL root\nCHANNEL root pv D extra # invalid\n"));
  }
  void countFilterDefaults_data() {
    QTest::addColumn<QString>("arguments");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("seconds");
    QTest::newRow("no-arguments") << "" << 1 << 1;
    QTest::newRow("count-only") << "2" << 2 << 1;
    QTest::newRow("explicit") << "-1 3" << -1 << 3;
  }
  void countFilterDefaults() {
    QFETCH(QString, arguments);
    QFETCH(int, count);
    QFETCH(int, seconds);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER " + arguments + "\n");
    const auto canonical = QString("%1 %2").arg(count).arg(seconds);
    QCOMPARE(d.channels()[0]->option("ALARMCOUNTFILTER"), canonical);
    d = parseConfig(writeConfig(d));
    QCOMPARE(d.channels()[0]->option("ALARMCOUNTFILTER"), canonical);
    Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0];
    QCOMPARE(e.state(n).filterCount, count); QCOMPARE(e.state(n).filterSeconds, seconds);
    e.event(n, {}); e.event(n, {3, 2, 2, 1, "12"});
    QCOMPARE(e.state(n).severity, 0);
    time += seconds * 1000 - 1; e.tick(); QCOMPARE(e.state(n).severity, 0);
    ++time; e.tick(); QCOMPARE(e.state(n).severity, 2);
  }
  void countFilterInvalidArguments() {
    for (const auto arguments : {"bad", "2 bad", "-2", "1 -1", "1000001", "1 2 3"})
      QVERIFY_THROWS_EXCEPTION(ParseError, parseConfig(QString("GROUP NULL root\nCHANNEL root pv\n"
          "$ALARMCOUNTFILTER ") + arguments + "\n"));
  }
  void combinedGuidance() {
    auto d = parseConfig("GROUP NULL root\n$GUIDANCE\nGroup first\n$END\n"
        "$GUIDANCE\nGroup second\n$END\nCHANNEL root pv\n"
        "$GUIDANCE\nFirst instruction\n\n$END\n$GUIDANCE\n$END\n"
        "$GUIDANCE\nSecond instruction\n$END\n");
    for (int load = 0; load < 2; ++load) {
      QCOMPARE(d.root->option("GUIDANCE_TEXT"), QString("Group first\nGroup second"));
      QCOMPARE(d.channels()[0]->option("GUIDANCE_TEXT"), QString("First instruction\n\nSecond instruction"));
      int blocks = 0;
      for (const auto& directive : d.channels()[0]->directives)
        if (directive.key == "GUIDANCE_TEXT") ++blocks;
      QCOMPARE(blocks, 1);
      d = parseConfig(writeConfig(d));
    }
  }

  void legacyMasks() {
    const QHash<QString, QString> masks = {{"D", "-D---"},     {"DA", "-DA--"}, {"---D-", "-D---"},
                                           {"LATDC", "CDATL"}, {"DD", "-D---"}, {"-", "-----"},
                                           {"0", "-----"}, {"DX", "-D---"}, {"cdatl", "-----"}};
    for (auto i = masks.cbegin(); i != masks.cend(); ++i) {
      auto d = parseConfig("GROUP NULL root\nCHANNEL root pv " + i.key() + "\n$FORCEPV gate " +
                           i.key() + " 1 0\n");
      QCOMPARE(d.channels()[0]->mask.text(), i.value());
      Engine e(d);
      e.setMask(d.channels()[0], Mask{});
      e.forceValue(d.channels()[0], 1);
      QCOMPARE(e.state(d.channels()[0]).mask.text(), i.value());
      QCOMPARE(parseConfig(writeConfig(d)).channels()[0]->mask.text(), i.value());
    }
  }
  void legacyForceValues_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("effective");
    QTest::newRow("linac order") << "gate 0 1 -D---" << "gate ----- 1 0";
    QTest::newRow("invalid force") << "gate D bad 2" << "gate -D--- 1 0";
    QTest::newRow("invalid reset") << "gate D 1 bad" << "gate -D--- 1 0";
    QTest::newRow("numeric prefix") << "gate D 1 0junk" << "gate -D--- 1 0";
    QTest::newRow("force suffix") << "gate D 1junk 2" << "gate -D--- 1 0";
    QTest::newRow("defaults") << "gate D" << "gate -D--- 1 0";
    QTest::newRow("pv only") << "gate" << "gate ----- 1 0";
    QTest::newRow("lowercase ne") << "gate D 1 ne" << "gate -D--- 1 NE";
    QTest::newRow("extra tokens") << "gate D 1 0 ignored" << "gate -D--- 1 0";
  }
  void legacyForceValues() {
    QFETCH(QString, input);
    QFETCH(QString, effective);
    auto d = parseConfig("GROUP NULL root\n$FORCEPV " + input +
                         "\nCHANNEL root pv ---T-\n");
    QCOMPARE(d.root->option("FORCEPV"), effective);
    Engine e(d);
    auto n = d.channels()[0];
    e.forceValue(d.root.get(), 1);
    QCOMPARE(e.state(n).mask.text(), effective.split(' ')[1]);
    e.forceValue(d.root.get(), 0);
    QCOMPARE(e.state(n).mask.text(), QString("---T-"));
    QCOMPARE(writeConfig(parseConfig(writeConfig(d))), writeConfig(d));
  }
  void siblingGroupNames() {
    auto d = parseConfig("GROUP NULL root\nGROUP root child\nCHANNEL child pv1\n"
                         "GROUP root child\nCHANNEL child pv2\n");
    auto saved = parseConfig(writeConfig(d));
    QCOMPARE(saved.root->children.size(), size_t(2));
    QCOMPARE(saved.root->children[0]->children[0]->name, QString("pv1"));
    QCOMPARE(saved.root->children[1]->children[0]->name, QString("pv2"));
  }
  void malformed_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("ancestor name") << "GROUP NULL root\nGROUP root root";
    QTest::newRow("distant ancestor name")
        << "GROUP NULL root\nGROUP root branch\nGROUP branch root";
    QTest::newRow("reserved group name") << "GROUP NULL root\nGROUP root NULL";
    QTest::newRow("missing root") << "CHANNEL root pv";
    QTest::newRow("bad parent") << "GROUP NULL root\nCHANNEL missing pv";
    QTest::newRow("guidance") << "GROUP NULL a\n$GUIDANCE\nunclosed";
    QTest::newRow("unknown") << "GROUP NULL a\n$UNKNOWN abc";
    QTest::newRow("bad count") << "GROUP NULL a\nCHANNEL a pv\n$ALARMCOUNTFILTER -2 4";
    QTest::newRow("bad calc") << "GROUP NULL a\n$FORCEPV_CALC !@#";
    QTest::newRow("missing calc") << "GROUP NULL a\n$FORCEPV CALC -D--- 1 NE";
    QTest::newRow("nonfinite heartbeat") << "GROUP NULL a\n$HEARTBEATPV beat nan 1";
    QTest::newRow("overflow heartbeat") << "GROUP NULL a\n$HEARTBEATPV beat 1e100 1";
    QTest::newRow("bad heartbeat") << "GROUP NULL a\n$HEARTBEATPV beat 0 1";
  }
  void malformed() {
    QFETCH(QString, text);
    QVERIFY_THROWS_EXCEPTION(ParseError, parseConfig(text));
  }
  void calcRequiresExpression() {
    QTemporaryDir dir;
    const QString invalid = "GROUP NULL root\nCHANNEL root pv\n$FORCEPV CALC -D--- 1 NE\n";
    QFile child(dir.filePath("child"));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.write(invalid.toUtf8());
    child.close();
    QVERIFY_THROWS_EXCEPTION(ParseError, loadConfig(child.fileName()));
    QVERIFY_THROWS_EXCEPTION(ParseError,
                             parseConfig("GROUP NULL parent\nINCLUDE parent child\n", dir.path()));
    // The expression may precede the FORCEPV directive or follow an include.
    auto d = parseConfig("GROUP NULL root\n$FORCEPV_CALC 1\n$FORCEPV CALC -D--- 1 NE\n");
    QCOMPARE(d.root->option("FORCEPV_CALC"), QString("1"));
    QFile expression(dir.filePath("expression"));
    QVERIFY(expression.open(QIODevice::WriteOnly));
    expression.write("$FORCEPV_CALC 1\n");
    expression.close();
    d = parseConfig("GROUP NULL parent\n$FORCEPV CALC -D--- 1 NE\n"
                    "INCLUDE parent expression\n",
                    dir.path());
    QCOMPARE(d.root->option("FORCEPV_CALC"), QString("1"));
  }
  void includes() {
    QTemporaryDir temp;
    QFile f(temp.filePath("child"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("GROUP NULL child\nCHANNEL child pv\n");
    f.close();
    auto d = parseConfig("GROUP NULL root\nINCLUDE root child", temp.path());
    QCOMPARE(d.channels().size(), 1);
    QCOMPARE(d.root->children[0]->parent, d.root.get());
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("GROUP NULL child\nINCLUDE child child\n");
    f.close();
    QVERIFY_THROWS_EXCEPTION(ParseError,
                             parseConfig("GROUP NULL root\nINCLUDE root child", temp.path()));
  }
  void nestedIncludesUseConfiguredDirectory() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath("sub"));
    QVERIFY(QDir(dir.path()).mkpath("entry"));
    auto write = [&](const QString& path, const QByteArray& text) {
      QFile file(dir.filePath(path));
      return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
    };
    QVERIFY(write("entry/main", "GROUP NULL root\nINCLUDE root sub/child\n"));
    QVERIFY(write("sub/child", "GROUP NULL child\nINCLUDE child shared\n"));
    QVERIFY(write("shared", "GROUP NULL shared\nCHANNEL shared expected\n"));
    // A same-named file beside the included file must not shadow the configured one.
    QVERIFY(write("sub/shared", "GROUP NULL wrong\nCHANNEL wrong unexpected\n"));
    auto d = loadConfig(dir.filePath("entry/main"), dir.path());
    QCOMPARE(d.channels().size(), 1);
    QCOMPARE(d.channels()[0]->name, QString("expected"));
    QCOMPARE(d.channels()[0]->parent->parent->name, QString("child"));
    QCOMPARE(writeConfig(parseConfig(writeConfig(d))), writeConfig(d));
    auto parsed = parseConfig("GROUP NULL root\nINCLUDE root sub/child\n", dir.path());
    QCOMPARE(parsed.channels()[0]->name, QString("expected"));
    QVERIFY(write("shared", "GROUP NULL shared\nINCLUDE shared sub/child\n"));
    QVERIFY_THROWS_EXCEPTION(ParseError, loadConfig(dir.filePath("entry/main"), dir.path()));
  }
  void valueUpdatesDoNotLogAlarms() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    Engine e(d, {true});
    auto n = d.channels()[0];
    int logs = 0;
    e.alarmLog = [&](Node*, const State&, qint64) { ++logs; };
    e.event(n, {0, 0, 0, 1, "0"});
    for (int value = 1; value <= 10; ++value)
      e.event(n, {0, 0, 0, 1, QString::number(value)});
    QCOMPARE(logs, 0);
    QCOMPARE(e.state(n).value, QString("10"));
    e.event(n, {3, 2, 2, 1, "20"});
    QCOMPARE(logs, 1);
    e.event(n, {3, 2, 2, 1, "21"});
    QCOMPARE(logs, 1);
    e.event(n, {3, 2, 0, 1, "21"}); // ACKS-only change
    QCOMPARE(logs, 2);
    e.event(n, {3, 2, 0, 0, "21"}); // ACKT-only change
    QCOMPARE(logs, 3);
    e.event(n, {3, 2, 0, 0, "21"}); // duplicate callback
    QCOMPARE(logs, 3);
    e.resetMask(n); // The operator mask changes ahead of the IOC callback.
    e.event(n, {3, 2, 0, 1, "21"});
    QCOMPARE(logs, 4);
    e.setMask(n, Mask::parse("----L"));
    e.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(logs, 4);
  }
  void enableRunsSeverityCommands_data() {
    QTest::addColumn<bool>("global");
    QTest::newRow("local") << false;
    QTest::newRow("global") << true;
  }
  void enableRunsSeverityCommands() {
    QFETCH(bool, global);
    auto d = parseConfig("GROUP NULL root\n$SEVRCOMMAND UP_ALARM group-notify\n"
                         "CHANNEL root pv\n$SEVRCOMMAND UP_MAJOR channel-major\n"
                         "$SEVRCOMMAND UP_ALARM channel-alarm\n");
    Engine e(d, {global});
    auto n = d.channels()[0];
    QStringList commands;
    e.command = [&](const QString& command) { commands << command; };
    e.event(n, {0, 0, 0, 1, "0"});
    e.setMask(n, Mask::parse("-D---"));
    e.event(n, {3, 2, 2, 1, "20"});
    QVERIFY(commands.isEmpty());
    e.resetMask(d.root.get());
    QCOMPARE(commands.count("channel-major"), 1);
    QCOMPARE(commands.count("channel-alarm"), 1);
    QCOMPARE(commands.count("group-notify"), 1);
    QCOMPARE(e.state(d.root.get()).unack, 2);
    e.resetMask(n); // An unchanged mask must not repeat the command.
    QCOMPARE(commands.size(), 3);
    commands.clear();
    e.setMask(n, Mask::parse("CD---"));
    e.setMask(n, Mask::parse("C----"));
    QVERIFY(commands.isEmpty()); // Still cancelled.
  }
  void forceCalcRoundedNeReset_data() {
    QTest::addColumn<bool>("group"); QTest::addColumn<QString>("reset");
    for (bool group : {false, true})
      for (const auto& reset : {QString("NE"), QString("16777216")})
        QTest::newRow(qPrintable(QString(group ? "group-" : "channel-") + reset)) << group << reset;
  }
  void forceCalcRoundedNeReset() {
    QFETCH(bool, group); QFETCH(QString, reset);
    const QString force = "$FORCEPV CALC D 16777216 " + reset +
        "\n$FORCEPV_CALC A\n$FORCEPV_CALC_A input\n";
    auto d = parseConfig("GROUP NULL root\n" + (group ? force : QString()) +
        "CHANNEL root pv\n" + (group ? QString() : force));
    d = parseConfig(writeConfig(d));
    FakePv pv; Engine e(d, {}, &pv); e.start();
    auto n = d.channels()[0]; e.event(n, {});
    QVERIFY(pv.numbers.contains("input"));
    auto update = pv.numbers.value("input");
    // 16777217 rounds to the force value as a float, but differs as a double.
    // ALH applies D here and then fails to reset it; Qt intentionally corrects this.
    update(16777217); QVERIFY(e.state(n).mask[Disable]);
    update(16777220); QVERIFY(!e.state(n).mask[Disable]);
    // Moving within the rounded forced value must keep the mask applied.
    update(16777216); QVERIFY(e.state(n).mask[Disable]);
    update(16777217); QVERIFY(e.state(n).mask[Disable]);
    update(16777220); QVERIFY(!e.state(n).mask[Disable]);
  }
  void forceScalarPrecision() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"
                         "$FORCEPV gate -D--- 16777216 16777220\n");
    Engine e(d);
    auto n = d.channels()[0];
    e.forceValue(n, 16777217);
    QVERIFY(!e.state(n).mask[Disable]);
    e.forceValue(n, 16777216);
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, 16777221);
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, 16777220);
    QVERIFY(!e.state(n).mask[Disable]);
    e.configureForce(n, {{"FORCEPV", "gate -D--- 16777216 NE"}}, false);
    e.forceValue(n, 16777216);
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, 16777217);
    QVERIFY(!e.state(n).mask[Disable]);
    // Preserve the legacy rounding rule for CALC results.
    e.configureForce(n, {{"FORCEPV", "CALC -D--- 16777216 NE"}}, false);
    e.forceValue(n, 16777217);
    QVERIFY(e.state(n).mask[Disable]);
  }
  void suppressedGlobalTransients_data() {
    QTest::addColumn<bool>("disable");
    QTest::addColumn<bool>("passive");
    QTest::addColumn<bool>("writable");
    QTest::newRow("NoAck") << false << false << true;
    QTest::newRow("Disable") << true << false << true;
    QTest::newRow("passive-NoAck") << false << true << true;
    QTest::newRow("passive-Disable") << true << true << true;
    QTest::newRow("failed-ack") << false << false << false;
  }
  void suppressedGlobalTransients() {
    QFETCH(bool, disable);
    QFETCH(bool, passive);
    QFETCH(bool, writable);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ACKPV ack 7\n");
    FakePv pv;
    pv.writable = writable;
    Engine e(d, {true, passive}, &pv);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    if (disable)
      e.setMask(n, Mask::parse("-D---"));
    else
      e.noAck(d.root.get(), true);
    e.event(n, {3, 2, 2, 1, "20"});
    e.event(n, {0, 0, 2, 1, "0"});
    QVERIFY(!e.audible());
    if (disable)
      e.resetMask(n);
    else {
      time += 3600000;
      e.tick();
    }
    QCOMPARE(e.state(n).severity, 0);
    if (passive) {
      QVERIFY(pv.writes.isEmpty());
      QCOMPARE(e.state(n).unack, 2);
    } else if (!writable) {
      QCOMPARE(pv.writes.size(), 1);
      QCOMPARE(e.state(n).unack, 2);
    } else {
      QCOMPARE(pv.writes.size(), 2);
      QCOMPARE(pv.writes[0].name, QString("pv"));
      QCOMPARE(pv.writes[0].kind, WriteKind::Acknowledge);
      QCOMPARE(pv.writes[0].value, 2.0);
      QCOMPARE(pv.writes[1].name, QString("ack"));
      QCOMPARE(pv.writes[1].value, 7.0);
      QCOMPARE(e.state(n).unack, 0);
      QCOMPARE(e.state(d.root.get()).unack, 0);
      QVERIFY(!e.audible());
    }
    pv.writes.clear();
    e.setMask(n, Mask::parse("--A--"));
    e.event(n, {3, 2, 2, 1, "20"});
    e.resetMask(n); // Active alarms still require an operator acknowledgement.
    QCOMPARE(e.state(n).unack, 2);
    QVERIFY(pv.writes.isEmpty());
  }
  void disabledStartupGroupCommands_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::addColumn<int>("severity");
    for (int severity : {0, 2, 4}) {
      QTest::newRow(qPrintable(QString("local-%1").arg(severity))) << false << false << severity;
      QTest::newRow(qPrintable(QString("global-%1").arg(severity))) << true << false << severity;
      QTest::newRow(qPrintable(QString("passive-%1").arg(severity))) << true << true << severity;
    }
  }
  void disabledStartupGroupCommands() {
    QFETCH(bool, global); QFETCH(bool, passive); QFETCH(int, severity);
    auto d = parseConfig("GROUP NULL root\n$SEVRPV root-output\n"
                         "$SEVRCOMMAND UP_ANY root-up\n$SEVRCOMMAND DOWN_ANY root-down\n"
                         "GROUP root branch\n$SEVRPV branch-output\n"
                         "$SEVRCOMMAND UP_ANY branch-up\n$SEVRCOMMAND DOWN_ANY branch-down\n"
                         "CHANNEL branch disabled D\nCHANNEL branch enabled\n");
    FakePv pv; Engine e(d, {global, passive}, &pv);
    QStringList commands;
    e.command = [&](const QString& command) { commands << command; };
    e.start();
    auto disabled = d.channels()[0], enabled = d.channels()[1];
    e.event(disabled, {});
    e.event(disabled, {3, 2, 2, 1, "2"});
    e.event(disabled, {});
    QVERIFY(commands.isEmpty());
    QVERIFY(pv.writes.isEmpty());
    QCOMPARE(e.state(d.root.get()).severity, 4); // The sibling still awaits observation.
    e.event(enabled, {severity ? 3 : 0, severity, severity, 1, "first"});
    QCOMPARE(commands, severity ? QStringList({"branch-up", "root-up"}) : QStringList());
    QCOMPARE(e.state(d.root.get()).severity, severity);
    QCOMPARE(pv.writes.size(), global && !passive ? 2 : 0);
    for (const auto& write : pv.writes) {
      QCOMPARE(write.kind, WriteKind::Severity);
      QCOMPARE(write.value, double(severity));
    }
    commands.clear(); pv.writes.clear();
    e.event(disabled, {3, 2, 2, 1, "again"});
    e.event(enabled, {severity ? 3 : 0, severity, 0, 1, "value-only-or-ack"});
    QVERIFY(commands.isEmpty());
    QVERIFY(pv.writes.isEmpty());
    e.event(enabled, {});
    QCOMPARE(commands, severity ? QStringList({"branch-down", "root-down"}) : QStringList());
  }
  void combinedCancelDisable_data() {
    QTest::addColumn<bool>("group"); QTest::addColumn<bool>("automatic");
    QTest::newRow("channel-manual") << false << false;
    QTest::newRow("group-manual") << true << false;
    QTest::newRow("channel-force") << false << true;
    QTest::newRow("group-force") << true << true;
  }
  void combinedCancelDisable() {
    QFETCH(bool, group); QFETCH(bool, automatic);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    FakePv pv; Engine e(d, {}, &pv); e.start();
    auto n = d.channels()[0], target = group ? d.root.get() : n;
    e.event(n, {}); e.event(n, {3, 2, 2, 1, "2"}); e.acknowledge(n);
    e.setMask(target, Mask::parse("CD"), automatic);
    QCOMPARE(e.state(n).severity, 0);
    QCOMPARE(e.state(n).unack, 0);
    QVERIFY(!pv.alarms.contains(n->name));
    e.setMask(target, Mask::parse("C"), automatic);
    QCOMPARE(e.state(n).unack, 0);
    e.setMask(target, {}, automatic);
    QVERIFY(pv.alarms.contains(n->name));
    pv.alarms[n->name]({});
    QCOMPARE(e.state(n).severity, 0);
    QCOMPARE(e.state(n).unack, 0);
    QCOMPARE(e.state(d.root.get()).unack, 0);
    QVERIFY(!e.audible());
    // A real alarm following resubscription must still latch normally.
    pv.alarms[n->name]({3, 2, 2, 1, "2"});
    QCOMPARE(e.state(n).unack, 2);
    QVERIFY(e.audible());
  }
  void enableWhileCancelled() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine e(d); auto n = d.channels()[0];
    e.event(n, {}); e.event(n, {3, 2, 2, 1, "2"});
    e.setMask(n, Mask::parse("D"));
    e.setMask(n, Mask::parse("CD"));
    // A previously disabled channel keeps its cached severity when cancelled.
    QCOMPARE(e.state(n).severity, 2);
    e.setMask(n, Mask::parse("C"));
    QCOMPARE(e.state(n).unack, 0);
    QVERIFY(!e.audible());
    e.setMask(n, {});
    QCOMPARE(e.state(n).unack, 2); // Restore the cached alarm on Add, as ALH does.
    e.event(n, {3, 2, 2, 1, "2"});
    QCOMPARE(e.state(d.root.get()).unack, 2);
  }
  void initialGroupError_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::newRow("global") << true << false;
    QTest::newRow("local") << false << false;
    QTest::newRow("passive") << true << true;
  }
  void initialGroupError() {
    QFETCH(bool, global);
    QFETCH(bool, passive);
    auto d = parseConfig("GROUP NULL root\n$SEVRPV root-output\n"
                         "$SEVRCOMMAND UP_ERROR root-error\nGROUP root child\n"
                         "$SEVRPV child-output\n$SEVRCOMMAND UP_ERROR child-error\n"
                         "CHANNEL child missing\n");
    FakePv pv;
    Engine e(d, {global, passive}, &pv);
    QStringList commands;
    e.command = [&](QString s) { commands << s; };
    auto n = d.channels()[0];
    e.start();
    QCOMPARE(e.state(d.root.get()).severity, 4);
    e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QCOMPARE(commands, QStringList({"child-error", "root-error"}));
    QCOMPARE(pv.writes.size(), global && !passive ? 2 : 0);
    for (const auto& write : pv.writes) {
      QCOMPARE(write.kind, WriteKind::Severity);
      QCOMPARE(write.value, 4.0);
    }
    commands.clear();
    pv.writes.clear();
    e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QVERIFY(commands.isEmpty());
    QVERIFY(pv.writes.isEmpty());
    e.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(e.state(d.root.get()).severity, 0);
    QCOMPARE(pv.writes.size(), global && !passive ? 2 : 0);
    for (const auto& write : pv.writes)
      QCOMPARE(write.value, 0.0);
  }
  void initialChannelError_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::newRow("local") << false << false;
    QTest::newRow("global") << true << false;
    QTest::newRow("passive") << true << true;
  }
  void initialChannelError() {
    QFETCH(bool, global);
    QFETCH(bool, passive);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root missing\n$SEVRPV output\n"
                         "$SEVRCOMMAND UP_ERROR notify\n");
    FakePv pv;
    Engine e(d, {global, passive}, &pv);
    QStringList commands;
    e.command = [&](QString command) { commands << command; };
    auto n = d.channels()[0];
    e.start();
    QVERIFY(pv.writes.isEmpty()); // Await the real startup result.
    e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QVERIFY(commands.isEmpty()); // ERROR -> ERROR is not a severity transition in ALH.
    QCOMPARE(e.state(d.root.get()).severity, 4);
    QCOMPARE(pv.writes.size(), global && !passive ? 1 : 0);
    if (!pv.writes.isEmpty()) {
      QCOMPARE(pv.writes[0].name, QString("output"));
      QCOMPARE(pv.writes[0].value, 4.0);
      QCOMPARE(pv.writes[0].kind, WriteKind::Severity);
    }
    pv.writes.clear();
    commands.clear();
    e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QVERIFY(pv.writes.isEmpty());
    QVERIFY(commands.isEmpty());
  }
  void initiallyDisabledOutput_data() {
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::addColumn<QString>("mask");
    QTest::newRow("disabled") << true << false << "-D---";
    QTest::newRow("cancelled-disabled") << true << false << "CD---";
    QTest::newRow("local") << false << false << "-D---";
    QTest::newRow("passive") << true << true << "-D---";
  }
  void initiallyDisabledOutput() {
    QFETCH(bool, global);
    QFETCH(bool, passive);
    QFETCH(QString, mask);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv " + mask + "\n$SEVRPV output\n");
    FakePv pv;
    Engine e(d, {global, passive}, &pv);
    e.start();
    QCOMPARE(pv.writes.size(), global && !passive ? 1 : 0);
    if (!pv.writes.isEmpty()) {
      QCOMPARE(pv.writes[0].name, QString("output"));
      QCOMPARE(pv.writes[0].value, -1.0);
      QCOMPARE(pv.writes[0].kind, WriteKind::Severity);
    }
    pv.writes.clear();
    e.start();
    e.event(d.channels()[0], {3, 2, 2, 1, "20"});
    QVERIFY(pv.writes.isEmpty());
    QCOMPARE(e.state(d.root.get()).severity, 0);
  }
  void groupsWithoutMonitors_data() {
    QTest::addColumn<QString>("channels");
    QTest::addColumn<bool>("global");
    QTest::addColumn<bool>("passive");
    QTest::newRow("empty") << "" << true << false;
    QTest::newRow("cancelled") << "CHANNEL branch pv C----\n" << true << false;
    QTest::newRow("cancelled-disabled") << "CHANNEL branch pv CD---\n" << true << false;
    QTest::newRow("disabled") << "CHANNEL branch pv -D---\n" << true << false;
    QTest::newRow("local") << "CHANNEL branch pv C----\n" << false << false;
    QTest::newRow("passive") << "CHANNEL branch pv C----\n" << true << true;
  }
  void groupsWithoutMonitors() {
    QFETCH(QString, channels);
    QFETCH(bool, global);
    QFETCH(bool, passive);
    auto d = parseConfig("GROUP NULL root\n$SEVRPV root-output\nGROUP root branch\n"
                         "$SEVRPV branch-output\n" +
                         channels);
    FakePv pv;
    Engine e(d, {global, passive}, &pv);
    e.start();
    QCOMPARE(pv.writes.size(), global && !passive ? 2 : 0);
    for (const auto& write : pv.writes) {
      QCOMPARE(write.value, 0.0);
      QCOMPARE(write.kind, WriteKind::Severity);
    }
    pv.writes.clear();
    e.start();
    for (auto n : d.channels()) e.event(n, {3, 2, 2, 1, "2"});
    QVERIFY(pv.writes.isEmpty());
  }
  void emptyBranchBesideMonitoredChannel() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root active\nGROUP root empty\n"
                         "$SEVRPV empty-output\n");
    FakePv pv;
    Engine e(d, {true}, &pv);
    e.start();
    QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(pv.writes[0].name, QString("empty-output"));
    QCOMPARE(pv.writes[0].value, 0.0);
  }
  void explicitRelativePaths_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("useEnvironment");
    for (bool environment : {false, true})
      for (const auto& path : {"file", "nested/file", "./file", "../file", "/tmp/qtalh-absolute"})
        QTest::newRow(qPrintable(QString(environment ? "environment-" : "flags-") + path))
            << QString(path) << environment;
  }
  void explicitRelativePaths() {
    QFETCH(QString, path); QFETCH(bool, useEnvironment);
    struct Restore {
      QByteArray previous = qgetenv("ALARMHANDLER");
      ~Restore() { if (previous.isNull()) qunsetenv("ALARMHANDLER"); else qputenv("ALARMHANDLER", previous); }
    } restore;
    QTemporaryDir dir;
    const auto config = dir.filePath("configs"), logs = dir.filePath("logs");
    qputenv("ALARMHANDLER", config.toLocal8Bit());
    QStringList args{"qtalh", "-a", path, "-o", path};
    if (!useEnvironment) args << "-f" << config << "-l" << logs;
    args << path;
    const auto o = parseOptions(args);
    const bool explicitPath = path.startsWith('.') || QDir::isAbsolutePath(path);
    QCOMPARE(o.config, QFileInfo(explicitPath ? path : QDir(config).filePath(path)).absoluteFilePath());
    const auto expectedLog = QFileInfo(explicitPath ? path :
        QDir(useEnvironment ? config : logs).filePath(path)).absoluteFilePath();
    QCOMPARE(o.alarmFile, expectedLog); QCOMPARE(o.opmodFile, expectedLog);
  }
  void logDirectoryInheritance() {
    struct RestoreEnvironment {
      QByteArray previous = qgetenv("ALARMHANDLER");
      ~RestoreEnvironment() {
        if (previous.isNull())
          qunsetenv("ALARMHANDLER");
        else
          qputenv("ALARMHANDLER", previous);
      }
    } restore;
    QTemporaryDir dir;
    qputenv("ALARMHANDLER", dir.filePath("environment").toLocal8Bit());
    auto inherited = parseOptions({"qtalh", "test.alhConfig"});
    QCOMPARE(inherited.alarmFile, dir.filePath("environment/ALH-default.alhAlarm"));
    QCOMPARE(inherited.opmodFile, dir.filePath("environment/ALH-default.alhOpmod"));
    auto flag = parseOptions({"qtalh", "-f", dir.filePath("config"), "-a", "custom.alarm"});
    QCOMPARE(flag.alarmFile, dir.filePath("config/custom.alarm"));
    QCOMPARE(flag.opmodFile, dir.filePath("config/ALH-default.alhOpmod"));
    auto explicitDir =
        parseOptions({"qtalh", "-l", dir.filePath("logs"), "-f", dir.filePath("config")});
    QCOMPARE(explicitDir.alarmFile, dir.filePath("logs/ALH-default.alhAlarm"));
    auto explicitCurrent = parseOptions({"qtalh", "-l", "."});
    QCOMPARE(explicitCurrent.alarmFile, QDir::current().absoluteFilePath("ALH-default.alhAlarm"));
    auto absoluteFile = parseOptions({"qtalh", "-a", dir.filePath("absolute.alarm")});
    QCOMPARE(absoluteFile.alarmFile, dir.filePath("absolute.alarm"));
    qunsetenv("ALARMHANDLER");
    QCOMPARE(parseOptions({"qtalh"}).opmodFile,
             QDir::current().absoluteFilePath("ALH-default.alhOpmod"));
  }
  void loggingLockOptionsFollowConfiguration() {
    auto o = parseOptions({"qtalh", "-L", "first.alhConfig"});
    QVERIFY(o.lockFile.isEmpty()); // Resolve the default when the logging service opens.
    o = parseOptions({"qtalh", "-L", "-Lfile", "first.alhConfig", "first.alhConfig"});
    o.config = "second.alhConfig";
    QCOMPARE(o.lockFile, QString("first.alhConfig"));
  }
  void loggingRetentionDefaults() {
    QCOMPARE(parseOptions({"qtalh"}).maxRecords, 2000);
    QCOMPARE(parseOptions({"qtalh", "-L"}).maxRecords, 0);
    QCOMPARE(parseOptions({"qtalh", "-L", "-T"}).maxRecords, 0);
    QCOMPARE(parseOptions({"qtalh", "-L", "-m", "7"}).maxRecords, 7);
    QCOMPARE(parseOptions({"qtalh", "-m", "7", "-L"}).maxRecords, 7);
    QCOMPARE(parseOptions({"qtalh", "-L", "-m", "0"}).maxRecords, 0);
  }
  void severityOutputsUseRecoveryWrites() {
    auto d = parseConfig("GROUP NULL root\n$SEVRPV group-sev\n"
                         "CHANNEL root pv\n$SEVRPV channel-sev\n");
    FakePv pv;
    Engine e(d, {true}, &pv);
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    pv.writes.clear();
    e.event(n, {3, 2, 2, 1, "20"});
    QCOMPARE(pv.writes.size(), 2);
    for (const auto& write : pv.writes) {
      QCOMPARE(write.kind, WriteKind::Severity);
      QCOMPARE(write.value, 2.0);
    }
    pv.writes.clear();
    e.setMask(n, Mask::parse("-D---"));
    // ALH updates group state on Disable without publishing group SEVRPVs.
    QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(pv.writes[0].name, QString("channel-sev"));
    QCOMPARE(pv.writes[0].value, -1.0);
    QCOMPARE(e.state(d.root.get()).severity, 0);
    for (const auto& write : pv.writes)
      QCOMPARE(write.kind, WriteKind::Severity);
  }
  void normalizedSeverityCommands_data() {
    QTest::addColumn<QString>("trigger");
    QTest::addColumn<QString>("canonical");
    QTest::addColumn<int>("initial");
    QTest::addColumn<int>("finalSeverity");
    QTest::newRow("numeric") << "UP_2" << "UP_MAJOR" << 0 << 2;
    QTest::newRow("lowercase") << "UP_major" << "UP_MAJOR" << 0 << 2;
    QTest::newRow("mixed-case") << "DOWN_mInOr" << "DOWN_MINOR" << 2 << 1;
    QTest::newRow("normal-alias") << "DOWN_NO_ALARMS" << "DOWN_NO_ALARM" << 2 << 0;
  }
  void normalizedSeverityCommands() {
    QFETCH(QString, trigger);
    QFETCH(QString, canonical);
    QFETCH(int, initial);
    QFETCH(int, finalSeverity);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$SEVRCOMMAND " + trigger +
                         "  echo 'Case Sensitive'\n");
    auto n = d.channels()[0];
    QCOMPARE(n->option("SEVRCOMMAND"), canonical + "  echo 'Case Sensitive'");
    QCOMPARE(writeConfig(parseConfig(writeConfig(d))), writeConfig(d));
    Engine e(d);
    e.event(n, {0, initial, initial, 1, "0"});
    QStringList commands;
    e.command = [&](const QString& command) { commands << command; };
    e.event(n, {0, finalSeverity, finalSeverity, 1, "1"});
    QCOMPARE(commands, QStringList({"echo 'Case Sensitive'"}));
  }
  void localLatch() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    FakePv pv;
    Engine e(d, {}, &pv);
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    e.event(n, {3, 2, 2, 1, "99"});
    QCOMPARE(e.state(n).unack, 2);
    QCOMPARE(e.state(d.root.get()).severity, 2);
    e.event(n, {0, 0, 2, 1, "0"});
    QCOMPARE(e.state(n).unack, 2);
    e.acknowledge(n);
    QCOMPARE(e.state(n).unack, 0);
    QVERIFY(pv.writes.isEmpty());
  }
  void transientMask() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv ---T-");
    Engine e(d);
    auto n = d.channels()[0];
    e.event(n, {3, 2, 2, 0, "99"});
    e.event(n, {0, 0, 0, 0, "0"});
    QCOMPARE(e.state(n).unack, 0);
  }
  void masks() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    FakePv pv;
    Engine e(d, {}, &pv);
    auto n = d.channels()[0];
    e.start();
    e.event(n, {3, 2, 2, 1, "99"});
    e.setMask(n, Mask::parse("-D---"));
    QCOMPARE(e.state(d.root.get()).severity, 0);
    e.event(n, {4, 1, 1, 1, "11"});
    QCOMPARE(e.state(n).severity, 1);
    QCOMPARE(e.state(d.root.get()).severity, 0);
    e.resetMask(n);
    QCOMPARE(e.state(d.root.get()).severity, 1);
    e.setMask(n, Mask::parse("--A--"));
    QCOMPARE(e.state(d.root.get()).unack, 0);
    e.resetMask(n);
    QCOMPARE(e.state(d.root.get()).unack, 1);
    e.setMask(n, Mask::parse("C----"));
    QCOMPARE(e.state(n).severity, 0);
    QVERIFY(pv.cancelled.contains("pv"));
  }
  void logMask() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    Engine e(d);
    auto n = d.channels()[0];
    int logs = 0;
    e.alarmLog = [&](Node*, const State&, qint64) { ++logs; };
    e.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(logs, 0);
    e.event(n, {3, 2, 2, 1, "99"});
    QCOMPARE(logs, 1);
    e.setMask(n, Mask::parse("----L"));
    e.event(n, {0, 0, 0, 1, "0"});
    QCOMPARE(logs, 1);
  }
  void globalAcknowledgement() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ACKPV ack 7");
    FakePv pv;
    Engine e(d, {true, false, false, false}, &pv);
    auto n = d.channels()[0];
    e.event(n, {3, 2, 2, 1, "99"});
    e.acknowledge(n);
    QCOMPARE(pv.writes.size(), 2);
    QCOMPARE(pv.writes[0].kind, WriteKind::Acknowledge);
    QCOMPARE(pv.writes[0].value, 2.0);
    QCOMPARE(pv.writes[1].name, QString("ack"));
    QCOMPARE(e.state(n).unack, 2);
    e.event(n, {3, 2, 0, 1, "99"});
    QCOMPARE(e.state(n).unack, 0);
  }
  void failedAckTWrite_data() {
    QTest::addColumn<bool>("noAckT");
    QTest::newRow("enable-transient-ack") << true;
    QTest::newRow("disable-transient-ack") << false;
  }
  void failedAckTWrite() {
    QFETCH(bool, noAckT);
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    FakePv pv;
    Engine e(d, {true, false}, &pv);
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, !noAckT, "0"});
    auto requested = e.state(n).mask;
    requested[AckT] = !noAckT;
    requested[Log] = true;
    pv.writable = false;
    e.setMask(n, requested);
    QCOMPARE(e.state(n).mask[AckT], noAckT);
    QVERIFY(e.state(n).mask[Log]); // Other mask bits still apply.
    QCOMPARE(e.state(d.root.get()).maskCounts[AckT], int(noAckT));
    QCOMPARE(pv.writes.size(), 1);
    pv.writable = true;
    e.setMask(n, requested);
    QCOMPARE(pv.writes.size(), 2); // Reapplying must retry the failed setting.
    QCOMPARE(pv.writes.last().kind, WriteKind::AckTransient);
    QCOMPARE(pv.writes.last().value, double(noAckT));
    QCOMPARE(e.state(n).mask[AckT], !noAckT);
    QCOMPARE(e.state(d.root.get()).maskCounts[AckT], int(!noAckT));
  }
  void groupAckTPartialFailure() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root readonly\nCHANNEL root writable");
    FakePv pv;
    pv.unwritable.insert("readonly");
    Engine e(d, {true, false}, &pv);
    for (auto n : d.channels())
      e.event(n, {0, 0, 0, 1, "0"});
    e.setMask(d.root.get(), Mask::parse("---T-"));
    QCOMPARE(pv.writes.size(), 2);
    QVERIFY(!e.state(d.channels()[0]).mask[AckT]);
    QVERIFY(e.state(d.channels()[1]).mask[AckT]);
    QCOMPARE(e.state(d.root.get()).maskCounts[AckT], 1);
  }
  void ackTWithCancellation() {
    struct SubscribedPv : FakePv {
      bool put(const QString& name, double v, WriteKind k) override {
        return FakePv::put(name, v, k) && alarms.contains(name);
      }
    } pv;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv");
    Engine e(d, {true, false}, &pv);
    e.start();
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    e.setMask(n, Mask::parse("C--T-"));
    QVERIFY(e.state(n).mask[AckT]);
    QVERIFY(!pv.alarms.contains(n->name));
    e.setMask(n, Mask{});
    QVERIFY(!e.state(n).mask[AckT]);
    QVERIFY(pv.alarms.contains(n->name));
    QCOMPARE(pv.writes.size(), 2);
  }
  void passive() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ACKPV ack 7\n$SEVRPV sev");
    FakePv pv;
    Engine e(d, {true, true, true, false}, &pv);
    auto n = d.channels()[0];
    e.event(n, {3, 2, 2, 1, "99"});
    e.acknowledge(n);
    e.setMask(n, Mask::parse("CDATL"));
    QVERIFY(pv.writes.isEmpty());
    QCOMPARE(e.state(n).mask.text(), QString("CDA-L"));
  }
  void timeouts() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER -1 2");
    Engine e(d);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    e.event(n, {3, 2, 2, 1, "99"});
    QCOMPARE(e.state(n).severity, 0);
    time += 1999;
    e.tick();
    QCOMPARE(e.state(n).severity, 0);
    ++time;
    e.tick();
    QCOMPARE(e.state(n).severity, 2);
    e.noAck(n, true);
    QVERIFY(e.state(n).mask[Ack]);
    time += 3600001;
    e.tick();
    QVERIFY(!e.state(n).mask[Ack]);
  }
  void deadlineRescheduling() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root slow\n$ALARMCOUNTFILTER -1 5\n"
                         "CHANNEL root fast\n$ALARMCOUNTFILTER -1 2\n");
    Engine e(d);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto slow = d.channels()[0], fast = d.channels()[1];
    for (auto n : d.channels()) e.event(n, {0, 0, 0, 1, "0"});
    e.event(slow, {3, 2, 2, 1, "10"});
    time = 2000;
    e.event(fast, {3, 2, 2, 1, "20"});
    time = 4000;
    e.tick();
    QCOMPARE(e.state(fast).severity, 2);
    QCOMPARE(e.state(slow).severity, 0);
    // Cancellation must not discard another channel's later deadline.
    e.setMask(fast, Mask::parse("C----"));
    time = 6000;
    e.tick();
    QCOMPARE(e.state(slow).severity, 2);
    // A nested timer survives expiration of its parent's earlier timer.
    e.noAck(d.root.get(), true);
    time += 1000;
    e.noAck(slow, true);
    time = 3606000;
    e.tick();
    QVERIFY(e.state(slow).mask[Ack]);
    QVERIFY(!e.state(fast).mask[Ack]);
    time += 1000;
    e.tick();
    QVERIFY(!e.state(slow).mask[Ack]);
    // A retained mutable state reference remains supported across idle ticks.
    auto& state = e.mutableState(slow);
    e.tick();
    e.setMask(slow, Mask::parse("--A--"));
    state.noAckUntil = time + 100;
    time += 100;
    e.tick();
    QCOMPARE(state.noAckUntil, qint64(0));
    QVERIFY(!state.mask[Ack]);
  }
  void historyTimestampBoundaries() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    Engine e(d);
    qint64 time = 0;
    e.now = e.monotonicNow = [&] { return time; };
    int severity = 0;
    for (qint64 timestamp : {1000LL, 1999LL, 2000LL, 1500LL, -1LL, -1000LL, -1001LL}) {
      time = timestamp;
      severity = severity ? 0 : 2;
      e.event(d.channels()[0], {severity ? 3 : 0, severity, severity, 1, "value"});
      QVERIFY(e.history.first().startsWith(
          QDateTime::fromMSecsSinceEpoch(time).toString("dd-MMM-yyyy HH:mm:ss") + " "));
    }
    QCOMPARE(e.history.size(), 7);
  }
  void forceRecoveryAfterDisconnect() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV force -D--- 1 NE");
    Engine e(d);
    auto n = d.channels()[0];
    e.forceValue(n, 1);
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, std::numeric_limits<double>::quiet_NaN());
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, 0);
    QVERIFY(!e.state(n).mask[Disable]);
    e.forceValue(n, 1);
    e.forceValue(n, std::numeric_limits<double>::quiet_NaN());
    e.forceValue(n, 1);
    QVERIFY(e.state(n).mask[Disable]);
    e.forceValue(n, 2);
    QVERIFY(!e.state(n).mask[Disable]);
  }
  void cancelPendingFilter() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER 1 2\n"
                         "$SEVRCOMMAND UP_MAJOR command\n$SEVRPV severity");
    FakePv pv;
    Engine e(d, {true, false, false, false}, &pv);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto n = d.channels()[0];
    int logs = 0, commands = 0;
    e.alarmLog = [&](Node*, const State&, qint64) { ++logs; };
    e.command = [&](QString) { ++commands; };
    e.event(n, {0, 0, 0, 1, "0"});
    pv.writes.clear();
    e.event(n, {3, 2, 2, 1, "99"});
    e.setMask(n, Mask::parse("C----"));
    time += 3000;
    e.tick();
    QCOMPARE(e.state(n).severity, 0);
    QCOMPARE(logs, 0);
    QCOMPARE(commands, 0);
    QVERIFY(pv.writes.isEmpty());
    e.resetMask(n);
    e.event(n, {3, 2, 2, 1, "99"});
    QCOMPARE(e.state(n).severity, 0);
    time += 2000;
    e.tick();
    QCOMPARE(e.state(n).severity, 2);
    QCOMPARE(commands, 1);
    QCOMPARE(logs, 1);
  }
  void heartbeatPlacement() {
    for (const auto& text : {"$HEARTBEATPV beat 1 7\nGROUP NULL root\nCHANNEL root pv\n",
                             "GROUP NULL root\nCHANNEL root pv\n$HEARTBEATPV beat 1 7\n",
                             "GROUP NULL root\nGROUP root child\n$HEARTBEATPV beat 1 7\n"}) {
      auto d = parseConfig(text);
      QCOMPARE(d.root->option("HEARTBEATPV"), QString("beat 1 7"));
      QCOMPARE(parseConfig(writeConfig(d)).root->option("HEARTBEATPV"), QString("beat 1 7"));
    }
    QTemporaryDir dir;
    QFile child(dir.filePath("child"));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.write("GROUP NULL child\n$HEARTBEATPV included 2 3\nCHANNEL child pv\n");
    child.close();
    auto d = parseConfig("GROUP NULL root\nINCLUDE root child\n", dir.path());
    QCOMPARE(d.root->option("HEARTBEATPV"), QString("included 2 3"));
    QVERIFY(d.root->children.front()->option("HEARTBEATPV").isEmpty());
    d = parseConfig("GROUP NULL root\n$HEARTBEATPV first 1 7\nINCLUDE root child\n", dir.path());
    QCOMPARE(d.root->option("HEARTBEATPV"), QString("first 1 7"));
    FakePv pv;
    Engine e(d, {true, false, false, false}, &pv);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    e.start();
    time += 1000;
    e.tick();
    QCOMPARE(pv.writes.last().name, QString("first"));
    QCOMPARE(pv.writes.last().value, 7.0);
  }
  void heartbeatUnavailableAndRecovery() {
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV beat 0.001 7\n");
    FakePv pv; pv.writable = false;
    Engine e(d, {true}, &pv);
    qint64 time = 1000; e.monotonicNow = [&] { return time; };
    QStringList errors; e.error = [&](const QString& text) { errors << text; };
    e.start(); QCOMPARE(e.heartbeatDelay(), -1);
    for (int i = 0; i < 1000; ++i) { ++time; e.tickHeartbeat(); }
    QVERIFY(pv.writes.isEmpty()); QCOMPARE(errors.size(), 1);
    QVERIFY(errors[0].contains("beat"));
    pv.writable = true; QCOMPARE(e.heartbeatDelay(), 1);
    time += 50; e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 1); QCOMPARE(pv.writes[0].kind, WriteKind::Heartbeat);
    QCOMPARE(pv.writes[0].value, 7.0); QCOMPARE(e.heartbeatDelay(), 1);
    pv.writable = false;
    for (int i = 0; i < 1000; ++i) { ++time; e.tickHeartbeat(); }
    QCOMPARE(pv.writes.size(), 1); QCOMPARE(errors.size(), 2);
    e.stop(); pv.writable = true; ++time; e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 1); QCOMPARE(e.heartbeatDelay(), -1);
  }
  void heartbeatCadence() {
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV beat 0.1 7\n");
    FakePv pv;
    Engine e(d, {true}, &pv);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    e.start();
    QCOMPARE(e.heartbeatDelay(), 100);
    time = 1099;
    e.tickHeartbeat();
    QVERIFY(pv.writes.isEmpty());
    time = 1125;
    e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(e.heartbeatDelay(), 75);
    time = 1200;
    e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 2);
    time = 1650; // Skip missed beats without a burst or shifting the phase.
    e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 3);
    QCOMPARE(e.heartbeatDelay(), 50);
    e.stop();
    QCOMPARE(e.heartbeatDelay(), -1);
    time = 2000;
    e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 3);
    e.start();
    QCOMPARE(e.heartbeatDelay(), 100);
    e.options.passive = true;
    QCOMPARE(e.heartbeatDelay(), -1);
    time = 2100;
    e.tickHeartbeat();
    QCOMPARE(pv.writes.size(), 3);
  }
  void automaticAckTBeforeFirstMonitor_data() {
    QTest::addColumn<bool>("configuredT");
    QTest::addColumn<bool>("startupError");
    QTest::newRow("ack-transients") << false << false;
    QTest::newRow("no-ack-transients") << true << false;
    QTest::newRow("ack-transients-after-error") << false << true;
    QTest::newRow("no-ack-transients-after-error") << true << true;
  }
  void automaticAckTBeforeFirstMonitor() {
    QFETCH(bool, configuredT);
    QFETCH(bool, startupError);
    QString mask = configuredT ? "---T-" : "-----";
    auto d = parseConfig("GROUP NULL root\n$FORCEPV CALC " + mask +
                         " 1 NE\n$FORCEPV_CALC 1\nCHANNEL root pv " + mask + "\n");
    FakePv pv;
    pv.writable = false;
    Engine e(d, {true}, &pv);
    auto n = d.channels()[0];
    if (startupError)
      e.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    e.start();
    QCOMPARE(pv.writes.size(), 1);
    QCOMPARE(e.state(n).mask[AckT], configuredT);
    e.tick();
    QCOMPARE(pv.writes.size(), 1);
    // The first real IOC value contradicts the configured/forced mask.
    e.event(n, {0, 0, 0, int(configuredT), "0"});
    QCOMPARE(e.state(n).mask[AckT], !configuredT);
    pv.writable = true;
    e.tick();
    QCOMPARE(pv.writes.size(), 2);
    QCOMPARE(pv.writes.last().kind, WriteKind::AckTransient);
    QCOMPARE(pv.writes.last().value, double(!configuredT));
    QCOMPARE(e.state(n).mask[AckT], configuredT);
    e.tick();
    QCOMPARE(pv.writes.size(), 2);
  }
  void automaticAckTRecovery() {
    auto d = parseConfig("GROUP NULL root\n$FORCEPV CALC ----- 1 NE\n$FORCEPV_CALC 1\n"
                         "CHANNEL root pv ---T-\nCHANNEL root other ---T-\n");
    FakePv pv;
    pv.unwritable.insert("pv");
    Engine e(d, {true}, &pv);
    e.start();
    auto n = d.channels()[0];
    QVERIFY(e.state(n).mask[AckT]);
    QVERIFY(!e.state(d.channels()[1]).mask[AckT]);
    auto attempts = pv.writes.size();
    e.tick();
    QCOMPARE(pv.writes.size(), attempts); // Do not flood errors while unavailable.
    pv.unwritable.clear();
    e.event(n, {0, 0, 0, 0, "0"});
    e.tick();
    QVERIFY(!e.state(n).mask[AckT]);
    QCOMPARE(pv.writes.size(), attempts + 1);
    QCOMPARE(pv.writes.last().kind, WriteKind::AckTransient);
    QCOMPARE(pv.writes.last().value, 1.0);
    // Successful settings are not replayed over subsequent IOC/operator changes.
    e.event(n, {0, 0, 0, 0, "0"});
    e.tick();
    QCOMPARE(pv.writes.size(), attempts + 1);
    QVERIFY(e.state(n).mask[AckT]);
  }
  void automaticAckTSuperseded_data() {
    QTest::addColumn<QString>("action");
    for (auto action : {"reset", "operator", "disable", "reconfigure", "stop", "passive"})
      QTest::newRow(action) << QString(action);
  }
  void automaticAckTSuperseded() {
    QFETCH(QString, action);
    auto d = parseConfig("GROUP NULL root\n$FORCEPV gate ---T- 1 NE\nCHANNEL root pv\n");
    FakePv pv;
    pv.writable = false;
    Engine e(d, {true}, &pv);
    e.start();
    auto root = d.root.get(), n = d.channels()[0];
    e.forceValue(root, 1);
    if (action == "reset")
      e.forceValue(root, 0);
    else if (action == "operator")
      e.setMask(n, Mask::parse("-D---"));
    else if (action == "disable")
      e.setForceDisabled(root, true);
    else if (action == "reconfigure")
      e.configureForce(root, {}, false);
    else if (action == "stop")
      e.stop();
    else if (action == "passive")
      e.options.passive = true;
    auto attempts = pv.writes.size();
    pv.writable = true;
    e.tick();
    QCOMPARE(pv.writes.size(), attempts + (action == "reset" ? 1 : 0));
    if (action == "reset")
      QCOMPARE(pv.writes.last().value, 1.0);
    QVERIFY(!e.state(n).mask[AckT]);
  }
  void forceAndHeartbeat() {
    auto d = parseConfig(
        "GROUP NULL root\n$HEARTBEATPV beat 0.1 7\nCHANNEL root pv\n$FORCEPV force -D--- 1 NE");
    FakePv pv;
    Engine e(d, {true, false, false, false}, &pv);
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    e.start();
    auto n = d.channels()[0];
    pv.numbers["force"](1);
    QVERIFY(e.state(n).mask[Disable]);
    pv.numbers["force"](0);
    QVERIFY(!e.state(n).mask[Disable]);
    time += 101;
    e.tick();
    QVERIFY(!pv.writes.isEmpty());
    QCOMPARE(pv.writes.last().name, QString("beat"));
    QCOMPARE(pv.writes.last().value, 7.0);
  }
  void beepHierarchyAndSave() {
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch pv");
    Engine e(d);
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    e.event(n, {3, 2, 2, 1, "99"});
    QVERIFY(e.audible());
    e.setBeep(n->parent, 3);
    QVERIFY(!e.audible());
    auto saved = parseConfig(writeConfig(d));
    QCOMPARE(saved.channels()[0]->parent->option("BEEPSEVR"), QString("INVALID"));
    e.setBeep(n->parent, 1);
    QVERIFY(e.audible());
    e.silenceCurrent = true;
    QVERIFY(!e.audible());
  }
  void constantCalculationAndReconfigure() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$FORCEPV CALC -D--- 1 "
                         "NE\n$FORCEPV_CALC A+B\n$FORCEPV_CALC_A 0.5\n$FORCEPV_CALC_B 0.5");
    FakePv pv;
    Engine e(d, {}, &pv);
    auto n = d.channels()[0];
    e.start();
    QVERIFY(e.state(n).mask[Disable]);
    e.event(n, {3, 2, 2, 1, "99"});
    e.configureForce(n, {{"FORCEPV", "other -D--- 1 NE"}}, false);
    QCOMPARE(e.state(n).severity, 2);
    QVERIFY(pv.numbers.contains("other"));
    pv.numbers["other"](0);
    pv.numbers["other"](1);
    QVERIFY(e.state(n).mask[Disable]);
    pv.numbers["other"](0);
    QVERIFY(!e.state(n).mask[Disable]);
    QCOMPARE(e.state(n).severity, 2);
  }
  void commands() {
    auto d = parseConfig(
        "GROUP NULL root\n$SEVRCOMMAND UP_ALARM root-alarm\nCHANNEL root pv\n$SEVRCOMMAND UP_MAJOR "
        "high\n$SEVRCOMMAND DOWN_ANY clear\n$STATCOMMAND HIHI status");
    Engine e(d);
    QStringList commands;
    e.command = [&](QString s) { commands << s; };
    auto n = d.channels()[0];
    e.event(n, {0, 0, 0, 1, "0"});
    commands.clear();
    e.event(n, {3, 2, 2, 1, "99"});
    QVERIFY(commands.contains("high"));
    QVERIFY(commands.contains("status"));
    QVERIFY(commands.contains("root-alarm"));
    e.event(n, {0, 0, 2, 1, "0"});
    QVERIFY(commands.contains("clear"));
  }
  void styleOptions() {
    QVERIFY(parseOptions({"qtalh"}).style.isEmpty());
    QCOMPARE(parseOptions({"qtalh", "-style", "motif"}).style, QString("motif"));
    QCOMPARE(parseOptions({"qtalh", "-STYLE", "FuSiOn"}).style, QString("FuSiOn"));
    QCOMPARE(parseOptions({"qtalh", "-style=Fusion"}).style, QString("Fusion"));
    QCOMPARE(parseOptions({"qtalh", "-style", "Windows", "-style=fusion"}).style, QString("fusion"));
    auto terminated = parseOptions({"qtalh", "--", "-style=fusion"});
    QVERIFY(terminated.style.isEmpty());
    QVERIFY(terminated.config.endsWith("-style=fusion"));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-style"}));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-style="}));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-style", ""}));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-style", "-c"}));
    // Availability belongs to GUI startup, not headless option parsing.
    QVERIFY(parseOptions({"qtalh", "--help", "-style=unknown"}).help);
    QVERIFY(parseOptions({"qtalh", "--version", "-style=unknown"}).version);
    QVERIFY(parseOptions({"qtalh", "--validate", "-style=unknown"}).validate);
  }
  void options() {
    auto o = parseOptions({"qtalh", "-D", "-S", "-global", "-c", "-filter", "unack", "-m", "0"});
    QVERIFY(o.noLog);
    QVERIFY(o.engine.passive);
    QVERIFY(o.engine.global);
    QVERIFY(o.editor);
    QCOMPARE(o.filter, 2);
    QCOMPARE(o.maxRecords, 0);
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-m", "bad"}));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-xrm", "foo"}));
  }
  void queueCodec() {
    QByteArray record = "1 2 10-Sep-2026 12:00:00 service error";
    auto bytes = encodeQueue(record);
    QCOMPARE(decodeQueue(bytes, record.size()), record);
    long type = 0;
    memcpy(&type, record.constData(), sizeof(long));
    QVERIFY(type > 0);
#ifdef Q_OS_WIN
    QString error;
    QVERIFY(!sendQueue(123, record, &error));
    QVERIFY(error.contains("unavailable on Windows"));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-P", "123"}));
    QVERIFY_THROWS_EXCEPTION(ParseError, parseOptions({"qtalh", "-O", "123"}));
#else
    int id = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);
    QVERIFY(id >= 0);
    QCOMPARE(msgsnd(id, bytes.constData(), record.size(), 0), 0);
    QCOMPARE(receiveQueue(id), record);
    QCOMPARE(msgctl(id, IPC_RMID, nullptr), 0);
#endif
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, encodeQueue(QByteArray(260, 'a')));
  }
  void printerCodec() {
    auto r = QByteArray("1 2 10-Sep-2026 12:00:00 service error");
    QCOMPARE(printerRecord(r, "bw"), QByteArray("10-Sep-2026 12:00:00 service error"));
    QCOMPARE(printerRecord(r, "bw_bold"),
             QByteArray("\033[1m10-Sep-2026 12:00:00 service error\033[0m"));
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, printerRecord("1 1 short", "bw"));
  }
  void tenThousandChannels() {
    QString text = "GROUP NULL large\n";
    for (int i = 0; i < 10000; ++i)
      text += QString("CHANNEL large pv%1\n").arg(i);
    auto d = parseConfig(text);
    Engine e(d);
    QElapsedTimer timer;
    timer.start();
    for (auto n : d.channels())
      e.event(n, {3, 2, 2, 1, "99"});
    QCOMPARE(e.state(d.root.get()).counts[2], 10000);
    QCOMPARE(e.state(d.root.get()).unackCounts[2], 10000);
    e.acknowledge(d.root.get());
    QCOMPARE(e.state(d.root.get()).unack, 0);
    qInfo() << "10000 channel burst and acknowledgement (ms):" << timer.elapsed();
    QVERIFY(timer.elapsed() < 10000);
  }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"
