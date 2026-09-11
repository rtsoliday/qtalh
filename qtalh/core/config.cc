// ALH text format port from alConfig.c, with bounded transactional parsing.
// See ../../LICENSE. INCLUDEs are expanded on save, matching legacy ALH.
#include "model.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <cmath>
#include <limits>
#include <postfix.h>
namespace alh {
namespace {
QStringList words(const QString& s) {
  return s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
}
class Parser {
public:
  Document doc;
  QSet<QString> stack;
  QString heartbeat;
  void validate() const {
    if (!doc.root)
      throw ParseError("Configuration has no root group");
    for (auto n : doc.nodes())
      if (words(n->option("FORCEPV")).value(0) == "CALC" &&
          n->option("FORCEPV_CALC").trimmed().isEmpty())
        throw ParseError("FORCEPV CALC requires a FORCEPV_CALC expression for " + n->name);
  }
  void read(const QString& text, const QString& base, Node* attach = nullptr, int depth = 0) {
    if (depth > 50)
      throw ParseError("Configuration exceeds 50 nested includes");
    Node* current = attach;
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
      const QString line = lines[i].trimmed();
      if (line.isEmpty() || line.startsWith('#'))
        continue;
      auto w = words(line);
      const QString key = w[0];
      auto fail = [&](const QString& msg) {
        throw ParseError(QString("Line %1: %2 [%3]").arg(i + 1).arg(msg, line));
      };
      auto findParent = [&](const QString& name) -> Node* {
        Node* p = current;
        if (p && !p->group)
          p = p->parent;
        while (p && p->name != name)
          p = p->parent;
        if (!p)
          fail("Parent group not found: " + name);
        return p;
      };
      if (key == "GROUP" || key == "CHANNEL") {
        if (w.size() < (key == "GROUP" ? 3 : 3) || w.size() > (key == "GROUP" ? 3 : 4))
          fail("Invalid group/channel declaration");
        auto n = std::make_shared<Node>();
        n->group = key == "GROUP";
        n->name = w[2];
        if (!n->group && w.size() == 4)
          n->mask = Mask::parse(w[3]);
        if (w[1] == "NULL") {
          if (!n->group)
            fail("A channel needs a parent group");
          if (attach)
            n->parent = attach;
          else if (doc.root)
            fail("Multiple root groups");
        } else
          n->parent = findParent(w[1]);
        if (n->group && n->name == "NULL")
          fail("NULL is reserved for the root's parent");
        int nesting = 0;
        for (Node* p = n->parent; p; p = p->parent) {
          // Parent references are names, so a repeated ancestor name cannot
          // distinguish an intended sibling from a child when saving/editing.
          if (n->group && n->name == p->name)
            fail("Group name duplicates an ancestor: " + n->name);
          ++nesting;
        }
        if (nesting >= 50)
          fail("Group nesting exceeds 50 levels");
        if (n->parent)
          n->parent->children.push_back(n);
        else
          doc.root = n;
        current = n.get();
      } else if (key == "INCLUDE") {
        if (w.size() != 3)
          fail("INCLUDE requires parent and filename");
        Node* parent = findParent(w[1]);
        const QString path = QFileInfo(QDir(base).filePath(w[2])).absoluteFilePath();
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (stack.contains(canonical))
          fail("Include cycle: " + path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
          fail("Cannot open include: " + path);
        stack.insert(canonical);
        // ALH resolves every INCLUDE against the configured directory.
        read(QString::fromLocal8Bit(f.readAll()), base, parent, depth + 1);
        stack.remove(canonical);
        current = parent;
      } else if (key.startsWith('$')) {
        QString name = key.mid(1);
        QString value = line.mid(key.size()).trimmed();
        if (name == "BEEPSEVERITY") {
          doc.beepSeverity = severityValue(value);
          continue;
        }
        if (name == "HEARTBEATPV") {
          const auto args = words(value);
          if (args.isEmpty() || args.size() > 3)
            fail("Invalid heartbeat arguments");
          bool ok = true;
          if (args.size() > 2)
            args[2].toShort(&ok);
          if (!ok)
            fail("Invalid heartbeat value");
          if (args.size() > 1) {
            double v = args[1].toDouble(&ok);
            if (!ok || !std::isfinite(v) || v <= 0 || v > std::numeric_limits<int>::max() / 1000.0)
              fail("Heartbeat interval must be positive and at most 2147483.647 seconds");
          }
          // Legacy ALH uses the first heartbeat in the facility, including
          // directives encountered inside INCLUDE files.
          if (heartbeat.isEmpty())
            heartbeat = value;
          continue;
        }
        if (!current)
          fail("Directive requires a group/channel");
        const QSet<QString> supported = {
            "HEARTBEATPV",      "ACKPV",   "FORCEPV", "FORCEPV_CALC", "SEVRPV",
            "GUIDANCE",         "ALIAS",   "COMMAND", "SEVRCOMMAND",  "STATCOMMAND",
            "ALARMCOUNTFILTER", "BEEPSEVR"};
        if (!supported.contains(name) &&
            !QRegularExpression("^FORCEPV_CALC_[A-F]$").match(name).hasMatch())
          fail("Unsupported directive " + key);
        if (name == "GUIDANCE" && value.isEmpty()) {
          QStringList body;
          bool ended = false;
          while (++i < lines.size()) {
            if (lines[i].trimmed().compare("$END", Qt::CaseInsensitive) == 0) {
              ended = true;
              break;
            }
            body.push_back(lines[i]);
          }
          if (!ended)
            fail("Unterminated guidance");
          name = "GUIDANCE_TEXT";
          value = body.join('\n');
        } else if (value.isEmpty())
          fail("Missing directive value");
        const auto args = words(value);
        if (name == "ACKPV" || name == "ALARMCOUNTFILTER" || name == "STATCOMMAND")
          if (current->group)
            fail(name + " applies to channels");
        if (name == "ACKPV") {
          bool ok = false;
          if (args.size() == 2)
            args[1].toShort(&ok);
          if (!ok)
            fail("ACKPV requires a PV and short integer");
        }
        if (name == "BEEPSEVR")
          severityValue(value);
        if (name == "FORCEPV") {
          if (args.size() < 2 || args.size() > 4)
            fail("FORCEPV needs PV, mask, optional force/reset values");
          Mask::parse(args[1]);
          bool ok = true;
          if (args.size() > 2)
            args[2].toDouble(&ok);
          if (!ok)
            fail("Invalid force value");
          if (args.size() > 3 && args[3] != "NE")
            args[3].toDouble(&ok);
          if (!ok)
            fail("Invalid reset value");
        }
        if (name == "FORCEPV_CALC") {
          QByteArray encoded = value.toLocal8Bit(),
                     compiled(INFIX_TO_POSTFIX_SIZE(encoded.size() + 1), 0);
          short error = 0;
          if (postfix(encoded.constData(), compiled.data(), &error))
            fail("Invalid CALC expression");
        }
        if (name == "ALARMCOUNTFILTER") {
          bool a = false, b = false;
          int count = 0, seconds = 0;
          if (args.size() == 2) {
            count = args[0].toInt(&a);
            seconds = args[1].toInt(&b);
          }
          if (!a || !b || count < -1 || count > 1000000 || seconds < 0)
            fail("Invalid count filter");
        }
        if (name == "SEVRCOMMAND" || name == "STATCOMMAND") {
          if (args.size() < 2)
            fail("Command requires trigger and command text");
          if (name == "SEVRCOMMAND") {
            QString trigger = args[0];
            if (!trigger.startsWith("UP_") && !trigger.startsWith("DOWN_"))
              fail("Severity trigger must start UP_ or DOWN_");
            trigger = trigger.mid(trigger.startsWith("DOWN_") ? 5 : 3);
            if (trigger != "ANY" && trigger != "ALARM") {
              const auto canonical = severityName(severityValue(trigger));
              value = (args[0].startsWith("DOWN_") ? "DOWN_" : "UP_") + canonical +
                      value.mid(args[0].size());
            }
          } else {
            bool found = false;
            for (int n = 0; n < 25; ++n)
              found |= statusName(n) == args[0];
            if (!found)
              fail("Unknown status command trigger");
          }
        }
        current->directives.push_back({name, value});
      } else
        fail("Unknown statement");
    }
  }
};
void writeNode(QTextStream& out, const Node& n) {
  out << (n.group ? "GROUP " : "CHANNEL ") << (n.parent ? n.parent->name : QString("NULL")) << ' '
      << n.name;
  if (!n.group)
    out << ' ' << n.mask.text();
  out << '\n';
  for (const auto& d : n.directives) {
    if (d.key == "GUIDANCE_TEXT")
      out << "$GUIDANCE\n" << d.value << "\n$END\n";
    else
      out << '$' << d.key << ' ' << d.value << '\n';
  }
  // Group and channel ordering follows the legacy writer.
  for (const auto& child : n.children)
    if (!child->group)
      writeNode(out, *child);
  for (const auto& child : n.children)
    if (child->group)
      writeNode(out, *child);
}
} // namespace
Document parseConfig(const QString& text, const QString& base) {
  Parser p;
  p.read(text, base.isEmpty() ? QDir::currentPath() : base);
  p.validate();
  p.doc.root->setOption("HEARTBEATPV", p.heartbeat);
  return p.doc;
}
Document loadConfig(const QString& path, const QString& configDir) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    throw ParseError("Cannot open " + path + ": " + f.errorString());
  Parser p;
  p.stack.insert(QFileInfo(path).canonicalFilePath());
  p.read(QString::fromLocal8Bit(f.readAll()),
         configDir.isEmpty() ? QFileInfo(path).absolutePath() : configDir);
  p.validate();
  p.doc.filename = QFileInfo(path).absoluteFilePath();
  p.doc.root->setOption("HEARTBEATPV", p.heartbeat);
  return p.doc;
}
QString writeConfig(const Document& doc) {
  QString text;
  QTextStream out(&text);
  if (doc.beepSeverity > 1)
    out << "$BEEPSEVERITY " << severityName(doc.beepSeverity) << '\n';
  if (doc.root)
    writeNode(out, *doc.root);
  return text;
}
void saveConfig(const Document& doc, const QString& path) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    throw ParseError(f.errorString());
  auto data = writeConfig(doc).toLocal8Bit();
  if (f.write(data) != data.size() || !f.commit())
    throw ParseError(f.errorString());
}
QString writeReport(const Document& doc) {
  QString result;
  for (auto n : doc.nodes()) {
    int depth = 0;
    for (auto p = n->parent; p; p = p->parent)
      ++depth;
    result += QString(depth * 2, ' ') + (n->group ? "+ " : "- ") + n->name +
              (n->group ? QString() : " " + n->mask.text()) + '\n';
  }
  return result;
}
} // namespace alh
