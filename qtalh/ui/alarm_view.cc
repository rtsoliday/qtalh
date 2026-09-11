// Motif-style presentation only; alarm state remains in the item model.
#include "alarm_view.h"
#include <QHeaderView>
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
class RowHeight : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
    return {100, 24};
  }
};
QVariant field(const QModelIndex& row, int column, int role = Qt::DisplayRole) {
  return row.sibling(row.row(), column).data(role);
}
} // namespace
QSize MotifButton::sizeHint() const {
  return QFontMetrics(font()).size(Qt::TextSingleLine, text()) + QSize(14, 10);
}
void MotifButton::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  bevel(&painter, rect(), background, isDown());
  painter.setPen(Qt::black);
  painter.drawRect(rect().adjusted(2, 2, -2, -2));
  painter.setFont(font());
  painter.setPen(background == QColor("red") ? Qt::white : Qt::black);
  painter.drawText(rect().adjusted(4, 2, -4, -2).translated(0, 2), Qt::AlignCenter, text());
}
QSize MotifCheckBox::sizeHint() const {
  auto metrics = QFontMetrics(font());
  return {metrics.horizontalAdvance(text()) + 16, qMax(18, metrics.height())};
}
void MotifCheckBox::paintEvent(QPaintEvent*) {
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
  setHeaderHidden(true);
  setRootIsDecorated(false);
  setIndentation(0);
  setUniformRowHeights(true);
  setItemDelegate(new RowHeight(this));
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setExpandsOnDoubleClick(false);
  setDragEnabled(true);
  setFrameShape(QFrame::NoFrame);
  auto p = palette();
  p.setColor(QPalette::Base, base);
  setPalette(p);
  header()->setMinimumSectionSize(0);
  header()->setStretchLastSection(false);
  connect(this, &QTreeView::expanded, this, [this] { scheduleExtent(); });
  connect(this, &QTreeView::collapsed, this, [this] { scheduleExtent(); });
}
void AlarmView::setModel(QAbstractItemModel* next) {
  if (next == model())
    return;
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
  connect(next, &QAbstractItemModel::dataChanged, this, [this] { scheduleExtent(); });
  updateExtent();
}
std::array<QRect, 9> AlarmView::cells(const QModelIndex& row, const QRect& bounds) const {
  std::array<QRect, 9> result;
  int depth = 0;
  if (tree)
    for (auto parent = row.parent(); parent.isValid(); parent = parent.parent())
      ++depth;
  int x = bounds.left() + (tree ? 20 + depth * 12 : 14);
  const int y = bounds.top() + 5;
  auto add = [&](int column, int width, int height = 18) {
    result[column] = QRect(x, y, width, height);
    x += width + 3;
  };
  add(0, 14);
  add(1, 14);
  auto nameFont = field(row, 2, Qt::FontRole).value<QFont>();
  add(2, QFontMetrics(nameFont).horizontalAdvance(field(row, 2).toString()) + 10);
  for (int column : {3, 4, 5})
    if (!field(row, column).toString().isEmpty())
      add(column, 14);
  auto font = field(row, 6, Qt::FontRole).value<QFont>();
  QFontMetrics metrics(font);
  add(6, metrics.horizontalAdvance(field(row, 6).toString()) + 4);
  add(7, 10);
  add(8, metrics.horizontalAdvance(field(row, 8).toString()) + 4);
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
      bool selected = column == 2 && selectionModel() &&
                      selectionModel()->isRowSelected(row.row(), row.parent());
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
      width = qMax(width, cells(index, QRect(0, 0, 1, 24))[8].right() + 10);
      if (tree && isExpanded(index))
        visit(index);
    }
  };
  visit({});
  setColumnWidth(0, qMax(width, viewport()->width()));
  viewport()->update();
}
} // namespace alh
