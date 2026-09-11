// Content-sized ALH rows, following awAlh.c and line.c. See ../../LICENSE.
#pragma once
#include <QCheckBox>
#include <QPushButton>
#include <QTreeView>
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
  void drawRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
  void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}

private:
  bool tree;
  bool extentPending = false;
  std::array<QRect, 9> cells(const QModelIndex&, const QRect&) const;
  void updateExtent();
  void scheduleExtent();
};
} // namespace alh
