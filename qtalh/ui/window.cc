#include "core/diagnostics.h"
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
// Qt Widgets port of ALH awAlh/awAct/axRunW/axSubW and dialog workflows.
// Author credits are recorded in ../../AUTHORS.md; see ../../LICENSE for the license.
#include "alarm_view.h"
#include "dialogs.h"
#include "help.h"
#include "window.h"
#include "services/sdds_export.h"
#include <QDesktopServices>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTextDocument>
#include <QtWidgets>
#include <cmath>
#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace alh {
namespace {
#ifndef Q_OS_WIN
int interruptPipe[2] = {-1, -1};
void writeInterruptPipe(int) {
  const int saved = errno;
  const char byte = 1;
  if (::write(interruptPipe[1], &byte, 1) < 0) {} // A full pipe already wakes the reader.
  errno = saved;
}
#endif
// Used by both command execution and its tooltip preview.
QString launchCommand(QString command) {
  if (command.startsWith("MASTER_ONLY")) command = command.mid(11).trimmed();
  // Prefer the Qt display manager while preserving arguments, quoting, and redirects.
  static const QRegularExpression executable(R"(^\s*("[^"]+"|'[^']+'|[^\s;&|<>]+))");
  const auto match = executable.match(command);
  QString program = match.captured(1);
  if (program.startsWith('"') || program.startsWith('\''))
    program = program.mid(1, program.size() - 2);
  program.replace('\\', '/');
  const auto basename = program.section('/', -1);
  if (basename == "medm" || basename == "medm.exe")
    command.replace(match.capturedStart(1), match.capturedLength(1), "qtedm");
  return command;
}
QStringList relatedEntries(const QString& command) {
  return command.split(QRegularExpression("[!\\n]"), Qt::SkipEmptyParts);
}
QString commandPreview(const QString& command) {
  const auto entries = relatedEntries(command);
  auto preview = [](const QString& value) {
    return (value.startsWith("MASTER_ONLY") ? QString("Master only:\n") : QString()) + launchCommand(value);
  };
  if (entries.size() == 1) return preview(entries.front());
  if (entries.size() % 2) return "Invalid related-process command:\n" + command;
  QStringList choices;
  for (int i = 0; i + 1 < entries.size(); i += 2)
    choices << entries[i].trimmed() + ":\n" + preview(entries[i + 1].trimmed());
  return choices.join("\n\n");
}
// Dialog fields have independent defaults; whitespace must never shift a later
// field into an earlier argument. Keep file-format compatibility in the parser.
QString forceDirective(QString name, QString mask, QString force, QString reset) {
  name = name.trimmed(); force = force.trimmed(); reset = reset.trimmed();
  if (name.contains(QRegularExpression("\\s")))
    throw ParseError("Force PV name must be a single token");
  auto number = [](const QString& text, double fallback) {
    if (text.isEmpty()) return QString::number(fallback);
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value)) throw ParseError("Invalid Force PV value: " + text);
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
  };
  return name + ' ' + Mask::parse(mask).text() + ' ' + number(force, 1) + ' ' +
      (reset.compare("NE", Qt::CaseInsensitive) == 0 ? QString("NE") : number(reset, 0));
}
void applyInitialGeometry(QWidget* window, const QString& geometry) {
  // X geometry permits dimensions, position only, and negative edge offsets.
  static const QRegularExpression syntax(R"(^=?(?:(\d+)?(?:x(\d+))?)?([+-]\d+)?([+-]\d+)?$)");
  const auto match = syntax.match(geometry);
  if (!match.hasMatch() || geometry.isEmpty()) return;
  QSize size = window->size();
  for (int field = 1; field <= 2; ++field) {
    if (match.captured(field).isEmpty()) continue;
    bool ok = false;
    int value = match.captured(field).toInt(&ok);
    if (!ok || value <= 0) return;
    if (field == 1) size.setWidth(value);
    else size.setHeight(value);
  }
  int offsets[2]{};
  for (int axis = 0; axis < 2; ++axis) {
    if (match.captured(axis + 3).isEmpty()) continue;
    bool ok = false;
    offsets[axis] = match.captured(axis + 3).toInt(&ok);
    if (!ok) return;
  }
  window->resize(size);
  const auto screen = window->screen();
  if (!screen) return;
  const auto available = screen->availableGeometry();
  const auto frame = window->frameGeometry();
  QPoint position = frame.topLeft();
  if (!match.captured(3).isEmpty())
    position.setX(match.captured(3).startsWith('-')
        ? available.right() + 1 - frame.width() + offsets[0] : available.left() + offsets[0]);
  if (!match.captured(4).isEmpty())
    position.setY(match.captured(4).startsWith('-')
        ? available.bottom() + 1 - frame.height() + offsets[1] : available.top() + offsets[1]);
  window->move(position);
}
// Pane divider drawn directly: classic ALH uses a black rule, the modern view a
// one-pixel line that highlights while hovered. No style sheet reaches the views.
class PaneSplitter : public QSplitter {
public:
  using QSplitter::QSplitter;
protected:
  QSplitterHandle* createHandle() override { return new Handle(orientation(), this); }
private:
  class Handle : public QSplitterHandle {
  public:
    Handle(Qt::Orientation orientation, QSplitter* parent) : QSplitterHandle(orientation, parent) {
      setAttribute(Qt::WA_Hover);
    }
  protected:
    void paintEvent(QPaintEvent*) override {
      QPainter painter(this);
      if (legacyAppearance()) { painter.fillRect(rect(), Qt::black); return; }
      const auto& colors = palette();
      painter.fillRect(rect(), colors.color(QPalette::Base));
      const bool hot = underMouse();
      const int line = hot ? 2 : 1;
      painter.fillRect(QRect((width() - line) / 2, 0, line, height()),
                       hot ? colors.color(QPalette::Highlight)
                           : blend(colors.color(QPalette::Window), colors.color(QPalette::WindowText), 0.16));
    }
  };
};
// A one-pixel rule that follows the current palette.
class SeparatorLine : public QWidget {
public:
  explicit SeparatorLine(Qt::Orientation orientation) {
    setObjectName("separatorLine");
    if (orientation == Qt::Horizontal) setFixedHeight(1);
    else setFixedWidth(1);
  }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    const auto& colors = palette();
    painter.fillRect(rect(), blend(colors.color(QPalette::Window), colors.color(QPalette::WindowText), 0.16));
  }
};
QWidget* separatorLine(Qt::Orientation orientation) { return new SeparatorLine(orientation); }
// A small rounded sample of an ALH severity color for menus and choices.
QIcon severitySwatch(int severity) {
  QPixmap pixmap(28, 28);
  pixmap.setDevicePixelRatio(2);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  const auto color = severityColor(severity);
  painter.setPen(QPen(color.darker(130), 1));
  painter.setBrush(color);
  painter.drawRoundedRect(QRectF(1.5, 1.5, 11, 11), 3, 3);
  return QIcon(pixmap);
}
QString code(int s) {
  static const QString codes[] = {QStringLiteral(" "), QStringLiteral("Y"), QStringLiteral("R"),
                                  QStringLiteral("V"), QStringLiteral("E")};
  return codes[qBound(0, s, 4)];
}
// A modal window below a dialog may be a stack object inside exec(). Closing,
// clearing or deleting the dialog there would destroy it, so wait until it returns.
// Delete-on-close heap boxes are not in exec() and are deleted safely with their parent.
bool hostsModal(const QWidget* dialog) {
  for (auto window : QApplication::topLevelWidgets())
    if (window->isVisible() && window->isModal() && !window->testAttribute(Qt::WA_DeleteOnClose))
      for (auto w = window->parentWidget(); w; w = w->parentWidget())
        if (w == dialog) return true;
  return false;
}
} // namespace
AlarmModel::AlarmModel(Document* d, Engine* e, bool t, QObject* p)
    : QAbstractItemModel(p), doc(d), engine(e), tree(t), group(d->root.get()) {}
bool AlarmModel::visible(Node* n) const {
  if (!filter)
    return true;
  auto& s = engine->state(n);
  if (s.shelf.until) return false;
  const auto visible = engine->presentation(n);
  const int severity = n->group ? visible.severity : s.severity;
  const int unack = n->group ? visible.unack : s.unack;
  return filter == 1 ? severity > 0 || unack > 0 : unack > 0;
}
QVector<Node*> AlarmModel::children(Node* n) const {
  auto cached = childCache.constFind(n);
  if (cached != childCache.cend())
    return cached.value();
  auto key = n;
  QVector<Node*> result;
  if (!n) {
    if (tree && doc->root && visible(doc->root.get()))
      result.push_back(doc->root.get());
    else if (!tree)
      n = group;
  }
  if (n) {
    for (auto& c : n->children)
      if (c->group && visible(c.get()))
        result.push_back(c.get());
    if (!tree)
      for (auto& c : n->children)
        if (!c->group && visible(c.get()))
          result.push_back(c.get());
  }
  childCache.insert(key, result);
  return result;
}
QModelIndex AlarmModel::index(int row, int col, const QModelIndex& p) const {
  if (row < 0 || col < 0 || col >= 9 || p.column() > 0)
    return {};
  if (!tree && p.isValid())
    return {};
  // A const copy shares the cached list; a mutable one would copy all rows here.
  const auto list = children(node(p));
  if (row >= list.size())
    return {};
  return createIndex(row, col, list.at(row));
}
QModelIndex AlarmModel::parent(const QModelIndex& i) const {
  if (!tree)
    return {};
  auto n = node(i);
  if (!n || !n->parent)
    return {};
  auto p = n->parent;
  auto siblings = children(p->parent);
  int row = siblings.indexOf(p);
  return row < 0 ? QModelIndex() : createIndex(row, 0, p);
}
int AlarmModel::rowCount(const QModelIndex& p) const {
  if (p.column() > 0 || (!tree && p.isValid()))
    return 0;
  return children(node(p)).size();
}
QVariant AlarmModel::data(const QModelIndex& i, int role) const {
  auto n = node(i);
  if (!n)
    return {};
  // Painting and measuring request these per cell; they need no alarm state.
  if (role == Qt::FontRole)
    return font(i.column(), n->group);
  if (role == Qt::TextAlignmentRole)
    return int(i.column() < 2 ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
  const auto& s = engine->state(n);
  const auto shown = engine->presentation(n);
  int severity = shown.severity;
  int unack = shown.unack;
  if (role == AlarmSeverityRole) return qMax(severity, unack);
  if (role == Qt::ToolTipRole) {
    const auto changed = i.column() != 4 && i.column() != 5 && modificationText ? modificationText(n) : QString();
    const auto changes = changed.isEmpty() ? QString() : "\n\n" + changed;
    if (s.shelf.until && i.column() != 4 && i.column() != 5)
      return Engine::channelPath(n) + "\nShelved until " +
          QDateTime::fromMSecsSinceEpoch(s.shelf.until).toString(Qt::ISODate) +
          "\nShelved by: " + s.shelf.username + "\nReason: " + s.shelf.reason + "\nUnderlying: " + statusName(s.status) + " " +
          severityName(s.severity) + "; unacknowledged: " + severityName(s.unack) + "\n" + s.value + changes;
    if (i.column() == 4) {
      QStringList guidance;
      for (const auto& key : {"GUIDANCE_TEXT", "GUIDANCE"})
        if (!n->option(key).isEmpty()) guidance << n->option(key);
      return Qt::convertFromPlainText(guidance.join("\n\n"));
    }
    if (i.column() == 5)
      return Qt::convertFromPlainText(commandPreview(n->option("COMMAND")));
    return n->name + "\n" + statusName(s.status) + " " + severityName(s.severity) + "\n" + s.value + changes;
  }
  if (role == Qt::BackgroundRole) {
    if (i.column() == 0)
      return severityColor(unack);
    if (i.column() == 1)
      return severityColor(severity);
    static const QColor group("#b0c3ca"), channel("lightblue"), masked("blue"), shelved("#d8c6e7");
    if (i.column() == 2) {
      if (s.shelf.until || shown.shelved)
        return shelved;
      return legacyAppearance() ? (n->group ? group : channel) : QColor();
    }
    if (i.column() == 6 && coloredMasks) {
      // ALH colors <CDATL> when C, D, or A (including the timed H) is set.
      const bool silenced = n->group
                                ? s.maskCounts[Cancel] || s.maskCounts[Disable] || s.maskCounts[Ack]
                                : s.mask[Cancel] || s.mask[Disable] || s.mask[Ack];
      if (silenced || s.noAckUntil)
        return masked;
    }
  }
  if (role != Qt::DisplayRole)
    return {};
  switch (i.column()) {
  case 0:
    return code(unack);
  case 1:
    return code(severity);
  case 2:
    return info(n).label + (modifications && modifications(n) ? QStringLiteral(" \u270E") : QString()) +
        (s.shelf.until ? " [Shelved]" :
        shown.shelved ? QString(" [%1 shelved]").arg(shown.shelved) : QString());
  case 3:
    return info(n).childGroups ? QStringLiteral("\u25B6") : QString();
  case 4:
    return info(n).guidance ? QStringLiteral("G") : QString();
  case 5:
    return info(n).command ? QStringLiteral("P") : QString();
  case 6: {
    QString m = s.mask.text();
    if (n->group) {
      m = "-----";
      for (int j = 0; j < 5; ++j)
        if (s.maskCounts[j])
          m[j] = QString("CDATL")[j];
    }
    if (s.noAckUntil)
      m[2] = 'H';
    return '<' + m + '>';
  }
  case 7:
    return s.highestBeepThreshold > 1 ? code(s.highestBeepThreshold) : QString();
  case 8:
    if (!severity && !unack)
      return QString();
    if (n->group)
      return QString("(%1,%2,%3,%4,%5)")
          .arg(shown.counts[4])
          .arg(shown.counts[3])
          .arg(shown.counts[2])
          .arg(shown.counts[1])
          .arg(shown.counts[0]);
    return QString("<%1,%2>").arg(statusName(s.status), severityName(severity)) +
           (unack ? QString(",<%1>").arg(severityName(unack)) : QString());
  }
  return {};
}
QFont AlarmModel::font(int column, bool group) const {
  if (!legacyAppearance()) {
    const auto base = QApplication::font();
    if (column != 2) return base;
    // Rebuild the name fonts only when the application font is replaced.
    if (!styledFonts || !styledBase.isCopyOf(base)) {
      styledBase = base;
      for (int bold = 0; bold < 2; ++bold) {
        styledName[bold] = base;
        styledName[bold].setBold(bold);
      }
      styledFonts = true;
    }
    return styledName[group];
  }
  if (!legacyFonts) {
    legacyText = QFont("monospace", 10);
    legacyText.setStyleHint(QFont::TypeWriter);
    legacyText.setPixelSize(12);
    legacyText.setStretch(100);
    legacyText.setStretch(85);
    legacyName = QFont("Helvetica", 10);
    legacyName.setBold(true);
    legacyName.setPixelSize(12);
    legacyName.setStretch(100);
    // Severity letters in the indicator boxes: bold, full width and without
    // antialiasing, so they stay crisp on the colored boxes as ALH's bitmap
    // fonts were, rather than thin condensed strokes blurred into red.
    legacyIndicator = QFont("monospace", 10);
    legacyIndicator.setStyleHint(QFont::TypeWriter, QFont::NoAntialias);
    legacyIndicator.setPixelSize(12);
    legacyIndicator.setBold(true);
    legacyIndicator.setStretch(100);
    legacyFonts = true;
  }
  return column == 2 ? legacyName : column < 2 ? legacyIndicator : legacyText;
}
AlarmModel::RowSignature AlarmModel::signature(Node* n) const {
  const auto& s = engine->state(n);
  const auto shown = engine->presentation(n);
  RowSignature result;
  result.counts = shown.counts;
  result.severity = shown.severity;
  result.unack = shown.unack;
  result.shelved = shown.shelved;
  result.status = s.status;
  result.beep = s.highestBeepThreshold;
  for (int bit = 0; bit < 5; ++bit)
    if (n->group ? s.maskCounts[bit] != 0 : s.mask[bit]) result.flags |= 1 << bit;
  if (s.noAckUntil) result.flags |= 1 << 5;
  if (s.shelf.until) result.flags |= 1 << 6;
  const auto& facts = info(n);
  if (facts.childGroups) result.flags |= 1 << 7;
  if (facts.guidance) result.flags |= 1 << 8;
  if (facts.command) result.flags |= 1 << 9;
  if (coloredMasks) result.flags |= 1 << 10;
  result.label = facts.label;
  if (modifications) result.modified = modifications(n);
  return result;
}
int AlarmModel::RowSignature::firstDifference(const RowSignature& o) const {
  // Columns: 0 unacknowledged, 1 severity, 2 name, 3 arrow, 4 G, 5 P, 6 mask,
  // 7 beep, 8 counts or channel alarm data (which also shows both severities).
  if (unack != o.unack) return 0;
  if (severity != o.severity) return 1;
  constexpr int shelf = 1 << 6, arrow = 1 << 7, guidance = 1 << 8, command = 1 << 9;
  const int changedFlags = flags ^ o.flags;
  if (label != o.label || shelved != o.shelved || modified != o.modified || (changedFlags & shelf)) return 2;
  if (changedFlags & arrow) return 3;
  if (changedFlags & guidance) return 4;
  if (changedFlags & command) return 5;
  if (changedFlags) return 6;
  if (beep != o.beep) return 7;
  return 8;
}
const AlarmModel::NodeInfo& AlarmModel::info(Node* n) const {
  auto found = nodeInfo.find(n);
  if (found != nodeInfo.end()) return found.value();
  NodeInfo facts;
  facts.label = n->label();
  facts.childGroups = n->group && std::any_of(n->children.begin(), n->children.end(),
                                              [](const auto& c) { return c->group; });
  facts.guidance = !n->option("GUIDANCE").isEmpty() || !n->option("GUIDANCE_TEXT").isEmpty();
  facts.command = !n->option("COMMAND").isEmpty();
  return nodeInfo.insert(n, std::move(facts)).value();
}
Qt::ItemFlags AlarmModel::flags(const QModelIndex& i) const {
  if (!i.isValid()) return Qt::NoItemFlags;
  auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
  if (i.column() == 2) flags |= Qt::ItemIsDragEnabled;
  return flags;
}
QStringList AlarmModel::mimeTypes() const {
  return {"text/plain"};
}
QMimeData* AlarmModel::mimeData(const QModelIndexList& list) const {
  auto data = new QMimeData;
  QStringList names;
  for (auto i : list)
    if (node(i) && !names.contains(node(i)->name))
      names << node(i)->name;
  data->setText(names.join('\n'));
  return data;
}
void AlarmModel::reset(Document* d, Engine* e) {
  beginResetModel();
  childCache.clear();
  signatures.clear();
  nodeInfo.clear();
  doc = d;
  engine = e;
  group = d->root.get();
  endResetModel();
}
void AlarmModel::setGroup(Node* n) {
  beginResetModel();
  childCache.clear();
  signatures.clear();
  group = n;
  endResetModel();
}
void AlarmModel::setFilter(int f) {
  beginResetModel();
  childCache.clear();
  signatures.clear();
  filter = f;
  endResetModel();
}
void AlarmModel::refresh() {
  std::function<void(QModelIndex)> visit = [&](QModelIndex p) {
    if (filter) {
      // Preserve persistent indexes for rows that remain visible: a full reset
      // discards pressed acknowledgement buttons and delayed expansion clicks.
      // Remove/insert only changed membership, so disappearing targets still
      // invalidate safely and cannot redirect an action to a replacement row.
      auto key = node(p);
      auto previous = children(key);
      childCache.remove(key);
      const auto desired = children(key);
      childCache[key] = previous;
      // Membership is usually unchanged; only a change needs the row diff.
      if (desired != previous) {
        QSet<Node*> retained;
        for (auto n : desired) retained.insert(n);
        for (int last = previous.size() - 1; last >= 0;) {
          if (retained.contains(previous[last])) { --last; continue; }
          int first = last;
          while (first > 0 && !retained.contains(previous[first - 1])) --first;
          beginRemoveRows(p, first, last);
          previous.remove(first, last - first + 1);
          childCache[key] = previous;
          endRemoveRows();
          last = first - 1;
        }
        for (int first = 0; first < desired.size();) {
          if (first < previous.size() && previous[first] == desired[first]) { ++first; continue; }
          int last = first;
          while (last + 1 < desired.size() &&
                 (first == previous.size() || desired[last + 1] != previous[first])) ++last;
          beginInsertRows(p, first, last);
          for (int i = first; i <= last; ++i) previous.insert(i, desired[i]);
          childCache[key] = previous;
          endInsertRows();
          first = last + 1;
        }
      }
    }
    // Report each run of changed rows, starting at the first changed column
    // (counts alone, typically). Unchanged rows and cells keep their pixels.
    const auto rows = children(node(p));
    const int count = rows.size();
    int first = -1, column = 8;
    for (int row = 0; row <= count; ++row) {
      int changed = -1;
      if (row < count) {
        auto next = signature(rows[row]);
        auto previous = signatures.find(rows[row]);
        if (previous == signatures.end()) {
          signatures.insert(rows[row], std::move(next));
          changed = 0;
        } else if (!(*previous == next)) {
          changed = next.firstDifference(*previous);
          *previous = std::move(next);
        }
      }
      if (changed >= 0) {
        if (first < 0) first = row;
        column = qMin(column, changed);
      } else if (first >= 0) {
        emit dataChanged(index(first, column, p), index(row - 1, 8, p));
        first = -1;
        column = 8;
      }
    }
    if (tree)
      for (int r = 0; r < count; ++r)
        visit(index(r, 0, p));
  };
  visit({});
}
Window::Window(Document d, Options o, bool connections)
    : doc(std::move(d)), options(o), connectPv(connections) {
  // Runtime alarm filters have no useful meaning without live editor alarms.
  if (options.editor) options.filter = 0;
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowIcon(QIcon(":/qtalh/alh.xbm"));
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { delete this; });
  if (legacyAppearance()) {
    QFont mainFont("monospace");
    mainFont.setPixelSize(12);
    mainFont.setStretch(85);
    setFont(mainFont);
  }
  setupEngine();
  buildUi();
  menus();
  if (options.editor) rememberIncludes();
  if (!doc.filename.isEmpty())
    logging->operation(nullptr, "Setup Config File : " + doc.filename);
  showLoadWarnings();
  resize(1000, 600);
  heartbeatTimer.setParent(this);
  heartbeatTimer.setObjectName("heartbeatTimer");
  heartbeatTimer.setSingleShot(true);
  heartbeatTimer.setTimerType(Qt::PreciseTimer);
  connect(&heartbeatTimer, &QTimer::timeout, this, [this] {
    engine->tickHeartbeat();
    scheduleHeartbeat();
  });
  refreshTimer.setInterval(200);
  connect(&refreshTimer, &QTimer::timeout, this, [this] {
    engine->tick();
    // Unavailable heartbeat PVs suspend the precise timer. The ordinary refresh
    // resumes it after CA reports connection/write access, without a retry storm.
    if (!heartbeatTimer.isActive()) scheduleHeartbeat();
    if (dirty) {
      refresh();
      dirty = false;
    }
  });
  refreshTimer.start();
  // Status/blinking keeps its one-second cadence independently of alarm audio.
  statusTimer.setParent(this);
  statusTimer.setObjectName("runtimeStatusTimer");
  statusTimer.setInterval(1000);
  connect(&statusTimer, &QTimer::timeout, this, [this] {
    if (dirty) refresh();
    else refreshStatus();
    // Descriptor use grows with connected servers; check it every ten seconds.
    if (!options.editor && ++statusTicks % 10 == 0) {
      const auto warning = openFileMonitor.check(openFiles());
      if (!warning.isEmpty()) {
        qWarning("QtALH: %s", qPrintable(warning));
        if (logging) logging->operation(nullptr, warning);
        showMessage(warning);
      }
    }
  });
  statusTimer.start();
  beepTimer.setParent(this);
  beepTimer.setObjectName("alarmBeepTimer");
  beepTimer.setInterval(2000);
  connect(&beepTimer, &QTimer::timeout, this, [this] {
    // Possible inherited Linux ALH bug, deferred: becoming inaudible prevents
    // new playback but does not stop an already playing custom sound. Silence,
    // acknowledgement, or suppression can therefore leave a long clip playing
    // to completion, as with ALH's background play process. Stop active audio
    // when it is no longer wanted in a later change; retain behavior for now.
    if (engine->audible()) {
      // A bad custom file or media backend must not silence future alarms.
      // Keep the error diagnostic, but fall back to the ordinary system bell.
      if (options.sound.isEmpty() || !sound || sound->error() != QMediaPlayer::NoError)
        systemBeep();
      else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const bool playing = sound->playbackState() == QMediaPlayer::PlayingState;
#else
        const bool playing = sound->state() == QMediaPlayer::PlayingState;
#endif
        if (!playing) {
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
          // Qt 5's GStreamer backend can truncate short Ogg sounds when
          // seeking after EndOfMedia. Reload so every beep plays the full
          // clip, as ALH's fresh `play` process does.
          sound->setMedia(sound->media());
#else
          sound->setPosition(0);
#endif
          sound->play();
        }
      }
    }
  });
  beepTimer.start();
  if (!options.sound.isEmpty()) {
    sound = new QMediaPlayer(this);
    sound->setObjectName("alarmSound");
    auto reportSoundError = [this] {
      error(QString("Cannot play alarm sound %1: %2").arg(options.sound, sound->errorString()));
    };
    const auto source = QUrl::fromLocalFile(QFileInfo(options.sound).absoluteFilePath());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    sound->setAudioOutput(new QAudioOutput(sound));
    connect(sound, &QMediaPlayer::errorOccurred, this, reportSoundError);
    sound->setSource(source);
#else
    connect(sound, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            reportSoundError);
    sound->setMedia(source);
#endif
  }
  if (!options.editor)
    engine->start();
  scheduleHeartbeat();
  refresh();
}
void Window::scheduleHeartbeat() {
  heartbeatTimer.stop();
  int delay = engine->heartbeatDelay();
  if (delay >= 0)
    heartbeatTimer.start(delay);
}
void Window::systemBeep() {
  QApplication::beep();
}
Window::~Window() {
  debugLog(options.debug, "window", "close " + doc.filename);
  heartbeatTimer.stop();
  refreshTimer.stop();
  statusTimer.stop();
  beepTimer.stop();
  if (sound)
    sound->stop();
  // The QWidget base destructor hides a visible window after the Document
  // and Engine members are destroyed; the resulting focus change makes the
  // views read their current index. Detach them first, as replace() does.
  if (treeView) treeView->setModel(nullptr);
  if (groupView) groupView->setModel(nullptr);
  if (analytics) analytics->detach();
  if (notifications) notifications->detach();
  engine.reset();
  ca.reset();
  delete runtime;
}
namespace {
// A node's effective Force PV settings: the first value of each FORCEPV
// directive, as Node::option() reads them, so reordering is not an edit. The
// directives share the document's strings rather than formatting new text.
QVector<Directive> forceSettings(const Node* n) {
  QVector<Directive> settings;
  for (const auto& d : n->directives)
    if (d.key.startsWith(QLatin1String("FORCEPV")) &&
        std::none_of(settings.cbegin(), settings.cend(), [&](const Directive& s) { return s.key == d.key; }))
      settings.push_back(d);
  std::sort(settings.begin(), settings.end(),
            [](const Directive& a, const Directive& b) { return a.key < b.key; });
  return settings;
}
bool sameSettings(const QVector<Directive>& a, const QVector<Directive>& b) {
  return std::equal(a.cbegin(), a.cend(), b.cbegin(), b.cend(), [](const Directive& x, const Directive& y) {
    return x.key == y.key && x.value == y.value;
  });
}
enum Modification { MaskChanged = 1, BeepChanged = 2, ForceDisabled = 4, NoAckTimer = 8, Shelved = 16,
                    FacilityBeepChanged = 32, ForceEdited = 64 };
} // namespace
void Window::setupEngine() {
  options.engine.debug = options.debug;
  debugLog(options.debug, "config", QString("load %1: %2 nodes, %3 channels")
      .arg(doc.filename).arg(doc.nodes().size()).arg(doc.channels().size()));
  heartbeatTimer.stop();
  if (analytics) analytics->detach();
  if (notifications) notifications->detach();
  engine.reset();
  ca.reset();

  if (connectPv && !options.editor) {
    ca = std::make_unique<PvRouter>(options.engine);
    ca->error = [this](const QString& s) { error(s); };
    ca->configureInitialAckT(doc);
  }
  engine = std::make_unique<Engine>(doc, options.engine, ca.get());
  engine->silenceForever = options.silent;
  engine->error = [this](const QString& s) { error(s); };
  engine->changed = [this] { dirty = true; };
  if (!logging) {
    logging = std::make_unique<Logging>(options, doc.root->name);
    logging->error = [this](const QString& s) { error(s); };
    logging->message = [this](const QString& s) {
      // A Reload Facility broadcast replaces the document right after this
      // message; the copied text holds no node references, so keep the reason
      // readable across that reload, as ALH does.
      showText("Broadcast Message", s)->setProperty("keepOnReplace", true);
    };
    logging->reload = [this] {
      QTimer::singleShot(0, this, [this] {
        // replace() closes, clears or deletes every dialog of this window, so it
        // waits for their file choosers, editors and warnings to return.
        for (auto dialog : findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly))
          if (hostsModal(dialog)) {
            QTimer::singleShot(250, this, [this] { logging->reload(); });
            return;
          }
        try {
          replace(loadConfig(doc.filename, options.configDir), false);
          // Audit only a validated reload, never editor snapshots or a rejected file.
          logging->operation(nullptr, "Setup Config File : " + doc.filename);
          showLoadWarnings();
        } catch (const std::exception& e) {
          error(e.what());
        }
      });
    };
  }
  // setupEngine runs only after the replacement document has validated.
  logging->setFacility(doc.root->name, doc.root->label());
  // Root edits, undo/redo and reloads all replace the document through here.
  setWindowTitle((options.editor ? "Alarm Configuration Tool: " : "Alarm Handler: ") +
                 doc.root->name);
  if (!options.editor) {
    engine->acknowledgement = [this](Node* n) { logging->acknowledgement(n); };
    engine->alarmLog = [this](Node* n, const State& s, qint64 t) { logging->alarm(n, s, t); };
    engine->operation = [this](Node* n, const QString& s, OperationKind kind) { logging->operation(n, s, kind); };
  }
  if (!options.editor) {
    const bool starting = !notifications;
    if (!notifications) notifications = std::make_unique<NotificationService>();
    notifications->operation = [this](const QString& text) { if (logging) logging->operation(nullptr, text); };
    notifications->attach(engine.get(), doc.channels(), doc.filename);
    notifications->setConfiguration(doc);
    // -notifications enables them once at startup; reloads keep the operator's choice.
    if (starting && options.notifications) notifications->enable(true);
    if (!analytics) analytics = std::make_unique<AnalyticsService>();
    analytics->attach(engine.get(), doc.filename);
  }
  engine->command = [this](QString command) {
    debugLog(options.debug, "command", "requested " + command);
    if (logging && !logging->commandsAllowed())
      return;
    if (command.startsWith("MASTER_ONLY") && logging && !logging->isMaster()) return;
    command = launchCommand(command);
#ifdef Q_OS_WIN
    QProcess process;
    process.setProgram(qEnvironmentVariable("COMSPEC", "cmd.exe"));
    process.setNativeArguments("/d /s /c \"" + command + "\"");
    if (!process.startDetached())
#else
    if (!QProcess::startDetached("/bin/sh", {"-c", inheritedLimitShellPrefix() + command}))
#endif
      error("Cannot start command: " + command);
  };
  selection = selectedGroup = doc.root.get();
  configuredBeepSeverity = doc.beepSeverity;
  configuredBeep.clear();
  // Readers default to MINOR, so record only other thresholds.
  for (auto n : doc.nodes())
    if (engine->state(n).beepThreshold != 1) configuredBeep.insert(n, engine->state(n).beepThreshold);
  configuredForce.clear();
  forceEdited.clear();
  for (auto n : doc.nodes()) {
    const auto settings = forceSettings(n);
    if (!settings.isEmpty()) configuredForce.insert(n, settings);
  }
  dirty = true;
}
int Window::modifications(Node* n) const {
  if (options.editor || !engine) return 0;
  const auto& s = engine->state(n);
  int result = 0;
  if (!n->group && s.mask.bits != n->mask.bits) result |= MaskChanged;
  if (s.beepThreshold != configuredBeep.value(n, 1)) result |= BeepChanged;
  if (s.forceDisabled) result |= ForceDisabled;
  if (s.noAckUntil) result |= NoAckTimer;
  if (s.shelf.until) result |= Shelved;
  if (!n->parent && doc.beepSeverity != configuredBeepSeverity) result |= FacilityBeepChanged;
  if (forceEdited.contains(n)) result |= ForceEdited;
  return result;
}
QString Window::modificationText(Node* n) const {
  const int changes = modifications(n);
  if (!changes) return {};
  const auto& s = engine->state(n);
  QStringList lines = {"Changed in this runtime:"};
  if (changes & MaskChanged) lines << "Mask " + s.mask.text() + " (configured " + n->mask.text() + ")";
  if (changes & BeepChanged)
    lines << "Beep severity " + severityName(s.beepThreshold) + " (configured " +
             severityName(configuredBeep.value(n, 1)) + ")";
  if (changes & ForceDisabled) lines << "Force PV disabled";
  if (changes & ForceEdited) lines << "Force PV settings edited";
  if (changes & NoAckTimer) lines << "NoAck for one hour";
  if (changes & Shelved) lines << "Shelved";
  if (changes & FacilityBeepChanged)
    lines << "ALH beep severity " + severityName(doc.beepSeverity) + " (configured " +
             severityName(configuredBeepSeverity) + ")";
  return lines.join('\n');
}
namespace {
// Current Alarm History sorts on a key where text order would mislead: Time is
// chronological, Severity follows its rank, and numeric values sort as numbers
// before other text. Items without a key compare by their text.
class HistoryItem : public QTableWidgetItem {
public:
  bool operator<(const QTableWidgetItem& other) const override {
    const auto mine = data(Qt::UserRole), theirs = other.data(Qt::UserRole);
    if (mine.isValid() != theirs.isValid()) return mine.isValid();
    if (mine.isValid()) return mine.toDouble() < theirs.toDouble();
    return QTableWidgetItem::operator<(other);
  }
};
// ALH's pane-width scale; the trough turns blue with the footer while silenced.
QString paneWidthStyle(const char* trough) {
  return QString("QSlider {background:%1; border:1px solid; border-top-color:#dde6e9; "
                 "border-left-color:#dde6e9; border-bottom-color:#5f696d; border-right-color:#5f696d;}"
                 "QSlider::groove:horizontal {height:11px; margin:1px; background:%1; "
                 "border:1px solid; border-top-color:#5f696d; border-left-color:#5f696d; "
                 "border-bottom-color:#dde6e9; border-right-color:#dde6e9;}"
                 "QSlider::handle:horizontal {width:28px; margin:1px 0; background:#b0c3ca; "
                 "border:1px solid; border-top-color:#dde6e9; border-left-color:#dde6e9; "
                 "border-bottom-color:#5f696d; border-right-color:#5f696d;}")
      .arg(trough);
}
} // namespace
void Window::buildUi() {
  auto central = new QWidget;
  auto layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  auto splitter = new PaneSplitter;
  splitter->setObjectName("alarmPanes");
  // Classic ALH divides the panes with a black rule; the modern rule is subtle.
  splitter->setHandleWidth(legacyAppearance() ? 2 : 5);
  treeView = new AlarmView(true);
  groupView = new AlarmView(false);
  treeView->setObjectName("alarmTree");
  groupView->setObjectName("groupContents");
  treeModel = new AlarmModel(&doc, engine.get(), true, this);
  groupModel = new AlarmModel(&doc, engine.get(), false, this);
  if (!options.editor)
    for (auto model : {treeModel, groupModel}) {
      model->modifications = [this](Node* n) { return modifications(n); };
      model->modificationText = [this](Node* n) { return modificationText(n); };
    }
  treeModel->coloredMasks = groupModel->coloredMasks = options.maskColor;
  treeView->setModel(treeModel);
  groupView->setModel(groupModel);
  for (auto view : {treeView, groupView})
    splitter->addWidget(view);
  splitter->setSizes({500, 500});
  layout->addWidget(splitter, 1);
  if (legacyAppearance()) {
    // ALH's width slider; the modern splitter handle is dragged directly.
    auto divider = new QSlider(Qt::Horizontal);
    divider->setObjectName("paneWidth");
    divider->setRange(5, 95);
    divider->setValue(50);
    divider->setFixedHeight(17);
    divider->setToolTip("Tree/group window width");
    divider->setStyleSheet(paneWidthStyle("#b0c3ca"));
    paneWidth = divider;
    connect(divider, &QSlider::valueChanged, splitter, [splitter](int value) {
      int width = splitter->width() - splitter->handleWidth();
      splitter->setSizes({width * value / 100, width * (100 - value) / 100});
    });
    connect(splitter, &QSplitter::splitterMoved, divider, [splitter, divider](int, int) {
      auto sizes = splitter->sizes();
      QSignalBlocker block(divider);
      if (sizes[0] + sizes[1])
        divider->setValue(100 * sizes[0] / (sizes[0] + sizes[1]));
    });
    layout->addWidget(divider);
  }
  if (!options.editor) {
    for (auto view : {treeView, groupView}) {
      view->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& point) {
        const QPersistentModelIndex index(view->indexAt(point));
        auto model = static_cast<AlarmModel*>(view->model());
        auto target = model ? model->node(index) : nullptr;
        if (!target) return;
        auto menu = new QMenu(view);
        menu->setObjectName("alarmContextMenu");
        if (options.shelving) {
          auto shelve = menu->addAction(target->group ? "Shelve this group..." : "Shelve this channel...");
          shelve->setObjectName("shelveContextTarget");
          connect(shelve, &QAction::triggered, this, [this, model, index] {
            if (index.isValid()) shelveDialog(model->node(index));
          });
        }
        auto details = menu->addAction("Alarm Handler Properties...");
        details->setObjectName("propertiesContextTarget");
        connect(details, &QAction::triggered, this, [this, model, index] {
          if (!index.isValid()) return;
          selection = model->node(index);
          scheduleDialogSync();
          properties();
        });
        connect(model, &QAbstractItemModel::modelReset, menu, &QMenu::close);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        menu->popup(view->viewport()->mapToGlobal(point));
      });
    }
  }
  connect(treeView, &QTreeView::clicked, this, [this](QModelIndex i) { click(i, treeModel); });
  connect(groupView, &QTreeView::clicked, this, [this](QModelIndex i) { click(i, groupModel); });
  for (auto view : {treeView, groupView})
    connect(view, &QTreeView::doubleClicked, this, [this, view](QModelIndex i) {
      auto model = static_cast<AlarmModel*>(view->model());
      auto n = model->node(i);
      if (!n || !n->group) return;
      const auto index = treeIndex(n);
      if (!index.isValid()) return;
      if (i.column() == 3) {
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
          treeView->expand(parent);
        expandBranch(index);
      } else if (i.column() == 2 && view == groupView) {
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
          treeView->expand(parent);
        treeView->setCurrentIndex(index.sibling(index.row(), 2));
        select(n);
        treeView->scrollTo(index);
      }
    });
  connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            auto n = treeModel->node(i);
            if (n && (i.column() == 0 || i.column() == 2))
              select(n);
          });
  connect(groupView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            auto n = groupModel->node(i);
            if (n)
              selection = n;
            scheduleDialogSync();
          });
  execution = new QLabel;
  execution->setObjectName("executionStatus");
  disabledForceLabel = new QLabel;
  disabledForceLabel->setObjectName("disabledForcePvCount");
  shelvedLabel = new QLabel;
  shelvedLabel->setObjectName("shelvedCount");
  shelvedLabel->setTextFormat(Qt::RichText);
  connect(shelvedLabel, &QLabel::linkActivated, this, [this] { showShelvedAlarms(); });
  filename = new QLabel;
  filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  silenceBox = new MotifCheckBox("Silence 30 minutes");
  currentBox = new MotifCheckBox("Silence current");
  silenceBox->setObjectName("silenceInterval");
  currentBox->setObjectName("silenceCurrent");
  silenceForeverLabel = new QLabel;
  silenceForeverLabel->setObjectName("silenceForever");
  beepLabel = new QLabel;
  beepLabel->setObjectName("beepSeverity");
  notificationsEnabledLabel = new QLabel;
  notificationsEnabledLabel->setObjectName("notificationsEnabledStatus");
  notificationsEnabledLabel->setToolTip("Automatic notifications in this runtime. Change in Setup → Notifications….");
  displayFilterLabel = new QLabel;
  displayFilterLabel->setObjectName("displayFilterStatus");
  displayFilterLabel->setWordWrap(false);
  displayFilterLabel->setToolTip("Some channels are hidden by Setup → Display Filter.");
  displayFilterLabel->hide();
  const QStringList legendLines = {
      "Mask <CDATL>:  <Cancel,Disable,noAck,noackT,noLog>     H=noAck 1hr timer",
      "Group Alarm Counts:  (ERROR,INVALID,MAJOR,MINOR,NOALARM)",
      "Channel Alarm Data: <Status,Severity> (<Unack Severity>)"};
  if (legacyAppearance()) {
    footer = new QWidget;
    footer->setObjectName("statusFooter");
    footer->setAutoFillBackground(true); // Painted blue during a silence interval.
    footer->setMinimumHeight(114);
    auto bottom = new QHBoxLayout(footer);
    bottom->setContentsMargins(10, 4, 4, 17);
    auto info = new QVBoxLayout;
    info->setSpacing(2);
    auto statusRow = new QHBoxLayout;
    statusRow->addWidget(execution);
    statusRow->addStretch();
    statusRow->addWidget(disabledForceLabel);
    statusRow->addWidget(shelvedLabel);
    statusRow->addWidget(displayFilterLabel);
    info->addLayout(statusRow);
    for (const auto& line : legendLines) info->addWidget(new QLabel(line));
    info->addWidget(filename);
    bottom->addLayout(info, 1);
    auto silence = new QVBoxLayout;
    silence->setSpacing(4);
    silence->setAlignment(Qt::AlignRight);
    QFont toggleFont("monospace");
    toggleFont.setPixelSize(13);
    toggleFont.setStretch(100);
    silenceBox->setFont(toggleFont);
    currentBox->setFont(toggleFont);
    // Each line occupies the same height, including the status labels.
    for (QWidget* row : {static_cast<QWidget*>(silenceBox), static_cast<QWidget*>(currentBox),
                         static_cast<QWidget*>(silenceForeverLabel), static_cast<QWidget*>(beepLabel),
                         static_cast<QWidget*>(notificationsEnabledLabel)}) {
      if (auto label = qobject_cast<QLabel*>(row)) label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      row->setFixedHeight(20);
      silence->addWidget(row, 0, Qt::AlignRight);
    }
    bottom->addLayout(silence);
  } else {
    // Flat status area: facility state on the left, alarm sound on the right.
    footer = new QWidget;
    footer->setObjectName("statusFooter");
    footer->setAutoFillBackground(true);
    auto bottom = new QHBoxLayout(footer);
    bottom->setContentsMargins(12, 8, 12, 8);
    bottom->setSpacing(16);
    auto info = new QVBoxLayout;
    info->setSpacing(4);
    auto statusRow = new QHBoxLayout;
    statusRow->setSpacing(14);
    execution->setWordWrap(true);
    statusRow->addWidget(execution, 1);
    statusRow->addWidget(disabledForceLabel);
    statusRow->addWidget(shelvedLabel);
    statusRow->addWidget(displayFilterLabel);
    info->addLayout(statusRow);
    filename->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->addWidget(filename);
    auto legend = new QWidget;
    legend->setObjectName("alarmLegend");
    auto legendLayout = new QVBoxLayout(legend);
    legendLayout->setContentsMargins(18, 0, 0, 0);
    legendLayout->setSpacing(2);
    for (const auto& line : legendLines) {
      auto label = new QLabel(line);
      label->setWordWrap(true);
      label->setForegroundRole(QPalette::PlaceholderText);
      legendLayout->addWidget(label);
    }
    auto toggle = new QToolButton;
    toggle->setObjectName("alarmLegendToggle");
    toggle->setText("Alarm legend");
    toggle->setCheckable(true);
    toggle->setAutoRaise(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    legend->hide();
    connect(toggle, &QToolButton::toggled, legend, [toggle, legend](bool open) {
      toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
      legend->setVisible(open);
    });
    info->addWidget(toggle, 0, Qt::AlignLeft);
    info->addWidget(legend);
    info->addStretch();
    bottom->addLayout(info, 1);
    bottom->addWidget(separatorLine(Qt::Vertical));
    auto silence = new QVBoxLayout;
    silence->setSpacing(2);
    for (QWidget* row : {static_cast<QWidget*>(silenceBox), static_cast<QWidget*>(currentBox),
                         static_cast<QWidget*>(silenceForeverLabel), static_cast<QWidget*>(beepLabel),
                         static_cast<QWidget*>(notificationsEnabledLabel)}) {
      if (auto label = qobject_cast<QLabel*>(row)) label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      silence->addWidget(row, 0, Qt::AlignRight);
    }
    silence->addStretch();
    bottom->addLayout(silence);
    layout->addWidget(separatorLine(Qt::Horizontal));
  }
  layout->addWidget(footer);
  connect(silenceBox, &QCheckBox::toggled, this, [this](bool yes) {
    engine->setSilenceUntil(yes ? engine->monotonicNow() + silenceMinutes * 60000 : 0);
    refreshStatus();
  });
  connect(currentBox, &QCheckBox::toggled, this,
          [this](bool yes) { engine->setSilenceCurrent(yes); });
  messageArea = new QLabel;
  messageArea->setObjectName("messageArea");
  messageArea->setWordWrap(true);
  if (legacyAppearance()) {
    messageBanner = messageArea;
  } else {
    // Errors appear in a dismissible warning strip instead of a bare line.
    auto banner = new QFrame;
    banner->setObjectName("messageBanner");
    banner->setAutoFillBackground(true);
    auto colors = banner->palette();
    const bool dark = colors.color(QPalette::Window).lightness() < 128;
    colors.setColor(QPalette::Window, QColor(dark ? "#3d2e0a" : "#fff4d6"));
    colors.setColor(QPalette::WindowText, QColor(dark ? "#fde68a" : "#5c3d00"));
    banner->setPalette(colors);
    auto row = new QHBoxLayout(banner);
    row->setContentsMargins(12, 6, 6, 6);
    auto icon = new QLabel;
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16, 16));
    row->addWidget(icon, 0, Qt::AlignTop);
    row->addWidget(messageArea, 1);
    auto dismiss = new QToolButton;
    dismiss->setObjectName("dismissMessage");
    dismiss->setAutoRaise(true);
    dismiss->setText(QStringLiteral("✕"));
    dismiss->setToolTip("Dismiss message");
    connect(dismiss, &QToolButton::clicked, banner, &QWidget::hide);
    row->addWidget(dismiss, 0, Qt::AlignTop);
    messageBanner = banner;
  }
  messageBanner->hide();
  layout->addWidget(messageBanner);
  setCentralWidget(central);
  treeModel->setFilter(options.filter);
  groupModel->setFilter(options.filter);
  treeView->expand(treeModel->index(0, 0));
  treeView->setCurrentIndex(treeModel->index(0, 2));
  runtime = new QWidget(nullptr, Qt::Window);
  runtime->setWindowTitle("Alarm Handler");
  runtime->setObjectName("runtimeWindow");
  runtime->setWindowIcon(windowIcon());
  if (legacyAppearance()) runtime->setFont(font());
  if (legacyAppearance()) runtime->setStyleSheet("#runtimeWindow {border:2px outset #b0c3ca;}");
  auto rlayout = new QHBoxLayout(runtime);
  rlayout->setContentsMargins(6, 5, 6, 5);
  runtimeButton = new MotifButton(doc.root->label());
  // Native platform button fonts can override inherited fonts under a stylesheet.
  // Keep the compact legacy button on the explicitly selected legacy font.
  if (legacyAppearance()) runtimeButton->setFont(font());
  runtimeButton->setObjectName("runtimeAlarm");
  runtimeButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  rlayout->addWidget(runtimeButton);
  runtimeNotificationsEnabledLabel = new QLabel(runtime);
  runtimeNotificationsEnabledLabel->setObjectName("runtimeNotificationsEnabledStatus");
  runtimeNotificationsEnabledLabel->setToolTip(notificationsEnabledLabel->toolTip());
  runtimeNotificationsEnabledLabel->hide();
  rlayout->addWidget(runtimeNotificationsEnabledLabel);
  if (legacyAppearance()) runtime->resize(220, 35);
  else {
    rlayout->setContentsMargins(4, 4, 4, 4);
    runtimeButton->setToolTip("Show the Alarm Handler main window");
    runtime->setMinimumSize(220, 35);
    runtime->adjustSize();
  }
  connect(runtimeButton, &QPushButton::clicked, this, [this] {
    show();
    raise();
    activateWindow();
  });
  runtimeButton->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(runtimeButton, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
    QMenu menu(runtimeButton);
    menu.setObjectName("runtimeContextMenu");
    connect(menu.addAction("Open Main Window"), &QAction::triggered, this, [this] {
      show(); raise(); activateWindow();
    });
    connect(menu.addAction("Active Alarms..."), &QAction::triggered, this, [this] { showActiveAlarms(); });
    connect(menu.addAction("Alarm Summary Panel..."), &QAction::triggered, this, [this] { showSummary(); });
    menu.exec(runtimeButton->mapToGlobal(point));
  });
  runtime->installEventFilter(this);
  if (!options.font.isEmpty()) {
    QFont f;
    auto parts = options.font.split('-');
    if (options.font.startsWith('-') && parts.size() > 7) {
      f = QFont(parts.value(2), 10);
      bool ok = false;
      int px = parts.value(7).toInt(&ok);
      if (ok && px > 0)
        f.setPixelSize(px);
      f.setBold(parts.value(3) == "bold");
    } else if (!f.fromString(options.font))
      f = QFont(options.font);
    setPresentationFont(runtimeButton, f);
  }
}
void Window::showInitial() {
  if (options.editor || options.mainWindow)
    show();
  if (!options.editor)
    runtime->show();
  // Apply after showing, when native frame dimensions are known. Default
  // runtime launches position the compact window; -mainwindow selects the main view.
  applyInitialGeometry(options.editor || options.mainWindow ? this : runtime, options.geometry);
}
void Window::error(const QString& s) {
  // Logging failures also arrive here. Audit once without recursively trying
  // to write the diagnostic for an unavailable operation log.
  if (logging && !auditingError) {
    QScopedValueRollback<bool> guard(auditingError, true);
    logging->operation(nullptr, s);
  }
  qWarning("QtALH: %s", qPrintable(s));
  showMessage(s);
  if (!options.noErrorPopup && (isVisible() || (runtime && runtime->isVisible()))) {
    if (!errorPopup) {
      auto popup = new QMessageBox(QMessageBox::Warning, "ALH Message", s, QMessageBox::Ok, this);
      popup->setAttribute(Qt::WA_DeleteOnClose);
      errorPopup = popup;
      popup->show();
    } else
      errorPopup->setDetailedText((errorPopup->detailedText() + "\n" + s).right(16000));
  }
}
// Loading notes are shown once per file load, not for each editor round trip.
void Window::showLoadWarnings() {
  if (doc.warnings.isEmpty()) return;
  for (const auto& warning : doc.warnings) qWarning("QtALH: %s", qPrintable(warning));
  showMessage(doc.warnings.join('\n'));
}
void Window::showMessage(const QString& text) {
  if (!messageArea) return;
  messageArea->setText(text);
  messageBanner->show();
}
void Window::refresh() {
  dirty = false;
  // Keep expansion and action targets synchronized as filtered rows appear or
  // disappear. Surviving rows retain their indexes through incremental updates.
  QSet<Node*> expanded;
  Node* currentTree = treeModel->node(treeView->currentIndex());
  Node* currentGroup = groupModel->node(groupView->currentIndex());
  std::function<void(QModelIndex, bool)> visit = [&](QModelIndex parent, bool restore) {
    for (int row = 0; row < treeModel->rowCount(parent); ++row) {
      auto index = treeModel->index(row, 0, parent);
      auto node = treeModel->node(index);
      if (restore) {
        treeView->setExpanded(index, expanded.contains(node));
        if (node == currentTree)
          treeView->setCurrentIndex(index);
      } else if (treeView->isExpanded(index))
        expanded.insert(node);
      visit(index, restore);
    }
  };
  if (options.filter)
    visit({}, false);
  QSignalBlocker treeSignals(treeView->selectionModel());
  QSignalBlocker groupSignals(groupView->selectionModel());
  treeModel->refresh();
  groupModel->refresh();
  if (options.filter) {
    visit({}, true);
    for (int row = 0; row < groupModel->rowCount(); ++row) {
      auto index = groupModel->index(row, 0);
      if (groupModel->node(index) == currentGroup)
        groupView->setCurrentIndex(index);
    }
  }
  if (options.filter) restoreVisibleSelection();
  const int disabledForces = engine->disabledForces();
  disabledForceLabel->setText(disabledForces ? QString("Disabled forcePVs: %1").arg(disabledForces)
                                            : QString());
  refreshStatus();
  refreshShelfList();
  refreshHistory();
  scheduleDialogSync();
  if (undoAction)
    undoAction->setEnabled(!undo.isEmpty());
  if (redoAction)
    redoAction->setEnabled(!redo.isEmpty());
}
void Window::updateSilenceColors() {
  // Both timed and permanent silence have the same visible background. With
  // -maskcolor the panes retain their colors; the footer still turns blue.
  const bool silenced = !options.editor && engine &&
      (engine->silenceForever || engine->silenceUntil > engine->monotonicNow());
  if (silenced == silenceColored)
    return;
  silenceColored = silenced;
  QPalette colors; // Unset roles follow the application palette.
  if (silenced) {
    colors.setColor(QPalette::Window, Qt::blue);
    // Text is left alone: check marks are drawn in it on the white Base box.
    for (auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Link,
                      QPalette::PlaceholderText})
      colors.setColor(role, Qt::white);
  }
  // The classic appearance's application style sheet stops palette
  // inheritance, so each footer widget receives the colors directly.
  footer->setPalette(colors);
  for (auto child : footer->findChildren<QWidget*>())
    child->setPalette(colors);
  if (legacyAppearance()) {
    // The classic message line belongs to ALH's message area.
    messageBanner->setAutoFillBackground(silenced);
    messageBanner->setPalette(colors);
  }
  if (paneWidth)
    paneWidth->setStyleSheet(paneWidthStyle(silenced ? "blue" : "#b0c3ca"));
  for (auto view : {treeView, groupView})
    static_cast<AlarmView*>(view)->setSilenced(silenced && !options.maskColor);
}
// Time-dependent indicators and logging ownership still update while alarms are idle.
void Window::refreshStatus() {
  execution->setText(
      QString("Execution Status: %1 %2%3")
          .arg(options.engine.global ? "Global" : "Local",
               options.engine.passive ? "Passive" : "Active",
               !options.editor && logging && options.lock ? (logging->isMaster() ? " — Master" : " — Slave") : ""));
  filename->setText("Filename:  " + doc.filename + (modified ? " *" : ""));
  filename->setToolTip(doc.filename);
  silenceForeverLabel->setText(
      QString("Silence Forever: %1").arg(engine->silenceForever ? "On" : "Off"));
  updateSilenceColors();
  auto warningBackground = [this](QLabel* label, bool warning) {
    auto colors = footer->palette();
    if (warning) {
      const QColor background("#fbbf24");
      colors.setColor(QPalette::Window, background);
      colors.setColor(QPalette::WindowText, contrastingText(background));
    }
    if (label->palette() != colors) label->setPalette(colors);
    label->setAutoFillBackground(warning);
    auto weight = label->font();
    if (weight.bold() != warning) {
      weight.setBold(warning);
      label->setFont(weight);
    }
  };
  warningBackground(silenceForeverLabel, engine->silenceForever);
  beepLabel->setText(QString("ALH Beep Severity: %1").arg(severityName(doc.beepSeverity)));
  warningBackground(beepLabel, doc.beepSeverity != 1);
  const QStringList filters = {"No filter", "Active Alarms Only", "Unacknowledged Alarms Only"};
  displayFilterLabel->setText("Display Filter: " + filters.value(options.filter, "Unknown"));
  displayFilterLabel->setVisible(!options.editor && options.filter != 0);
  warningBackground(displayFilterLabel, options.filter != 0);
  bool configuredNotifications = false;
  if (notifications) {
    // Resolved once at attach; this runs on every refresh and status tick.
    const auto configuration = notifications->configurationPath();
    for (const auto& subscription : notifications->policy.settings().subscriptions)
      if (subscription.configuration == configuration) {
        configuredNotifications = true;
        break;
      }
  }
  notificationsEnabledLabel->setVisible(configuredNotifications);
  notificationsEnabledLabel->setText(QString("Notifications Enabled: %1")
      .arg(notifications && notifications->policy.enabled() ? "YES" : "NO"));
  runtimeNotificationsEnabledLabel->setVisible(configuredNotifications);
  runtimeNotificationsEnabledLabel->setText(QString("Notifications: %1")
      .arg(notifications && notifications->policy.enabled() ? "ON" : "OFF"));
  {
    QSignalBlocker b(silenceBox);
    silenceBox->setChecked(engine->silenceUntil > engine->monotonicNow());
  }
  {
    QSignalBlocker b(currentBox);
    currentBox->setChecked(engine->silenceCurrent);
  }
  auto& s = engine->state(doc.root.get());
  const auto shown = engine->presentation(doc.root.get());
  const auto shelfLinkColor = silenceColored ? QColor(Qt::white) : footer->palette().color(QPalette::Link);
  shelvedLabel->setText(shown.shelved
      ? QString("<a href='shelves' style='color:%1'>Shelved: %2</a>")
            .arg(shelfLinkColor.name()).arg(shown.shelved)
      : QString());
  shelvedLabel->setVisible(!options.editor && shown.shelved > 0);
  int color = shown.unack ? ((engine->now() / 1000) % 2 ? shown.unack : 0) : shown.severity;
  auto button = static_cast<MotifButton*>(runtimeButton);
  const auto background = severityColor(color);
  if (button->background != background) {
    button->background = background;
    button->update();
  }
  QString mask = "-----";
  for (int bit = 0; bit < 5; ++bit)
    if (s.maskCounts[bit])
      mask[bit] = QString("CDATL")[bit];
  runtimeButton->setText(doc.root->label() + (mask == "-----" ? QString() : "  <" + mask + ">") +
      (shown.shelved ? QString("  [Shelved: %1]").arg(shown.shelved) : QString()));
  // Grow the compact window for longer text or a larger font; neither changes
  // on most status ticks, so skip the layout query then.
  refreshTools();
  const auto runtimeText = runtimeButton->text() +
      (configuredNotifications ? runtimeNotificationsEnabledLabel->text() : QString());
  if (runtimeSizedText != runtimeText || runtimeSizedFont != runtimeButton->font()) {
    runtimeSizedText = runtimeText;
    runtimeSizedFont = runtimeButton->font();
    auto desired = runtime->sizeHint().expandedTo(QSize(220, 35));
    if (runtime->width() < desired.width() || runtime->height() < desired.height()) runtime->resize(desired);
  }
  scheduleDialogSync();
}
void Window::restoreVisibleSelection() {
  const QSignalBlocker treeSignals(treeView->selectionModel());
  const QSignalBlocker groupSignals(groupView->selectionModel());
  QModelIndex groupIndex;
  for (int row = 0; selection && row < groupModel->rowCount(); ++row) {
    const auto index = groupModel->index(row, 2);
    if (groupModel->node(index) == selection) { groupIndex = index; break; }
  }
  const auto treeSelection = selection && selection->group ? treeIndex(selection) : QModelIndex();
  if (groupIndex.isValid()) groupView->setCurrentIndex(groupIndex);
  else if (treeSelection.isValid()) treeView->setCurrentIndex(treeSelection);
  else {
    selection = nullptr;
    treeView->setCurrentIndex({});
    groupView->setCurrentIndex({});
  }
  scheduleDialogSync();
}
void Window::select(Node* n) {
  selection = n;
  scheduleDialogSync();
  if (n->group) {
    selectedGroup = n;
    groupModel->setGroup(n);
  }
}
void Window::click(const QModelIndex& i, AlarmModel* m) {
  auto n = m->node(i);
  if (!n)
    return;
  if (i.column() == 2 || i.column() == 6 || i.column() == 7) {
    selection = n;
    scheduleDialogSync();
  }
  switch (i.column()) {
  case 0:
    engine->acknowledge(n);
    break;
  case 2:
    if (n->group && m == treeModel)
      select(n);
    break;
  case 3:
    if (n->group) {
      auto index = treeIndex(n);
      if (index.isValid()) {
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
          treeView->expand(parent);
        expandOneLevel(index);
      }
    }
    break;
  case 4:
    guidance(n);
    break;
  case 5:
    related(n);
    break;
  case 6:
    masks();
    break;
  case 7:
    beepSeverity(false);
    break;
  }
  refresh();
}
QModelIndex Window::treeIndex(Node* node) const {
  const auto parent = node->parent ? treeIndex(node->parent) : QModelIndex();
  if (node->parent && !parent.isValid()) return {};
  for (int row = 0; row < treeModel->rowCount(parent); ++row) {
    const auto index = treeModel->index(row, 0, parent);
    if (treeModel->node(index) == node) return index;
  }
  return {};
}
void Window::expandOneLevel(const QModelIndex& i) {
  if (!i.isValid()) return;
  const auto row = i.sibling(i.row(), 0);
  if (treeView->isExpanded(row)) {
    treeView->collapse(row);
    return;
  }
  // ALH's EXPANDCOLLAPSE1 opens only immediate children, even after Expand Branch.
  for (int child = 0; child < treeModel->rowCount(row); ++child)
    treeView->collapse(treeModel->index(child, 0, row));
  treeView->expand(row);
}
void Window::expandBranch(const QModelIndex& i) {
  if (!i.isValid()) return;
  // The painted name/arrow/mask cells use nonzero columns, which have no model children.
  const auto row = i.sibling(i.row(), 0);
  treeView->expand(row);
  for (int child = 0; child < treeModel->rowCount(row); ++child)
    expandBranch(treeModel->index(child, 0, row));
}
void Window::menus() {
  auto action = [this](QMenu* m, QString name, std::function<void()> fn,
                       QKeySequence key = {}) -> QAction* {
    auto a = m->addAction(name);
    a->setObjectName(name.remove('&'));
    if (!key.isEmpty())
      a->setShortcut(key);
    connect(a, &QAction::triggered, this, std::move(fn));
    return a;
  };
  auto file = menuBar()->addMenu("&File");
  if (options.editor)
    action(
        file, "&New",
        [this] {
          auto o = options;
          o.config.clear();
          auto d = parseConfig("GROUP NULL NewGroup\n");
          auto w = new Window(std::move(d), o, connectPv);
          w->showInitial();
        },
        QKeySequence("Ctrl+N"));
  action(file, "&Open...", [this] { open(); }, QKeySequence("Ctrl+O"));
  if (options.editor)
    action(file, "&Save", [this] { save(); }, QKeySequence("Ctrl+S"));
  action(file, "Save &As...", [this] { save(true); });
  if (options.editor) {
    action(file, "Activate ALH...", [this] { activateRuntime(); });
    action(file, "Report...", [this] {
      auto p = chooseFile(this, "Report File", {}, "Reports (*.alhReport)", true);
      if (!p.isEmpty()) {
        QFile f(p);
        if (f.open(QIODevice::WriteOnly))
          f.write(writeReport(doc).toLocal8Bit());
        else
          error(f.errorString());
      }
    });
  }
  // Phoebus conversion is a migration tool, so it stays out of operator menus
  // unless QTALH_PHOEBUS_MENU is set. --to-phoebus/--from-phoebus always work.
  const auto phoebusMenu = qEnvironmentVariable("QTALH_PHOEBUS_MENU");
  if (!phoebusMenu.isEmpty() && phoebusMenu != "0") {
    action(file, "Import Phoebus XML...", [this] { importPhoebus(); });
    action(file, "Export Phoebus XML...", [this] { exportPhoebus(); });
  }
  if (!options.editor && sddsAvailable()) {
    auto snapshot = action(file, "Export Alarm Snapshot (SDDS)...", [this] {
      const auto path = chooseFile(this, "Export Alarm Snapshot (SDDS)", {}, "SDDS files (*.sdds)", true);
      if (!path.isEmpty()) try { saveAlarmSnapshot(path); }
        catch (const std::exception& e) { error(QString::fromUtf8(e.what())); }
    });
    snapshot->setObjectName("exportAlarmSnapshotSdds");
  }
  action(file, "Print...", [this] {
    QPrinter printer;
    QPrintDialog dialog(&printer, this);
    if (dialog.exec() == QDialog::Accepted) {
      QTextDocument report;
      report.setPlainText(writeReport(doc));
      report.print(&printer);
    }
  });
  action(file, "&Close", [this] {
    if (!options.editor) {
      exitApplication();
      return;
    }
    quitting = true;
    close();
  });
  if (options.editor)
    action(file, "Close All", [] { QApplication::closeAllWindows(); });
  file->addSeparator();
  action(file, "E&xit", [this] { exitApplication(); }, QKeySequence("Ctrl+X"));
  if (options.editor) {
    auto edit = menuBar()->addMenu("&Edit");
    undoAction = action(edit, "&Undo", [this] { undoEdit(); }, QKeySequence("Alt+Backspace"));
    redoAction = action(edit, "&Redo", [this] { redoEdit(); }, QKeySequence("Ctrl+Y"));
    edit->addSeparator();
    action(edit, "Cu&t", [this] { cut(true); }, QKeySequence("Shift+Delete"));
    action(edit, "&Copy", [this] { cut(false); }, QKeySequence("Ctrl+Insert"));
    action(edit, "&Paste", [this] { paste(); }, QKeySequence("Shift+Insert"));
    action(edit, "C&lear", [this] { clearNode(); });
    auto insert = menuBar()->addMenu("&Insert");
    action(insert, "Group...", [this] {
      bool ok;
      auto name =
          QInputDialog::getText(this, "Insert Group", "Group name:", QLineEdit::Normal, {}, &ok);
      if (ok)
        try {
          addNode(true, name);
        } catch (const std::exception& e) {
          error(e.what());
        }
    });
    action(insert, "Channel...", [this] {
      bool ok;
      auto name =
          QInputDialog::getText(this, "Insert Channel", "PV name:", QLineEdit::Normal, {}, &ok);
      if (ok)
        try {
          addNode(false, name);
        } catch (const std::exception& e) {
          error(e.what());
        }
    });
    action(insert, "Configuration File...", [this] { open(true); });
  } else {
    auto a = menuBar()->addMenu("&Action");
    action(a, "Acknowledge Alarm", [this] {
      if (selection) engine->acknowledge(selection);
      refresh();
    });
    action(a, "Display Guidance", [this] { guidance(); });
    action(a, "Start Related Process", [this] { related(); });
    action(a, "Force Process Variable...", [this] { forceDialog(); });
    action(a, "Force Mask...", [this] { masks(true); });
    action(a, "Modify Mask Settings...", [this] { masks(); });
    action(a, "Beep Severity...", [this] { beepSeverity(false); });
    if (options.shelving) {
      action(a, "Shelve Alarms...", [this] { shelveDialog(selection); });
      action(a, "Unshelve Alarms", [this] { engine->unshelve(selection); refresh(); });
    }
    action(a, "NoAck for One Hour...", [this] { noAckDialog(); });
    if (options.broadcast) {
      a->addSeparator();
      action(a, "Send Message...", [this] { broadcast(0); });
      action(a, "Stop Alarm Logging...", [this] { broadcast(1); });
      action(a, "Reload Facility...", [this] { broadcast(2); });
    }
  }
  auto view = menuBar()->addMenu("&View");
  if (!options.editor) {
    action(view, "Active Alarms...", [this] { showActiveAlarms(); }, QKeySequence("Ctrl+L"));
    action(view, "Alarm Summary Panel...", [this] { showSummary(); });
    action(view, "Alarm Details...", [this] { alarmDetails(); }, QKeySequence("Ctrl+D"));
    action(view, "Runtime Overrides...", [this] { showOverrides(); });
    if (options.shelving) action(view, "Shelved Alarms...", [this] { showShelvedAlarms(); });
    action(view, "Alarm Analytics...", [this] { showAnalytics(); });
  }
  action(view, "Find Alarm...", [this] { findAlarm(); }, QKeySequence("Ctrl+F"));
  view->addSeparator();
  action(
      view, "Expand One Level", [this] { expandOneLevel(treeView->currentIndex()); },
      QKeySequence("+"));
  action(
      view, "Expand Branch", [this] { expandBranch(treeView->currentIndex()); }, QKeySequence("*"));
  action(view, "Expand All", [this] { treeView->expandAll(); }, QKeySequence("Ctrl+*"));
  action(
      view, "Collapse Branch", [this] {
        const auto index = treeView->currentIndex();
        if (index.isValid()) treeView->collapse(index.sibling(index.row(), 0));
      },
      QKeySequence("-"));
  if (!options.editor) {
    action(view, "Current Alarm History", [this] { showHistory(); });
    action(view, "Configuration File", [this] {
      // The running configuration with the comments and blank lines of its
      // source files. Runtime and unsaved editor changes appear in place;
      // each INCLUDE line is followed by its file, indented as in ALH.
      showText("Configuration File", writeConfig(doc, ConfigText::Listing), true);
    });
    action(view, "Alarm Log File", [this] { showLiveLog(true); });
    action(view, "Browser for Alarm Log",
           [this] { showLogBrowser(true); });
    action(view, "Operation Log File",
           [this] { showLiveLog(false); });
    action(view, "Browser for Operation Log",
           [this] { showLogBrowser(false); });
  }
  action(view, "Properties Window", [this] { properties(); });
  if (!options.editor) {
    auto setup = menuBar()->addMenu("&Setup");
    action(setup, "Notifications...", [this] { showNotifications(); });
    auto filterMenu = setup->addMenu("Display Filter...");
    auto filterGroup = new QActionGroup(filterMenu);
    const QStringList filters = {"No filter", "Active Alarms Only", "Unacknowledged Alarms Only"};
    for (int i = 0; i < filters.size(); ++i) {
      auto item = filterMenu->addAction(filters[i]);
      item->setCheckable(true); item->setChecked(options.filter == i);
      filterGroup->addAction(item);
      connect(item, &QAction::triggered, this, [this, i] {
        const QSignalBlocker treeSignals(treeView->selectionModel());
        const QSignalBlocker groupSignals(groupView->selectionModel());
        options.filter = i; treeModel->setFilter(i); groupModel->setFilter(i);
        treeView->expandAll();
        restoreVisibleSelection();
        refreshStatus();
      });
    }
    auto beepMenu = setup->addMenu("ALH Beep Severity...");
    // TODO: Possible inherited ALH bug, deferred: global beep threshold changes
    // have no operation audit entry, unlike per-node changes in Engine::setBeep.
    // Raising this threshold can silence alarms without a trace; address later.
    auto beepGroup = new QActionGroup(beepMenu);
    for (int i = 1; i < 5; ++i) {
      auto item = beepMenu->addAction(severityName(i));
      item->setCheckable(true); item->setChecked(doc.beepSeverity == i);
      beepGroup->addAction(item);
      connect(item, &QAction::triggered, this, [this, i] { doc.beepSeverity = i; refresh(); });
    }
    connect(beepMenu, &QMenu::aboutToShow, this, [this, beepGroup] {
      for (auto action : beepGroup->actions()) action->setChecked(action->text() == severityName(doc.beepSeverity));
    });
    auto intervalMenu = setup->addMenu("Silence Time Interval");
    auto intervalActions = new QActionGroup(intervalMenu);
    for (int minutes : {5, 10, 15, 30, 60}) {
      auto item = intervalMenu->addAction(QString::number(minutes) + " minutes");
      item->setCheckable(true);
      item->setChecked(minutes == silenceMinutes);
      intervalActions->addAction(item);
      connect(item, &QAction::triggered, this, [this, minutes] {
        // ALH ends active timed silence when a different interval is selected.
        // Selecting the same interval must not restart the existing countdown.
        if (silenceMinutes != minutes) engine->setSilenceUntil(0);
        silenceMinutes = minutes;
        silenceBox->setText(QString("Silence %1 minutes").arg(minutes));
        if (logging) logging->operation(nullptr, minutes == 60 ? "Silence interval set to 1 hour"
            : QString("Silence interval set to %1 minutes").arg(minutes));
        refresh();
      });
    }
    auto silenceForeverAction = setup->addAction("Silence Forever");
    silenceForeverAction->setCheckable(true);
    silenceForeverAction->setChecked(engine->silenceForever);
    connect(silenceForeverAction, &QAction::toggled, this, [this](bool yes) {
      options.silent = yes;
      engine->setSilenceForever(yes);
      refresh();
    });
  }
  if (!options.editor) {
    const QHash<QString, QString> shortcuts = {
        {"Acknowledge Alarm", "Ctrl+A"},     {"Display Guidance", "Ctrl+G"},
        {"Start Related Process", "Ctrl+P"}, {"Force Process Variable...", "Ctrl+V"},
        {"Force Mask...", "Ctrl+M"},         {"Modify Mask Settings...", "Ctrl+S"},
        {"Beep Severity...", "Ctrl+B"},      {"NoAck for One Hour...", "Ctrl+N"}};
    for (auto a : findChildren<QAction*>())
      if (shortcuts.contains(a->text()))
        a->setShortcut(QKeySequence(shortcuts[a->text()]));
  }
  // Menu actions are created only here; selection changes enable them often.
  const QStringList selectedActions = {"Acknowledge Alarm", "Display Guidance", "Start Related Process",
      "Force Process Variable...", "Force Mask...", "Modify Mask Settings...", "Beep Severity...",
      "Shelve Alarms...", "Unshelve Alarms", "NoAck for One Hour...", "Properties Window", "Alarm Details..."};
  selectionActions.clear();
  for (auto a : findChildren<QAction*>())
    if (selectedActions.contains(a->text())) selectionActions.push_back(a);
  auto help = new QMenu("Help", this);
  if (!legacyAppearance()) menuBar()->addMenu(help);
  else {
    QFont menuFont("monospace");
    menuFont.setPixelSize(13);
    menuFont.setStretch(100);
    menuBar()->setFont(menuFont);
    menuBar()->setFixedHeight(30);
    menuBar()->setStyleSheet(
        "QMenuBar {border:1px solid; border-top-color:#dde6e9; border-left-color:#dde6e9; "
        "border-bottom-color:#5f696d; border-right-color:#5f696d;} "
        "QMenuBar::item {padding:2px 7px; "
        "background:transparent;} QMenuBar::item:selected {border:1px inset #b0c3ca;}");
    for (auto menu : findChildren<QMenu*>())
      if (menu != help) menu->setFont(menuFont);
    menuBar()->addSeparator();

    auto helpButton = new QToolButton(menuBar());
    helpButton->setText("Help");
    helpButton->setFont(menuFont);
    helpButton->setStyleSheet(
        "QToolButton {border:0; padding:2px 10px;} QToolButton::menu-indicator {image:none;}");
    helpButton->setMenu(help);
    helpButton->setPopupMode(QToolButton::InstantPopup);
    menuBar()->setCornerWidget(helpButton, Qt::TopRightCorner);
  }
  auto contents = action(help, "Help Topics", [this] { showHelp(this, "first-run"); });
  auto helpKeys = QKeySequence::keyBindings(QKeySequence::HelpContents);
  if (!helpKeys.contains(QKeySequence(Qt::Key_F1))) helpKeys.append(QKeySequence(Qt::Key_F1));
  contents->setShortcuts(helpKeys);
  action(help, "Keyboard Shortcuts", [this] { showHelp(this, "keyboard-shortcuts"); });
  action(help, "Online Manual", [] {
    QDesktopServices::openUrl(QUrl("https://ops.aps.anl.gov/manuals/QtALH/"));
  });
  help->addSeparator();
  action(help, "About QtALH", [this] { showAbout(this); });
}
void Window::replace(Document d, bool snapshot) {
  // The summary's tiles name the old top-level groups; rebuild it after the reload.
  const bool summaryOpen = summaryDialog && summaryDialog->isVisible();
  const bool sameRuntime = !options.editor && !snapshot &&
      QFileInfo(d.filename).absoluteFilePath() == QFileInfo(doc.filename).absoluteFilePath();
  const auto savedShelves = sameRuntime ? engine->shelves() : QVector<ShelfSnapshot>();
  QHash<QString, int> savedNotificationAcks;
  if (sameRuntime && notifications) {
    const auto active = notifications->policy.activeChannels();
    QHash<QString, int> counts;
    for (auto n : doc.channels()) ++counts[Engine::nodeIdentity(n)];
    for (auto n : doc.channels()) {
      auto id = Engine::nodeIdentity(n);
      if (counts[id] == 1 && active.contains(id)) savedNotificationAcks[id] = engine->state(n).unack;
    }
  }
  Node* runtimeSelection = nullptr;
  if (sameRuntime && selection) {
    const auto id = Engine::nodeIdentity(selection);
    int oldMatches = 0, newMatches = 0;
    for (auto n : doc.nodes()) if (Engine::nodeIdentity(n) == id) ++oldMatches;
    for (auto n : d.nodes()) if (Engine::nodeIdentity(n) == id) {
      runtimeSelection = n;
      ++newMatches;
    }
    if (oldMatches != 1 || newMatches != 1) runtimeSelection = nullptr;
  }
  debugLog(options.debug, "config", snapshot ? "apply editor change" : "reload configuration");
  // Serialization emits channels before groups. Track the ordinal within each
  // kind so that a mixed child list cannot redirect an open Properties dialog.
  QVector<QPair<bool, int>> selectedPath;
  for (auto n = selection; n && n->parent; n = n->parent) {
    int ordinal = 0;
    for (const auto& child : n->parent->children) {
      if (child.get() == n) break;
      if (child->group == n->group) ++ordinal;
    }
    selectedPath.prepend(qMakePair(n->group, ordinal));
  }
  if (snapshot) {
    redo.clear();
    rememberEdit(undo, writeConfig(doc, ConfigText::Edit));
    modified = true;
  }
  // These tools retain raw node pointers. WA_DeleteOnClose defers destruction
  // until the event loop, so dispose of them before replacing the document or
  // refreshing/reopening the summary against the new engine.
  for (auto dialog : {activeAlarmsDialog, findDialog, overridesDialog, summaryDialog})
    delete dialog.data();
  // Detach views before old model objects are released. Dialogs marked
  // keepOnReplace hold no document, node or engine state and stay open.
  for (auto dialog : findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly)) {
    // The reused Help window holds no configuration state.
    if (dialog->objectName() == "helpWindow") continue;
    if (selectionDialogs.contains(dialog->objectName()) &&
        selectionDialogs[dialog->objectName()].window == dialog) {
      clearDialogContent(dialog);
      selectionDialogs[dialog->objectName()].node = nullptr;
    } else if (!dialog->property("keepOnReplace").toBool()) {
      dialog->setEnabled(false);
      dialog->close();
    }
  }
  treeView->setModel(nullptr);
  groupView->setModel(nullptr);
  if (analytics) analytics->detach();
  if (notifications) notifications->detach();
  engine.reset();
  ca.reset();
  doc = std::move(d);
  setupEngine();
  engine->restoreShelves(savedShelves);
  engine->restoreLocalAcknowledgements(savedNotificationAcks);
  treeModel->reset(&doc, engine.get());
  groupModel->reset(&doc, engine.get());
  treeView->setModel(treeModel);
  groupView->setModel(groupModel);
  connect(treeView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            // Same filter as buildUi(): mask/beep cells set the action target only.
            auto n = treeModel->node(i);
            if (n && (i.column() == 0 || i.column() == 2))
              select(n);
          });
  connect(groupView->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](QModelIndex i, QModelIndex) {
            if (auto n = groupModel->node(i))
              selection = n;
            scheduleDialogSync();
          });
  auto restored = doc.root.get();
  if (sameRuntime) restored = runtimeSelection;
  for (const auto& step : sameRuntime ? QVector<QPair<bool, int>>() : selectedPath) {
    Node* next = nullptr;
    int ordinal = 0;
    for (const auto& child : restored->children)
      if (child->group == step.first && ordinal++ == step.second) {
        next = child.get();
        break;
      }
    if (!next) break;
    restored = next;
  }
  if (!restored) {
    select(doc.root.get());
    selection = nullptr;
    for (const auto& key : selectionDialogs.keys()) {
      auto dialog = selectionDialogs.value(key).window;
      if (dialog) dialog->close();
    }
    showMessage("The selected alarm was removed or is ambiguous after reload. Select an alarm to continue.");
  } else {
    showSelection(restored);
  }
  treeView->expandAll();
  if (!options.editor)
    engine->start();
  scheduleHeartbeat();
  refresh();
  if (summaryOpen) showSummary();
}
void Window::open(bool insert) {
  if (insert && !selection) {
    error("Select a destination group before inserting a configuration.");
    return;
  }
  auto path = chooseFile(this, "Alarm Configuration File", options.configDir,
                         "Alarm configurations (*.alhConfig);;All files (*)");
  if (path.isEmpty())
    return;
  try {
    auto d = loadConfig(path, options.configDir);
    if (insert) {
      if (!selection) {
        error("Select a destination group before inserting a configuration.");
        return;
      }
      auto parent = selection->group ? selection : selection->parent;
      // The inserted configuration, with its INCLUDE files, joins the file of
      // the group it is added to.
      auto c = cloneNode(*d.root, parent, false);
      parent->children.push_back(c);
      auto next = writeConfig(doc, ConfigText::Edit);
      const auto moved = hierarchyChange(doc, next);
      parent->children.pop_back();
      if (!moved.isEmpty())
        throw ParseError(moved);
      auto result = parseConfig(next);
      result.filename = doc.filename;
      result.latin1 = doc.latin1;
      replace(std::move(result));
    } else if (!options.editor) {
      // ALH passes its area to setupConfig, which cancels the previous
      // configuration's Channel Access and heartbeat before reusing the window.
      replace(std::move(d), false);
      logging->operation(nullptr, "Setup Config File : " + doc.filename);
      showLoadWarnings();
    } else {
      // Every edit saves and reloads the configuration text; refuse a file
      // that would not reload with the same hierarchy.
      writeCheckedConfig(d);
      auto o = options;
      o.config = path;
      auto w = new Window(std::move(d), o, connectPv);
      w->showInitial();
    }
  } catch (const std::exception& e) {
    error(e.what());
  }
}
void Window::save(bool as) {
  auto path = doc.filename;
  if (as || path.isEmpty())
    path = chooseFile(this, "Save Configuration", path, "Alarm configurations (*.alhConfig)", true);
  if (!path.isEmpty())
    try {
      saveTo(path);
    } catch (const std::exception& e) {
      error(e.what());
    }
}
namespace {
bool readsIncludes(const Document& d) {
  for (auto n : d.nodes())
    if (n->origin || !n->includes.isEmpty()) return true;
  return false;
}
} // namespace
void Window::rememberIncludes() {
  savedIncludes.clear();
  if (!readsIncludes(doc)) return;
  const auto files = writeConfigFiles(doc, doc.filename, options.configDir, false);
  for (int i = 1; i < files.size(); ++i) savedIncludes.insert(files[i].path, files[i].text);
}
void Window::saveTo(const QString& p) {
  if (!options.editor) {
    // Runtime Save As exports the current masks, as legacy ALH does, in one
    // file. Keep the monitored facility bound to its original reload,
    // broadcast and lock files.
    Document saved = doc;
    saved.root = cloneNode(*doc.root);
    auto originals = doc.nodes(), copies = saved.nodes();
    for (int i = 0; i < originals.size(); ++i)
      if (!originals[i]->group)
        copies[i]->mask = engine->state(originals[i]).mask;
    saveConfig(saved, p);
    logging->operation(nullptr, "Setup Save New Config: " + p);
    modified = false;
    refresh();
    return;
  }
  const auto path = QFileInfo(p).absoluteFilePath();
  const bool here = !doc.filename.isEmpty() && QFileInfo(doc.filename).absoluteFilePath() == path;
  // The editor keeps INCLUDE lines and writes the INCLUDE files whose groups
  // and channels were edited.
  QVector<ConfigFile> files;
  try {
    files = writeConfigFiles(doc, path, options.configDir);
  } catch (const ParseError&) {
    // From another directory, a relative INCLUDE line can name another file.
    // Save a copy that contains its INCLUDE files instead.
    if (here || !readsIncludes(doc)) throw;
  }
  if (files.isEmpty()) {
    saveConfig(doc, path);
    logging->operation(nullptr, "Setup Save New Config: " + path);
    auto saved = loadConfig(path, options.configDir);
    replace(std::move(saved), false);
    rememberIncludes();
    modified = false;
    refresh();
    showMessage("Saved " + path + " as one file: its INCLUDE lines would name other files there.");
    return;
  }
  QStringList changed, dropped;
  QSet<QString> included;
  for (int i = 1; i < files.size(); ++i) {
    included << files[i].path;
    const auto saved = savedIncludes.constFind(files[i].path);
    if (saved == savedIncludes.cend() || *saved != files[i].text) changed << files[i].path;
  }
  for (auto it = savedIncludes.cbegin(); it != savedIncludes.cend(); ++it)
    if (!included.contains(it.key())) dropped << it.key();
  if (!changed.isEmpty()) {
    QString text = "Saving also changes these INCLUDE files. Other configurations that "
                   "include them will read the changes.\n\n" + changed.join('\n');
    if (!dropped.isEmpty())
      text += "\n\nNo longer included, and left as they are:\n\n" + dropped.join('\n');
    QMessageBox box(QMessageBox::Question, "Save INCLUDE Files", text,
                    QMessageBox::Save | QMessageBox::Cancel, this);
    box.setObjectName("includeSaveConfirmation");
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Save) return;
  }
  // Unchanged INCLUDE files are never rewritten. Write those that change
  // before the file that reads them, and none when one cannot be written.
  QVector<ConfigFile> writes;
  for (int i = 1; i < files.size(); ++i)
    if (changed.contains(files[i].path)) writes << files[i];
  writes << ConfigFile{path, files.front().text};
  QStringList written;
  try {
    saveConfigTexts(doc, writes, written);
    written.removeAll(path);
  } catch (const std::exception& e) {
    written.removeAll(path);
    savedIncludes.clear();
    for (const auto& f : files.mid(1))
      if (written.contains(f.path)) savedIncludes.insert(f.path, f.text);
    throw ParseError(QString::fromUtf8(e.what()) +
                     (written.isEmpty() ? QString() : "\nAlready saved: " + written.join(", ")));
  }
  logging->operation(nullptr, "Setup Save New Config: " + path);
  for (const auto& file : written) logging->operation(nullptr, "Setup Save Include File: " + file);
  savedIncludes.clear();
  for (const auto& f : files.mid(1)) savedIncludes.insert(f.path, f.text);
  doc.filename = path;
  modified = false;
  refresh();
}
void Window::showSelection(Node* n) {
  // The action target, its group's contents and the visible highlight.
  select(n->group ? n : n->parent);
  selection = n;
  const QSignalBlocker treeSignals(treeView->selectionModel());
  const QSignalBlocker groupSignals(groupView->selectionModel());
  treeView->setCurrentIndex(treeIndex(n->group ? n : n->parent));
  if (!n->group)
    for (int row = 0; row < groupModel->rowCount(); ++row) {
      auto index = groupModel->index(row, 2);
      if (groupModel->node(index) == n) groupView->setCurrentIndex(index);
    }
}
void Window::addNode(bool group, const QString& name) {
  if (!options.editor)
    throw ParseError("Configuration edits require -c mode");
  if (name.isEmpty() || name.contains(QRegularExpression("\\s")))
    throw ParseError("Name must be a nonempty token");
  // The new node goes right after the selection: after a channel or subgroup
  // selected among a group's contents, in that node's file, or first in a
  // group selected in the tree. It then becomes the selection, so further
  // insertions follow it; a new group becomes the group shown.
  Node* parent = doc.root.get();
  int index = 0;
  auto n = std::make_shared<Node>();
  if (selection && selection->parent && (!selection->group || selection != selectedGroup)) {
    parent = selection->parent;
    const auto& siblings = parent->children;
    index = int(std::find_if(siblings.begin(), siblings.end(),
                             [this](const auto& c) { return c.get() == selection; }) -
                siblings.begin()) + 1;
    n->origin = selection->origin;
  } else if (selection) {
    parent = selection;
  }
  if (!parent)
    throw ParseError("Select a parent group");
  n->group = group;
  n->name = name;
  n->parent = parent;
  insertChild(*parent, index, n);
  auto text = writeConfig(doc, ConfigText::Edit);
  const auto moved = hierarchyChange(doc, text);
  takeChild(*parent, index);
  if (!moved.isEmpty())
    throw ParseError(moved);
  auto d = parseConfig(text);
  d.filename = doc.filename;
  d.latin1 = doc.latin1;
  // Find the new node in the reloaded document under the same parent, which
  // the restored selection identifies. Writing can move it a little, past
  // INCLUDE files read at the same place or before subgroups (see writeConfig),
  // so take the same name and kind nearest its index.
  const bool inside = !selection || parent == selection;
  replace(std::move(d));
  Node* reloaded = !selection ? nullptr : inside ? selection : selection->parent;
  if (!reloaded)
    return;
  Node* added = nullptr;
  int distance = 0;
  const auto& children = reloaded->children;
  for (int i = 0; i < int(children.size()); ++i)
    if (children[i]->name == name && children[i]->group == group &&
        (!added || qAbs(i - index) < distance)) {
      added = children[i].get();
      distance = qAbs(i - index);
    }
  if (added)
    showSelection(added);
}
void Window::rememberEdit(QStringList& stack, QString snapshot) {
  constexpr qint64 maximumBytes = 32 * 1024 * 1024;
  constexpr int maximumSnapshots = 100;
  auto bytes = [](const QString& s) { return qint64(s.capacity()) * 2 + 128; };
  // A single oversized snapshot cannot fit; discard history rather than keep
  // an apparently adjacent redo step which skips this configuration.
  if (bytes(snapshot) > maximumBytes) {
    undo.clear(); redo.clear();
    return;
  }
  stack.push_back(std::move(snapshot));
  qint64 total = 0;
  for (const auto& s : undo) total += bytes(s);
  for (const auto& s : redo) total += bytes(s);
  while (undo.size() + redo.size() > maximumSnapshots || total > maximumBytes) {
    // Prefer the oldest past state; keep the just-added snapshot so the most
    // recent undo/redo can still be reversed when older history is evicted.
    auto& oldest = !undo.isEmpty() &&
        (&stack != &undo || undo.size() > 1 || redo.isEmpty()) ? undo : redo;
    total -= bytes(oldest.front());
    oldest.removeFirst();
  }
}
void Window::undoEdit() {
  if (undo.isEmpty())
    return;
  auto previous = undo.takeLast();
  rememberEdit(redo, writeConfig(doc, ConfigText::Edit));
  auto d = parseConfig(previous);
  d.filename = doc.filename;
  d.latin1 = doc.latin1;
  replace(std::move(d), false);
  modified = true;
  refresh();
}
void Window::redoEdit() {
  if (redo.isEmpty())
    return;
  auto next = redo.takeLast();
  rememberEdit(undo, writeConfig(doc, ConfigText::Edit));
  auto d = parseConfig(next);
  d.filename = doc.filename;
  d.latin1 = doc.latin1;
  replace(std::move(d), false);
  modified = true;
  refresh();
}
namespace {
bool notificationDestination(const Directive& d) {
  return d.key == "NOTIFY_EMAIL" || d.key == "NOTIFY_WEBHOOK";
}
QString destinationId(const Directive& d) {
  return d.value.trimmed().section(QRegularExpression("\\s+"), 0, 0);
}
bool sameDestination(const Directive& a, const Directive& b) {
  // Conservatively reject different definitions rather than deliver copied
  // alarms to a destination that merely has the same name.
  return a.key == b.key && a.value.simplified() == b.value.simplified();
}
Node* correspondingNode(const Document& before, const Document& after, Node* node) {
  const auto index = before.nodes().indexOf(node);
  return index < 0 ? nullptr : after.nodes().at(index);
}
} // namespace
Document Window::withoutSelection() const {
  if (!selection || !selection->parent)
    throw ParseError("The root group cannot be removed.");
  Document candidate = doc;
  candidate.root = cloneNode(*doc.root);
  auto selected = correspondingNode(doc, candidate, selection);
  auto parent = selected->parent;
  const auto& siblings = parent->children;
  const int offset = int(std::find_if(siblings.begin(), siblings.end(),
                                     [selected](const auto& n) { return n.get() == selected; }) -
                         siblings.begin());
  takeChild(*parent, offset);
  auto result = parseConfig(writeCheckedConfig(candidate, ConfigText::Edit));
  result.filename = doc.filename;
  result.latin1 = doc.latin1;
  return result;
}
void Window::cut(bool remove) {
  if (!selection)
    return;
  Document copied;
  copied.root = std::make_shared<Node>();
  copied.root->group = true;
  copied.root->name = "__QTALH_CLIPBOARD__";
  copied.root->children.push_back(cloneNode(*selection, copied.root.get()));
  QSet<QString> names;
  for (auto node : copied.nodes())
    if (node != copied.root.get() && node->group) names.insert(node->name);
  for (int suffix = 1; names.contains(copied.root->name); ++suffix)
    copied.root->name = "__QTALH_CLIPBOARD__" + QString::number(suffix);
  QString serialized;
  Document remaining;
  try {
    QSet<QString> required, defined;
    for (auto node : copied.nodes())
      for (const auto& d : node->directives)
        if (d.key == "NOTIFY")
          for (const auto& id : notificationDestinationIds(d.value)) required.insert(id);
        else if (notificationDestination(d))
          defined.insert(destinationId(d));
    // Destinations have document-wide scope. Carry only the definitions this
    // subtree needs from elsewhere, independently of the source window.
    for (auto node : doc.nodes())
      for (const auto& d : node->directives)
        if (notificationDestination(d) && required.contains(destinationId(d)) &&
            !defined.contains(destinationId(d))) {
          copied.root->directives << d;
          defined.insert(destinationId(d));
        }
    // Check the actual cross-window payload before publishing it or deleting
    // the source of a Cut. Invalid data must leave both clipboards intact.
    serialized = writeCheckedConfig(copied, ConfigText::Source);
    parseConfig(serialized);
    if (remove) remaining = withoutSelection();
  } catch (const std::exception& e) {
    error(e.what());
    return;
  }
  clipboard = std::move(copied);
  auto mime = new QMimeData;
  mime->setText(selection->name);
  mime->setData("application/x-qtalh-config", serialized.toUtf8());
  QApplication::clipboard()->setMimeData(mime);
  if (remove)
    replace(std::move(remaining));
}
void Window::clearNode() {
  try {
    replace(withoutSelection());
  } catch (const std::exception& e) {
    error(e.what());
  }
}
void Window::paste() {
  if (!selection) {
    error("Select a destination group before pasting.");
    return;
  }
  auto mime = QApplication::clipboard()->mimeData();
  Document copied = clipboard;
  if (mime && mime->hasFormat("application/x-qtalh-config")) {
    try {
      copied = parseConfig(QString::fromUtf8(mime->data("application/x-qtalh-config")));
    } catch (const std::exception& e) {
      error(e.what());
      return;
    }
  }
  if (!copied.root || copied.root->children.empty())
    return;
  auto p = selection->group ? selection : selection->parent;
  if (!p)
    return;
  try {
    Document candidate = doc;
    candidate.root = cloneNode(*doc.root);
    p = correspondingNode(doc, candidate, p);
    // Pasted nodes join the file of the group they are pasted into.
    auto c = cloneNode(*copied.root->children.front(), p, false);
    QHash<QString, Directive> destinations;
    for (auto node : candidate.nodes())
      for (const auto& directive : node->directives)
        if (notificationDestination(directive)) destinations.insert(destinationId(directive), directive);
    auto addDestination = [&](const Directive& directive) {
      const auto id = destinationId(directive);
      const auto existing = destinations.constFind(id);
      if (existing != destinations.cend()) {
        if (!sameDestination(*existing, directive))
          throw ParseError("Notification destination " + id + " has a different definition here");
        return false;
      }
      destinations.insert(id, directive);
      return true;
    };
    Document subtree;
    subtree.root = c;
    for (auto node : subtree.nodes())
      for (int i = 0; i < node->directives.size();)
        if (notificationDestination(node->directives[i]) && !addDestination(node->directives[i]))
          node->directives.removeAt(i);
        else
          ++i;
    for (const auto& directive : copied.root->directives)
      if (notificationDestination(directive) && addDestination(directive))
        candidate.root->directives << directive;
    p->children.push_back(c);
    auto d = parseConfig(writeCheckedConfig(candidate, ConfigText::Edit));
    d.filename = doc.filename;
    d.latin1 = doc.latin1;
    clipboard = std::move(copied);
    replace(std::move(d));
  } catch (const std::exception& e) {
    error(e.what());
  }
}
QString Window::selectionState() const {
  if (!selection) return {};
  const auto& state = engine->state(selection);
  QString result = selection->name + '\n' + selection->mask.text() + '\n' + state.mask.text();
  for (int count : state.maskCounts) result += ':' + QString::number(count);
  result += QString("/%1/%2/%3/%4")
                .arg(state.beepThreshold).arg(state.forceDisabled).arg(state.noAckUntil)
                .arg(doc.beepSeverity);
  for (const auto& directive : selection->directives)
    result += '\n' + directive.key + ' ' + directive.value;
  return result;
}
QDialog* Window::beginSelectionDialog(const QString& key, const QString& title,
                                     std::function<void()> build) {
  auto& entry = selectionDialogs[key];
  if (entry.window && rebuildingDialog != key) {
    entry.window->show();
    entry.window->raise();
    entry.window->activateWindow();
    return nullptr;
  }
  if (!entry.window) {
    entry.window = new QDialog(this);
    entry.window->setAttribute(Qt::WA_DeleteOnClose);
    entry.window->setObjectName(key);
    auto dialog = entry.window.data();
    connect(dialog, &QDialog::finished, this, [this, key, dialog] {
      if (selectionDialogs[key].window == dialog) selectionDialogs.remove(key);
    });
  }
  entry.build = std::move(build);
  entry.node = selection;
  entry.state = selectionState();
  entry.window->setProperty("pendingEdits", false);
  entry.window->setWindowTitle(title);
  return entry.window;
}
void Window::finishSelectionDialog(QDialog* dialog) {
  auto edited = [dialog] { dialog->setProperty("pendingEdits", true); };
  for (auto field : dialog->findChildren<QLineEdit*>())
    connect(field, &QLineEdit::textEdited, dialog, edited);
  for (auto field : dialog->findChildren<QPlainTextEdit*>())
    if (!field->isReadOnly()) connect(field, &QPlainTextEdit::textChanged, dialog, edited);
  for (auto field : dialog->findChildren<QCheckBox*>())
    connect(field, &QCheckBox::clicked, dialog, edited);
  dialog->show();
}
void Window::clearDialogContent(QDialog* dialog) {
  delete dialog->layout();
  qDeleteAll(dialog->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
  qDeleteAll(dialog->findChildren<QButtonGroup*>(QString(), Qt::FindDirectChildrenOnly));
}
void Window::rebuildSelectionDialog(const QString& key) {
  auto entry = selectionDialogs.value(key);
  if (!entry.window || !selection) return;
  auto geometry = entry.window->geometry();
  int tab = 0;
  if (auto tabs = entry.window->findChild<QTabWidget*>("propertyTabs")) tab = tabs->currentIndex();
  QHash<QString, int> scrollPositions;
  for (auto scroll : entry.window->findChildren<QScrollArea*>())
    if (!scroll->objectName().isEmpty()) scrollPositions[scroll->objectName()] = scroll->verticalScrollBar()->value();
  clearDialogContent(entry.window);
  rebuildingDialog = key;
  entry.build();
  rebuildingDialog.clear();
  entry.window->setGeometry(geometry);
  if (auto tabs = entry.window->findChild<QTabWidget*>("propertyTabs")) tabs->setCurrentIndex(tab);
  for (auto scroll : entry.window->findChildren<QScrollArea*>()) {
    int value = scrollPositions.value(scroll->objectName());
    QTimer::singleShot(0, scroll, [scroll, value] { scroll->verticalScrollBar()->setValue(value); });
  }
}
void Window::scheduleDialogSync() {
  for (const auto& action : selectionActions)
    if (action) action->setEnabled(selection != nullptr);
  if (!selection) {
    for (const auto& key : selectionDialogs.keys()) {
      const auto dialog = selectionDialogs.value(key).window;
      if (!dialog) continue;
      if (!hostsModal(dialog)) dialog->close();
      else if (!dialogClosePending) {
        dialogClosePending = true;
        QTimer::singleShot(250, this, [this] {
          dialogClosePending = false;
          scheduleDialogSync();
        });
      }
    }
    return;
  }
  if (dialogSyncPending || selectionDialogs.isEmpty()) return;
  dialogSyncPending = true;
  // Rebuild after the current input callback returns; its controls may be replaced.
  QTimer::singleShot(0, this, [this] {
    dialogSyncPending = false;
    const auto state = selectionState();
    for (const auto& key : selectionDialogs.keys()) {
      const auto entry = selectionDialogs.value(key);
      if (!entry.window) continue;
      // isAncestorOf() stops at window boundaries, so it never sees a modal child window.
      if (QApplication::activeModalWidget() == entry.window || hostsModal(entry.window)) continue;
      if (selection && entry.node == selection) {
        if (auto summary = entry.window->findChild<QLabel*>("currentMaskSummary")) {
          const auto& current = engine->state(selection);
          QString mask = current.mask.text();
          if (selection->group)
            for (int i = 0; i < 5; ++i) mask[i] = current.maskCounts[i] ? QString("CDATL")[i] : QChar('-');
          summary->setText("Current Mask Summary: " + mask);
        }
      }
      if (entry.node != selection ||
          (entry.state != state && !entry.window->property("pendingEdits").toBool()))
        rebuildSelectionDialog(key);
    }
  });
}
void Window::properties() {
  if (!selection) return;
  auto original = selection;
  auto dialog = beginSelectionDialog("propertiesDialog", "Alarm Handler Properties",
                                     [this] { properties(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  QHash<QString, QLineEdit*> edits;
  auto field = [&](const QString& key, const QString& value) {
    auto edit = new QLineEdit(value, dialog);
    edit->setObjectName("property" + key);
    // Count filtering applies to channels only.
    edit->setReadOnly(!options.editor || (original->group && (key == "COUNT" || key == "SECONDS")));
    edits[key] = edit;
    return edit;
  };
  QVector<QPlainTextEdit*> areas;
  auto textArea = [&](const QString& key, int legacyHeight) {
    QStringList lines;
    for (const auto& directive : original->directives)
      if (directive.key == key) lines << directive.value;
    auto edit = new QPlainTextEdit(lines.join('\n'));
    edit->setObjectName("property" + key);
    edit->setReadOnly(!options.editor);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    if (legacyAppearance()) edit->setFixedHeight(legacyHeight);
    else {
      const int lines = key == "GUIDANCE_TEXT" ? 6 : key == "GUIDANCE" ? 2 : 3;
      edit->setMinimumHeight(edit->fontMetrics().height() * lines + 10);
      if (key != "GUIDANCE_TEXT") setPresentationFont(edit, contentFont());
      if (edit->isReadOnly()) {
        // Read-only values read as text, like the single-line fields.
        edit->setFrameShape(QFrame::NoFrame);
        QPalette flat;
        flat.setColor(QPalette::Base, Qt::transparent);
        edit->setPalette(flat);
        edit->setPlaceholderText("Not set");
        if (edit->document()->isEmpty()) edit->setFixedHeight(edit->fontMetrics().height() + 10);
      }
    }
    areas << edit;
    return edit;
  };
  QString currentMask = engine->state(original).mask.text();
  if (original->group)
    for (int i = 0; i < 5; ++i)
      currentMask[i] = engine->state(original).maskCounts[i] ? QString("CDATL")[i] : QChar('-');
  const auto countParts = original->option("ALARMCOUNTFILTER").split(' ', Qt::SkipEmptyParts);
  const auto forceParts = original->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
  QLineEdit *name = nullptr, *mask = nullptr, *count = nullptr, *seconds = nullptr, *forceName = nullptr,
            *beep = nullptr;
  QPlainTextEdit *severity = nullptr, *status = nullptr, *url = nullptr, *guidance = nullptr;
  if (legacyAppearance()) {
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto body = new QWidget;
    auto form = dialogColumn(body);
    scroll->setWidget(body);
    layout->addWidget(scroll);
    auto line = [&](const QString& label, const QString& key) {
      auto row = dialogRow(form);
      row->addWidget(new QLabel(label));
      auto edit = field(key, original->option(key));
      row->addWidget(edit, 1);
      return edit;
    };
    auto top = dialogRow(form);
    top->addWidget(new QLabel(original->group ? "Group" : "Channel"));
    name = field("NAME", original->name); top->addWidget(name, 1);
    auto masks = dialogRow(form);
    auto maskColumn = new QVBoxLayout;
    mask = field("MASK", original->mask.text());
    mask->setMaximumWidth(65);
    if (options.editor && !original->group) {
      auto row = new QHBoxLayout;
      row->addWidget(new QLabel("Alarm Mask")); row->addWidget(mask); row->addStretch();
      maskColumn->addLayout(row);
    } else {
      mask->hide();
      maskColumn->addWidget(new QLabel("Current Mask " + currentMask));
      maskColumn->addWidget(new QLabel("Reset Mask   " + (original->group ? QString() : original->mask.text())));
    }
    masks->addLayout(maskColumn, 1);
    auto filterWidget = new QWidget;
    auto filterLayout = dialogColumn(filterWidget);
    auto filterFrame = dialogFrame(filterLayout, "Alarm Count Filter");
    auto filterRow = dialogRow(filterFrame);
    count = field("COUNT", countParts.value(0)); seconds = field("SECONDS", countParts.value(1));
    count->setMaximumWidth(42); seconds->setMaximumWidth(42);
    filterRow->addWidget(new QLabel("Count")); filterRow->addWidget(count);
    filterRow->addWidget(new QLabel("Seconds")); filterRow->addWidget(seconds);
    masks->addWidget(filterWidget, 1);
    auto forceFrame = dialogFrame(form, "Force Process Variable");
    auto forceRow = dialogRow(forceFrame);
    forceRow->addWidget(new QLabel("PV Name"));
    forceName = field("FORCE_NAME", forceParts.value(0)); forceRow->addWidget(forceName, 1);
    auto forceValues = dialogRow(forceFrame);
    for (int i = 1; i <= 3; ++i) {
      auto edit = field("FORCE_" + QString::number(i), forceParts.value(i));
      forceValues->addWidget(new QLabel(QStringList{"Force Mask", "Force Value", "Reset Value"}[i - 1]));
      edit->setMaximumWidth(65); forceValues->addWidget(edit);
    }
    auto calc = dialogFrame(form);
    auto expressionRow = dialogRow(calc);
    expressionRow->addWidget(new QLabel("Force CALC     Expression"));
    expressionRow->addWidget(field("FORCEPV_CALC", original->option("FORCEPV_CALC")), 1);
    auto grid = new QGridLayout;
    for (int i = 0; i < ForceCalcInputs; ++i) {
      QString letter(QChar('A' + i)), key = forceCalcInputKey(i);
      grid->addWidget(new QLabel(letter), i % 6, (i / 6) * 2);
      grid->addWidget(field(key, original->option(key)), i % 6, (i / 6) * 2 + 1);
    }
    calc->addLayout(grid);
    beep = line("Beep Severity", "BEEPSEVR");
    beep->setMaximumWidth(80);
    // Keep the short severity field next to its label, as in the Motif form.
    auto beepRow = qobject_cast<QHBoxLayout*>(form->itemAt(form->count() - 1)->layout());
    beepRow->setStretch(1, 0);
    beepRow->addStretch();
    line("Severity PV Name", "SEVRPV");
    if (!original->group) line("Acknowledgement PV / Value", "ACKPV");
    if (!original->parent) line("Heartbeat PV / Interval / Value", "HEARTBEATPV");
    line("Alias", "ALIAS");
    form->addWidget(new QLabel("Related Process Command"));
    form->addWidget(field("COMMAND", original->option("COMMAND")));
    auto area = [&](const QString& label, const QString& key, int height) {
      form->addWidget(new QLabel(label));
      auto edit = textArea(key, height);
      form->addWidget(edit);
      return edit;
    };
    severity = area("Alarm Severity Commands", "SEVRCOMMAND", 52);
    status = area("Alarm Status Commands", "STATCOMMAND", 52);
    url = area("Guidance URL", "GUIDANCE", 30);
    guidance = area("Guidance Text", "GUIDANCE_TEXT", 100);
    form->addStretch();
  } else {
    // Modern Properties: the same fields grouped into tabs of aligned forms.
    auto tabs = new QTabWidget;
    tabs->setObjectName("propertyTabs");
    tabs->setDocumentMode(false);
    auto page = [&](const QString& title, const QString& objectName) {
      auto area = new QScrollArea;
      area->setObjectName(objectName);
      area->setWidgetResizable(true);
      area->setFrameShape(QFrame::NoFrame);
      auto widget = new QWidget;
      auto column = dialogColumn(widget);
      column->setContentsMargins(12, 12, 12, 12);
      column->setSpacing(10);
      area->setWidget(widget);
      tabs->addTab(area, title);
      return column;
    };
    auto form = [](QVBoxLayout* parent) {
      auto result = new QFormLayout;
      result->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
      result->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      result->setHorizontalSpacing(14);
      result->setVerticalSpacing(8);
      parent->addLayout(result);
      return result;
    };
    auto monospace = [](const QString& text) {
      auto label = new QLabel(text.isEmpty() ? QString("—") : text);
      setPresentationFont(label, contentFont());
      label->setTextInteractionFlags(Qt::TextSelectableByMouse);
      return label;
    };
    auto generalPage = page("General", "propertyGeneralScroll");
    auto general = form(generalPage);
    name = field("NAME", original->name);
    general->addRow(original->group ? "Group" : "Channel", name);
    general->addRow("Alias", field("ALIAS", original->option("ALIAS")));
    mask = field("MASK", original->mask.text());
    if (options.editor && !original->group) {
      mask->setMaximumWidth(mask->fontMetrics().horizontalAdvance("CDATL") + 24);
      general->addRow("Alarm Mask", mask);
    } else {
      mask->hide();
      general->addRow("Current Mask", monospace(currentMask));
      general->addRow("Reset Mask", monospace(original->group ? QString() : original->mask.text()));
    }
    count = field("COUNT", countParts.value(0));
    seconds = field("SECONDS", countParts.value(1));
    auto filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);
    for (auto edit : {count, seconds}) edit->setMaximumWidth(edit->fontMetrics().horizontalAdvance("00000") + 20);
    filterRow->addWidget(count);
    // An unset filter reads "Not set" instead of a sentence with two blanks.
    if (options.editor || !countParts.isEmpty()) {
      filterRow->addWidget(new QLabel("alarms within"));
      filterRow->addWidget(seconds);
      filterRow->addWidget(new QLabel("seconds"));
    } else {
      seconds->hide();
    }
    filterRow->addStretch();
    general->addRow("Alarm Count Filter", filterRow);
    beep = field("BEEPSEVR", original->option("BEEPSEVR"));
    if (options.editor) {
      auto names = new QCompleter(QStringList{"MINOR", "MAJOR", "INVALID", "ERROR"}, beep);
      names->setCaseSensitivity(Qt::CaseInsensitive);
      beep->setCompleter(names);
      beep->setPlaceholderText("MINOR, MAJOR, INVALID or ERROR");
    }
    general->addRow("Beep Severity", beep);
    general->addRow("Severity PV Name", field("SEVRPV", original->option("SEVRPV")));
    if (!original->group)
      general->addRow("Acknowledgement PV / Value", field("ACKPV", original->option("ACKPV")));
    if (!original->parent)
      general->addRow("Heartbeat PV / Interval / Value", field("HEARTBEATPV", original->option("HEARTBEATPV")));
    generalPage->addStretch();
    auto forcePage = page("Force PV", "propertyForceScroll");
    forcePage->addWidget(sectionHeading("Force Process Variable"));
    auto forceFields = form(forcePage);
    forceName = field("FORCE_NAME", forceParts.value(0));
    forceFields->addRow("PV Name", forceName);
    for (int i = 1; i <= 3; ++i)
      forceFields->addRow(QStringList{"Force Mask", "Force Value", "Reset Value"}[i - 1],
                          field("FORCE_" + QString::number(i), forceParts.value(i)));
    forcePage->addWidget(sectionHeading("Force CALC"));
    auto calcFields = form(forcePage);
    calcFields->addRow("Expression", field("FORCEPV_CALC", original->option("FORCEPV_CALC")));
    auto grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    for (int i = 0; i < ForceCalcInputs; ++i) {
      QString letter(QChar('A' + i)), key = forceCalcInputKey(i);
      grid->addWidget(new QLabel(letter), i % 6, (i / 6) * 2);
      grid->addWidget(field(key, original->option(key)), i % 6, (i / 6) * 2 + 1);
    }
    forcePage->addLayout(grid);
    forcePage->addStretch();
    auto labeled = [](QVBoxLayout* parent, const QString& label, QWidget* widget, int stretch) {
      auto caption = new QLabel(label);
      caption->setBuddy(widget);
      parent->addWidget(caption);
      // Empty read-only text takes one line; content and editors share the space.
      auto text = qobject_cast<QPlainTextEdit*>(widget);
      if (text && text->isReadOnly() && text->document()->isEmpty()) stretch = 0;
      parent->addWidget(widget, stretch);
    };
    auto commandPage = page("Commands", "propertyCommandsScroll");
    labeled(commandPage, "Related Process Command", field("COMMAND", original->option("COMMAND")), 0);
    labeled(commandPage, "Alarm Severity Commands", severity = textArea("SEVRCOMMAND", 0), 1);
    labeled(commandPage, "Alarm Status Commands", status = textArea("STATCOMMAND", 0), 1);
    commandPage->addStretch(0);
    auto guidancePage = page("Guidance", "propertyGuidanceScroll");
    labeled(guidancePage, "Guidance URL", url = textArea("GUIDANCE", 0), 1);
    labeled(guidancePage, "Guidance Text", guidance = textArea("GUIDANCE_TEXT", 0), 3);
    guidancePage->addStretch(0);
    layout->addWidget(tabs, 1);
    // Read-only values read as text rather than empty input boxes.
    for (auto edit : edits)
      if (edit->isReadOnly()) {
        edit->setFrame(false);
        QPalette flat;
        flat.setColor(QPalette::Base, Qt::transparent);
        edit->setPalette(flat);
        edit->setPlaceholderText("Not set");
      }
  }
  if (!options.editor && beep->text().isEmpty())
    beep->setText(severityName(engine->state(original).beepThreshold));
  auto buttons = dialogActions(layout, dialog,
      options.editor ? QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::Close
                     : QDialogButtonBox::Close,
      "properties");
  if (options.editor) {
    disconnect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked, dialog, [this] {
      QTimer::singleShot(0, this, [this] { rebuildSelectionDialog("propertiesDialog"); });
    });
  }
  if (options.editor) connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
    try {
      Document candidate = doc;
      candidate.root = cloneNode(*doc.root);
      auto n = candidate.nodes().at(doc.nodes().indexOf(original));
      // The form has no notification fields; keep those directives as written.
      const auto previous = n->directives;
      QVector<Directive> notify;
      for (const auto& d : n->directives) if (isNotificationDirective(d.key)) notify << d;
      n->name = name->text().trimmed(); n->directives = notify;
      if (!n->group) n->mask = Mask::parse(mask->text());
      for (const auto& key : {"ALIAS", "COMMAND", "SEVRPV", "BEEPSEVR", "FORCEPV_CALC"})
        n->setOption(key, edits[key]->text());
      for (int i = 0; i < ForceCalcInputs; ++i)
        n->setOption(forceCalcInputKey(i), edits[forceCalcInputKey(i)]->text());
      if (!forceName->text().trimmed().isEmpty()) {
        n->setOption("FORCEPV", forceDirective(forceName->text(), edits["FORCE_1"]->text(),
            edits["FORCE_2"]->text(), edits["FORCE_3"]->text()));
      }
      if (!n->group) {
        n->setOption("ACKPV", edits["ACKPV"]->text());
        if (!count->text().isEmpty() || !seconds->text().isEmpty())
          n->setOption("ALARMCOUNTFILTER",
              (count->text().trimmed().isEmpty() ? QString("1") : count->text().trimmed()) + ' ' +
              (seconds->text().trimmed().isEmpty() ? QString("1") : seconds->text().trimmed()));
      }
      if (!n->parent) n->setOption("HEARTBEATPV", edits["HEARTBEATPV"]->text());
      for (auto item : {qMakePair(severity, QString("SEVRCOMMAND")),
                        qMakePair(status, QString("STATCOMMAND")), qMakePair(url, QString("GUIDANCE"))})
        for (auto value : item.first->toPlainText().split('\n'))
          if (!value.trimmed().isEmpty()) n->directives.push_back({item.second, value.trimmed()});
      n->setOption("GUIDANCE_TEXT", guidance->toPlainText());
      // Unchanged directives keep their place, comments and text as written.
      n->directives = keepDirectiveSources(previous, n->directives);
      auto checked = parseConfig(writeCheckedConfig(candidate, ConfigText::Edit)); checked.filename = doc.filename;
      checked.latin1 = doc.latin1;
      replace(std::move(checked));
    } catch (const std::exception& e) {
      QMessageBox::warning(dialog, "Invalid properties", e.what());
    }
  });
  sizeDialog(dialog, legacyAppearance() ? QSize(500, 730) : QSize(640, 520));
  finishSelectionDialog(dialog);
}
void Window::masks(bool forced) {
  if (!selection) return;
  if (options.editor) { properties(); return; }
  auto n = selection;
  auto dialog = beginSelectionDialog(forced ? "forceMaskDialog" : "modifyMaskDialog",
                                     forced ? "Force Mask" : "Modify Mask Settings",
                                     [this, forced] { masks(forced); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group Name:" : "Channel Name:", n->label());
  auto summary = [this, n] {
    if (!n->group) return engine->state(n).mask.text();
    QString value = "-----";
    for (int i = 0; i < 5; ++i)
      if (engine->state(n).maskCounts[i]) value[i] = QString("CDATL")[i];
    return value;
  };
  if (forced) {
    const auto alignment = legacyAppearance() ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter;
    auto current = new QLabel("Current Mask Summary: " + summary());
    current->setObjectName("currentMaskSummary");
    current->setAlignment(alignment);
    layout->addWidget(current);
    auto reset = new QLabel("Reset Mask: " + (n->group ? QString() : n->mask.text()));
    reset->setAlignment(alignment);
    layout->addWidget(reset);
    auto value = new QLabel("Mask: " + summary());
    value->setAlignment(alignment);
    QVector<QCheckBox*> choices;
    if (legacyAppearance()) {
      layout->addWidget(value);
      auto choicesColumn = dialogRow(layout);
      choicesColumn->addStretch(1);
      auto choicesPanel = new QVBoxLayout;
      choicesColumn->addLayout(choicesPanel, 3);
      choices = maskChoices(choicesPanel, summary(), options.engine.passive);
    } else {
      if (!n->group) reset->setToolTip("The configured mask restored by Reset");
      choices = maskChoices(layout, summary(), options.engine.passive, 2);
      value->setObjectName("forceMaskPreview");
      layout->addWidget(value);
      for (auto label : {current, reset, value}) setPresentationFont(label, contentFont());
    }
    for (auto choice : choices)
      connect(choice, &QCheckBox::toggled, dialog,
              [choices, value] { value->setText("Mask: " + selectedMask(choices)); });
    auto buttons = dialogActions(layout, dialog,
        QDialogButtonBox::Apply | QDialogButtonBox::Reset | QDialogButtonBox::Close,
        "masks");
    // The operator names NoAckT only by changing T from the mask shown. An
    // untouched T box carries the row's bit, which a duplicate row may not
    // have refreshed; it must not write the record's ACKT.
    auto shownAckT = std::make_shared<bool>(summary()[AckT] != '-');
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
      engine->cancelNoAckTimer(n);
      engine->setMask(n, Mask::parse(selectedMask(choices)), false,
                      choices[AckT]->isChecked() != *shownAckT);
      engine->setSilenceCurrent(false);
      *shownAckT = summary()[AckT] != '-';
      current->setText("Current Mask Summary: " + summary());
      dialog->setProperty("pendingEdits", false);
      refresh();
    });
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, dialog, [=] {
      engine->cancelNoAckTimer(n);
      engine->resetMask(n);
      engine->setSilenceCurrent(false);
      auto mask = summary();
      for (int i = 0; i < choices.size(); ++i) choices[i]->setChecked(mask[i] != '-');
      *shownAckT = mask[AckT] != '-';
      current->setText("Current Mask Summary: " + mask);
      dialog->setProperty("pendingEdits", false);
      refresh();
    });
    sizeDialog(dialog, QSize(277, 244));
  } else {
    auto grid = new QGridLayout;
    const QStringList labels = {"Add/Cancel Alarms", "Enable/Disable Alarms", "Ack/NoAck Alarms",
                                 "Ack/NoAck Transient Alarms", "Log/NoLog Alarms"};
    const QStringList off = {"Add", "Enable", "Ack", "AckT", "Log"};
    const QStringList on = {"Cancel", "Disable", "NoAck", "NoAckT", "NoLog"};
    int channels = 0;
    if (n->group && !legacyAppearance())
      for (auto channel : doc.channels())
        for (auto parent = channel->parent; parent; parent = parent->parent)
          if (parent == n) { ++channels; break; }
    if (legacyAppearance()) {
      grid->setHorizontalSpacing(6); grid->setVerticalSpacing(6);
    } else {
      grid->setHorizontalSpacing(12); grid->setVerticalSpacing(8);
      auto hint = new QLabel(n->group ? "Highlighted choices are the current setting of every "
                                        "channel in the group. Changes apply immediately."
                                      : "Highlighted choices are the current setting. "
                                        "Changes apply immediately.");
      hint->setWordWrap(true);
      hint->setForegroundRole(QPalette::PlaceholderText);
      layout->addWidget(hint);
    }
    const auto& state = engine->state(n);
    for (int bit = 0; bit < 5; ++bit) {
      grid->addWidget(new QLabel(labels[bit]), bit, 0);
      // Modern rows show the active setting as a selected segment.
      auto segments = legacyAppearance() ? nullptr : new QButtonGroup(dialog);
      // Only modern rows place their segments in a layout; the grid owns it.
      auto pair = segments ? new QHBoxLayout : nullptr;
      if (pair) pair->setSpacing(0);
      for (int choice = 0; choice < 3; ++choice) {
        const QString text = choice == 0 ? off[bit] : choice == 1 ? on[bit] : QString("Reset");
        auto button = segments && choice < 2 ? segmentButton(text, choice == 0, choice == 1)
                                             : new QPushButton(text);
        button->setObjectName(QString("maskAction%1_%2").arg(bit).arg(choice));
        button->setEnabled(!options.engine.passive || bit != AckT);
        if (!segments) grid->addWidget(button, bit, choice + 1);
        else if (choice < 2) { segments->addButton(button, choice); pair->addWidget(button); }
        else {
          button->setToolTip("Restore the configured setting");
          grid->addLayout(pair, bit, 1);
          grid->addWidget(button, bit, 2);
        }
        connect(button, &QPushButton::clicked, dialog, [this, n, bit, choice] {
          if (bit == Ack) engine->cancelNoAckTimer(n);
          engine->modifyMask(n, bit, choice);
          engine->setSilenceCurrent(false);
          refresh();
        });
      }
      if (segments) {
        const int set = n->group ? state.maskCounts[bit] : state.mask[bit] ? 1 : 0;
        const int total = n->group ? channels : 1;
        if (set == 0) segments->button(0)->setChecked(true);
        else if (set >= total) segments->button(1)->setChecked(true);
        else {
          auto mixed = new QLabel(QString("Mixed (%1 of %2)").arg(set).arg(total));
          mixed->setForegroundRole(QPalette::PlaceholderText);
          grid->addWidget(mixed, bit, 3);
        }
      }
    }
    if (!legacyAppearance()) grid->setColumnStretch(3, 1);
    layout->addLayout(grid);
    auto actions = dialogActions(layout, dialog, QDialogButtonBox::Close,
                  "masks");
    // Start on Close so no segment looks different from the other settings.
    if (!legacyAppearance()) actions->button(QDialogButtonBox::Close)->setFocus();
    sizeDialog(dialog, QSize(336, 202));
  }
  finishSelectionDialog(dialog);
}
void Window::forceDialog() {
  auto n = selection;
  if (!n) return;
  auto dialog = beginSelectionDialog("forcePvDialog", "Force Process Variable",
                                     [this] { forceDialog(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group Name:" : "Channel Name:", n->label());
  auto settingsParent = layout;
  QScrollArea* scroll = nullptr;
  QWidget* body = nullptr;
  if (!legacyAppearance()) {
    scroll = new QScrollArea;
    scroll->setObjectName("forcePvScroll"); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    body = new QWidget;
    settingsParent = dialogColumn(body);
    scroll->setWidget(body); layout->addWidget(scroll, 1);
  }
  auto settings = dialogFrame(settingsParent);
  auto disabled = new QCheckBox("ForcePV Disabled");
  disabled->setChecked(engine->state(n).forceDisabled);
  settings->addWidget(disabled);
  settings->addSpacing(legacyAppearance() ? 8 : 2);
  auto words = n->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
  auto pvName = new QLineEdit(words.value(0));
  pvName->setObjectName("forcePvName");
  auto value = new QLabel("Force Mask " + words.value(1, "-----"));
  auto force = new QLineEdit(words.value(2, "1")), reset = new QLineEdit(words.value(3, "0"));
  force->setObjectName("forceValue"); reset->setObjectName("forceReset");
  auto expression = new QLineEdit(n->option("FORCEPV_CALC"));
  expression->setObjectName("forceExpression");
  QVector<QCheckBox*> choices;
  QVector<QLineEdit*> inputs;
  for (int i = 0; i < ForceCalcInputs; ++i) {
    auto field = new QLineEdit(n->option(forceCalcInputKey(i)));
    field->setObjectName("forceInput" + QString(QChar('A' + i)));
    inputs << field;
  }
  if (legacyAppearance()) {
    settings->addWidget(new QLabel("Force Process Variable Name (or CALC):"));
    settings->addWidget(pvName);
    settings->addWidget(value);
    choices = maskChoices(settings, words.value(1, "-----"));
    auto values = new QFormLayout;
    values->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    force->setMaximumWidth(75); reset->setMaximumWidth(75);
    values->addRow("Force Value", force); values->addRow("Reset Value", reset);
    settings->addLayout(values);
    auto calc = dialogFrame(settings, "Force CALC");
    calc->addWidget(new QLabel("Expression:"));
    calc->addWidget(expression);
    // All twelve inputs, A–F beside G–L.
    auto inputGrid = new QGridLayout;
    calc->addLayout(inputGrid);
    for (int i = 0; i < ForceCalcInputs; ++i) {
      inputGrid->addWidget(new QLabel(QString(QChar('A' + i))), i % 6, (i / 6) * 2);
      inputGrid->addWidget(inputs[i], i % 6, (i / 6) * 2 + 1);
    }
  } else {
    auto nameLabel = new QLabel("Force Process Variable Name (or CALC):");
    nameLabel->setBuddy(pvName);
    settings->addWidget(nameLabel);
    settings->addWidget(pvName);
    settings->addWidget(value);
    choices = maskChoices(settings, words.value(1, "-----"), false, 2);
    auto values = new QHBoxLayout;
    values->setSpacing(8);
    for (auto pair : {qMakePair(QString("Force Value"), force), qMakePair(QString("Reset Value"), reset)}) {
      auto label = new QLabel(pair.first);
      label->setBuddy(pair.second);
      values->addWidget(label);
      values->addWidget(pair.second, 1);
    }
    settings->addLayout(values);
    auto calc = dialogFrame(settings, "Force CALC");
    auto expressionRow = dialogRow(calc);
    expressionRow->addWidget(new QLabel("Expression"));
    expressionRow->addWidget(expression, 1);
    auto inputGrid = new QGridLayout;
    inputGrid->setHorizontalSpacing(10);
    inputGrid->setVerticalSpacing(6);
    calc->addLayout(inputGrid);
    // Three columns of four keep all twelve inputs visible on small screens.
    for (int i = 0; i < ForceCalcInputs; ++i) {
      inputGrid->addWidget(new QLabel(QString(QChar('A' + i))), i % 4, (i / 4) * 2);
      inputGrid->addWidget(inputs[i], i % 4, (i / 4) * 2 + 1);
    }
    settingsParent->addStretch();
  }
  for (auto box : choices)
    connect(box, &QCheckBox::toggled, dialog,
            [value, choices] { value->setText("Force Mask " + selectedMask(choices)); });
  // Keyboard order follows the form: name, mask, values, then CALC inputs.
  QVector<QWidget*> order{disabled, pvName};
  for (auto box : choices) order << box;
  order << force << reset << expression;
  for (auto input : inputs) order << input;
  for (int i = 1; i < order.size(); ++i) QWidget::setTabOrder(order[i - 1], order[i]);
  auto buttons = dialogActions(layout, dialog,
      QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::Close,
      "masks");
  disconnect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, dialog, &QDialog::close);
  connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked, dialog, [=] {
    const auto saved = n->option("FORCEPV").split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    pvName->setText(saved.value(0));
    const auto mask = saved.value(1, "-----");
    for (int i = 0; i < choices.size(); ++i) choices[i]->setChecked(mask[i] != '-');
    force->setText(saved.value(2, "1")); reset->setText(saved.value(3, "0"));
    disabled->setChecked(engine->state(n).forceDisabled);
    expression->setText(n->option("FORCEPV_CALC"));
    for (int i = 0; i < inputs.size(); ++i)
      inputs[i]->setText(n->option(forceCalcInputKey(i)));
    dialog->setProperty("pendingEdits", false);
  });
  connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=] {
    try {
      QString text = "GROUP NULL check\n";
      if (!pvName->text().trimmed().isEmpty())
        text += "$FORCEPV " + forceDirective(pvName->text(), selectedMask(choices),
            force->text(), reset->text()) + "\n";
      // Preserve editable CALC settings even while a scalar PV is selected.
      if (!expression->text().trimmed().isEmpty())
        text += "$FORCEPV_CALC " + expression->text() + "\n";
      for (int i = 0; i < ForceCalcInputs; ++i)
        if (!inputs[i]->text().trimmed().isEmpty())
          text += "$" + forceCalcInputKey(i) + " " + inputs[i]->text() + "\n";
      auto checked = parseConfig(text);
      engine->configureForce(n, checked.root->directives, disabled->isChecked());
      if (sameSettings(forceSettings(n), configuredForce.value(n))) forceEdited.remove(n);
      else forceEdited.insert(n);
      dialog->setProperty("pendingEdits", false);
      dirty = true;
    } catch (const std::exception& e) {
      // Reloads and filtered-out selections can clear or close this dialog
      // while the error is shown, so never block in a stack-owned child box.
      auto box = new QMessageBox(QMessageBox::Warning, "Invalid Force PV", e.what(),
                                 QMessageBox::Ok, dialog);
      box->setAttribute(Qt::WA_DeleteOnClose);
      box->setWindowModality(Qt::ApplicationModal);
      box->show();
    }
  });
  // Show the whole form, including all twelve CALC inputs, when the screen allows;
  // the scroll area's own hint ignores its content.
  const int contentHeight = body ? layout->sizeHint().height() - scroll->sizeHint().height() +
                                       body->sizeHint().height() : 0;
  sizeDialog(dialog, legacyAppearance() ? QSize(276, 494) : QSize(520, qMax(480, contentHeight)));
  finishSelectionDialog(dialog);
}
void Window::beepSeverity(bool global) {
  auto n = selection;
  if (!global && !n) return;
  auto dialog = beginSelectionDialog("beepSeverityDialog", "Set Beep Severity",
                                     [this, global] { beepSeverity(global); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, global ? "Facility:" : n->group ? "Group Name:" : "Channel Name:",
                global ? doc.root->label() : n->label());
  auto severityRow = dialogRow(layout);
  severityRow->addStretch();
  auto severityColumn = new QVBoxLayout;
  severityRow->addLayout(severityColumn);
  severityRow->addStretch();
  auto frame = dialogFrame(severityColumn);
  auto buttons = new QButtonGroup(dialog);
  for (int severity = 1; severity < 5; ++severity) {
    auto button = new QRadioButton(severityName(severity));
    if (!legacyAppearance()) button->setIcon(severitySwatch(severity));
    button->setObjectName("beepSeverity" + QString::number(severity));
    button->setChecked((global ? doc.beepSeverity : engine->state(n).beepThreshold) == severity);
    frame->addWidget(button); buttons->addButton(button, severity);
    connect(button, &QRadioButton::clicked, dialog, [this, global, n, severity] {
      int previous = global ? doc.beepSeverity : engine->state(n).beepThreshold;
      if (severity == previous) return;
      if (options.editor) {
        redo.clear(); rememberEdit(undo, writeConfig(doc, ConfigText::Edit)); modified = true;
      }
      // Global changes retain the deferred audit gap documented at the ALH
      // Beep Severity menu; per-node changes are audited by Engine::setBeep.
      if (global) doc.beepSeverity = severity;
      else engine->setBeep(n, severity);
      refresh();
    });
  }
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "beep-severity");
  sizeDialog(dialog, QSize(170, 178));
  finishSelectionDialog(dialog);
}
// ALH's Modify NoAck Mask dialog follows the selection. Its toggle starts or
// ends the selection's one-hour NoAck timer at once.
void Window::noAckDialog() {
  auto n = selection;
  if (!n) return;
  auto dialog = beginSelectionDialog("noAckDialog", "Modify NoAck Mask", [this] { noAckDialog(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group:" : "Channel:", n->label());
  if (n->group) {
    int channels = 0;
    std::function<void(const Node*)> count = [&](const Node* c) {
      for (const auto& child : c->children)
        if (child->group) count(child.get());
        else ++channels;
    };
    count(n);
    auto affected = new QLabel(QString("Channels affected: %1").arg(channels));
    affected->setObjectName("noAckChannelCount");
    layout->addWidget(affected);
  }
  auto toggle = new QCheckBox("NoAck For One Hour");
  toggle->setObjectName("noAckOneHour");
  toggle->setChecked(engine->state(n).noAckUntil != 0);
  layout->addWidget(toggle);
  connect(toggle, &QCheckBox::toggled, dialog, [this, n](bool enabled) {
    engine->noAck(n, enabled);
    // The toggle already shows this state. Record it, so that a later expiry or
    // restore is seen as a change even before the dialog has been rebuilt.
    selectionDialogs["noAckDialog"].state = selectionState();
    refresh();
  });
  dialogActions(layout, dialog, QDialogButtonBox::Close, "selection-and-alarm-silence");
  sizeDialog(dialog, QSize(230, 140));
  // Nothing is left pending, so timer expiry or a Runtime Overrides restore
  // still refreshes the toggle; finishSelectionDialog() would block that.
  dialog->show();
}
void Window::guidance(Node* target) {
  if (!target) target = selection;
  if (!target) return;
  auto url = target->option("GUIDANCE");
  if (!url.isEmpty()) {
    QUrl u =
        QUrl::fromUserInput(url, QFileInfo(doc.filename).absolutePath(), QUrl::AssumeLocalFile);
    if (!QDesktopServices::openUrl(u))
      error("Cannot open guidance: " + url);
  } else
    showText("Guidance: " + target->name,
             target->option("GUIDANCE_TEXT", "No guidance available."), false, false);
}
void Window::related(Node* target) {
  if (!target) target = selection;
  if (!target) return;
  auto text = target->option("COMMAND");
  if (text.isEmpty())
    return;
  auto entries = relatedEntries(text);
  if (entries.isEmpty())
    return;
  if (entries.size() > 1 && entries.size() % 2) {
    error("Related process command requires label/command pairs.");
    return;
  }
  if (entries.size() > 1) {
    auto menu = new QMenu(this);
    for (int i = 0; i + 1 < entries.size(); i += 2) {
      auto a = menu->addAction(entries[i].trimmed());
      QString command = entries[i + 1].trimmed();
      connect(a, &QAction::triggered, this, [this, command] { engine->command(command); });
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QCursor::pos());
  } else
    engine->command(entries.front().trimmed());
}
QDialog* Window::showText(const QString& title, const QString& content, bool monospace,
                          bool searchable) {
  auto dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(title);
  dialog->resize(600, 300);
  auto l = dialogColumn(dialog);
  auto text = new QPlainTextEdit;
  text->setReadOnly(true);
  if (legacyAppearance()) text->setFont(font());
  else if (monospace) setPresentationFont(text, contentFont());
  // Configuration text keeps its lines; prose wraps in the modern viewer.
  text->setLineWrapMode(legacyAppearance() || monospace ? QPlainTextEdit::NoWrap
                                                        : QPlainTextEdit::WidgetWidth);
  text->setPlainText(content);
  l->addWidget(text);
  if (searchable) findBar(l, text, -1);
  dialogActions(l, dialog, QDialogButtonBox::Close);
  if (!legacyAppearance()) dialog->resize(monospace ? 720 : 600, monospace ? 520 : 340);
  if (!searchable) {
    // Guidance follows its content, while long instructions remain scrollable
    // in a dialog smaller than the screen. Leave space for window decorations.
    const auto screen = this->screen();
    const QSize maximum = screen ? screen->availableGeometry().size() - QSize(64, 80)
                                 : QSize(960, 640);
    const auto margins = l->contentsMargins();
    const QFontMetrics metrics(text->font());
    int longest = 0;
    for (const auto& line : content.split('\n'))
      longest = qMax(longest, metrics.horizontalAdvance(line));
    const int padding = 2 * text->frameWidth() + qCeil(2 * text->document()->documentMargin()) +
                        text->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int width = qBound(qMin(320, maximum.width()),
                            longest + padding + margins.left() + margins.right(), maximum.width());
    QTextDocument measurement;
    measurement.setDefaultFont(text->font());
    measurement.setPlainText(content);
    if (text->lineWrapMode() != QPlainTextEdit::NoWrap)
      measurement.setTextWidth(qMax(1, width - margins.left() - margins.right() - padding));
    l->activate();
    const int chrome = l->sizeHint().height() - text->sizeHint().height();
    const int height = qCeil(measurement.size().height()) + padding + chrome;
    dialog->resize(width, qBound(qMin(140, maximum.height()), height, maximum.height()));
  }
  dialog->show();
  return dialog;
}
void Window::showHistory() {
  if (historyDialog) { historyDialog->show(); historyDialog->raise(); return; }
  auto d = new QDialog(this);
  d->setAttribute(Qt::WA_DeleteOnClose);
  d->setObjectName("historyDialog");
  d->setWindowTitle("Alarm Handler: Current Alarm History");
  auto l = dialogColumn(d);
  historyDialog = d;
  shownHistory.clear();
  if (legacyAppearance()) {
    auto heading = dialogRow(l);
    auto close = new QPushButton("Close");
    heading->addWidget(close);
    heading->addStretch();
    connect(close, &QPushButton::clicked, d, &QDialog::close);
  }
  // A shared table keeps headings aligned with cells, including when a long PV
  // name requires horizontal scrolling in the classic appearance.
  auto table = new QTableWidget(0, 5);
  table->setObjectName("historyTable");
  table->setHorizontalHeaderLabels(legacyAppearance()
      ? QStringList{"TIME_STAMP", "PROCESS_VARIABLE_NAME", "STATUS", "SEVERITY", "VALUE"}
      : QStringList{"Time", "Process Variable", "Status", "Severity", "Value"});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setAlternatingRowColors(!legacyAppearance());
  table->setShowGrid(false);
  table->setWordWrap(false);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  if (legacyAppearance()) {
    auto historyFont = font();
    historyFont.setStretch(100);
    setPresentationFont(table, historyFont);
    table->setFrameShape(QFrame::NoFrame);
  } else setPresentationFont(table, contentFont());
  // Operators choose the order by clicking a heading; the choice lasts for this
  // runtime. Newest first remains the default.
  table->horizontalHeader()->setSortIndicator(historySortColumn, historySortOrder);
  table->setSortingEnabled(true);
  connect(table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          [this](int column, Qt::SortOrder order) {
            historySortColumn = column;
            historySortOrder = order;
          });
  l->addWidget(table, 1);
  historyTable = table;
  refresh();
  if (!legacyAppearance()) {
    auto buttons = dialogActions(l, d, QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setFocus();
  }
  sizeDialog(d, legacyAppearance() ? QSize(720, 225) : QSize(860, 360)); d->show();
}
void Window::refreshHistory() {
  if (!historyTable || engine->history == shownHistory) return;
  shownHistory = engine->history;
  static const QRegularExpression fields("^(\\S+ \\S+) (\\S+) (\\S+) (\\S+) (.*)$");
  // Rows are rewritten in engine order (newest first), then sorted as chosen.
  historyTable->setSortingEnabled(false);
  historyTable->setRowCount(shownHistory.size());
  for (int row = 0; row < shownHistory.size(); ++row) {
    const auto match = fields.match(shownHistory[row]);
    for (int column = 0; column < 5; ++column) {
      auto item = historyTable->item(row, column);
      if (!item) historyTable->setItem(row, column, item = new HistoryItem);
      item->setText(match.hasMatch() ? match.captured(column + 1) : column ? QString() : shownHistory[row]);
      item->setData(Qt::BackgroundRole, QVariant());
      item->setData(Qt::ForegroundRole, QVariant());
      item->setData(Qt::UserRole, QVariant());
    }
    // Entries sharing a timestamp keep their order through the age key.
    historyTable->item(row, 0)->setData(Qt::UserRole, double(shownHistory.size() - row));
    if (!match.hasMatch()) continue;
    bool numeric = false;
    const double value = match.captured(5).toDouble(&numeric);
    if (numeric && !qIsNaN(value)) historyTable->item(row, 4)->setData(Qt::UserRole, value);
    for (int severity = 0; severity < 5; ++severity)
      if (match.captured(4) == severityName(severity)) {
        historyTable->item(row, 3)->setData(Qt::UserRole, double(severity));
        if (!severity) continue;
        const auto color = severityColor(severity);
        historyTable->item(row, 3)->setBackground(color);
        historyTable->item(row, 3)->setForeground(contrastingText(color));
      }
  }
  historyTable->setSortingEnabled(true);
  historyTable->resizeColumnsToContents();
}
void Window::broadcast(int mode) {
  if (!logging) return;
  auto dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("broadcastDialog");
  const bool legacy = legacyAppearance();
  const QStringList titles = {"Send Message", "Stop Alarm Logging", "Reload Facility"};
  dialog->setWindowTitle(legacy ? QString("ALH MessageEntryDialog") : titles.value(mode));
  auto layout = dialogColumn(dialog);
  const QStringList prompts = {"Message for other Alarm Handlers using this configuration:",
                               "Reason for stopping alarm logging:",
                               "Reason for reloading the facility configuration:"};
  layout->addWidget(new QLabel(legacy ? QString("Type message (See help for detail):") : prompts.value(mode)));
  auto text = new QLineEdit(mode == 2 ? "Reload config. Reason: " : "");
  text->setObjectName("broadcastMessage");
  layout->addWidget(text);
  auto minutes = new QSpinBox;
  minutes->setRange(1, 10); minutes->setValue(1);
  if (mode == 1) {
    auto row = dialogRow(layout);
    if (legacy) {
      row->addWidget(new QLabel("Minutes without alarm logging:")); row->addWidget(minutes);
    } else {
      minutes->setSuffix(minutes->value() == 1 ? " minute" : " minutes");
      connect(minutes, QOverload<int>::of(&QSpinBox::valueChanged), minutes,
              [minutes](int value) { minutes->setSuffix(value == 1 ? " minute" : " minutes"); });
      row->addWidget(new QLabel("Stop alarm logging for")); row->addWidget(minutes); row->addStretch();
    }
  } else {
    minutes->setParent(dialog);
    minutes->hide();
  }
  auto buttons = dialogActions(layout, dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
      "logging-and-shared-operation");
  if (!legacy) buttons->button(QDialogButtonBox::Ok)->setText(QStringList{"Send", "Stop Logging", "Reload"}.value(mode));
  connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, text, minutes, mode] {
    if (mode == 0) {
      const auto reason = Logging::plainBroadcastError(text->text());
      if (!reason.isEmpty()) { error(reason); return; }
    }
    if (logging->sendBroadcast(text->text(), mode == 1 ? minutes->value() : 0, mode == 2)) dialog->close();
    else error("Could not broadcast message");
  });
  sizeDialog(dialog, legacy ? QSize(390, mode == 1 ? 135 : 105) : QSize(460, 0)); dialog->show();
}
void Window::exitOnInterrupt() {
  // ALH -B: SIGINT runs exit_quit without confirmation. closeEvent refuses
  // while this instance's broadcast is being delivered, as exit_quit does.
  quitting = true;
  // A stack modal dialog of this window (file chooser, print dialog, message
  // box) or a context menu may be inside exec(). Closing the window there
  // would delete it from that event loop and free the stack object. Reject or
  // close them and exit once their exec() has returned.
  bool waiting = false;
  if (auto popup = QApplication::activePopupWidget())
    for (auto w = popup->parentWidget(); w; w = w->parentWidget())
      if (w == this || w == runtime) {
        waiting = true;
        popup->close();
        break;
      }
  for (auto window : QApplication::topLevelWidgets()) {
    if (!window->isModal() || window->testAttribute(Qt::WA_DeleteOnClose) ||
        (!window->isVisible() && window != QApplication::activeModalWidget()))
      continue;
    for (auto w = window->parentWidget(); w; w = w->parentWidget())
      if (w == this || w == runtime) {
        waiting = true;
        if (auto dialog = qobject_cast<QDialog*>(window)) dialog->reject();
        else window->close();
        break;
      }
  }
  if (waiting) {
    QTimer::singleShot(50, this, [this] { exitOnInterrupt(); });
    return;
  }
  close();
}
void handleInterrupt(Window* window) {
#ifndef Q_OS_WIN
  static QPointer<Window> target;
  target = window;
  if (interruptPipe[0] >= 0 || ::pipe(interruptPipe) != 0) return;
  for (int fd : interruptPipe) {
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  }
  auto notifier = new QSocketNotifier(interruptPipe[0], QSocketNotifier::Read, qApp);
  QObject::connect(notifier, &QSocketNotifier::activated, notifier, [] {
    char bytes[16];
    while (::read(interruptPipe[0], bytes, sizeof bytes) > 0) {}
    if (target) target->exitOnInterrupt();
  });
  // Like ALH's signal(SIGINT, broadcastMess_exit_quit), the handler stays
  // installed, so a refused interrupt can be repeated after delivery.
  struct sigaction action = {};
  action.sa_handler = writeInterruptPipe;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  sigaction(SIGINT, &action, nullptr);
#else
  Q_UNUSED(window);
#endif
}
void Window::exitApplication() {
  if (exitPopup) {
    exitPopup->raise();
    exitPopup->activateWindow();
    return;
  }
  auto parent = isVisible() ? static_cast<QWidget*>(this) : runtime;
  // The configuration tool does not monitor alarms; legacy ACT asks
  // "Exit Alarm Configuration Tool?" (awAct.c MENU_FILE_QUIT).
  const QString tool = options.editor ? "Alarm Configuration Tool" : "Alarm Handler";
  exitPopup = new QMessageBox(legacyAppearance() ? QMessageBox::Warning : QMessageBox::Question,
                              tool, "Exit " + tool + "?",
                              QMessageBox::Ok | QMessageBox::Cancel, parent);
  if (!legacyAppearance()) {
    if (!options.editor)
      exitPopup->setInformativeText("Alarm monitoring and logging for " + doc.root->label() + " will stop.");
    exitPopup->button(QMessageBox::Ok)->setText("Exit");
  }
  exitPopup->setObjectName("exitConfirmation");
  exitPopup->setDefaultButton(QMessageBox::Cancel);
  exitPopup->setAttribute(Qt::WA_DeleteOnClose);
  exitPopup->setWindowModality(Qt::ApplicationModal);
  connect(exitPopup, &QDialog::finished, this, [this](int result) {
    exitPopup = nullptr;
    if (result == QMessageBox::Ok) {
      quitting = true;
      close();
    }
  });
  exitPopup->show();
}
bool Window::eventFilter(QObject* object, QEvent* event) {
  if (object == runtime && event->type() == QEvent::Close && !quitting) {
    exitApplication();
    event->ignore();
    return true;
  }
  return QMainWindow::eventFilter(object, event);
}
void Window::closeEvent(QCloseEvent* event) {
  // ALH prevents sender exit while a broadcast is being delivered. Otherwise
  // the last window can end the event loop before peers see Reload/Stop Logging.
  if (logging && logging->broadcastPending() && quitting) {
    error("A broadcast is still being delivered. Wait up to one minute before exiting.");
    quitting = false;
    event->ignore();
    return;
  }
  if (!options.editor && !quitting) {
    hide();
    event->ignore();
    return;
  }
  if (modified) {
    auto answer =
        QMessageBox::question(this, "Exit ALH", "Save configuration changes?",
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel) {
      event->ignore();
      quitting = false;
      return;
    }
    if (answer == QMessageBox::Save) {
      save();
      if (modified) {
        event->ignore();
        quitting = false;
        return;
      }
    }
  }
  if (runtime) {
    // Close the visible compact window so Qt can emit lastWindowClosed even
    // when the main display was never shown. Hiding it leaves app.exec running.
    // This window is already closing, so this is not an exit request: an
    // editor closed from its title bar must not ask "Exit ...?" afterwards.
    QScopedValueRollback<bool> closing(quitting, true);
    runtime->close();
  }
  // Hiding the main display, cancelling, or deferring exit is not an exit.
  logging->operation(nullptr, "Setup---Exit");
  event->accept();
}
} // namespace alh
