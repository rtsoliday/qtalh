#pragma once
#include "core/engine.h"
namespace alh {
struct Options {
  EngineOptions engine;
  bool editor = false, mainWindow = false, silent = false, noLog = false, dated = false,
       xml = false;
  bool lock = false, broadcast = false, noErrorPopup = false, maskColor = false, debug = false;
  bool help = false, version = false, validate = false;
  // Enable this runtime's saved notification subscriptions at startup.
  bool notifications = false;
  // Shelving stays out of the menus unless USE_SHELVING=1: its shared,
  // attributed change workflow is not implemented yet.
  bool shelving = false;
  // An empty lockFile follows config; -Lfile remains an explicit override.
  QString config, configDir, logDir = ".", alarmFile = "ALH-default.alhAlarm",
                             opmodFile = "ALH-default.alhOpmod", lockFile, sound, font, geometry;
  QString display, platform, style;
  // Conversions that exit without a GUI: the configuration (ALH, or Phoebus XML
  // for fromPhoebus) is written to this output file.
  QString toPhoebus, fromPhoebus;
  int filter = 0, maxRecords = 2000, printerKey = 0, databaseKey = 0;
};
Options parseOptions(const QStringList&);
QString usage();
} // namespace alh
