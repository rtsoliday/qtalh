// Operator tools beyond ALH: active alarm list, alarm search, alarm details,
// runtime overrides, summary panel, and Phoebus XML import/export. They use
// the shared dialog helpers, so they follow the classic and modern appearances.
#include "alarm_view.h"
#include "core/phoebus.h"
#include "dialogs.h"
#include "window.h"
#include <QtWidgets>
#include <cmath>
namespace alh {
namespace {
constexpr int SortRole = Qt::UserRole + 1;
constexpr int FilterRole = Qt::UserRole + 2;
QString letter(int severity) {
  static const QString letters[] = {QString(), "Y", "R", "V", "E"};
  return letters[qBound(0, severity, 4)];
}
Node* nodeOf(const QVariant& value) { return reinterpret_cast<Node*>(value.value<quintptr>()); }
QVariant nodeValue(Node* n) { return QVariant::fromValue(reinterpret_cast<quintptr>(n)); }
QString groupPath(const Node* n) {
  return n->parent ? Engine::channelPath(n->parent) : QString("/");
}
QString localTime(qint64 ms) {
  return ms ? QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss") : QString();
}

// ---- Active alarms ---------------------------------------------------------
struct ActiveRow {
  Node* node = nullptr;
  int severity = 0, unack = 0, status = 0;
  qint64 changed = 0;
  QString value, label, group;
  bool sameState(const ActiveRow& o) const {
    return severity == o.severity && unack == o.unack && status == o.status &&
           changed == o.changed && value == o.value;
  }
};
class ActiveAlarmModel : public QAbstractTableModel {
public:
  using QAbstractTableModel::QAbstractTableModel;
  QVector<ActiveRow> rows;
  int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : rows.size(); }
  int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 7; }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    static const QStringList headers = {"Ack", "Severity", "Changed", "Channel", "Status", "Value", "Group"};
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) return headers.value(section);
    return {};
  }
  QVariant data(const QModelIndex& index, int role) const override {
    if (!index.isValid() || index.row() >= rows.size()) return {};
    const auto& r = rows[index.row()];
    const int column = index.column();
    const int colored = column == 0 ? r.unack : column == 1 ? r.severity : 0;
    switch (role) {
    case Qt::DisplayRole:
      switch (column) {
      case 0: return letter(r.unack);
      case 1: return severityName(r.severity);
      case 2: return r.changed ? QDateTime::fromMSecsSinceEpoch(r.changed).toString("HH:mm:ss") : QString();
      case 3: return r.label;
      case 4: return statusName(r.status);
      case 5: return r.value;
      default: return r.group;
      }
    case SortRole:
      // Severity orders first and the latest change next, as in TAlh's lists.
      if (column == 0) return qint64(r.unack) * 10000000000000LL + r.changed;
      if (column == 1) return qint64(r.severity) * 10000000000000LL + r.changed;
      if (column == 2) return r.changed;
      return data(index, Qt::DisplayRole).toString().toLower();
    case Qt::BackgroundRole:
      if (colored) return severityColor(colored);
      return {};
    case Qt::ForegroundRole:
      // Classic rows letter MAJOR in white on ALH's pure red.
      if (colored) return legacyAppearance() && colored == 2 ? QColor(Qt::white) : contrastingText(severityColor(colored));
      return {};
    case Qt::TextAlignmentRole:
      return column == 0 ? int(Qt::AlignCenter) : int(Qt::AlignLeft | Qt::AlignVCenter);
    case Qt::ToolTipRole:
      if (column == 2) return localTime(r.changed);
      return Engine::channelPath(r.node);
    case Qt::UserRole:
      return nodeValue(r.node);
    case FilterRole:
      if (column == 3) return r.label + '\n' + r.node->name;
      return data(index, Qt::DisplayRole);
    }
    return {};
  }
  // Keyed by node, so selection and scrolling survive each update.
  void update(QVector<ActiveRow> next) {
    QSet<Node*> wanted;
    for (const auto& row : next) wanted.insert(row.node);
    for (int r = rows.size() - 1; r >= 0; --r)
      if (!wanted.contains(rows[r].node)) {
        beginRemoveRows({}, r, r);
        rows.remove(r);
        endRemoveRows();
      }
    QHash<Node*, int> present;
    for (int r = 0; r < rows.size(); ++r) present.insert(rows[r].node, r);
    QVector<ActiveRow> added;
    for (auto& row : next) {
      const auto found = present.constFind(row.node);
      if (found == present.cend()) {
        row.label = row.node->label();
        row.group = groupPath(row.node);
        added << row;
      } else if (!rows[*found].sameState(row)) {
        auto& current = rows[*found];
        current.severity = row.severity; current.unack = row.unack; current.status = row.status;
        current.changed = row.changed; current.value = row.value;
        emit dataChanged(index(*found, 0), index(*found, 6));
      }
    }
    if (added.isEmpty()) return;
    beginInsertRows({}, rows.size(), rows.size() + added.size() - 1);
    rows += added;
    endInsertRows();
  }
};
class ActiveAlarmFilter : public QSortFilterProxyModel {
public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  bool unacknowledgedOnly = false;
  void refilter() { invalidateFilter(); }
protected:
  bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
    auto model = static_cast<ActiveAlarmModel*>(sourceModel());
    if (unacknowledgedOnly && !model->rows.value(row).unack) return false;
    return QSortFilterProxyModel::filterAcceptsRow(row, parent);
  }
};
QVector<Node*> selectedNodes(QAbstractItemView* view) {
  QVector<Node*> nodes;
  for (const auto& index : view->selectionModel()->selectedRows())
    if (auto n = nodeOf(index.data(Qt::UserRole))) nodes << n;
  if (nodes.isEmpty() && view->currentIndex().isValid())
    if (auto n = nodeOf(view->currentIndex().siblingAtColumn(0).data(Qt::UserRole))) nodes << n;
  return nodes;
}

// Up/Down in the search text move through the matches.
class ArrowKeys : public QObject {
public:
  ArrowKeys(QListWidget* list, QObject* parent) : QObject(parent), list(list) {}
protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() != QEvent::KeyPress) return false;
    const int key = static_cast<QKeyEvent*>(event)->key();
    if ((key != Qt::Key_Up && key != Qt::Key_Down) || !list->count()) return false;
    list->setCurrentRow(qBound(0, list->currentRow() + (key == Qt::Key_Down ? 1 : -1), list->count() - 1));
    return true;
  }
private:
  QListWidget* list;
};

// ---- Alarm details ---------------------------------------------------------
// Alarm fields by record type; values arrive as strings from Channel Access.
QStringList limitFields(const QString& type) {
  static const QSet<QString> analog = {"ai", "ao", "calc", "calcout", "longin", "longout", "int64in",
                                       "int64out", "sub", "dfanout", "sel", "pid", "epid"};
  if (analog.contains(type)) return {"HIHI", "HIGH", "LOW", "LOLO", "HHSV", "HSV", "LSV", "LLSV", "HYST"};
  if (type == "bi" || type == "bo") return {"ZSV", "OSV", "COSV", "ZNAM", "ONAM"};
  if (type == "mbbi" || type == "mbbo") {
    QStringList fields{"UNSV", "COSV"};
    static const QStringList prefixes = {"ZR", "ON", "TW", "TH", "FR", "FV", "SX", "SV",
                                         "EI", "NI", "TE", "EL", "TV", "TT", "FT", "FF"};
    for (const auto& p : prefixes) fields << p + "SV" << p + "ST";
    return fields;
  }
  return {};
}
QString describeLimits(const QString& type, const QHash<QString, QString>& v) {
  if (v.isEmpty() && !limitFields(type).isEmpty())
    return "Not available: the record's alarm limit fields did not respond.";
  QStringList lines;
  if (v.contains("HHSV")) {
    for (auto pair : {qMakePair(QString("HIHI"), QString("HHSV")), qMakePair(QString("HIGH"), QString("HSV")),
                      qMakePair(QString("LOW"), QString("LSV")), qMakePair(QString("LOLO"), QString("LLSV"))})
      lines << QString("%1 %2 %3").arg(pair.first, -5).arg(v.value(pair.first), -10).arg(v.value(pair.second));
    if (!v.value("HYST").isEmpty() && v.value("HYST").toDouble() != 0)
      lines << "Hysteresis " + v.value("HYST");
  } else if (v.contains("ZSV")) {
    lines << QString("0 (%1)  %2").arg(v.value("ZNAM"), v.value("ZSV"))
          << QString("1 (%1)  %2").arg(v.value("ONAM"), v.value("OSV"))
          << "Change of state  " + v.value("COSV");
  } else if (v.contains("UNSV")) {
    static const QStringList prefixes = {"ZR", "ON", "TW", "TH", "FR", "FV", "SX", "SV",
                                         "EI", "NI", "TE", "EL", "TV", "TT", "FT", "FF"};
    for (int i = 0; i < prefixes.size(); ++i) {
      const auto severity = v.value(prefixes[i] + "SV");
      if (!severity.isEmpty() && severity != "NO_ALARM")
        lines << QString("%1 (%2)  %3").arg(i).arg(v.value(prefixes[i] + "ST"), severity);
    }
    lines << "Unknown state  " + v.value("UNSV") << "Change of state  " + v.value("COSV");
  }
  if (lines.isEmpty()) return "Record type " + type + " has no alarm limits.";
  return "Record type " + type + "\n" + lines.join('\n');
}
// A plain explanation of why the channel is (or is not) alarming.
QString explain(const State& s, const QHash<QString, QString>& limits) {
  if (s.shelf.until)
    return "Shelved until " + localTime(s.shelf.until) + " by " + s.shelf.username + ": " + s.shelf.reason + ".";
  if (s.mask[Cancel]) return "The alarm is cancelled by its mask, so it is not monitored.";
  if (s.mask[Disable]) return "The alarm is disabled by its mask, so it is not reported.";
  if (!s.initialized) return "No value has been received from the IOC yet.";
  const auto status = statusName(s.status), severity = severityName(s.severity);
  QString text;
  if (s.filterUntil && s.pending.severity != s.severity)
    text = QString("A count filter is holding the previous %1 alarm (%2). The latest input is %3 "
                   "with value %4.")
               .arg(severity, status, severityName(s.pending.severity), s.pending.value);
  else if (!s.severity) text = "Not in alarm (value " + s.value + ").";
  else if (status == "HIHI" || status == "HIGH" || status == "LOW" || status == "LOLO") {
    static const QHash<QString, QString> severityField = {{"HIHI", "HHSV"}, {"HIGH", "HSV"},
                                                          {"LOW", "LSV"}, {"LOLO", "LLSV"}};
    bool valueOk = false, limitOk = false;
    const double value = s.value.toDouble(&valueOk), limit = limits.value(status).toDouble(&limitOk);
    const bool high = status.startsWith('H');
    if (valueOk && limitOk && (high ? value >= limit : value <= limit))
      text = QString("The value %1 is %2 the %3 limit of %4, configured as %5.")
                 .arg(s.value, high ? "at or above" : "at or below", status, limits.value(status),
                      limits.value(severityField.value(status)));
    else {
      text = QString("The record reports a %1 limit alarm with severity %2 for value %3.").arg(status, severity, s.value);
      if (limitOk) text += " The " + status + " limit is " + limits.value(status) +
                           "; hysteresis or filtering can retain an alarm after the value crosses back.";
    }
  } else if (status == "STATE") text = "The record's current state (" + s.value + ") is configured as " + severity + ".";
  else if (status == "COS") text = "The record changed state; change of state is configured as " + severity + ".";
  else if (status == "LINK") text = "One of the record's input links is disconnected or failed (" + severity + ").";
  else if (status == "UDF") text = "The record's value is undefined; it has not processed successfully (" + severity + ").";
  else if (status == "COMM" || status == "TIMEOUT" || status == "READ" || status == "WRITE")
    text = "The record reports a hardware " + status.toLower() + " problem (" + severity + ").";
  else if (status == "NOT_CONNECTED") text = "QtALH cannot connect to this PV; check the IOC and network.";
  else if (status == "NO_READ_ACCESS") text = "QtALH has no read access to this PV.";
  else if (status == "NO_WRITE_ACCESS") text = "QtALH has no write access to this PV for global acknowledgement.";
  else text = "The record reports status " + status + " with severity " + severity + ".";
  if (s.unack && s.unack != s.severity)
    text += " An earlier " + severityName(s.unack) + " alarm is still unacknowledged.";
  if (s.filterUntil && s.pending.severity == s.severity)
    text += " A count filter is holding the latest change.";
  if (s.noAckUntil) text += " Acknowledgement is suspended for one hour (NoAck timer).";
  return text;
}
} // namespace

void Window::reveal(Node* n) {
  if (!n) return;
  show();
  raise();
  activateWindow();
  Node* group = n->group ? n : n->parent;
  const auto index = treeIndex(group);
  QModelIndex channelIndex;
  if (!n->group) {
    // Check the channel itself before changing the current selection. Its
    // group may be visible because a sibling passes the display filter.
    AlarmModel contents(&doc, engine.get(), false);
    contents.setGroup(group);
    contents.setFilter(options.editor ? 0 : options.filter);
    for (int row = 0; row < contents.rowCount(); ++row)
      if (contents.node(contents.index(row, 2)) == n) {
        channelIndex = contents.index(row, 2);
        break;
      }
  }
  if (!index.isValid() || (!n->group && !channelIndex.isValid())) {
    showMessage(Engine::channelPath(n) + " is hidden by the display filter (Setup → Display Filter).");
    return;
  }
  for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) treeView->expand(parent);
  {
    const QSignalBlocker block(treeView->selectionModel());
    treeView->setCurrentIndex(index.sibling(index.row(), 2));
  }
  treeView->scrollTo(index);
  select(group);
  if (!n->group)
    for (int row = 0; row < groupModel->rowCount(); ++row)
      if (groupModel->node(groupModel->index(row, 2)) == n) {
        groupView->setCurrentIndex(groupModel->index(row, 2));
        groupView->scrollTo(groupModel->index(row, 2));
        selection = n;
        break;
      }
  scheduleDialogSync();
}

void Window::showActiveAlarms() {
  if (options.editor) return;
  if (activeAlarmsDialog) {
    activeAlarmsDialog->show(); activeAlarmsDialog->raise(); activeAlarmsDialog->activateWindow();
    return;
  }
  auto dialog = new QDialog(this);
  activeAlarmsDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("activeAlarmsDialog");
  dialog->setWindowTitle("Active Alarms: " + doc.root->label());
  auto layout = dialogColumn(dialog);
  auto controls = dialogRow(layout);
  auto filter = new QLineEdit;
  filter->setObjectName("activeAlarmsFilter");
  auto unackOnly = new QCheckBox("Unacknowledged only");
  unackOnly->setObjectName("activeUnacknowledgedOnly");
  if (legacyAppearance()) controls->addWidget(new QLabel("Filter:"));
  else {
    filter->setPlaceholderText("Filter by channel, status, value or group");
    filter->setClearButtonEnabled(true);
  }
  controls->addWidget(filter, 1);
  controls->addWidget(unackOnly);
  auto model = new ActiveAlarmModel(dialog);
  model->setObjectName("activeAlarmModel");
  auto proxy = new ActiveAlarmFilter(dialog);
  proxy->setSourceModel(model);
  proxy->setSortRole(SortRole);
  proxy->setFilterRole(FilterRole);
  proxy->setFilterKeyColumn(-1);
  proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
  proxy->setDynamicSortFilter(true);
  auto table = new QTableView;
  table->setObjectName("activeAlarmsTable");
  table->setModel(proxy);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSortingEnabled(true);
  table->sortByColumn(1, Qt::DescendingOrder);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  table->setWordWrap(false);
  if (!legacyAppearance()) {
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
  }
  layout->addWidget(table, 1);
  auto status = new QLabel;
  status->setObjectName("activeAlarmsStatus");
  if (!legacyAppearance()) status->setForegroundRole(QPalette::PlaceholderText);
  layout->addWidget(status);
  auto actions = dialogRow(layout);
  auto acknowledge = new QPushButton("Acknowledge");
  acknowledge->setObjectName("activeAcknowledge");
  auto guidanceButton = new QPushButton("Guidance");
  auto details = new QPushButton("Details...");
  auto showButton = new QPushButton("Show in Tree");
  showButton->setObjectName("activeShowInTree");
  for (auto button : {acknowledge, guidanceButton, details, showButton}) actions->addWidget(button);
  actions->addStretch();
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "active-alarms");
  auto updateButtons = [=] {
    const auto nodes = selectedNodes(table);
    bool unack = false;
    for (auto n : nodes) unack |= engine->state(n).unack != 0;
    acknowledge->setEnabled(unack && !options.engine.passive);
    for (auto button : {guidanceButton, details, showButton}) button->setEnabled(nodes.size() == 1);
  };
  connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, dialog, updateButtons);
  connect(proxy, &QAbstractItemModel::dataChanged, dialog, updateButtons);
  connect(filter, &QLineEdit::textChanged, proxy, &QSortFilterProxyModel::setFilterFixedString);
  connect(unackOnly, &QCheckBox::toggled, dialog, [proxy](bool on) {
    proxy->unacknowledgedOnly = on;
    proxy->refilter();
  });
  connect(acknowledge, &QPushButton::clicked, dialog, [=] {
    for (auto n : selectedNodes(table)) engine->acknowledge(n);
    refresh();
  });
  connect(guidanceButton, &QPushButton::clicked, dialog, [=] {
    const auto nodes = selectedNodes(table);
    if (nodes.size() == 1) guidance(nodes.front());
  });
  connect(details, &QPushButton::clicked, dialog, [=] {
    const auto nodes = selectedNodes(table);
    if (nodes.size() != 1) return;
    reveal(nodes.front());
    alarmDetails();
  });
  connect(showButton, &QPushButton::clicked, dialog, [=] {
    const auto nodes = selectedNodes(table);
    if (nodes.size() == 1) reveal(nodes.front());
  });
  connect(table, &QTableView::doubleClicked, dialog,
          [=](const QModelIndex& index) { reveal(nodeOf(index.siblingAtColumn(0).data(Qt::UserRole))); });
  refreshTools();
  table->resizeColumnsToContents();
  table->setColumnWidth(3, qMax(table->columnWidth(3), 220));
  updateButtons();
  sizeDialog(dialog, QSize(900, 420));
  dialog->show();
}

void Window::findAlarm() {
  if (findDialog) {
    findDialog->show(); findDialog->raise(); findDialog->activateWindow();
    if (auto text = findDialog->findChild<QLineEdit*>("findAlarmText")) { text->setFocus(); text->selectAll(); }
    return;
  }
  auto dialog = new QDialog(this);
  findDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("findAlarmDialog");
  dialog->setWindowTitle("Find Alarm");
  auto layout = dialogColumn(dialog);
  layout->addWidget(new QLabel("Name, alias, PV or path contains:"));
  auto text = new QLineEdit;
  text->setObjectName("findAlarmText");
  if (!legacyAppearance()) {
    text->setPlaceholderText("For example: vacuum or B:VAC:Gauge1");
    text->setClearButtonEnabled(true);
  }
  layout->addWidget(text);
  auto results = new QListWidget;
  results->setObjectName("findAlarmResults");
  layout->addWidget(results, 1);
  auto status = new QLabel;
  status->setObjectName("findAlarmStatus");
  if (!legacyAppearance()) status->setForegroundRole(QPalette::PlaceholderText);
  layout->addWidget(status);
  auto buttons = dialogActions(layout, dialog, QDialogButtonBox::Close,
                               "find-alarm");
  auto showButton = buttons->addButton("Show in Tree", QDialogButtonBox::ActionRole);
  showButton->setObjectName("findShowInTree");
  struct Entry { Node* node; QString text, haystack; };
  auto entries = std::make_shared<QVector<Entry>>();
  for (auto n : doc.nodes()) {
    const auto path = Engine::channelPath(n);
    const auto label = n->label();
    entries->push_back({n, (n->group ? "Group: " : QString()) + label +
                               (label != n->name ? "  (" + n->name + ")" : QString()) + "    " + path,
                        (n->name + '\n' + label + '\n' + path).toLower()});
  }
  auto update = [=] {
    results->clear();
    const auto query = text->text().trimmed().toLower();
    if (query.isEmpty()) {
      status->setText(QString("%1 groups and channels.").arg(entries->size()));
      return;
    }
    constexpr int Shown = 500;
    int matches = 0;
    for (const auto& entry : *entries)
      if (entry.haystack.contains(query) && ++matches <= Shown) {
        auto item = new QListWidgetItem(entry.text, results);
        item->setData(Qt::UserRole, nodeValue(entry.node));
      }
    if (results->count()) results->setCurrentRow(0);
    status->setText(matches > Shown ? QString("First %1 of %2 matches; refine the text.").arg(Shown).arg(matches)
                    : matches ? QString("%1 match%2.").arg(matches).arg(matches == 1 ? "" : "es")
                              : QString("No matches."));
  };
  auto showCurrent = [=] {
    if (auto item = results->currentItem()) reveal(nodeOf(item->data(Qt::UserRole)));
  };
  // Debounce typing in very large configurations.
  auto timer = new QTimer(dialog);
  timer->setSingleShot(true);
  timer->setInterval(entries->size() > 5000 ? 150 : 0);
  connect(timer, &QTimer::timeout, dialog, update);
  connect(text, &QLineEdit::textChanged, timer, qOverload<>(&QTimer::start));
  connect(text, &QLineEdit::returnPressed, dialog, showCurrent);
  connect(results, &QListWidget::itemActivated, dialog, showCurrent);
  connect(showButton, &QPushButton::clicked, dialog, showCurrent);
  text->installEventFilter(new ArrowKeys(results, text));
  update();
  sizeDialog(dialog, QSize(560, 380));
  dialog->show();
  text->setFocus();
}

void Window::alarmDetails() {
  if (!selection || options.editor) return;
  auto n = selection;
  auto dialog = beginSelectionDialog("alarmDetailsDialog", "Alarm Details", [this] { alarmDetails(); });
  if (!dialog) return;
  auto layout = dialogColumn(dialog);
  dialogHeading(layout, n->group ? "Group Name:" : "Channel Name:", n->label());
  auto form = new QFormLayout;
  form->setHorizontalSpacing(legacyAppearance() ? 8 : 14);
  layout->addLayout(form);
  auto field = [&](const QString& label, const QString& name) {
    auto value = new QLabel;
    value->setObjectName(name);
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value);
    return value;
  };
  field("Path", "detailsPath")->setText(Engine::channelPath(n));
  if (!n->group) field("PV", "detailsPv")->setText(n->name);
  auto severity = field("Severity", "detailsSeverity");
  auto status = n->group ? nullptr : field("Status", "detailsStatus");
  auto value = n->group ? nullptr : field("Value", "detailsValue");
  auto unack = field("Unacknowledged", "detailsUnacknowledged");
  auto changed = n->group ? nullptr : field("Last change", "detailsChanged");
  auto counts = n->group ? field("Alarm counts", "detailsCounts") : nullptr;
  auto mask = field("Mask", "detailsMask");
  auto beep = field("Beep severity", "detailsBeep");
  if (!n->group) {
    const auto filter = n->option("ALARMCOUNTFILTER");
    field("Count filter", "detailsFilter")->setText(filter.isEmpty() ? QString("None") : filter);
  }
  auto force = field("Force PV", "detailsForce");
  auto explanation = new QLabel;
  explanation->setObjectName("alarmExplanation");
  explanation->setWordWrap(true);
  explanation->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto why = dialogFrame(layout, n->group ? QString("Summary") : QString("Why"));
  why->addWidget(explanation);
  QLabel* limits = nullptr;
  if (!n->group) {
    auto limitFrame = dialogFrame(layout, "Alarm limits from the IOC");
    limits = new QLabel(ca ? "Reading…" : "Not available without Channel Access.");
    limits->setObjectName("alarmLimits");
    limits->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setPresentationFont(limits, contentFont());
    limitFrame->addWidget(limits);
  }
  layout->addStretch();
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "alarm-details");
  // Limit values feed the explanation; they belong to this build of the window.
  auto limitValues = std::make_shared<QHash<QString, QString>>();
  auto update = [=] {
    const auto& s = engine->state(n);
    const auto shown = engine->presentation(n);
    severity->setText(severityName(n->group ? shown.severity : s.severity));
    unack->setText(severityName(n->group ? shown.unack : s.unack));
    if (status) status->setText(statusName(s.status));
    if (value) value->setText(s.value);
    if (changed) changed->setText(localTime(s.changedAt));
    if (counts)
      counts->setText(QString("%1 ERROR, %2 INVALID, %3 MAJOR, %4 MINOR, %5 normal%6")
                          .arg(shown.counts[4]).arg(shown.counts[3]).arg(shown.counts[2])
                          .arg(shown.counts[1]).arg(shown.counts[0])
                          .arg(shown.shelved ? QString("; %1 shelved").arg(shown.shelved) : QString()));
    QString current = s.mask.text();
    if (n->group)
      for (int i = 0; i < 5; ++i) current[i] = s.maskCounts[i] ? QString("CDATL")[i] : QChar('-');
    mask->setText(n->group ? current + " (channels with each setting)"
                           : current + (current == n->mask.text() ? QString()
                                                                   : "  (configured " + n->mask.text() + ")"));
    const int configured = configuredBeep.value(n, 1);
    beep->setText(severityName(s.beepThreshold) +
                  (s.beepThreshold == configured ? QString() : "  (configured " + severityName(configured) + ")"));
    const auto forcePv = n->option("FORCEPV");
    force->setText(forcePv.isEmpty() ? QString("None")
                                     : forcePv.section(' ', 0, 0) + (s.forceDisabled ? "  (disabled)" : QString()));
    if (n->group) {
      const int active = shown.counts[1] + shown.counts[2] + shown.counts[3] + shown.counts[4];
      const int waiting = shown.unackCounts[1] + shown.unackCounts[2] + shown.unackCounts[3] + shown.unackCounts[4];
      explanation->setText(active || waiting
          ? QString("%1 channel%2 in alarm and %3 awaiting acknowledgement. Use View → Active Alarms to list them.")
                .arg(active).arg(active == 1 ? "" : "s").arg(waiting)
          : QString("No channel in this group is in alarm."));
    } else
      explanation->setText(explain(s, *limitValues));
  };
  update();
  // Owned by window content, so a rebuild for a new selection replaces it.
  auto timer = new QTimer(explanation);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, explanation, update);
  timer->start();
  if (limits && ca) {
    const auto record = n->name.section('.', 0, 0);
    QPointer<QLabel> target = limits;
    ca->readStrings({record + ".RTYP"}, [this, target, record, limitValues, update](QHash<QString, QString> type) {
      if (!target || !ca) return;
      const auto recordType = type.value(record + ".RTYP");
      const auto fields = limitFields(recordType);
      if (recordType.isEmpty()) {
        target->setText("Not available: the record does not respond.");
        return;
      }
      if (fields.isEmpty()) {
        target->setText(describeLimits(recordType, {}));
        return;
      }
      QStringList names;
      for (const auto& f : fields) names << record + '.' + f;
      ca->readStrings(names, [target, record, recordType, limitValues, update](QHash<QString, QString> read) {
        if (!target) return;
        for (auto it = read.cbegin(); it != read.cend(); ++it)
          limitValues->insert(it.key().section('.', 1), it.value());
        target->setText(describeLimits(recordType, *limitValues));
        update();
      });
    });
  }
  sizeDialog(dialog, QSize(520, legacyAppearance() ? 360 : 520));
  finishSelectionDialog(dialog);
}

void Window::showOverrides() {
  if (options.editor) return;
  if (overridesDialog) {
    overridesDialog->show(); overridesDialog->raise(); overridesDialog->activateWindow();
    return;
  }
  auto dialog = new QDialog(this);
  overridesDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("runtimeOverridesDialog");
  dialog->setWindowTitle("Runtime Overrides: " + doc.root->label());
  auto layout = dialogColumn(dialog);
  auto hint = new QLabel("Settings changed in this runtime and still in effect. Restore returns the "
                         "selected items to the configuration.");
  hint->setWordWrap(true);
  if (!legacyAppearance()) hint->setForegroundRole(QPalette::PlaceholderText);
  layout->addWidget(hint);
  auto table = new QTableWidget(0, 4);
  table->setObjectName("runtimeOverridesTable");
  table->setHorizontalHeaderLabels({"Group or channel", "Override", "Current", "Configured"});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setStretchLastSection(true);
  if (!legacyAppearance()) {
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
  }
  layout->addWidget(table, 1);
  auto status = new QLabel;
  status->setObjectName("runtimeOverridesStatus");
  layout->addWidget(status);
  auto actions = dialogRow(layout);
  auto restore = new QPushButton("Restore Selected");
  restore->setObjectName("restoreOverrides");
  auto showButton = new QPushButton("Show in Tree");
  actions->addWidget(restore);
  actions->addWidget(showButton);
  actions->addStretch();
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "runtime-overrides");
  auto selected = [table] {
    QVector<QPair<Node*, int>> items;
    for (const auto& index : table->selectionModel()->selectedRows())
      if (auto item = table->item(index.row(), 0))
        items << qMakePair(nodeOf(item->data(Qt::UserRole)), item->data(SortRole).toInt());
    return items;
  };
  auto updateButtons = [=] {
    const auto items = selected();
    restore->setEnabled(!items.isEmpty() && !options.engine.passive);
    showButton->setEnabled(items.size() == 1);
  };
  connect(table, &QTableWidget::itemSelectionChanged, dialog, updateButtons);
  connect(restore, &QPushButton::clicked, dialog, [=] {
    for (const auto& item : selected()) {
      Node* n = item.first;
      switch (item.second) {
      case 0: engine->resetMask(n); break;
      case 1: engine->setBeep(n, configuredBeep.value(n, 1)); break;
      case 2: engine->setForceDisabled(n, false); break;
      case 3: engine->noAck(n, false); break;
      case 4: engine->unshelve(n); break;
      case 5:
        doc.beepSeverity = configuredBeepSeverity;
        if (logging) logging->operation(nullptr, "Restore ALH beep severity " + severityName(doc.beepSeverity));
        break;
      }
    }
    refresh();
  });
  connect(showButton, &QPushButton::clicked, dialog, [=] {
    const auto items = selected();
    if (items.size() == 1) reveal(items.front().first);
  });
  connect(table, &QTableWidget::cellDoubleClicked, dialog, [=](int row, int) {
    if (auto item = table->item(row, 0)) reveal(nodeOf(item->data(Qt::UserRole)));
  });
  refreshTools();
  table->resizeColumnsToContents();
  updateButtons();
  sizeDialog(dialog, QSize(820, 380));
  dialog->show();
}

void Window::showSummary() {
  if (options.editor) return;
  if (summaryDialog) {
    summaryDialog->show(); summaryDialog->raise(); summaryDialog->activateWindow();
    return;
  }
  auto dialog = new QDialog(this);
  summaryDialog = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("alarmSummaryDialog");
  dialog->setWindowTitle("Alarm Summary: " + doc.root->label());
  auto layout = dialogColumn(dialog);
  // One tile per top-level group, or the facility when it has no subgroups.
  QVector<Node*> areas;
  bool directChannels = false;
  for (const auto& child : doc.root->children)
    if (child->group) areas << child.get();
    else directChannels = true;
  if (areas.isEmpty() || directChannels) areas.prepend(doc.root.get());
  const int columns = qMin(4, int(std::ceil(std::sqrt(double(areas.size())))));
  auto grid = new QGridLayout;
  grid->setSpacing(legacyAppearance() ? 4 : 8);
  for (int i = 0; i < areas.size(); ++i) {
    auto tile = new MotifButton(areas[i]->label());
    tile->setObjectName("summaryTile");
    tile->setProperty("summaryNode", nodeValue(areas[i]));
    tile->setMinimumSize(170, 64);
    tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    tile->setToolTip(Engine::channelPath(areas[i]) + "\nClick to show this group in the main window.");
    Node* area = areas[i];
    connect(tile, &QPushButton::clicked, this, [this, area] { reveal(area); });
    grid->addWidget(tile, i / columns, i % columns);
  }
  layout->addLayout(grid, 1);
  auto totals = new QLabel;
  totals->setObjectName("summaryTotals");
  totals->setAlignment(Qt::AlignCenter);
  layout->addWidget(totals);
  refreshTools();
  sizeDialog(dialog, QSize(qMax(260, columns * 190), qMax(140, int((areas.size() + columns - 1) / columns) * 76 + 40)));
  dialog->show();
}

void Window::refreshTools() {
  if (!engine || options.editor) return;
  if (activeAlarmsDialog) {
    auto model = activeAlarmsDialog->findChild<ActiveAlarmModel*>("activeAlarmModel");
    QVector<ActiveRow> rows;
    int shelved = 0, unacknowledged = 0;
    for (auto n : doc.channels()) {
      const auto shown = engine->presentation(n);
      if (shown.shelved) { ++shelved; continue; }
      if (!shown.severity && !shown.unack) continue;
      const auto& s = engine->state(n);
      ActiveRow row;
      row.node = n; row.severity = shown.severity; row.unack = shown.unack;
      row.status = s.status; row.changed = s.changedAt; row.value = s.value;
      unacknowledged += row.unack != 0;
      rows << row;
    }
    const int count = rows.size();
    if (model) model->update(std::move(rows));
    if (auto status = activeAlarmsDialog->findChild<QLabel*>("activeAlarmsStatus"))
      status->setText(QString("%1 alarm%2, %3 unacknowledged%4").arg(count).arg(count == 1 ? "" : "s")
                          .arg(unacknowledged)
                          .arg(shelved ? QString("; %1 shelved not shown").arg(shelved) : QString()));
  }
  if (overridesDialog) {
    auto table = overridesDialog->findChild<QTableWidget*>("runtimeOverridesTable");
    struct Item { Node* node; int kind; QString what, current, configured; };
    QVector<Item> items;
    if (doc.beepSeverity != configuredBeepSeverity)
      items.push_back({doc.root.get(), 5, "ALH beep severity", severityName(doc.beepSeverity),
                       severityName(configuredBeepSeverity)});
    for (auto n : doc.nodes()) {
      const auto& s = engine->state(n);
      if (!n->group && s.mask.text() != n->mask.text())
        items.push_back({n, 0, "Mask", s.mask.text(), n->mask.text()});
      const int configured = configuredBeep.value(n, 1);
      if (s.beepThreshold != configured)
        items.push_back({n, 1, "Beep severity", severityName(s.beepThreshold), severityName(configured)});
      if (s.forceDisabled) items.push_back({n, 2, "Force PV disabled", "Disabled", "Enabled"});
      if (s.noAckUntil && (!n->parent || !engine->state(n->parent).noAckUntil))
        items.push_back({n, 3, "NoAck for one hour", "Active", "Off"});
      if (s.shelf.until)
        items.push_back({n, 4, "Shelved", "Until " + localTime(s.shelf.until) + " by " + s.shelf.username,
                         "Not shelved"});
    }
    QStringList signature;
    for (const auto& item : items)
      signature << QString::number(reinterpret_cast<quintptr>(item.node)) + item.what + item.current;
    if (table && table->property("signature").toStringList() != signature) {
      table->setProperty("signature", signature);
      QSet<QString> keep;
      for (const auto& index : table->selectionModel()->selectedRows())
        if (auto item = table->item(index.row(), 0))
          keep << QString::number(item->data(Qt::UserRole).value<quintptr>()) + ':' + item->data(SortRole).toString();
      {
        const QSignalBlocker block(table);
        table->clearSelection();
        table->setRowCount(items.size());
        for (int row = 0; row < items.size(); ++row) {
          const auto& item = items[row];
          const QStringList values = {Engine::channelPath(item.node), item.what, item.current, item.configured};
          for (int column = 0; column < 4; ++column) {
            auto cell = table->item(row, column);
            if (!cell) table->setItem(row, column, cell = new QTableWidgetItem);
            cell->setText(values[column]);
            cell->setToolTip(values[column]);
          }
          table->item(row, 0)->setData(Qt::UserRole, nodeValue(item.node));
          table->item(row, 0)->setData(SortRole, item.kind);
          if (keep.contains(QString::number(reinterpret_cast<quintptr>(item.node)) + ':' + QString::number(item.kind)))
            table->selectionModel()->select(table->model()->index(row, 0),
                                            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
      }
      emit table->itemSelectionChanged();
    }
    if (auto status = overridesDialog->findChild<QLabel*>("runtimeOverridesStatus"))
      status->setText(items.isEmpty() ? QString("No runtime overrides: everything matches the configuration.")
                                      : QString("%1 override%2.").arg(items.size()).arg(items.size() == 1 ? "" : "s"));
  }
  if (summaryDialog) {
    const auto facility = engine->presentation(doc.root.get());
    int active = 0, waiting = 0;
    for (int severity = 1; severity < SeverityCount; ++severity) {
      active += facility.counts[severity];
      waiting += facility.unackCounts[severity];
    }
    const bool blink = (engine->now() / 1000) % 2;
    for (auto tile : summaryDialog->findChildren<MotifButton*>("summaryTile")) {
      auto n = nodeOf(tile->property("summaryNode"));
      const auto shown = engine->presentation(n);
      const int inAlarm = shown.counts[1] + shown.counts[2] + shown.counts[3] + shown.counts[4];
      const int unack = shown.unackCounts[1] + shown.unackCounts[2] + shown.unackCounts[3] + shown.unackCounts[4];
      // Like the runtime window: unacknowledged alarms blink, acknowledged stay solid.
      const auto background = severityColor(shown.unack ? (blink ? shown.unack : 0) : shown.severity);
      QStringList lines{n->label()};
      if (inAlarm) lines << QString("%1 in alarm").arg(inAlarm);
      if (unack) lines << QString("%1 unacknowledged").arg(unack);
      if (!inAlarm && !unack) lines << "No alarms";
      if (shown.shelved) lines << QString("%1 shelved").arg(shown.shelved);
      tile->setText(lines.join('\n'));
      if (tile->background != background) {
        tile->background = background;
        tile->update();
      }
    }
    if (auto totals = summaryDialog->findChild<QLabel*>("summaryTotals"))
      totals->setText(QString("%1 in alarm, %2 unacknowledged").arg(active).arg(waiting));
  }
}

void Window::showConversionNotes(const QString& title, const QStringList& notes, QWidget* parent) {
  if (notes.isEmpty()) return;
  QMessageBox box(QMessageBox::Information, title,
                  QString("Converted with %1 note%2. Some settings have no equivalent in the "
                          "other format; Show Details lists them with example paths.")
                      .arg(notes.size()).arg(notes.size() == 1 ? "" : "s"),
                  QMessageBox::Ok, parent);
  box.setObjectName("conversionNotes");
  box.setDetailedText(notes.join('\n'));
  box.exec();
}

void Window::importPhoebus() {
  auto path = chooseFile(this, "Import Phoebus Alarm Configuration", options.configDir,
                         "Phoebus alarm configurations (*.xml);;All files (*)");
  if (path.isEmpty()) return;
  try {
    QStringList notes;
    auto imported = loadPhoebusXml(path, &notes);
    // Review and save the result in the configuration tool.
    auto editorOptions = options;
    editorOptions.editor = true;
    editorOptions.config.clear();
    auto window = new Window(std::move(imported), editorOptions, connectPv);
    window->modified = true;
    window->showInitial();
    window->refresh();
    showConversionNotes("Phoebus Import", notes, window);
  } catch (const std::exception& e) {
    error(e.what());
  }
}

void Window::exportPhoebus() {
  const QFileInfo source(doc.filename);
  const auto suggested = doc.filename.isEmpty() ? options.configDir
                                                : source.absolutePath() + '/' + source.completeBaseName() + ".xml";
  auto path = chooseFile(this, "Export Phoebus Alarm Configuration", suggested,
                         "Phoebus alarm configurations (*.xml)", true);
  if (path.isEmpty()) return;
  try {
    QStringList notes;
    savePhoebusXml(doc, path, &notes);
    if (logging) logging->operation(nullptr, "Setup Export Phoebus XML: " + path);
    showConversionNotes("Phoebus Export", notes, this);
  } catch (const std::exception& e) {
    error(e.what());
  }
}
} // namespace alh
