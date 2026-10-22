#include "core/diagnostics.h"
#include "core/phoebus.h"
#include "core/version.h"
#include "services/options.h"
#include "ui/window.h"
#include "ui/dialogs.h"
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QStyleFactory>
#include <QTextStream>
#include <epicsVersion.h>
#include <clocale>
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
      QTextStream(stdout) << "QtALH " QTALH_VERSION " (based on ALH " ALH_BASE_VERSION ") Qt "
                          << qVersion() << " " << EPICS_VERSION_STRING << '\n';
      return 0;
    }
    if (options.validate) {
      auto doc = alh::loadConfig(options.config, options.configDir);
      alh::debugLog(options.debug, "config", QString("validated %1 channels").arg(doc.channels().size()));
      QTextStream(stdout) << "Valid configuration: " << doc.channels().size() << " channels, "
                          << doc.nodes().size() - doc.channels().size() << " groups\n";
      return 0;
    }
    if (!options.toPhoebus.isEmpty() || !options.fromPhoebus.isEmpty()) {
      if (!options.toPhoebus.isEmpty() && !options.fromPhoebus.isEmpty())
        throw alh::ParseError("Choose one of --to-phoebus and --from-phoebus");
      QStringList notes;
      alh::Document doc;
      QString output;
      if (!options.toPhoebus.isEmpty()) {
        doc = alh::loadConfig(options.config, options.configDir);
        alh::savePhoebusXml(doc, output = options.toPhoebus, &notes);
      } else {
        doc = alh::loadPhoebusXml(options.config, &notes);
        alh::saveConfig(doc, output = options.fromPhoebus);
      }
      for (const auto& note : notes)
        QTextStream(stderr) << "qtalh: conversion note: " << note << '\n';
      QTextStream(stdout) << "Wrote " << output << ": " << doc.channels().size() << " channels, "
                          << doc.nodes().size() - doc.channels().size() << " groups\n";
      return 0;
    }
    if (!options.display.isEmpty())
      qputenv("DISPLAY", options.display.toLocal8Bit());
    // Qt only receives its platform switch. Application options (including the
    // runtime-only font and the motif alias) have already been parsed above.
    QByteArray program = args.front().toLocal8Bit(), platformFlag("-platform"),
               platform = options.platform.toLocal8Bit();
    int qtArgc = platform.isEmpty() ? 1 : 3;
    char* qtArgv[] = {program.data(), platformFlag.data(), platform.data(), nullptr};
    qtArgv[qtArgc] = nullptr;
    QApplication app(qtArgc, qtArgv);
    // QApplication adopts the user's locale on Unix. Keep C numeric parsing so
    // FORCEPV values, CALC literals and EPICS settings use '.' decimals, as in
    // legacy ALH and the headless modes above.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setApplicationName("qtalh");
    QApplication::setOrganizationName("EPICS");
    alh::initializeAppearance(options.style);
    const bool modern = options.style.isEmpty() || options.style.compare("modern", Qt::CaseInsensitive) == 0;
    alh::debugLog(options.debug, "appearance", alh::legacyAppearance() ? "style=motif" :
                  modern ? "style=modern" : "style=" + QApplication::style()->objectName());
    alh::Document doc;
    if (options.editor && options.config.isEmpty())
      doc = alh::parseConfig("GROUP NULL NewGroup\n");
    else {
      if (!QFileInfo::exists(options.config)) {
        // ALH opens a file selection box here. Name the missing path (a relative
        // name resolves against -f or ALARMHANDLER) and preselect a file whose
        // name differs only in case, such as Booster.alhconfig.
        const QFileInfo missing(options.config);
        QTextStream(stderr) << "qtalh: Configuration not found: " << missing.absoluteFilePath() << '\n';
        QString initial = missing.dir().exists() ? missing.absolutePath() : options.configDir;
        for (const auto& name : missing.dir().entryList(QDir::Files))
          if (name.compare(missing.fileName(), Qt::CaseInsensitive) == 0)
            initial = missing.dir().filePath(name);
        options.config =
            alh::chooseFile(nullptr, "Alarm Configuration File (not found: " + missing.fileName() + ")",
                            initial, "Alarm configurations (*.alhConfig)");
        if (options.config.isEmpty())
          return 0;
      }
      doc = alh::loadConfig(options.config, options.configDir);
      // The editor saves and reloads the text on every edit; refuse a file
      // that would not reload with the same hierarchy.
      if (options.editor)
        alh::writeCheckedConfig(doc);
    }
    auto window = new alh::Window(std::move(doc), options);
    if (!options.editor && options.broadcast) alh::handleInterrupt(window);
    window->showInitial();
    return app.exec();
  } catch (const std::exception& e) {
    QTextStream(stderr) << "qtalh: " << e.what() << '\n';
    return 1;
  }
}
