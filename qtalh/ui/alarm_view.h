// Content-sized ALH rows, following awAlh.c and line.c. See ../../LICENSE.
#pragma once
#include "appearance.h"
#include <QCheckBox>
#include <QCache>
#include <QMap>
#include <QDrag>
#include <QPushButton>
#include <QStaticText>
#include <QTreeView>
#include <QTimer>
#include <array>
namespace alh {
// Model role: the highest severity a row shows, current or unacknowledged.
constexpr int AlarmSeverityRole = Qt::UserRole + 20;
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
  // ALH's Silence N minutes display: a blue pane with white text.
  void setSilenced(bool);
  void setModel(QAbstractItemModel*) override;
  void scrollTo(const QModelIndex&, ScrollHint hint = EnsureVisible) override;
  QRect visualRect(const QModelIndex&) const override;
  QModelIndex indexAt(const QPoint&) const override;
  void dataChanged(const QModelIndex&, const QModelIndex&,
                   const QVector<int>& roles = QVector<int>()) override;

protected:
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void startNameDrag(const QModelIndex&);
  virtual void executeNameDrag(QDrag*);
  bool event(QEvent*) override;
  void currentChanged(const QModelIndex&, const QModelIndex&) override;
  bool viewportEvent(QEvent*) override;
  void paintEvent(QPaintEvent*) override;
  void drawStyledRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const;
  void drawRow(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
  void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}
  void resizeEvent(QResizeEvent*) override;
  void updateGeometries() override;

private:
  QPersistentModelIndex hovered, pressed;
  QPersistentModelIndex mouseTarget, pendingArrow, dragTarget;
  QPoint dragOrigin;
  QTimer arrowTimer;
  bool tree;
  // The region being painted; rows skip cells outside it.
  QRegion paintRegion;
  // Rows with alarms scrolled out of view: a bar along the bottom counts those
  // above and below, and each count scrolls to the nearest such row.
  QWidget* offscreenBar = nullptr;
  MotifButton *alarmsAbove = nullptr, *alarmsBelow = nullptr;
  QPersistentModelIndex nearestAbove, nearestBelow;
  QTimer offscreenTimer;
  int offscreenHeight = 0;
  bool offscreenUpdating = false, offscreenGeometryUpdating = false;
  void scheduleOffscreen() { if (!offscreenTimer.isActive()) offscreenTimer.start(); }
  void updateOffscreen();
  bool extentPending = false;
  // Data-only changes repaint their own rows; other triggers repaint everything.
  bool extentRepaint = true;
  // Views can hold thousands of rows. Keep each visible row's extent so a data
  // change re-measures only the reported rows; structural changes (including
  // tree expansion) measure all. Recorded indexes stay valid until then.
  bool extentFull = true;
  QVector<QModelIndex> extentRows;
  bool rowShown(const QModelIndex&) const;
  // A full measurement marks the rows it visits and then drops unvisited ones,
  // so retained rows are updated in place rather than reallocated.
  struct RowExtent {
    int width = 0;
    quint32 generation = 0;
  };
  QHash<const void*, RowExtent> rowExtents;
  quint32 extentGeneration = 0;
  QMap<int, int> extentCounts;
  void setRowExtent(const QModelIndex&);
  void updateRow(const QModelIndex&);
  // Text/font values survive model refreshes; cap retained labels and measurements.
  mutable QCache<QPair<QFont, QString>, int> textWidths{256 * 1024};
  int textAdvance(const QString&, const QFont&) const;
  // Modern rows repaint the same labels and chips repeatedly: keep prepared
  // text layouts and rendered chips instead of shaping and rasterizing again.
  mutable QCache<QPair<QFont, QString>, QStaticText> staticTexts{4096};
  mutable QHash<QPair<quint64, quint64>, QPixmap> chipImages;
  void drawLabel(QPainter*, const QRect&, const QString&, const QFont&, bool centered) const;
  void drawChip(QPainter*, const QRect&, const QColor& fill, const QColor& outline) const;
  void invalidateTextWidths();
  std::array<QRect, 9> cells(const QModelIndex&, const QRect&) const;
  void updateExtent();
  void scheduleExtent(bool full = true, bool repaint = true);
};
} // namespace alh
