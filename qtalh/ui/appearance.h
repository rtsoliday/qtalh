#pragma once
#include <QtWidgets>
namespace alh {
// Selected once, before any application windows are constructed. An empty name
// selects the modern appearance; "motif" selects the classic ALH appearance.
void initializeAppearance(const QString& style = {});
bool legacyAppearance();
QColor contrastingText(const QColor& background);
// ALH severity colors (0 = no alarm). The modern appearance returns an invalid
// color for no alarm so that rows keep the surrounding palette.
QColor severityColor(int severity);
// Blend of two colors; used for subtle borders, hover tints and secondary text.
QColor blend(const QColor& from, const QColor& to, qreal amount);
QColor secondaryText(const QPalette&);
// Explicit fonts retain their family and startup size when the UI is zoomed.
void setPresentationFont(QWidget*, const QFont&);
QFont contentFont();
void sizeDialog(QDialog*, const QSize& legacySize);
} // namespace alh
