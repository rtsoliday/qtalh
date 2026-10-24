// Qt port of ALH configuration structures. See ../../LICENSE.
#include "model.h"
#include <QRegularExpression>
#include <alarm.h>
namespace alh {
QString Mask::text() const {
  QString result = "-----";
  const QString letters = "CDATL";
  for (int i = 0; i < 5; ++i)
    if (bits[i])
      result[i] = letters[i];
  return result;
}
Mask Mask::parse(const QString& text) {
  Mask result;
  const QString letters = "CDATL";
  // Legacy ALH treats masks as sets of letters, including short/reordered masks.
  // Like alSetMask, ignore all characters other than the five mask letters.
  for (auto ch : text) {
    if (ch == '-')
      continue;
    int bit = letters.indexOf(ch);
    if (bit >= 0)
      result[bit] = true;
  }
  return result;
}
QString Node::option(const QString& key, const QString& fallback) const {
  for (const auto& d : directives)
    if (d.key == key)
      return d.value;
  return fallback;
}
QString Node::option(QLatin1String key, const QString& fallback) const {
  for (const auto& d : directives)
    if (d.key == key)
      return d.value;
  return fallback;
}
void Node::setOption(const QString& key, const QString& value) {
  // Replace the first value in place, keeping its position and comments.
  int first = -1;
  for (int i = directives.size() - 1; i >= 0; --i)
    if (directives[i].key == key) {
      if (first >= 0) directives.removeAt(first);
      first = i;
    }
  if (value.isEmpty()) {
    if (first >= 0) directives.removeAt(first);
  } else if (first >= 0) {
    directives[first].value = value;
  } else {
    directives.push_back({key, value});
  }
}
QString Node::label() const {
  return option("ALIAS", name);
}
static void walk(Node* n, QVector<Node*>& out) {
  if (!n)
    return;
  out.push_back(n);
  for (auto& c : n->children)
    walk(c.get(), out);
}
QVector<Node*> Document::nodes() const {
  QVector<Node*> out;
  walk(root.get(), out);
  return out;
}
QVector<Node*> Document::channels() const {
  QVector<Node*> out;
  for (auto n : nodes())
    if (!n->group)
      out.push_back(n);
  return out;
}
QString severityName(int v) {
  // Static literals copy without allocation; these names are formatted per event.
  static const QString names[] = {QStringLiteral("NO_ALARM"), QStringLiteral("MINOR"),
                                  QStringLiteral("MAJOR"), QStringLiteral("INVALID"),
                                  QStringLiteral("ERROR")};
  return names[qBound(0, v, 4)];
}
int severityValue(const QString& s) {
  const auto upper = s.toUpper();
  for (int i = 0; i < 5; ++i)
    if (upper == severityName(i))
      return i;
  if (upper == QLatin1String("NO_ALARMS"))
    return 0;
  bool ok = false;
  int n = s.toInt(&ok);
  if (ok && n >= 0 && n <= 4)
    return n;
  throw ParseError("Unknown severity: " + s);
}
QString statusName(int v) {
  static const QString names[] = {QStringLiteral("NO_ALARM"),
                                  QStringLiteral("READ"),
                                  QStringLiteral("WRITE"),
                                  QStringLiteral("HIHI"),
                                  QStringLiteral("HIGH"),
                                  QStringLiteral("LOLO"),
                                  QStringLiteral("LOW"),
                                  QStringLiteral("STATE"),
                                  QStringLiteral("COS"),
                                  QStringLiteral("COMM"),
                                  QStringLiteral("TIMEOUT"),
                                  QStringLiteral("HWLIMIT"),
                                  QStringLiteral("CALC"),
                                  QStringLiteral("SCAN"),
                                  QStringLiteral("LINK"),
                                  QStringLiteral("SOFT"),
                                  QStringLiteral("BAD_SUB"),
                                  QStringLiteral("UDF"),
                                  QStringLiteral("DISABLE"),
                                  QStringLiteral("SIMM"),
                                  QStringLiteral("READ_ACCESS"),
                                  QStringLiteral("WRITE_ACCESS"),
                                  QStringLiteral("NOT_CONNECTED"),
                                  QStringLiteral("NO_READ_ACCESS"),
                                  QStringLiteral("NO_WRITE_ACCESS")};
  return v >= 0 && v < int(sizeof(names) / sizeof(*names)) ? names[v] : QString::number(v);
}
std::shared_ptr<Node> cloneNode(const Node& n, Node* parent, bool keepOrigins) {
  auto c = std::make_shared<Node>();
  c->name = n.name;
  c->group = n.group;
  c->mask = n.mask;
  c->directives = n.directives;
  c->before = n.before;
  c->source = n.source;
  if (keepOrigins) {
    c->origin = n.origin;
    c->includes = n.includes;
    c->documentLines = n.documentLines;
  }
  c->parent = parent;
  for (auto& child : n.children)
    c->children.push_back(cloneNode(*child, c.get(), keepOrigins));
  return c;
}
std::shared_ptr<Node> takeChild(Node& group, int index) {
  auto child = group.children.at(index);
  group.children.erase(group.children.begin() + index);
  for (auto& l : group.includes)
    if (l.children > index) --l.children;
  return child;
}
void insertChild(Node& group, int index, std::shared_ptr<Node> child) {
  group.children.insert(group.children.begin() + index, std::move(child));
  for (auto& l : group.includes)
    if (l.children > index) ++l.children;
}
QVector<Directive> keepDirectiveSources(const QVector<Directive>& before,
                                        const QVector<Directive>& after) {
  QHash<QString, QVector<int>> values; // Indexes into `after`, by key.
  QHash<QString, int> last;            // Each key's last index in `before`.
  for (int i = 0; i < after.size(); ++i) values[after[i].key] << i;
  for (int i = 0; i < before.size(); ++i) last[before[i].key] = i;
  QVector<bool> placed(after.size());
  QHash<QString, int> used;
  QVector<Directive> result;
  for (int i = 0; i < before.size(); ++i) {
    const auto& old = before[i];
    const auto& list = values[old.key];
    int& next = used[old.key];
    if (next < list.size()) {
      Directive d = after[list[next]];
      placed[list[next++]] = true;
      // The source text is written again only while the value is unchanged.
      d.before = old.before;
      d.source = old.source;
      d.sourceValue = old.sourceValue;
      result << d;
    }
    // A removed value is dropped with the comments before it.
    if (last[old.key] == i)
      while (next < list.size()) {
        placed[list[next]] = true;
        result << after[list[next++]];
      }
  }
  for (int i = 0; i < after.size(); ++i)
    if (!placed[i]) result << after[i];
  return result;
}
} // namespace alh
