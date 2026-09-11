#include "dialogs.h"
#include <algorithm>
namespace alh {
namespace {
// QDialogButtonBox otherwise uses the desktop's platform-specific button order.
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
    auto rank = [this](QAbstractButton* button) {
      switch (standardButton(button)) {
      case Ok: return 0;
      case Apply: return 2;
      case Reset: return 3;
      case Cancel: return 4;
      case Close: return 5;
      case Help: return 6;
      default: return 1;
      }
    };
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](QAbstractButton* a, QAbstractButton* b) { return rank(a) < rank(b); });
    for (auto button : ordered) {
      button->setMinimumWidth(52);
      button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      row->addWidget(button, 1);
    }
  }
};
class MotifStyle : public QProxyStyle {
public:
  MotifStyle() : QProxyStyle(QStyleFactory::create("Windows")) {}
  int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                const QWidget* widget = nullptr, QStyleHintReturn* data = nullptr) const override {
    if (hint == SH_DialogButtonLayout) return QDialogButtonBox::WinLayout;
    return QProxyStyle::styleHint(hint, option, widget, data);
  }
  QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                         const QWidget* widget = nullptr) const override {
    auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
    if (widget && qobject_cast<const QDialog*>(widget->window())) {
      if (type == CT_LineEdit) size.setHeight(option->fontMetrics.height() + 2);
      if (type == CT_PushButton)
        size = QSize(qMax(52, contents.width() + 12), option->fontMetrics.height() + 6);
    }
    return size;
  }
  int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr) const override {
    switch (metric) {
    case PM_LayoutLeftMargin: case PM_LayoutRightMargin:
    case PM_LayoutTopMargin: case PM_LayoutBottomMargin:
    case PM_LayoutHorizontalSpacing: case PM_LayoutVerticalSpacing: return 3;
    case PM_ScrollBarExtent: return 17;
    case PM_IndicatorWidth: case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth: case PM_ExclusiveIndicatorHeight: return 10;
    case PM_ButtonMargin: return 4;
    default: return QProxyStyle::pixelMetric(metric, option, widget);
    }
  }
  void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override {
    if (element != PE_IndicatorRadioButton && element != PE_IndicatorCheckBox) {
      QProxyStyle::drawPrimitive(element, option, painter, widget);
      return;
    }
    painter->save();
    const auto r = option->rect.adjusted(1, 1, -1, -1);
    bool on = option->state & State_On;
    auto light = option->palette.light().color(), dark = option->palette.dark().color();
    painter->setBrush(on ? option->palette.mid() : option->palette.button());
    if (element == PE_IndicatorRadioButton) {
      QPolygon points;
      points << QPoint(r.center().x(), r.top()) << QPoint(r.left(), r.center().y())
             << QPoint(r.center().x(), r.bottom()) << QPoint(r.right(), r.center().y());
      painter->setPen(on ? dark : light);
      painter->drawPolygon(points);
      painter->setPen(on ? light : dark);
      painter->drawPolyline(points.constData() + 1, 3);
    } else {
      painter->fillRect(r, on ? option->palette.mid() : option->palette.button());
      painter->setPen(on ? dark : light);
      painter->drawLine(r.topLeft(), r.topRight());
      painter->drawLine(r.topLeft(), r.bottomLeft());
      painter->setPen(on ? light : dark);
      painter->drawLine(r.bottomLeft(), r.bottomRight());
      painter->drawLine(r.topRight(), r.bottomRight());
    }
    painter->restore();
  }
};
}
void initializeAppearance() {
  QApplication::setStyle(new MotifStyle);
  QFont font("monospace");
  font.setPixelSize(12);
  font.setStretch(85);
  QApplication::setFont(font);
  QPalette palette;
  palette.setColor(QPalette::Window, QColor("#b0c3ca"));
  palette.setColor(QPalette::Button, QColor("#b0c3ca"));
  palette.setColor(QPalette::Light, QColor("#dde6e9"));
  palette.setColor(QPalette::Dark, QColor("#5f696d"));
  palette.setColor(QPalette::Mid, QColor("#82979f"));
  palette.setColor(QPalette::Shadow, QColor("#5f696d"));
  palette.setColor(QPalette::WindowText, Qt::black);
  palette.setColor(QPalette::ButtonText, Qt::black);
  palette.setColor(QPalette::Text, Qt::black);
  palette.setColor(QPalette::Base, Qt::white);
  palette.setColor(QPalette::Highlight, QColor("#82979f"));
  palette.setColor(QPalette::HighlightedText, Qt::black);
  QApplication::setPalette(palette);
  // A stylesheet also overrides class fonts supplied by desktop themes.
  qApp->setStyleSheet(
      "QDialog, QDialog QWidget, QLabel {font-family:monospace; font-size:12px;}"
      "QLineEdit[readOnly=\"true\"], QPlainTextEdit[readOnly=\"true\"] {background:#b0c3ca;}"
      "QToolTip {background:#ffffd0; color:black; border:1px solid #5f696d;}");
}
QVBoxLayout* dialogColumn(QWidget* widget) {
  widget->setFont(QApplication::font());
  auto layout = new QVBoxLayout(widget);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(3);
  return layout;
}
QVBoxLayout* dialogFrame(QVBoxLayout* parent, const QString& title) {
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
  row->setSpacing(5);
  parent->addLayout(row);
  return row;
}
QDialogButtonBox* dialogActions(QVBoxLayout* parent, QDialog* dialog,
                                QDialogButtonBox::StandardButtons buttons, const QString& help) {
  auto separator = new QFrame;
  separator->setFrameShape(QFrame::HLine);
  separator->setFrameShadow(QFrame::Sunken);
  parent->addWidget(separator);
  if (!help.isEmpty())
    buttons |= QDialogButtonBox::Help;
  auto box = new MotifButtonBox(buttons);
  box->setCenterButtons(true);
  for (auto button : box->buttons()) {
    button->setMinimumWidth(52);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (box->standardButton(button) == QDialogButtonBox::Close)
      button->setText("Dismiss");
  }
  QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  QObject::connect(box, &QDialogButtonBox::helpRequested, dialog,
                   [dialog, help] { QMessageBox::information(dialog, "ALH InformationDialog", help); });
  parent->addWidget(box);
  return box;
}
void dialogHeading(QVBoxLayout* parent, const QString& kind, const QString& name) {
  auto row = dialogRow(parent);
  row->addWidget(new QLabel(kind));
  row->addWidget(new QLabel(name), 1);
}
QVector<QCheckBox*> maskChoices(QVBoxLayout* parent, const QString& mask, bool passive) {
  auto frame = dialogFrame(parent);
  QVector<QCheckBox*> boxes;
  const QStringList labels = {"Cancel Alarm", "Disable Alarm", "NoAck Alarm",
                               "NoAck Transient Alarm", "NoLog Alarm"};
  for (int i = 0; i < 5; ++i) {
    auto box = new QCheckBox(labels[i]);
    box->setObjectName("maskBit" + QString::number(i));
    box->setChecked(i < mask.size() && mask[i] != '-');
    box->setEnabled(!passive || i != 3);
    frame->addWidget(box);
    boxes << box;
  }
  return boxes;
}
QString selectedMask(const QVector<QCheckBox*>& boxes) {
  QString result = "-----";
  for (int i = 0; i < boxes.size(); ++i)
    if (boxes[i]->isChecked()) result[i] = QString("CDATL")[i];
  return result;
}
QString chooseFile(QWidget* parent, const QString& title, const QString& initial,
                   const QString& filters, bool save) {
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
                                "Choose a directory and file, or enter a filename. Use Filter to update the file list.");
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
