// ALH text format port from alConfig.c, with bounded transactional parsing.
// See ../../LICENSE. INCLUDEs are expanded on save, matching legacy ALH.
#include "model.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <cmath>
#include <cstdio>
#include <limits>
#include <postfix.h>
namespace alh {
namespace {
QStringList words(const QString& s) {
  // Compile once: every configuration line is split into words.
  static const QRegularExpression whitespace("\\s+");
  return s.split(whitespace, Qt::SkipEmptyParts);
}
// ALH copies configuration bytes verbatim. Bytes that the locale codec cannot
// decode (for example ISO-8859-1 text under a UTF-8 locale) would become
// U+FFFD and be saved that way, so read such files losslessly as ISO-8859-1.
QString decode(const QByteArray& bytes, bool& latin1) {
  const QString text = QString::fromLocal8Bit(bytes);
  // Only replacements added by the decoder count, not U+FFFD stored as UTF-8.
  if (text.count(QChar::ReplacementCharacter) <= bytes.count("\xEF\xBF\xBD"))
    return text;
  latin1 = true;
  return QString::fromLatin1(bytes);
}
bool forceCalcInput(const QString& name) {
  static const QRegularExpression input("^FORCEPV_CALC_[A-F]$");
  return input.match(name).hasMatch();
}
// QFile drops a read error that follows a short read, so one readAll() can
// return a valid prefix with no error. Read until no data remains; a failed
// read then sets QFile::error(). ALH omits ferror() and accepts the prefix.
bool readFile(QFile& f, QByteArray& data) {
  data = f.readAll();
  for (QByteArray more; !(more = f.readAll()).isEmpty();)
    data += more;
  return f.error() == QFileDevice::NoError;
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
  // `file` names an included file in diagnostics; it is empty for the
  // top-level text, whose messages keep the plain "Line N" form.
  void read(const QString& text, const QString& base, Node* attach = nullptr, int depth = 0,
            const QString& file = QString()) {
    if (depth > 50)
      throw ParseError("Configuration exceeds 50 nested includes");
    Node* current = attach;
    // ALH starts each file, including INCLUDE files, without a current
    // group/channel: node directives before this file's first GROUP, CHANNEL
    // or INCLUDE must not attach to the INCLUDE's parent group.
    bool declared = false;
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
      const QString line = lines[i].trimmed();
      if (line.isEmpty() || line.startsWith('#'))
        continue;
      auto w = words(line);
      const QString key = w[0];
      // Handle declaration comments without stripping hashes from names.
      // Directive comments are handled separately according to argument type.
      if (key == "GROUP" || key == "CHANNEL" || key == "INCLUDE")
        for (int j = 3; j < w.size(); ++j)
          if (w[j].startsWith('#')) {
            w = w.mid(0, j);
            break;
          }
      auto fail = [&](const QString& msg) {
        // Name the include so its line is not read as the including file's.
        throw ParseError((file.isEmpty() ? QString() : file + ": ") +
                         QString("Line %1: %2 [%3]").arg(i + 1).arg(msg, line));
      };
      auto severity = [&](const QString& s) {
        try {
          return severityValue(s);
        } catch (const ParseError& e) {
          fail(QString::fromStdString(e.what()));
        }
        return 0;
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
          if (current) {
            // ALH resolves subsequent NULL parents against the current group,
            // both in the top-level file and in INCLUDE files, including after
            // a channel or nested include. Only the first group is the root.
            n->parent = current->group ? current : current->parent;
          }
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
        declared = true;
      } else if (key == "INCLUDE") {
        if (w.size() != 3)
          fail("INCLUDE requires parent and filename");
        Node* parent = findParent(w[1]);
        const QString path = QFileInfo(QDir(base).filePath(w[2])).absoluteFilePath();
        const QFileInfo target(path);
        const QString canonical = target.canonicalFilePath();
        if (stack.contains(canonical))
          fail("Include cycle: " + path);
        // Check before opening: reading a device such as /dev/zero never ends,
        // and opening a FIFO waits for a writer, at startup and on reload. A
        // symbolic link to a regular file is accepted; a missing file is an
        // open error below. The top-level path may still be a pipe (qtalh <(...)).
        if (target.exists() && !target.isFile())
          fail("Include is not a regular file: " + path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
          fail("Cannot open include: " + path);
        stack.insert(canonical);
        // ALH resolves every INCLUDE against the configured directory.
        // A read error rejects the whole configuration so a partial include
        // cannot silently remove alarms during startup or reload.
        QByteArray text;
        if (!readFile(f, text))
          fail("Cannot read include: " + path + ": " + f.errorString());
        read(decode(text, doc.latin1), base, parent, depth + 1, path);
        stack.remove(canonical);
        current = parent;
        declared = true;
      } else if (key.startsWith('$')) {
        QString name = key.mid(1);
        QString value = line.mid(key.size()).trimmed();
        // Fixed-argument ALH directives ignore trailing comments. Free-form
        // commands, aliases and guidance retain their text. CALC is handled below.
        const bool pvFirst = name == "HEARTBEATPV" || name == "ACKPV" ||
                             name == "FORCEPV" || name == "SEVRPV" ||
                             forceCalcInput(name);
        if (pvFirst || name == "BEEPSEVERITY" || name == "BEEPSEVR" ||
            name == "ALARMCOUNTFILTER") {
          auto args = words(value);
          // A PV's first token may itself start with '#'. Only subsequent
          // whitespace-separated hashes introduce a trailing comment.
          for (int j = pvFirst ? 1 : 0; j < args.size(); ++j)
            if (args[j].startsWith('#')) {
              value = args.mid(0, j).join(' ');
              break;
            }
        }
        if (name == "BEEPSEVERITY") {
          doc.beepSeverity = severity(value);
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
        if (!current || !declared)
          fail("Directive requires a group/channel");
        const QSet<QString> supported = {
            "HEARTBEATPV",      "ACKPV",   "FORCEPV", "FORCEPV_CALC", "SEVRPV",
            "GUIDANCE",         "ALIAS",   "COMMAND", "SEVRCOMMAND",  "STATCOMMAND",
            "ALARMCOUNTFILTER", "BEEPSEVR"};
        if (!supported.contains(name) &&
            !forceCalcInput(name))
          fail("Unsupported directive " + key);
        if (name == "GUIDANCE" && value.isEmpty()) {
          QStringList body;
          bool ended = false;
          const int start = i;
          while (++i < lines.size()) {
            // ALH recognizes the terminator by prefix, including trailing comments.
            // An exact match can swallow subsequent channel declarations.
            if (lines[i].trimmed().startsWith("$END", Qt::CaseInsensitive)) {
              ended = true;
              break;
            }
            // ALH also ends the block, reporting an invalid end, at any other
            // line whose first character after spaces and tabs is '$'. Reject it
            // rather than swallow subsequent declarations into guidance text.
            static const QRegularExpression invalidEnd("^[ \\t]*\\$");
            if (lines[i].contains(invalidEnd))
              fail("Guidance must end with $END, not " + lines[i].trimmed());
            body.push_back(lines[i]);
          }
          if (!ended) {
            i = start; // Report the $GUIDANCE line, not one past the end.
            fail("Unterminated guidance");
          }
          // ALH appends every inline block to the node's guidance list.
          if (body.isEmpty())
            continue;
          name = "GUIDANCE_TEXT";
          value = body.join('\n');
          bool appended = false;
          for (auto& directive : current->directives)
            if (directive.key == name) {
              directive.value += '\n' + value;
              appended = true;
              break;
            }
          if (appended)
            continue;
        } else if (value.isEmpty() && name != "ALARMCOUNTFILTER")
          fail("Missing directive value");
        const auto args = words(value);
        if (name == "SEVRPV" ||
            forceCalcInput(name)) {
          // ALH reads one token for these directives and ignores the remainder.
          // Normalize before storage so runtime, editor and save/reload never
          // treat trailing arguments as part of a PV name (or numeric constant).
          value = args[0];
        }
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
          severity(value);
        if (name == "FORCEPV") {
          // alConfig.c reads the numeric tail with sscanf: a failed force
          // conversion leaves force=1/reset=0 and stops reading; an invalid
          // reset defaults to zero. Normalize once so runtime and editor use
          // the same effective values, without guessing a different order.
          double forced = 1, reset = 0;
          int consumed = 0;
          const auto tail = args.mid(2).join(' ').toLocal8Bit();
          double parsed = 0;
          const int count = std::sscanf(tail.constData(), "%lf%n", &parsed, &consumed);
          // Intentionally fix ALH's pre-existing nine-character reset-token
          // truncation bug: read the complete value and retain full precision
          // when saving. Save the shortest exact form so common values (0.3,
          // 1e-05) still fit that field; legacy ALH may misread longer values,
          // for example by truncating exponents. Reproducing that bug is not a
          // compatibility goal.
          const auto resetText = count == 1 ? tail.mid(consumed).trimmed().split(' ').value(0)
                                           : QByteArray();
          if (count >= 1)
            forced = parsed;
          const bool ne = resetText.startsWith("NE") || resetText.startsWith("ne");
          if (!resetText.isEmpty() && !ne && std::sscanf(resetText.constData(), "%lf", &reset) != 1)
            reset = 0;
          // Preserve legacy defaults and numeric-prefix parsing, but reject
          // nonfinite results, including overflow. NaN resets can otherwise
          // leave a force mask applied indefinitely. The editor applies the
          // same finite-value requirement to its separate numeric fields.
          if (!std::isfinite(forced))
            fail("FORCEPV force value must be finite");
          if (!ne && !std::isfinite(reset))
            fail("FORCEPV reset value must be finite or NE");
          value = args[0] + " " + Mask::parse(args.value(1)).text() + " " +
                  QString::number(forced, 'g', QLocale::FloatingPointShortest) + " " +
                  (ne ? QString("NE") : QString::number(reset, 'g', QLocale::FloatingPointShortest));
        }
        if (name == "FORCEPV_CALC") {
          auto valid = [](const QString& expression) {
            QByteArray encoded = expression.toLocal8Bit(),
                       compiled(INFIX_TO_POSTFIX_SIZE(encoded.size() + 1), 0);
            short error = 0;
            return postfix(encoded.constData(), compiled.data(), &error) == 0;
          };
          // Intentionally fix ALH's pre-existing single-token CALC parsing:
          // accept and preserve complete expressions containing whitespace,
          // including on save, even though legacy ALH reads only the first token.
          // Accept trailing hash comments while retaining CALC's # (not-equal)
          // operator. Prefer a valid full expression, then the longest valid
          // expression before a whitespace-separated hash.
          static const QRegularExpression comment("\\s+#");
          while (!valid(value)) {
            const int start = value.lastIndexOf(comment);
            if (start < 0)
              fail("Invalid CALC expression");
            value = value.left(start).trimmed();
          }
        }
        if (name == "ALARMCOUNTFILTER") {
          // ALH's %i conversions accept decimal, octal and hexadecimal integers.
          // Omitted arguments use its one-count, one-second defaults.
          bool a = true, b = true;
          int count = 1, seconds = 1;
          if (!args.isEmpty())
            count = args[0].toInt(&a, 0);
          if (args.size() > 1)
            seconds = args[1].toInt(&b, 0);
          if (args.size() > 2 || !a || !b || count < -1 || count > 1000000 || seconds < 0)
            fail("Invalid count filter");
          value = QString("%1 %2").arg(count).arg(seconds);
        }
        if (name == "SEVRCOMMAND" || name == "STATCOMMAND") {
          if (args.size() < 2)
            fail("Command requires trigger and command text");
          if (name == "SEVRCOMMAND") {
            QString trigger = args[0];
            if (!trigger.startsWith("UP_") && !trigger.startsWith("DOWN_"))
              fail("Severity trigger must start UP_ or DOWN_");
            trigger = trigger.mid(trigger.startsWith("DOWN_") ? 5 : 3);
            // Possible inherited ALH bug, deferred: DOWN_ALARM is accepted
            // here but cannot match the dispatcher, which only implements
            // UP_ALARM. Reject it with guidance to use DOWN_NO_ALARM or
            // DOWN_ANY in a later validation change; retain acceptance now.
            if (trigger != "ANY" && trigger != "ALARM") {
              const auto canonical = severityName(severity(trigger));
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
        // These ALH setters replace earlier values; ALIAS/COMMAND and the
        // other first-wins directives retain their existing precedence.
        if (name == "SEVRPV") {
          // '-' is ALH's unset sentinel, so a later real PV may replace it.
          // Once a real PV is selected, subsequent directives are ignored.
          if (current->option(name, "-") == "-")
            current->setOption(name, value);
        } else if (name == "BEEPSEVR" || name == "ACKPV" || name == "FORCEPV_CALC")
          current->setOption(name, value);
        else
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
  // Reject a read error instead of accepting a valid prefix as a smaller
  // configuration; callers keep the running document on ParseError.
  QByteArray text;
  if (!readFile(f, text))
    throw ParseError("Cannot read " + path + ": " + f.errorString());
  p.read(decode(text, p.doc.latin1),
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
  const auto text = writeConfig(doc);
  // Edits may add text that ISO-8859-1 cannot represent; keep it, not '?'.
  auto data = doc.latin1 ? text.toLatin1() : text.toLocal8Bit();
  if (doc.latin1 && QString::fromLatin1(data) != text)
    data = text.toLocal8Bit();
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
