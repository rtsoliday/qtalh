#include "appearance.h"
#include "core/model.h"
#include <cmath>
namespace alh {
namespace {
bool legacy = true;
constexpr auto baseFontProperty = "_qtalhBaseFont";

qreal pointSize(const QFont& font) {
  return font.pointSizeF() > 0 ? font.pointSizeF() : QFontInfo(font).pointSizeF();
}

QFont smallerDefaultFont(QFont font) {
  font.setPointSizeF(qMax(qreal(6), pointSize(font) - 1));
  return font;
}

// One application filter also handles modal dialogs and focused text editors.
// Claim ShortcutOverride so widget-local zoom cannot consume these keys first.
class FontScaling : public QObject {
public:
  explicit FontScaling(QObject* parent) : QObject(parent), startup(QApplication::font()) {}
  QFont scaled(QFont font) const {
    if (offset) font.setPointSizeF(qMax(qreal(6), pointSize(font) + offset));
    return font;
  }
protected:
  bool eventFilter(QObject* receiver, QEvent* event) override {
    if (!qobject_cast<QWidget*>(receiver) ||
        (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress))
      return false;
    auto key = static_cast<QKeyEvent*>(event);
    // Qt maps ControlModifier to Command on macOS.
    auto modifiers = key->modifiers() & ~Qt::KeypadModifier;
    if (modifiers != Qt::ControlModifier &&
        modifiers != (Qt::ControlModifier | Qt::ShiftModifier)) return false;
    const int code = key->key();
    const bool increase = code == Qt::Key_Plus || code == Qt::Key_Equal;
    if (!increase && (modifiers != Qt::ControlModifier ||
                      (code != Qt::Key_Minus && code != Qt::Key_0))) return false;
    event->accept();
    if (event->type() == QEvent::ShortcutOverride) return true;
    const qreal base = pointSize(startup);
    offset = code == Qt::Key_0 ? 0 :
        qBound(qMin(0, qCeil(6 - base)), offset + (increase ? 1 : -1),
               qMax(0, qFloor(72 - base)));
    QApplication::setFont(scaled(startup));
    for (auto widget : QApplication::allWidgets()) {
      const auto original = widget->property(baseFontProperty);
      if (original.isValid()) widget->setFont(scaled(original.value<QFont>()));
    }
    return true;
  }
private:
  QFont startup;
  int offset = 0;
};
QPointer<FontScaling> fontScaling;
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
void initializeAppearance(const QString& name) {
  delete fontScaling.data();
  legacy = name.isEmpty() || name.compare("motif", Qt::CaseInsensitive) == 0;
  if (!legacy) {
    auto style = QStyleFactory::create(name);
    if (!style)
      throw ParseError("Unknown widget style: " + name + ". Available styles: motif, " +
                       QStyleFactory::keys().join(", "));
    QApplication::setStyle(style);
    QApplication::setFont(smallerDefaultFont(QApplication::font()));
    fontScaling = new FontScaling(qApp);
    qApp->installEventFilter(fontScaling);
    return;
  }
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

bool legacyAppearance() { return legacy; }
QColor contrastingText(const QColor& color) {
  auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  double luminance = 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
                     0.0722 * linear(color.blueF());
  return luminance > 0.179 ? QColor(Qt::black) : QColor(Qt::white);
}
void setPresentationFont(QWidget* widget, const QFont& font) {
  if (!legacyAppearance()) widget->setProperty(baseFontProperty, font);
  widget->setFont(fontScaling ? fontScaling->scaled(font) : font);
}
QFont contentFont() {
  if (legacyAppearance()) return QApplication::font();
  auto font = smallerDefaultFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  // Some platform plugins report the UI font for FixedFont. Ask fontconfig/the
  // platform font matcher for a monospace family in that case.
  if (!QFontInfo(font).fixedPitch()) font.setFamily("monospace");
  font.setStyleHint(QFont::TypeWriter);
  font.setFixedPitch(true);
  return font;
}
void sizeDialog(QDialog* dialog, const QSize& legacySize) {
  if (legacyAppearance()) { dialog->resize(legacySize); return; }
  dialog->ensurePolished();
  auto available = dialog->screen()->availableGeometry().size() - QSize(40, 60);
  dialog->resize(dialog->sizeHint().expandedTo(legacySize).boundedTo(available));
}
} // namespace alh
