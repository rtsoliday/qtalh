// Motif-style presentation only; alarm state remains in the item model.
#include "alarm_view.h"
#include <QAccessible>
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
const QColor base("#b0c3ca"), light("#dde6e9"), dark("#5f696d"), red("red"), blue("blue");
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
// Modern rows: square chips three pixels inside the row, sized from the font.
int alarmRowHeight(const QWidget* widget) {
  return legacyAppearance() ? 24 : qMax(24, widget->fontMetrics().height() + 11);
}
int chipSize(const QWidget* widget) { return alarmRowHeight(widget) - 6; }
void roundedChip(QPainter* painter, const QRect& rect, const QColor& fill, const QColor& outline,
                 qreal radius = 4) {
  painter->setPen(outline.isValid() ? QPen(outline, 1) : QPen(Qt::NoPen));
  painter->setBrush(fill.isValid() ? QBrush(fill) : QBrush(Qt::NoBrush));
  painter->drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}
// A filled disclosure triangle, as ALH's arrow, pointing down when open.
void disclosure(QPainter* painter, const QRect& rect, bool open, const QColor& color) {
  const QPointF c = QRectF(rect).center();
  const qreal s = qMax(3.0, rect.height() * 0.21);
  QPolygonF triangle;
  if (open)
    triangle << QPointF(c.x() - s, c.y() - s * 0.55) << QPointF(c.x() + s, c.y() - s * 0.55)
             << QPointF(c.x(), c.y() + s * 0.65);
  else
    triangle << QPointF(c.x() - s * 0.55, c.y() - s) << QPointF(c.x() - s * 0.55, c.y() + s)
             << QPointF(c.x() + s * 0.65, c.y());
  painter->setPen(Qt::NoPen);
  painter->setBrush(color);
  painter->drawPolygon(triangle);
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
  if (!legacyAppearance()) {
    auto bold = font();
    bold.setBold(true);
    return QFontMetrics(bold).size(0, text()) + QSize(28, 14);
  }
  // Multi-line text sizes for summary tiles; single lines are unchanged.
  return QFontMetrics(font()).size(0, text()) + QSize(14, 10);
}
void MotifButton::paintEvent(QPaintEvent* event) {
  if (!legacyAppearance()) {
    Q_UNUSED(event);
    // A flat severity tile: the whole button carries the alarm color.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& colors = palette();
    QColor fill = background.isValid() ? background : colors.color(QPalette::Button);
    if (isDown()) fill = fill.darker(112);
    else if (underMouse()) fill = background.isValid() ? fill.lighter(106) : blend(fill, colors.color(QPalette::Highlight), 0.08);
    const QColor outline = background.isValid() ? background.darker(125)
                                                : blend(colors.color(QPalette::Button), colors.color(QPalette::ButtonText), 0.28);
    roundedChip(&painter, rect().adjusted(1, 1, -1, -1), fill, outline, 6);
    if (hasFocus())
      roundedChip(&painter, rect().adjusted(2, 2, -2, -2), QColor(), colors.color(QPalette::Highlight), 5);
    auto bold = font();
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(background.isValid() ? contrastingText(background) : colors.color(QPalette::ButtonText));
    painter.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignCenter, text());
    return;
  }
  QPainter painter(this);
  bevel(&painter, rect(), background, isDown());
  painter.setPen(Qt::black);
  painter.drawRect(rect().adjusted(2, 2, -2, -2));
  painter.setFont(font());
  painter.setPen(background == red ? Qt::white : Qt::black);
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
  painter.setPen(palette().color(QPalette::WindowText)); // White on a silenced footer.
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
  setSilenced(false);
  if (!legacyAppearance()) {
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover);
  }
  header()->setMinimumSectionSize(0);
  header()->setStretchLastSection(false);
  connect(this, &QTreeView::expanded, this, [this] { scheduleExtent(); });
  connect(this, &QTreeView::collapsed, this, [this] { scheduleExtent(); });
}
void AlarmView::setSilenced(bool silenced) {
  // Roles left unset follow the application palette. Alarm, acknowledgement
  // and name controls keep their own colors, as ALH's changeTreeColor() skips them.
  QPalette colors;
  if (legacyAppearance())
    colors.setColor(QPalette::Base, base);
  if (silenced) {
    colors.setColor(QPalette::Base, blue);
    colors.setColor(QPalette::Text, Qt::white);
    colors.setColor(QPalette::Highlight, Qt::white);
  }
  // The classic appearance's application style sheet stops palette
  // inheritance, so the viewport (the pane background) is set directly.
  setPalette(colors);
  viewport()->setPalette(colors);
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
  connect(next, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex& topLeft, const QModelIndex& bottomRight) {
            const auto parent = topLeft.parent();
            if (!extentFull && rowShown(topLeft))
              for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
                extentRows.push_back(model()->index(row, 0, parent));
            scheduleExtent(false, false);
          });
  extentFull = extentRepaint = true;
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
  staticTexts.clear();
  chipImages.clear();
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
    const int chip = chipSize(this);
    int x = bounds.left() + 8 + (tree ? depth * (chip / 2 + 10) : 0);
    const int y = bounds.top() + 3;
    auto add = [&](int column, int width) {
      result[column] = QRect(x, y, width, chip);
      x += width + 4;
    };
    auto advance = [&](int column) {
      return textAdvance(field(row, column).toString(), field(row, column, Qt::FontRole).value<QFont>());
    };
    add(0, chip); add(1, chip);
    add(2, qMax(chip, advance(2) + 12));
    for (int column : {3, 4, 5})
      if (!field(row, column).toString().isEmpty()) add(column, chip);
    add(6, advance(6) + 10); add(7, qMax(10, advance(7) + 4)); add(8, advance(8) + 8);
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
void AlarmView::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight,
                            const QVector<int>& roles) {
  // QAbstractItemView repaints the whole viewport for any multi-cell range.
  // Each row here is painted as one item with uniform height and no editors,
  // so repainting the reported rows is sufficient.
  if (!topLeft.isValid() || !bottomRight.isValid() || topLeft.parent() != bottomRight.parent() ||
      !uniformRowHeights()) {
    QTreeView::dataChanged(topLeft, bottomRight, roles);
    return;
  }
  if (isVisible()) {
    const auto first = QTreeView::visualRect(topLeft.sibling(topLeft.row(), 0));
    const auto last = QTreeView::visualRect(bottomRight.sibling(bottomRight.row(), 0));
    const auto rows = first.united(last);
    const auto visible = viewport()->rect();
    if (rows.isEmpty() || !rows.intersects(visible)) {
      // Nothing on screen changed.
    } else if (topLeft.column() == 0) {
      viewport()->update(rows);
    } else {
      // Cells are laid out consecutively: repaint from the first changed cell
      // to the end of each visible row, usually just the alarm counts.
      QRegion region;
      const int height = first.height();
      int row = topLeft.row();
      if (!tree && first.top() < 0) row += -first.top() / height;
      for (; row <= bottomRight.row(); ++row) {
        const auto index = topLeft.sibling(row, topLeft.column());
        const auto rowRect = tree ? QTreeView::visualRect(index.sibling(row, 0))
                                  : first.translated(0, (row - topLeft.row()) * height);
        if (rowRect.isEmpty() || rowRect.bottom() < visible.top()) continue;
        if (rowRect.top() > visible.bottom()) {
          if (!tree) break;
          continue;
        }
        const int left = cells(index, rowRect)[topLeft.column()].left();
        region += QRect(left, rowRect.top(), rowRect.right() - left + 1, rowRect.height());
      }
      viewport()->update(region);
    }
  }
#if QT_CONFIG(accessibility)
  if (QAccessible::isActive()) {
    QAccessibleTableModelChangeEvent event(this, QAccessibleTableModelChangeEvent::DataChanged);
    event.setFirstRow(topLeft.row());
    event.setFirstColumn(topLeft.column());
    event.setLastRow(bottomRight.row());
    event.setLastColumn(bottomRight.column());
    QAccessible::updateAccessibility(&event);
  }
#endif
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
  // The pane is ALH gray, or blue with white text during a silence interval.
  const QColor pane = palette().color(QPalette::Base), ink = palette().color(QPalette::Text);
  painter->fillRect(option.rect, pane);
  auto rectangles = cells(row, option.rect);
  if (tree && row.parent().isValid()) {
    auto current = row;
    int x = rectangles[0].left() - 11;
    painter->setPen(ink);
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
    // Partial updates repaint only the changed cells; skip the others.
    if (rect.isEmpty() || !paintRegion.intersects(rect))
      continue;
    const auto text = field(row, column).toString();
    QColor color = field(row, column, Qt::BackgroundRole).value<QColor>();
    if (!color.isValid())
      color = pane;
    painter->setFont(field(row, column, Qt::FontRole).value<QFont>());
    painter->setPen(Qt::black);
    if (column == 3) {
      QPolygon arrow;
      arrow << QPoint(rect.left() + 2, rect.top() + 3) << QPoint(rect.left() + 2, rect.bottom() - 3)
            << QPoint(rect.right() - 1, rect.center().y());
      painter->setBrush(ink);
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
      painter->setPen(color == red ? Qt::white : Qt::black);
      const int textOffset = column == 2 ? 2 : 0;
      painter->drawText(rect.adjusted(2, 0, -2, 0).translated(0, textOffset), Qt::AlignCenter, text);
    } else if (column == 1) {
      // ALH's severity box keeps its color, so it shows on a silenced pane.
      painter->fillRect(rect.adjusted(0, 2, 0, -2), color);
      if (!text.trimmed().isEmpty()) {
        painter->setPen(color == red ? Qt::white : Qt::black);
        painter->drawText(rect, Qt::AlignCenter, text);
      }
    } else {
      painter->setPen(ink);
      if (column == 6 && color != pane) {
        painter->fillRect(rect, color);
        painter->setPen(color == blue ? Qt::white : Qt::black);
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
    auto index = mouseTarget;
    mouseTarget = QModelIndex();
    if (index.isValid() && index == indexAt(event->pos())) {
      // Modern rows highlight as a whole, so their passive cells select like the name.
      if (!legacyAppearance() && (index.column() == 1 || index.column() == 8))
        index = index.sibling(index.row(), 2);
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
    auto index = indexAt(event->pos());
    if (!legacyAppearance() && (index.column() == 1 || index.column() == 8))
      index = index.sibling(index.row(), 2);
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
  if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
    updateRow(currentIndex());
  return result;
}
void AlarmView::paintEvent(QPaintEvent* event) {
  paintRegion = event->region();
  QTreeView::paintEvent(event);
}
void AlarmView::updateRow(const QModelIndex& index) {
  if (index.isValid()) viewport()->update(QTreeView::visualRect(index.sibling(index.row(), 0)));
}
void AlarmView::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
  QTreeView::currentChanged(current, previous);
  // Styled rows draw selection and focus across the row, but Qt repaints only
  // the current cell. Repaint both rows so neither keeps a stale highlight.
  updateRow(current);
  updateRow(previous);
}
bool AlarmView::viewportEvent(QEvent* event) {
  // Row extents do not depend on the viewport, which only sets the minimum width.
  if (!legacyAppearance() && event->type() == QEvent::Resize) scheduleExtent(false);
  if (!legacyAppearance() && (event->type() == QEvent::MouseMove ||
      event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
      event->type() == QEvent::Leave)) {
    const QModelIndex old = hovered, oldPressed = pressed;
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease) {
      hovered = indexAt(static_cast<QMouseEvent*>(event)->pos());
      if (event->type() == QEvent::MouseButtonPress) pressed = hovered;
      if (event->type() == QEvent::MouseButtonRelease) pressed = QModelIndex();
    } else if (event->type() == QEvent::Leave) {
      hovered = QModelIndex(); pressed = QModelIndex();
    }
    // Mouse motion within one cell changes nothing that is painted.
    if (old != hovered || oldPressed != pressed) {
      updateRow(old);
      updateRow(hovered);
      updateRow(oldPressed);
      const int column = hovered.isValid() ? hovered.column() : -1;
      const bool control = column == 0 || column == 3 || column == 4 || column == 5;
      if (control != (viewport()->cursor().shape() == Qt::PointingHandCursor)) {
        if (control) viewport()->setCursor(Qt::PointingHandCursor);
        else viewport()->unsetCursor();
      }
    }
  }
  return QTreeView::viewportEvent(event);
}
void AlarmView::drawLabel(QPainter* painter, const QRect& rect, const QString& text,
                          const QFont& font, bool centered) const {
  if (text.isEmpty()) return;
  const auto key = qMakePair(font, text);
  auto prepared = staticTexts.object(key);
  if (!prepared) {
    prepared = new QStaticText(text);
    prepared->setTextFormat(Qt::PlainText);
    prepared->prepare(QTransform(), font);
    staticTexts.insert(key, prepared);
  }
  const QSizeF size = prepared->size();
  painter->setFont(font);
  painter->drawStaticText(QPointF(centered ? rect.left() + (rect.width() - size.width()) / 2 : rect.left(),
                                  rect.top() + qRound((rect.height() - size.height()) / 2)), *prepared);
}
void AlarmView::drawChip(QPainter* painter, const QRect& rect, const QColor& fill,
                         const QColor& outline) const {
  if (!fill.isValid() && !outline.isValid()) return;
  const qreal ratio = painter->device()->devicePixelRatioF();
  const auto key = qMakePair((quint64(fill.isValid() ? fill.rgba() : 0) << 32) |
                                 (outline.isValid() ? outline.rgba() : 0),
                             (quint64(rect.width()) << 32) | (quint64(rect.height()) << 16) |
                                 quint64(qRound(ratio * 64)));
  auto found = chipImages.constFind(key);
  if (found == chipImages.cend()) {
    // Only a few sizes and colors occur; bound the cache for palette or zoom changes.
    if (chipImages.size() > 256) chipImages.clear();
    QPixmap image(rect.size() * ratio);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter chipPainter(&image);
    chipPainter.setRenderHint(QPainter::Antialiasing);
    roundedChip(&chipPainter, QRect(QPoint(), rect.size()), fill, outline);
    chipPainter.end();
    found = chipImages.insert(key, image);
  }
  painter->drawPixmap(rect.topLeft(), *found);
}
void AlarmView::drawStyledRow(QPainter* painter, const QStyleOptionViewItem& option,
                             const QModelIndex& row) const {
  painter->save();
  const auto& colors = palette();
  const QColor base = colors.color(QPalette::Base), text = colors.color(QPalette::Text);
  const QColor accent = colors.color(QPalette::Highlight), muted = secondaryText(colors);
  const QColor outline = blend(base, text, 0.24), hoverFill = blend(base, text, 0.08),
               pressFill = blend(base, text, 0.16);
  const bool selected = selectedRow(this, row);
  const bool rowHovered = hovered.isValid() && hovered.parent() == row.parent() && hovered.row() == row.row();
  // Selection is a tint plus an accent edge, leaving alarm colors untouched.
  painter->fillRect(option.rect, selected ? blend(base, accent, hasFocus() ? 0.2 : 0.13)
                                 : rowHovered ? blend(base, text, 0.04) : base);
  if (selected)
    painter->fillRect(QRect(option.rect.left(), option.rect.top() + 2, 3, option.rect.height() - 4), accent);
  const auto rectangles = cells(row, option.rect);
  const int chip = chipSize(this);
  if (tree && row.parent().isValid()) {
    // Subtle connectors under the parent's acknowledgement chip, as in ALH's branch lines.
    painter->setPen(QPen(blend(base, text, 0.2), 1));
    const int middle = option.rect.top() + option.rect.height() / 2;
    int x = rectangles[0].left() - 10;
    bool leaf = true;
    for (auto current = row; current.parent().isValid(); current = current.parent(), x -= chip / 2 + 10) {
      const bool last = current.row() + 1 == model()->rowCount(current.parent());
      if (leaf) {
        painter->drawLine(x, option.rect.top(), x, middle);
        painter->drawLine(x, middle, rectangles[0].left() - 3, middle);
      }
      if (!last) painter->drawLine(x, option.rect.top(), x, option.rect.bottom());
      leaf = false;
    }
  }
  painter->setRenderHint(QPainter::Antialiasing);
  for (int column = 0; column < 9; ++column) {
    const auto rect = rectangles[column];
    // Partial updates repaint only the changed cells; skip the others.
    if (rect.isEmpty() || !paintRegion.intersects(rect)) continue;
    const auto index = row.sibling(row.row(), column);
    const auto value = field(row, column).toString();
    const auto color = field(row, column, Qt::BackgroundRole).value<QColor>();
    auto font = field(row, column, Qt::FontRole).value<QFont>();
    const bool hot = hovered == index, down = pressed == index;
    switch (column) {
    case 0: case 1: {
      // Acknowledgement and severity chips carry ALH's letters and colors.
      const bool alarm = color.isValid() && !value.trimmed().isEmpty();
      if (alarm) drawChip(painter, rect, down && column == 0 ? color.darker(112) : color, color.darker(125));
      else if (column == 0) drawChip(painter, rect, down ? pressFill : hot ? hoverFill : QColor(), outline);
      if (alarm) {
        font.setBold(true);
        painter->setPen(contrastingText(color));
        drawLabel(painter, rect, value, font, true);
      }
      break;
    }
    case 2:
      painter->setPen(text);
      drawLabel(painter, rect.adjusted(6, 0, -4, 0), value, font, false);
      break;
    case 3:
      if (hot || down) drawChip(painter, rect, down ? pressFill : hoverFill, QColor());
      disclosure(painter, rect, tree && isExpanded(row.sibling(row.row(), 0)), blend(text, base, 0.2));
      break;
    case 4: case 5:
      drawChip(painter, rect, down ? pressFill : hot ? hoverFill : blend(base, text, 0.04), outline);
      font.setBold(true);
      painter->setPen(text);
      drawLabel(painter, rect, value, font, true);
      break;
    default: {
      // Channel alarm data and configured masks stand out; group counts and
      // the default <-----> mask are secondary.
      QColor pen = muted;
      if (column == 8 && value.startsWith('<')) pen = text;
      if (column == 6 && std::any_of(value.begin(), value.end(), [](QChar c) { return c.isLetter(); }))
        pen = text;
      if (column == 6 && color.isValid()) {
        drawChip(painter, rect.adjusted(0, 2, 0, -2), color, QColor());
        pen = contrastingText(color);
      }
      painter->setPen(pen);
      drawLabel(painter, rect.adjusted(column == 6 ? 5 : 2, 0, 0, 0), value, font, false);
    }
    }
  }
  painter->restore();
}
void AlarmView::scheduleExtent(bool full, bool repaint) {
  extentFull |= full;
  extentRepaint |= repaint;
  if (extentPending)
    return;
  extentPending = true;
  QTimer::singleShot(0, this, [this] {
    extentPending = false;
    updateExtent();
  });
}
void AlarmView::setRowExtent(const QModelIndex& index) {
  const int extent = cells(index, QRect(0, 0, 1, alarmRowHeight(this)))[8].right() + 10;
  auto found = rowExtents.find(index.internalPointer());
  if (found != rowExtents.end()) {
    found->generation = extentGeneration;
    if (found->width == extent) return;
    auto count = extentCounts.find(found->width);
    if (--*count == 0) extentCounts.erase(count);
    found->width = extent;
  } else {
    rowExtents.insert(index.internalPointer(), {extent, extentGeneration});
  }
  ++extentCounts[extent];
}
bool AlarmView::rowShown(const QModelIndex& index) const {
  for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
    if (!isExpanded(parent)) return false;
  return true;
}
void AlarmView::updateExtent() {
  if (!model())
    return;
  if (extentFull) {
    ++extentGeneration;
    std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parent) {
      for (int row = 0; row < model()->rowCount(parent); ++row) {
        const auto index = model()->index(row, 0, parent);
        setRowExtent(index);
        if (tree && isExpanded(index)) visit(index);
      }
    };
    visit({});
    for (auto it = rowExtents.begin(); it != rowExtents.end();) {
      if (it->generation == extentGeneration) { ++it; continue; }
      auto count = extentCounts.find(it->width);
      if (--*count == 0) extentCounts.erase(count);
      it = rowExtents.erase(it);
    }
  } else {
    for (const auto& index : extentRows)
      if (index.isValid()) setRowExtent(index);
  }
  int width = extentCounts.isEmpty() ? 0 : qMax(0, extentCounts.lastKey());
  extentFull = false;
  extentRows.clear();
  width = qMax(width, viewport()->width());
  const bool repaint = extentRepaint || width != columnWidth(0);
  extentRepaint = false;
  setColumnWidth(0, width);
  if (repaint) viewport()->update();
}
} // namespace alh
