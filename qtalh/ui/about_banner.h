#pragma once
#include <QtWidgets>

namespace alh {
// A decorative alarm drill, independent of the monitored facility. Keeping time
// explicit also lets tests render the entire sequence without wall-clock waits.
class AboutBanner : public QWidget {
public:
  enum class Phase { Watching, Ringing, Acknowledging, Celebrating };
  explicit AboutBanner(QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(460, 156); }
  Phase phase() const;
  bool startDrill();
  void advance(qreal seconds);

protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

private:
  QTimer timer;
  QElapsedTimer clock;
  qreal time = 0, drillTime = -1;
};
} // namespace alh
