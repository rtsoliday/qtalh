// ALH text format port from alConfig.c, with bounded transactional parsing.
// See ../../LICENSE. INCLUDEs are expanded on save, matching legacy ALH.
#include "model.h"
#include <algorithm>
#include "notifications.h"
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
// ConfigText::Edit lines that parseConfig() reads as text kept from a file
// ("text"), or as the bounds of an INCLUDE file ("include" and "line", "end").
// Edit text begins with an "edit" line; no other text has markers.
constexpr const char* EditMarker = "#%qtalh-";
// A node's declaration and a directive as legacy ALH writes them.
QString declaration(const Node& n) {
  return (n.group ? QStringLiteral("GROUP ") : QStringLiteral("CHANNEL ")) +
         (n.parent ? n.parent->name : QStringLiteral("NULL")) + ' ' + n.name +
         (n.group ? QString() : ' ' + n.mask.text());
}
QString directiveText(const Directive& d) {
  return d.key == QLatin1String("GUIDANCE_TEXT") ? "$GUIDANCE\n" + d.value + "\n$END"
                                                  : '$' + d.key + ' ' + d.value;
}
// ALH copies configuration bytes verbatim. Bytes that the locale codec cannot
// decode (for example ISO-8859-1 text under a UTF-8 locale) would become
// U+FFFD and be saved that way, so read such files losslessly as ISO-8859-1.
QString decode(QByteArray bytes, bool& latin1) {
  // ALH reads each line with fgets() and C string functions, so a NUL byte
  // ends that line's text and later lines still load. Do the same; Qt 5's
  // QByteArray conversions would instead drop everything after the first NUL.
  for (int nul = bytes.indexOf('\0'); nul >= 0; nul = bytes.indexOf('\0', nul)) {
    const int end = bytes.indexOf('\n', nul);
    bytes.remove(nul, (end < 0 ? bytes.size() : end) - nul);
  }
  const QString text = QString::fromLocal8Bit(bytes);
  // Only replacements added by the decoder count, not U+FFFD stored as UTF-8.
  if (text.count(QChar::ReplacementCharacter) <= bytes.count("\xEF\xBF\xBD"))
    return text;
  latin1 = true;
  return QString::fromLatin1(bytes);
}
bool forceCalcInput(const QString& name) {
  static const QRegularExpression input("^FORCEPV_CALC_[A-L]$");
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
  // Text to read for INCLUDE files, by absolute path, instead of the disk.
  const QHash<QString, QString>* included = nullptr;
  // The node keeping each $BEEPSEVERITY and $HEARTBEATPV line; null for doc.
  QHash<QString, Node*> lineOwners;
  // Large configurations repeat directive keys and many values (commands,
  // Force PVs, count filters); nodes share one copy of each instead.
  QSet<QString> strings;
  QString intern(const QString& s) { return *strings.insert(s); }
  // The first heartbeat in the facility is the root's, read from a file.
  void setHeartbeat() {
    doc.root->setOption("HEARTBEATPV", heartbeat);
    for (auto& d : doc.root->directives)
      if (d.key == QLatin1String("HEARTBEATPV")) d.sourceValue = d.value;
    // Directive lists grow while reading; large configurations keep many.
    for (auto n : doc.nodes())
      n->directives.squeeze();
  }
  void validate() const {
    if (!doc.root)
      throw ParseError("Configuration has no root group");
    // Destinations and subscriptions may appear in any order; check their references.
    try {
      validateNotifications(configurationNotifications(doc, "configuration", "qtalh@localhost", "sendmail"));
    } catch (const std::exception& e) {
      throw ParseError(QString("Notification directives: ") + QString::fromUtf8(e.what()));
    }
    for (auto n : doc.nodes())
      if (words(n->option("FORCEPV")).value(0) == "CALC" &&
          n->option("FORCEPV_CALC").trimmed().isEmpty())
        throw ParseError("FORCEPV CALC requires a FORCEPV_CALC expression for " + n->name);
  }
  // Reads one file's text and returns the number of groups and channels it
  // declared, including those of its INCLUDE files. `include` is the file's
  // INCLUDE, null for the top-level text. `file` names an included file in
  // diagnostics; it is empty for the top-level text, whose messages keep the
  // plain "Line N" form. With `markers`, the text is ConfigText::Edit text.
  int read(const QString& text, const QString& base, Node* attach = nullptr, int depth = 0,
           const QString& file = QString(), std::shared_ptr<Include> include = nullptr,
           bool markers = false) {
    if (depth > 50)
      throw ParseError("Configuration exceeds 50 nested includes");
    Node* current = attach;
    // ALH starts each file, including INCLUDE files, without a current
    // group/channel: node directives before this file's first GROUP, CHANNEL
    // or INCLUDE must not attach to the INCLUDE's parent group.
    bool declared = false;
    int declarations = 0;
    // Whether the last statement was an INCLUDE. An INCLUDE line that directly
    // follows its group's declaration or directives keeps that place.
    bool afterInclude = false;
    // Comment and blank lines since this file's last statement, which the next
    // statement keeps (Directive::before).
    QStringList pending;
    auto takeBefore = [&] {
      if (pending.isEmpty())
        return QString();
      const auto before = intern(pending.join('\n') + '\n');
      pending.clear();
      return before;
    };
    // Edit text keeps each INCLUDE file between markers; these are the
    // enclosing files' states while reading one.
    struct Region {
      std::shared_ptr<Include> include;
      Node *current, *parent;
      bool declared;
      int declarations, line, depth; // `line`: the INCLUDE's index in parent->includes.
      QStringList pending;
    };
    QVector<Region> regions;
    // A final newline ends the last line rather than starting an empty one.
    const auto lines = text.split('\n');
    const int end = lines.last().isEmpty() ? lines.size() - 1 : lines.size();
    for (int i = 0; i < end; ++i) {
      QString raw = lines[i];
      if (raw.endsWith('\r'))
        raw.chop(1);
      const QString line = raw.trimmed();
      if (markers && raw.startsWith(QLatin1String(EditMarker))) {
        const auto marker = raw.mid(int(qstrlen(EditMarker)));
        if (marker.startsWith(QLatin1String("text "))) {
          pending << marker.mid(5);
        } else if (marker.startsWith(QLatin1String("include "))) {
          const QString lineMarker = QString(EditMarker) + "line ";
          if (i + 1 >= end || !lines[i + 1].startsWith(lineMarker))
            throw ParseError("Include marker without its INCLUDE line");
          auto inc = std::make_shared<Include>();
          inc->path = marker.mid(8);
          inc->line = lines[++i].mid(lineMarker.size());
          auto w = words(inc->line);
          if (w.size() < 3 || w[0] != "INCLUDE")
            throw ParseError("Invalid INCLUDE line: " + inc->line);
          Node* parent = current;
          if (parent && !parent->group)
            parent = parent->parent;
          while (parent && parent->name != w[1])
            parent = parent->parent;
          if (!parent)
            throw ParseError("Parent group not found: " + w[1]);
          inc->before = takeBefore();
          inc->parent = include;
          parent->includes.push_back({inc, int(parent->children.size()),
                                      current == parent && !afterInclude ? parent->directives.size() : -1});
          regions.push_back({include, current, parent, declared, declarations,
                             parent->includes.size() - 1, depth, pending});
          include = inc;
          current = parent;
          declared = false;
          declarations = 0;
          pending.clear();
          if (++depth > 50)
            throw ParseError("Configuration exceeds 50 nested includes");
        } else if (marker == QLatin1String("end")) {
          if (regions.isEmpty())
            throw ParseError("Include end marker without its start");
          include->trailer = takeBefore();
          const auto region = regions.takeLast();
          region.parent->includes[region.line].empty = !declarations;
          include = region.include;
          current = region.parent;
          declared = true;
          afterInclude = true;
          declarations += region.declarations;
          depth = region.depth;
          pending = region.pending;
        } else {
          throw ParseError("Unknown marker: " + raw);
        }
        continue;
      }
      if (line.isEmpty() || line.startsWith('#')) {
        pending << raw;
        continue;
      }
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
        // ALH scans a fixed number of tokens and ignores the rest, including a
        // mask on a GROUP line and anything after a channel's mask.
        if (w.size() < 3)
          fail("Invalid group/channel declaration");
        auto n = std::make_shared<Node>();
        n->group = key == "GROUP";
        n->name = w[2];
        if (!n->group && w.size() >= 4)
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
        // A group may repeat an ancestor's name, as in ALH: parent names resolve
        // to the nearest such ancestor. writeCheckedConfig() guards saves and
        // edits that the text could not record unambiguously.
        int nesting = 0;
        for (Node* p = n->parent; p; p = p->parent)
          ++nesting;
        if (nesting >= 50)
          fail("Group nesting exceeds 50 levels");
        if (n->parent)
          n->parent->children.push_back(n);
        else
          doc.root = n;
        n->before = takeBefore();
        if (raw != declaration(*n))
          n->source = raw;
        n->origin = include;
        ++declarations;
        current = n.get();
        declared = true;
        afterInclude = false;
      } else if (key == "INCLUDE") {
        // As in ALH, tokens after the filename are ignored.
        if (w.size() < 3)
          fail("INCLUDE requires parent and filename");
        Node* parent = findParent(w[1]);
        const QString path = QFileInfo(QDir(base).filePath(w[2])).absoluteFilePath();
        QString text, key = path;
        if (included) {
          const auto found = included->constFind(path);
          if (found == included->cend())
            fail("INCLUDE no longer names its file: " + path);
          if (stack.contains(key))
            fail("Include cycle: " + path);
          text = *found;
        } else {
          const QFileInfo target(path);
          key = target.canonicalFilePath();
          if (stack.contains(key))
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
          // ALH resolves every INCLUDE against the configured directory.
          // A read error rejects the whole configuration so a partial include
          // cannot silently remove alarms during startup or reload.
          QByteArray data;
          if (!readFile(f, data))
            fail("Cannot read include: " + path + ": " + f.errorString());
          text = decode(data, doc.latin1);
        }
        stack.insert(key);
        auto inc = std::make_shared<Include>();
        inc->path = path;
        inc->line = raw;
        inc->before = takeBefore();
        inc->parent = include;
        parent->includes.push_back({inc, int(parent->children.size()),
                                    current == parent && !afterInclude ? parent->directives.size() : -1});
        const int index = parent->includes.size() - 1;
        const int count = read(text, base, parent, depth + 1, path, inc);
        parent->includes[index].empty = !count;
        declarations += count;
        stack.remove(key);
        current = parent;
        declared = true;
        afterInclude = true;
      } else if (key.startsWith('$')) {
        QString name = key.mid(1);
        QString value = line.mid(key.size()).trimmed();
        // Fixed-argument ALH directives ignore trailing comments. Free-form
        // commands, aliases and guidance retain their text. CALC is handled below.
        const bool pvFirst = name == "HEARTBEATPV" || name == "ACKPV" ||
                             name == "FORCEPV" || name == "SEVRPV" ||
                             forceCalcInput(name);
        if (pvFirst || name == "BEEPSEVERITY" || name == "BEEPSEVR" ||
            name == "ALARMCOUNTFILTER" || isNotificationDirective(name)) {
          auto args = words(value);
          // A PV's first token may itself start with '#'. Only subsequent
          // whitespace-separated hashes introduce a trailing comment.
          for (int j = pvFirst ? 1 : 0; j < args.size(); ++j)
            if (args[j].startsWith('#')) {
              value = args.mid(0, j).join(' ');
              break;
            }
        }
        // Keep the line that takes effect, with the comments before the others,
        // after the statement it followed in this file, or at the file's top.
        auto documentLine = [&](const QString& canonical) {
          DocumentLine l{name, value, takeBefore(), raw != canonical ? raw : QString(), include};
          if (lineOwners.contains(name)) {
            auto owner = lineOwners.value(name);
            auto& kept = owner ? owner->documentLines : doc.documentLines;
            for (int k = 0; k < kept.size(); ++k)
              if (kept[k].key == name) {
                l.before = kept[k].before + l.before;
                kept.removeAt(k);
                break;
              }
          }
          Node* owner = declared && current && current->origin == include ? current : nullptr;
          if (owner) {
            l.position = owner->directives.size();
            owner->documentLines.push_back(l);
          } else {
            doc.documentLines.push_back(l);
          }
          lineOwners[name] = owner;
        };
        if (name == "BEEPSEVERITY") {
          // The last line takes effect.
          doc.beepSeverity = severity(value);
          value = severityName(doc.beepSeverity);
          documentLine("$BEEPSEVERITY " + value);
          continue;
        }
        if (name == "HEARTBEATPV") {
          // ALH reads the PV, interval and value and ignores further tokens.
          const auto args = words(value).mid(0, 3);
          if (args.isEmpty())
            fail("Invalid heartbeat arguments");
          value = args.join(' ');
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
          if (heartbeat.isEmpty()) {
            heartbeat = value;
            documentLine("$HEARTBEATPV " + value);
          }
          continue;
        }
        if (!current || !declared)
          fail("Directive requires a group/channel");
        static const QSet<QString> supported = {
            "HEARTBEATPV",      "ACKPV",   "FORCEPV", "FORCEPV_CALC", "SEVRPV",
            "GUIDANCE",         "ALIAS",   "COMMAND", "SEVRCOMMAND",  "STATCOMMAND",
            "ALARMCOUNTFILTER", "BEEPSEVR", "NOTIFY", "NOTIFY_EMAIL", "NOTIFY_WEBHOOK"};
        if (!supported.contains(name) &&
            !forceCalcInput(name))
          fail("Unsupported directive " + key);
        if (forceCalcInput(name) && name.back() > QLatin1Char('F'))
          doc.warnings << (file.isEmpty() ? QString() : file + ": ") +
              QString("Line %1: $%2 is a QtALH extension; legacy ALH ignores CALC inputs G "
                      "through L and evaluates them as 0").arg(i + 1).arg(name);
        if (isNotificationDirective(name)) {
          try {
            checkNotificationDirective(name, value);
          } catch (const ParseError& e) {
            fail(QString::fromStdString(e.what()));
          }
        }
        // The statement as written; a guidance block spans several lines.
        QString sourceText = raw;
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
            body.push_back(lines[i].endsWith('\r') ? lines[i].chopped(1) : lines[i]);
          }
          if (!ended) {
            i = start; // Report the $GUIDANCE line, not one past the end.
            fail("Unterminated guidance");
          }
          QStringList block;
          for (int k = start; k <= i; ++k)
            block << (lines[k].endsWith('\r') ? lines[k].chopped(1) : lines[k]);
          // ALH appends every inline block to the node's guidance list. An
          // empty block changes nothing; keep it as text, as comments are kept.
          if (body.isEmpty()) {
            pending += block;
            continue;
          }
          name = "GUIDANCE_TEXT";
          value = body.join('\n');
          sourceText = block.join('\n');
          bool appended = false;
          for (auto& directive : current->directives)
            if (directive.key == name) {
              // One block written in place of several.
              directive.value += '\n' + value;
              directive.source.clear();
              directive.sourceValue = QString("");
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
          // Omitted arguments use its one-count, one-second defaults, and tokens
          // after the seconds (for example a copied "-----" mask) are ignored.
          bool a = true, b = true;
          int count = 1, seconds = 1;
          if (!args.isEmpty())
            count = args[0].toInt(&a, 0);
          if (args.size() > 1)
            seconds = args[1].toInt(&b, 0);
          if (!a || !b || count < -1 || count > 1000000 || seconds < 0)
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
        name = intern(name);
        value = intern(value);
        // A replaced directive is written no more; its comments precede the
        // replacement, which follows the node's other directives.
        auto store = [&](bool replace) {
          Directive d{name, value, takeBefore(), QString(), value};
          if (sourceText != directiveText(d))
            d.source = sourceText;
          if (replace)
            for (int k = 0; k < current->directives.size(); ++k)
              if (current->directives[k].key == name) {
                d.before = current->directives[k].before + d.before;
                current->directives.removeAt(k);
                break;
              }
          current->directives.push_back(d);
          afterInclude = false;
        };
        // These ALH setters replace earlier values; ALIAS/COMMAND and the
        // other first-wins directives retain their existing precedence.
        if (name == "SEVRPV") {
          // '-' is ALH's unset sentinel, so a later real PV may replace it.
          // Once a real PV is selected, subsequent directives are ignored.
          if (current->option(name, "-") == "-")
            store(true);
        } else if (name == "BEEPSEVR" || name == "ACKPV" || name == "FORCEPV_CALC")
          store(true);
        else
          store(false);
      } else
        fail("Unknown statement");
    }
    if (!regions.isEmpty())
      throw ParseError("Include start marker without its end");
    (include ? include->trailer : doc.trailer) = takeBefore();
    return declarations;
  }
};
void writeNode(QTextStream& out, const Node& n) {
  out << declaration(n) << '\n';
  for (const auto& d : n.directives)
    out << directiveText(d) << '\n';
  // Group and channel ordering follows the legacy writer.
  for (const auto& child : n.children)
    if (!child->group)
      writeNode(out, *child);
  for (const auto& child : n.children)
    if (child->group)
      writeNode(out, *child);
}
// Source, Listing and Edit text, and the files that Save writes. Nodes are
// written in document order with the comments and text kept from reading
// (see Directive). A line is written as read only while it would still read
// back the same way in its new context; otherwise it is written as legacy ALH
// would write it.
class SourceWriter {
public:
  // `files`: write each INCLUDE file separately (writeFiles()).
  SourceWriter(const Document& doc, ConfigText form, bool files = false)
      : doc(doc), form(form), separate(files) {
    // INCLUDE files to write: those declaring a node, with the files that
    // include them, and empty ones whose group remains. A file whose groups
    // and channels were all removed is no longer included.
    for (auto n : doc.nodes()) {
      for (auto i = n->origin.get(); i; i = i->parent.get())
        present.insert(i);
      for (const auto& l : n->includes)
        attach.insert(l.include.get(), n);
    }
    for (bool added = true; added;) {
      added = false;
      for (auto n : doc.nodes())
        for (const auto& l : n->includes)
          if (l.empty && !present.contains(l.include.get()) &&
              (!l.include->parent || present.contains(l.include->parent.get()))) {
            present.insert(l.include.get());
            added = true;
          }
    }
    for (const auto& l : doc.documentLines)
      if (!l.origin || present.contains(l.origin.get()))
        documentKeys.insert(l.key);
    for (auto n : doc.nodes())
      for (const auto& l : n->documentLines)
        documentKeys.insert(l.key);
  }
  QString text() {
    QString out = form == ConfigText::Edit ? QString(EditMarker) + "edit\n" : QString();
    writeFile(nullptr, out);
    return out;
  }
  QVector<ConfigFile> files(const QString& path) {
    QVector<ConfigFile> result{{path, QString()}};
    writeFile(nullptr, result[0].text);
    for (int i = 0; i < queue.size(); ++i) {
      current = attach.value(queue[i]);
      declared = false;
      result.push_back({queue[i]->path, QString()});
      writeFile(queue[i], result.back().text);
    }
    return result;
  }

private:
  const Document& doc;
  ConfigText form;
  bool separate;
  QSet<const Include*> present, written;
  QSet<QString> documentKeys; // $BEEPSEVERITY/$HEARTBEATPV keys written as document lines.
  // How a parser reading the output would resolve the next declaration, and
  // whether it would accept a directive.
  Node* current = nullptr;
  bool declared = false;
  int depth = 0; // Listing indentation, in INCLUDE levels.
  QVector<const Include*> queue; // INCLUDE files still to write separately.
  QHash<const Include*, Node*> attach; // Their INCLUDE lines' parent groups.

  void line(QString& out, const QString& text) const {
    const QString indent(form == ConfigText::Listing ? 3 * depth : 0, ' ');
    for (const auto& l : text.split('\n'))
      out += (l.isEmpty() ? l : indent + l) + '\n';
  }
  // Comment and blank lines kept from reading, and empty guidance blocks,
  // which are written only where a directive may appear.
  void lines(QString& out, const QString& text) const {
    if (text.isEmpty())
      return;
    QString kept = text;
    kept.chop(1);
    for (const auto& l : kept.split('\n')) {
      const auto trimmed = l.trimmed();
      if (!declared && !trimmed.isEmpty() && !trimmed.startsWith('#'))
        continue;
      if (form == ConfigText::Edit)
        out += (l.startsWith(QLatin1String(EditMarker)) ? QString(EditMarker) + "text " + l : l) + '\n';
      else
        line(out, l);
    }
  }
  Node* findParent(const QString& name) const {
    Node* p = current;
    if (p && !p->group)
      p = p->parent;
    while (p && p->name != name)
      p = p->parent;
    return p;
  }
  bool readsBack(const Node& n) const {
    auto w = words(n.source);
    for (int j = 3; j < w.size(); ++j)
      if (w[j].startsWith('#')) {
        w = w.mid(0, j);
        break;
      }
    if (w.size() < 3 || w[0] != (n.group ? "GROUP" : "CHANNEL") || w[2] != n.name)
      return false;
    if (!n.group && Mask::parse(w.value(3)).bits != n.mask.bits)
      return false;
    if (w[1] == "NULL")
      return n.group && (current ? (current->group ? current : current->parent) : nullptr) == n.parent;
    return n.parent && findParent(w[1]) == n.parent;
  }
  void writeDocumentLine(QString& out, const DocumentLine& l) const {
    const bool beep = l.key == QLatin1String("BEEPSEVERITY");
    const QString value = beep ? severityName(doc.beepSeverity)
                               : doc.root ? doc.root->option("HEARTBEATPV") : QString();
    if (value.isEmpty())
      return;
    lines(out, l.before);
    line(out, value == l.value && !l.source.isEmpty() ? l.source : '$' + l.key + ' ' + value);
  }
  void writeFile(const Include* file, QString& out) {
    if (!file && doc.beepSeverity > 1 && !documentKeys.contains("BEEPSEVERITY"))
      line(out, "$BEEPSEVERITY " + severityName(doc.beepSeverity));
    for (const auto& l : doc.documentLines)
      if (l.origin.get() == file)
        writeDocumentLine(out, l);
    if (doc.root)
      visit(*doc.root, nullptr, file, out);
    lines(out, file ? file->trailer : doc.trailer);
  }
  void visit(Node& n, const Include* inherited, const Include* file, QString& out) {
    const Include* own = n.origin ? n.origin.get() : inherited;
    if (own == file) {
      writeNode(n, file, out);
    } else {
      // A node of an INCLUDE file read from this one: write its INCLUDE once.
      for (auto k = own; k; k = k->parent.get())
        if (k->parent.get() == file) {
          writeInclude(k, out);
          return;
        }
    }
    // Other children may belong to this file. Empty INCLUDE files keep their
    // place among them; the others are written with their first node.
    const int count = int(n.children.size());
    QVector<int> order;
    for (int i = 0; i < count; ++i) order << i;
    // After a subgroup that repeats this group's or an ancestor's name, this
    // group can no longer be named: write its channels first, as legacy ALH
    // does. A file read cannot place them after such a subgroup.
    if (shadowed(n))
      std::stable_sort(order.begin(), order.end(),
                       [&](int a, int b) { return !n.children[a]->group && n.children[b]->group; });
    for (int i = 0; i <= count; ++i) {
      for (const auto& l : n.includes)
        if (l.empty && l.include->parent.get() == file && (l.directives < 0 || inlined()) &&
            (l.children == i || (i == count && l.children > count)))
          writeInclude(l.include.get(), out);
      if (i < count)
        visit(*n.children[order[i]], own, file, out);
    }
  }
  static bool shadowed(const Node& n) {
    for (const auto& child : n.children)
      if (child->group)
        for (auto p = &n; p; p = p->parent)
          if (p->name == child->name) return true;
    return false;
  }
  // INCLUDE files are expanded in place: a directive after one would no
  // longer reach its group, so INCLUDE lines do not keep their place among
  // the directives.
  bool inlined() const { return form == ConfigText::Source && !separate; }
  void writeNode(const Node& n, const Include* file, QString& out) {
    lines(out, n.before);
    line(out, !n.source.isEmpty() && readsBack(n) ? n.source : declaration(n));
    current = const_cast<Node*>(&n);
    declared = true;
    // $BEEPSEVERITY, $HEARTBEATPV and INCLUDE lines read among the directives.
    QVector<const DocumentLine*> placed;
    for (const auto& l : n.documentLines)
      placed << &l;
    QVector<const Include*> included;
    QVector<int> positions;
    if (!inlined())
      for (const auto& l : n.includes)
        if (l.directives >= 0 && l.include->parent.get() == file && present.contains(l.include.get())) {
          included << l.include.get();
          positions << l.directives;
        }
    // Each goes before the first directive read after it, so directives
    // added by editing stay with the group's earlier ones.
    int count = 0;
    QSet<const void*> done;
    auto place = [&](bool last) {
      for (auto l : placed)
        if ((l->position <= count || last) && !done.contains(l)) {
          done.insert(l);
          writeDocumentLine(out, *l);
        }
      for (int i = 0; i < included.size(); ++i)
        if ((positions[i] <= count || last) && !done.contains(included[i])) {
          done.insert(included[i]);
          writeInclude(included[i], out);
        }
    };
    for (const auto& d : n.directives) {
      if (!n.parent && d.key == QLatin1String("HEARTBEATPV") && documentKeys.contains(d.key))
        continue;
      if (!d.sourceValue.isNull())
        place(false);
      lines(out, d.before);
      line(out, !d.source.isEmpty() && d.value == d.sourceValue ? d.source : directiveText(d));
      ++count;
    }
    place(true);
  }
  void writeInclude(const Include* k, QString& out) {
    if (written.contains(k))
      return;
    written.insert(k);
    lines(out, k->before);
    const auto w = words(k->line);
    Node* const parent = attach.value(k, findParent(w.value(1)));
    // A Properties rename changes the owning group, not its saved INCLUDE
    // text. Keep that text only while its parent token still names the owner.
    const auto includeLine = parent && findParent(w.value(1)) != parent
                                 ? "INCLUDE " + parent->name + ' ' + w.value(2)
                                 : k->line;
    switch (form) {
    case ConfigText::Source:
      // Expanded in place, read on from the included text.
      if (separate) {
        line(out, includeLine);
        queue << k;
        current = parent;
        declared = true;
      } else {
        writeFile(k, out);
      }
      return;
    case ConfigText::Listing:
      line(out, includeLine);
      current = parent;
      declared = false;
      ++depth;
      writeFile(k, out);
      --depth;
      break;
    case ConfigText::Edit:
      out += QString(EditMarker) + "include " + k->path + '\n' + EditMarker + "line " + includeLine + '\n';
      current = parent;
      declared = false;
      writeFile(k, out);
      out += QString(EditMarker) + "end\n";
      break;
    case ConfigText::Canonical:
      break;
    }
    // After an INCLUDE file, the parser continues in its parent group.
    current = parent;
    declared = true;
  }
};
} // namespace
Document parseConfig(const QString& text, const QString& base) {
  Parser p;
  // Only Edit text, which names itself on its first line, has markers.
  const bool markers = text.startsWith(QString(EditMarker) + "edit\n");
  p.read(markers ? text.mid(int(qstrlen(EditMarker)) + 5) : text,
         base.isEmpty() ? QDir::currentPath() : base, nullptr, 0, QString(), nullptr, markers);
  p.validate();
  p.setHeartbeat();
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
  p.setHeartbeat();
  return p.doc;
}
QString writeConfig(const Document& doc, ConfigText form) {
  if (form != ConfigText::Canonical)
    return SourceWriter(doc, form).text();
  QString text;
  QTextStream out(&text);
  if (doc.beepSeverity > 1)
    out << "$BEEPSEVERITY " << severityName(doc.beepSeverity) << '\n';
  if (doc.root)
    writeNode(out, *doc.root);
  return text;
}
namespace {
// Nodes in writeConfig() order: each node's channels, then its groups.
void writerOrder(Node* n, QVector<Node*>& out) {
  out.push_back(n);
  for (bool groups : {false, true})
    for (const auto& child : n->children)
      if (child->group == groups)
        writerOrder(child.get(), out);
}
bool repeatsAncestorName(const Node& n) {
  if (n.group)
    for (auto p = n.parent; p; p = p->parent)
      if (p->name == n.name)
        return true;
  for (const auto& child : n.children)
    if (repeatsAncestorName(*child))
      return true;
  return false;
}
int depth(const Node* n) {
  int result = 0;
  for (auto p = n->parent; p; p = p->parent)
    ++result;
  return result;
}
// Which node `reloaded` places differently from `doc`, or empty.
QString moved(const Document& doc, const Document& reloaded) {
  QVector<Node*> expected, actual;
  writerOrder(doc.root.get(), expected);
  writerOrder(reloaded.root.get(), actual);
  for (int i = 0; i < expected.size(); ++i)
    if (i >= actual.size() || depth(expected[i]) != depth(actual[i])) {
      const auto parent = expected[i]->parent ? expected[i]->parent->name : QString();
      return QString("%1 %2 would move under another group named %3 when saved; "
                     "rename one of the %3 groups")
          .arg(expected[i]->group ? "Group" : "Channel", expected[i]->name, parent);
    }
  return {};
}
// Why `reread`, read from saved text, is not `doc`, or empty. The hierarchy is
// compared too: legacy ALH's text cannot show it when a group repeats an
// ancestor's name.
QString differences(const Document& doc, const Document& reread) {
  const auto change = moved(doc, reread);
  if (!change.isEmpty())
    return change;
  if (writeConfig(reread) != writeConfig(doc))
    return "the saved text would read back as a different configuration";
  return {};
}
} // namespace
QString hierarchyChange(const Document& doc, const QString& text) {
  // Parent names resolve to the nearest matching ancestor. Once a group repeats
  // an ancestor's name, a later line meant for the outer group can resolve to
  // the inner one, so reload the text and require the same hierarchy.
  if (!doc.root || !repeatsAncestorName(*doc.root))
    return {};
  Document reloaded;
  try {
    reloaded = parseConfig(text);
  } catch (const ParseError&) {
    return {}; // Callers that reload the text report other errors themselves.
  }
  return moved(doc, reloaded);
}
QString writeCheckedConfig(const Document& doc, ConfigText form) {
  const auto text = writeConfig(doc, form);
  const auto change = hierarchyChange(doc, text);
  if (!change.isEmpty())
    throw ParseError(change);
  return text;
}
QVector<ConfigFile> writeConfigFiles(const Document& doc, const QString& path,
                                     const QString& configDir, bool check) {
  const auto absolute = QFileInfo(path).absoluteFilePath();
  const auto files = SourceWriter(doc, ConfigText::Source, true).files(absolute);
  // A file included more than once is written once. Different paths through
  // symbolic links still name one file; their copies must agree before any
  // writes, even when an unchanged copy would otherwise be left on disk.
  auto fileKey = [](const QString& path) {
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : canonical;
  };
  QHash<QString, QString> included;
  QHash<QString, QString> physicalFiles{{fileKey(absolute), files.front().text}};
  QVector<ConfigFile> result{files.front()};
  for (int i = 1; i < files.size(); ++i) {
    // The readback parser needs every INCLUDE spelling, including aliases
    // that do not require another physical write.
    included.insert(files[i].path, files[i].text);
    const auto key = fileKey(files[i].path);
    const auto found = physicalFiles.constFind(key);
    if (found == physicalFiles.cend()) {
      physicalFiles.insert(key, files[i].text);
      result << files[i];
    } else if (*found != files[i].text) {
      throw ParseError(files[i].path + " is included more than once, and its copies differ");
    }
  }
  if (!check)
    return result;
  // Read the files back as loadConfig() would, before anything is written.
  Parser p;
  p.included = &included;
  p.stack.insert(absolute);
  try {
    p.read(result.front().text, configDir.isEmpty() ? QFileInfo(absolute).absolutePath() : configDir);
    p.validate();
  } catch (const ParseError& e) {
    throw ParseError("Cannot save " + absolute + ": " + QString::fromStdString(e.what()));
  }
  p.setHeartbeat();
  const auto problem = differences(doc, p.doc);
  if (!problem.isEmpty())
    throw ParseError("Cannot save " + absolute + ": " + problem);
  return result;
}
void saveConfig(const Document& doc, const QString& path) {
  const auto text = writeConfig(doc, ConfigText::Source);
  Document reread;
  try {
    reread = parseConfig(text);
  } catch (const ParseError& e) {
    throw ParseError("Cannot save " + path + ": " + QString::fromStdString(e.what()));
  }
  const auto problem = differences(doc, reread);
  if (!problem.isEmpty())
    throw ParseError(problem);
  saveConfigText(doc, path, text);
}
void saveConfigText(const Document& doc, const QString& path, const QString& text) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    throw ParseError(path + ": " + f.errorString());
  // Edits may add text that ISO-8859-1 cannot represent; keep it, not '?'.
  auto data = doc.latin1 ? text.toLatin1() : text.toLocal8Bit();
  if (doc.latin1 && QString::fromLatin1(data) != text)
    data = text.toLocal8Bit();
  if (f.write(data) != data.size() || !f.commit())
    throw ParseError(path + ": " + f.errorString());
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
