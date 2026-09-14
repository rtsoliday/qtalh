#include "test_compat.h"
#include "ui/window.h"
#include "ui/dialogs.h"
#include "ui/alarm_view.h"
#include "services/log_browser.h"
#include "services/log_identity.h"
#include "services/ipc.h"
#ifndef Q_OS_WIN
#include <sys/msg.h>
#include <sys/resource.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#endif
#include <QtWidgets>
#include <alarm.h>
#include <QThread>
#include <QSemaphore>
#include <QAbstractItemModelTester>
#include <QCryptographicHash>
#include <QtEndian>
#include <atomic>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
using namespace alh;
static QString checkpointFor(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return logIdentity::checkpointPath(file);
}
#ifndef Q_OS_WIN
struct UiQueue {
  int id = -1, key = 0x54000000 | (QCoreApplication::applicationPid() & 0xFFFFFF);
  UiQueue() {
    while ((id = msgget(key, IPC_CREAT | IPC_EXCL | 0600)) < 0 && errno == EEXIST) ++key;
  }
  ~UiQueue() { if (id >= 0) msgctl(id, IPC_RMID, nullptr); }
};
#endif
class BellTestWindow : public Window {
public:
  using Window::Window;
  int bells = 0;
protected:
  void systemBeep() override { ++bells; }
};
class UiTests : public QObject {
  Q_OBJECT
  QTemporaryDir notificationConfig;
  Options options(bool editor) {
    Options o;
    o.editor = editor;
    o.silent = true;
    o.noLog = true;
    o.noErrorPopup = true;
    o.mainWindow = true;
    return o;
  }
  Document sample() {
    return parseConfig(
        "GROUP NULL BOOSTER\n$GUIDANCE\nBooster guidance\n$END\nGROUP BOOSTER "
        "Power_Supplies\nCHANNEL Power_Supplies test:power\nGROUP BOOSTER Timing\nCHANNEL Timing "
        "test:timing\nGROUP BOOSTER Vacuum\nCHANNEL Vacuum test:vacuum\n");
  }
private slots:
  void runtimeErrorsAreAudited_data() {
    QTest::addColumn<bool>("noLog");
    QTest::newRow("operation-log") << false;
    QTest::newRow("logging-disabled") << true;
  }
  void runtimeErrorsAreAudited() {
    QFETCH(bool, noLog);
    QTemporaryDir dir;
    auto o = options(false); o.noLog = noLog;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    const QString message = "Runtime audit regression";
    QTest::ignoreMessage(QtWarningMsg, "QtALH: Runtime audit regression");
    w->alarmEngine().error(message);
    QFile file(o.opmodFile);
    if (noLog) QVERIFY(!file.exists());
    else {
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll().count(message.toUtf8()), 1);
    }
    bool visibleMessage = false;
    for (auto label : w->findChildren<QLabel*>()) visibleMessage |= label->text() == message;
    QVERIFY(visibleMessage);
    QVERIFY(w->findChildren<QMessageBox*>().isEmpty()); // -noerrorpopup still audits.
  }
  void runtimeErrorAuditFailureDoesNotRecurse() {
#ifdef Q_OS_LINUX
    QTemporaryDir dir;
    auto o = options(false); o.noLog = false;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = "/dev/full";
    QTest::ignoreMessage(QtWarningMsg, "QIODevice::seek (QFile, \"/dev/full\"): Cannot call seek on a sequential device");
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("QtALH: Log write failed:.*"));
    QTest::ignoreMessage(QtWarningMsg, "QtALH: Original runtime failure");
    w->alarmEngine().error("Original runtime failure");
    bool visibleMessage = false;
    for (auto label : w->findChildren<QLabel*>()) visibleMessage |= label->text() == "Original runtime failure";
    QVERIFY(visibleMessage);
#else
    QSKIP("Uses Linux /dev/full to fail operation-log writes");
#endif
  }
  void globalCommunicationErrorRemainsVisible() {
    auto o = options(false); o.engine.global = true; o.filter = 2;
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root missing\n"), o, false);
    auto n = w->document().channels()[0]; auto& engine = w->alarmEngine();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    engine.event(n, {ALARM_NSTATUS, 4, 0, -1, "0"});
    QTRY_COMPARE(tree->model()->rowCount(), 1);
    QTRY_COMPARE(group->model()->rowCount(), 1);
    QCOMPARE(group->model()->index(0, 2).data().toString(), QString("missing"));
    engine.acknowledge(n);
    QTRY_COMPARE(tree->model()->rowCount(), 0);
    QTRY_COMPARE(group->model()->rowCount(), 0);
  }
  void editorIgnoresRuntimeFilters() {
    for (int filter : {1, 2}) {
      auto o = options(true); o.filter = filter;
      auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
      auto tree = w->findChild<QTreeView*>("alarmTree");
      auto group = w->findChild<QTreeView*>("groupContents");
      QCOMPARE(tree->model()->rowCount(), 1);
      QVERIFY(tree->currentIndex().isValid());
      QCOMPARE(group->model()->rowCount(), 1);
      auto mime = new QMimeData;
      mime->setData("application/x-qtalh-config", "GROUP NULL clipboard\nCHANNEL clipboard copied\n");
      QApplication::clipboard()->setMimeData(mime);
      auto paste = w->findChild<QAction*>("Paste"); QVERIFY(paste);
      paste->trigger();
      QCOMPARE(w->document().channels().size(), 2);
      w->undoEdit();
      QCOMPARE(w->document().channels().size(), 1);
      QVERIFY(tree->currentIndex().isValid());
    }
  }

  void filterChangesKeepActionTargetVisible() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), options(false), false);
    auto group = w->findChild<QTreeView*>("groupContents");
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto n = w->document().channels()[0];
    auto& engine = w->alarmEngine();
    engine.event(n, {3, 2, 2, 1, "alarm"});
    group->setCurrentIndex(group->model()->index(0, 2));
    auto action = [&](const QString& name) -> QAction* {
      for (auto a : w->findChildren<QAction*>()) if (a->text() == name) return a;
      return nullptr;
    };
    action("Active Alarms Only")->trigger();
    QVERIFY(group->currentIndex().isValid());
    QCOMPARE(group->currentIndex().data().toString(), QString("pv"));
    QVERIFY(action("NoAck for One Hour")->isEnabled());
    action("Properties Window")->trigger();
    QPointer<QDialog> properties = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(properties);
    // An alarm which clears and is acknowledged disappears on the next refresh.
    engine.event(n, {}); engine.acknowledge(n);
    QTRY_VERIFY(!group->currentIndex().isValid());
    QVERIFY(!tree->currentIndex().isValid());
    QVERIFY(!action("NoAck for One Hour")->isEnabled());
    QVERIFY(!action("Force Mask...")->isEnabled());
    QTRY_VERIFY(!properties);
    action("NoAck for One Hour")->trigger(); QVERIFY(!engine.state(n).mask[Ack]);
    // A new alarm must not silently reselect the old target.
    engine.event(n, {3, 2, 2, 1, "again"});
    QTRY_COMPARE(group->model()->rowCount(), 1);
    QVERIFY(!group->currentIndex().isValid());
    group->setCurrentIndex(group->model()->index(0, 2));
    QVERIFY(action("NoAck for One Hour")->isEnabled());
    action("No filter")->trigger();
    QCOMPARE(group->currentIndex().data().toString(), QString("pv"));
    // Explicitly selecting a filter which hides the target also clears actions.
    engine.acknowledge(n);
    action("Unacknowledged Alarms Only")->trigger();
    QVERIFY(!group->currentIndex().isValid());
    QVERIFY(!action("Acknowledge Alarm")->isEnabled());
  }
  void silenceControlsAndAudit() {
    QTemporaryDir dir; auto o = options(false); o.noLog = false; o.silent = false;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    auto& engine = w->alarmEngine(); qint64 elapsed = 1000;
    engine.monotonicNow = [&] { return elapsed; };
    auto interval = w->findChild<QCheckBox*>("silenceInterval");
    auto current = w->findChild<QCheckBox*>("silenceCurrent");
    auto trigger = [&](const QString& name) {
      for (auto a : w->findChildren<QAction*>()) if (a->text() == name) a->trigger();
    };
    interval->setChecked(true); const auto deadline = engine.silenceUntil;
    elapsed += 100; trigger("30 minutes"); QCOMPARE(engine.silenceUntil, deadline);
    trigger("5 minutes"); QCOMPARE(engine.silenceUntil, qint64(0)); QVERIFY(!interval->isChecked());
    current->setChecked(true); QVERIFY(engine.silenceCurrent);
    current->setChecked(false); QVERIFY(!engine.silenceCurrent);
    trigger("Silence Forever"); QVERIFY(engine.silenceForever);
    trigger("Silence Forever"); QVERIFY(!engine.silenceForever);
    interval->setChecked(true); QCOMPARE(engine.silenceUntil, elapsed + 5 * 60000);
    elapsed = engine.silenceUntil; engine.tick(); engine.tick();
    QCOMPARE(engine.silenceUntil, qint64(0));
    QFile file(o.opmodFile); QVERIFY(file.open(QIODevice::ReadOnly)); const auto log = file.readAll();
    QCOMPARE(log.count("Silence Selected Minutes set to TRUE"), 2);
    QCOMPARE(log.count("Silence Selected Minutes set to FALSE"), 2);
    QCOMPARE(log.count("Silence Current set to TRUE"), 1);
    QCOMPARE(log.count("Silence Current set to FALSE"), 1);
    QCOMPARE(log.count("Silence Forever set to TRUE"), 1);
    QCOMPARE(log.count("Silence Forever set to FALSE"), 1);
    QVERIFY(log.contains("Silence interval set to 5 minutes"));
    // A new audible alarm also records the automatic end of current silence.
    engine.event(w->document().channels()[0], {});
    current->setChecked(true);
    engine.event(w->document().channels()[0], {3, 2, 2, 1, "alarm"});
    QVERIFY(!engine.silenceCurrent);
    file.seek(0); const auto after = file.readAll();
    QCOMPARE(after.count("Silence Current set to FALSE"), 2);
  }
  void historicalRingOrder_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("text") << false; QTest::newRow("xml") << true;
  }
  void historicalRingOrder() {
    QFETCH(bool, xml);
    QTemporaryDir dir; Options o; o.maxRecords = 3; o.xml = xml;
    o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    Logging log(o, "root"); auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    State state; state.severity = 2;
    for (auto value : {"event-A", "event-B", "event-C", "event-D"}) {
      state.value = value; log.alarm(d.channels()[0], state, time.toMSecsSinceEpoch());
    }
    LogSearch request; request.path = o.alarmFile; request.from = time; request.to = time;
    auto check = [&](const QStringList& expected) {
      const auto result = searchLogs(request); QVERIFY(result.errors.isEmpty());
      QCOMPARE(result.records, expected.size());
      auto lines = result.text.split('\n', Qt::SkipEmptyParts);
      for (int i = 0; i < expected.size(); ++i) QVERIFY(lines[i].contains(expected[i]));
    };
    check({"event-B", "event-C", "event-D"});
    request.maximumRecords = 2; check({"event-B", "event-C"}); QVERIFY(searchLogs(request).truncated);
    request.maximumRecords = 100; request.contains = "event-C"; check({"event-C"}); request.contains.clear();
    // A newer intact but stale slot must not outrank the slot matching the log.
    QFile checkpoint(checkpointFor(o.alarmFile)); QVERIFY(checkpoint.open(QIODevice::ReadWrite));
    auto data = checkpoint.readAll(); auto stale = data.mid(96, 96);
    qToBigEndian<quint64>(100, stale.data() + 8);
    stale.replace(64, 32, QCryptographicHash::hash(stale.left(64), QCryptographicHash::Sha256));
    checkpoint.seek(96); QCOMPARE(checkpoint.write(stale), qint64(96)); QVERIFY(checkpoint.flush());
    check({"event-B", "event-C", "event-D"});
    // With no matching checkpoint, retain physical order for equal timestamps.
    checkpoint.seek(0); checkpoint.write("broken!"); QVERIFY(checkpoint.flush());
    check({"event-D", "event-B", "event-C"});
    checkpoint.close(); QVERIFY(QFile::remove(checkpoint.fileName()));
    check({"event-D", "event-B", "event-C"});
    auto cancelled = searchLogs(request, [] { return true; }); QVERIFY(cancelled.cancelled);
  }
  void logSearchOutlivesReceiver() {
    auto receiver = std::make_unique<QObject>();
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    bool delivered = false;
    QPointer<QThread> worker = startLogSearch(receiver.get(), {},
        [&](const LogSearchResult&) { delivered = true; },
        [entered, release, cancelled](const LogSearch&, const std::function<bool()>& stop) {
          entered->release();
          release->tryAcquire(1, 3000); // Simulate I/O which cannot observe cancellation yet.
          *cancelled = stop();
          return LogSearchResult();
        });
    QTRY_VERIFY(entered->available());
    QElapsedTimer duration; duration.start(); receiver.reset();
    QVERIFY2(duration.elapsed() < 500, "Closing a search must not wait for blocked I/O");
    QVERIFY(worker); QVERIFY(worker->isInterruptionRequested());
    bool guiTick = false; QTimer::singleShot(0, this, [&] { guiTick = true; });
    QTRY_VERIFY(guiTick); QVERIFY(!delivered);
    release->release(); QTRY_VERIFY(!worker);
    QVERIFY(cancelled->load()); QVERIFY(!delivered);
    // A surviving receiver gets its result, and also releases the worker.
    receiver = std::make_unique<QObject>();
    worker = startLogSearch(receiver.get(), {}, [&](const LogSearchResult& result) {
      delivered = result.records == 42;
    }, [](const LogSearch&, const std::function<bool()>&) {
      LogSearchResult result; result.records = 42; return result;
    });
    QTRY_VERIFY(delivered); QTRY_VERIFY(!worker);
  }
  void forceResetThroughEditorAndRuntime() {
    for (bool editor : {false, true}) {
      QTemporaryDir dir;
      auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), options(editor), false);
      auto view = w->findChild<QTreeView*>("groupContents"); view->setCurrentIndex(view->model()->index(0, 2));
      for (auto a : w->findChildren<QAction*>())
        if (a->text() == (editor ? "Properties Window" : "Force Process Variable...")) a->trigger();
      auto dialog = w->findChild<QDialog*>(editor ? "propertiesDialog" : "forcePvDialog"); QVERIFY(dialog);
      dialog->findChild<QLineEdit*>(editor ? "propertyFORCE_NAME" : "forcePvName")->setText("gate");
      dialog->findChild<QLineEdit*>(editor ? "propertyFORCE_2" : "forceValue")->setText("2");
      dialog->findChild<QLineEdit*>(editor ? "propertyFORCE_3" : "forceReset")->setText("0.000001");
      dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
      auto check = [&] { QCOMPARE(w->document().channels()[0]->option("FORCEPV").section(' ', 3, 3).toDouble(), 0.000001); };
      check();
      if (editor) { w->addNode(false, "another"); check(); w->undoEdit(); check(); w->redoEdit(); check(); }
      w->saveTo(dir.filePath("saved"));
      QCOMPARE(loadConfig(dir.filePath("saved")).channels()[0]->option("FORCEPV").section(' ', 3, 3).toDouble(), 0.000001);
    }
  }
  void forceDialogIndependentDefaults() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\n$FORCEPV gate D 1 0\nCHANNEL root pv\n"), options(false), false);
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Force Process Variable...") a->trigger();
    auto dialog = w->findChild<QDialog*>("forcePvDialog"); QVERIFY(dialog);
    auto force = dialog->findChild<QLineEdit*>("forceValue");
    auto reset = dialog->findChild<QLineEdit*>("forceReset");
    force->clear(); reset->setText("0");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(w->document().root->option("FORCEPV"), QString("gate -D--- 1 0"));
    auto& e = w->alarmEngine(); auto n = w->document().channels()[0];
    e.forceValue(w->document().root.get(), 0); QVERIFY(!e.state(n).mask[Disable]);
    e.forceValue(w->document().root.get(), 1); QVERIFY(e.state(n).mask[Disable]);
    force->setText("2.5"); reset->clear();
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(w->document().root->option("FORCEPV"), QString("gate -D--- 2.5 0"));
  }
  void propertiesIndependentDefaults() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), options(true), false);
    auto view = w->findChild<QTreeView*>("groupContents"); view->setCurrentIndex(view->model()->index(0, 2));
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Properties Window") a->trigger();
    auto dialog = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("propertyFORCE_NAME")->setText("gate");
    dialog->findChild<QLineEdit*>("propertyFORCE_2")->setText("5");
    dialog->findChild<QLineEdit*>("propertyFORCE_3")->setText("0");
    dialog->findChild<QLineEdit*>("propertySECONDS")->setText("7");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(w->document().channels()[0]->option("FORCEPV"), QString("gate ----- 5 0"));
    QCOMPARE(w->document().channels()[0]->option("ALARMCOUNTFILTER"), QString("1 7"));
    w->undoEdit(); QVERIFY(w->document().channels()[0]->option("FORCEPV").isEmpty());
  }
  void scalarForcePreservesCalc() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\n$FORCEPV gate D 1 0\n"
        "$FORCEPV_CALC A+B\n$FORCEPV_CALC_A input\n$FORCEPV_CALC_B 2\nCHANNEL root pv\n"), options(false), false);
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Force Process Variable...") a->trigger();
    auto dialog = w->findChild<QDialog*>("forcePvDialog"); QVERIFY(dialog);
    auto apply = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply);
    apply->click();
    QCOMPARE(w->document().root->option("FORCEPV_CALC"), QString("A+B"));
    QCOMPARE(w->document().root->option("FORCEPV_CALC_A"), QString("input"));
    QCOMPARE(w->document().root->option("FORCEPV_CALC_B"), QString("2"));
    dialog->findChild<QLineEdit*>("forcePvName")->setText("CALC"); apply->click();
    QCOMPARE(w->document().root->option("FORCEPV"), QString("CALC -D--- 1 0"));
    QTemporaryDir dir; w->saveTo(dir.filePath("copy"));
    QCOMPARE(loadConfig(dir.filePath("copy")).root->option("FORCEPV_CALC_A"), QString("input"));
  }
  void configurationAudit_data() {
    QTest::addColumn<bool>("editor"); QTest::addColumn<bool>("noLog");
    QTest::newRow("runtime") << false << false;
    QTest::newRow("editor") << true << false;
    QTest::newRow("runtime-no-log") << false << true;
    QTest::newRow("editor-no-log") << true << true;
  }
  void configurationAudit() {
    QFETCH(bool, editor); QFETCH(bool, noLog);
    QTemporaryDir dir; auto o = options(editor); o.noLog = noLog;
    o.config = dir.filePath("config"); o.alarmFile = dir.filePath("alarm");
    o.opmodFile = dir.filePath("ops"); o.lock = editor; o.broadcast = editor;
    saveConfig(parseConfig("GROUP NULL root\n$ALIAS Facility\nCHANNEL root pv\n"), o.config);
    auto w = std::make_unique<Window>(loadConfig(o.config), o, false);
    w->setAttribute(Qt::WA_DeleteOnClose, false);
    auto records = [&] { QFile f(o.opmodFile); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    QCOMPARE(records().count("Setup Config File : " + o.config.toLocal8Bit()), noLog ? 0 : 1);
    QVERIFY_THROWS_EXCEPTION(ParseError, w->saveTo(dir.filePath("missing/copy")));
    QVERIFY(!records().contains("Setup Save New Config"));
    const auto saved = dir.filePath("copy"); w->saveTo(saved);
    QCOMPARE(records().count("Setup Save New Config: " + saved.toLocal8Bit()), noLog ? 0 : 1);
    QCOMPARE(w->document().filename, editor ? saved : o.config);
    if (editor) {
      // Editor audit logging must never open the alarm destination or runtime locks.
      QVERIFY(!QFile::exists(o.alarmFile));
      QVERIFY(!QFile::exists(o.config + ".LOCK"));
      w->close();
      QCOMPARE(records().count("Setup---Exit"), noLog ? 0 : 1);
    }
    if (noLog) QVERIFY(!QFile::exists(o.opmodFile));
    else QVERIFY(records().contains("Facility: :  Setup Config File"));
  }
  void editorRootTitleFollowsUndo() {
    QTemporaryDir dir; auto o = options(true); o.noLog = false;
    o.config = dir.filePath("config"); o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
    saveConfig(parseConfig("GROUP NULL original\nCHANNEL original pv\n"), o.config);
    auto w = std::make_unique<Window>(loadConfig(o.config), o, false);
    QCOMPARE(w->windowTitle(), QString("Alarm Configuration Tool: original"));
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Properties Window") a->trigger();
    auto dialog = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("propertyNAME")->setText("renamed");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(w->document().root->name, QString("renamed"));
    QCOMPARE(w->windowTitle(), QString("Alarm Configuration Tool: renamed"));
    w->undoEdit(); QCOMPARE(w->windowTitle(), QString("Alarm Configuration Tool: original"));
    w->redoEdit(); QCOMPARE(w->windowTitle(), QString("Alarm Configuration Tool: renamed"));
    QFile f(o.opmodFile); QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll().count("Setup Config File"), 1); // Edits are not file loads.
  }
  void operationAliasFollowsReload_data() {
    QTest::addColumn<QString>("replacementAlias");
    QTest::addColumn<QString>("replacementName");
    QTest::addColumn<bool>("invalid");
    QTest::newRow("alias-renamed") << "New Facility" << "internal" << false;
    QTest::newRow("alias-removed") << "" << "internal" << false;
    QTest::newRow("root-renamed") << "New Facility" << "newroot" << false;
    QTest::newRow("rejected-reload") << "New Facility" << "newroot" << true;
  }
  void operationAliasFollowsReload() {
    QFETCH(QString, replacementAlias); QFETCH(QString, replacementName); QFETCH(bool, invalid);
    QTemporaryDir dir; auto o = options(false); o.broadcast = true; o.noLog = false; o.lock = true;
    o.configDir = dir.path(); o.config = dir.filePath("config");
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
#ifndef Q_OS_WIN
    UiQueue queue; QVERIFY(queue.id >= 0); o.databaseKey = queue.key;
#endif
    auto d = parseConfig("GROUP NULL internal\n$ALIAS Operator Facility\nCHANNEL internal pv\n");
    saveConfig(d, o.config);
    auto w = std::make_unique<Window>(loadConfig(o.config), o, false);
    auto record = [&] {
      auto n = w->document().channels()[0];
      w->alarmEngine().event(n, {});
      w->alarmEngine().event(n, {3, 2, 0, 1, "alarm"});
      w->alarmEngine().setMask(n, Mask::parse("A"));
      w->alarmEngine().setSilenceCurrent(true);
    };
    record();
#ifndef Q_OS_WIN
    QVERIFY(receiveQueue(queue.id).startsWith("1 1 internal  "));
    QVERIFY(receiveQueue(queue.id).startsWith("2 7 internal  "));
    QVERIFY(receiveQueue(queue.id).isEmpty());
#endif
    d.root->name = replacementName; d.root->setOption("ALIAS", replacementAlias); saveConfig(d, o.config);
    if (invalid) { QFile file(o.config); QVERIFY(file.open(QIODevice::Append)); file.write("INVALID\n"); }
    auto senderOptions = o; senderOptions.noLog = true; senderOptions.databaseKey = 0;
    Logging sender(senderOptions, "internal"); QVERIFY(sender.sendBroadcast("reload", 0, true));
    if (invalid) {
      QTRY_VERIFY(([&] { for (auto label : w->findChildren<QLabel*>())
        if (label->text().contains("Unknown statement")) return true; return false; })());
    } else QTRY_COMPARE(w->document().root->name + w->document().root->label(), d.root->name + d.root->label());
    const QString expectedName = invalid ? "internal" : replacementName;
    const QString expectedLabel = invalid ? "Operator Facility" : d.root->label();
    QCOMPARE(w->document().root->name, expectedName);
    QCOMPARE(w->windowTitle(), "Alarm Handler: " + expectedName);
    QCOMPARE(w->document().filename, o.config);
    // Existing log destinations and master ownership must survive the update.
    w->alarmEngine().resetMask(w->document().channels()[0]);
#ifndef Q_OS_WIN
    QVERIFY(receiveQueue(queue.id).startsWith("2 7 " + expectedName.toLocal8Bit() + "  "));
#endif
    record();
#ifndef Q_OS_WIN
    // A rejected reload retained its initialized channel, so normal and alarm
    // transitions both log; a successful reload skips its initial connection.
    if (invalid) QVERIFY(receiveQueue(queue.id).startsWith("1 1 internal  "));
    QVERIFY(receiveQueue(queue.id).startsWith("1 1 " + expectedName.toLocal8Bit() + "  "));
    const auto operation = receiveQueue(queue.id);
    QVERIFY(operation.startsWith("2 7 " + expectedName.toLocal8Bit() + "  "));
    QVERIFY(operation.contains(expectedLabel.toLocal8Bit() + ": pv:  Change Mask --A--"));
    QVERIFY(receiveQueue(queue.id).isEmpty());
#endif
    QFile file(o.opmodFile); QVERIFY(file.open(QIODevice::ReadOnly)); const auto text = file.readAll();
    QCOMPARE(text.count("Setup Config File : " + o.config.toLocal8Bit()), invalid ? 1 : 2);
    QVERIFY(text.contains(expectedLabel.toLocal8Bit() + ": :  Setup Config File : " + o.config.toLocal8Bit()));
    QVERIFY(text.contains("Operator Facility: pv:  Change Mask --A--"));
    QVERIFY(text.contains(expectedLabel.toLocal8Bit() + ": pv:  Change Mask --A--"));
    QFile alarms(o.alarmFile); QVERIFY(alarms.open(QIODevice::ReadOnly));
    QVERIFY(alarms.readAll().count("MAJOR") >= 2); // No truncation on reload.
  }
  void reloadSelectionIdentity_data() {
    QTest::addColumn<QString>("channels"); QTest::addColumn<bool>("retained");
    QTest::newRow("insert") << "CHANNEL root inserted\nCHANNEL root first\nCHANNEL root target\n$FORCEPV target_gate D 1 0\n" << true;
    QTest::newRow("reorder") << "CHANNEL root target\n$FORCEPV target_gate D 1 0\nCHANNEL root first\n" << true;
    QTest::newRow("removed") << "CHANNEL root first\n" << false;
    QTest::newRow("ambiguous") << "CHANNEL root target\nCHANNEL root target\n" << false;
  }
  void reloadSelectionIdentity() {
    QFETCH(QString, channels); QFETCH(bool, retained);
    QTemporaryDir dir; auto o = options(false); o.broadcast = true; o.configDir = dir.path(); o.config = dir.filePath("config");
    saveConfig(parseConfig("GROUP NULL root\nCHANNEL root first\nCHANNEL root target\n$FORCEPV target_gate D 1 0\n"), o.config);
    auto w = std::make_unique<Window>(loadConfig(o.config), o, false);
    auto view = w->findChild<QTreeView*>("groupContents"); view->setCurrentIndex(view->model()->index(1, 2));
    auto trigger = [&](const QString& name) { for (auto a : w->findChildren<QAction*>()) if (a->text() == name) a->trigger(); };
    trigger("Force Process Variable..."); trigger("Modify Mask Settings...");
    QPointer<QDialog> force = w->findChild<QDialog*>("forcePvDialog");
    QPointer<QDialog> mask = w->findChild<QDialog*>("modifyMaskDialog"); QVERIFY(force && mask);
    auto d = parseConfig("GROUP NULL root\n$ALIAS reloaded\n" + channels); saveConfig(d, o.config);
    Logging sender(o, "root"); QVERIFY(sender.sendBroadcast("reload", 0, true));
    QTRY_COMPARE(w->document().root->label(), QString("reloaded"));
    if (retained) {
      QTRY_VERIFY(force->findChild<QLineEdit*>("forcePvName"));
      QCOMPARE(force->findChild<QLineEdit*>("forcePvName")->text(), QString("target_gate"));
      QCOMPARE(view->currentIndex().data().toString(), QString("target"));
      mask->findChild<QPushButton*>("maskAction1_1")->click();
      for (auto n : w->document().channels()) QCOMPARE(w->alarmEngine().state(n).mask[Disable], n->name == "target");
    } else {
      QTRY_VERIFY(!force && !mask);
      trigger("Acknowledge Alarm"); trigger("NoAck for One Hour"); trigger("Display Guidance"); trigger("Start Related Process");
      trigger("Force Process Variable..."); QVERIFY(!w->findChild<QDialog*>("forcePvDialog"));
      for (auto n : w->document().channels()) QCOMPARE(w->alarmEngine().state(n).mask.text(), QString("-----"));
      view->setCurrentIndex(view->model()->index(0, 2)); trigger("Force Process Variable...");
      QVERIFY(w->findChild<QDialog*>("forcePvDialog"));
    }
  }
  void incrementalLiveLogReader() {
    QTemporaryDir dir; const auto path = dir.filePath("log");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray row = "12-Sep-2026 12:00:00 : " + QByteArray(170, 'x') + '\n';
    for (int i = 0; i < 20000; ++i) file.write(row);
    file.close();
    auto initial = readLiveLog({}, path);
    QVERIFY(initial.error.isEmpty()); QVERIFY(initial.reset && initial.limited);
    QCOMPARE(initial.lines.size(), LiveLogMaximumRecords);
    // Initial legacy recovery must examine every slot before retaining its tail.
    QCOMPARE(initial.bytesRead, QFileInfo(path).size() + 128);
    auto idle = readLiveLog(initial.cursor, path); QVERIFY(idle.lines.isEmpty()); QVERIFY(idle.bytesRead <= 128);
    QVERIFY(file.open(QIODevice::Append)); file.write("new record\npartial"); file.close();
    auto added = readLiveLog(initial.cursor, path); QCOMPARE(added.lines, QStringList{"new record"});
    QVERIFY(!added.reset); QVERIFY(added.bytesRead < 300);
    QVERIFY(file.open(QIODevice::Append)); file.write(" completed\n"); file.close();
    auto completed = readLiveLog(added.cursor, path); QCOMPARE(completed.lines, QStringList{"partial completed"});
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("replacement\n"); file.close();
    auto replaced = readLiveLog(completed.cursor, path); QVERIFY(replaced.reset); QCOMPARE(replaced.lines, QStringList{"replacement"});
    const auto today = dir.filePath("log.2026-09-13"); QFile next(today); QVERIFY(next.open(QIODevice::WriteOnly)); next.write("today\n"); next.close();
    auto switched = readLiveLog(replaced.cursor, today); QVERIFY(switched.reset); QCOMPARE(switched.lines, QStringList{"today"});
  }
  void legacyLiveRing_data() {
    QTest::addColumn<bool>("stale"); QTest::addColumn<bool>("byteLimit");
    for (bool stale : {false, true}) for (bool byteLimit : {false, true})
      QTest::newRow(qPrintable(QString("stale%1-bytes%2").arg(stale).arg(byteLimit))) << stale << byteLimit;
  }
  void legacyLiveRing() {
    QFETCH(bool, stale); QFETCH(bool, byteLimit);
    QTemporaryDir dir; const auto path = dir.filePath("ring");
    QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite));
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    auto row = [&](int id) {
      return (QLocale::c().toString(time.addSecs(id), "dd-MMM-yyyy HH:mm:ss") +
              QString(" : id=%1 ").arg(id, 4, 10, QChar('0'))).toLocal8Bit() +
              QByteArray(byteLimit ? 400 : 0, 'x') + '\n';
    };
    for (int id = 1001; id <= 1500; ++id) file.write(row(id));
    for (int id = 1; id <= 1000; ++id) file.write(row(id));
    QVERIFY(file.flush());
    if (stale) {
      QByteArray slot(64, 0); slot.replace(0, 8, "ALHPOS01");
      qToBigEndian<quint64>(1501, slot.data() + 8);
      qToBigEndian<quint64>(1500, slot.data() + 16);
      qToBigEndian<quint64>(1, slot.data() + 24); // Valid structure, stale fingerprint.
      slot += QCryptographicHash::hash(slot, QCryptographicHash::Sha256);
      QFile checkpoint(path + ".qtalh-position"); QVERIFY(checkpoint.open(QIODevice::WriteOnly));
      QCOMPARE(checkpoint.write(slot), qint64(slot.size()));
    }
    const auto live = readLiveLog({}, path);
    QVERIFY(live.error.isEmpty()); QVERIFY(live.reset && live.limited);
    const int retained = qMin<qint64>(LiveLogMaximumRecords, LiveLogMaximumBytes / row(1).size());
    QCOMPARE(live.lines.size(), retained);
    for (int i = 0; i < retained; ++i)
      QCOMPARE(live.lines[i] + '\n', QString::fromLocal8Bit(row(1501 - retained + i)));
    const auto idle = readLiveLog(live.cursor, path);
    QVERIFY(idle.lines.isEmpty()); QVERIFY(idle.bytesRead <= 128);
    int polls = 0;
    QVERIFY(readLiveLog({}, path, [&] { return ++polls > 10; }).retry);
  }
  void legacyLiveContinuations() {
    QTemporaryDir dir; const auto path = dir.filePath("ring");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray newest = "13-Sep-2026 12:00:02 : newest\nnewest continuation\n";
    const QByteArray oldest = "13-Sep-2026 12:00:01 : oldest\noldest continuation\n";
    file.write(newest + oldest); file.close();
    const auto live = readLiveLog({}, path);
    QVERIFY(live.error.isEmpty());
    QCOMPARE(live.lines.join('\n') + '\n', QString::fromLocal8Bit(oldest + newest));
  }
  void replacedLiveLog_data() {
    QTest::addColumn<bool>("grow"); QTest::addColumn<bool>("partial");
    for (bool grow : {false, true}) for (bool partial : {false, true})
      QTest::newRow(qPrintable(QString("grow%1-partial%2").arg(grow).arg(partial))) << grow << partial;
  }
  void replacedLiveLog() {
    QFETCH(bool, grow); QFETCH(bool, partial);
    QTemporaryDir dir; const auto path = dir.filePath("log");
    const QByteArray anchor = QByteArray(150, 'z') + (partial ? QByteArray() : QByteArray("\n"));
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("old head\n" + anchor); file.close();
    const auto initial = readLiveLog({}, path);
    QVERIFY(initial.error.isEmpty());
    // Preserve size, mtime and the entire anchor: only file identity distinguishes
    // the new snapshot. Keeping the old inode also prevents immediate inode reuse.
    QVERIFY(file.rename(path + ".old"));
    QFile replacement(path); QVERIFY(replacement.open(QIODevice::WriteOnly));
    replacement.write("new head\n" + anchor + (grow ? QByteArray("suffix\n") : QByteArray()));
    QVERIFY(replacement.flush());
    QVERIFY(replacement.setFileTime(initial.cursor.modified, QFileDevice::FileModificationTime));
    replacement.close();
    const auto next = readLiveLog(initial.cursor, path);
    QVERIFY(next.error.isEmpty()); QVERIFY(next.reset);
    const auto fresh = readLiveLog({}, path);
    QCOMPARE(next.lines, fresh.lines);
    QCOMPARE(next.cursor.partial, fresh.cursor.partial);
    QCOMPARE(next.lines.first(), QString("new head"));
  }
  void liveRingReader_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("text") << false;
    QTest::newRow("xml") << true;
  }
  void liveRingReader() {
    QFETCH(bool, xml);
    QTemporaryDir dir; Options o; o.maxRecords = 3; o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    o.xml = xml;
    Logging log(o, "root"); auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State s; s.severity = 2;
    const auto time = QDateTime::currentMSecsSinceEpoch();
    auto write = [&](const QString& value) { s.value = value; log.alarm(d.channels()[0], s, time); };
    auto hasValue = [xml](const QString& line, const QString& value) {
      return xml ? line.contains("<value>" + value + "</value>") : line.trimmed().endsWith(value);
    };
    write("A"); write("B"); write("C"); write("D");
    auto first = readLiveLog({}, o.alarmFile); QVERIFY(first.error.isEmpty()); QCOMPARE(first.lines.size(), 3);
    QVERIFY(hasValue(first.lines[0], "B")); QVERIFY(hasValue(first.lines[2], "D"));
    write("E"); auto next = readLiveLog(first.cursor, o.alarmFile); QVERIFY(!next.reset); QCOMPARE(next.lines.size(), 1);
    QVERIFY(hasValue(next.lines[0], "E"));
    // A whole cycle may produce identical physical contents; sequence numbers
    // must still deliver each new event, including equal timestamps and values.
    write("C"); write("D"); write("E");
    auto repeated = readLiveLog(next.cursor, o.alarmFile); QVERIFY(!repeated.reset); QCOMPARE(repeated.lines.size(), 3);
    QVERIFY(hasValue(repeated.lines[0], "C"));
    write(QString(100, 'X'));
    auto longer = readLiveLog(repeated.cursor, o.alarmFile);
    QVERIFY(!longer.reset); QCOMPARE(longer.lines.size(), 1);
    QVERIFY(hasValue(longer.lines[0], QString(xml ? 100 : 20, 'X')));
    auto cancelled = readLiveLog({}, o.alarmFile, [] { return true; }); QVERIFY(cancelled.retry);
  }
  void replacedLiveRing() {
    QTemporaryDir dir;
    Options o; o.maxRecords = 3; o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state;
    LiveLogUpdate initial;
    {
      Logging log(o, "root");
      state.value = "old"; log.alarm(d.channels()[0], state, 1000);
      initial = readLiveLog({}, o.alarmFile);
    }
    QVERIFY(initial.error.isEmpty()); QCOMPARE(initial.cursor.validatedSequence, quint64(1));
    QVERIFY(QFile::rename(o.alarmFile, o.alarmFile + ".old"));
    // The old checkpoint remains at its identity-qualified location. A new
    // file at the same path must acquire an independent cursor.
    {
      Logging log(o, "root");
      for (const auto& value : {"new A", "new B"}) {
        state.value = value; log.alarm(d.channels()[0], state, 2000);
      }
    }
    // The unrelated new ring has a higher valid sequence. Carrying the old
    // sequence across the replacement would deliver only B and retain "old".
    const auto next = readLiveLog(initial.cursor, o.alarmFile);
    QVERIFY(next.error.isEmpty()); QVERIFY(next.reset);
    QCOMPARE(next.cursor.validatedSequence, quint64(2));
    QCOMPARE(next.lines.size(), 2);
    QVERIFY(next.lines[0].trimmed().endsWith("new A"));
    QVERIFY(next.lines[1].trimmed().endsWith("new B"));
  }
  void liveRingCheckpointRecovery_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<int>("validSlot");
    for (bool xml : {false, true}) for (int slot : {0, 1})
      QTest::newRow(qPrintable(QString("xml%1-slot%2").arg(xml).arg(slot))) << xml << slot;
  }
  void liveRingCheckpointRecovery() {
    QFETCH(bool, xml); QFETCH(int, validSlot);
    QTemporaryDir dir; Options o; o.maxRecords = 3; o.xml = xml;
    o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    Logging log(o, "root"); auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2;
    const auto time = QDateTime::currentMSecsSinceEpoch();
    auto write = [&](const QString& value) { state.value = value; log.alarm(d.channels()[0], state, time); };
    QStringList expected;
    for (int i = 0; i < 4 + validSlot; ++i) {
      const auto value = "event-" + QString::number(i);
      write(value); expected << value;
      if (expected.size() > 3) expected.removeFirst();
    }
    // Give the other, stale slot the highest sequence and a valid checksum.
    QFile checkpoint(checkpointFor(o.alarmFile)); QVERIFY(checkpoint.open(QIODevice::ReadWrite));
    const auto data = checkpoint.readAll(); const int staleOffset = (1 - validSlot) * 96;
    auto stale = data.mid(staleOffset, 96);
    qToBigEndian<quint64>(100, stale.data() + 8);
    stale.replace(64, 32, QCryptographicHash::hash(stale.left(64), QCryptographicHash::Sha256));
    QVERIFY(checkpoint.seek(staleOffset)); QCOMPARE(checkpoint.write(stale), qint64(96)); checkpoint.close();
    const auto recovered = readLiveLog({}, o.alarmFile);
    QVERIFY(recovered.error.isEmpty()); QVERIFY(recovered.reset); QCOMPARE(recovered.lines.size(), 3);
    for (int i = 0; i < 3; ++i) QVERIFY(recovered.lines[i].contains(expected[i]));
    const auto idle = readLiveLog(recovered.cursor, o.alarmFile);
    QVERIFY(idle.lines.isEmpty()); QVERIFY(!idle.reset); QVERIFY(idle.bytesRead <= 128);
    // The next real write replaces the stale slot. Its sequence is below 100;
    // incremental delivery must use the checkpoint which actually matched.
    write("event-next");
    const auto next = readLiveLog(recovered.cursor, o.alarmFile);
    QVERIFY(next.error.isEmpty()); QVERIFY(!next.reset); QCOMPARE(next.lines.size(), 1);
    QVERIFY(next.lines[0].contains("event-next"));
  }
#ifndef Q_OS_WIN
  void logCheckpointThroughSymlink_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<bool>("writeAlias");
    for (bool xml : {false, true}) for (bool alias : {false, true})
      QTest::newRow(qPrintable(QString("xml%1-writeAlias%2").arg(xml).arg(alias))) << xml << alias;
  }
  void logCheckpointThroughSymlink() {
    QFETCH(bool, xml); QFETCH(bool, writeAlias);
    QTemporaryDir dir;
    const auto real = dir.filePath("ring"), alias = dir.filePath("alias");
    QFile file(real); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
    QVERIFY(QFile::link(real, alias));
    Options o; o.maxRecords = 3; o.xml = xml;
    o.alarmFile = writeAlias ? alias : real; o.opmodFile = dir.filePath("ops");
    Logging log(o, "root"); auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    State state; state.severity = 2;
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    auto write = [&](const QString& value) { state.value = value; log.alarm(d.channels()[0], state, time.toMSecsSinceEpoch()); };
    for (auto value : {"event-A", "event-B", "event-C", "event-D"}) write(value);
    for (const auto& path : {real, alias}) {
      const auto live = readLiveLog({}, path);
      QVERIFY(live.error.isEmpty()); QCOMPARE(live.lines.size(), 3);
      const QStringList expected = {"event-B", "event-C", "event-D"};
      for (int i = 0; i < 3; ++i) QVERIFY(live.lines[i].contains(expected[i]));
      LogSearch request; request.path = path; request.from = request.to = time;
      request.maximumRecords = 2;
      const auto search = searchLogs(request);
      QVERIFY(search.errors.isEmpty()); QVERIFY(search.truncated); QCOMPARE(search.records, 2);
      const auto rows = search.text.split('\n', Qt::SkipEmptyParts);
      for (int i = 0; i < 2; ++i) QVERIFY(rows[i].contains(expected[i]));
    }
    const auto before = readLiveLog({}, alias); write("event-E");
    const auto after = readLiveLog(before.cursor, alias);
    QVERIFY(!after.reset); QCOMPARE(after.lines.size(), 1); QVERIFY(after.lines[0].contains("event-E"));
  }
#endif
#ifndef Q_OS_WIN
  void renamedRingCheckpoint_data() {
    QTest::addColumn<bool>("freshReader"); QTest::addColumn<bool>("xml");
    for (bool fresh : {false, true}) for (bool xml : {false, true})
      QTest::newRow(qPrintable(QString("fresh%1-xml%2").arg(fresh).arg(xml))) << fresh << xml;
  }
  void renamedRingCheckpoint() {
    QFETCH(bool, freshReader); QFETCH(bool, xml);
    QTemporaryDir dir; Options o; o.lock = true; o.maxRecords = 3; o.xml = xml;
    o.config = dir.filePath("config"); o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    auto old = std::make_unique<Logging>(o, "root"); QStringList errors;
    auto error = [&](const QString& text) { errors << text; }; old->error = error;
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    auto write = [&](Logging& log, const QString& value) {
      state.value = "event-" + value; log.alarm(d.channels()[0], state, time.toMSecsSinceEpoch());
    };
    for (auto value : {"A", "B", "C", "D"}) write(*old, value);
    const auto oldCheckpoint = checkpointFor(o.alarmFile);
    const auto renamed = dir.filePath("renamed"); QVERIFY(QFile::rename(o.alarmFile, renamed));
    auto next = std::make_unique<Logging>(o, "root"); next->error = error;
    QVERIFY(oldCheckpoint != checkpointFor(o.alarmFile));
    for (auto value : {"X", "Y", "Z", "W"}) write(*next, value);
    write(*old, "E"); // Both descriptors must retain independent checkpoints.
    if (freshReader) {
      QFile file(renamed); QVERIFY(file.open(QIODevice::ReadOnly));
      if (logIdentity::readLocation(file).isEmpty()) QSKIP("Filesystem has no persistent xattrs");
      old.reset(); next.reset();
      QMutexLocker guard(&logIdentity::mutex); logIdentity::locations.clear();
    }
    auto check = [&](const QString& path, const QStringList& expected) {
      auto live = readLiveLog({}, path); QVERIFY(live.error.isEmpty()); QCOMPARE(live.lines.size(), expected.size());
      for (int i = 0; i < expected.size(); ++i) QVERIFY(live.lines[i].contains("event-" + expected[i]));
      LogSearch request; request.path = path; request.from = request.to = time;
      auto search = searchLogs(request); QVERIFY(search.errors.isEmpty());
      auto rows = search.text.split('\n', Qt::SkipEmptyParts); QCOMPARE(rows.size(), expected.size());
      for (int i = 0; i < expected.size(); ++i) QVERIFY(rows[i].contains("event-" + expected[i]));
    };
    check(renamed, {"C", "D", "E"}); check(o.alarmFile, {"Y", "Z", "W"});
    // Reopening an alias with another retention limit also forces recovery.
    o.alarmFile = renamed; o.maxRecords = 2;
    Logging reopened(o, "root"); reopened.error = error; write(reopened, "F");
    check(renamed, {"E", "F"});
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
  }
#endif
  void logCheckpointThroughHardLink_data() {
    QTest::addColumn<bool>("xml"); QTest::addColumn<bool>("writeAlias");
    QTest::addColumn<bool>("freshReader");
    for (bool xml : {false, true}) for (bool alias : {false, true}) for (bool fresh : {false, true})
      QTest::newRow(qPrintable(QString("xml%1-alias%2-fresh%3").arg(xml).arg(alias).arg(fresh)))
          << xml << alias << fresh;
  }
  void logCheckpointThroughHardLink() {
    QFETCH(bool, xml); QFETCH(bool, writeAlias); QFETCH(bool, freshReader);
    QTemporaryDir dir; QVERIFY(QDir(dir.path()).mkdir("different"));
    const auto real = dir.filePath("ring"), alias = dir.filePath("different/alias");
    QFile file(real); QVERIFY(file.open(QIODevice::ReadWrite));
#ifdef Q_OS_WIN
    QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()),
                            reinterpret_cast<LPCWSTR>(real.utf16()), nullptr));
#else
    QCOMPARE(::link(QFile::encodeName(real).constData(), QFile::encodeName(alias).constData()), 0);
#endif
    Options o; o.maxRecords = 3; o.xml = xml; o.lock = true;
    o.lockFile = dir.filePath("lock"); o.alarmFile = writeAlias ? alias : real; o.opmodFile = dir.filePath("ops");
    auto log = std::make_unique<Logging>(o, "root");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state; state.severity = 2;
    const auto time = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    auto write = [&](const QString& value) { state.value = value; log->alarm(d.channels()[0], state, time.toMSecsSinceEpoch()); };
    for (auto value : {"event-A", "event-B", "event-C", "event-D"}) write(value);
    if (freshReader) {
      if (logIdentity::readLocation(file).isEmpty()) QSKIP("Filesystem has no persistent xattrs/streams");
      log.reset();
      // Simulate a separate reader process: only the on-file locator survives.
      QMutexLocker guard(&logIdentity::mutex); logIdentity::locations.clear();
    }
    const QStringList expected = {"event-B", "event-C", "event-D"};
    for (const auto& path : {real, alias}) {
      const auto live = readLiveLog({}, path);
      QVERIFY(live.error.isEmpty()); QCOMPARE(live.lines.size(), 3);
      for (int i = 0; i < 3; ++i) QVERIFY(live.lines[i].contains(expected[i]));
      LogSearch request; request.path = path; request.from = request.to = time; request.maximumRecords = 2;
      const auto search = searchLogs(request); QVERIFY(search.errors.isEmpty()); QVERIFY(search.truncated);
      const auto rows = search.text.split('\n', Qt::SkipEmptyParts); QCOMPARE(rows.size(), 2);
      for (int i = 0; i < 2; ++i) QVERIFY(rows[i].contains(expected[i]));
    }
    const auto before = readLiveLog({}, alias);
    if (freshReader) {
      o.alarmFile = writeAlias ? real : alias; // Reopen through the other link.
      log = std::make_unique<Logging>(o, "root");
    }
    write("event-E");
    const auto after = readLiveLog(before.cursor, alias);
    QVERIFY(!after.reset); QCOMPARE(after.lines.size(), 1); QVERIFY(after.lines[0].contains("event-E"));
  }
#ifndef Q_OS_WIN
  void liveCheckpointWriteFailures() {
    QTemporaryDir dir; Options o; o.maxRecords = 1;
    o.alarmFile = dir.filePath("ring"); o.opmodFile = dir.filePath("ops");
    Logging log(o, "root"); auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n"); State state;
    auto write = [&](int value) { state.value = QString("record-%1").arg(value); log.alarm(d.channels()[0], state, 1000); };
    write(1); write(2); const auto before = readLiveLog({}, o.alarmFile);
    QCOMPARE(before.cursor.validatedSequence, quint64(2));
    QStringList errors; log.error = [&](const QString& message) { errors << message; };
    struct rlimit original{}, restricted{}; QCOMPARE(getrlimit(RLIMIT_FSIZE, &original), 0);
    restricted = original; restricted.rlim_cur = 128;
    auto handler = std::signal(SIGXFSZ, SIG_IGN);
    const int limited = setrlimit(RLIMIT_FSIZE, &restricted);
    if (!limited) { write(3); write(4); }
    const int restored = setrlimit(RLIMIT_FSIZE, &original); std::signal(SIGXFSZ, handler);
    QCOMPARE(limited, 0); QCOMPARE(restored, 0); QCOMPARE(errors.size(), 2);
    for (const auto& error : errors) QVERIFY(error.contains("Cannot save alarm log position"));
    write(5); QCOMPARE(errors.size(), 2);
    const auto after = readLiveLog(before.cursor, o.alarmFile);
    QVERIFY(after.error.isEmpty()); QCOMPARE(after.cursor.validatedSequence, quint64(5));
    // Three new alarms exceed the retained ring. Do not present this as just
    // one new record appended to the obsolete pre-outage display.
    QVERIFY(after.reset); QVERIFY(after.limited); QCOMPARE(after.lines.size(), 1);
    QVERIFY(after.lines[0].contains("record-5"));
  }
#endif
  void liveLogViews_data() {
    QTest::addColumn<bool>("alarm"); QTest::addColumn<bool>("dated");
    QTest::newRow("alarm") << true << false; QTest::newRow("operation") << false << false;
    QTest::newRow("dated-alarm") << true << true; QTest::newRow("dated-operation") << false << true;
  }
  void liveLogViews() {
    QFETCH(bool, alarm); QFETCH(bool, dated);
    QTemporaryDir dir; auto o = options(false); o.noLog = false; o.maxRecords = 2; o.dated = dated;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    auto trigger = [&](const QString& name) { for (auto a : w->findChildren<QAction*>()) if (a->text() == name) a->trigger(); };
    auto n = w->document().channels()[0]; auto& e = w->alarmEngine(); e.event(n, {});
    e.event(n, {3, 2, 0, 1, "event-A"}); e.event(n, {4, 1, 0, 1, "event-B"});
    e.modifyMask(n, Log, 1); e.modifyMask(n, Log, 0);
    trigger(alarm ? "Alarm Log File" : "Operation Log File");
    auto dialog = w->findChild<QDialog*>(alarm ? "liveAlarmLog" : "liveOperationLog"); QVERIFY(dialog);
    auto text = dialog->findChild<QPlainTextEdit*>("liveLogText");
    QTRY_VERIFY(text->toPlainText().contains(alarm ? "event-B" : "Modify Mask"));
    if (alarm) {
      e.event(n, {3, 2, 0, 1, "event-C"});
      QTRY_VERIFY(text->toPlainText().contains("event-C"));
      QVERIFY(text->toPlainText().indexOf("event-B") < text->toPlainText().indexOf("event-C"));
    }
    const auto newPath = dir.filePath("new-log");
    QTimer::singleShot(0, w.get(), [&] {
      auto chooser = w->findChild<QDialog*>("fileSelectionDialog"); QVERIFY(chooser);
      if (auto native = qobject_cast<QFileDialog*>(chooser)) { native->selectFile(newPath); QMetaObject::invokeMethod(native, "accept"); }
      else { auto selection = chooser->findChild<QLineEdit*>("fileSelection"); QVERIFY(selection); selection->setText(newPath); QMetaObject::invokeMethod(selection, "returnPressed"); }
    });
    trigger(alarm ? "New Alarm Log File..." : "New Operation Modification Log File...");
    e.event(n, {4, 1, 0, 1, "new-destination"}); e.modifyMask(n, Log, 1);
    const auto actual = newPath + (dated ? QDate::currentDate().toString(".yyyy-MM-dd") : QString());
    QTRY_COMPARE(dialog->findChild<QLabel*>("liveLogPath")->text(), "File: " + actual);
    QTRY_VERIFY(text->toPlainText().contains(alarm ? "new-destination" : "Modify Mask"));
    QVERIFY(!text->toPlainText().contains("event-B"));
    QPointer<QDialog> lifetime = dialog; dialog->close(); QTRY_VERIFY(!lifetime);
  }
  void datedLogSelection_data() {
    QTest::addColumn<bool>("alarm");
    QTest::addColumn<QString>("selection");
    for (bool alarm : {true, false})
      for (const auto& selection : {"unchanged", "today", "older", "basename", "cancel",
                                    "current-date-base", "new-date-base", "invalid-date"}) {
        const auto row = QByteArray(alarm ? "alarm-" : "opmod-") + selection;
        QTest::newRow(row.constData()) << alarm << QString(selection);
      }
  }
  void datedLogSelection() {
    QFETCH(bool, alarm); QFETCH(QString, selection);
    QTemporaryDir dir;
    auto o = options(false); o.noLog = false; o.maxRecords = 0; o.dated = true;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    if (selection == "current-date-base") {
      o.alarmFile += ".2000-01-01"; o.opmodFile += ".2000-01-01";
    }
    const auto base = alarm ? o.alarmFile : o.opmodFile;
    const auto suffix = QDate::currentDate().toString(".yyyy-MM-dd");
    auto chosen = base, expected = base;
    bool ambiguous = false;
    if (selection == "today") {
      chosen = base + suffix; ambiguous = true;
    } else if (selection == "older" || selection == "basename" || selection == "cancel") {
      chosen = dir.filePath("other.2000-01-01"); ambiguous = true;
      if (selection != "cancel")
        expected = selection == "basename" ? chosen : dir.filePath("other");
    } else if (selection == "new-date-base") {
      chosen = expected = dir.filePath("new.2000-01-01");
    } else if (selection == "invalid-date") {
      chosen = expected = dir.filePath("other.2000-02-30");
    }
    if (ambiguous || selection == "current-date-base" || selection == "invalid-date") {
      QFile file(chosen); QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("preserved-existing-record\n");
    }
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    auto trigger = [&](const QString& name) {
      for (auto a : w->findChildren<QAction*>()) if (a->text() == name) a->trigger();
    };
    trigger(alarm ? "Alarm Log File" : "Operation Log File");
    auto live = w->findChild<QDialog*>(alarm ? "liveAlarmLog" : "liveOperationLog");
    QVERIFY(live);
    QString prefilled;
    bool submitted = false, sawChoice = false, timedOut = false;
    QElapsedTimer deadline; deadline.start();
    QTimer responder;
    connect(&responder, &QTimer::timeout, w.get(), [&] {
      auto modal = qobject_cast<QDialog*>(qApp->activeModalWidget());
      if (!modal) return;
      if (deadline.elapsed() > 5000) { timedOut = true; modal->reject(); return; }
      if (auto message = qobject_cast<QMessageBox*>(modal)) {
        if (message->windowTitle() == "Dated Log File") {
          sawChoice = true;
          const auto label = selection == "basename" ? "Use as Basename" : "Use Log Series";
          if (selection == "cancel") message->button(QMessageBox::Cancel)->click();
          else for (auto button : message->buttons()) if (button->text() == label) button->click();
        } else if (message->button(QMessageBox::Yes)) {
          message->button(QMessageBox::Yes)->click();
        }
      } else if (!submitted && modal->objectName() == "fileSelectionDialog") {
        submitted = true;
        // Accept outside this timer callback so it can also answer any nested
        // overwrite prompt from either the Motif or Fusion chooser.
        QTimer::singleShot(0, &responder, [&, modal] {
          if (auto chooser = qobject_cast<QFileDialog*>(modal)) {
            prefilled = chooser->selectedFiles().value(0);
            // selectFile() can leave a focused filename editor unchanged.
            // Enter the path as the operator would in the visible dialog.
            auto field = chooser->findChild<QLineEdit*>("fileNameEdit");
            if (!field) { timedOut = true; modal->reject(); return; }
            field->setText(chosen);
            QMetaObject::invokeMethod(chooser, "accept");
          } else {
            auto field = modal->findChild<QLineEdit*>("fileSelection");
            if (!field) { timedOut = true; modal->reject(); return; }
            prefilled = field->text(); field->setText(chosen);
            QMetaObject::invokeMethod(field, "returnPressed");
          }
        });
      }
    });
    responder.start(10);
    trigger(alarm ? "New Alarm Log File..." : "New Operation Modification Log File...");
    responder.stop();
    QVERIFY(!timedOut); QVERIFY(submitted); QCOMPARE(prefilled, base);
    QCOMPARE(sawChoice, ambiguous);
    auto& engine = w->alarmEngine(); auto n = w->document().channels()[0];
    engine.event(n, {}); engine.event(n, {3, 2, 0, 1, "after-selection"});
    engine.modifyMask(n, Log, 1);
    QTRY_COMPARE(live->findChild<QLabel*>("liveLogPath")->text(), "File: " + expected + suffix);
    QFile output(expected + suffix); QVERIFY(output.open(QIODevice::ReadOnly));
    QVERIFY(output.readAll().contains(alarm ? "after-selection" : "Modify Mask"));
    if (ambiguous) {
      QFile original(chosen); QVERIFY(original.open(QIODevice::ReadOnly));
      QVERIFY(original.readAll().contains("preserved-existing-record"));
    }
    if (ambiguous && selection != "basename") QVERIFY(!QFileInfo::exists(chosen + suffix));
  }
  void filteredAlarmAcrossMidnight_data() {
    QTest::addColumn<bool>("xml");
    QTest::newRow("text") << false; QTest::newRow("xml") << true;
  }
  void filteredAlarmAcrossMidnight() {
    QFETCH(bool, xml);
    QTemporaryDir dir;
    Options o; o.dated = true; o.xml = xml; o.maxRecords = 0;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("opmod");
    const auto yesterday = QDate::currentDate().addDays(-1);
    qint64 clock = QDateTime(yesterday, QTime(23, 59, 58)).toMSecsSinceEpoch();
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n$ALARMCOUNTFILTER -1 2\n");
    Engine e(d); e.now = e.monotonicNow = [&] { return clock; }; Logging log(o, "root");
    e.alarmLog = [&](Node* n, const State& state, qint64 time) { log.alarm(n, state, time); };
    auto n = d.channels()[0]; e.event(n, {}); clock += 1000;
    e.event(n, {3, 2, 0, 1, "before-midnight"});
    QCOMPARE(e.state(n).severity, 0);
    clock += 2000; e.tick(); QCOMPARE(e.state(n).severity, 2);
    e.event(n, {0, 0, 0, 1, "after-midnight"});
    LogSearch request; request.path = o.alarmFile;
    request.from = QDateTime(yesterday, QTime(0, 0));
    request.to = QDateTime(yesterday, QTime(23, 59, 59, 999));
    auto result = searchLogs(request);
    QCOMPARE(result.records, 1); QVERIFY(result.text.contains("before-midnight"));
    request.from = request.from.addDays(1); request.to = request.to.addDays(1);
    result = searchLogs(request);
    QCOMPARE(result.records, 1); QVERIFY(result.text.contains("after-midnight"));
    log.operation(n, "current operation");
    QFile op(log.opmodPath()); QVERIFY(op.open(QIODevice::ReadOnly));
    QVERIFY(op.readAll().contains("current operation"));
  }

  void initTestCase() {
    qputenv("XDG_CONFIG_HOME", notificationConfig.path().toUtf8());
    QCoreApplication::setApplicationName("qtalh-ui-tests-" + QString::number(QCoreApplication::applicationPid()));
    initializeAppearance(qEnvironmentVariable("QTALH_TEST_STYLE"));
  }
#ifdef Q_OS_WIN
  void windowsShellCommand() {
    QTemporaryDir dir;
    const auto path = dir.filePath("command output.txt");
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->alarmEngine().command("echo qtalh-command>\"" + QDir::toNativeSeparators(path) + "\"");
    QTRY_VERIFY(QFileInfo::exists(path));
    QFile output(path);
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll().trimmed(), QByteArray("qtalh-command"));
  }
#endif
#ifndef Q_OS_WIN
  void legacyDisplayCommand_data() {
    QTest::addColumn<QString>("program");
    QTest::newRow("bare") << "medm";
    QTest::newRow("quoted") << "\"medm\"";
    QTest::newRow("absolute") << "'/legacy tools/bin/medm'";
    QTest::newRow("master only") << "MASTER_ONLY medm";
    QTest::newRow("already qt") << "qtedm";
  }
  void legacyDisplayCommand() {
    QFETCH(QString, program);
    QTemporaryDir dir;
    QFile stub(dir.filePath("qtedm"));
    QVERIFY(stub.open(QIODevice::WriteOnly));
    stub.write("#!/bin/sh\nprintf '%s\\n' \"$@\"\n");
    stub.close();
    QVERIFY(stub.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString command = program + " -x -attach 'medm display.adl' > '" +
                            dir.filePath("output") + "'";
    auto d = parseConfig("GROUP NULL root\n$COMMAND " + command);
    auto w = std::make_unique<Window>(d, options(false), false);
    const auto oldPath = qgetenv("PATH");
    qputenv("PATH", dir.path().toLocal8Bit() + ":" + oldPath);
    w->alarmEngine().command(w->document().root->option("COMMAND"));
    qputenv("PATH", oldPath);
    auto output = [&] {
      QFile file(dir.filePath("output"));
      return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    QTRY_COMPARE(output(), QByteArray("-x\n-attach\nmedm display.adl\n"));
    QCOMPARE(w->document().root->option("COMMAND"), command);
  }
#endif

  void analyticsWorkflow() {
    auto w = std::make_unique<Window>(sample(), options(false), false); w->show();
    auto service = w->alarmAnalytics(); QVERIFY(service);
    qint64 now = 0; service->monotonicNow = [&] { return now; };
    service->utcNow = [&] { return 1700000000000 + now; }; service->reset();
    auto& engine = w->alarmEngine(); const auto nodes = w->document().channels();
    for (auto n : nodes) engine.event(n, {0,0,0,1,"0"});
    for (int i=0;i<5;++i) {
      now += 1000; engine.event(nodes[0], {3,2,2,1,"20"});
      now += 100; engine.acknowledge(nodes[0]);
      now += 100; engine.event(nodes[0], {0,0,0,1,"0"});
    }
    now += 1000; engine.event(nodes[1], {3,2,2,1,"20"});
    engine.shelve(nodes[1],1,"Analytics screenshot", "tester");
    now += 2000;
    for (auto action:w->findChildren<QAction*>()) if(action->text()=="Alarm Analytics...") action->trigger();
    auto dialog=w->findChild<QDialog*>("analyticsDialog"); QVERIFY(dialog);
    auto tabs=dialog->findChild<QTabWidget*>("analyticsTabs"); QCOMPARE(tabs->count(),4);
    auto table=dialog->findChild<QTableView*>("analyticsTable0"); QVERIFY(table);
    QTRY_COMPARE(table->model()->rowCount(),3);
    QCOMPARE(table->model()->index(0,1).data().toString(),QString("5"));
    QCOMPARE(dialog->findChild<QComboBox*>("analyticsRange")->currentIndex(),1);
    // Every title must fit when its column becomes the sort column.
    for (int t = 0; t < 4; ++t) {
      tabs->setCurrentIndex(t); QCoreApplication::processEvents();
      auto data = dialog->findChild<QTableView*>("analyticsTable" + QString::number(t));
      auto header = data->horizontalHeader();
      for (int col = 0; col < data->model()->columnCount(); ++col) {
        data->sortByColumn(col, Qt::DescendingOrder);
        QVERIFY2(data->columnWidth(col) >= header->sectionSizeHint(col),
                 qPrintable(data->model()->headerData(col, Qt::Horizontal).toString()));
      }
      data->sortByColumn(t == 1 ? 3 : 1, Qt::DescendingOrder);
    }
    QDir().mkpath(TEST_OUTPUT);
    for(int t=0;t<4;++t){tabs->setCurrentIndex(t);QCoreApplication::processEvents();
      QVERIFY(dialog->grab().save(QString(TEST_OUTPUT)+(legacyAppearance()?"/classic-analytics-":"/fusion-analytics-")+QString::number(t)+".png"));}
    tabs->setCurrentIndex(0);table->setCurrentIndex(table->model()->index(0,0));
    dialog->findChild<QPushButton*>("analyticsDetails")->click();
    QVERIFY(dialog->findChild<QDialog*>("analyticsChannelDetails"));
    QTemporaryDir dir; auto output=dir.filePath("analytics.csv");
    QTimer::singleShot(0,dialog,[dialog,output] {
      auto file=dialog->findChild<QDialog*>("fileSelectionDialog"); QVERIFY(file);
      if(auto native=qobject_cast<QFileDialog*>(file)){native->selectFile(output);QMetaObject::invokeMethod(native,"accept");}
      else {auto selection=file->findChild<QLineEdit*>("fileSelection"); QVERIFY(selection);selection->setText(output);QMetaObject::invokeMethod(selection,"returnPressed");}
    });
    dialog->findChild<QPushButton*>("analyticsExportTable")->click();
    QFile csv(output);QVERIFY(csv.open(QIODevice::ReadOnly));QVERIFY(csv.readAll().contains("activation_share_percent"));
    auto search=dialog->findChild<QLineEdit*>("analyticsSearch");search->setText("timing");
    now+=1000;QTRY_COMPARE(table->model()->rowCount(),1);
    dialog->findChild<QPushButton*>("analyticsReset")->click();
    now+=1000;QTRY_COMPARE(table->model()->rowCount(),1);
    QCOMPARE(table->model()->index(0,1).data().toString(),QString("0"));
    auto editor=std::make_unique<Window>(sample(),options(true),false);QVERIFY(!editor->alarmAnalytics());
  }
  void analyticsPreservesScrollOnRefresh() {
    QString config = "GROUP NULL scroll\n";
    for (int i = 0; i < 100; ++i) config += QString("CHANNEL scroll pv%1\n").arg(i);
    auto w = std::make_unique<Window>(parseConfig(config), options(false), false);
    w->show();
    auto service = w->alarmAnalytics();
    qint64 now = 0;
    service->monotonicNow = [&] { return now; };
    service->utcNow = [&] { return 1700000000000LL + now; };
    service->reset();
    for (auto channel : w->document().channels()) {
      w->alarmEngine().event(channel, {0, 0, 0, 1, "0"});
      w->alarmEngine().event(channel, {3, 2, 2, 1, "12"});
    }
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Alarm Analytics...") action->trigger();
    auto dialog = w->findChild<QDialog*>("analyticsDialog"); QVERIFY(dialog);
    auto tabs = dialog->findChild<QTabWidget*>("analyticsTabs");
    for (int tab = 0; tab < 4; ++tab) {
      tabs->setCurrentIndex(tab);
      auto table = dialog->findChild<QTableView*>("analyticsTable" + QString::number(tab));
      QTRY_COMPARE(table->model()->rowCount(), 100);
      table->setColumnWidth(0, 1000);
      table->setCurrentIndex(table->model()->index(50, 2));
      table->doItemsLayout();
      auto horizontal = table->horizontalScrollBar();
      auto vertical = table->verticalScrollBar();
      QVERIFY(horizontal->maximum() > 0); QVERIFY(vertical->maximum() > 0);
      horizontal->setValue(qMax(1, horizontal->maximum() / 2));
      vertical->setValue(qMax(1, vertical->maximum() / 2));
      const int x = horizontal->value(), y = vertical->value();
      const auto selected = table->currentIndex().data(Qt::UserRole).toString();
      QSignalSpy refreshed(table->model(), &QAbstractItemModel::modelReset);
      for (int round = 0; round < 2; ++round) {
        const int prior = refreshed.count();
        now += 1000;
        QTRY_VERIFY(refreshed.count() > prior);
        QCoreApplication::processEvents();
        QCOMPARE(horizontal->value(), x); QCOMPARE(vertical->value(), y);
        QCOMPARE(table->currentIndex().column(), 2);
        QCOMPARE(table->currentIndex().data(Qt::UserRole).toString(), selected);
        QCOMPARE(table->columnWidth(0), 1000);
      }
    }
  }

  void analyticsLargeDashboard() {
    QString config="GROUP NULL large\n"; for(int i=0;i<10000;++i)config+=QString("CHANNEL large pv%1\n").arg(i);
    auto w=std::make_unique<Window>(parseConfig(config),options(false),false);
    auto& e=w->alarmEngine(); for(auto n:w->document().channels()){e.event(n,{0,0,0,1,"0"});e.event(n,{3,2,2,1,"20"});}
    for(auto action:w->findChildren<QAction*>())if(action->text()=="Alarm Analytics...")action->trigger();
    auto dialog=w->findChild<QDialog*>("analyticsDialog");QVERIFY(dialog);
    auto table=dialog->findChild<QTableView*>("analyticsTable0");QElapsedTimer time;time.start();
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(),10000,10000);
    qInfo()<<"10,000-channel analytics dashboard first result, ms:"<<time.elapsed();
    QElapsedTimer reaction;reaction.start();table->setCurrentIndex(table->model()->index(9999,0));QCoreApplication::processEvents();
    QVERIFY(reaction.elapsed()<1000);
  }

  void notificationFirstRun() {
    NotificationStore store; store.load(); store.save(NotificationSettings{});
    QTemporaryDir dir;
    auto document = sample(); document.filename = dir.filePath("alarms.alh");
    auto w = std::make_unique<Window>(std::move(document), options(false), false);
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Notifications...") action->trigger();
    auto dialog = w->findChild<QDialog*>("notificationsDialog"); QVERIFY(dialog);
    auto tabs = dialog->findChild<QTabWidget*>();
    QCOMPARE(tabs->tabText(0), QString("Destinations"));
    QCOMPARE(tabs->currentIndex(), 0);
    tabs->setCurrentIndex(1);
    auto add = dialog->findChild<QPushButton*>("addNotificationSubscription");
    // Cancelling the prerequisite leaves no partial destination or subscription.
    QTimer::singleShot(0, dialog, [dialog] {
      auto editor = dialog->findChild<QDialog*>("notificationDestinationEditor");
      QVERIFY(editor); editor->reject();
    });
    add->click();
    QCOMPARE(dialog->findChild<QListWidget*>("notificationDestinations")->count(), 0);
    QTimer::singleShot(0, dialog, [dialog] {
      auto editor = dialog->findChild<QDialog*>("notificationDestinationEditor");
      QVERIFY(editor);
      editor->findChild<QLineEdit*>("destinationName")->setText("Operators");
      editor->findChild<QComboBox*>()->setCurrentText("webhook");
      editor->findChild<QLineEdit*>("webhookUrl")->setText("http://127.0.0.1:1/test");
      QTimer::singleShot(0, dialog, [dialog] {
        auto subscription = dialog->findChild<QDialog*>("notificationSubscriptionEditor");
        QVERIFY(subscription); subscription->reject();
      });
      editor->accept();
    });
    add->click();
    QCOMPARE(dialog->findChild<QListWidget*>("notificationDestinations")->count(), 1);
    QCOMPARE(dialog->findChild<QListWidget*>("notificationSubscriptions")->count(), 0);
    QVERIFY(!dialog->findChild<QLabel*>("notificationSetupHint")->text().contains("first"));
    dialog->findChild<QPushButton*>("saveNotifications")->click();
    QCOMPARE(store.load().destinations.size(), 1);
    store.save(NotificationSettings{});
  }

  void notificationWorkflow() {
    QTemporaryDir dir;
    auto document = sample(); document.filename = dir.filePath("alarms.alh");
    NotificationSettings settings;
    NotificationDestination destination;
    destination.id = "ops"; destination.name = "Operators";
    destination.kind = "webhook"; destination.url = "http://127.0.0.1:1/test";
    settings.destinations << destination;
    NotificationSubscription subscription;
    subscription.id = "rule"; subscription.name = "Power supply alarms";
    subscription.configuration = notificationConfiguration(document.filename);
    subscription.wildcard = "test:power";
    subscription.stages = {{"initial", 60, {"ops"}}, {"escalate", 600, {"ops"}}};
    settings.subscriptions << subscription;
    NotificationStore store; store.load(); store.save(settings);
    auto open = [](Window* w) {
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == "Notifications...") action->trigger();
      return w->findChild<QDialog*>("notificationsDialog");
    };
    auto w = std::make_unique<Window>(std::move(document), options(false), false);
    w->show();
    auto dialog = open(w.get()); QVERIFY(dialog);
    auto enable = dialog->findChild<QCheckBox*>("notificationsEnabled");
    QVERIFY(enable); QVERIFY(!enable->isChecked());
    auto notificationIndicator = w->findChild<QLabel*>("notificationsEnabledStatus");
    QVERIFY(notificationIndicator); QVERIFY(!notificationIndicator->isHidden());
    QCOMPARE(notificationIndicator->text(), QString("Notifications Enabled: NO"));
    auto rules = dialog->findChild<QListWidget*>("notificationSubscriptions");
    auto destinations = dialog->findChild<QListWidget*>("notificationDestinations");
    QCOMPARE(rules->count(), 1); QCOMPARE(destinations->count(), 1);
    rules->setCurrentRow(0);
    QTimer::singleShot(0, dialog, [dialog] {
      auto editor = dialog->findChild<QDialog*>("notificationSubscriptionEditor");
      QVERIFY(editor);
      QCOMPARE(editor->findChild<QLabel*>("subscriptionMatchCount")->text(),
               QString("Matches 1 channels in this configuration"));
      QDir().mkpath(TEST_OUTPUT);
      QVERIFY(editor->grab().save(QString(TEST_OUTPUT) +
          (legacyAppearance() ? "/classic-notification-subscription.png" : "/fusion-notification-subscription.png")));
      editor->findChild<QLineEdit*>("subscriptionName")->setText("Updated subscription");
      editor->accept();
    });
    auto page = dialog->findChild<QTabWidget*>()->currentWidget();
    QVERIFY(page->isAncestorOf(rules));
    for (auto button : page->findChildren<QPushButton*>())
      if (button->text() == "Edit") button->click();
    QVERIFY(!dialog->findChild<QPushButton*>("testNotification")->isEnabled());
    dialog->findChild<QPushButton*>("saveNotifications")->click();
    QCOMPARE(store.load().subscriptions.first().name, QString("Updated subscription"));
    enable->setChecked(true);
    QTRY_COMPARE(notificationIndicator->text(), QString("Notifications Enabled: YES"));
    enable->setChecked(false);
    QTRY_COMPARE(notificationIndicator->text(), QString("Notifications Enabled: NO"));
    enable->setChecked(true);
    QTest::qWait(550);
    QVERIFY(dialog->findChild<QLabel*>("notificationStatus")->text().contains("enabled"));
    QDir().mkpath(TEST_OUTPUT);
    QVERIFY(dialog->grab().save(QString(TEST_OUTPUT) +
        (legacyAppearance() ? "/classic-notifications.png" : "/fusion-notifications.png")));
    auto freshDocument = sample(); freshDocument.filename = dir.filePath("alarms.alh");
    auto fresh = std::make_unique<Window>(std::move(freshDocument), options(false), false);
    auto freshDialog = open(fresh.get()); QVERIFY(freshDialog);
    QVERIFY(!freshDialog->findChild<QCheckBox*>("notificationsEnabled")->isChecked());
    auto unrelatedDocument = sample(); unrelatedDocument.filename = dir.filePath("other.alh");
    auto unrelated = std::make_unique<Window>(std::move(unrelatedDocument), options(false), false);
    QVERIFY(unrelated->findChild<QLabel*>("notificationsEnabledStatus")->isHidden());
    store.save(NotificationSettings{});
    dialog->findChild<QPushButton*>("reloadNotifications")->click();
    QTRY_VERIFY(notificationIndicator->isHidden());
  }
  void notificationsUnavailableInEditor() {
    auto w = std::make_unique<Window>(sample(), options(true), false);
    QVERIFY(w->findChild<QLabel*>("notificationsEnabledStatus")->isHidden());
    for (auto action : w->findChildren<QAction*>())
      QVERIFY(action->text() != "Notifications...");
  }

  void shelvingContextMenu() {
    auto w = std::make_unique<Window>(parseConfig(
        "GROUP NULL root\nCHANNEL root one\nCHANNEL root two\n"
        "GROUP root section\nCHANNEL section three\nCHANNEL section four\n"), options(false), false);
    w->show();
    auto& engine = w->alarmEngine();
    const auto channels = w->document().channels();
    for (auto n : channels) engine.event(n, {3, 2, 2, 1, "12"});
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto contents = w->findChild<QTreeView*>("groupContents");
    QCoreApplication::processEvents();
    auto openMenu = [](QTreeView* view, const QModelIndex& index) {
      const auto point = view->visualRect(index).center();
      QTest::mouseClick(view->viewport(), Qt::RightButton, Qt::NoModifier, point);
      QContextMenuEvent context(QContextMenuEvent::Mouse, point, view->viewport()->mapToGlobal(point));
      QApplication::sendEvent(view->viewport(), &context);
      return view->findChild<QMenu*>("alarmContextMenu");
    };
    contents->setCurrentIndex(contents->model()->index(2, 2));
    // Right-clicking even the acknowledgement cell must not acknowledge the alarm.
    auto menu = openMenu(contents, contents->model()->index(1, 0)); QVERIFY(menu);
    QCOMPARE(engine.state(channels[0]).unack, 2);
    auto action = menu->findChild<QAction*>("shelveContextTarget"); QVERIFY(action);
    QCOMPARE(action->text(), QString("Shelve this channel..."));
    QCOMPARE(menu->actions().size(), 2);
    auto detailsAction = menu->actions()[1];
    QCOMPARE(detailsAction->text(), QString("Alarm Handler Properties..."));
    detailsAction->trigger();
    auto properties = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(properties);
    QCOMPARE(properties->windowTitle(), QString("Alarm Handler Properties"));
    QCOMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("one"));
    properties->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    menu = openMenu(contents, contents->model()->index(1, 0)); QVERIFY(menu);
    menu->findChild<QAction*>("shelveContextTarget")->trigger(); menu->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QPointer<QDialog> first = w->findChild<QDialog*>("shelveDialog"); QVERIFY(first);
    QCOMPARE(first->findChild<QLabel*>("shelfTarget")->text(), QString("/root/one"));
    contents->setCurrentIndex(contents->model()->index(2, 2));
    QCOMPARE(first->findChild<QLabel*>("shelfTarget")->text(), QString("/root/one"));
    menu = openMenu(contents, contents->model()->index(2, 2)); QVERIFY(menu);
    menu->findChild<QAction*>("shelveContextTarget")->trigger(); menu->close();
    QVERIFY(!first || !first->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    auto dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLabel*>("shelfTarget")->text(), QString("/root/two"));
    dialog->findChild<QLineEdit*>("shelfUsername")->setText("ui_tester");
    dialog->findChild<QLineEdit*>("shelfReason")->setText("Right-click channel");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(engine.state(channels[0]).shelf.until, qint64(0));
    QVERIFY(engine.state(channels[1]).shelf.until > engine.now());
    menu = openMenu(contents, contents->model()->index(0, 2)); QVERIFY(menu);
    action = menu->findChild<QAction*>("shelveContextTarget");
    QCOMPARE(action->text(), QString("Shelve this group..."));
    menu->actions()[1]->trigger();
    properties = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(properties);
    QCOMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("section"));
    properties->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(tree->model()->index(0, 2)).center());
    menu = openMenu(contents, contents->model()->index(0, 2)); QVERIFY(menu);
    menu->findChild<QAction*>("shelveContextTarget")->trigger(); menu->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLabel*>("shelfTarget")->text(), QString("/root/section"));
    QVERIFY(dialog->findChild<QLabel*>("shelfScope")->text().contains("2 channels"));
    dialog->findChild<QLineEdit*>("shelfUsername")->setText("ui_tester");
    dialog->findChild<QLineEdit*>("shelfReason")->setText("Right-click group");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (int i : {2, 3}) QVERIFY(engine.state(channels[i]).shelf.until > engine.now());
    QCOMPARE(engine.state(channels[0]).shelf.until, qint64(0));
    menu = openMenu(tree, tree->model()->index(0, 2)); QVERIFY(menu);
    QPointer<QAction> stale = menu->findChild<QAction*>("shelveContextTarget");
    QPointer<QAction> staleProperties = menu->findChild<QAction*>("propertiesContextTarget");
    static_cast<AlarmModel*>(tree->model())->reset(&w->document(), &engine);
    if (stale) stale->trigger();
    if (staleProperties) staleProperties->trigger();
    QVERIFY(!w->findChild<QDialog*>("shelveDialog"));
    QVERIFY(!w->findChild<QDialog*>("propertiesDialog"));
    auto editor = std::make_unique<Window>(sample(), options(true), false);
    QVERIFY(editor->findChild<QTreeView*>("alarmTree")->contextMenuPolicy() != Qt::CustomContextMenu);
  }

  void shelvingCustomUnits_data() {
    QTest::addColumn<QString>("choice");
    QTest::addColumn<int>("amount");
    QTest::addColumn<int>("minutes");
    QTest::newRow("minutes") << "Custom minutes" << 90 << 90;
    QTest::newRow("hours") << "Custom hours" << 36 << 2160;
    QTest::newRow("days") << "Custom days" << 2 << 2880;
    QTest::newRow("maximum-days") << "Custom days" << 365 << 525600;
  }
  void shelvingCustomUnits() {
    QFETCH(QString, choice); QFETCH(int, amount); QFETCH(int, minutes);
    auto w = std::make_unique<Window>(sample(), options(false), false);
    auto& engine = w->alarmEngine();
    const qint64 now = 1700000000000LL; engine.now = engine.monotonicNow = [=] { return now; };
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Shelve Alarms...") action->trigger();
    auto dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    auto duration = dialog->findChild<QComboBox*>("shelfDuration");
    auto custom = dialog->findChild<QSpinBox*>("shelfCustomMinutes");
    duration->setCurrentText(choice);
    QVERIFY(custom->isEnabled());
    custom->setValue(amount);
    if (choice == "Custom hours") {
      duration->setCurrentText("Custom minutes"); QCOMPARE(custom->value(), minutes);
      duration->setCurrentText(choice); QCOMPARE(custom->value(), amount);
    }
    dialog->findChild<QLineEdit*>("shelfUsername")->setText("ui_tester");
    dialog->findChild<QLineEdit*>("shelfReason")->setText("Custom duration");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    for (auto n : w->document().channels())
      QCOMPARE(engine.state(n).shelf.until, now + qint64(minutes) * 60000);
    auto shelfList = [&] {
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == "Shelved Alarms...") action->trigger();
      return w->findChild<QDialog*>("shelvedAlarmsDialog");
    }();
    QVERIFY(shelfList);
    auto table = shelfList->findChild<QTableWidget*>("shelvedAlarmsTable");
    QCOMPARE(table->rowCount(), w->document().channels().size());
  }

  void shelvingWorkflow() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->show();
    auto& e = w->alarmEngine();
    const qint64 startTime = QDateTime(QDate(2026, 9, 11), QTime(12, 0)).toMSecsSinceEpoch();
    qint64 time = startTime; e.now = e.monotonicNow = [&] { return time; };
    const auto channels = w->document().channels();
    for (auto n : channels) e.event(n, {3, 2, 2, 1, "99"});
    e.shelve(channels[0], 15, "Individual maintenance", "tester");
    QAction *shelve = nullptr, *list = nullptr;
    for (auto a : w->findChildren<QAction*>()) {
      if (a->text() == "Shelve Alarms...") shelve = a;
      if (a->text() == "Shelved Alarms...") list = a;
    }
    QVERIFY(shelve && list); shelve->trigger();
    auto dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    auto reason = dialog->findChild<QLineEdit*>("shelfReason");
    auto username = dialog->findChild<QLineEdit*>("shelfUsername");
    QVERIFY(username); QVERIFY(username->text().isEmpty());
    auto duration = dialog->findChild<QComboBox*>("shelfDuration");
    auto custom = dialog->findChild<QSpinBox*>("shelfCustomMinutes");
    auto apply = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    QVERIFY(!apply->isEnabled()); QCOMPARE(duration->currentData().toInt(), 60);
    QVERIFY(dialog->findChild<QLabel*>("shelfScope")->text().contains("2 channels"));
    reason->setText("  "); QVERIFY(!apply->isEnabled());
    reason->setText("Vacuum maintenance"); QVERIFY(!apply->isEnabled());
    username->setText("  "); QVERIFY(!apply->isEnabled());
    username->setText("two names"); QVERIFY(!apply->isEnabled());
    username->setText("  operator_one  "); QVERIFY(apply->isEnabled());
    duration->setCurrentIndex(5); QVERIFY(custom->isEnabled()); custom->setValue(2);
    auto tree = w->findChild<QTreeView*>("alarmTree");
    tree->setCurrentIndex(tree->model()->index(0, 0, tree->model()->index(0, 0)));
    QCOMPARE(dialog->findChild<QLabel*>("shelfTarget")->text(), QString("/BOOSTER"));
    const auto name = legacyAppearance() ? "/classic" : "/fusion";
    QDir().mkpath(QString(TEST_OUTPUT));
    QVERIFY(dialog->grab().save(QString(TEST_OUTPUT) + name + "-shelve-dialog.png"));
    apply->click(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(e.presentation(w->document().root.get()).shelved, 3);
    QCOMPARE(e.state(channels[0]).shelf.until, startTime + 900000);
    QCOMPARE(e.state(channels[1]).shelf.until, startTime + 120000);
    QVERIFY(w->findChild<QLabel*>("shelvedCount")->text().contains("Shelved: 3"));
    QVERIFY(w->grab().save(QString(TEST_OUTPUT) + name + "-shelved-main.png"));
    list->trigger();
    auto browser = w->findChild<QDialog*>("shelvedAlarmsDialog"); QVERIFY(browser);
    auto table = browser->findChild<QTableWidget*>("shelvedAlarmsTable");
    QCOMPARE(table->rowCount(), 3);
    QCOMPARE(table->item(0, 0)->text(), QString("/BOOSTER/Power_Supplies/test:power"));
    QCOMPARE(table->item(0, 5)->text(), QString("Individual maintenance"));
    QCOMPARE(table->item(0, 7)->text(), QString("tester"));
    QCOMPARE(e.state(channels[1]).shelf.username, QString("operator_one"));
    QVERIFY(browser->grab().save(QString(TEST_OUTPUT) + name + "-shelved-list.png"));
    table->selectRow(0); browser->findChild<QPushButton*>("changeShelf")->click();
    dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit*>("shelfReason")->text(), QString("Individual maintenance"));
    QVERIFY(dialog->findChild<QLineEdit*>("shelfUsername")->text().isEmpty());
    QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
    dialog->findChild<QLineEdit*>("shelfUsername")->setText("ui_tester");
    dialog->findChild<QLineEdit*>("shelfReason")->setText("Extended maintenance");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(e.state(channels[0]).shelf.until, startTime + 3600000);
    QCOMPARE(e.state(channels[0]).shelf.username, QString("ui_tester"));
    const auto remaining = table->item(0, 4)->text();
    time += 1000;
    QTRY_VERIFY(table->item(0, 4)->text() != remaining);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
    browser->findChild<QPushButton*>("unshelveSelected")->click();
    QCOMPARE(table->rowCount(), 2); QCOMPARE(e.state(channels[0]).shelf.until, qint64(0));
    time = startTime + 120000; e.tick();
    QTRY_COMPARE(table->rowCount(), 0);
    QCOMPARE(e.presentation(w->document().root.get()).unack, 2);
    QVERIFY(!w->findChild<QLabel*>("shelvedCount")->isVisible());
  }
  void shelvingFiltersAndIndicators() {
    auto d = sample(); Engine e(d); qint64 time = 1000; e.now = e.monotonicNow = [&] { return time; };
    for (auto n : d.channels()) e.event(n, {});
    auto n = d.channels()[0]; e.event(n, {3, 2, 2, 1, "99"});
    AlarmModel tree(&d, &e, true), group(&d, &e, false); group.setGroup(n->parent);
    e.shelve(n, 1, "check filters", "tester");
    for (int filter : {1, 2}) {
      tree.setFilter(filter); group.setFilter(filter);
      QCOMPARE(tree.rowCount(), 0); QCOMPARE(group.rowCount(), 0);
    }
    tree.setFilter(0); group.setFilter(0);
    QCOMPARE(tree.index(0, 1).data().toString(), QString(" "));
    QVERIFY(tree.index(0, 2).data().toString().contains("1 shelved"));
    QVERIFY(group.index(0, 2).data().toString().contains("[Shelved]"));
    QVERIFY(group.index(0, 2).data(Qt::ToolTipRole).toString().contains("check filters"));
    QCOMPARE(group.index(0, 6).data().toString(), QString("<----->"));
    e.event(n, {}); time += 60000; e.tick();
    for (int filter : {1, 2}) {
      tree.setFilter(filter); group.setFilter(filter);
      QCOMPARE(tree.rowCount(), 1); QCOMPARE(group.rowCount(), 1);
    }
  }
  void shelvingReloadAndSave() {
    QTemporaryDir dir;
    auto o = options(false); o.broadcast = true; o.configDir = dir.path();
    o.config = dir.filePath("config");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root one\nCHANNEL root two\n");
    saveConfig(d, o.config); d.filename = o.config;
    auto w = std::make_unique<Window>(d, o, false);
    auto old = w->document().channels()[0];
    w->alarmEngine().event(old, {3, 2, 2, 1, "99"}); w->alarmEngine().event(old, {});
    w->alarmEngine().shelve(old, 60, "Preserve across reload", "tester");
    const auto deadline = w->alarmEngine().state(old).shelf.until;
    const auto copy = dir.filePath("saved"); w->saveTo(copy);
    QCOMPARE(w->alarmEngine().state(old).shelf.until, deadline);
    QCOMPARE(writeConfig(loadConfig(copy)), writeConfig(d));
    for (auto a : w->findChildren<QAction*>()) if (a->text() == "Shelve Alarms...") a->trigger();
    QPointer<QDialog> pending = w->findChild<QDialog*>("shelveDialog"); QVERIFY(pending);
    saveConfig(parseConfig("GROUP NULL root\nCHANNEL root two\nCHANNEL root added\nCHANNEL root one\n"), o.config);
    Logging sender(o, "root"); QVERIFY(sender.sendBroadcast("reload", 0, true));
    QTRY_COMPARE_WITH_TIMEOUT(w->document().channels().size(), 3, 5000);
    QVERIFY(!pending || !pending->isEnabled());
    auto n = w->document().channels()[2]; QCOMPARE(n->name, QString("one"));
    QCOMPARE(w->alarmEngine().state(n).shelf.until, deadline);
    QCOMPARE(w->alarmEngine().state(n).shelf.reason, QString("Preserve across reload"));
    QCOMPARE(w->alarmEngine().state(n).shelf.username, QString("tester"));
    w->alarmEngine().event(n, {});
    QCOMPARE(w->alarmEngine().state(n).unack, 2);
    QCOMPARE(w->alarmEngine().state(w->document().channels()[1]).shelf.until, qint64(0));
    auto fresh = std::make_unique<Window>(loadConfig(o.config), o, false);
    QCOMPARE(fresh->alarmEngine().presentation(fresh->document().root.get()).shelved, 0);
    QCOMPARE(w->alarmEngine().presentation(w->document().root.get()).shelved, 1);
  }

  void reloadPreservesSilence_data() {
    QTest::addColumn<bool>("startupSilent");
    QTest::newRow("operator-enabled-sound") << true;
    QTest::newRow("operator-muted-sound") << false;
  }
  void reloadPreservesSilence() {
    QFETCH(bool, startupSilent);
    QTemporaryDir dir;
    auto o = options(false);
    o.silent = startupSilent;
    o.broadcast = true;
    o.configDir = dir.path();
    o.config = dir.filePath("config");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root before\n");
    saveConfig(d, o.config);
    d.filename = o.config;
    auto w = std::make_unique<Window>(d, o, false);
    QAction* action = nullptr;
    for (auto a : w->findChildren<QAction*>())
      if (a->text() == "Silence Forever")
        action = a;
    QVERIFY(action);
    action->setChecked(!startupSilent);
    saveConfig(parseConfig("GROUP NULL root\nCHANNEL root after\n"), o.config);
    Logging sender(o, "root");
    QVERIFY(sender.sendBroadcast("reload", 0, true));
    QTRY_COMPARE_WITH_TIMEOUT(w->document().channels()[0]->name, QString("after"), 5000);
    QCOMPARE(w->alarmEngine().silenceForever, !startupSilent);
    QCOMPARE(action->isChecked(), !startupSilent);
    w->alarmEngine().event(w->document().channels()[0], {3, 2, 2, 1, "99"});
    QCOMPARE(w->alarmEngine().audible(), startupSilent);
  }
  void runtimeSaveAsPreservesFacility() {
    QTemporaryDir dir;
    auto o = options(false);
    o.broadcast = true;
    o.configDir = dir.path();
    o.config = dir.filePath("original");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root initial\n");
    saveConfig(d, o.config);
    d.filename = o.config;
    auto w = std::make_unique<Window>(d, o, false);
    w->alarmEngine().setMask(w->document().channels()[0], Mask::parse("-D---"));
    auto copy = dir.filePath("snapshot");
    w->saveTo(copy);
    QCOMPARE(w->document().filename, o.config);
    QVERIFY(loadConfig(copy).channels()[0]->mask[Disable]);
    QVERIFY(!loadConfig(o.config).channels()[0]->mask[Disable]);
    saveConfig(parseConfig("GROUP NULL root\nCHANNEL root original_update\n"), o.config);
    saveConfig(parseConfig("GROUP NULL root\nCHANNEL root snapshot_update\n"), copy);
    Logging sender(o, "root");
    QVERIFY(sender.sendBroadcast("reload", 0, true));
    QTRY_COMPARE_WITH_TIMEOUT(w->document().channels()[0]->name,
                              QString("original_update"), 5000);
    QCOMPARE(w->document().filename, o.config);
  }
  void rejectIncompleteCalcReload() {
    QTemporaryDir dir;
    auto o = options(false);
    o.broadcast = true;
    o.configDir = dir.path();
    o.config = dir.filePath("config");
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    saveConfig(d, o.config);
    d.filename = o.config;
    auto w = std::make_unique<Window>(d, o, false);
    auto n = w->document().channels()[0];
    w->alarmEngine().event(n, {3, 2, 2, 1, "99"});
    w->alarmEngine().shelve(n, 60, "Preserve after failed reload", "tester");
    const auto deadline = w->alarmEngine().state(n).shelf.until;
    QFile file(o.config);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("GROUP NULL replacement\n$FORCEPV CALC -D--- 1 NE\n");
    file.close();
    Logging sender(o, "root");
    QVERIFY(sender.sendBroadcast("reload", 0, true));
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
          for (auto label : w->findChildren<QLabel*>())
            if (label->text().contains("requires a FORCEPV_CALC expression"))
              return true;
          return false;
        }(),
        5000);
    QCOMPARE(w->document().root->name, QString("root"));
    QCOMPARE(w->document().channels()[0], n);
    QCOMPARE(w->alarmEngine().state(n).unack, 2);
    QCOMPARE(w->alarmEngine().state(n).shelf.until, deadline);
  }
  void heartbeatUsesIndependentTimer() {
    QTemporaryDir dir;
    const auto prefix = "qtalh_ui_heartbeat_" + QString::number(QCoreApplication::applicationPid()) + ":";
    const auto port = QByteArray::number(39000 + QCoreApplication::applicationPid() % 2000);
    struct Environment {
      QMap<QByteArray, QByteArray> previous;
      void set(const QByteArray& name, const QByteArray& value) { previous[name] = qgetenv(name.constData()); qputenv(name.constData(), value); }
      ~Environment() { for (auto it = previous.cbegin(); it != previous.cend(); ++it)
        if (it.value().isNull()) qunsetenv(it.key().constData()); else qputenv(it.key().constData(), it.value()); }
    } environment;
    environment.set("EPICS_CA_AUTO_ADDR_LIST", "NO");
    environment.set("EPICS_CA_ADDR_LIST", "127.0.0.1:" + port);
    environment.set("EPICS_CA_SERVER_PORT", port);
    environment.set("EPICS_CA_REPEATER_PORT", QByteArray::number(port.toInt() + 1));
    environment.set("EPICS_CAS_INTF_ADDR_LIST", "127.0.0.1");
    environment.set("EPICS_CAS_BEACON_ADDR_LIST", "127.0.0.1:" + QByteArray::number(port.toInt() + 1));
    QFile database(dir.filePath("heartbeat.db")); QVERIFY(database.open(QIODevice::WriteOnly));
    database.write(("record(ao,\"" + prefix + "beat\") { field(ASG,\"dynamic\") }\n"
        "record(ao,\"" + prefix + "permit\") { field(VAL,\"1\") }\n").toLatin1()); database.close();
    QFile access(dir.filePath("heartbeat.acf")); QVERIFY(access.open(QIODevice::WriteOnly));
    access.write(("ASG(DEFAULT) { RULE(1,READ) RULE(1,WRITE) }\nASG(dynamic) { INPA(\"" +
        prefix + "permit\") RULE(1,READ) RULE(1,WRITE) { CALC(\"A=1\") } }\n").toLatin1()); access.close();
    struct Ioc {
      QProcess process;
      ~Ioc() { process.kill(); process.waitForFinished(3000); }
    } ioc;
    auto o = options(false); o.engine.global = true;
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV " + prefix + "beat 0.1 1\n");
    auto w = std::make_unique<Window>(d, o, true);
    auto timer = w->findChild<QTimer*>("heartbeatTimer"); QVERIFY(timer);
    QVERIFY(!timer->isActive()); QCOMPARE(timer->timerType(), Qt::PreciseTimer);
    QSignalSpy beats(timer, &QTimer::timeout);
    QTest::qWait(300); QCOMPARE(beats.size(), 0);
    ioc.process.start(QString(EPICS_TEST_BIN) + "/softIoc", {"-d", database.fileName(), "-a", access.fileName()});
    QVERIFY(ioc.process.waitForStarted());
    ChannelAccess driver; driver.prepare(prefix + "permit"); driver.prepare(prefix + "beat");
    QTRY_VERIFY_WITH_TIMEOUT(timer->isActive() && driver.canWrite(prefix + "permit"), 10000);
    beats.clear(); QTest::qWait(1150);
    QVERIFY2(beats.size() >= 9, qPrintable(QString("Only %1 heartbeat deadlines serviced").arg(beats.size())));
    QVERIFY(driver.put(prefix + "permit", 0));
    QTRY_VERIFY(!driver.canWrite(prefix + "beat")); QTRY_VERIFY(!timer->isActive());
    beats.clear(); QTest::qWait(350); QCOMPARE(beats.size(), 0);
    QVERIFY(driver.put(prefix + "permit", 1)); QTRY_VERIFY(timer->isActive());
    beats.clear(); QTest::qWait(450); QVERIFY(beats.size() >= 3);
    ioc.process.kill(); QVERIFY(ioc.process.waitForFinished(3000));
    QTRY_VERIFY(!timer->isActive());
  }
  void initialGeometry_data() {
    QTest::addColumn<int>("mode"); QTest::addColumn<QString>("geometry");
    for (int mode : {0, 1, 2})
      for (auto geometry : {"+100+120", "-20-30", "-0-0", "1200x800+50+60", "=1200x800-0-0"})
        QTest::newRow(qPrintable(QString::number(mode) + ':' + geometry)) << mode << QString(geometry);
  }
  void initialGeometry() {
    QFETCH(int, mode); QFETCH(QString, geometry);
    auto o = options(mode == 2); o.mainWindow = mode == 1; o.geometry = geometry;
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\n"), o, false);
    w->showInitial();
    QWidget* target = w.get();
    if (mode == 0) {
      target = nullptr;
      for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "runtimeWindow" && widget->isVisible()) target = widget;
    }
    QVERIFY(target); QVERIFY(target->isVisible());
    if (geometry.contains('x')) QCOMPARE(target->size(), QSize(1200, 800));
    auto screen = target->screen()->availableGeometry();
    auto frame = target->frameGeometry();
    if (geometry.endsWith("-0-0")) {
      QCOMPARE(frame.right(), screen.right()); QCOMPARE(frame.bottom(), screen.bottom());
    } else if (geometry == "-20-30") {
      QCOMPARE(frame.right(), screen.right() - 20); QCOMPARE(frame.bottom(), screen.bottom() - 30);
    } else {
      QCOMPARE(frame.topLeft(), screen.topLeft() + (geometry.contains('x') ? QPoint(50, 60) : QPoint(100, 120)));
    }
  }
  void groupBeepIndicators() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root peer\nGROUP root branch\n"
                         "CHANNEL branch leaf\n$BEEPSEVR ERROR\n");
    Engine e(d);
    AlarmModel tree(&d, &e, true), contents(&d, &e, false);
    auto root = d.root.get(), branch = root->children[1].get();
    auto peer = d.channels()[0], leaf = d.channels()[1];
    const auto rootIndex = tree.index(0, 0);
    auto indicator = [&](Node* n) {
      if (n == root) return tree.index(0, 7).data().toString();
      return tree.index(0, 7, rootIndex).data().toString();
    };
    QCOMPARE(indicator(root), QString("E"));
    QCOMPARE(indicator(branch), QString("E"));
    QCOMPARE(contents.index(0, 7).data().toString(), QString("E"));
    QCOMPARE(e.state(root).beepThreshold, 1); // Summary must not change audio thresholds.
    e.setBeep(leaf, 1);
    QCOMPARE(indicator(root), QString()); QCOMPARE(indicator(branch), QString());
    e.setBeep(peer, 2);
    QCOMPARE(indicator(root), QString("R")); QCOMPARE(indicator(branch), QString());
    e.setBeep(branch, 3);
    QCOMPARE(indicator(root), QString("V")); QCOMPARE(indicator(branch), QString("V"));
    e.setBeep(leaf, 4);
    QCOMPARE(indicator(root), QString("E")); QCOMPARE(indicator(branch), QString("E"));
    e.shelve(leaf, 1, "maintenance", "tester");
    QCOMPARE(indicator(branch), QString("E")); // The configured restriction still exists.
    e.setBeep(leaf, 1);
    QCOMPARE(indicator(root), QString("V"));
    e.setBeep(branch, 1);
    QCOMPARE(indicator(root), QString("R"));
    e.setBeep(peer, 1);
    QCOMPARE(indicator(root), QString()); QCOMPARE(indicator(branch), QString());
  }
  void runtimeModels() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->show();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    QVERIFY(tree);
    QVERIFY(group);
    QCOMPARE(tree->model()->rowCount(), 1);
    QCOMPARE(group->model()->rowCount(), 3);
    auto channels = w->document().channels();
    for (auto n : channels)
      w->alarmEngine().event(n, {0, 0, 0, 1, "0"});
    w->alarmEngine().event(channels.last(), {3, 2, 2, 1, "99"});
    QTest::qWait(250);
    QCOMPARE(tree->model()->index(0, 1).data().toString(), QString("R"));
    QCOMPARE(tree->model()->index(0, 8).data().toString(), QString("(0,0,1,0,2)"));
    QDir().mkpath(QString(TEST_OUTPUT));
    QVERIFY(w->grab().save(QString(TEST_OUTPUT) + (legacyAppearance() ? "/qtalh-main.png" : "/fusion-main.png")));
    auto index = tree->model()->index(0, 0);
    tree->setCurrentIndex(index);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(index).center());
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).unack, 0);
  }
  void actionTooltips_data() {
    QTest::addColumn<QString>("directives");
    QTest::addColumn<int>("column");
    QTest::addColumn<QString>("expected");
    QTest::newRow("guidance-text")
        << "$GUIDANCE\nCheck <pressure> & temperature.\nKeep  two spaces.\n$END\n" << 4
        << "Check <pressure> & temperature.\nKeep  two spaces.";
    QTest::newRow("guidance-url") << "$GUIDANCE https://example.invalid/operator-help\n" << 4
        << "https://example.invalid/operator-help";
    QTest::newRow("guidance-both")
        << "$GUIDANCE https://example.invalid/help\n$GUIDANCE\nCheck the source.\n$END\n" << 4
        << "Check the source.\nhttps://example.invalid/help";
    QTest::newRow("command-literal") << "$COMMAND echo '<b>alarm</b>' > output.txt\n" << 5
        << "echo '<b>alarm</b>' > output.txt";
    QTest::newRow("command-display")
        << "$COMMAND '/legacy tools/medm' -x -macro \"P=ring\" panel.adl\n" << 5
        << "qtedm -x -macro \"P=ring\" panel.adl";
    QTest::newRow("command-menu")
        << "$COMMAND !First!echo one!Second!MASTER_ONLY medm second.adl!\n" << 5
        << "First:\necho one\nSecond:\nMaster only:\nqtedm second.adl";
  }
  void actionTooltips() {
    QFETCH(QString, directives);
    QFETCH(int, column);
    QFETCH(QString, expected);
    auto d = parseConfig("GROUP NULL root\n" + directives + "CHANNEL root pv\n" + directives);
    auto w = std::make_unique<Window>(std::move(d), options(false), false);
    QStringList commands;
    w->alarmEngine().command = [&](const QString& command) { commands << command; };
    w->show();
    QCoreApplication::processEvents();
    for (const auto& name : {"alarmTree", "groupContents"}) {
      auto view = w->findChild<QTreeView*>(name);
      auto index = view->model()->index(0, column);
      auto position = view->visualRect(index).center();
      QCOMPARE(view->indexAt(position), index);
      QHelpEvent event(QEvent::ToolTip, position, view->viewport()->mapToGlobal(position));
      QApplication::sendEvent(view->viewport(), &event);
      QTextDocument tooltip;
      tooltip.setHtml(QToolTip::text());
      // Plain-text extraction represents paragraph spacing with one newline.
      QCOMPARE(tooltip.toPlainText(), expected);
      QVERIFY(commands.isEmpty());
      QVERIFY(w->findChildren<QDialog*>().isEmpty());
      QToolTip::hideText();
    }
  }
  void footerSpacing() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->show();
    QCoreApplication::processEvents();
    QList<int> centers;
    for (const auto& name :
         {"silenceInterval", "silenceCurrent", "silenceForever", "beepSeverity"}) {
      auto row = w->findChild<QWidget*>(name);
      QVERIFY(row);
      if (legacyAppearance()) QCOMPARE(row->height(), 20);
      else QVERIFY(row->height() >= row->fontMetrics().height());
      centers.append(row->mapTo(w.get(), row->rect().center()).y());
    }
    if (legacyAppearance()) {
      QCOMPARE(centers[1] - centers[0], centers[2] - centers[1]);
      QCOMPARE(centers[2] - centers[1], centers[3] - centers[2]);
    }
    QVERIFY(w->findChild<QLabel*>("silenceForever")->text().startsWith("Silence Forever: "));
    QVERIFY(w->findChild<QLabel*>("beepSeverity")->text().startsWith("ALH Beep Severity: "));
  }
  void exitConfirmation_data() {
    QTest::addColumn<QString>("entry");
    QTest::newRow("menu-exit") << "Exit";
    QTest::newRow("menu-close") << "Close";
    QTest::newRow("runtime-close") << "runtime";
  }
  void exitConfirmation() {
    QFETCH(QString, entry);
    QTemporaryDir dir; auto o = options(false); o.noLog = false;
    o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
    auto records = [&] { QFile f(o.opmodFile); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    auto w = std::make_unique<Window>(sample(), o, false);
    w->setAttribute(Qt::WA_DeleteOnClose, false);
    w->showInitial();
    QWidget* runtime = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "runtimeWindow")
        runtime = widget;
    QVERIFY(runtime);
    // Closing the main display alone still leaves the compact runtime available.
    w->close();
    QVERIFY(!w->isVisible());
    QVERIFY(runtime->isVisible());
    if (entry != "runtime")
      w->show();
    auto requestExit = [&] {
      if (entry == "runtime") {
        runtime->close();
      } else {
        for (auto action : w->findChildren<QAction*>())
          if (QString(action->text()).remove('&') == entry)
            action->trigger();
      }
    };
    auto confirmation = [&]() -> QMessageBox* {
      for (auto widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "exitConfirmation" && widget->isVisible())
          return qobject_cast<QMessageBox*>(widget);
      return nullptr;
    };
    requestExit();
    auto dialog = confirmation();
    QVERIFY(dialog);
    QCOMPARE(dialog->text(), QString("Exit Alarm Handler?"));
    QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
    requestExit();
    QCOMPARE(confirmation(), dialog);
    QTest::mouseClick(dialog->button(QMessageBox::Cancel), Qt::LeftButton);
    QVERIFY(runtime->isVisible());
    QVERIFY(!records().contains("Setup---Exit"));
    requestExit();
    dialog = confirmation();
    QVERIFY(dialog);
    QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
    QVERIFY(!runtime->isVisible());
    QVERIFY(!w->isVisible());
    QCOMPARE(records().count("Setup---Exit"), 1);
  }
  void pendingBroadcastDefersExit() {
    QTemporaryDir dir;
    auto o = options(false); o.broadcast = true; o.config = dir.filePath("config");
    o.noLog = false; o.alarmFile = dir.filePath("alarm"); o.opmodFile = dir.filePath("ops");
    auto w = std::make_unique<Window>(sample(), o, false);
    w->setAttribute(Qt::WA_DeleteOnClose, false); w->showInitial();
    QWidget* runtime = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "runtimeWindow") runtime = widget;
    QVERIFY(runtime);
    for (auto action : w->findChildren<QAction*>())
      if (QString(action->text()).remove('&') == "Send Message...") action->trigger();
    auto broadcast = w->findChild<QDialog*>("broadcastDialog"); QVERIFY(broadcast);
    auto message = broadcast->findChild<QLineEdit*>("broadcastMessage"); QVERIFY(message);
    message->setText("deliver before exit");
    auto buttons = broadcast->findChild<QDialogButtonBox*>(); QVERIFY(buttons);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    QVERIFY(QFileInfo(o.config + ".MESS").size() > 0);
    for (auto action : w->findChildren<QAction*>())
      if (QString(action->text()).remove('&') == "Exit") action->trigger();
    auto confirmation = w->findChild<QMessageBox*>("exitConfirmation"); QVERIFY(confirmation);
    QTest::mouseClick(confirmation->button(QMessageBox::Ok), Qt::LeftButton);
    QVERIFY(w->isVisible()); QVERIFY(runtime->isVisible());
    QVERIFY(QFileInfo(o.config + ".MESS").size() > 0);
    QFile audit(o.opmodFile); QVERIFY(audit.open(QIODevice::ReadOnly));
    QVERIFY(!audit.readAll().contains("Setup---Exit"));
  }
  void failedAlarmSoundFallsBack() {
    QTemporaryDir dir;
    auto o = options(false); o.silent = false;
    o.sound = dir.filePath("missing-alarm.wav");
    auto w = std::make_unique<BellTestWindow>(
        parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    auto player = w->findChild<QMediaPlayer*>("alarmSound"); QVERIFY(player);
    auto timer = w->findChild<QTimer*>("alarmBeepTimer"); QVERIFY(timer);
    timer->stop(); // Drive the real alarm timer callback without making noise.
    QTRY_VERIFY_WITH_TIMEOUT(player->error() != QMediaPlayer::NoError, 10000);
    auto& engine = w->alarmEngine(); auto n = w->document().channels()[0];
    engine.event(n, {3, 2, 2, 1, "alarm"});
    auto tick = [&] { QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection)); };
    tick(); tick(); QCOMPARE(w->bells, 2);
    engine.setSilenceCurrent(true); tick(); QCOMPARE(w->bells, 2);
    engine.setSilenceCurrent(false); engine.setSilenceForever(true);
    tick(); QCOMPARE(w->bells, 2);
    engine.setSilenceForever(false);
    engine.setSilenceUntil(engine.monotonicNow() + 60000);
    tick(); QCOMPARE(w->bells, 2);
    engine.setSilenceUntil(0); tick(); QCOMPARE(w->bells, 3);
    engine.acknowledge(n); tick(); QCOMPARE(w->bells, 3);
  }
  void oggAlarmSound() {
#ifdef Q_OS_MACOS
    // Native media backends need the Cocoa event loop; the offscreen plugin
    // cannot drive playback reliably. Run just this case in a native child.
    if (QGuiApplication::platformName() != "cocoa") {
      QProcess child;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert("QT_QPA_PLATFORM", "cocoa");
      child.setProcessEnvironment(environment);
      child.start(QCoreApplication::applicationFilePath(), {"oggAlarmSound"});
      QVERIFY(child.waitForStarted());
      if (!child.waitForFinished(45000)) {
        child.kill();
        child.waitForFinished();
        QFAIL("Native macOS alarm playback test timed out");
      }
      const auto output = child.readAllStandardOutput() + child.readAllStandardError();
      QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
               output.constData());
      return;
    }
#endif
    const QString file = QFINDTESTDATA("alarm.ogg");
    QVERIFY2(!file.isEmpty(), "The bundled Ogg alarm fixture must be available");
    auto o = options(false);
    o.sound = file;
    auto w = std::make_unique<Window>(sample(), o, false);
    auto player = w->findChild<QMediaPlayer*>("alarmSound");
    QVERIFY(player);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    player->audioOutput()->setMuted(true);
#else
    player->setMuted(true);
#endif
    QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus() == QMediaPlayer::LoadedMedia ||
                                 player->error() != QMediaPlayer::NoError,
                             10000);
    QVERIFY2(player->error() == QMediaPlayer::NoError, qPrintable(player->errorString()));
    // Decode to completion twice: repeating alarms must restart after the first beep.
    for (int repeat = 0; repeat < 2; ++repeat) {
      player->setPosition(0);
      player->play();
      QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus() == QMediaPlayer::EndOfMedia ||
                                   player->error() != QMediaPlayer::NoError,
                               10000);
      QVERIFY2(player->error() == QMediaPlayer::NoError, qPrintable(player->errorString()));
      QVERIFY(player->duration() > 0);
    }
  }
  void rejectAmbiguousGroupInsertion() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\n"), options(true), false);
    const auto original = writeConfig(w->document());
    QVERIFY_THROWS_EXCEPTION(ParseError, w->addNode(true, "root"));
    QCOMPARE(writeConfig(w->document()), original);
    w->addNode(true, "first");
    w->addNode(true, "second");
    QCOMPARE(w->document().root->children.size(), size_t(2));
    for (const auto& child : w->document().root->children)
      QCOMPARE(child->parent, w->document().root.get());
    w->undoEdit();
    QCOMPARE(w->document().root->children.size(), size_t(1));
    w->redoEdit();
    QCOMPARE(w->document().root->children.size(), size_t(2));
  }
  void editorUndoSave() {
    auto w = std::make_unique<Window>(sample(), options(true), false);
    w->show();
    int count = w->document().channels().size();
    w->addNode(false, "new:pv");
    QCOMPARE(w->document().channels().size(), count + 1);
    w->undoEdit();
    QCOMPARE(w->document().channels().size(), count);
    w->redoEdit();
    QCOMPARE(w->document().channels().size(), count + 1);
    QTemporaryDir dir;
    w->saveTo(dir.filePath("saved.alhConfig"));
    QCOMPARE(w->document().filename, dir.filePath("saved.alhConfig"));
    QCOMPARE(loadConfig(dir.filePath("saved.alhConfig")).channels().size(), count + 1);
    QVERIFY(w->grab().save(QString(TEST_OUTPUT) + (legacyAppearance() ? "/qtalh-editor.png" : "/fusion-editor.png")));
  }
  void editorBeepSeverityUndoAndSave() {
    auto w = std::make_unique<Window>(sample(), options(true), false);
    QTemporaryDir dir;
    auto path = dir.filePath("beep.alhConfig");
    w->saveTo(path);
    auto tree = w->findChild<QTreeView*>("alarmTree");
    QVERIFY(tree);
    auto setSeverity = [&](const QString& name) {
      auto index = tree->model()->index(0, 7);
      if (!QMetaObject::invokeMethod(tree, "clicked", Qt::DirectConnection,
                                     Q_ARG(QModelIndex, index))) return false;
      auto dialog = w->findChild<QDialog*>("beepSeverityDialog");
      if (!dialog) return false;
      auto radio = dialog->findChild<QRadioButton*>("beepSeverity" + QString::number(severityValue(name)));
      if (!radio) return false;
      radio->click(); dialog->close();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      return true;
    };
    auto modified = [&] {
      for (auto label : w->findChildren<QLabel*>())
        if (label->text().startsWith("Filename:"))
          return label->text().endsWith(" *");
      return false;
    };
    QVERIFY(setSeverity("MAJOR"));
    QVERIFY(modified());
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).beepThreshold, 2);
    w->undoEdit();
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).beepThreshold, 1);
    w->redoEdit();
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).beepThreshold, 2);
    w->saveTo(path);
    QCOMPARE(loadConfig(path).root->option("BEEPSEVR"), QString("MAJOR"));
    QVERIFY(!modified());
    QVERIFY(setSeverity("MAJOR"));
    QVERIFY(!modified()); // Accepting an unchanged value is not an edit.
    w->undoEdit();
    QVERIFY(setSeverity("INVALID"));
    w->redoEdit(); // A new beep edit must discard the old redo branch.
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).beepThreshold, 3);
  }
  void editorSelectionModelsStayBounded() {
    auto w = std::make_unique<Window>(sample(), options(true), false);
    const auto count = w->findChildren<QItemSelectionModel*>().size();
    for (int i = 0; i < 10; ++i) {
      w->addNode(false, "new:" + QString::number(i));
      w->undoEdit();
      w->redoEdit();
      QCoreApplication::processEvents();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QCOMPARE(w->findChildren<QItemSelectionModel*>().size(), count);
    }
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    auto child = tree->model()->index(0, 0, tree->model()->index(0, 0));
    tree->setCurrentIndex(child);
    QCOMPARE(group->model()->rowCount(), 1);
    QCOMPARE(group->model()->index(0, 2).data().toString(), QString("test:power"));
  }
  void copyBetweenWindows() {
    auto from = std::make_unique<Window>(sample(), options(true), false);
    auto to = std::make_unique<Window>(parseConfig("GROUP NULL destination"), options(true), false);
    auto action = [](Window* window, const QString& name) {
      for (auto item : window->findChildren<QAction*>())
        if (QString(item->text()).remove('&') == name)
          return item;
      return static_cast<QAction*>(nullptr);
    };
    QVERIFY(action(from.get(), "Copy"));
    action(from.get(), "Copy")->trigger();
    from.reset();
    QVERIFY(action(to.get(), "Paste"));
    action(to.get(), "Paste")->trigger();
    QCOMPARE(to->document().channels().size(), 3);
    to->undoEdit();
    QCOMPARE(to->document().channels().size(), 0);
  }
  void cutClipboardNamedGroupBetweenWindows() {
    auto from = std::make_unique<Window>(parseConfig(
        "GROUP NULL root\nGROUP root __QTALH_CLIPBOARD__\n"
        "GROUP __QTALH_CLIPBOARD__ __QTALH_CLIPBOARD__1\n"
        "CHANNEL __QTALH_CLIPBOARD__1 pv\n"), options(true), false);
    auto to = std::make_unique<Window>(parseConfig("GROUP NULL destination"), options(true), false);
    const auto original = writeConfig(from->document());
    auto group = from->findChild<QTreeView*>("groupContents"); QVERIFY(group);
    group->setCurrentIndex(group->model()->index(0, 2));
    auto cut = from->findChild<QAction*>("Cut"); QVERIFY(cut); cut->trigger();
    QCOMPARE(from->document().channels().size(), 0);
    from->undoEdit(); QCOMPARE(writeConfig(from->document()), original);
    from->redoEdit(); QCOMPARE(from->document().channels().size(), 0);
    from.reset(); // Paste must work using only the serialized system clipboard.
    auto paste = to->findChild<QAction*>("Paste"); QVERIFY(paste); paste->trigger();
    QCOMPARE(to->document().channels().size(), 1);
    QCOMPARE(to->document().channels()[0]->name, QString("pv"));
    QCOMPARE(to->document().root->children[0]->name, QString("__QTALH_CLIPBOARD__"));
    QCOMPARE(to->document().channels()[0]->parent->name, QString("__QTALH_CLIPBOARD__1"));
    to->undoEdit(); QCOMPARE(to->document().channels().size(), 0);
    to->redoEdit(); QCOMPARE(to->document().channels().size(), 1);
  }
  void dialogs() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->show();
    QStringList names = {"Properties Window", "Display Guidance", "Current Alarm History",
                         "Modify Mask Settings...", "Force Process Variable...", "Force Mask...", "Beep Severity..."};
    for (auto name : names) {
      QAction* action = nullptr;
      for (auto a : w->findChildren<QAction*>())
        if (a->text().remove('&') == name)
          action = a;
      QVERIFY2(action, qPrintable(name));
      action->trigger();
      QCoreApplication::processEvents();
      bool found = false;
      for (auto dialog : w->findChildren<QDialog*>())
        if (dialog->isVisible()) {
          found = true;
          dialog->close();
        }
      QVERIFY2(found, qPrintable(name));
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
  }
  void motifFileSelection() {
    if (!legacyAppearance()) {
      QTemporaryDir dir;
      QFile file(dir.filePath("existing.alhConfig"));
      QVERIFY(file.open(QIODevice::WriteOnly)); file.write("reference"); file.close();
      QTimer::singleShot(0, [&] {
        auto dialog = qobject_cast<QFileDialog*>(qApp->activeModalWidget());
        QVERIFY(dialog);
        QVERIFY(dialog->testOption(QFileDialog::DontUseNativeDialog));
        QCOMPARE(dialog->fileMode(), QFileDialog::ExistingFile);
        QCOMPARE(dialog->nameFilters(), QStringList{"Configurations (*.alhConfig)"});
        dialog->selectFile(file.fileName());
        QMetaObject::invokeMethod(dialog, "accept");
      });
      QCOMPARE(chooseFile(nullptr, "Open", dir.path(), "Configurations (*.alhConfig)"), file.fileName());
      QTimer::singleShot(0, [&] {
        auto dialog = qobject_cast<QFileDialog*>(qApp->activeModalWidget());
        QVERIFY(dialog); QCOMPARE(dialog->acceptMode(), QFileDialog::AcceptSave);
        dialog->selectFile(dir.filePath("new.alhConfig"));
        QMetaObject::invokeMethod(dialog, "accept");
      });
      QCOMPARE(chooseFile(nullptr, "Save", dir.path(), "Configurations (*.alhConfig)", true),
               dir.filePath("new.alhConfig"));
      QVERIFY(!QFileInfo::exists(dir.filePath("new.alhConfig")));
      for (bool overwrite : {false, true}) {
        QTimer::singleShot(0, [&] {
          auto dialog = qobject_cast<QFileDialog*>(qApp->activeModalWidget());
          QVERIFY(dialog); dialog->selectFile(file.fileName());
          QTimer::singleShot(0, [&] {
            auto confirmation = qobject_cast<QMessageBox*>(qApp->activeModalWidget());
            QVERIFY(confirmation);
            confirmation->button(overwrite ? QMessageBox::Yes : QMessageBox::No)->click();
          });
          QMetaObject::invokeMethod(dialog, "accept");
          if (!overwrite) { QVERIFY(dialog->isVisible()); dialog->reject(); }
        });
        QCOMPARE(chooseFile(nullptr, "Overwrite", dir.path(), "Configurations (*.alhConfig)", true),
                 overwrite ? file.fileName() : QString());
      }
      QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("reference")); file.close();
      QTimer::singleShot(0, [] { qobject_cast<QDialog*>(qApp->activeModalWidget())->reject(); });
      QVERIFY(chooseFile(nullptr, "Cancel", dir.path(), "All files (*)").isEmpty());
      return;
    }
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkdir("child"));
    QFile file(dir.filePath("child/example.alhConfig"));
    QVERIFY(file.open(QIODevice::WriteOnly)); file.write("reference"); file.close();
    QTimer::singleShot(0, [&] {
      auto dialog = qApp->activeModalWidget();
      QVERIFY(dialog);
      auto filter = dialog->findChild<QLineEdit*>("fileFilter");
      auto selection = dialog->findChild<QLineEdit*>("fileSelection");
      QVERIFY(filter); QVERIFY(selection);
      filter->setText(dir.filePath("child/*.alhConfig"));
      QMetaObject::invokeMethod(filter, "returnPressed");
      auto files = dialog->findChild<QListWidget*>("fileNames");
      QCOMPARE(files->count(), 1);
      files->setCurrentRow(0);
      QCOMPARE(selection->text(), file.fileName());
      dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    QCOMPARE(chooseFile(nullptr, "Open", dir.path(), "Configurations (*.alhConfig)"), file.fileName());
    QTimer::singleShot(0, [&] {
      auto dialog = qApp->activeModalWidget();
      dialog->findChild<QLineEdit*>("fileSelection")->setText("new.alhConfig");
      dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    QCOMPARE(chooseFile(nullptr, "Save", dir.path(), "Configurations (*.alhConfig)", true),
             dir.filePath("new.alhConfig"));
    QVERIFY(!QFileInfo::exists(dir.filePath("new.alhConfig"))); // Selection does not write.
    QTimer::singleShot(0, [&] {
      qApp->activeModalWidget()->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    });
    QVERIFY(chooseFile(nullptr, "Cancel", dir.path(), "All files (*)").isEmpty());
  }
  void forceDialogActions() {
    auto d = parseConfig("GROUP NULL root\n$FORCEPV gate -D--- 1 NE\nCHANNEL root pv\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Force Process Variable...") action->trigger();
    auto dialog = w->findChild<QDialog*>("forcePvDialog");
    QVERIFY(dialog);
    auto pv = dialog->findChild<QLineEdit*>("forcePvName");
    auto buttons = dialog->findChild<QDialogButtonBox*>();
    pv->setText("discarded");
    buttons->button(QDialogButtonBox::Cancel)->click();
    QVERIFY(dialog->isVisible());
    QCOMPARE(pv->text(), QString("gate"));
    pv->setText("new_gate");
    dialog->findChild<QCheckBox*>("maskBit4")->setChecked(true);
    buttons->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(w->document().root->option("FORCEPV"), QString("new_gate -D--L 1 NE"));
    pv->clear();
    buttons->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(pv->text(), QString("new_gate"));
    buttons->button(QDialogButtonBox::Close)->click();
  }
  void groupMaskActions() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root first -D---\nCHANNEL root second ----L\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    auto trigger = [&](const QString& name) {
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == name) action->trigger();
    };
    trigger("Modify Mask Settings...");
    auto modify = w->findChild<QDialog*>("modifyMaskDialog");
    QVERIFY(modify);
    modify->findChild<QPushButton*>("maskAction0_1")->click();
    auto channels = w->document().channels();
    QCOMPARE(w->alarmEngine().state(channels[0]).mask.text(), QString("CD---"));
    QCOMPARE(w->alarmEngine().state(channels[1]).mask.text(), QString("C---L"));
    modify->close();
    trigger("Force Mask...");
    auto forced = w->findChild<QDialog*>("forceMaskDialog");
    QVERIFY(forced);
    for (int bit = 0; bit < 5; ++bit)
      forced->findChild<QCheckBox*>("maskBit" + QString::number(bit))->setChecked(bit == 2);
    auto buttons = forced->findChild<QDialogButtonBox*>();
    buttons->button(QDialogButtonBox::Apply)->click();
    for (auto channel : channels)
      QCOMPARE(w->alarmEngine().state(channel).mask.text(), QString("--A--"));
    buttons->button(QDialogButtonBox::Reset)->click();
    QCOMPARE(w->alarmEngine().state(channels[0]).mask.text(), QString("-D---"));
    QCOMPARE(w->alarmEngine().state(channels[1]).mask.text(), QString("----L"));
  }
  void explicitLogSearchPaths_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("ordinary") << QString("alarm");
    QTest::newRow("hidden") << QString(".alarm");
    QTest::newRow("literal-date") << QString("alarm.2026-09-13");
    QTest::newRow("hidden-literal-date") << QString(".alarm.2026-09-13");
  }
  void explicitLogSearchPaths() {
    QFETCH(QString, name);
    QTemporaryDir dir;
    auto write = [&](const QString& path, const QByteArray& text) {
      QFile file(dir.filePath(path)); QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(text), qint64(text.size()));
    };
    write(name, "14-Sep-2026 10:00:00 : selected-match\n13-Sep-2026 10:00:00 : out-of-range\n");
    const auto base = name.endsWith("2026-09-13") ? name.left(name.size() - 11) : name;
    write(base + ".2026-09-14", "14-Sep-2026 11:00:00 : sibling-match\n");
    write(base + ".extra.2026-09-14", "14-Sep-2026 12:00:00 : wrong-family-match\n");
    LogSearch request; request.path = dir.filePath(name); request.contains = "match";
    request.from = QDateTime(QDate(2026, 9, 14), QTime(0, 0));
    request.to = request.from.addDays(1).addMSecs(-1);
    const auto result = searchLogs(request);
    QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join('\n')));
    QCOMPARE(result.records, 2);
    QCOMPARE(result.text.count("selected-match"), 1);
    QCOMPARE(result.text.count("sibling-match"), 1);
    QVERIFY(!result.text.contains("wrong-family"));
    QVERIFY(!result.text.contains("out-of-range"));
    QVERIFY(result.text.indexOf("selected-match") < result.text.indexOf("sibling-match"));
  }
  void historicalLogSearch() {
    QTemporaryDir dir;
    auto write = [&](const QString& name, const QByteArray& text) {
      QFile file(dir.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(text), qint64(text.size()));
    };
    write("alarm.2026-09-10", "10-Sep-2026 11:59:59 : PV before\n"
          "10-Sep-2026 12:00:00 : PV start\n"
          "<entry><date>10-Sep-2026</date> <time>23:00:00</time> PV xml</entry>\n"
          "a header without a timestamp\n");
    write("alarm", "Fri Sep 11 08:00:00 2026 : PV legacy\n");
    write("alarm.2026-09-11", "11-Sep-2026 12:00:00 : PV end\n"
          "11-Sep-2026 12:00:01 : PV after\n11-Sep-2026 10:00:00 : OTHER\n");
    write("alarm.extra.2026-09-11", "11-Sep-2026 09:00:00 : PV wrong family\n");
    write("alarm.2026-09-09", "09-Sep-2026 09:00:00 : PV wrong date\n");
    LogSearch request;
    request.path = dir.filePath("alarm.2026-09-12"); // Today's file need not exist.
    request.contains = "PV";
    request.from = QDateTime(QDate(2026, 9, 10), QTime(12, 0));
    request.to = QDateTime(QDate(2026, 9, 11), QTime(12, 0));
    auto result = searchLogs(request);
    QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join('\n')));
    QCOMPARE(result.records, 4);
    QCOMPARE(result.skipped, 1);
    QVERIFY(result.text.indexOf("start") < result.text.indexOf("xml"));
    QVERIFY(result.text.indexOf("xml") < result.text.indexOf("legacy"));
    QVERIFY(result.text.indexOf("legacy") < result.text.indexOf("end"));
    QVERIFY(!result.text.contains("wrong"));
    QVERIFY(!result.text.contains("before"));
    QVERIFY(!result.text.contains("after"));
    request.contains = "pv";
    QCOMPARE(searchLogs(request).records, 0); // Legacy With is case-sensitive.
    request.contains = "PV";
    request.maximumRecords = 2;
    QVERIFY(searchLogs(request).truncated);
    request.maximumRecords = 100000;
    request.maximumBytes = 10;
    QVERIFY(searchLogs(request).truncated);
    QVERIFY(searchLogs(request, [] { return true; }).cancelled);
    request.to = request.from.addSecs(-1);
    QVERIFY(!searchLogs(request).errors.isEmpty());
  }
  void multilineHistoricalSearch_data() {
    QTest::addColumn<bool>("checkpoint");
    QTest::newRow("legacy") << false; QTest::newRow("checkpoint") << true;
  }
  void multilineHistoricalSearch() {
    QFETCH(bool, checkpoint);
    QTemporaryDir dir; const auto path = dir.filePath("alarm");
    const QList<QByteArray> rows = {
      "13-Sep-2026 12:00:00 : newest\nonly-in-continuation C\n",
      "Sun Sep 13 12:00:00 2026 : oldest\nonly-in-continuation A\n",
      "<entry><date>13-Sep-2026</date> <time>12:00:00</time> middle\nonly-in-continuation B</entry>\n"};
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    for (const auto& row : rows) file.write(row);
    file.close();
    if (checkpoint) {
      QByteArray slot(64, 0), digest(32, 0); slot.replace(0, 8, "ALHPOS01");
      qToBigEndian<quint64>(4, slot.data() + 8); qToBigEndian<quint64>(3, slot.data() + 16);
      qToBigEndian<quint64>(1, slot.data() + 24);
      for (int index = 0; index < rows.size(); ++index) {
        const auto hash = QCryptographicHash::hash(QByteArray::number(index) + ':' + rows[index], QCryptographicHash::Sha256);
        for (int i = 0; i < digest.size(); ++i) digest[i] = char(digest.at(i) ^ hash.at(i));
      }
      slot.replace(32, 32, digest); slot += QCryptographicHash::hash(slot, QCryptographicHash::Sha256);
      QFile position(path + ".qtalh-position"); QVERIFY(position.open(QIODevice::WriteOnly));
      position.write(slot);
    }
    LogSearch request; request.path = path; request.contains = "only-in-continuation";
    request.from = request.to = QDateTime(QDate(2026, 9, 13), QTime(12, 0));
    const auto result = searchLogs(request);
    QVERIFY(result.errors.isEmpty()); QCOMPARE(result.records, 3); QCOMPARE(result.skipped, 0);
    QCOMPARE(result.text, QString::fromLocal8Bit(checkpoint ? rows[1] + rows[2] + rows[0] : rows[0] + rows[1] + rows[2]));
    request.maximumRecords = 1;
    const auto limited = searchLogs(request); QVERIFY(limited.truncated); QCOMPARE(limited.records, 1);
    QCOMPARE(limited.text, QString::fromLocal8Bit(rows[checkpoint ? 1 : 0]));
    request.maximumRecords = 100; request.maximumBytes = 40;
    QVERIFY(searchLogs(request).truncated);
    request.contains = "absent"; QVERIFY(!searchLogs(request).truncated);
    // Search past multiple chunks and recognize a match spanning their boundary.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("13-Sep-2026 12:00:00 : large\n");
    file.write(QByteArray(2 * 64 * 1024 - file.pos() - 2, 'x'));
    file.write("needle\n"); file.close();
    request.contains = "needle"; QVERIFY(searchLogs(request).truncated);
    int polls = 0;
    QVERIFY(searchLogs(request, [&] { return ++polls > 2; }).cancelled);
  }
  void historicalBrowserUi() {
    QTemporaryDir dir;
    QFile file(dir.filePath("events.2026-09-10"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("10-Sep-2026 12:00:59 : wanted\n10-Sep-2026 12:01:00 : outside\n");
    file.close();
    auto o = options(false);
    o.alarmFile = dir.filePath("events");
    o.opmodFile = dir.filePath("events");
    auto w = std::make_unique<Window>(sample(), o, false);
    for (const auto& actionName : {"Browser for Alarm Log", "Browser for Operation Log"}) {
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == actionName) action->trigger();
      auto dialog = w->findChild<QDialog*>(QString(actionName).contains("Alarm")
                                             ? "alarmLogBrowser" : "opmodLogBrowser");
      QVERIFY(dialog);
      auto search = dialog->findChild<QPushButton*>("logSearch");
      QCoreApplication::processEvents();
      QTRY_VERIFY(search->isEnabled());
      dialog->findChild<QDateTimeEdit*>("logFrom")->setDateTime(QDateTime(QDate(2026, 9, 10), QTime(12, 0)));
      dialog->findChild<QDateTimeEdit*>("logTo")->setDateTime(QDateTime(QDate(2026, 9, 10), QTime(12, 0)));
      search->click();
      QTRY_VERIFY(search->isEnabled());
      auto text = dialog->findChild<QPlainTextEdit*>("logResults")->toPlainText();
      QVERIFY(text.contains("wanted"));
      QVERIFY(!text.contains("outside"));
      search->click();
      dialog->close(); // Closing also interrupts and joins any active reader.
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
  }
  void selectionFollowingDialogs() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root first -D---\n$FORCEPV gate1 -D--- 1 NE\n"
                         "$BEEPSEVR MAJOR\nCHANNEL root second ----L\n$FORCEPV gate2 ----L 1 NE\n"
                         "$BEEPSEVR INVALID\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    auto view = w->findChild<QTreeView*>("groupContents");
    view->setCurrentIndex(view->model()->index(0, 2));
    for (const auto& name : {"Properties Window", "Modify Mask Settings...", "Force Mask...",
                             "Force Process Variable...", "Beep Severity..."})
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == name) action->trigger();
    auto properties = w->findChild<QDialog*>("propertiesDialog");
    auto force = w->findChild<QDialog*>("forcePvDialog");
    auto masks = w->findChild<QDialog*>("forceMaskDialog");
    auto beep = w->findChild<QDialog*>("beepSeverityDialog");
    QVERIFY(properties && force && masks && beep);
    int savedScroll = 0;
    if (auto tabs = properties->findChild<QTabWidget*>("propertyTabs")) {
      properties->resize(640, 300); tabs->setCurrentIndex(1);
      QCoreApplication::processEvents();
      auto scroll = properties->findChild<QScrollArea*>("propertyForceScroll")->verticalScrollBar();
      savedScroll = qMin(40, scroll->maximum()); QVERIFY(savedScroll > 0);
      scroll->setValue(savedScroll);
    }
    view->setCurrentIndex(view->model()->index(1, 2));
    QTRY_COMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("second"));
    QCOMPARE(w->findChild<QDialog*>("propertiesDialog"), properties);
    if (!legacyAppearance()) {
      QCOMPARE(properties->findChild<QTabWidget*>("propertyTabs")->currentIndex(), 1);
      QTRY_COMPARE(properties->findChild<QScrollArea*>("propertyForceScroll")->verticalScrollBar()->value(), savedScroll);
    }
    QCOMPARE(force->findChild<QLineEdit*>("forcePvName")->text(), QString("gate2"));
    QVERIFY(masks->findChild<QCheckBox*>("maskBit4")->isChecked());
    QVERIFY(beep->findChild<QRadioButton*>("beepSeverity3")->isChecked());
    auto second = w->document().channels()[1];
    w->alarmEngine().setMask(second, Mask::parse("C----"));
    w->alarmEngine().setBeep(second, 2);
    QTRY_VERIFY(masks->findChild<QCheckBox*>("maskBit0")->isChecked());
    QTRY_VERIFY(beep->findChild<QRadioButton*>("beepSeverity2")->isChecked());
    // The rebinding must update action targets too, not only labels.
    auto modify = w->findChild<QDialog*>("modifyMaskDialog");
    modify->findChild<QPushButton*>("maskAction1_1")->click();
    QCOMPARE(w->alarmEngine().state(second).mask.text(), QString("CD---"));
    QCOMPARE(w->alarmEngine().state(w->document().channels()[0]).mask.text(), QString("-D---"));
    QCoreApplication::processEvents();
    auto pv = force->findChild<QLineEdit*>("forcePvName");
    pv->setFocus();
    pv->selectAll();
    QTest::keyClicks(pv, "unfinished");
    w->alarmEngine().setBeep(second, 3);
    QTest::qWait(250);
    QCOMPARE(force->findChild<QLineEdit*>("forcePvName")->text(), QString("unfinished"));
    force->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(force->findChild<QLineEdit*>("forcePvName")->text(), QString("gate2"));
  }
  void persistentPropertiesApplyCancel() {
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL root one\nCHANNEL root two\n"),
                                      options(true), false);
    auto view = w->findChild<QTreeView*>("groupContents");
    view->setCurrentIndex(view->model()->index(2, 2));
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Properties Window") action->trigger();
    auto dialog = w->findChild<QDialog*>("propertiesDialog");
    QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("propertyALIAS")->setText("saved alias");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    QVERIFY(dialog->isVisible());
    QCOMPARE(w->document().channels()[1]->option("ALIAS"), QString("saved alias"));
    QTRY_VERIFY(dialog->findChild<QLineEdit*>("propertyNAME"));
    QCOMPARE(dialog->findChild<QLineEdit*>("propertyNAME")->text(), QString("two"));
    dialog->findChild<QLineEdit*>("propertyALIAS")->setText("discarded");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    QTRY_COMPARE(dialog->findChild<QLineEdit*>("propertyALIAS")->text(), QString("saved alias"));
    w->undoEdit();
    QTRY_VERIFY(dialog->findChild<QLineEdit*>("propertyALIAS"));
    QVERIFY(dialog->findChild<QLineEdit*>("propertyALIAS")->text().isEmpty());
    w->redoEdit();
    QTRY_VERIFY(dialog->findChild<QLineEdit*>("propertyALIAS"));
    QCOMPARE(dialog->findChild<QLineEdit*>("propertyALIAS")->text(), QString("saved alias"));
  }
  void disabledForceIndicator() {
    auto d = parseConfig("GROUP NULL root\n$FORCEPV rootgate -D--- 1 NE\n"
                         "CHANNEL root pv\n$FORCEPV gate -D--- 1 NE\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    auto label = w->findChild<QLabel*>("disabledForcePvCount");
    QVERIFY(label);
    QVERIFY(label->text().isEmpty());
    w->alarmEngine().setForceDisabled(w->document().root.get(), true);
    w->alarmEngine().setForceDisabled(w->document().channels()[0], true);
    QTRY_COMPARE(label->text(), QString("Disabled forcePVs: 2"));
    w->alarmEngine().setForceDisabled(w->document().root.get(), false);
    QTRY_COMPARE(label->text(), QString("Disabled forcePVs: 1"));
    w->alarmEngine().setForceDisabled(w->document().channels()[0], false);
    QTRY_VERIFY(label->text().isEmpty());
  }
  void activateEditedRuntime() {
    QTemporaryDir dir;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv\n");
    d.filename = dir.filePath("runtime.alhConfig");
    saveConfig(d, d.filename);
    auto o = options(true);
    o.config = d.filename;
    auto editor = std::make_unique<Window>(d, o, false);
    editor->addNode(false, "unsaved");
    auto before = writeConfig(editor->document());
    Window* runtime = nullptr;
    for (auto action : editor->findChildren<QAction*>())
      if (action->text() == "Activate ALH...") action->trigger();
    for (auto widget : QApplication::topLevelWidgets())
      if (widget != editor.get() && widget->windowTitle() == "Alarm Handler: root")
        runtime = dynamic_cast<Window*>(widget);
    QVERIFY(runtime);
    std::unique_ptr<Window> owner(runtime);
    QCOMPARE(runtime->document().channels().size(), 2);
    QCOMPARE(loadConfig(d.filename).channels().size(), 1); // Activation does not save over disk.
    runtime->alarmEngine().setMask(runtime->document().channels()[0], Mask::parse("-D---"));
    QCOMPARE(writeConfig(editor->document()), before);
    editor.reset();
    QCOMPARE(runtime->document().channels().size(), 2); // Independent window lifetime.
  }
  void debugCommandLine() {
    QTemporaryDir dir;
    auto path = dir.filePath("debug.alhConfig");
    saveConfig(sample(), path);
    auto executable = QDir(QCoreApplication::applicationDirPath()).filePath("qtalh");
    for (bool debug : {false, true}) {
      QProcess process;
      QStringList args{"--validate", path};
      if (debug) args.prepend("-debug");
      process.start(executable, args);
      QVERIFY(process.waitForFinished());
      QCOMPARE(process.exitCode(), 0);
      auto output = process.readAllStandardError();
      QCOMPARE(output.contains("qtalh debug"), debug);
      if (debug) {
        QVERIFY(output.contains("[startup]"));
        QVERIFY(output.contains("[config]"));
      }
    }
  }
  void styleCommandLine() {
    const auto executable = QDir(QCoreApplication::applicationDirPath()).filePath("qtalh");
    QTemporaryDir dir;
    auto config = dir.filePath("style.alhConfig");
    saveConfig(parseConfig("GROUP NULL root\n"), config);
    for (const auto& command : {"--help", "--version", "--validate"}) {
      QProcess process;
      auto env = QProcessEnvironment::systemEnvironment();
      env.remove("DISPLAY"); env.insert("QT_QPA_PLATFORM", "not-a-platform");
      process.setProcessEnvironment(env);
      process.start(executable, {"-style=unknown", command, config});
      QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
    }
    {
      QProcess process;
      process.start(executable, {"-platform", "offscreen", "-style=unknown", config});
      QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 1);
      auto error = process.readAllStandardError();
      QVERIFY(error.contains("Unknown widget style")); QVERIFY(error.contains("Fusion"));
    }
    for (const auto& style : QStringList{"", "motif", "FuSiOn"}) {
      QProcess process;
      auto env = QProcessEnvironment::systemEnvironment();
      env.insert("QT_STYLE_OVERRIDE", "Fusion");
      process.setProcessEnvironment(env);
      process.setReadChannel(QProcess::StandardError);
      QStringList arguments{"-platform", "offscreen", "-c", "-D", "-s", "-debug", config};
      if (!style.isEmpty()) arguments.prepend("-style=" + style);
      process.start(executable, arguments);
      QVERIFY(process.waitForStarted());
      QByteArray output;
      for (int attempt = 0; attempt < 10 && !output.contains("[appearance]"); ++attempt) {
        process.waitForReadyRead(500); output += process.readAllStandardError();
      }
      process.terminate();
      if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(); }
      QVERIFY2(output.contains(style == "FuSiOn" ? "style=fusion" : "style=motif"), output.constData());
      QVERIFY(!output.contains("invalid style override"));
    }
  }
  void propertiesEditRoundTrip() {
    auto d = parseConfig("GROUP NULL root\nCHANNEL root pv -D---\n$ACKPV ack 2\n"
                         "$FORCEPV gate -D--- 1 NE\n$ALARMCOUNTFILTER 2 10\n"
                         "$SEVRCOMMAND UP_MAJOR command\n$STATCOMMAND HIHI status-command\n"
                         "$GUIDANCE https://example.invalid/help\n$GUIDANCE\nOperator instructions\n$END\n"
                         "$GUIDANCE\nAdditional instructions\n$END\n");
    auto w = std::make_unique<Window>(d, options(true), false);
    auto view = w->findChild<QTreeView*>("groupContents");
    auto index = view->model()->index(0, 2);
    QVERIFY(QMetaObject::invokeMethod(view, "clicked", Qt::DirectConnection, Q_ARG(QModelIndex, index)));
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Properties Window") action->trigger();
    auto dialog = w->findChild<QDialog*>("propertiesDialog");
    QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("propertyALIAS")->setText("Edited alarm");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
    auto n = w->document().channels()[0];
    QCOMPARE(n->option("ALIAS"), QString("Edited alarm"));
    QVERIFY(n->option("BEEPSEVR").isEmpty());
    QCOMPARE(n->option("FORCEPV"), QString("gate -D--- 1 NE"));
    QCOMPARE(n->option("ACKPV"), QString("ack 2"));
    QCOMPARE(n->option("ALARMCOUNTFILTER"), QString("2 10"));
    QCOMPARE(n->option("SEVRCOMMAND"), QString("UP_MAJOR command"));
    QCOMPARE(n->option("STATCOMMAND"), QString("HIHI status-command"));
    QCOMPARE(n->option("GUIDANCE"), QString("https://example.invalid/help"));
    QCOMPARE(n->option("GUIDANCE_TEXT"), QString("Operator instructions\nAdditional instructions"));
    QTemporaryDir saved; QVERIFY(saved.isValid());
    const auto path = saved.filePath("guidance.alhConfig");
    w->saveTo(path);
    QCOMPARE(loadConfig(path).channels()[0]->option("GUIDANCE_TEXT"),
             QString("Operator instructions\nAdditional instructions"));
    QVERIFY(n->mask[Disable]);
    w->undoEdit();
    QVERIFY(w->document().channels()[0]->option("ALIAS").isEmpty());
  }
  void filtering() {
    auto d = sample();
    Engine e(d);
    auto channels = d.channels();
    for (auto n : channels)
      e.event(n, {0, 0, 0, 1, "0"});
    AlarmModel m(&d, &e, true);
    m.setFilter(1);
    QCOMPARE(m.rowCount(), 0);
    e.event(channels[0], {3, 2, 2, 1, "1"});
    m.refresh();
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.rowCount(m.index(0, 0)), 1);
  }
  void activeFilterRetainsTransientAlarms() {
    auto d = sample();
    Engine e(d);
    for (auto n : d.channels())
      e.event(n, {0, 0, 0, 1, "0"});
    auto n = d.channels()[0];
    AlarmModel tree(&d, &e, true), group(&d, &e, false);
    group.setGroup(n->parent);
    tree.setFilter(1);
    group.setFilter(1);
    e.event(n, {3, 2, 2, 1, "99"});
    e.event(n, {0, 0, 2, 1, "0"});
    tree.refresh();
    group.refresh();
    QCOMPARE(tree.rowCount(), 1);
    QCOMPARE(tree.rowCount(tree.index(0, 0)), 1);
    QCOMPARE(group.rowCount(), 1);
    e.acknowledge(n);
    tree.refresh();
    group.refresh();
    QCOMPARE(tree.rowCount(), 0);
    QCOMPARE(group.rowCount(), 0);
  }
  void manualMasksResetSilenceCurrent_data() {
    QTest::addColumn<bool>("group"); QTest::addColumn<int>("action");
    for (bool group : {false, true}) for (int action : {0, 1, 2})
      QTest::newRow(qPrintable(QString("group-%1-action-%2").arg(group).arg(action))) << group << action;
  }
  void manualMasksResetSilenceCurrent() {
    QFETCH(bool, group); QFETCH(int, action);
    auto o = options(false); o.silent = false;
    auto w = std::make_unique<Window>(parseConfig("GROUP NULL root\nCHANNEL root pv\n"), o, false);
    auto& e = w->alarmEngine(); auto n = w->document().channels()[0];
    e.event(n, {3, 2, 0, 1, "20"});
    if (!group) {
      auto view = w->findChild<QTreeView*>("groupContents");
      view->setCurrentIndex(view->model()->index(0, 2));
    }
    e.silenceCurrent = true; QVERIFY(!e.audible());
    auto menuAction = w->findChild<QAction*>(action == 0 ? "Modify Mask Settings..." : "Force Mask...");
    QVERIFY(menuAction); menuAction->trigger();
    if (action == 0) {
      auto dialog = w->findChild<QDialog*>("modifyMaskDialog"); QVERIFY(dialog);
      auto button = dialog->findChild<QPushButton*>("maskAction4_1"); QVERIFY(button);
      button->click();
    } else {
      auto dialog = w->findChild<QDialog*>("forceMaskDialog"); QVERIFY(dialog);
      auto buttons = dialog->findChild<QDialogButtonBox*>(); QVERIFY(buttons);
      buttons->button(action == 1 ? QDialogButtonBox::Apply : QDialogButtonBox::Reset)->click();
    }
    QVERIFY(!e.silenceCurrent); QVERIFY(e.audible());
    QCOMPARE(e.state(n).unack, 2); // Resuming sound must not acknowledge the alarm.
  }
  void manualNoAckCancelsTimer() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    auto& e = w->alarmEngine();
    qint64 time = 1000;
    e.now = e.monotonicNow = [&] { return time; };
    auto root = w->document().root.get();
    e.noAck(root, true);
    e.noAck(w->document().channels()[0], true);
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Modify Mask Settings...")
        action->trigger();
    QDialog* dialog = nullptr;
    for (auto candidate : w->findChildren<QDialog*>())
      if (candidate->windowTitle() == "Modify Mask Settings")
        dialog = candidate;
    QVERIFY(dialog);
    auto noAck = dialog->findChild<QPushButton*>("maskAction2_1");
    QVERIFY(noAck);
    noAck->click(); // Explicitly set NoAck On, without changing other fields.
    QCOMPARE(e.state(root).noAckUntil, qint64(0));
    time += 3600001;
    e.tick();
    for (auto n : w->document().channels()) {
      QVERIFY(e.state(n).mask[Ack]);
      QCOMPARE(e.state(n).noAckUntil, qint64(0));
    }
  }
  void expandViewActions_data() {
    QTest::addColumn<bool>("editor"); QTest::addColumn<int>("column");
    for (bool editor : {false, true})
      for (int column : {0, 2, 3, 6})
        QTest::newRow(qPrintable(QString("%1-column-%2").arg(editor ? "editor" : "runtime").arg(column)))
            << editor << column;
  }
  void expandViewActions() {
    QFETCH(bool, editor); QFETCH(int, column);
    auto w = std::make_unique<Window>(parseConfig(
        "GROUP NULL root\nGROUP root A\nGROUP A B\nGROUP B C\nCHANNEL C one\n"
        "GROUP root X\nGROUP X Y\nCHANNEL Y two\n"), options(editor), false);
    w->show();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto model = tree->model();
    const auto root = model->index(0, 0);
    const auto a = model->index(0, 0, root), b = model->index(0, 0, a);
    const auto x = model->index(1, 0, root);
    auto trigger = [&](const QString& title) {
      for (auto action : w->findChildren<QAction*>())
        if (action->text() == title) { action->trigger(); return; }
      QFAIL("View action missing");
    };
    tree->collapseAll(); tree->expand(root);
    tree->setCurrentIndex(a.sibling(a.row(), column));
    trigger("Expand Branch");
    QVERIFY(tree->isExpanded(a)); QVERIFY(tree->isExpanded(b));
    QVERIFY(!tree->isExpanded(x));
    trigger("Collapse Branch"); QVERIFY(!tree->isExpanded(a));
    trigger("Expand One Level");
    QVERIFY(tree->isExpanded(a)); QVERIFY(!tree->isExpanded(b));
    trigger("Expand One Level"); QVERIFY(!tree->isExpanded(a));
    trigger("Expand One Level"); QVERIFY(tree->isExpanded(a));
    trigger("Expand All");
    QVERIFY(tree->isExpanded(root)); QVERIFY(tree->isExpanded(a));
    QVERIFY(tree->isExpanded(b)); QVERIFY(tree->isExpanded(x));
    trigger("Collapse Branch"); QVERIFY(!tree->isExpanded(a)); QVERIFY(tree->isExpanded(x));
    tree->collapseAll(); tree->setCurrentIndex(QModelIndex());
    trigger("Expand Branch"); QVERIFY(!tree->isExpanded(root));
    tree->expand(root); tree->setCurrentIndex(a.sibling(a.row(), column));
    w->activateWindow(); tree->setFocus(); QCoreApplication::processEvents();
    QTest::keyClick(tree, Qt::Key_Asterisk);
    QVERIFY(tree->isExpanded(a)); QVERIFY(tree->isExpanded(b));
    QTest::keyClick(tree, Qt::Key_Minus); QVERIFY(!tree->isExpanded(a));
    QTest::keyClick(tree, Qt::Key_Plus);
    QVERIFY(tree->isExpanded(a)); QVERIFY(!tree->isExpanded(b));
    QTest::keyClick(tree, Qt::Key_Plus); QVERIFY(!tree->isExpanded(a));
    QTest::keyClick(tree, Qt::Key_Asterisk, Qt::ControlModifier);
    QVERIFY(tree->isExpanded(a)); QVERIFY(tree->isExpanded(b)); QVERIFY(tree->isExpanded(x));
  }

  void cancelledRowSelection_data() {
    QTest::addColumn<bool>("editor");
    QTest::addColumn<bool>("treePane");
    QTest::newRow("runtime-tree") << false << true;
    QTest::newRow("runtime-group") << false << false;
    QTest::newRow("editor-tree") << true << true;
    QTest::newRow("editor-group") << true << false;
  }
  void cancelledRowSelection() {
    QFETCH(bool, editor);
    QFETCH(bool, treePane);
    auto w = std::make_unique<Window>(parseConfig(treePane
        ? "GROUP NULL root\nGROUP root first\nCHANNEL first one\nGROUP root second\nCHANNEL second two\n"
        : "GROUP NULL root\nCHANNEL root first\nCHANNEL root second\n"), options(editor), false);
    w->show(); QCoreApplication::processEvents();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    auto view = treePane ? tree : group;
    const auto parent = treePane ? tree->model()->index(0, 0) : QModelIndex();
    const auto first = view->model()->index(0, 2, parent);
    const auto second = view->model()->index(1, 2, parent);
    view->setCurrentIndex(first);
    QCoreApplication::processEvents();
    const auto contents = group->model()->index(0, 2).data().toString();
    const auto point = view->visualRect(second).center();
    const auto blank = QPoint(view->viewport()->width() / 2, view->viewport()->height() - 10);
    QVERIFY(!view->indexAt(blank).isValid());
    for (const auto release : {blank, view->visualRect(first).center()}) {
      QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QCOMPARE(view->currentIndex(), first);
      QCOMPARE(group->model()->index(0, 2).data().toString(), contents);
      QTest::mouseMove(view->viewport(), release);
      QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, release);
      QCOMPARE(view->currentIndex(), first);
      QCOMPARE(group->model()->index(0, 2).data().toString(), contents);
    }
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(view->currentIndex(), second);
    if (treePane) QCOMPARE(group->model()->index(0, 2).data().toString(), QString("two"));
  }

  void mouseNavigation_data() {
    QTest::addColumn<bool>("editor");
    QTest::newRow("runtime") << false;
    QTest::newRow("editor") << true;
  }
  void mouseNavigation() {
    QFETCH(bool, editor);
    auto w = std::make_unique<Window>(parseConfig(
        "GROUP NULL root\nGROUP root A\nGROUP A B\nGROUP B C\nCHANNEL C one\n"
        "GROUP root X\nCHANNEL X two\n"), options(editor), false);
    w->show(); QCoreApplication::processEvents();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    const auto root = tree->model()->index(0, 0);
    const auto a = tree->model()->index(0, 0, root);
    const auto b = tree->model()->index(0, 0, a);
    const auto c = tree->model()->index(0, 0, b);
    auto doubleClick = [](QTreeView* view, QModelIndex index) {
      const auto point = view->visualRect(index).center();
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    };
    for (auto view : {tree, group}) {
      tree->setCurrentIndex(root.sibling(0, 2));
      for (bool expanded : {false, true}) {
        tree->collapseAll(); tree->expand(root);
        if (expanded) { tree->expand(a); tree->expand(b); }
        QCoreApplication::processEvents();
        const auto arrow = view == tree ? a.sibling(a.row(), 3) : group->model()->index(0, 3);
        doubleClick(view, arrow);
        QVERIFY(tree->isExpanded(a)); QVERIFY(tree->isExpanded(b));
        QCOMPARE(tree->currentIndex(), root.sibling(0, 2));
        QCOMPARE(group->model()->index(0, 2).data().toString(), QString("A"));
        QTest::qWait(QApplication::doubleClickInterval() + 30);
        QVERIFY(tree->isExpanded(a)); QVERIFY(tree->isExpanded(b));
      }
    }
    doubleClick(group, group->model()->index(0, 2));
    QCOMPARE(tree->currentIndex(), a.sibling(a.row(), 2));
    QCOMPARE(group->model()->index(0, 2).data().toString(), QString("B"));
    QVERIFY(!group->currentIndex().isValid());
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Collapse Branch") action->trigger();
    QVERIFY(!tree->isExpanded(a));
    tree->expand(a); tree->expand(b); tree->setCurrentIndex(c.sibling(c.row(), 2));
    doubleClick(group, group->model()->index(0, 2));
    QVERIFY(!w->findChild<QDialog*>("propertiesDialog"));
    QCOMPARE(group->currentIndex(), group->model()->index(0, 2));
    for (int column : {0, 1, 6, 7, 8}) {
      auto index = group->model()->index(0, column);
      if (!group->visualRect(index).isEmpty()) doubleClick(group, index);
      if (!(editor && column >= 6)) QVERIFY(!w->findChild<QDialog*>("propertiesDialog"));
      QCOMPARE(group->model()->index(0, 2).data().toString(), QString("one"));
    }
  }
  void rowButtonsPreserveSelection_data() {
    QTest::addColumn<bool>("treePane");
    QTest::newRow("tree") << true;
    QTest::newRow("group") << false;
  }
  void rowButtonsPreserveSelection() {
    QFETCH(bool, treePane);
    auto d = parseConfig(treePane
        ? "GROUP NULL root\nGROUP root first\nCHANNEL first one\nGROUP root second\n"
          "$GUIDANCE\nSecond guidance\n$END\n$COMMAND First choice!unused-one!Second choice!unused-two\nCHANNEL second two\n"
        : "GROUP NULL root\nCHANNEL root first\nCHANNEL root second\n"
          "$GUIDANCE\nSecond guidance\n$END\n$COMMAND First choice!unused-one!Second choice!unused-two\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    const auto nodes = w->document().channels();
    for (auto node : nodes) w->alarmEngine().event(node, {3, 2, 2, 1, "12"});
    w->show(); QCoreApplication::processEvents();
    auto view = w->findChild<QTreeView*>(treePane ? "alarmTree" : "groupContents");
    const auto parent = treePane ? view->model()->index(0, 0) : QModelIndex();
    const auto first = view->model()->index(0, 2, parent);
    view->setCurrentIndex(first);
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Properties Window") action->trigger();
    auto properties = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(properties);
    for (int column : {0, 4, 5}) {
      const auto index = view->model()->index(1, column, parent);
      const auto point = view->visualRect(index).center();
      QCOMPARE(view->indexAt(point), index);
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
      QCoreApplication::processEvents();
      QCOMPARE(view->currentIndex(), first);
      QCOMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("first"));
      if (column == 4) {
        bool found = false;
        for (auto dialog : w->findChildren<QDialog*>())
          if (dialog->windowTitle() == "Guidance: second") {
            QCOMPARE(dialog->findChild<QPlainTextEdit*>()->toPlainText(), QString("Second guidance"));
            found = true; dialog->close();
          }
        QVERIFY(found);
      }
      if (column == 5) {
        bool found = false;
        for (auto menu : w->findChildren<QMenu*>())
          if (menu->isVisible() && !menu->actions().isEmpty() && menu->actions().first()->text() == "First choice") {
            found = true; menu->close();
          }
        QVERIFY(found);
      }
    }
    QCOMPARE(w->alarmEngine().state(nodes[0]).unack, 2);
    QCOMPARE(w->alarmEngine().state(nodes[1]).unack, 0);
    for (auto action : w->findChildren<QAction*>())
      if (action->text().contains("Acknowledge Alarm")) action->trigger();
    QCOMPARE(w->alarmEngine().state(nodes[0]).unack, 0);
  }
  void middleButtonNameCopy_data() {
    QTest::addColumn<bool>("treePane"); QTest::addColumn<int>("row");
    QTest::newRow("left group") << true << 0;
    QTest::newRow("right group") << false << 0;
    QTest::newRow("channel") << false << 1;
  }
  void middleButtonNameCopy() {
    QFETCH(bool, treePane); QFETCH(int, row);
    auto d = parseConfig("GROUP NULL root\nGROUP root branch\nCHANNEL branch child\nCHANNEL root test:pv\n");
    Engine engine(d); AlarmModel model(&d, &engine, treePane);
    AlarmView view(treePane); view.setModel(&model); view.resize(800, 200); view.show();
    QCoreApplication::processEvents();
    auto clipboard = QApplication::clipboard();
    clipboard->setText("previous clipboard");
    if (clipboard->supportsSelection()) clipboard->setText("previous selection", QClipboard::Selection);
    const auto index = model.index(row, 2);
    const auto selected = view.currentIndex();
    const auto expected = index.data().toString();
    const auto point = view.visualRect(index).center();
    QTest::mouseMove(view.viewport(), point);
    QCOMPARE(clipboard->text(), QString("previous clipboard"));
    QTest::mouseClick(view.viewport(), Qt::MiddleButton, Qt::NoModifier, point);
    QCOMPARE(view.currentIndex(), selected);
    QCOMPARE(clipboard->text(), expected);
    if (clipboard->supportsSelection()) QCOMPARE(clipboard->text(QClipboard::Selection), expected);
    // Non-name controls must leave the copied name alone.
    QTest::mouseClick(view.viewport(), Qt::MiddleButton, Qt::NoModifier,
                      view.visualRect(model.index(row, 0)).center());
    QCOMPARE(clipboard->text(), expected);
    view.hide(); // Copy remains available after leaving the source, without a drag/drop.
    QPlainTextEdit destination; destination.show(); destination.setFocus();
    QCoreApplication::processEvents();
    destination.paste();
    QCOMPARE(destination.toPlainText(), expected);
    if (clipboard->supportsSelection()) {
      destination.clear();
      QTest::mouseClick(destination.viewport(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
      QTRY_COMPARE(destination.toPlainText(), expected);
    }
  }

  void middleButtonNameDrag() {
    class DragView : public AlarmView {
    public:
      DragView() : AlarmView(false) {}
      QStringList dragged;
      QPixmap preview;
      void executeNameDrag(QDrag* drag) override {
        dragged << drag->mimeData()->text();
        preview = drag->pixmap();
      }
    };
    auto d = parseConfig("GROUP NULL root\nCHANNEL root first\nCHANNEL root second\n");
    Engine engine(d); AlarmModel model(&d, &engine, false);
    DragView view; view.setModel(&model); view.resize(800, 200); view.show();
    QCoreApplication::processEvents();
    const auto first = model.index(0, 2), second = model.index(1, 2);
    view.setCurrentIndex(first);
    auto drag = [&](QModelIndex index, Qt::MouseButton button) {
      const auto point = view.visualRect(index).center();
      QTest::mousePress(view.viewport(), button, Qt::NoModifier, point);
      const auto movePoint = point + QPoint(QApplication::startDragDistance() + 2, 0);
      QMouseEvent move(QEvent::MouseMove, movePoint, view.viewport()->mapToGlobal(movePoint),
                       Qt::NoButton, button, Qt::NoModifier);
      QApplication::sendEvent(view.viewport(), &move);
      QTest::mouseRelease(view.viewport(), button, Qt::NoModifier, point);
    };
    drag(second, Qt::MiddleButton);
    QCOMPARE(view.dragged, QStringList({"second"}));
    QCOMPARE(QApplication::clipboard()->text(), QString("second"));
    if (QApplication::clipboard()->supportsSelection())
      QCOMPARE(QApplication::clipboard()->text(QClipboard::Selection), QString("second"));
    QVERIFY(!view.preview.isNull());
    QVERIFY(view.preview.width() >= QFontMetrics(QToolTip::font()).horizontalAdvance("second"));
    QCOMPARE(view.currentIndex(), first);
    drag(model.index(1, 0), Qt::MiddleButton);
    QCOMPARE(view.dragged.size(), 1);
    drag(second, Qt::LeftButton);
    QCOMPARE(view.dragged.size(), 1);
    QCOMPARE(view.currentIndex(), second);
    QVERIFY(!second.data(Qt::ToolTipRole).toString().contains("middle mouse button"));
    QVERIFY(model.flags(second) & Qt::ItemIsDragEnabled);
    QVERIFY(!(model.flags(model.index(1, 0)) & Qt::ItemIsDragEnabled));
    // A reset invalidates an armed drag or delayed arrow; no old node may be used.
    QTest::mousePress(view.viewport(), Qt::MiddleButton, Qt::NoModifier, view.visualRect(second).center());
    model.reset(&d, &engine);
    const QPoint movePoint(500, 100);
    QMouseEvent move(QEvent::MouseMove, movePoint, view.viewport()->mapToGlobal(movePoint),
                     Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &move);
    QCOMPARE(view.dragged.size(), 1);
  }

  void channelSelectionFeedback_data() {
    QTest::addColumn<int>("filter");
    QTest::newRow("all") << 0;
    QTest::newRow("active") << 1;
  }
  void channelSelectionFeedback() {
    QFETCH(int, filter);
    auto opts = options(false); opts.filter = filter;
    auto w = std::make_unique<Window>(parseConfig(
        "GROUP NULL root\nCHANNEL root first\nCHANNEL root second\n"), opts, false);
    auto& engine = w->alarmEngine(); const auto nodes = w->document().channels();
    for (auto node : nodes) engine.event(node, {3, 2, 2, 1, "12"});
    w->show();
    auto view = w->findChild<QTreeView*>("groupContents");
    QTRY_COMPARE(view->model()->rowCount(), 2);
    QCoreApplication::processEvents();
    auto clickName = [&](int row) {
      const auto index = view->model()->index(row, 2);
      QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                        view->visualRect(index).center());
    };
    auto checkBevel = [&](int row, bool down) {
      if (!legacyAppearance()) return;
      const auto image = view->viewport()->grab().toImage();
      const auto rect = view->visualRect(view->model()->index(row, 2));
      const auto point = (rect.topLeft() + QPoint(5, 0)) * image.devicePixelRatio();
      QCOMPARE(image.pixelColor(point), QColor(down ? "#5f696d" : "#dde6e9"));
    };
    clickName(0); checkBevel(0, true); checkBevel(1, false);
    QCOMPARE(engine.state(nodes[0]).unack, 2); QCOMPARE(engine.state(nodes[1]).unack, 2);
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Properties Window") action->trigger();
    auto properties = w->findChild<QDialog*>("propertiesDialog"); QVERIFY(properties);
    QCOMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("first"));
    checkBevel(0, true); // Opening another window must not remove the selection indicator.
    clickName(1);
    QTRY_COMPARE(properties->findChild<QLineEdit*>("propertyNAME")->text(), QString("second"));
    checkBevel(0, false); checkBevel(1, true);
    engine.event(nodes[1], {3, 2, 2, 1, "13"});
    QTest::qWait(1200); checkBevel(1, true);
    for (auto action : w->findChildren<QAction*>())
      if (action->text().contains("Acknowledge Alarm")) action->trigger();
    QCOMPARE(engine.state(nodes[0]).unack, 2); QCOMPARE(engine.state(nodes[1]).unack, 0);
    QDir().mkpath(TEST_OUTPUT);
    QVERIFY(w->grab().save(QString(TEST_OUTPUT) + (legacyAppearance()
        ? "/classic-channel-selection.png" : "/fusion-channel-selection.png")));
  }

  void filteredPendingClicks_data() {
    QTest::addColumn<bool>("treePane");
    QTest::addColumn<int>("filter");
    QTest::addColumn<QString>("change");
    for (bool tree : {false, true})
      for (int filter : {1, 2})
        for (const auto change : {"value", "sibling", "target"})
          QTest::newRow(qPrintable(QString("%1-filter%2-%3")
              .arg(tree ? "arrow" : "ack").arg(filter).arg(change))) << tree << filter << QString(change);
  }
  void filteredPendingClicks() {
    QFETCH(bool, treePane); QFETCH(int, filter); QFETCH(QString, change);
    auto d = parseConfig(treePane
        ? "GROUP NULL root\nGROUP root target\nGROUP target leaf\nCHANNEL leaf pv\n"
          "GROUP root sibling\nCHANNEL sibling other\n"
        : "GROUP NULL root\nCHANNEL root pv\nCHANNEL root other\n");
    Engine engine(d);
    for (auto n : d.channels()) engine.event(n, {3, 2, 2, 1, "alarm"});
    AlarmModel model(&d, &engine, treePane); model.setFilter(filter);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    AlarmView view(treePane); view.setModel(&model); view.resize(700, 300); view.show();
    if (treePane) view.expand(model.index(0, 0));
    QCoreApplication::processEvents();
    auto parent = treePane ? model.index(0, 0) : QModelIndex();
    QPersistentModelIndex target(model.index(0, treePane ? 3 : 0, parent));
    auto targetNode = model.node(target);
    auto point = view.visualRect(target).center();
    QCOMPARE(view.indexAt(point), QModelIndex(target));
    int clicks = 0; Node* clicked = nullptr;
    connect(&view, &QTreeView::clicked, &view, [&](QModelIndex index) {
      ++clicks; clicked = model.node(index);
      if (!treePane) engine.acknowledge(clicked);
    });
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    if (treePane) QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    else QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    if (change == "value") engine.event(d.channels()[0], {3, 2, 2, 1, "new value"});
    else {
      auto n = d.channels()[change == "target" ? 0 : 1];
      engine.event(n, {}); engine.acknowledge(n);
    }
    model.refresh();
    // Also reinsert a sibling while the action is armed; surviving indexes must
    // remain usable across both removals and insertions, not only value changes.
    if (change == "sibling") {
      engine.event(d.channels()[1], {3, 2, 2, 1, "again"}); model.refresh();
    }
    if (treePane) QTest::qWait(QApplication::doubleClickInterval() + 30);
    else QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(clicks, change == "target" ? 0 : 1);
    if (change == "target") QVERIFY(!target.isValid());
    else {
      QCOMPARE(clicked, targetNode);
      if (!treePane) QCOMPARE(engine.state(d.channels()[0]).unack, 0);
    }
  }
  void filteredSelectionSurvivesUpdates() {
    auto o = options(false);
    o.filter = 1;
    auto w = std::make_unique<Window>(sample(), o, false);
    w->show();
    for (auto n : w->document().channels())
      w->alarmEngine().event(n, {3, 2, 2, 1, "99"});
    QTest::qWait(250);
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto root = tree->model()->index(0, 0);
    tree->expand(root);
    auto branch = tree->model()->index(1, 0, root);
    tree->setCurrentIndex(branch);
    auto name = branch.sibling(branch.row(), 2).data().toString();
    w->alarmEngine().event(w->document().channels()[0], {3, 2, 2, 1, "100"});
    QTest::qWait(1200);
    QVERIFY(tree->isExpanded(tree->model()->index(0, 0)));
    auto current = tree->currentIndex();
    QCOMPARE(current.sibling(current.row(), 2).data().toString(), name);
  }
  void motifLayoutAndHitTargets() {
    auto d = parseConfig(
        "GROUP NULL Radiation_Monitors\nGROUP Radiation_Monitors Gamma_and_Neutron_Readings\nGROUP "
        "Gamma_and_Neutron_Readings Linac/PAR\nCHANNEL Linac/PAR test:pv -D---\nGROUP "
        "Radiation_Monitors Masking\nCHANNEL Masking test:mask");
    auto w = std::make_unique<Window>(std::move(d), options(false), false);
    for (auto node : w->document().channels())
      w->alarmEngine().event(node, {0, 0, 0, 1, "0"});
    w->showInitial();
    QTest::qWait(250);
    QWidget* runtime = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "runtimeWindow")
        runtime = widget;
    QVERIFY(runtime);
    if (legacyAppearance()) QCOMPARE(runtime->size(), QSize(220, 35));
    else { QVERIFY(runtime->width() >= 220); QVERIFY(runtime->height() >= 35); }
    QCOMPARE(runtime->findChildren<QPushButton*>().size(), 1);
    QCOMPARE(runtime->findChild<QPushButton*>("runtimeAlarm")->text(),
             QString("Radiation_Monitors  <-D--->"));
    auto tree = w->findChild<QTreeView*>("alarmTree");
    auto group = w->findChild<QTreeView*>("groupContents");
    auto root = tree->model()->index(0, 0);
    auto gamma = tree->model()->index(0, 0, root);
    QVERIFY(tree->isExpanded(root));
    QVERIFY(!tree->isExpanded(gamma));
    QCOMPARE(root.sibling(0, 8).data().toString(), QString());
    auto name = gamma.sibling(gamma.row(), 2);
    auto nameRect = tree->visualRect(name);
    auto font = name.data(Qt::FontRole).value<QFont>();
    QVERIFY(nameRect.width() >= QFontMetrics(font).horizontalAdvance(name.data().toString()) + 8);
    QCOMPARE(tree->indexAt(nameRect.center()), name);
    auto arrow = gamma.sibling(gamma.row(), 3);
    QCOMPARE(tree->indexAt(tree->visualRect(arrow).center()), arrow);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(arrow).center());
    QTRY_VERIFY(tree->isExpanded(gamma));
    auto leaf = tree->model()->index(0, 0, gamma);
    QVERIFY(leaf.sibling(0, 3).data().toString().isEmpty());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->visualRect(leaf.sibling(0, 2)).center());
    QCOMPARE(group->model()->rowCount(), 1);
    QCOMPARE(group->model()->index(0, 2).data().toString(), QString("test:pv"));
    QVERIFY(group->model()->index(0, 8).data().toString().isEmpty());
    auto slider = w->findChild<QSlider*>("paneWidth");
    QVERIFY(slider);
    slider->setValue(70);
    QVERIFY(tree->width() > group->width());
  }
  void fontSizeShortcuts() {
    const auto startup = QApplication::font();
    // Always reset the application-wide setting, including after a failed check.
    struct ResetFont {
      ~ResetFont() {
        QWidget target;
        QTest::keyClick(&target, Qt::Key_0, Qt::ControlModifier);
      }
    } reset;
    auto o = options(false);
    QFont custom("monospace", 14);
    o.font = custom.toString();
    auto w = std::make_unique<Window>(sample(), o, false);
    w->showInitial();
    auto view = w->findChild<QTreeView*>("groupContents");
    QVERIFY(view);
    QWidget* runtime = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "runtimeWindow") runtime = widget;
    QVERIFY(runtime);
    auto button = runtime->findChild<QPushButton*>("runtimeAlarm");
    QVERIFY(button);
    QCoreApplication::processEvents();
    auto index = view->model()->index(0, 2);
    const auto originalRect = view->visualRect(index);
    const auto originalButtonFont = button->font();
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Current Alarm History") action->trigger();
    auto history = w->findChild<QDialog*>("historyDialog");
    QVERIFY(history);
    auto reader = history->findChild<QPlainTextEdit*>();
    QVERIFY(reader);
    const auto originalReaderFont = reader->font();
    QTest::keyClick(view, Qt::Key_Equal, Qt::ControlModifier);
    if (legacyAppearance()) {
      QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
      QTest::keyClick(view, Qt::Key_Minus, Qt::ControlModifier);
      QTest::keyClick(view, Qt::Key_0, Qt::ControlModifier);
      QCOMPARE(QApplication::font(), startup);
      QCOMPARE(view->visualRect(index), originalRect);
      QCOMPARE(button->font(), originalButtonFont);
      QCOMPARE(reader->font(), originalReaderFont);
      return;
    }
    auto points = [](const QFont& font) {
      return font.pointSizeF() > 0 ? font.pointSizeF() : QFontInfo(font).pointSizeF();
    };
    const qreal base = points(startup);
    QCOMPARE(QApplication::font().pointSizeF(), base + 1);
    // Modal dialogs and text editors use the same global shortcuts.
    history->setWindowModality(Qt::ApplicationModal);
    QTest::keyClick(reader, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(QApplication::font().pointSizeF(), base + 2);
    QCOMPARE(reader->font().pointSizeF(), points(originalReaderFont) + 2);
    QCOMPARE(reader->font().family(), originalReaderFont.family());
    QVERIFY(QFontInfo(reader->font()).fixedPitch());
    QCOMPARE(button->font().pointSizeF(), points(originalButtonFont) + 2);
    QCOMPARE(button->font().family(), originalButtonFont.family());
    QCoreApplication::processEvents();
    QVERIFY(view->visualRect(index).height() > originalRect.height());
    QVERIFY(view->visualRect(index).width() > originalRect.width());
    QCOMPARE(view->indexAt(view->visualRect(index).center()), index);
    auto second = std::make_unique<Window>(sample(), options(true), false);
    QCOMPARE(second->font().pointSizeF(), base + 2);
    for (auto action : second->findChildren<QAction*>())
      if (action->text() == "Properties Window") action->trigger();
    auto properties = second->findChild<QDialog*>("propertiesDialog");
    QVERIFY(properties);
    auto command = properties->findChild<QPlainTextEdit*>("propertySEVRCOMMAND");
    QVERIFY(command);
    QCOMPARE(command->font().pointSizeF(), points(contentFont()) + 2);
    QTest::keyClick(button, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(QApplication::font().pointSizeF(), base + 1);
    QCOMPARE(second->font().pointSizeF(), base + 1);
    QTest::keyClick(command, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(QApplication::font(), startup);
    QCOMPARE(reader->font(), originalReaderFont);
    QCOMPARE(button->font(), originalButtonFont);
    QCoreApplication::processEvents();
    QCOMPARE(view->visualRect(index), originalRect);
    // Guard against invalid/negative sizes and excessive growth on auto-repeat.
    for (int n = 0; n < 100; ++n) QTest::keyClick(view, Qt::Key_Minus, Qt::ControlModifier);
    QVERIFY(QApplication::font().pointSizeF() >= 6);
    auto smallest = QApplication::font();
    QTest::keyClick(view, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(QApplication::font(), smallest);
    for (int n = 0; n < 100; ++n) QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier);
    QVERIFY(QApplication::font().pointSizeF() <= 72);
    auto largest = QApplication::font();
    QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier);
    QCOMPARE(QApplication::font(), largest);
  }
  void styledRowGeometryAndPalette() {
    if (legacyAppearance()) QSKIP("Styled presentation contract; run with QTALH_TEST_STYLE=fusion");
    QVERIFY(QFontInfo(contentFont()).fixedPitch());
    struct Restore {
      QFont font = QApplication::font();
      QPalette palette = QApplication::palette();
      ~Restore() { QApplication::setFont(font); QApplication::setPalette(palette); }
    } restore;
    auto d = parseConfig("GROUP NULL root\nCHANNEL root very_long_channel_name_for_geometry\n$GUIDANCE\nGuidance\n$END\n$COMMAND echo test\n");
    auto w = std::make_unique<Window>(d, options(false), false);
    w->show();
    auto view = w->findChild<QTreeView*>("groupContents");
    QCoreApplication::processEvents();
    auto index = view->model()->index(0, 2);
    auto initial = view->visualRect(index);
    QVERIFY(!index.data(Qt::BackgroundRole).value<QColor>().isValid());
    auto large = QApplication::font(); large.setPointSize(20); QApplication::setFont(large);
    QCoreApplication::processEvents();
    auto enlarged = view->visualRect(index);
    QVERIFY(enlarged.height() > initial.height());
    QVERIFY(enlarged.width() > initial.width());
    for (int column : {0, 1, 2, 4, 5, 6, 7, 8}) {
      auto cell = index.sibling(0, column);
      auto rect = view->visualRect(cell);
      if (!rect.isEmpty()) QCOMPARE(view->indexAt(rect.center()), cell);
    }
    auto dark = QApplication::palette();
    dark.setColor(QPalette::Base, QColor("#202020")); dark.setColor(QPalette::Text, Qt::white);
    dark.setColor(QPalette::Window, QColor("#303030")); dark.setColor(QPalette::WindowText, Qt::white);
    dark.setColor(QPalette::Button, QColor("#404040")); dark.setColor(QPalette::ButtonText, Qt::white);
    QApplication::setPalette(dark);
    auto channel = w->document().channels()[0];
    for (int severity = 0; severity <= 4; ++severity) {
      w->alarmEngine().event(channel, {severity ? 3 : 0, severity, severity, 1, "1"});
      QTest::qWait(150);
      auto badge = view->visualRect(view->model()->index(0, 1));
      auto capture = view->viewport()->grab().toImage();
      if (severity) {
        auto expected = view->model()->index(0, 1).data(Qt::BackgroundRole).value<QColor>();
        QCOMPARE(capture.pixelColor((badge.topLeft() + QPoint(3, 3)) * capture.devicePixelRatio()), expected);
      }
    }
    QVERIFY(w->grab().save(QString(TEST_OUTPUT) + "/fusion-large-dark.png"));
    QVERIFY(w->findChild<QWidget*>("alarmLegend")->isHidden());
  }
  void relatedCommandDelimiters_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<QStringList>("labels");
    QTest::addColumn<QStringList>("commands");
    QTest::newRow("plain") << "echo hello" << QStringList{} << QStringList{"echo hello"};
    QTest::newRow("wrapped-single") << "!echo hello!" << QStringList{} << QStringList{"echo hello"};
    QTest::newRow("leading-and-trailing")
        << "!Display!echo hello!" << QStringList{"Display"} << QStringList{"echo hello"};
    QTest::newRow("repeated-delimiters")
        << "!!First!!echo one!!Second!echo two!" << QStringList{"First", "Second"}
        << QStringList{"echo one", "echo two"};
    QTest::newRow("malformed-pairs") << "First!echo one!Second" << QStringList{} << QStringList{};
  }
  void relatedCommandDelimiters() {
    QFETCH(QString, command);
    QFETCH(QStringList, labels);
    QFETCH(QStringList, commands);
    auto d = parseConfig("GROUP NULL root\n$COMMAND " + command);
    auto w = std::make_unique<Window>(std::move(d), options(false), false);
    QStringList launched;
    w->alarmEngine().command = [&](const QString& s) { launched << s; };
    auto action = w->findChild<QAction*>("Start Related Process");
    QVERIFY(action);
    auto before = w->findChildren<QMenu*>();
    action->trigger();
    QStringList actualLabels;
    for (auto menu : w->findChildren<QMenu*>())
      if (!before.contains(menu)) {
        for (auto item : menu->actions()) {
          actualLabels << item->text();
          item->trigger();
        }
        menu->close();
      }
    QCOMPARE(actualLabels, labels);
    QCOMPARE(launched, commands);
  }
  void idleRefreshKeepsLiveIndicators() {
    auto o = options(false);
    o.filter = 1;
    auto w = std::make_unique<Window>(sample(), o, false);
    qint64 time = 2000;
    auto& engine = w->alarmEngine();
    engine.now = engine.monotonicNow = [&] { return time; };
    for (auto n : w->document().channels()) engine.event(n, {0, 0, 0, 1, "0"});
    w->show();
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Current Alarm History") action->trigger();
    auto history = w->findChild<QDialog*>("historyDialog")->findChild<QPlainTextEdit*>();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    QVERIFY(tree);
    QTest::qWait(300);
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy insertions(tree->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy historyChanges(history, &QPlainTextEdit::textChanged);
    QTest::qWait(1200);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(historyChanges.count(), 0);
    auto n = w->document().channels()[0];
    engine.event(n, {3, 2, 2, 1, "99"});
    // A new visible alarm updates membership without invalidating other rows.
    QTRY_VERIFY(insertions.count() > 0);
    QCOMPARE(tree->model()->rowCount(), 1);
    QCOMPARE(resets.count(), 0);
    QTRY_VERIFY(history->toPlainText().contains("99"));
    MotifButton* runtime = nullptr;
    for (auto widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "runtimeWindow")
        runtime = static_cast<MotifButton*>(widget->findChild<QPushButton*>("runtimeAlarm"));
    QVERIFY(runtime);
    const auto initialColor = runtime->background;
    resets.clear();
    historyChanges.clear();
    time = 3000;
    QTRY_VERIFY_WITH_TIMEOUT(runtime->background != initialColor, 1500);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(historyChanges.count(), 0);
    engine.silenceUntil = 3500;
    auto silence = w->findChild<QCheckBox*>("silenceInterval");
    QVERIFY(silence);
    QTRY_VERIFY_WITH_TIMEOUT(silence->isChecked(), 1500);
    time = 4000;
    QTRY_VERIFY_WITH_TIMEOUT(!silence->isChecked(), 1500);
    QSignalSpy updates(tree->model(), &QAbstractItemModel::dataChanged);
    engine.acknowledge(n);
    QTRY_VERIFY(updates.count() > 0);
    QCOMPARE(tree->model()->index(0, 0).data().toString().trimmed(), QString());
    QCOMPARE(resets.count(), 0);
  }
  void largeGroup() {
    QString text = "GROUP NULL large\n";
    for (int i = 0; i < 10000; ++i)
      text += QString("CHANNEL large pv%1\n").arg(i);
    QElapsedTimer timer;
    timer.start();
    auto w = std::make_unique<Window>(parseConfig(text), options(false), false);
    w->show();
    for (auto node : w->document().channels())
      w->alarmEngine().event(node, {3, 2, 2, 1, "99"});
    QTest::qWait(250);
    auto group = w->findChild<QTreeView*>("groupContents");
    QCOMPARE(group->model()->rowCount(), 10000);
    QCOMPARE(group->model()->index(9999, 2).data().toString(), QString("pv9999"));
    QCOMPARE(w->alarmEngine().state(w->document().root.get()).counts[2], 10000);
    qInfo() << "10000-row window and alarm burst (ms):" << timer.elapsed();
    QVERIFY(timer.elapsed() < 10000);
  }
  void lifecycle() {
    for (int i = 0; i < 20; ++i) {
      auto w = std::make_unique<Window>(sample(), options(false), false);
      w->show();
      w->alarmEngine().event(w->document().channels()[0], {3, 2, 2, 1, "1"});
      QCoreApplication::processEvents();
    }
  }
};
QTEST_MAIN(UiTests)
#include "test_ui.moc"
