#include "core/diagnostics.h"
#include "window.h"
#include "dialogs.h"
#include "services/log_browser.h"
#include <QThread>
#include <QtWidgets>
namespace alh {
namespace {
class LogSearchJob final : public QThread {
public:
  LogSearchJob(LogSearch search, QObject* parent) : QThread(parent), request(std::move(search)) {}
  ~LogSearchJob() override {
    requestInterruption();
    wait();
  }
  LogSearchResult result;
private:
  LogSearch request;
  void run() override {
    result = searchLogs(request, [this] { return isInterruptionRequested(); });
  }
};
}
void Window::activateRuntime() {
  if (!options.editor) return;
  try {
    // Validate and clone the edited configuration; runtime masks must not mutate the editor.
    auto snapshot = parseConfig(writeConfig(doc));
    if (doc.filename.isEmpty()) {
      save(true);
      if (doc.filename.isEmpty()) return;
    }
    snapshot.filename = doc.filename;
    auto runtimeOptions = options;
    runtimeOptions.editor = false;
    runtimeOptions.config = doc.filename;
    debugLog(options.debug, "window", "activate runtime from " + snapshot.filename);
    auto window = new Window(std::move(snapshot), runtimeOptions, connectPv);
    window->showInitial();
  } catch (const std::exception& e) {
    error(e.what());
  }
}
void Window::showLogBrowser(bool alarm) {
  if (!logging) return;
  auto dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName(alarm ? "alarmLogBrowser" : "opmodLogBrowser");
  dialog->setWindowTitle(alarm ? "Browser for Alarm Log" : "Browser for Operation Log");
  dialog->resize(800, 420);
  auto layout = dialogColumn(dialog);
  auto fileRow = dialogRow(layout);
  fileRow->addWidget(new QLabel("Log file:"));
  auto path = new QLineEdit(alarm ? logging->alarmPath() : logging->opmodPath());
  path->setObjectName("logPath");
  fileRow->addWidget(path, 1);
  auto browse = new QPushButton("Browse...");
  fileRow->addWidget(browse);
  connect(browse, &QPushButton::clicked, dialog, [=] {
    auto chosen = chooseFile(dialog, "Log File", path->text(), "All files (*)");
    if (!chosen.isEmpty()) path->setText(chosen);
  });
  auto interval = dialogRow(layout);
  auto from = new QDateTimeEdit(QDateTime(QDate::currentDate(), QTime(0, 0)));
  auto to = new QDateTimeEdit(QDateTime(QDate::currentDate(), QTime(23, 59)));
  from->setObjectName("logFrom");
  to->setObjectName("logTo");
  for (auto field : {from, to}) {
    field->setDisplayFormat("yyyy-MM-dd HH:mm");
    field->setCalendarPopup(true);
  }
  interval->addWidget(new QLabel("From:")); interval->addWidget(from);
  interval->addWidget(new QLabel("To:")); interval->addWidget(to);
  interval->addStretch();
  auto filterRow = dialogRow(layout);
  filterRow->addWidget(new QLabel("With:"));
  auto contains = new QLineEdit;
  contains->setObjectName("logContains");
  filterRow->addWidget(contains, 1);
  auto search = new QPushButton("ShowSelected");
  search->setObjectName("logSearch");
  filterRow->addWidget(search);
  auto stop = new QPushButton("Stop");
  stop->setEnabled(false);
  filterRow->addWidget(stop);
  auto text = new QPlainTextEdit;
  text->setObjectName("logResults");
  text->setReadOnly(true);
  if (!legacyAppearance()) setPresentationFont(text, contentFont());
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  layout->addWidget(text, 1);
  auto status = new QLabel;
  status->setObjectName("logSearchStatus");
  status->setWordWrap(true);
  layout->addWidget(status);
  auto findRow = dialogRow(layout);
  findRow->addWidget(new QLabel("Search:"));
  auto find = new QLineEdit;
  findRow->addWidget(find, 1);
  for (bool backwards : {false, true}) {
    auto button = new QPushButton(backwards ? "Reverse" : "Forward");
    findRow->addWidget(button);
    connect(button, &QPushButton::clicked, dialog, [=] {
      auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags();
      if (!text->find(find->text(), flags)) {
        text->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        text->find(find->text(), flags);
      }
    });
  }
  auto running = std::make_shared<QPointer<LogSearchJob>>();
  connect(search, &QPushButton::clicked, dialog, [=] {
    if (*running) return;
    LogSearch request;
    request.path = path->text(); request.contains = contains->text();
    request.from = from->dateTime();
    // The UI interval includes the entire final minute, as in the legacy browser.
    request.to = to->dateTime().addSecs(60).addMSecs(-1);
    if (request.path.trimmed().isEmpty() || from->dateTime() > to->dateTime()) {
      status->setText("Choose a log file and a From time no later than To.");
      return;
    }
    auto job = new LogSearchJob(request, dialog);
    *running = job;
    search->setEnabled(false); stop->setEnabled(true);
    status->setText("Searching current and dated log files...");
    connect(job, &QThread::finished, dialog, [=] {
      const auto& result = job->result;
      text->setPlainText(result.text);
      QString summary = QString("%1 matching records").arg(result.records);
      if (result.truncated) summary += "; result limit reached — narrow the search";
      if (result.cancelled) summary += "; search stopped";
      if (result.skipped) summary += QString("; %1 lines without a recognized timestamp").arg(result.skipped);
      if (!result.errors.isEmpty()) summary += "\n" + result.errors.mid(0, 3).join('\n');
      status->setText(summary);
      *running = nullptr;
      search->setEnabled(true); stop->setEnabled(false);
      job->deleteLater();
    });
    job->start();
  });
  connect(stop, &QPushButton::clicked, dialog, [running] {
    if (*running) (*running)->requestInterruption();
  });
  dialogActions(layout, dialog, QDialogButtonBox::Close,
                "ShowSelected searches the current log and its .yyyy-MM-dd files within the "
                "inclusive local-time interval. With is a case-sensitive text filter. "
                "Results are chronological and limited to 100,000 records or 10 MiB.");
  dialog->show();
  QTimer::singleShot(0, search, &QPushButton::click);
}

void Window::shelveDialog(Node* target, bool change) {
  if (options.editor || !target) return;
  for (auto prior : findChildren<QDialog*>("shelveDialog")) {
    if (!prior->isEnabled()) continue;
    if (prior->property("shelfTargetNode").value<quintptr>() == reinterpret_cast<quintptr>(target) &&
        prior->property("shelfChange").toBool() == change) {
      prior->show(); prior->raise(); prior->activateWindow(); return;
    }
    prior->setEnabled(false);
    prior->close();
  }
  auto dialog = new QDialog(this);
  dialog->setObjectName("shelveDialog");
  dialog->setProperty("shelfTargetNode", QVariant::fromValue(reinterpret_cast<quintptr>(target)));
  dialog->setProperty("shelfChange", change);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(change ? "Change Shelf" : "Shelve Alarms");
  auto layout = dialogColumn(dialog);
  auto targetLabel = new QLabel(Engine::channelPath(target));
  targetLabel->setTextFormat(Qt::PlainText);
  targetLabel->setWordWrap(true);
  targetLabel->setObjectName("shelfTarget");
  layout->addWidget(targetLabel);
  auto scope = new QLabel;
  scope->setObjectName("shelfScope");
  layout->addWidget(scope);
  auto explanation = new QLabel("Shelving affects this runtime's display and sound only. Monitoring, logging, "
      "commands and output PVs continue. Shelves survive reloads, but end when this runtime closes.");
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto form = new QFormLayout;
  auto duration = new QComboBox;
  duration->setObjectName("shelfDuration");
  for (int minutes : {15, 30, 60, 240, 480})
    duration->addItem(minutes < 60 ? QString("%1 minutes").arg(minutes) :
        QString("%1 hour%2").arg(minutes / 60).arg(minutes == 60 ? "" : "s"), minutes);
  duration->addItem("Custom minutes", 0);
  duration->addItem("Custom hours", -60);
  duration->addItem("Custom days", -1440);
  duration->setCurrentIndex(2);
  auto custom = new QSpinBox;
  custom->setObjectName("shelfCustomMinutes");
  custom->setRange(1, Engine::MaximumShelfMinutes);
  custom->setProperty("minutesPerUnit", 1);
  custom->setSuffix(" minutes");
  custom->setValue(60);
  custom->setEnabled(false);
  form->addRow("Duration", duration);
  auto customLabel = new QLabel("Custom minutes");
  form->addRow(customLabel, custom);
  auto username = new QLineEdit;
  username->setObjectName("shelfUsername");
  username->setMaxLength(120);
  username->setPlaceholderText("Enter your username");
  form->addRow("Username (required)", username);
  auto reason = new QLineEdit;
  reason->setObjectName("shelfReason");
  reason->setMaxLength(240);
  if (change) reason->setText(engine->state(target).shelf.reason);
  form->addRow("Reason (required)", reason);
  layout->addLayout(form);
  auto status = new QLabel;
  status->setWordWrap(true);
  layout->addWidget(status);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText(change ? "Change Shelf" : "Shelve");
  layout->addWidget(buttons);
  auto update = [=] {
    if (!dialog->isEnabled()) return;
    int existing = 0, available = 0;
    for (auto n : doc.channels()) {
      auto ancestor = n;
      while (ancestor && ancestor != target) ancestor = ancestor->parent;
      if (!ancestor) continue;
      if (engine->state(n).shelf.until > engine->now()) ++existing;
      else ++available;
    }
    scope->setText(change ? "Change this channel's deadline and reason." :
        QString("%1 channels to shelve; %2 existing shelves retained.").arg(available).arg(existing));
    const auto text = reason->text().trimmed();
    buttons->button(QDialogButtonBox::Ok)->setEnabled((change || available > 0) &&
        Engine::validShelfUsername(username->text()) &&
        !text.isEmpty() && !text.contains(QRegularExpression("[\\r\\n\\x{2028}\\x{2029}]")));
  };
  connect(reason, &QLineEdit::textChanged, dialog, update);
  connect(username, &QLineEdit::textChanged, dialog, update);
  connect(duration, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog,
          [=] {
            const int value = duration->currentData().toInt();
            custom->setEnabled(value <= 0);
            if (value > 0) return;
            const int unit = qMax(1, -value);
            const int minutes = custom->value() * custom->property("minutesPerUnit").toInt();
            custom->setRange(1, Engine::MaximumShelfMinutes / unit);
            custom->setValue((minutes + unit - 1) / unit);
            custom->setProperty("minutesPerUnit", unit);
            custom->setSuffix(unit == 1440 ? " days" : unit == 60 ? " hours" : " minutes");
            customLabel->setText(duration->currentText());
          });
  auto timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, update);
  timer->start(1000);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
    if (!dialog->isEnabled()) return;
    try {
      const int value = duration->currentData().toInt();
      const int minutes = value > 0 ? value : custom->value() * qMax(1, -value);
      engine->shelve(target, minutes, reason->text(), username->text(), change);
      dialog->accept();
      refresh();
    } catch (const std::exception& e) { status->setText(e.what()); }
  });
  update();
  sizeDialog(dialog, QSize(560, dialog->sizeHint().height()));
  dialog->show();
  username->setFocus();
}
void Window::showShelvedAlarms() {
  if (options.editor) return;
  if (shelfListDialog && shelfListDialog->isEnabled()) {
    shelfListDialog->show(); shelfListDialog->raise(); shelfListDialog->activateWindow(); return;
  }
  auto dialog = new QDialog(this);
  shelfListDialog = dialog;
  dialog->setObjectName("shelvedAlarmsDialog");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Shelved Alarms — This Runtime");
  auto layout = dialogColumn(dialog);
  auto table = new QTableWidget(0, 8);
  table->setObjectName("shelvedAlarmsTable");
  table->setHorizontalHeaderLabels({"Channel path", "Severity", "Unacknowledged", "Expires (local)",
                                   "Remaining", "Reason", "Value", "Username"});
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->horizontalHeader()->setStretchLastSection(false);
  layout->addWidget(table);
  auto row = new QHBoxLayout;
  auto remove = new QPushButton("Unshelve Selected"); remove->setObjectName("unshelveSelected");
  auto change = new QPushButton("Change Shelf..."); change->setObjectName("changeShelf");
  auto close = new QPushButton("Close");
  row->addWidget(remove); row->addWidget(change); row->addStretch(); row->addWidget(close);
  layout->addLayout(row);
  connect(table, &QTableWidget::itemSelectionChanged, dialog, [=] {
    const int count = table->selectionModel()->selectedRows().size();
    remove->setEnabled(count > 0); change->setEnabled(count == 1);
  });
  connect(remove, &QPushButton::clicked, dialog, [=] {
    if (!dialog->isEnabled()) return;
    const auto rows = table->selectionModel()->selectedRows();
    for (const auto& row : rows) {
      auto n = reinterpret_cast<Node*>(table->item(row.row(), 0)->data(Qt::UserRole).value<quintptr>());
      engine->unshelve(n);
    }
    refresh();
  });
  connect(change, &QPushButton::clicked, dialog, [=] {
    if (!dialog->isEnabled()) return;
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.size() == 1)
      shelveDialog(reinterpret_cast<Node*>(table->item(rows[0].row(), 0)->data(Qt::UserRole).value<quintptr>()), true);
  });
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  auto timer = new QTimer(dialog);
  connect(timer, &QTimer::timeout, dialog, [this] { refreshShelfList(); });
  timer->start(1000);
  refreshShelfList();
  table->resizeColumnsToContents();
  dialog->resize(qMax(1050, table->horizontalHeader()->length() + table->verticalHeader()->width() + 80), 400);
  dialog->show();
}
void Window::refreshShelfList() {
  if (!shelfListDialog || !shelfListDialog->isEnabled()) return;
  auto table = shelfListDialog->findChild<QTableWidget*>("shelvedAlarmsTable");
  if (!table) return;
  QSet<quintptr> selected;
  for (const auto& row : table->selectionModel()->selectedRows())
    selected.insert(table->item(row.row(), 0)->data(Qt::UserRole).value<quintptr>());
  QSignalBlocker blocker(table);
  QVector<Node*> channels;
  for (auto n : doc.channels()) if (engine->state(n).shelf.until) channels.push_back(n);
  table->setRowCount(channels.size());
  table->clearSelection();
  for (int row = 0; row < channels.size(); ++row) {
    auto n = channels[row];
    const auto& s = engine->state(n);
    const auto seconds = qMax(qint64(0), (s.shelf.until - engine->now() + 999) / 1000);
    const QString remaining = QString("%1:%2:%3").arg(seconds / 3600).arg((seconds / 60) % 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'));
    const QStringList values{Engine::channelPath(n), severityName(s.severity), severityName(s.unack),
        QDateTime::fromMSecsSinceEpoch(s.shelf.until).toString(Qt::ISODate), remaining, s.shelf.reason, s.value, s.shelf.username};
    for (int col = 0; col < values.size(); ++col) {
      auto item = table->item(row, col);
      if (!item) { item = new QTableWidgetItem; table->setItem(row, col, item); }
      item->setText(values[col]); item->setToolTip(values[col]);
      if (col == 0) item->setData(Qt::UserRole, QVariant::fromValue(reinterpret_cast<quintptr>(n)));
    }
    if (selected.contains(reinterpret_cast<quintptr>(n)))
      table->selectionModel()->select(table->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
  }
  const int count = table->selectionModel()->selectedRows().size();
  shelfListDialog->findChild<QPushButton*>("unshelveSelected")->setEnabled(count > 0);
  shelfListDialog->findChild<QPushButton*>("changeShelf")->setEnabled(count == 1);
}

} // namespace alh
