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
void Node::setOption(const QString& key, const QString& value) {
  for (int i = directives.size() - 1; i >= 0; --i)
    if (directives[i].key == key)
      directives.removeAt(i);
  if (!value.isEmpty())
    directives.push_back({key, value});
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
  static const char* names[] = {"NO_ALARM", "MINOR", "MAJOR", "INVALID", "ERROR"};
  return names[qBound(0, v, 4)];
}
int severityValue(const QString& s) {
  for (int i = 0; i < 5; ++i)
    if (s.toUpper() == severityName(i))
      return i;
  if (s.toUpper() == "NO_ALARMS")
    return 0;
  bool ok = false;
  int n = s.toInt(&ok);
  if (ok && n >= 0 && n <= 4)
    return n;
  throw ParseError("Unknown severity: " + s);
}
QString statusName(int v) {
  static const char* names[] = {"NO_ALARM",
                                "READ",
                                "WRITE",
                                "HIHI",
                                "HIGH",
                                "LOLO",
                                "LOW",
                                "STATE",
                                "COS",
                                "COMM",
                                "TIMEOUT",
                                "HWLIMIT",
                                "CALC",
                                "SCAN",
                                "LINK",
                                "SOFT",
                                "BAD_SUB",
                                "UDF",
                                "DISABLE",
                                "SIMM",
                                "READ_ACCESS",
                                "WRITE_ACCESS",
                                "NOT_CONNECTED",
                                "NO_READ_ACCESS",
                                "NO_WRITE_ACCESS"};
  return v >= 0 && v < int(sizeof(names) / sizeof(*names)) ? QString::fromLatin1(names[v])
                                                           : QString::number(v);
}
std::shared_ptr<Node> cloneNode(const Node& n, Node* parent) {
  auto c = std::make_shared<Node>();
  c->name = n.name;
  c->group = n.group;
  c->mask = n.mask;
  c->directives = n.directives;
  c->parent = parent;
  for (auto& child : n.children)
    c->children.push_back(cloneNode(*child, c.get()));
  return c;
}
} // namespace alh
