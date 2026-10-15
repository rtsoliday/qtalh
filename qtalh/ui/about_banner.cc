#include "about_banner.h"
#include "appearance.h"
#include <cmath>

namespace alh {
namespace {
constexpr qreal ringSeconds = 0.95, ackSeconds = 0.85, partySeconds = 1.8;
constexpr qreal pi = 3.14159265358979323846;
qreal ease(qreal value) {
  value = qBound(qreal(0), value, qreal(1));
  return value * value * (3 - 2 * value);
}
QPointF between(const QPointF& from, const QPointF& to, qreal amount) {
  return from + (to - from) * amount;
}
void roundedLine(QPainter& painter, const QColor& color, qreal width,
                 const QPointF& from, const QPointF& to) {
  painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawLine(from, to);
}
} // namespace

AboutBanner::AboutBanner(QWidget* parent) : QWidget(parent), timer(this) {
  setObjectName("aboutAlarmBanner");
  setAccessibleName("Beeper, the alarm robot");
  setAccessibleDescription("Click or press Space for an alarm drill. Beeper rings the bell, presses ACK, and celebrates.");
  setToolTip("Click Beeper for a tiny alarm drill");
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMinimumWidth(360);
  setFixedHeight(sizeHint().height());
  timer.setObjectName("aboutAnimationTimer");
  timer.setInterval(33);
  connect(&timer, &QTimer::timeout, this, [this] { advance(clock.restart() / 1000.0); });
}

AboutBanner::Phase AboutBanner::phase() const {
  if (drillTime < 0) return Phase::Watching;
  if (drillTime < ringSeconds) return Phase::Ringing;
  if (drillTime < ringSeconds + ackSeconds) return Phase::Acknowledging;
  return Phase::Celebrating;
}

bool AboutBanner::startDrill() {
  if (phase() != Phase::Watching) return false;
  drillTime = 0;
  update();
  return true;
}

void AboutBanner::advance(qreal seconds) {
  if (!std::isfinite(seconds) || seconds <= 0) return;
  time += seconds;
  if (drillTime >= 0) {
    drillTime += seconds;
    if (drillTime >= ringSeconds + ackSeconds + partySeconds) drillTime = -1;
  }
  update();
}

void AboutBanner::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  clock.start();
  timer.start();
}

void AboutBanner::hideEvent(QHideEvent* event) {
  timer.stop();
  clock.invalidate();
  QWidget::hideEvent(event);
}

void AboutBanner::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    startDrill();
    event->accept();
  } else QWidget::mousePressEvent(event);
}

void AboutBanner::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    if (!event->isAutoRepeat()) startDrill();
    event->accept();
  } else QWidget::keyPressEvent(event);
}

void AboutBanner::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const qreal scale = qMin(width() / 460.0, height() / 156.0);
  painter.translate((width() - 460 * scale) / 2, (height() - 156 * scale) / 2);
  painter.scale(scale, scale);

  const auto base = palette().color(QPalette::Base);
  const auto ink = palette().color(QPalette::Text);
  const auto accent = palette().color(QPalette::Link);
  const auto muted = secondaryText(palette());
  const bool dark = base.lightness() < 128;
  const QColor teal("#36b8b2"), green("#45bd8b"), amber("#f5b84b"), red("#ed727a");
  const QColor outline(dark ? "#a7d5db" : "#34515f");
  const auto state = phase();
  const bool ringing = state == Phase::Ringing;
  const bool acknowledging = state == Phase::Acknowledging;
  const bool party = state == Phase::Celebrating;
  const qreal ack = acknowledging ? ease((drillTime - ringSeconds) / ackSeconds) : 0;
  const qreal celebration = party ? (drillTime - ringSeconds - ackSeconds) / partySeconds : 0;
  const bool cleared = state == Phase::Watching || party || (acknowledging && ack > 0.65);

  QPainterPath frame;
  frame.addRoundedRect(QRectF(0.5, 0.5, 459, 155), 12, 12);
  QLinearGradient background(0, 0, 460, 156);
  background.setColorAt(0, blend(base, teal, dark ? 0.18 : 0.10));
  background.setColorAt(1, base);
  painter.fillPath(frame, background);
  painter.setPen(QPen(blend(base, ink, 0.18), 1));
  painter.drawPath(frame);
  painter.setClipPath(frame);

  auto text = [&](const QRectF& bounds, const QString& value, const QColor& color,
                  int pixels = 11, bool bold = false, int alignment = Qt::AlignLeft | Qt::AlignVCenter) {
    auto font = this->font();
    font.setPixelSize(pixels);
    font.setBold(bold);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(bounds, alignment, value);
  };

  // A pocket-sized annunciator panel: the first two lamps react to the drill.
  text(QRectF(20, 10, 190, 20), party ? "ALL CLEAR!" : cleared ? "ALARM WATCH" : "BEEP! BEEP!", ink, 12, true);
  painter.setPen(QPen(blend(base, ink, 0.16), 1));
  painter.setBrush(blend(base, ink, dark ? 0.06 : 0.025));
  painter.drawRoundedRect(QRectF(18, 36, 184, 102), 9, 9);
  const QStringList channels{"Vacuum", "Timing", "RF"};
  for (int row = 0; row < 3; ++row) {
    const qreal y = 55 + row * 30;
    const QColor lamp = cleared || row == 2 ? green : row == 0 ? red : amber;
    const qreal pulse = !cleared && row < 2 ? 0.12 + 0.10 * std::sin(time * 12) : 0.09;
    painter.setPen(Qt::NoPen);
    painter.setBrush(blend(base, lamp, pulse));
    painter.drawEllipse(QPointF(35, y), 12, 12);
    painter.setBrush(lamp);
    painter.drawEllipse(QPointF(35, y), 5, 5);
    text(QRectF(49, y - 10, 66, 20), channels[row], ink);
    QPainterPath trace;
    for (int point = 0; point <= 35; ++point) {
      const qreal x = 114 + point * 1.4;
      const qreal wave = std::sin(point * 0.45 - time * 3 + row) * (cleared ? 2 : 4);
      if (!point) trace.moveTo(x, y + wave);
      else trace.lineTo(x, y + wave);
    }
    painter.setPen(QPen(lamp, 1.4));
    painter.drawPath(trace);
    text(QRectF(169, y - 10, 26, 20), cleared || row == 2 ? "OK" : "!", muted, 10, true);
  }

  // The ACK button squashes when Beeper's hand reaches it.
  const qreal press = acknowledging ? ease((ack - 0.60) / 0.16) : 0;
  const qreal buttonY = 110 + press * 3;
  painter.setPen(Qt::NoPen);
  painter.setBrush(blend(base, ink, 0.30));
  painter.drawRoundedRect(QRectF(387, 112, 57, 26), 7, 7);
  painter.setBrush(cleared ? blend(base, green, 0.30) : amber);
  painter.drawRoundedRect(QRectF(387, buttonY, 57, 23), 7, 7);
  text(QRectF(387, buttonY, 57, 23), "ACK", cleared ? ink : QColor("#34404b"), 11, true, Qt::AlignCenter);

  // Beeper's idle breathing, startled hop, and celebratory bounce.
  const qreal hop = ringing ? -std::abs(std::sin(drillTime * pi * 3)) * 7 :
      party ? -std::abs(std::sin(celebration * pi * 3)) * (1 - celebration) * 9 : std::sin(time * 2.5) * 1.7;
  const qreal lean = acknowledging ? std::sin(ack * pi / 2) * 12 : 0;
  painter.setPen(Qt::NoPen);
  painter.setBrush(blend(base, ink, 0.09));
  painter.drawEllipse(QRectF(255 + lean, 134, 84, 8));
  painter.save();
  painter.translate(lean, hop);
  roundedLine(painter, outline, 5, QPointF(278, 120), QPointF(274, 133));
  roundedLine(painter, outline, 5, QPointF(312, 120), QPointF(316, 133));
  painter.setPen(Qt::NoPen);
  painter.setBrush(teal);
  painter.drawRoundedRect(QRectF(261, 130, 22, 8), 4, 4);
  painter.drawRoundedRect(QRectF(309, 130, 22, 8), 4, 4);

  const QPointF rightHand = acknowledging ? between(QPointF(351, 110), QPointF(416 - lean, 114 - hop), ack) :
      party ? QPointF(349, 72 + std::sin(time * 16) * 5) : QPointF(350, 112 + std::sin(time * 2.5) * 2);
  const QPointF leftHand = ringing ? QPointF(239, 82) : QPointF(240, 109 + std::sin(time * 3) * 3);
  roundedLine(painter, outline, 5, QPointF(260, 99), leftHand);
  roundedLine(painter, outline, 5, QPointF(329, 99), rightHand);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor("#ffe5ba"));
  painter.drawEllipse(leftHand, 6, 6);
  painter.drawEllipse(rightHand, 6, 6);

  QLinearGradient shell(255, 52, 335, 123);
  shell.setColorAt(0, QColor("#b3f0e6"));
  shell.setColorAt(1, teal);
  painter.setBrush(shell);
  painter.setPen(QPen(outline, 2));
  painter.drawRoundedRect(QRectF(254, 51, 83, 74), 27, 27);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor("#e6fffa"));
  painter.drawRoundedRect(QRectF(264, 64, 63, 44), 18, 18);
  const bool blink = state == Phase::Watching && std::fmod(time, 4.6) > 4.40;
  for (qreal x : {281.0, 310.0}) {
    if (blink || (party && celebration > 0.45 && x > 300))
      roundedLine(painter, QColor("#344e60"), 3, QPointF(x - 4, 81), QPointF(x + 4, 81));
    else {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor("#344e60"));
      painter.drawEllipse(QPointF(x, 81), 5, ringing ? 7 : 6);
      painter.setBrush(Qt::white);
      painter.drawEllipse(QPointF(x - 1.5, 79), 1.6, 1.6);
    }
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor("#f5a7ad"));
  painter.drawEllipse(QPointF(270, 94), 5, 2.5);
  painter.drawEllipse(QPointF(321, 94), 5, 2.5);
  painter.setPen(QPen(QColor("#344e60"), 2, Qt::SolidLine, Qt::RoundCap));
  if (ringing) {
    painter.setBrush(QColor("#344e60"));
    painter.drawEllipse(QPointF(296, 98), 3, 4);
  } else {
    QPainterPath smile;
    smile.moveTo(289, 96);
    smile.quadTo(296, party ? 106 : 103, 303, 96);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(smile);
  }
  text(QRectF(271, 109, 49, 13), "Beeper", QColor("#284d57"), 9, true, Qt::AlignCenter);

  // A real little bell on a spring, with motion lines during the alarm.
  roundedLine(painter, outline, 2, QPointF(296, 51), QPointF(296, 37));
  painter.save();
  painter.translate(296, 32);
  painter.rotate(std::sin(time * (ringing ? 30 : 2)) * (ringing ? 22 : 4));
  QPainterPath bell;
  bell.moveTo(-10, 4);
  bell.quadTo(-6, 0, -6, -5);
  bell.cubicTo(-6, -14, 6, -14, 6, -5);
  bell.quadTo(6, 0, 10, 4);
  bell.closeSubpath();
  painter.setPen(QPen(QColor("#a77228"), 1.5));
  painter.setBrush(amber);
  painter.drawPath(bell);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor("#a77228"));
  painter.drawEllipse(QPointF(0, 6), 2.5, 2.5);
  painter.restore();
  if (ringing) {
    roundedLine(painter, amber, 2, QPointF(277, 23), QPointF(271, 19));
    roundedLine(painter, amber, 2, QPointF(315, 23), QPointF(321, 19));
    roundedLine(painter, amber, 2, QPointF(275, 32), QPointF(268, 32));
    roundedLine(painter, amber, 2, QPointF(317, 32), QPointF(324, 32));
  }
  painter.restore();

  if (acknowledging || party) {
    const QRectF bubble(354, 44, 83, 28);
    painter.setBrush(blend(base, teal, 0.13));
    painter.setPen(QPen(blend(base, teal, 0.5), 1));
    painter.drawRoundedRect(bubble, 10, 10);
    text(bubble, party ? "Phew!" : "On it!", ink, 12, true, Qt::AlignCenter);
  }
  if (party) {
    // A small, deterministic shower of stars: celebratory without flashing.
    const QColor colors[]{amber, teal, red, green};
    for (int i = 0; i < 9; ++i) {
      const qreal angle = (-160 + i * 19) * pi / 180;
      const qreal distance = 20 + celebration * 54;
      const QPointF center(298 + std::cos(angle) * distance,
                           48 + std::sin(angle) * distance + celebration * celebration * 35);
      QPainterPath star;
      for (int point = 0; point < 10; ++point) {
        const qreal a = point * pi / 5 - pi / 2;
        const qreal radius = (point % 2 ? 1.8 : 4.2) * (1 - celebration * 0.7);
        const QPointF vertex = center + QPointF(std::cos(a), std::sin(a)) * radius;
        if (!point) star.moveTo(vertex);
        else star.lineTo(vertex);
      }
      star.closeSubpath();
      painter.setPen(Qt::NoPen);
      painter.setBrush(colors[i % 4]);
      painter.drawPath(star);
    }
  }
  if (hasFocus()) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(accent, 1, Qt::DashLine));
    painter.drawRoundedRect(QRectF(3, 3, 454, 150), 10, 10);
  }
}
} // namespace alh
