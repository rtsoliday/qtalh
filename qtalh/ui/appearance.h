#pragma once
#include <QtWidgets>
namespace alh {
// Selected once, before any application windows are constructed.
void initializeAppearance(const QString& style = {});
bool legacyAppearance();
QColor contrastingText(const QColor& background);
// Explicit fonts retain their family and startup size when the UI is zoomed.
void setPresentationFont(QWidget*, const QFont&);
QFont contentFont();
void sizeDialog(QDialog*, const QSize& legacySize);
} // namespace alh
