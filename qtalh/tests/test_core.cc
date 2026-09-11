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
class CoreTests : public QObject {
  Q_OBJECT
private slots:
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
    QTest::newRow("two roots") << "GROUP NULL a\nGROUP NULL b";
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
    e.now = [&] { return time; };
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
    QCOMPARE(commands, QStringList({"notify"}));
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
    QCOMPARE(pv.writes.size(), 2);
    QCOMPARE(pv.writes[0].value, -1.0);
    QCOMPARE(pv.writes[1].value, 0.0);
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
    e.now = [&] { return time; };
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
    e.now = [&] { return time; };
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
    e.now = [&] { return time; };
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
    e.now = [&] { return time; };
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
    e.now = [&] { return time; };
    e.start();
    time += 1000;
    e.tick();
    QCOMPARE(pv.writes.last().name, QString("first"));
    QCOMPARE(pv.writes.last().value, 7.0);
  }
  void heartbeatCadence() {
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV beat 0.1 7\n");
    FakePv pv;
    Engine e(d, {true}, &pv);
    qint64 time = 1000;
    e.now = [&] { return time; };
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
    e.now = [&] { return time; };
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
