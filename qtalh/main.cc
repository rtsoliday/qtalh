#include "core/diagnostics.h"
#include "services/options.h"
#include "ui/window.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QFileDialog>
#include <QStyleFactory>
#include <QTextStream>
#include <epicsVersion.h>
int main(int argc, char** argv) {
  try {
    QStringList args;
    for (int i = 0; i < argc; ++i)
      args << QString::fromLocal8Bit(argv[i]);
    auto options = alh::parseOptions(args);
    alh::debugLog(options.debug, "startup", QString("mode=%1 config=%2 global=%3 passive=%4")
        .arg(options.editor ? "editor" : "runtime", options.config)
        .arg(options.engine.global).arg(options.engine.passive));
    if (options.help) {
      QTextStream(stdout) << alh::usage();
      return 0;
    }
    if (options.version) {
      QTextStream(stdout) << "QtALH (ALH 1.2.35) Qt " << qVersion() << " " << EPICS_VERSION_STRING
                          << '\n';
      return 0;
    }
    if (options.validate) {
      auto doc = alh::loadConfig(options.config, options.configDir);
      alh::debugLog(options.debug, "config", QString("validated %1 channels").arg(doc.channels().size()));
      QTextStream(stdout) << "Valid configuration: " << doc.channels().size() << " channels, "
                          << doc.nodes().size() - doc.channels().size() << " groups\n";
      return 0;
    }
    if (!options.display.isEmpty())
      qputenv("DISPLAY", options.display.toLocal8Bit());
    QApplication app(argc, argv);
    QApplication::setApplicationName("qtalh");
    QApplication::setOrganizationName("EPICS");
    alh::initializeAppearance();
    int styleIndex = args.indexOf("-style");
    if (styleIndex >= 0 && styleIndex + 1 < args.size())
      QApplication::setStyle(args[styleIndex + 1]);
    alh::Document doc;
    if (options.editor && options.config.isEmpty())
      doc = alh::parseConfig("GROUP NULL NewGroup\n");
    else {
      if (!QFileInfo::exists(options.config)) {
        options.config =
            alh::chooseFile(nullptr, "Alarm Configuration File", options.configDir,
                                         "Alarm configurations (*.alhConfig)");
        if (options.config.isEmpty())
          return 0;
      }
      doc = alh::loadConfig(options.config, options.configDir);
    }
    auto window = new alh::Window(std::move(doc), options);
    window->showInitial();
    return app.exec();
  } catch (const std::exception& e) {
    QTextStream(stderr) << "qtalh: " << e.what() << '\n';
    return 1;
  }
}
