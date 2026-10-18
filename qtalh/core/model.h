// Qt port of ALH. Derived from alLib.h/alConfig.c; see ../../LICENSE.
#pragma once
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>

namespace alh {
constexpr int SeverityCount = 5;
constexpr int ErrorSeverity = 4;
enum MaskBit { Cancel, Disable, Ack, AckT, Log };
struct Mask {
  std::array<bool, 5> bits{};
  bool operator[](int i) const {
    return bits.at(i);
  }
  bool& operator[](int i) {
    return bits.at(i);
  }
  QString text() const;
  static Mask parse(const QString& text);
};
struct Directive {
  QString key, value;
};
struct Node {
  bool group = false;
  QString name;
  Mask mask;
  Node* parent = nullptr;
  std::vector<std::shared_ptr<Node>> children;
  QVector<Directive> directives;
  QString option(const QString& key, const QString& fallback = {}) const;
  // Literal keys are compared without allocating a temporary key on hot paths.
  QString option(QLatin1String key, const QString& fallback = {}) const;
  template <std::size_t N> QString option(const char (&key)[N], const QString& fallback = {}) const {
    return option(QLatin1String(key, int(N - 1)), fallback);
  }
  void setOption(const QString& key, const QString& value);
  QString label() const;
};
struct Document {
  std::shared_ptr<Node> root;
  QString filename;
  int beepSeverity = 1;
  // Text that was not valid in the locale encoding is read as ISO-8859-1;
  // saveConfig() then writes ISO-8859-1 again when it can represent the text.
  bool latin1 = false;
  QVector<Node*> nodes() const;
  QVector<Node*> channels() const;
};
QString severityName(int value);
int severityValue(const QString& name);
QString statusName(int value);
struct ParseError : std::runtime_error {
  explicit ParseError(const QString& message) : std::runtime_error(message.toStdString()) {}
};
Document loadConfig(const QString& path, const QString& configDir = QString());
Document parseConfig(const QString& text, const QString& baseDir = QString());
QString writeConfig(const Document& doc);
void saveConfig(const Document& doc, const QString& path);
QString writeReport(const Document& doc);
std::shared_ptr<Node> cloneNode(const Node& node, Node* parent = nullptr);
} // namespace alh
