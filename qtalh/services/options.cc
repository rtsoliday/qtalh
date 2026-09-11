// CLI port from ALH file.c. In particular -D affects logging, not CA writes.
#include "options.h"
#include <QDir>
#include <QFileInfo>
namespace alh {
Options parseOptions(const QStringList& args) {
  Options o;
  bool explicitLogDir = false;
  o.configDir = qEnvironmentVariable("ALARMHANDLER", ".");
  o.font = qEnvironmentVariable("ALHMAINFONT");
  auto need = [&](int& i) -> QString {
    if (i + 1 >= args.size())
      throw ParseError("Missing value for " + args[i]);
    return args[++i];
  };
  auto integer = [&](int& i, int min) -> int {
    QString value = need(i);
    bool ok = false;
    int n = value.toInt(&ok);
    if (!ok || n < min)
      throw ParseError("Invalid integer: " + value);
    return n;
  };
  for (int i = 1; i < args.size(); ++i) {
    auto a = args[i];
    if (a == "-c")
      o.editor = true;
    else if (a == "-global")
      o.engine.global = true;
    else if (a == "-S")
      o.engine.passive = true;
    else if (a == "-D")
      o.noLog = true;
    else if (a == "-s")
      o.silent = true;
    else if (a == "-B")
      o.broadcast = true;
    else if (a == "-L")
      o.lock = true;
    else if (a == "-T")
      o.dated = true;
    else if (a == "-xml")
      o.xml = true;
    else if (a == "-debug")
      o.debug = true;
    else if (a == "-desc_field")
      o.engine.description = true;
    else if (a == "-caputackt")
      o.engine.caputAckT = true;
    else if (a == "-mainwindow")
      o.mainWindow = true;
    else if (a == "-noerrorpopup")
      o.noErrorPopup = true;
    else if (a == "-maskcolor")
      o.maskColor = true;
    else if (a == "-help" || a == "--help" || a == "-h")
      o.help = true;
    else if (a == "-v" || a == "-version" || a == "--version")
      o.version = true;
    else if (a == "--validate")
      o.validate = true;
    else if (a == "-a")
      o.alarmFile = need(i);
    else if (a == "-o")
      o.opmodFile = need(i);
    else if (a == "-f")
      o.configDir = need(i);
    else if (a == "-l") {
      o.logDir = need(i);
      explicitLogDir = true;
    } else if (a == "-Lfile")
      o.lockFile = need(i);
    else if (a == "-p")
      o.sound = need(i);
    else if (a == "-P")
      o.printerKey = integer(i, 1);
    else if (a == "-O")
      o.databaseKey = integer(i, 1);
    else if (a == "-m")
      o.maxRecords = integer(i, 0);
    else if (a == "-display" || a == "--display")
      o.display = need(i);
    else if (a == "-geometry")
      o.geometry = need(i);
    else if (a == "-fn" || a == "-font")
      o.font = need(i);
    else if (a == "-filter") {
      auto f = need(i);
      if (f.startsWith('n'))
        o.filter = 0;
      else if (f.startsWith('a'))
        o.filter = 1;
      else if (f.startsWith('u'))
        o.filter = 2;
      else
        throw ParseError("Filter must be no, active, or unack");
    } else if (a == "-platform" || a == "-style")
      need(i); // Consumed later by QApplication.
    else if (a == "--") {
      if (++i < args.size())
        o.config = args[i];
      if (i + 1 < args.size())
        throw ParseError("Only one configuration filename is accepted");
    } else if (a.startsWith('-'))
      throw ParseError("Unsupported option: " + a +
                       " (Xt resource overrides, CDEV and CMLOG are not available)");
    else {
      if (!o.config.isEmpty())
        throw ParseError("Only one configuration filename is accepted");
      o.config = a;
    }
  }
  o.engine.debug = o.debug;
#ifdef Q_OS_WIN
  if (o.printerKey || o.databaseKey)
    throw ParseError("-P and -O require System V queues and are unavailable on Windows");
#endif
  if (!explicitLogDir)
    o.logDir = o.configDir;
  if (o.config.isEmpty() && !o.editor)
    o.config = "ALH-default.alhConfig";
  if (!o.config.isEmpty())
    o.config = QFileInfo(QDir(o.configDir).filePath(o.config)).absoluteFilePath();
  o.alarmFile = QFileInfo(QDir(o.logDir).filePath(o.alarmFile)).absoluteFilePath();
  o.opmodFile = QFileInfo(QDir(o.logDir).filePath(o.opmodFile)).absoluteFilePath();
  return o;
}
QString usage() {
  return R"(Usage: qtalh [OPTIONS] [configfile]
  -c                 Alarm Configuration Tool
  -global            Use IOC ACKS/ACKT fields
  -S                 Passive mode (no CA writes or operator acknowledgement)
  -D                 Disable alarm and operation log writing
  -caputackt         Write configured ACKT settings at startup (global, active)
  -a file -o file    Alarm and operation log filenames
  -f dir -l dir      Configuration and log directories
  -m count           Maximum alarm records (0 means unlimited)
  -T -xml            Dated logs / XML-ish log format
  -L -Lfile file     Master/slave logging lock / alternate lock basename
  -B                 Message broadcast using configuration .MESS files
  -P key -O key      Printer / database System V queue keys (Linux/macOS only)
  -s -p sound        Silent / audio file (formats depend on Qt media backend)
  -filter no|active|unack
  -mainwindow -maskcolor -noerrorpopup -desc_field -debug
  -display display -geometry geometry -fn font
  -help -version     Show help / version without opening a display
  --validate        Validate the configuration without CA connections or GUI
QtALH supports ALH configuration files and Qt 5.15/6 on Linux, macOS and Windows.
)";
}
} // namespace alh
