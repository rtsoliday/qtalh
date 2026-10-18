// Phoebus alarm configuration XML import/export. The element order and
// defaults follow Phoebus's XmlModelReader/XmlModelWriter: <pv> holds
// description, enabled, latching, annunciating, delay, count and filter, then
// guidance/display/command/automated_action entries with <title>/<details>.
#include "phoebus.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <limits>
namespace alh {
namespace {
QByteArray readXmlFile(QFile& file) {
  // A positive short read does not prove EOF. QFile can return a valid XML
  // prefix while dropping the following read error; probe again before
  // accepting the configuration, as the ALH text loader does.
  QByteArray bytes = file.readAll();
  for (QByteArray more; !(more = file.readAll()).isEmpty();)
    bytes += more;
  if (file.error() != QFileDevice::NoError)
    throw ParseError("Cannot read " + file.fileName() + ": " + file.errorString());
  return bytes;
}
QString path(const Node* n) {
  QStringList parts;
  for (; n; n = n->parent) parts.prepend(n->name);
  return '/' + parts.join('/');
}
// Groups notes by kind so a large configuration yields a short report.
class Notes {
public:
  void add(const QString& kind, const Node* n) { add(kind, path(n)); }
  void add(const QString& kind, const QString& where) {
    auto& examples = entries[kind];
    if (!order.contains(kind)) order << kind;
    ++counts[kind];
    if (examples.size() < 3) examples << where;
  }
  void write(QStringList* out) const {
    if (!out) return;
    for (const auto& kind : order) {
      const int count = counts.value(kind);
      *out << QString("%1 (%2): %3%4").arg(kind).arg(count)
                  .arg(entries.value(kind).join(", "), count > 3 ? ", …" : "");
    }
  }
private:
  QStringList order;
  QHash<QString, QStringList> entries;
  QHash<QString, int> counts;
};
QStringList commandEntries(const QString& command) {
  // ALH's related-process syntax: one command, or !label!command! pairs.
  return command.split(QRegularExpression("[!\\n]"), Qt::SkipEmptyParts);
}

// ---- Export --------------------------------------------------------------
void titleDetails(QXmlStreamWriter& xml, const QString& element, const QString& title,
                  const QString& details) {
  xml.writeStartElement(element);
  xml.writeTextElement("title", title);
  xml.writeTextElement("details", details);
  xml.writeEndElement();
}
void writeItem(QXmlStreamWriter& xml, const Node& n, Notes& notes) {
  if (n.group && !n.option("ALIAS").isEmpty())
    notes.add("Group aliases have no Phoebus equivalent; group names retained", &n);
  const auto text = n.option("GUIDANCE_TEXT");
  if (!text.isEmpty()) titleDetails(xml, "guidance", "Guidance", text);
  int urls = 0;
  for (const auto& d : n.directives)
    if (d.key == "GUIDANCE")
      titleDetails(xml, "display", ++urls == 1 ? QString("Guidance") : QString("Guidance %1").arg(urls),
                   d.value);
  const auto command = n.option("COMMAND");
  if (!command.isEmpty()) {
    auto entries = commandEntries(command);
    auto strip = [&](QString value) {
      value = value.trimmed();
      if (value.startsWith("MASTER_ONLY")) {
        notes.add("MASTER_ONLY dropped from related-process commands", &n);
        value = value.mid(11).trimmed();
      }
      return value;
    };
    if (entries.size() == 1)
      titleDetails(xml, "command", "Related process", strip(entries.front()));
    else if (entries.size() % 2)
      notes.add("Malformed related-process menu not converted", &n);
    else
      for (int i = 0; i + 1 < entries.size(); i += 2)
        titleDetails(xml, "command", entries[i].trimmed(), strip(entries[i + 1]));
  }
  static const QList<QPair<QString, QString>> unsupported = {
      {"SEVRPV", "SEVRPV (group severity output) has no Phoebus equivalent"},
      {"ACKPV", "ACKPV has no Phoebus equivalent"},
      {"SEVRCOMMAND", "SEVRCOMMAND has no Phoebus equivalent"},
      {"STATCOMMAND", "STATCOMMAND has no Phoebus equivalent"},
      {"FORCEPV", "FORCEPV (and its CALC inputs) not converted; consider a Phoebus enabling filter"},
      {"BEEPSEVR", "BEEPSEVR has no Phoebus equivalent"}};
  for (const auto& item : unsupported)
    if (!n.option(item.first).isEmpty() && n.option(item.first) != "-") notes.add(item.second, &n);
  if (!n.parent && !n.option("HEARTBEATPV").isEmpty())
    notes.add("HEARTBEATPV has no Phoebus equivalent", &n);
  if (!n.parent && (!text.isEmpty() || urls || !command.isEmpty()))
    notes.add("Facility guidance and commands are written, but Phoebus does not read them at <config>", &n);
}
void writeNode(QXmlStreamWriter& xml, const Node& n, Notes& notes) {
  if (!n.parent) {
    xml.writeStartElement("config");
    xml.writeAttribute("name", n.name);
  } else if (n.group) {
    xml.writeStartElement("component");
    xml.writeAttribute("name", n.name);
  } else {
    xml.writeStartElement("pv");
    xml.writeAttribute("name", n.name);
    const auto alias = n.option("ALIAS");
    if (!alias.isEmpty()) xml.writeTextElement("description", alias);
    // Cancel and Disable both stop the alarm; Phoebus has only "enabled".
    xml.writeTextElement("enabled", n.mask[Cancel] || n.mask[Disable] ? "false" : "true");
    // ALH's NoAckT: transient alarms clear without acknowledgement.
    xml.writeTextElement("latching", n.mask[AckT] ? "false" : "true");
    xml.writeTextElement("annunciating", "false");
    if (n.mask[Ack]) notes.add("NoAck (A) mask has no Phoebus equivalent", &n);
    if (n.mask[Log]) notes.add("NoLog (L) mask has no Phoebus equivalent", &n);
    const auto filter = n.option("ALARMCOUNTFILTER").split(' ', Qt::SkipEmptyParts);
    if (filter.size() == 2 && filter[1].toInt() > 0) {
      const int count = filter[0].toInt();
      xml.writeTextElement("delay", filter[1]);
      if (count > 0) xml.writeTextElement("count", QString::number(count));
      if (count >= 0)
        notes.add("ALH count filters also delay clearing; Phoebus delays only the alarm", &n);
    }
  }
  writeItem(xml, n, notes);
  // Phoebus reads PVs before components at each level; keep ALH's order.
  for (const auto& child : n.children)
    if (!child->group) writeNode(xml, *child, notes);
  for (const auto& child : n.children)
    if (child->group) writeNode(xml, *child, notes);
  xml.writeEndElement();
}

// ---- Import --------------------------------------------------------------
struct Element {
  QString name, text;
  QHash<QString, QString> attributes;
  QVector<Element> children;
  QString child(const QString& tag) const {
    for (const auto& c : children)
      if (c.name == tag) return c.text;
    return {};
  }
};
constexpr auto XInclude = "http://www.w3.org/2001/XInclude";
Element readElement(QXmlStreamReader& xml, const QString& base, QSet<QString>& stack, int depth,
                    int nesting = 0);
Element readRoot(const QByteArray& bytes, const QString& base, QSet<QString>& stack, int depth) {
  if (depth > 20) throw ParseError("Phoebus XML exceeds 20 nested includes");
  QXmlStreamReader xml(bytes);
  while (!xml.atEnd() && !xml.isStartElement()) xml.readNext();
  if (xml.hasError() || !xml.isStartElement())
    throw ParseError("Not a Phoebus alarm configuration: " + xml.errorString());
  auto root = readElement(xml, base, stack, depth);
  // Consume the entire document: a valid first element must not hide trailing
  // declarations, broken comments, or a second configuration.
  while (!xml.atEnd()) xml.readNext();
  if (xml.hasError())
    throw ParseError(QString("Phoebus XML line %1: %2").arg(xml.lineNumber()).arg(xml.errorString()));
  return root;
}
Element readElement(QXmlStreamReader& xml, const QString& base, QSet<QString>& stack, int depth,
                    int nesting) {
  if (nesting > 100) throw ParseError("Phoebus XML nesting exceeds 100 levels");
  Element e;
  e.name = xml.name().toString();
  for (const auto& attribute : xml.attributes())
    e.attributes[attribute.name().toString()] = attribute.value().toString();
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEndElement()) break;
    if (xml.isCharacters()) e.text += xml.text();
    if (!xml.isStartElement()) continue;
    if (xml.namespaceUri() == QLatin1String(XInclude) && xml.name() == QLatin1String("include")) {
      // Phoebus configurations commonly include one <component> per file.
      const auto href = xml.attributes().value("href").toString();
      const auto pointer = xml.attributes().value("xpointer").toString();
      const auto parse = xml.attributes().value("parse").toString();
      if (!parse.isEmpty() && parse != "xml")
        throw ParseError("Unsupported Phoebus include parse mode: " + parse);
      xml.skipCurrentElement();
      const auto file = QFileInfo(QDir(base).filePath(href));
      const auto canonical = file.canonicalFilePath();
      if (href.isEmpty() || canonical.isEmpty())
        throw ParseError("Cannot open Phoebus include: " + href);
      if (stack.contains(canonical)) throw ParseError("Phoebus include cycle: " + href);
      // As for ALH INCLUDE, do not open FIFOs or read unbounded devices.
      // QFileInfo follows symbolic links, so links to regular files still work.
      if (!file.isFile())
        throw ParseError("Phoebus include is not a regular file: " + href);
      QFile f(canonical);
      if (!f.open(QIODevice::ReadOnly)) throw ParseError("Cannot open Phoebus include: " + href);
      stack.insert(canonical);
      auto included = readRoot(readXmlFile(f), file.absolutePath(), stack, depth + 1);
      if (!pointer.isEmpty()) {
        // Phoebus uses XInclude shorthand ID pointers to select a component
        // from an included <config>. Never silently ignore an unsupported or
        // unresolved selector, which would remove its alarms from the import.
        const Element* selected = nullptr;
        std::function<void(const Element&)> find = [&](const Element& candidate) {
          if (candidate.attributes.value("id") == pointer) {
            if (selected) throw ParseError("Ambiguous Phoebus include xpointer: " + pointer);
            selected = &candidate;
          }
          for (const auto& child : candidate.children) find(child);
        };
        find(included);
        if (!selected) throw ParseError("Cannot resolve Phoebus include xpointer: " + pointer);
        // Copy before replacing the owning tree.
        auto chosen = *selected;
        included = std::move(chosen);
      }
      if (included.name != "component" && included.name != "pv")
        throw ParseError("Phoebus include must select a <component> or <pv>: " + href);
      e.children << std::move(included);
      stack.remove(canonical);
      continue;
    }
    e.children << readElement(xml, base, stack, depth, nesting + 1);
  }
  e.text = e.text.trimmed();
  return e;
}
QString token(QString name, const QString& kind, Notes& notes, const QString& where) {
  // ALH names are whitespace-free tokens.
  const auto original = name;
  name.replace(QRegularExpression("\\s+"), "_");
  if (name.isEmpty()) name = "Unnamed";
  if (name != original) notes.add(kind + " names with spaces renamed", where);
  return name;
}
void addTitleDetails(const Element& e, Node& n, Notes& notes) {
  QStringList guidance;
  int count = 0;
  for (const auto& c : e.children) count += c.name == "guidance";
  for (const auto& c : e.children) {
    if (c.name == "guidance") {
      const auto details = c.child("details");
      guidance << (count > 1 && !c.child("title").isEmpty() ? c.child("title") + ":\n" + details : details);
    } else if (c.name == "display") {
      const auto details = c.child("details");
      // Web guidance opens like ALH's guidance URL; display files need Phoebus.
      if (!details.contains('\n') && !details.contains('\r') &&
          details.contains(QRegularExpression("^(https?|file):", QRegularExpression::CaseInsensitiveOption)))
        n.directives.push_back({"GUIDANCE", details});
      else {
        guidance << "Display " + c.child("title") + ": " + details;
        notes.add("Phoebus displays kept as guidance text", path(&n));
      }
    }
  }
  QStringList commands;
  for (const auto& c : e.children)
    if (c.name == "command") {
      const auto details = c.child("details");
      if (details.isEmpty()) {
        notes.add("Empty commands not converted", &n);
        continue;
      }
      auto title = c.child("title");
      if (title.isEmpty()) title = "Related process";
      // ALH splits COMMAND on ! and newlines. Keep commands that cannot be
      // represented as guidance, rather than changing their shell semantics
      // or letting their text become ALH declarations during the reparse.
      if (details.contains('!') || details.contains('\n') || details.contains('\r')) {
        guidance << "Command " + title + ":\n" + details;
        notes.add("Commands containing ! or line breaks kept as guidance text", &n);
        continue;
      }
      if (title.contains('!') || title.contains('\n') || title.contains('\r')) {
        title.replace('!', ' ').replace('\n', ' ').replace('\r', ' ');
        notes.add("Command titles containing ! or line breaks adjusted", &n);
      }
      commands << title << details;
    }
  if (commands.size() == 2) n.directives.push_back({"COMMAND", commands[1]});
  else if (!commands.isEmpty())
    n.directives.push_back({"COMMAND", '!' + commands.join('!') + '!'});
  if (!guidance.isEmpty()) {
    auto text = guidance.join("\n\n");
    // ALH ends a guidance block at any line whose first non-blank character is $
    // and reads the following lines as configuration; QtALH also skips Unicode
    // whitespace before $END. Prefix such lines so they stay guidance in both.
    static const QRegularExpression dollar("^(\\s*)\\$", QRegularExpression::MultilineOption |
                                                          QRegularExpression::UseUnicodePropertiesOption);
    if (text.contains(dollar)) {
      text.replace(dollar, "\\1\\$");
      notes.add("Guidance lines starting with $ written as \\$", path(&n));
    }
    n.directives.push_back({"GUIDANCE_TEXT", text});
  }
  for (const auto& c : e.children)
    if (c.name == "automated_action")
      notes.add("Automated actions not converted; use Setup → Notifications", path(&n));
}
void addChildren(const Element& e, Node& parent, Notes& notes) {
  for (const auto& c : e.children) {
    if (c.name != "pv" && c.name != "component") continue;
    auto n = std::make_shared<Node>();
    n->parent = &parent;
    n->group = c.name == "component";
    const auto where = path(&parent) + '/' + c.attributes.value("name");
    if (c.attributes.value("name").isEmpty())
      throw ParseError(QString("Phoebus %1 without a name at %2").arg(c.name, path(&parent)));
    n->name = token(c.attributes.value("name"), n->group ? "Component" : "PV", notes, where);
    if (n->group) {
      // ALH refers to parents by name, so a group cannot repeat an ancestor's name.
      auto repeats = [&](const QString& name) {
        if (name == "NULL") return true;
        for (Node* p = &parent; p; p = p->parent)
          if (p->name == name) return true;
        return false;
      };
      if (repeats(n->name)) {
        const auto base = n->name;
        for (int i = 2; repeats(n->name); ++i) n->name = base + '_' + QString::number(i);
        notes.add("Components repeating an ancestor's name renamed", where);
      }
    } else {
      const auto enabled = c.child("enabled").trimmed();
      if (!enabled.isEmpty() && enabled.compare("true", Qt::CaseInsensitive) != 0) {
        n->mask[Disable] = true;
        if (enabled.compare("false", Qt::CaseInsensitive) != 0)
          notes.add("Disable-until dates imported as Disable; shelve instead", where);
      }
      if (c.child("latching").compare("false", Qt::CaseInsensitive) == 0) n->mask[AckT] = true;
      const auto description = c.child("description").simplified();
      if (!description.isEmpty()) n->directives.push_back({"ALIAS", description});
      auto integer = [&](const QString& field, int maximum) {
        const auto text = c.child(field);
        if (text.isEmpty()) return 0;
        bool ok = false;
        const int value = text.toInt(&ok);
        if (!ok || value < 0 || value > maximum)
          throw ParseError("Invalid Phoebus " + field + " at " + where + ": " + text);
        return value;
      };
      const int delay = integer("delay", std::numeric_limits<int>::max());
      const int count = integer("count", 1000000);
      if (delay > 0)
        n->directives.push_back({"ALARMCOUNTFILTER", QString("%1 %2").arg(count > 0 ? count : -1).arg(delay)});
      else if (count > 0)
        notes.add("Alarm counts without a delay not converted", where);
      if (delay > 0 && count > 0)
        notes.add("ALH count filters also delay clearing; Phoebus delays only the alarm", where);
      if (!c.child("filter").isEmpty())
        notes.add("Enabling filters not converted; consider a FORCEPV CALC", where);
      if (c.child("annunciating").compare("true", Qt::CaseInsensitive) == 0)
        notes.add("Annunciation (voice) not available", where);
    }
    addTitleDetails(c, *n, notes);
    parent.children.push_back(n);
    if (n->group) addChildren(c, *n, notes);
  }
}
} // namespace

QByteArray writePhoebusXml(const Document& doc, QStringList* notes) {
  if (!doc.root) throw ParseError("Configuration has no root group");
  Notes collected;
  if (doc.beepSeverity > 1)
    collected.add("$BEEPSEVERITY has no Phoebus equivalent", doc.root.get());
  QByteArray bytes;
  QXmlStreamWriter xml(&bytes);
  xml.setAutoFormatting(true);
  xml.setAutoFormattingIndent(2);
  xml.writeStartDocument();
  writeNode(xml, *doc.root, collected);
  xml.writeEndDocument();
  collected.write(notes);
  return bytes;
}
void savePhoebusXml(const Document& doc, const QString& path, QStringList* notes) {
  const auto bytes = writePhoebusXml(doc, notes);
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
    throw ParseError("Cannot write " + path + ": " + f.errorString());
}
Document parsePhoebusXml(const QByteArray& bytes, const QString& baseDir, QStringList* notes) {
  QSet<QString> stack;
  const auto root = readRoot(bytes, baseDir.isEmpty() ? QDir::currentPath() : baseDir, stack, 0);
  if (root.name != "config")
    throw ParseError("Expected a Phoebus <config> element, found <" + root.name + ">");
  Notes collected;
  Document doc;
  doc.root = std::make_shared<Node>();
  doc.root->group = true;
  doc.root->name = token(root.attributes.value("name"), "Configuration", collected, "/");
  if (doc.root->name == "NULL") doc.root->name = "NULL_config";
  addTitleDetails(root, *doc.root, collected);
  addChildren(root, *doc.root, collected);
  collected.write(notes);
  // Serialize and reparse: the result obeys every ALH configuration rule.
  return parseConfig(writeConfig(doc), baseDir);
}
Document loadPhoebusXml(const QString& path, QStringList* notes) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) throw ParseError("Cannot open " + path + ": " + f.errorString());
  return parsePhoebusXml(readXmlFile(f), QFileInfo(path).absolutePath(), notes);
}
} // namespace alh
