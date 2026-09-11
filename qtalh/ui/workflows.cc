#include "core/diagnostics.h"
#include "window.h"
#include "dialogs.h"
#include "services/log_browser.h"
#include <QThread>
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
} // namespace alh
