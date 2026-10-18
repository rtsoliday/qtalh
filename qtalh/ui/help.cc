#include "help.h"
#include "about_banner.h"
#include <QDesktopServices>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <algorithm>

namespace alh {
namespace {
const QUrl manualUrl("https://ops.aps.anl.gov/manuals/QtALH/");

struct Topic {
  QString anchor, title, group, markdown;
};

QString readGuide(const QString& file) {
  QFile input(":/qtalh/help/" + file);
  if (!input.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(input.readAll());
}

QString slug(QString title) {
  title = title.toLower();
  title.remove(QRegularExpression("[^\\w\\s-]"));
  return title.replace(QRegularExpression("\\s+"), "-");
}

QVector<Topic> helpTopics() {
  QVector<Topic> guide;
  const auto text = readGuide("user-guide.md");
  const QRegularExpression heading("^#{2,3} (.+)$", QRegularExpression::MultilineOption);
  auto headings = heading.globalMatch(text);
  while (headings.hasNext()) {
    const auto match = headings.next();
    const auto next = heading.match(text, match.capturedEnd());
    auto body = text.mid(match.capturedEnd(),
                         (next.hasMatch() ? next.capturedStart() : text.size()) - match.capturedEnd());
    const auto title = match.captured(1).trimmed();
    guide.append({slug(title), title, {}, body.trimmed()});
  }
  QVector<Topic> topics;
  auto section = [&](const QString& anchor, const QString& group) {
    for (auto topic : guide) if (topic.anchor == anchor) {
      topic.group = group;
      topics.append(topic);
      return;
    }
  };
  auto page = [&](const QString& file, const QString& anchor, const QString& title,
                  const QString& group) {
    auto body = readGuide(file);
    // The surrounding guide supplies the title; demote the page's subheadings.
    body.remove(QRegularExpression("^# [^\\n]+\\n"));
    body.replace(QRegularExpression("^(#{2,5}) ", QRegularExpression::MultilineOption), "#\\1 ");
    topics.append({anchor, title, group, body.trimmed()});
  };
  section("first-run", "Overview");
  section("selection-and-alarm-silence", "Operating alarms");
  section("beep-severity", "Operating alarms");
  section("preview-guidance-and-commands", "Operating alarms");
  section("timed-shelving", "Operating alarms");
  // These focused pages expand the user guide's short operator-tools overview.
  const auto tools = readGuide("alarm-tools.md");
  auto toolHeadings = heading.globalMatch(tools);
  while (toolHeadings.hasNext()) {
    const auto match = toolHeadings.next();
    const auto next = heading.match(tools, match.capturedEnd());
    const auto title = match.captured(1).trimmed();
    topics.append({slug(title), title, "Alarm tools", tools.mid(match.capturedEnd(),
        (next.hasMatch() ? next.capturedStart() : tools.size()) - match.capturedEnd()).trimmed()});
  }
  section("logging-and-shared-operation", "Operating tools");
  page("logs.md", "log-browser", "Browse alarm and operation logs", "Operating tools");
  page("notifications.md", "notifications", "Personal notifications and escalation", "Operating tools");
  page("analytics.md", "analytics", "Session alarm analytics", "Operating tools");
  page("editor.md", "properties", "Properties and configuration editor", "Configuration");
  page("masks.md", "masks", "Masks and automatic forcing", "Configuration");
  page("phoebus.md", "phoebus", "Phoebus XML import and export", "Configuration");
  section("configuration-format", "Configuration");
  section("pvaccess-channels", "Configuration");
  section("command-line-reference", "Reference");
  section("choosing-an-appearance", "Reference");
  topics.append({"keyboard-shortcuts", "Keyboard shortcuts", "Reference",
      "Shortcuts apply to the active window. Use Command instead of Ctrl on macOS.\n\n"
      "| Shortcut | Action |\n| --- | --- |\n"
      "| F1 | Open Help; in a dialog with a Help button, open that dialog's topic |\n"
      "| Ctrl+O / Ctrl+X | Open a configuration / Exit |\n"
      "| Ctrl+N / Ctrl+S | New / Save (editor) |\n"
      "| Ctrl+F | Find Alarm; in Help, move to Find in help |\n"
      "| Enter / Shift+Enter | Next / previous match while typing in Find in help |\n"
      "| F3 / Shift+F3 | Next / previous Help match |\n"
      "| Esc | Close Help |\n"
      "| Ctrl+A | Acknowledge the selected alarm (runtime) |\n"
      "| Ctrl+G / Ctrl+P | Display guidance / start related process (runtime) |\n"
      "| Ctrl+V / Ctrl+M / Ctrl+S | Force Process Variable / Force Mask / Modify Mask Settings (runtime) |\n"
      "| Ctrl+B / Ctrl+N | Beep Severity / NoAck for One Hour (runtime) |\n"
      "| Ctrl+L / Ctrl+D | Active Alarms / Alarm Details (runtime) |\n"
      "| + / - | Expand one level / collapse the selected branch |\n"
      "| \\* / Ctrl+\\* | Expand the selected branch / expand the whole tree |\n"
      "| Alt+Backspace / Ctrl+Y | Undo / redo (editor) |\n"
      "| Shift+Delete / Ctrl+Insert / Shift+Insert | Cut / copy / paste (editor) |\n"
      "| Ctrl++ or Ctrl+= / Ctrl+- / Ctrl+0 | Increase / decrease / reset font size (except Motif mode) |\n\n"
      "Help search is case-insensitive, highlights every match, and wraps at both ends."});
  topics.append({"file-selection", "File selection", "Reference",
      "The modern and other Qt styles use Qt's file dialog: browse to a directory, "
      "then choose a file or type a filename, and pick a file type from the filter list.\n\n"
      "With `-style motif`, choose a directory and file, or enter a filename in "
      "**Selection**. Edit the **Filter** path and press Enter or click **Filter** to "
      "update the lists. Double-click a directory to enter it, or a file to choose it.\n\n"
      "In every style, **Cancel** keeps the current file, and saving over an existing "
      "file asks for confirmation."});
  section("troubleshooting", "Reference");
  return topics;
}

// The embedded guide uses the same authored Markdown as the website. Links to
// included pages stay in this window; other references open the online manual.
QString guideLink(const QString& href, const QVector<Topic>& topics) {
  QUrl url(href);
  if (!url.isRelative()) return href;
  const QHash<QString, QString> pages = {
      {"operate/alarm-tools", "active-alarms"}, {"operate/shelving", "timed-shelving"},
      {"operate/logs", "log-browser"}, {"operate/notifications", "notifications"},
      {"operate/analytics", "analytics"}, {"configure/editor", "properties"},
      {"configure/masks", "masks"}, {"configure/phoebus", "phoebus"},
      {"reference/configuration", "configuration-format"},
      {"reference/command-line", "command-line-reference"},
      {"configure/logging", "logging-and-shared-operation"},
      {"operate/troubleshooting", "troubleshooting"},
      {"get-started/first-run", "first-run"}, {"operate/appearance", "choosing-an-appearance"}};
  const auto fragment = url.fragment();
  auto hasTopic = [&](const QString& anchor) {
    return std::any_of(topics.cbegin(), topics.cend(), [&](const Topic& t) { return t.anchor == anchor; });
  };
  if (url.path().isEmpty() && hasTopic(fragment)) return "#" + fragment;
  auto path = url.path();
  path.remove(QRegularExpression("^/?(?:site/)?"));
  path.remove(QRegularExpression("\\.md$"));
  if (pages.contains(path)) return "#" + (hasTopic(fragment) ? fragment : pages.value(path));
  if (path == "qtalh-user-guide" && hasTopic(fragment)) return "#" + fragment;
  // Map repository-relative references to their published locations.
  if (path == "../README") {
    path = "get-started/install";
    if (fragment == "build-and-run") url.setFragment({});
  }
  else if (path == "../qtalh/tests/README") path = "develop/testing";
  else if (path == "../alh/documentation/ALH.html") path = "legacy/ALH.html";
  else if (path == "qtalh-appearance") path = "history/appearance";
  else if (path == "qtalh-compatibility") path = "understand/compatibility";
  else if (path == "qtalh-logging-analysis") path = "history/logging";
  else if (path == "qtalh-performance") path = "history/performance";
  else if (path.startsWith("../examples/")) path = "downloads/" + path.mid(3);
  // The manual is built without clean URLs, so its pages are published as .html files.
  if (!path.isEmpty() && !path.endsWith('/') && QFileInfo(path).suffix().isEmpty() &&
      !path.startsWith("downloads/") && !path.startsWith("legacy/"))
    path += ".html";
  url.setPath(path);
  return manualUrl.resolved(url).toString();
}

QString topicHtml(const Topic& topic, const QVector<Topic>& topics) {
  auto markdown = topic.markdown;
  // Screenshots, with any caption paragraph describing them, are available in the
  // online manual; the guide stays compact and self-contained. VitePress callouts
  // read as ordinary blockquotes here, with their titles in bold.
  markdown.remove(QRegularExpression(
      "!\\[[^\\]]*\\]\\([^\\n]+\\)(?:\\n\\n(?:This|The) screenshot[^\\n]*(?:\\n[^\\n]+)*)?"));
  markdown.replace(QRegularExpression("^::: ?(?:info|warning|tip) +(\\S.*)$",
                                      QRegularExpression::MultilineOption), "> **\\1**\n>");
  markdown.replace(QRegularExpression("^::: ?(?:info|warning|tip)?[ \\t]*$",
                                      QRegularExpression::MultilineOption), ">");
  QTextDocument document;
  document.setMarkdown(markdown);
  auto html = document.toHtml();
  const auto body = QRegularExpression("<body[^>]*>").match(html);
  html = html.mid(body.capturedEnd());
  html.truncate(html.lastIndexOf("</body>"));
  const QRegularExpression link("href=\"([^\"]+)\"");
  auto links = link.globalMatch(html);
  QVector<QRegularExpressionMatch> replacements;
  while (links.hasNext()) replacements.append(links.next());
  for (auto i = replacements.crbegin(); i != replacements.crend(); ++i)
    html.replace(i->capturedStart(1), i->capturedLength(1),
                 guideLink(QTextDocumentFragment::fromHtml(i->captured(1)).toPlainText(), topics).toHtmlEscaped());
  return "<h2><a name=\"" + topic.anchor + "\">" + topic.title.toHtmlEscaped() + "</a></h2>" + html;
}

class TopicDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    auto size = QStyledItemDelegate::sizeHint(option, index);
    // Derive row heights from the current font, including process-wide zoom.
    size.setHeight(qMax(size.height(), option.fontMetrics.height() +
        (index.data(Qt::UserRole).toString().isEmpty() ? 16 : 10)));
    return size;
  }
};

class HelpWindow : public QDialog {
public:
  explicit HelpWindow(QWidget* parent) : QDialog(parent), topics(helpTopics()) {
    setObjectName("helpWindow");
    setWindowTitle("QtALH Help");
    setModal(false);
    setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);
    auto row = new QHBoxLayout;
    find = new QLineEdit;
    find->setObjectName("helpFind");
    find->setPlaceholderText("Find in help");
    find->setAccessibleName("Find in help");
    find->setClearButtonEnabled(true);
    previous = new QToolButton;
    next = new QToolButton;
    previous->setObjectName("helpPrevious");
    next->setObjectName("helpNext");
    previous->setArrowType(Qt::UpArrow);
    next->setArrowType(Qt::DownArrow);
    previous->setToolTip("Previous match (Shift+Enter / Shift+F3)");
    next->setToolTip("Next match (Enter / F3)");
    previous->setAccessibleName("Previous match");
    next->setAccessibleName("Next match");
    previous->setAutoRaise(true);
    next->setAutoRaise(true);
    matchesLabel = new QLabel;
    matchesLabel->setObjectName("helpMatches");
    row->addWidget(find, 1);
    row->addWidget(previous);
    row->addWidget(next);
    row->addWidget(matchesLabel);
    layout->addLayout(row);

    list = new QListWidget;
    list->setObjectName("helpTopics");
    list->setAccessibleName("Help topics");
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setItemDelegate(new TopicDelegate(list));
    browser = new QTextBrowser;
    browser->setObjectName("helpBrowser");
    browser->setAccessibleName("Help text");
    browser->setOpenLinks(false);
    browser->document()->setDocumentMargin(14);
    const auto palette = this->palette();
    browser->document()->setDefaultStyleSheet(QString(
        "h2 { color: %1; margin-top: 22px; margin-bottom: 6px; } "
        "h3 { margin-top: 14px; margin-bottom: 4px; } "
        "a { color: %1; } th { background-color: %2; text-align: left; } "
        "td, th { padding: 5px; }")
        .arg(palette.color(QPalette::Link).name(), palette.color(QPalette::AlternateBase).name()));
    auto splitter = new QSplitter;
    splitter->setObjectName("helpSplitter");
    splitter->addWidget(list);
    splitter->addWidget(browser);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    QString html, group;
    int width = 0;
    for (const auto& topic : topics) {
      if (topic.group != group) {
        group = topic.group;
        auto heading = new QListWidgetItem(group.toUpper(), list);
        heading->setFlags(Qt::NoItemFlags);
        auto font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        heading->setForeground(secondaryText(palette));
      }
      auto item = new QListWidgetItem(topic.title, list);
      item->setData(Qt::UserRole, topic.anchor);
      rows.append(list->count() - 1);
      width = qMax(width, list->fontMetrics().horizontalAdvance(topic.title));
      html += topicHtml(topic, topics);
    }
    browser->setHtml(html);
    // Find the explicit topic anchors once; search and scroll use these positions.
    for (const auto& topic : topics) {
      int position = -1;
      for (auto block = browser->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it)
          if (it.fragment().charFormat().anchorNames().contains(topic.anchor)) position = block.position();
        if (position >= 0) break;
      }
      positions.append(position);
    }
    splitter->setSizes({qMin(width + 48, 330), 680});
    if (!legacyAppearance()) {
      const auto border = palette.color(QPalette::Mid).name();
      setStyleSheet(QString(
          "QListWidget#helpTopics { border: 1px solid %1; border-radius: 6px; padding: 4px; } "
          "QListWidget#helpTopics::item { padding: 0 8px; border-radius: 4px; } "
          "QListWidget#helpTopics::item:selected { background: %2; color: %3; } "
          "QTextBrowser#helpBrowser { border: 1px solid %1; border-radius: 6px; }")
          .arg(border, blend(palette.color(QPalette::Base), palette.color(QPalette::Highlight), 0.15).name(),
               palette.color(QPalette::Link).name()));
    }
    auto buttons = new QHBoxLayout;
    auto online = new QPushButton("Online manual");
    online->setAutoDefault(false);
    auto close = new QPushButton("Close");
    close->setObjectName("helpClose");
    close->setAutoDefault(false); // Enter in Find advances; it never dismisses Help.
    buttons->addWidget(online);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    // A styled widget can retain its resolved font when the application font
    // changes. Apply the new font explicitly throughout this uniform text UI.
    qApp->installEventFilter(this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(online, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(manualUrl); });
    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
      if (current) showTopic(current->data(Qt::UserRole).toString());
    });
    connect(browser->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { followScroll(); });
    connect(browser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
      if (url.isRelative() && url.path().isEmpty()) showTopic(url.fragment());
      else if (url.scheme() == "https" || url.scheme() == "http" || url.scheme() == "mailto")
        QDesktopServices::openUrl(url);
    });
    connect(find, &QLineEdit::textChanged, this, [this] { updateMatches(); });
    connect(find, &QLineEdit::returnPressed, this, [this] {
      findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier ? -1 : 1);
    });
    connect(previous, &QToolButton::clicked, this, [this] { findNext(-1); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(1); });
    auto shortcut = [this](QKeySequence key, std::function<void()> callback) {
      auto action = new QAction(this);
      action->setShortcut(key);
      addAction(action);
      connect(action, &QAction::triggered, this, std::move(callback));
    };
    shortcut(QKeySequence::Find, [this] { find->setFocus(); find->selectAll(); });
    shortcut(QKeySequence::FindNext, [this] { findNext(1); });
    shortcut(QKeySequence::FindPrevious, [this] { findNext(-1); });
    // F3 is useful on all platforms, including those with different standard keys.
    if (QKeySequence(QKeySequence::FindNext) != QKeySequence(Qt::Key_F3))
      shortcut(QKeySequence(Qt::Key_F3), [this] { findNext(1); });
    if (QKeySequence(QKeySequence::FindPrevious) != QKeySequence(Qt::SHIFT | Qt::Key_F3))
      shortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { findNext(-1); });
    updateMatches();
    resize(1000, 700);
    if (auto screen = this->screen()) resize(size().boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    showTopic("first-run");
  }

  void showTopic(const QString& anchor) {
    for (int i = 0; i < topics.size(); ++i) if (topics[i].anchor == anchor) {
      QScopedValueRollback<bool> guard(navigating, true);
      selectTopic(i);
      browser->scrollToAnchor(anchor);
      return;
    }
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == qApp && event->type() == QEvent::ApplicationFontChange) {
      const auto font = QApplication::font();
      setFont(font);
      for (auto widget : findChildren<QWidget*>()) widget->setFont(font);
    }
    return QDialog::eventFilter(watched, event);
  }

private:
  QVector<Topic> topics;
  QVector<int> rows, positions;
  QListWidget* list;
  QTextBrowser* browser;
  QLineEdit* find;
  QToolButton *previous, *next;
  QLabel* matchesLabel;
  QVector<QTextCursor> matches;
  int currentMatch = -1, currentStart = -1;
  bool navigating = false;

  void selectTopic(int index) {
    QSignalBlocker blocker(list);
    list->setCurrentRow(rows[index]);
    list->scrollToItem(list->currentItem(), QAbstractItemView::EnsureVisible);
  }

  int topicAt(int position) const {
    int current = 0;
    for (int i = 0; i < positions.size(); ++i)
      if (positions[i] >= 0 && positions[i] <= position) current = i;
    return current;
  }

  void followScroll() {
    if (navigating) return;
    int current = 0;
    const int top = browser->verticalScrollBar()->value() + 24;
    auto document = browser->document();
    for (int i = 0; i < positions.size(); ++i)
      if (positions[i] >= 0 && document->documentLayout()->blockBoundingRect(
              document->findBlock(positions[i])).top() <= top) current = i;
    selectTopic(current);
  }

  void updateMatches() {
    const int start = currentStart >= 0 ? currentStart : browser->cursorForPosition(QPoint(0, 0)).position();
    matches.clear();
    currentMatch = -1;
    if (!find->text().isEmpty()) {
      QTextCursor cursor(browser->document());
      while (!(cursor = browser->document()->find(find->text(), cursor)).isNull()) {
        if (currentMatch < 0 && cursor.selectionStart() >= start) currentMatch = matches.size();
        matches.append(cursor);
      }
      if (currentMatch < 0 && !matches.isEmpty()) currentMatch = 0;
    }
    showMatches();
  }

  void findNext(int direction) {
    if (matches.isEmpty()) return;
    currentMatch = (currentMatch + direction + matches.size()) % matches.size();
    showMatches();
  }

  void showMatches() {
    QList<QTextEdit::ExtraSelection> selections;
    const auto palette = browser->palette();
    for (int i = 0; i < matches.size(); ++i) {
      QTextEdit::ExtraSelection selection;
      selection.cursor = matches[i];
      selection.format.setBackground(i == currentMatch ? palette.highlight() :
          QBrush(blend(palette.color(QPalette::Base), palette.color(QPalette::Highlight), 0.25)));
      if (i == currentMatch) selection.format.setForeground(palette.highlightedText());
      selections.append(selection);
    }
    browser->setExtraSelections(selections);
    previous->setEnabled(!matches.isEmpty());
    next->setEnabled(!matches.isEmpty());
    matchesLabel->setText(find->text().isEmpty() ? QString() : matches.isEmpty() ? QString("No matches") :
        QString("%1 of %2").arg(currentMatch + 1).arg(matches.size()));
    currentStart = currentMatch >= 0 ? matches[currentMatch].selectionStart() : -1;
    if (currentMatch < 0) return;
    QScopedValueRollback<bool> guard(navigating, true);
    const auto rect = browser->cursorRect(matches[currentMatch]);
    const auto height = browser->viewport()->height();
    if (rect.top() < 0 || rect.bottom() > height) {
      auto bar = browser->verticalScrollBar();
      bar->setValue(bar->value() + rect.top() - height / 3);
    }
    selectTopic(topicAt(currentStart));
  }
};
} // namespace

QDialog* showHelp(QWidget* parent, const QString& topic) {
  QWidget* owner = parent;
  while (owner && owner->parentWidget()) {
    auto dialog = qobject_cast<QDialog*>(owner);
    if (dialog && dialog->isModal()) break;
    owner = owner->parentWidget();
  }
  auto help = owner ? static_cast<HelpWindow*>(owner->findChild<QDialog*>(
      "helpWindow", Qt::FindDirectChildrenOnly)) : nullptr;
  if (!help) help = new HelpWindow(owner);
  help->showNormal();
  if (!topic.isEmpty()) help->showTopic(topic);
  help->raise();
  help->activateWindow();
  return help;
}

void contextHelp(QWidget* widget, const QString& topic) {
  // Selection dialogs rebuild their contents; retain only one F1 action.
  auto action = widget->findChild<QAction*>("contextHelp", Qt::FindDirectChildrenOnly);
  if (!action) {
    action = new QAction(widget);
    action->setObjectName("contextHelp");
    auto keys = QKeySequence::keyBindings(QKeySequence::HelpContents);
    if (!keys.contains(QKeySequence(Qt::Key_F1))) keys.append(QKeySequence(Qt::Key_F1));
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widget->addAction(action);
    QObject::connect(action, &QAction::triggered, widget,
                     [widget, action] { showHelp(widget, action->data().toString()); });
  }
  action->setData(topic);
}

void showAbout(QWidget* parent) {
  QDialog dialog(parent);
  dialog.setObjectName("aboutDialog");
  dialog.setWindowTitle("About QtALH");
  dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(18, 18, 18, 14);
  layout->setSpacing(6);
  layout->addWidget(new AboutBanner(&dialog));
  auto title = new QLabel("QtALH");
  auto font = title->font();
  font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() * 1.7 : 18);
  font.setBold(true);
  setPresentationFont(title, font);
  layout->addWidget(title);
  auto message = new QLabel(
      "<p>Qt port of ALH 1.2.35<br>EPICS Alarm Handler and Alarm Configuration Tool<br>"
      "Developed at Argonne National Laboratory</p>"
      "<p><b>Qt port development and maintenance</b><br>Robert Soliday "
      "(<a href=\"mailto:soliday@anl.gov\">soliday@anl.gov</a>)</p>"
      "<p><b>Original ALH authors</b><br>Ben-Chin Cha, Janet Anderson, Mark Anderson, "
      "Marty Kraimer, and Albert Kagarmanov</p>"
      "<p><b>Additional ALH contributors</b><br>John Sinclair and Kay Kasemir (SNS), "
      "Andreas Luedeke (PSI)</p>");
  message->setWordWrap(true);
  message->setObjectName("aboutCredits");
  message->setOpenExternalLinks(true);
  message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
  layout->addWidget(message);
  auto footer = new QLabel("Qt " + QString(qVersion()) + " · Click Beeper for an alarm drill.");
  footer->setWordWrap(true);
  footer->setForegroundRole(QPalette::PlaceholderText);
  footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(footer);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
  buttons->button(QDialogButtonBox::Ok)->setDefault(true);
  auto help = buttons->addButton("Open help", QDialogButtonBox::HelpRole);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(help, &QPushButton::clicked, &dialog, [&] {
    dialog.accept();
    showHelp(parent, "first-run");
  });
  layout->addWidget(buttons);
  buttons->button(QDialogButtonBox::Ok)->setFocus();
  dialog.resize(480, dialog.sizeHint().height());
  dialog.exec();
}
} // namespace alh
