// Content-sized ALH rows, following awAlh.c and line.c. See ../../LICENSE.
#pragma once
#include "appearance.h"
#include <QCheckBox>
#include <QCache>
#include <QDrag>
#include <QPushButton>
#include <QTreeView>
#include <QTimer>
#include <array>
namespace alh {
class MotifButton : public QPushButton {
public:
  using QPushButton::QPushButton;
  QColor background = QColor("#b0c3ca");
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent*) override;
};
class MotifCheckBox : public QCheckBox {
public:
  using QCheckBox::QCheckBox;
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent*) override;
};
class AlarmView : public QTreeView {
public:
  explicit AlarmView(bool tree, QWidget* parent = nullptr);
  void setModel(QAbstractItemModel*) override;
  QRect visualRect(const QModelIndex&) const override;
  QModelIndex indexAt(const QPoint&) const override;

protected:
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void startNameDrag(const QModelIndex&);
  virtual void executeNameDrag(QDrag*);
  bool event(QEvent*) override;
  bool viewportEvent(QEvent*) override;
  void drawStyledRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const;
  void drawRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
  void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}

private:
  QPersistentModelIndex hovered, pressed;
  QPersistentModelIndex mouseTarget, pendingArrow, dragTarget;
  QPoint dragOrigin;
  QTimer arrowTimer;
  bool tree;
  bool extentPending = false;
  // Text/font values survive model refreshes; cap retained labels and measurements.
  mutable QCache<QPair<QFont, QString>, int> textWidths{256 * 1024};
  int textAdvance(const QString&, const QFont&) const;
  void invalidateTextWidths();
  std::array<QRect, 9> cells(const QModelIndex&, const QRect&) const;
  void updateExtent();
  void scheduleExtent();
};
} // namespace alh
