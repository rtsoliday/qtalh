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
// Text kept from a configuration file so that saving keeps its comments,
// blank lines and formatting. `before` holds the comment and blank lines that
// precede a statement; `source` holds the statement as written when that
// differs from the form QtALH writes.
struct Directive {
  QString key, value;
  // `source` is written while `value` still equals `sourceValue`.
  QString before, source, sourceValue;
};
// An INCLUDE line and the file it read. Nodes declared in that file refer to it.
struct Include {
  QString path; // Absolute path of the included file.
  QString line, before; // The INCLUDE line as written, and the lines before it.
  QString trailer; // Comment and blank lines after the file's last statement.
  std::shared_ptr<const Include> parent; // The including file; null for the top-level file.
};
// A $BEEPSEVERITY or $HEARTBEATPV line (see Directive), which applies to the
// whole configuration. Only the line that takes effect is kept.
struct DocumentLine {
  QString key, value, before, source; // `value` as read.
  std::shared_ptr<const Include> origin; // Null for the top-level file.
  // Kept by a node: written after this many of its directives.
  int position = 0;
  // Read order, against an INCLUDE line read after as many directives.
  int sequence = 0;
};
struct Node {
  bool group = false;
  QString name;
  Mask mask;
  Node* parent = nullptr;
  std::vector<std::shared_ptr<Node>> children;
  QVector<Directive> directives;
  // The declaration as read (see Directive), written while it still declares
  // this node in place. Nodes created by editing have none.
  QString before, source;
  // The INCLUDE file that declared this node; null for the top-level file and
  // for nodes added by editing, which belong to their parent's file.
  std::shared_ptr<const Include> origin;
  // The INCLUDE lines that named this group as parent, and where they were.
  struct IncludeLine {
    std::shared_ptr<const Include> include;
    int children = 0; // This group's children before the INCLUDE line.
    int directives = -1; // Its directives before the line, when the line followed them.
    bool empty = false; // The file declared no group or channel.
    int sequence = 0; // Read order, as DocumentLine::sequence.
  };
  QVector<IncludeLine> includes;
  // $BEEPSEVERITY and $HEARTBEATPV lines that followed this node's statements.
  QVector<DocumentLine> documentLines;
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
  // $BEEPSEVERITY and $HEARTBEATPV lines before the first group or channel of
  // their file; Node::documentLines holds the others.
  QVector<DocumentLine> documentLines;
  // Comment and blank lines after the top-level file's last statement.
  QString trailer;
  // Text that was not valid in the locale encoding is read as ISO-8859-1;
  // saveConfig() then writes ISO-8859-1 again when it can represent the text.
  bool latin1 = false;
  // Non-fatal notes from loading, such as directives legacy ALH reads differently.
  QStringList warnings;
  QVector<Node*> nodes() const;
  QVector<Node*> channels() const;
};
// $FORCEPV_CALC_A through _L: EPICS calcPerform accepts twelve arguments.
// Legacy ALH reads only A through F.
constexpr int ForceCalcInputs = 12;
inline QString forceCalcInputKey(int i) { return QStringLiteral("FORCEPV_CALC_") + QChar('A' + i); }
QString severityName(int value);
int severityValue(const QString& name);
QString statusName(int value);
struct ParseError : std::runtime_error {
  explicit ParseError(const QString& message) : std::runtime_error(message.toStdString()) {}
};
Document loadConfig(const QString& path, const QString& configDir = QString());
// Also reads ConfigText::Edit text.
Document parseConfig(const QString& text, const QString& baseDir = QString());
enum class ConfigText {
  // Legacy ALH's form: no comments, INCLUDE files expanded, each group's
  // channels before its subgroups.
  Canonical,
  // The text as read, with comments and blank lines; changed statements are
  // written in Canonical form in place. INCLUDE files are expanded.
  Source,
  // As Source, but each INCLUDE line is followed by its file, indented three
  // spaces per level, as in ALH's Configuration File window.
  Listing,
  // For parseConfig() only: as Source, keeping INCLUDE files distinct.
  Edit,
};
QString writeConfig(const Document& doc, ConfigText form = ConfigText::Canonical);
struct ConfigFile {
  QString path, text;
};
// The files a document is read from, as Source text that keeps each INCLUDE
// line: the top-level file (written to `path`), then each INCLUDE file. With
// `check`, throws ParseError when the files would not read back as this
// configuration.
QVector<ConfigFile> writeConfigFiles(const Document& doc, const QString& path,
                                     const QString& configDir = QString(), bool check = true);
// Why reloading `text` would change the hierarchy, or empty. Possible only
// when a group repeats an ancestor's name.
QString hierarchyChange(const Document& doc, const QString& text);
// writeConfig() text, or ParseError when hierarchyChange() reports a problem.
QString writeCheckedConfig(const Document& doc, ConfigText form = ConfigText::Canonical);
// Writes Source text, with INCLUDE files expanded.
void saveConfig(const Document& doc, const QString& path);
// Writes `text` as `doc` would be saved, in its encoding.
void saveConfigText(const Document& doc, const QString& path, const QString& text);
// Writes several files as saveConfigText() does, in order. All are written to
// temporary files before any replaces its file, so a write failure leaves every
// file unchanged. Throws ParseError; `replaced` lists the files already replaced
// when a later replacement fails.
void saveConfigTexts(const Document& doc, const QVector<ConfigFile>& files, QStringList& replaced);
QString writeReport(const Document& doc);
// With keepOrigins false, the copy belongs to the file of the group it is
// added to, and its INCLUDE files are written expanded into that file.
std::shared_ptr<Node> cloneNode(const Node& node, Node* parent = nullptr, bool keepOrigins = true);
// Remove or insert a group's child; its INCLUDE lines keep their place.
std::shared_ptr<Node> takeChild(Node& group, int index);
void insertChild(Node& group, int index, std::shared_ptr<Node> child);
// The directives of an edit that rebuilt `after` from form fields. Each key's
// values take the positions, comments and source text (Directive) of that
// key's earlier directives in order; further values follow the last of them,
// and new keys follow in `after` order.
QVector<Directive> keepDirectiveSources(const QVector<Directive>& before,
                                        const QVector<Directive>& after);
} // namespace alh
