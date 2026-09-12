#include "test_compat.h"
#include "ui/window.h"
#include "ui/dialogs.h"
#include "ui/alarm_view.h"
#include "services/log_browser.h"
#include <QtWidgets>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
using namespace alh;
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
    engine.shelve(nodes[1],1,"Analytics screenshot");
    now += 2000;
    for (auto action:w->findChildren<QAction*>()) if(action->text()=="Alarm Analytics...") action->trigger();
    auto dialog=w->findChild<QDialog*>("analyticsDialog"); QVERIFY(dialog);
    auto tabs=dialog->findChild<QTabWidget*>("analyticsTabs"); QCOMPARE(tabs->count(),4);
    auto table=dialog->findChild<QTableView*>("analyticsTable0"); QVERIFY(table);
    QTRY_COMPARE(table->model()->rowCount(),3);
    QCOMPARE(table->model()->index(0,1).data().toString(),QString("5"));
    QCOMPARE(dialog->findChild<QComboBox*>("analyticsRange")->currentIndex(),1);
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
    auto page = dialog->findChild<QTabWidget*>()->widget(0);
    for (auto button : page->findChildren<QPushButton*>())
      if (button->text() == "Edit") button->click();
    QVERIFY(!dialog->findChild<QPushButton*>("testNotification")->isEnabled());
    dialog->findChild<QPushButton*>("saveNotifications")->click();
    QCOMPARE(store.load().subscriptions.first().name, QString("Updated subscription"));
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
    store.save(NotificationSettings{});
  }
  void notificationsUnavailableInEditor() {
    auto w = std::make_unique<Window>(sample(), options(true), false);
    for (auto action : w->findChildren<QAction*>())
      QVERIFY(action->text() != "Notifications...");
  }

  void shelvingWorkflow() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    w->show();
    auto& e = w->alarmEngine();
    const qint64 startTime = QDateTime(QDate(2026, 9, 11), QTime(12, 0)).toMSecsSinceEpoch();
    qint64 time = startTime; e.now = [&] { return time; };
    const auto channels = w->document().channels();
    for (auto n : channels) e.event(n, {3, 2, 2, 1, "99"});
    e.shelve(channels[0], 15, "Individual maintenance");
    QAction *shelve = nullptr, *list = nullptr;
    for (auto a : w->findChildren<QAction*>()) {
      if (a->text() == "Shelve Alarms...") shelve = a;
      if (a->text() == "Shelved Alarms...") list = a;
    }
    QVERIFY(shelve && list); shelve->trigger();
    auto dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    auto reason = dialog->findChild<QLineEdit*>("shelfReason");
    auto duration = dialog->findChild<QComboBox*>("shelfDuration");
    auto custom = dialog->findChild<QSpinBox*>("shelfCustomMinutes");
    auto apply = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    QVERIFY(!apply->isEnabled()); QCOMPARE(duration->currentData().toInt(), 60);
    QVERIFY(dialog->findChild<QLabel*>("shelfScope")->text().contains("2 channels"));
    reason->setText("  "); QVERIFY(!apply->isEnabled());
    reason->setText("Vacuum maintenance"); QVERIFY(apply->isEnabled());
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
    QVERIFY(browser->grab().save(QString(TEST_OUTPUT) + name + "-shelved-list.png"));
    table->selectRow(0); browser->findChild<QPushButton*>("changeShelf")->click();
    dialog = w->findChild<QDialog*>("shelveDialog"); QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLineEdit*>("shelfReason")->text(), QString("Individual maintenance"));
    dialog->findChild<QLineEdit*>("shelfReason")->setText("Extended maintenance");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(e.state(channels[0]).shelf.until, startTime + 3600000);
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
    auto d = sample(); Engine e(d); qint64 time = 1000; e.now = [&] { return time; };
    for (auto n : d.channels()) e.event(n, {});
    auto n = d.channels()[0]; e.event(n, {3, 2, 2, 1, "99"});
    AlarmModel tree(&d, &e, true), group(&d, &e, false); group.setGroup(n->parent);
    e.shelve(n, 1, "check filters");
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
    w->alarmEngine().shelve(old, 60, "Preserve across reload");
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
    w->alarmEngine().shelve(n, 60, "Preserve after failed reload");
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
    // No IOC is needed: observe scheduling with a unique, unavailable output PV.
    qputenv("EPICS_CA_AUTO_ADDR_LIST", "NO");
    qputenv("EPICS_CA_ADDR_LIST", "127.0.0.1:59998");
    qputenv("EPICS_CA_REPEATER_PORT", "59999");
    auto o = options(false);
    o.engine.global = true;
    auto name = "qtalh_ui_heartbeat_" + QString::number(QCoreApplication::applicationPid());
    auto d = parseConfig("GROUP NULL root\n$HEARTBEATPV " + name + " 0.1 1\n");
    auto w = std::make_unique<Window>(d, o, true);
    auto timer = w->findChild<QTimer*>("heartbeatTimer");
    QVERIFY(timer);
    QVERIFY(timer->isActive());
    QCOMPARE(timer->timerType(), Qt::PreciseTimer);
    QSignalSpy beats(timer, &QTimer::timeout);
    QTest::qWait(1150);
    QVERIFY2(beats.size() >= 9,
             qPrintable(QString("Only %1 heartbeat deadlines serviced").arg(beats.size())));
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
    auto w = std::make_unique<Window>(sample(), options(false), false);
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
    requestExit();
    dialog = confirmation();
    QVERIFY(dialog);
    QTest::mouseClick(dialog->button(QMessageBox::Ok), Qt::LeftButton);
    QVERIFY(!runtime->isVisible());
    QVERIFY(!w->isVisible());
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
                         "$GUIDANCE https://example.invalid/help\n$GUIDANCE\nOperator instructions\n$END\n");
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
    QCOMPARE(n->option("GUIDANCE_TEXT"), QString("Operator instructions"));
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
  void manualNoAckCancelsTimer() {
    auto w = std::make_unique<Window>(sample(), options(false), false);
    auto& e = w->alarmEngine();
    qint64 time = 1000;
    e.now = [&] { return time; };
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
    QVERIFY(tree->isExpanded(gamma));
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
    auto view = w->findChild<AlarmView*>("groupContents");
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
    auto view = w->findChild<AlarmView*>("groupContents");
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
    engine.now = [&] { return time; };
    for (auto n : w->document().channels()) engine.event(n, {0, 0, 0, 1, "0"});
    w->show();
    for (auto action : w->findChildren<QAction*>())
      if (action->text() == "Current Alarm History") action->trigger();
    auto history = w->findChild<QDialog*>("historyDialog")->findChild<QPlainTextEdit*>();
    auto tree = w->findChild<QTreeView*>("alarmTree");
    QVERIFY(tree);
    QTest::qWait(300);
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy historyChanges(history, &QPlainTextEdit::textChanged);
    QTest::qWait(1200);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(historyChanges.count(), 0);
    auto n = w->document().channels()[0];
    engine.event(n, {3, 2, 2, 1, "99"});
    QTRY_VERIFY(resets.count() > 0);
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
    engine.acknowledge(n);
    QTRY_VERIFY(resets.count() > 0);
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
