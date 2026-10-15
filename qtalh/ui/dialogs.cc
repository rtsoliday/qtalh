#include "dialogs.h"
#include "help.h"
#include <QPainterPath>
#include <algorithm>
namespace alh {
namespace {
// QDialogButtonBox otherwise uses the desktop's platform-specific button order.
// Both appearances keep ALH's order (Apply, Reset, Cancel, Dismiss/Close): the
// classic row spans the dialog; the modern row is right-aligned with Help apart.
class MotifButtonBox : public QDialogButtonBox {
public:
  explicit MotifButtonBox(StandardButtons buttons) : QDialogButtonBox(buttons) {}
protected:
  void showEvent(QShowEvent* event) override {
    QDialogButtonBox::showEvent(event);
    auto row = qobject_cast<QBoxLayout*>(layout());
    if (!row) return;
    while (auto item = row->takeAt(0)) delete item;
    auto ordered = buttons();
    if (!legacyAppearance()) {
      std::stable_sort(ordered.begin(), ordered.end(), [this](QAbstractButton* a, QAbstractButton* b) {
        return rank(a) < rank(b);
      });
      for (auto button : ordered)
        if (standardButton(button) == Help) row->addWidget(button);
      row->addStretch(1);
      for (auto button : ordered)
        if (standardButton(button) != Help) row->addWidget(button);
      return;
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [this](QAbstractButton* a, QAbstractButton* b) { return rank(a) < rank(b); });
    for (auto button : ordered) {
      button->setMinimumWidth(52);
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      row->addWidget(button, 1);
    }
  }
private:
  int rank(QAbstractButton* button) const {
    switch (standardButton(button)) {
    case Ok: return 0;
    case Apply: return 2;
    case Reset: return 3;
    case Cancel: return 4;
    case Close: return 5;
    case Help: return 6;
    default: return 1;
    }
  }
};
}
QVBoxLayout* dialogColumn(QWidget* widget) {
  if (legacyAppearance()) widget->setFont(QApplication::font());
  auto layout = new QVBoxLayout(widget);
  if (legacyAppearance()) {
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
  } else if (qobject_cast<QDialog*>(widget)) {
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);
  } else {
    // Content panes inside a dialog, tab or scroll area add no second margin.
    layout->setContentsMargins(qobject_cast<QScrollArea*>(widget->parentWidget()) ? 4 : 0, 4, 4, 4);
    layout->setSpacing(8);
  }
  return layout;
}
QLabel* sectionHeading(const QString& title) {
  auto heading = new QLabel(title);
  heading->setObjectName("sectionHeading");
  QFont weight;
  weight.setBold(true);
  heading->setFont(weight);
  return heading;
}
QVBoxLayout* dialogFrame(QVBoxLayout* parent, const QString& title) {
  if (!legacyAppearance()) {
    // Modern sections are a heading over indented content, without a box.
    auto content = new QVBoxLayout;
    content->setSpacing(6);
    if (!title.isEmpty()) {
      parent->addSpacing(2);
      parent->addWidget(sectionHeading(title));
      content->setContentsMargins(12, 0, 0, 0);
    }
    parent->addLayout(content);
    return content;
  }
  auto frame = new QFrame;
  frame->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  auto layout = dialogColumn(frame);
  if (!title.isEmpty())
    layout->addWidget(new QLabel(title));
  parent->addWidget(frame);
  return layout;
}
QHBoxLayout* dialogRow(QVBoxLayout* parent) {
  auto row = new QHBoxLayout;
  if (legacyAppearance()) row->setSpacing(5);
  parent->addLayout(row);
  return row;
}
QDialogButtonBox* dialogActions(QVBoxLayout* parent, QDialog* dialog,
                                QDialogButtonBox::StandardButtons buttons, const QString& helpTopic) {
  if (legacyAppearance()) {
    auto separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    parent->addWidget(separator);
  } else {
    parent->addSpacing(4);
  }
  if (!helpTopic.isEmpty())
    buttons |= QDialogButtonBox::Help;
  QDialogButtonBox* box = new MotifButtonBox(buttons);
  box->setCenterButtons(legacyAppearance());
  for (auto button : box->buttons()) {
    if (legacyAppearance()) {
      button->setMinimumWidth(52);
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    if (legacyAppearance() && box->standardButton(button) == QDialogButtonBox::Close)
      button->setText("Dismiss");
    if (!legacyAppearance() && box->standardButton(button) == QDialogButtonBox::Help)
      button->setToolTip("Open help for this dialog (F1)");
  }
  QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  if (!helpTopic.isEmpty()) {
    contextHelp(dialog, helpTopic);
    QObject::connect(box, &QDialogButtonBox::helpRequested, dialog,
                     [dialog, helpTopic] { showHelp(dialog, helpTopic); });
  }
  parent->addWidget(box);
  return box;
}
void dialogHeading(QVBoxLayout* parent, const QString& kind, const QString& name) {
  auto row = dialogRow(parent);
  auto caption = new QLabel(kind);
  auto label = new QLabel(name);
  if (!legacyAppearance()) {
    caption->setForegroundRole(QPalette::PlaceholderText);
    QFont weight;
    weight.setBold(true);
    label->setFont(weight);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->setSpacing(8);
  }
  row->addWidget(caption);
  row->addWidget(label, 1);
}
QVector<QCheckBox*> maskChoices(QVBoxLayout* parent, const QString& mask, bool passive, int columns) {
  auto frame = dialogFrame(parent);
  QVector<QCheckBox*> boxes;
  const QStringList labels = {"Cancel Alarm", "Disable Alarm", "NoAck Alarm",
                               "NoAck Transient Alarm", "NoLog Alarm"};
  QGridLayout* grid = nullptr;
  if (!legacyAppearance() && columns > 1) {
    grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    frame->addLayout(grid);
  }
  const int rows = grid ? (5 + columns - 1) / columns : 5;
  for (int i = 0; i < 5; ++i) {
    auto box = new QCheckBox(labels[i]);
    box->setObjectName("maskBit" + QString::number(i));
    box->setChecked(i < mask.size() && mask[i] != '-');
    box->setEnabled(!passive || i != 3);
    if (grid) grid->addWidget(box, i % rows, i / rows);
    else frame->addWidget(box);
    boxes << box;
  }
  return boxes;
}
namespace {
// One part of a segmented choice: outer corners rounded, the selection filled.
class SegmentButton : public QPushButton {
public:
  SegmentButton(const QString& text, int edges) : QPushButton(text), edges(edges) {
    setCheckable(true);
    setAttribute(Qt::WA_Hover);
  }
  QSize sizeHint() const override {
    auto size = QFontMetrics(font()).size(Qt::TextSingleLine, text());
    return {qMax(64, size.width() + 24), size.height() + 12};
  }
  QSize minimumSizeHint() const override { return sizeHint(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = palette();
    const auto button = colors.color(QPalette::Button), ink = colors.color(QPalette::ButtonText);
    const auto accent = colors.color(QPalette::Highlight);
    QColor fill = isChecked() ? accent : isDown() ? blend(button, ink, 0.14)
                : underMouse() ? blend(button, ink, 0.06) : button;
    if (!isEnabled()) fill = blend(colors.color(QPalette::Window), button, 0.5);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Round only the outside corners of the segmented group.
    const qreal left = edges & 1 ? 5 : 0, right = edges & 2 ? 5 : 0;
    QPainterPath path;
    path.moveTo(r.left() + left, r.top());
    path.lineTo(r.right() - right, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + right);
    path.lineTo(r.right(), r.bottom() - right);
    path.quadTo(r.right(), r.bottom(), r.right() - right, r.bottom());
    path.lineTo(r.left() + left, r.bottom());
    path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - left);
    path.lineTo(r.left(), r.top() + left);
    path.quadTo(r.left(), r.top(), r.left() + left, r.top());
    painter.fillPath(path, fill);
    // Inner segments share their neighbor's border instead of doubling it.
    if (!(edges & 1)) painter.setClipRect(rect().adjusted(1, 0, 0, 0));
    painter.strokePath(path, QPen(isChecked() ? accent.darker(115) : blend(button, ink, 0.3), 1));
    painter.setClipping(false);
    if (hasFocus()) {
      painter.setPen(QPen(isChecked() ? colors.color(QPalette::HighlightedText) : accent, 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(r.adjusted(2.5, 2.5, -2.5, -2.5), 3, 3);
    }
    painter.setPen(!isEnabled() ? colors.color(QPalette::Disabled, QPalette::ButtonText)
                   : isChecked() ? colors.color(QPalette::HighlightedText) : ink);
    painter.drawText(rect(), Qt::AlignCenter, text());
  }
private:
  int edges;
};
}
QPushButton* segmentButton(const QString& text, bool leftEdge, bool rightEdge) {
  return new SegmentButton(text, (leftEdge ? 1 : 0) | (rightEdge ? 2 : 0));
}
void findBar(QVBoxLayout* parent, QPlainTextEdit* text, int classicSpacing) {
  auto row = dialogRow(parent);
  if (legacyAppearance()) row->setSpacing(classicSpacing);
  auto search = new QLineEdit;
  search->setObjectName("findText");
  auto find = [text, search](bool reverse) {
    const auto flags = reverse ? QTextDocument::FindBackward : QTextDocument::FindFlags();
    if (text->find(search->text(), flags)) return true;
    // Wrap around once, as the legacy Forward/Reverse buttons do.
    text->moveCursor(reverse ? QTextCursor::End : QTextCursor::Start);
    return text->find(search->text(), flags);
  };
  if (legacyAppearance()) {
    row->addWidget(new QLabel("Search:"));
    row->addWidget(search, 1);
    for (bool reverse : {false, true}) {
      auto button = new QPushButton(reverse ? "Reverse" : "Forward");
      row->addWidget(button);
      QObject::connect(button, &QPushButton::clicked, text, [find, reverse] { find(reverse); });
    }
    return;
  }
  search->setPlaceholderText("Find");
  search->setClearButtonEnabled(true);
  search->setToolTip("Enter finds the next match; Shift+Enter the previous match.");
  row->addWidget(search, 1);
  auto status = new QLabel;
  status->setObjectName("findStatus");
  status->setForegroundRole(QPalette::PlaceholderText);
  for (bool reverse : {true, false}) {
    auto button = new QPushButton(reverse ? "Previous" : "Next");
    button->setObjectName(reverse ? "findPrevious" : "findNext");
    button->setAutoDefault(false);
    row->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, text, [=] {
      status->setText(search->text().isEmpty() || find(reverse) ? QString() : QString("No matches"));
    });
  }
  row->insertWidget(1, status);
  QObject::connect(search, &QLineEdit::textChanged, status, &QLabel::clear);
  QObject::connect(search, &QLineEdit::returnPressed, text, [=] {
    const bool reverse = QApplication::keyboardModifiers() & Qt::ShiftModifier;
    status->setText(search->text().isEmpty() || find(reverse) ? QString() : QString("No matches"));
  });
}
QString selectedMask(const QVector<QCheckBox*>& boxes) {
  QString result = "-----";
  for (int i = 0; i < boxes.size(); ++i)
    if (boxes[i]->isChecked()) result[i] = QString("CDATL")[i];
  return result;
}
QString chooseFile(QWidget* parent, const QString& title, const QString& initial,
                   const QString& filters, bool save) {
  if (!legacyAppearance()) {
    QFileDialog dialog(parent, title);
    dialog.setObjectName("fileSelectionDialog");
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    dialog.setNameFilter(filters);
    const QFileInfo info(initial);
    dialog.setDirectory(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
    if (!info.isDir()) dialog.selectFile(info.fileName());
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    return QFileInfo(dialog.selectedFiles().front()).absoluteFilePath();
  }
  QDialog dialog(parent);
  dialog.setObjectName("fileSelectionDialog");
  dialog.setWindowTitle(title);
  dialog.resize(360, 372);
  auto layout = dialogColumn(&dialog);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  auto info = QFileInfo(initial);
  QString directory = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
  auto match = QRegularExpression("\\(([^)]+)\\)").match(filters);
  QString pattern = match.hasMatch() ? match.captured(1) : "*";
  layout->addWidget(new QLabel("Filter"));
  auto filter = new QLineEdit(QDir(directory).filePath(pattern));
  filter->setObjectName("fileFilter");
  filter->setMinimumHeight(26);
  layout->addWidget(filter);
  auto lists = dialogRow(layout);
  auto dirsColumn = new QVBoxLayout, filesColumn = new QVBoxLayout;
  dirsColumn->addWidget(new QLabel("Directories"));
  filesColumn->addWidget(new QLabel("Files"));
  auto dirs = new QListWidget, files = new QListWidget;
  dirs->setObjectName("fileDirectories"); files->setObjectName("fileNames");
  for (auto list : {dirs, files}) {
    list->setStyleSheet("QListWidget {background:#b0c3ca;}");
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  }
  dirsColumn->addWidget(dirs); filesColumn->addWidget(files);
  lists->addLayout(dirsColumn, 1); lists->addLayout(filesColumn, 1);
  layout->addWidget(new QLabel("Selection"));
  auto selection = new QLineEdit(info.isDir() ? QDir(directory).filePath("") : info.absoluteFilePath());
  selection->setObjectName("fileSelection");
  selection->setMinimumHeight(26);
  layout->addWidget(selection);
  QString result;
  auto refresh = [&] {
    QFileInfo chosenFilter(filter->text());
    auto next = chosenFilter.absolutePath();
    if (!QDir(next).exists()) {
      QMessageBox::warning(&dialog, "ALH WarningDialog", "Directory does not exist: " + next);
      return;
    }
    directory = next; pattern = chosenFilter.fileName();
    dirs->clear(); files->clear();
    dirs->addItem("..");
    dirs->addItems(QDir(directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name));
    files->addItems(QDir(directory).entryList(pattern.split(' ', Qt::SkipEmptyParts),
                                             QDir::Files, QDir::Name));
  };
  auto accept = [&] {
    auto path = QFileInfo(QDir(directory).filePath(selection->text())).absoluteFilePath();
    if (QFileInfo(path).isDir()) {
      filter->setText(QDir(path).filePath(pattern)); refresh(); return;
    }
    if (selection->text().trimmed().isEmpty() || (!save && !QFileInfo(path).isFile())) {
      QMessageBox::warning(&dialog, "ALH WarningDialog", "Select an existing file."); return;
    }
    if (save && QFileInfo::exists(path) &&
        QMessageBox::question(&dialog, "Overwrite File", "Replace " + path + "?",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
      return;
    result = path; dialog.accept();
  };
  auto buttons = dialogActions(layout, &dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                "file-selection");
  auto filterButton = buttons->addButton("Filter", QDialogButtonBox::ActionRole);
  QObject::connect(filterButton, &QPushButton::clicked, &dialog, refresh);
  QObject::connect(filter, &QLineEdit::returnPressed, &dialog, refresh);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, accept);
  QObject::connect(selection, &QLineEdit::returnPressed, &dialog, accept);
  QObject::connect(dirs, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem* item) {
    directory = QDir::cleanPath(QDir(directory).filePath(item->text()));
    filter->setText(QDir(directory).filePath(pattern));
    selection->setText(QDir(directory).filePath("")); refresh();
  });
  QObject::connect(files, &QListWidget::currentTextChanged, &dialog,
                   [&](const QString& name) { if (!name.isEmpty()) selection->setText(QDir(directory).filePath(name)); });
  QObject::connect(files, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem*) { accept(); });
  refresh();
  dialog.exec();
  return result;
}
} // namespace alh
