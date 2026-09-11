// Optional real-display comparison. Only the test IOC is contacted.
#include "ui/window.h"
#include "ui/dialogs.h"
#include <QGuiApplication>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QtTest>
using namespace alh;
struct Child : QProcess {
  ~Child() {
    if (state() != NotRunning) {
      kill();
      waitForFinished();
    }
  }
};
class VisualTests : public QObject {
  Q_OBJECT
private slots:
  void dialogGallery() {
    initializeAppearance();
    QTemporaryDir temp;
    QString output = QString(TEST_OUTPUT) + "/dialogs";
    QVERIFY(QDir().mkpath(output));
    for (bool editor : {false, true}) {
      auto d = parseConfig("GROUP NULL QTALH_REFERENCE\n$GUIDANCE\nOperator guidance for the reference facility.\n"
                           "Check the alarm source before acknowledgement.\n$END\n"
                           "CHANNEL QTALH_REFERENCE qtalh_visual:alarm\n$ALIAS Vacuum pressure\n"
                           "$ACKPV qtalh_visual:ack 1\n$SEVRPV qtalh_visual:severity\n"
                           "$FORCEPV CALC -D--- 1 NE\n$FORCEPV_CALC A+B\n$FORCEPV_CALC_A 0\n"
                           "$FORCEPV_CALC_B 0\n$ALARMCOUNTFILTER 2 10\n");
      Options options;
      options.editor = editor; options.noLog = true; options.silent = true;
      options.noErrorPopup = true; options.broadcast = true; options.mainWindow = true;
      options.config = temp.filePath(editor ? "editor.alhConfig" : "runtime.alhConfig");
      options.configDir = temp.path(); d.filename = options.config;
      saveConfig(d, options.config);
      auto window = std::make_unique<Window>(d, options, false);
      window->show();
      QTest::qWait(50);
      QVERIFY(window->grab().save(output + (editor ? "/qt-editor.png" : "/qt-main.png")));
      if (!editor)
        for (auto widget : QApplication::topLevelWidgets())
          if (widget->objectName() == "runtimeWindow")
            QVERIFY(widget->grab().save(output + "/qt-runtime.png"));
      auto captureModal = [&](const QString& actionName, const QString& file) {
        QAction* action = nullptr;
        for (auto candidate : window->findChildren<QAction*>())
          if (candidate->text().remove('&') == actionName) action = candidate;
        QVERIFY2(action, qPrintable(actionName));
        bool captured = false;
        QTimer::singleShot(100, [&] {
          auto dialog = qobject_cast<QDialog*>(qApp->activeModalWidget());
          if (dialog) {
            captured = dialog->grab().save(output + '/' + file + ".png");
            dialog->reject();
          }
        });
        action->trigger();
        QVERIFY(captured);
      };
      auto capture = [&](const QString& actionName, const QString& file) {
        QAction* action = nullptr;
        for (auto candidate : window->findChildren<QAction*>())
          if (candidate->text().remove('&') == actionName) action = candidate;
        QVERIFY2(action, qPrintable(actionName));
        action->trigger(); QTest::qWait(50);
        bool found = false;
        for (auto dialog : window->findChildren<QDialog*>())
          if (dialog->isVisible()) {
            QVERIFY(dialog->grab().save(output + '/' + file + ".png"));
            dialog->close(); found = true;
          }
        QVERIFY(found);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      };
      capture("Properties Window", editor ? "qt-editor-properties" : "qt-properties");
      if (editor) {
        captureModal("Group...", "qt-insert-group");
        captureModal("Channel...", "qt-insert-channel");
      } else {
        captureModal("About ALH", "qt-about");
        captureModal("Print...", "qt-print");
      }
      if (!editor) {
        capture("Modify Mask Settings...", "qt-mask");
        capture("Force Mask...", "qt-force-mask");
        capture("Force Process Variable...", "qt-force");
        capture("Beep Severity...", "qt-beep");
        auto channel = window->document().channels()[0];
        window->alarmEngine().event(channel, {3, 2, 2, 1, "20"});
        capture("Current Alarm History", "qt-history");
        capture("Display Guidance", "qt-guidance");
        capture("Send Message...", "qt-broadcast");
        capture("Stop Alarm Logging...", "qt-stop-logging");
        capture("Reload Facility...", "qt-reload");
        capture("Alarm Log File", "qt-log");
        capture("Browser for Alarm Log", "qt-alarm-log-browser");
        capture("Browser for Operation Log", "qt-opmod-log-browser");
        window->alarmEngine().setForceDisabled(window->document().root.get(), true);
        QTest::qWait(1100);
        QVERIFY(window->grab().save(output + "/qt-disabled-force-count.png"));
        capture("Exit", "qt-exit"); // Capture and cancel, never exit the test process.
      }
      auto view = window->findChild<QTreeView*>("groupContents");
      view->setCurrentIndex(view->model()->index(0, 2));
      capture("Properties Window", editor ? "qt-editor-channel-properties" : "qt-channel-properties");
      if (!editor) capture("Force Process Variable...", "qt-channel-force");
      QTimer::singleShot(100, [&] {
        auto dialog = qApp->activeModalWidget();
        if (dialog) {
          QVERIFY(dialog->grab().save(output + "/qt-file.png"));
          static_cast<QDialog*>(dialog)->reject();
        }
      });
      chooseFile(window.get(), "Alarm Configuration File", temp.path(), "Configuration (*.alhConfig)");
    }
  }
  void reference() {
    initializeAppearance();
    const QString output = QString(TEST_OUTPUT);
    QDir().mkpath(output);
    QTemporaryDir tmp;
    auto prefix = "qtalh_visual_" + QString::number(QCoreApplication::applicationPid()) + ":";
    int port = 42000 + QCoreApplication::applicationPid() % 10000;
    qputenv("EPICS_CA_AUTO_ADDR_LIST", "NO");
    qputenv("EPICS_CA_ADDR_LIST", ("127.0.0.1:" + QString::number(port)).toLatin1());
    qputenv("EPICS_CA_SERVER_PORT", QByteArray::number(port));
    qputenv("EPICS_CA_REPEATER_PORT", QByteArray::number(port + 1));
    qputenv("EPICS_CAS_INTF_ADDR_LIST", "127.0.0.1");
    qputenv("EPICS_CAS_BEACON_ADDR_LIST", ("127.0.0.1:" + QString::number(port + 1)).toLatin1());
    Child ioc;
    ioc.start(QString(EPICS_TEST_BIN) + "/softIoc",
              {"-m", "P=" + prefix, "-d", QFileInfo("tests/ioc.db").absoluteFilePath()});
    QVERIFY(ioc.waitForStarted());
    auto d =
        parseConfig("GROUP NULL QTALH_REFERENCE\nGROUP QTALH_REFERENCE Vacuum\nCHANNEL Vacuum " +
                    prefix + "alarm\n");
    auto path = tmp.filePath("visual.alhConfig");
    saveConfig(d, path);
    Child legacy;
    legacy.start("../bin/Linux-" + QSysInfo::currentCpuArchitecture() + "/alh",
                 {"-S", "-s", "-mainwindow", "-noerrorpopup", "-a", tmp.filePath("legacy.alhAlarm"),
                  "-o", tmp.filePath("legacy.alhOpmod"), path});
    QVERIFY(legacy.waitForStarted());
    WId id = 0;
    QElapsedTimer timer;
    timer.start();
    while (!id && timer.elapsed() < 15000) {
      QProcess info;
      info.start("xwininfo", {"-name", "Alarm Handler: QTALH_REFERENCE"});
      info.waitForFinished(1000);
      auto match = QRegularExpression("Window id: (0x[0-9a-fA-F]+)")
                       .match(QString::fromLocal8Bit(info.readAllStandardOutput()));
      if (match.hasMatch())
        id = match.captured(1).toULongLong(nullptr, 16);
      QTest::qWait(100);
    }
    QVERIFY2(id, "Legacy main window was not found on the X display");
    ChannelAccess driver;
    driver.prepare(prefix + "alarm");
    QTRY_VERIFY(driver.put(prefix + "alarm", 20));
    QTest::qWait(1000);
    auto screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    auto shot = screen->grabWindow(id);
    QVERIFY(!shot.isNull());
    QVERIFY(shot.save(output + "/motif-reference.png"));
    Options options;
    options.noLog = false;
    options.alarmFile = tmp.filePath("qt.alhAlarm");
    options.opmodFile = tmp.filePath("qt.alhOpmod");
    options.noErrorPopup = true;
    options.engine.passive = true;
    options.silent = true;
    options.mainWindow = true;
    options.config = path;
    d.filename = path;
    auto window = std::make_unique<Window>(std::move(d), options);
    window->show();
    QTRY_COMPARE(window->alarmEngine().state(window->document().channels()[0]).severity, 2);
    QTest::qWait(300);
    QVERIFY(window->grab().save(output + "/qt-reference.png"));
    // Compare ordered transition logs, ignoring wall-clock timestamps and padding.
    for (int value : {0, 6, 20, 0}) {
      QVERIFY(driver.put(prefix + "alarm", value));
      int severity = value >= 10 ? 2 : value >= 5 ? 1 : 0;
      QTRY_COMPARE(window->alarmEngine().state(window->document().channels()[0]).severity,
                   severity);
      QTest::qWait(300);
    }
    auto transitions = [&](const QString& filename) {
      QFile file(filename);
      if (!file.open(QIODevice::ReadOnly))
        return QStringList();
      QStringList result;
      for (auto line : QString::fromLocal8Bit(file.readAll()).split('\n')) {
        auto start = line.indexOf(prefix + "alarm");
        if (start < 0)
          continue;
        auto fields = line.mid(start).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (fields.size() >= 4)
          result << fields[1] + " " + fields[2];
      }
      return result;
    };
    auto legacyTrace = transitions(tmp.filePath("legacy.alhAlarm"));
    auto qtTrace = transitions(tmp.filePath("qt.alhAlarm"));
    QStringList expected{"NO_ALARM NO_ALARM", "HIGH MINOR", "HIHI MAJOR", "NO_ALARM NO_ALARM"};
    QCOMPARE(qtTrace.mid(qMax(0, int(qtTrace.size()) - 4)), expected);
    QCOMPARE(legacyTrace.mid(qMax(0, int(legacyTrace.size()) - 4)), expected);
    QFile trace(output + "/reference-trace.txt");
    QVERIFY(trace.open(QIODevice::WriteOnly));
    trace.write("Fixture: tests/ioc.db; loopback CA only\nInputs: 0, 6, 20, 0\nLegacy:\n" +
                legacyTrace.join('\n').toUtf8() + "\nQt:\n" + qtTrace.join('\n').toUtf8() + "\n");
  }
};
QTEST_MAIN(VisualTests)
#include "test_visual.moc"
