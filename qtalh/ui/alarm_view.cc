// Motif-style presentation only; alarm state remains in the item model.
#include "alarm_view.h"
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QMouseEvent>
#include <QMimeData>
#include <QToolTip>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
namespace alh {
namespace {
const QColor base("#b0c3ca"), light("#dde6e9"), dark("#5f696d");
void bevel(QPainter* painter, const QRect& rect, const QColor& color, bool down = false,
           int width = 2) {
  painter->fillRect(rect, color);
  for (int i = 0; i < width; ++i) {
    auto r = rect.adjusted(i, i, -i, -i);
    painter->setPen(down ? dark : light);
    painter->drawLine(r.topLeft(), r.topRight());
    painter->drawLine(r.topLeft(), r.bottomLeft());
    painter->setPen(down ? light : dark);
    painter->drawLine(r.bottomLeft(), r.bottomRight());
    painter->drawLine(r.topRight(), r.bottomRight());
  }
}
int controlHeight(const QWidget* widget) {
  QStyleOptionButton option;
  option.initFrom(widget);
  return widget->style()->sizeFromContents(QStyle::CT_PushButton, &option,
      QSize(0, widget->fontMetrics().height()), widget).height();
}
int alarmRowHeight(const QWidget* widget) {
  return legacyAppearance() ? 24 : qMax(controlHeight(widget), widget->fontMetrics().height() + 8) + 6;
}
class RowHeight : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
    return {100, alarmRowHeight(option.widget)};
  }
};
QVariant field(const QModelIndex& row, int column, int role = Qt::DisplayRole) {
  return row.sibling(row.row(), column).data(role);
}
bool selectedRow(const QTreeView* view, const QModelIndex& row) {
  const auto current = view->currentIndex();
  // Names and action controls are painted in model columns hidden from QTreeView.
  // A name click can select only that cell, so isRowSelected() misses the active target.
  return current.isValid() && current.sibling(current.row(), 0) == row.sibling(row.row(), 0);
}
} // namespace
QSize MotifButton::sizeHint() const {
  if (!legacyAppearance()) return QPushButton::sizeHint();
  return QFontMetrics(font()).size(Qt::TextSingleLine, text()) + QSize(14, 10);
}
void MotifButton::paintEvent(QPaintEvent* event) {
  if (!legacyAppearance()) {
    QPushButton::paintEvent(event);
    if (background.isValid()) {
      QPainter painter(this);
      QStyleOptionButton option;
      initStyleOption(&option);
      auto area = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this).adjusted(2, 2, -2, -2);
      painter.fillRect(area, background);
      painter.setPen(contrastingText(background));
      painter.drawText(area, Qt::AlignCenter, text());
    }
    return;
  }
  QPainter painter(this);
  bevel(&painter, rect(), background, isDown());
  painter.setPen(Qt::black);
  painter.drawRect(rect().adjusted(2, 2, -2, -2));
  painter.setFont(font());
  painter.setPen(background == QColor("red") ? Qt::white : Qt::black);
  painter.drawText(rect().adjusted(4, 2, -4, -2).translated(0, 2), Qt::AlignCenter, text());
}
QSize MotifCheckBox::sizeHint() const {
  if (!legacyAppearance()) return QCheckBox::sizeHint();
  auto metrics = QFontMetrics(font());
  return {metrics.horizontalAdvance(text()) + 16, qMax(18, metrics.height())};
}
void MotifCheckBox::paintEvent(QPaintEvent* event) {
  if (!legacyAppearance()) {
    Q_UNUSED(event);
    QPainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);
    auto labelRect = style()->subElementRect(QStyle::SE_CheckBoxContents, &option, this);
    labelRect.setRight(rect().right());
    auto indicator = option;
    indicator.text.clear();
    indicator.state &= ~QStyle::State_HasFocus;
    style()->drawControl(QStyle::CE_CheckBox, &indicator, &painter, this);
    int flags = Qt::AlignRight | Qt::AlignVCenter | Qt::TextShowMnemonic;
    if (!style()->styleHint(QStyle::SH_UnderlineShortcut, &option, this))
      flags |= Qt::TextHideMnemonic;
    // Align the caption with the status labels, without the style's trailing
    // checkbox padding. Keep native indicator, hover, and focus rendering.
    style()->drawItemText(&painter, labelRect, flags, option.palette,
                          isEnabled(), text(), QPalette::WindowText);
    if (option.state & QStyle::State_HasFocus) {
      QStyleOptionFocusRect focus;
      focus.initFrom(this);
      focus.rect = style()->subElementRect(QStyle::SE_CheckBoxFocusRect, &option, this);
      focus.rect.setRight(rect().right());
      focus.backgroundColor = palette().color(QPalette::Window);
      style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
    return;
  }
  QPainter painter(this);
  QRect box(0, (height() - 9) / 2, 9, 9);
  bevel(&painter, box, base, isChecked(), 1);
  if (isChecked())
    painter.fillRect(box.adjusted(2, 2, -2, -2), dark);
  painter.setFont(font());
  painter.setPen(Qt::black);
  painter.drawText(rect().adjusted(16, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
}
AlarmView::AlarmView(bool isTree, QWidget* parent) : QTreeView(parent), tree(isTree) {
  connect(qApp, &QGuiApplication::fontDatabaseChanged, this, [this] { invalidateTextWidths(); });
  setHeaderHidden(true);
  setRootIsDecorated(false);
  setIndentation(0);
  setUniformRowHeights(true);
  setItemDelegate(new RowHeight(this));
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setExpandsOnDoubleClick(false);
  setDragEnabled(false); // Names use ALH's middle-button drag, independent of selection.
  arrowTimer.setSingleShot(true);
  connect(&arrowTimer, &QTimer::timeout, this, [this] {
    const auto index = pendingArrow;
    pendingArrow = QModelIndex();
    if (index.isValid()) emit clicked(index);
  });
  setFrameShape(QFrame::NoFrame);
  if (legacyAppearance()) {
    auto p = palette();
    p.setColor(QPalette::Base, base);
    setPalette(p);
  } else {
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
  }
  header()->setMinimumSectionSize(0);
  header()->setStretchLastSection(false);
  connect(this, &QTreeView::expanded, this, [this] { scheduleExtent(); });
  connect(this, &QTreeView::collapsed, this, [this] { scheduleExtent(); });
}
void AlarmView::setModel(QAbstractItemModel* next) {
  if (next == model())
    return;
  arrowTimer.stop();
  pendingArrow = mouseTarget = dragTarget = QModelIndex();
  hovered = pressed = QModelIndex();
  if (model())
    disconnect(model(), nullptr, this, nullptr);
  QTreeView::setModel(next);
  // Qt leaves old selection models owned by the view/header when replacing a
  // model. The header can also create a temporary one before sharing ours.
  for (QObject* owner : {static_cast<QObject*>(this), static_cast<QObject*>(header())})
    for (auto selection :
         owner->findChildren<QItemSelectionModel*>(QString(), Qt::FindDirectChildrenOnly))
      if (selection != selectionModel() && selection != header()->selectionModel())
        delete selection;
  if (!next)
    return;
  // Keep the nine model fields for actions and selection, but paint them
  // consecutively within each row, as ALH does, rather than in fixed columns.
  for (int column = 1; column < 9; ++column)
    hideColumn(column);
  connect(next, &QAbstractItemModel::modelReset, this, [this] { scheduleExtent(); });
  connect(next, &QAbstractItemModel::rowsInserted, this, [this] { scheduleExtent(); });
  connect(next, &QAbstractItemModel::rowsRemoved, this, [this] { scheduleExtent(); });
  connect(next, &QAbstractItemModel::dataChanged, this, [this] { scheduleExtent(); });
  updateExtent();
}
int AlarmView::textAdvance(const QString& text, const QFont& font) const {
  if (text.isEmpty()) return 0;
  const auto key = qMakePair(font, text);
  if (const auto width = textWidths.object(key)) return *width;
  const int width = QFontMetrics(font).horizontalAdvance(text);
  // Include the key's text storage as well as fixed per-entry overhead in the
  // budget. Very long labels are measured normally without retaining them.
  if (text.size() <= (textWidths.maxCost() - 128) / 2)
    textWidths.insert(key, new int(width), 128 + 2 * int(text.size()));
  return width;
}
void AlarmView::invalidateTextWidths() {
  textWidths.clear();
  doItemsLayout();
  scheduleExtent();
}
std::array<QRect, 9> AlarmView::cells(const QModelIndex& row, const QRect& bounds) const {
  std::array<QRect, 9> result;
  int depth = 0;
  if (tree)
    for (auto parent = row.parent(); parent.isValid(); parent = parent.parent())
      ++depth;
  if (!legacyAppearance()) {
    const int height = alarmRowHeight(this) - 6;
    int x = bounds.left() + 6 + (tree ? depth * (height / 2 + 4) : 0);
    const int y = bounds.top() + 3;
    auto add = [&](int column, int width) {
      result[column] = QRect(x, y, width, height);
      x += width + 4;
    };
    auto textWidth = [&](int column, bool button) {
      auto font = field(row, column, Qt::FontRole).value<QFont>();
      QFontMetrics metrics(font);
      QSize contents(textAdvance(field(row, column).toString(), font), metrics.height());
      QStyleOptionButton option;
      option.initFrom(this);
      option.fontMetrics = metrics;
      return button ? style()->sizeFromContents(QStyle::CT_PushButton, &option, contents, this).width()
                    : contents.width() + 6;
    };
    add(0, height); add(1, height);
    add(2, qMax(height, textWidth(2, true)));
    for (int column : {3, 4, 5})
      if (!field(row, column).toString().isEmpty()) add(column, qMax(height, textWidth(column, true)));
    add(6, textWidth(6, false)); add(7, 10); add(8, textWidth(8, false));
    return result;
  }
  int x = bounds.left() + (tree ? 20 + depth * 12 : 14);
  const int y = bounds.top() + 5;
  auto add = [&](int column, int width, int height = 18) {
    result[column] = QRect(x, y, width, height);
    x += width + 3;
  };
  add(0, 14);
  add(1, 14);
  auto nameFont = field(row, 2, Qt::FontRole).value<QFont>();
  add(2, textAdvance(field(row, 2).toString(), nameFont) + 10);
  for (int column : {3, 4, 5})
    if (!field(row, column).toString().isEmpty())
      add(column, 14);
  auto font = field(row, 6, Qt::FontRole).value<QFont>();
  add(6, textAdvance(field(row, 6).toString(), font) + 4);
  add(7, 10);
  add(8, textAdvance(field(row, 8).toString(), font) + 4);
  return result;
}
QRect AlarmView::visualRect(const QModelIndex& index) const {
  if (!index.isValid())
    return {};
  auto bounds = QTreeView::visualRect(index.sibling(index.row(), 0));
  if (bounds.isEmpty())
    return {};
  return cells(index, bounds)[index.column()];
}
QModelIndex AlarmView::indexAt(const QPoint& point) const {
  auto row = QTreeView::indexAt(point);
  if (!row.isValid())
    return {};
  auto rectangles = cells(row, QTreeView::visualRect(row.sibling(row.row(), 0)));
  for (int column = 0; column < 9; ++column)
    if (rectangles[column].contains(point))
      return row.sibling(row.row(), column);
  return row.sibling(row.row(), 8);
}
void AlarmView::drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                        const QModelIndex& row) const {
  if (!legacyAppearance()) { drawStyledRow(painter, option, row); return; }
  painter->save();
  painter->fillRect(option.rect, base);
  auto rectangles = cells(row, option.rect);
  if (tree && row.parent().isValid()) {
    auto current = row;
    int x = rectangles[0].left() - 11;
    painter->setPen(Qt::black);
    bool leaf = true;
    while (current.parent().isValid()) {
      bool last = current.row() + 1 == model()->rowCount(current.parent());
      if (leaf) {
        painter->drawLine(x, option.rect.top(), x, option.rect.top() + 12);
        painter->drawLine(x, option.rect.top() + 12, x + 6, option.rect.top() + 12);
      }
      if (!last)
        painter->drawLine(x, option.rect.top(), x, option.rect.bottom());
      current = current.parent();
      x -= 12;
      leaf = false;
    }
  }
  for (int column = 0; column < 9; ++column) {
    const auto rect = rectangles[column];
    if (rect.isEmpty())
      continue;
    const auto text = field(row, column).toString();
    QColor color = field(row, column, Qt::BackgroundRole).value<QColor>();
    if (!color.isValid())
      color = base;
    painter->setFont(field(row, column, Qt::FontRole).value<QFont>());
    painter->setPen(Qt::black);
    if (column == 3) {
      QPolygon arrow;
      arrow << QPoint(rect.left() + 2, rect.top() + 3) << QPoint(rect.left() + 2, rect.bottom() - 3)
            << QPoint(rect.right() - 1, rect.center().y());
      painter->setBrush(Qt::black);
      painter->drawPolygon(arrow);
      painter->setPen(light);
      painter->drawLine(arrow[0], arrow[1]);
    } else if (column == 0 || column == 2 || column == 4 || column == 5) {
      const bool selected = column == 2 && selectedRow(this, row);
      bevel(painter, rect, color, selected);
      if (selected) {
        painter->setPen(Qt::black);
        painter->drawRect(rect.adjusted(2, 2, -2, -2));
      }
      painter->setPen(color == QColor("red") ? Qt::white : Qt::black);
      const int textOffset = column == 2 ? 2 : 0;
      painter->drawText(rect.adjusted(2, 0, -2, 0).translated(0, textOffset), Qt::AlignCenter, text);
    } else if (column == 1) {
      if (!text.trimmed().isEmpty()) {
        painter->fillRect(rect.adjusted(0, 2, 0, -2), color);
        painter->setPen(color == QColor("red") ? Qt::white : Qt::black);
        painter->drawText(rect, Qt::AlignCenter, text);
      }
    } else {
      if (column == 6 && color != base) {
        painter->fillRect(rect, color);
        painter->setPen(color == QColor("blue") ? Qt::white : Qt::black);
      }
      painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
    }
  }
  painter->restore();
}
void AlarmView::mousePressEvent(QMouseEvent* event) {
  const auto index = indexAt(event->pos());
  if (event->button() == Qt::MiddleButton) {
    QToolTip::hideText();
    dragTarget = index.column() == 2 ? index : QModelIndex();
    dragOrigin = event->pos();
    if (dragTarget.isValid()) {
      // X11 terminals paste PRIMARY with the middle button; Ctrl+V uses CLIPBOARD.
      auto data = model()->mimeData({dragTarget});
      auto clipboard = QApplication::clipboard();
      clipboard->setText(data->text(), QClipboard::Clipboard);
      if (clipboard->supportsSelection())
        clipboard->setText(data->text(), QClipboard::Selection);
      delete data;
    }
  } else if (event->button() == Qt::LeftButton) {
    mouseTarget = index;
  }
  event->accept();
}
void AlarmView::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::MiddleButton) dragTarget = QModelIndex();
  if (event->button() == Qt::LeftButton) {
    const auto index = mouseTarget;
    mouseTarget = QModelIndex();
    if (index.isValid() && index == indexAt(event->pos())) {
      // Commit selection only on release over the pressed cell, as in ALH.
      // Row controls act on their own target without moving the selected name.
      if (index.column() == 2 || index.column() == 6 || index.column() == 7) {
        setFocus(Qt::MouseFocusReason);
        setCurrentIndex(index);
      }
      if (index.column() == 3) {
        // Defer the toggle so a double-click only expands the branch.
        if (pendingArrow.isValid() && pendingArrow != index) {
          const auto previous = pendingArrow;
          pendingArrow = QModelIndex();
          emit clicked(previous);
        }
        pendingArrow = index;
        arrowTimer.start(QApplication::doubleClickInterval());
      } else {
        emit clicked(index);
      }
    }
  }
  event->accept();
}
void AlarmView::mouseDoubleClickEvent(QMouseEvent* event) {
  mouseTarget = QModelIndex(); // The following release must not repeat a single-click action.
  if (event->button() == Qt::LeftButton) {
    const auto index = indexAt(event->pos());
    if (index.column() == 3 && pendingArrow == index) {
      arrowTimer.stop();
      pendingArrow = QModelIndex();
    }
    if (index.isValid() && (index.column() == 2 || index.column() == 3)) {
      if (index.column() == 2) setCurrentIndex(index);
      emit doubleClicked(index);
    }
  }
  event->accept();
}
void AlarmView::mouseMoveEvent(QMouseEvent* event) {
  if ((event->buttons() & Qt::MiddleButton) && dragTarget.isValid() &&
      (event->pos() - dragOrigin).manhattanLength() >= QApplication::startDragDistance()) {
    const auto index = dragTarget;
    dragTarget = QModelIndex();
    startNameDrag(index);
  }
  event->accept();
}
void AlarmView::startNameDrag(const QModelIndex& index) {
  auto drag = new QDrag(this);
  drag->setMimeData(model()->mimeData({index}));
  // A drag pixmap follows the pointer across application boundaries and vanishes
  // on drop/cancel. An ordinary tooltip would stay at its initial screen position.
  const auto text = drag->mimeData()->text();
  const auto hintFont = QToolTip::font();
  const auto size = QFontMetrics(hintFont).size(Qt::TextSingleLine, text) + QSize(12, 8);
  QPixmap preview(size * devicePixelRatioF());
  preview.setDevicePixelRatio(devicePixelRatioF());
  const auto colors = QToolTip::palette();
  preview.fill(colors.color(QPalette::ToolTipBase));
  {
    QPainter painter(&preview);
    painter.setFont(hintFont);
    painter.setPen(colors.color(QPalette::ToolTipText));
    painter.drawRect(QRect(QPoint(), size).adjusted(0, 0, -1, -1));
    painter.drawText(QRect(QPoint(), size).adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignVCenter, text);
  }
  drag->setPixmap(preview);
  drag->setHotSpot(QPoint(-12, -18));
  QToolTip::hideText();
  executeNameDrag(drag);
  drag->deleteLater();
}
void AlarmView::executeNameDrag(QDrag* drag) {
  drag->exec(Qt::CopyAction);
}
bool AlarmView::event(QEvent* event) {
  bool result = QTreeView::event(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange || event->type() == QEvent::ScreenChangeInternal)
    invalidateTextWidths();
  return result;
}
bool AlarmView::viewportEvent(QEvent* event) {
  if (!legacyAppearance() && event->type() == QEvent::Resize) scheduleExtent();
  if (!legacyAppearance() && (event->type() == QEvent::MouseMove ||
      event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
      event->type() == QEvent::Leave)) {
    auto old = hovered;
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease) {
      hovered = indexAt(static_cast<QMouseEvent*>(event)->pos());
      if (event->type() == QEvent::MouseButtonPress) pressed = hovered;
      if (event->type() == QEvent::MouseButtonRelease) pressed = QModelIndex();
    } else if (event->type() == QEvent::Leave) {
      hovered = QModelIndex(); pressed = QModelIndex();
    }
    if (old.isValid() && event->type() != QEvent::Paint) viewport()->update(QTreeView::visualRect(old.sibling(old.row(), 0)));
    if (hovered.isValid() && event->type() != QEvent::Paint)
      viewport()->update(QTreeView::visualRect(hovered.sibling(hovered.row(), 0)));
  }
  return QTreeView::viewportEvent(event);
}
void AlarmView::drawStyledRow(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& row) const {
  painter->save();
  auto panel = option;
  panel.state.setFlag(QStyle::State_Selected, selectedRow(this, row));
  painter->fillRect(option.rect, palette().base());
  style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &panel, painter, this);
  const auto rectangles = cells(row, option.rect);
  for (int column = 0; column < 9; ++column) {
    auto rect = rectangles[column];
    if (rect.isEmpty()) continue;
    auto index = row.sibling(row.row(), column);
    auto text = field(row, column).toString();
    auto color = field(row, column, Qt::BackgroundRole).value<QColor>();
    painter->setFont(field(row, column, Qt::FontRole).value<QFont>());
    auto foreground = panel.state & QStyle::State_Selected ? palette().highlightedText() : palette().text();
    if (column == 3) {
      QStyleOption branch;
      branch.initFrom(this); branch.rect = rect;
      branch.state |= QStyle::State_Children;
      branch.state.setFlag(QStyle::State_Open, tree && isExpanded(row));
      if (hovered == index) branch.state |= QStyle::State_MouseOver;
      style()->drawPrimitive(QStyle::PE_IndicatorBranch, &branch, painter, this);
    } else if (column == 0 || column == 2 || column == 4 || column == 5) {
      QStyleOptionButton button;
      button.initFrom(this); button.rect = rect; button.text = text;
      button.fontMetrics = QFontMetrics(painter->font());
      button.state.setFlag(QStyle::State_MouseOver, hovered == index);
      button.state.setFlag(QStyle::State_Sunken,
                           pressed == index || (column == 2 && selectedRow(this, row)));
      button.state.setFlag(QStyle::State_HasFocus, hasFocus() && currentIndex() == index);
      style()->drawControl(QStyle::CE_PushButton, &button, painter, this);
      if (column == 0 && color.isValid()) {
        auto badge = rect.adjusted(4, 4, -4, -4);
        painter->fillRect(badge, color); painter->setPen(contrastingText(color));
        painter->drawRect(badge.adjusted(0, 0, -1, -1));
        painter->drawText(badge, Qt::AlignCenter, text);
      }
    } else {
      painter->setPen(foreground.color());
      if (color.isValid() && !text.trimmed().isEmpty()) {
        painter->fillRect(rect, color); painter->setPen(contrastingText(color));
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
      }
      painter->drawText(rect.adjusted(2, 0, -2, 0),
          (column == 1 ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter), text);
    }
  }
  if (hasFocus() && currentIndex().row() == row.row() && currentIndex().parent() == row.parent()) {
    QStyleOptionFocusRect focus;
    focus.initFrom(this); focus.rect = option.rect.adjusted(1, 1, -1, -1);
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, painter, this);
  }
  painter->restore();
}
void AlarmView::scheduleExtent() {
  if (extentPending)
    return;
  extentPending = true;
  QTimer::singleShot(0, this, [this] {
    extentPending = false;
    updateExtent();
  });
}
void AlarmView::updateExtent() {
  if (!model())
    return;
  int width = 0;
  std::function<void(QModelIndex)> visit = [&](QModelIndex parent) {
    for (int row = 0; row < model()->rowCount(parent); ++row) {
      auto index = model()->index(row, 0, parent);
      width = qMax(width, cells(index, QRect(0, 0, 1, alarmRowHeight(this)))[8].right() + 10);
      if (tree && isExpanded(index))
        visit(index);
    }
  };
  visit({});
  setColumnWidth(0, qMax(width, viewport()->width()));
  viewport()->update();
}
} // namespace alh
