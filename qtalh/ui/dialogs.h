#pragma once
#include "appearance.h"
namespace alh {
// Shared appearance-aware presentation for auxiliary windows; no alarm state lives here.
QVBoxLayout* dialogColumn(QWidget*);
QVBoxLayout* dialogFrame(QVBoxLayout*, const QString& title = {});
QHBoxLayout* dialogRow(QVBoxLayout*);
QDialogButtonBox* dialogActions(QVBoxLayout*, QDialog*,
                                QDialogButtonBox::StandardButtons, const QString& helpTopic = {});
void dialogHeading(QVBoxLayout*, const QString& kind, const QString& name);
// Columns apply to the modern appearance; the classic panel is one column.
QVector<QCheckBox*> maskChoices(QVBoxLayout*, const QString& mask, bool passive = false,
                                int columns = 1);
QLabel* sectionHeading(const QString& title);
// A checkable part of a segmented choice; the edges select the rounded corners.
QPushButton* segmentButton(const QString& text, bool leftEdge, bool rightEdge);
// Search row for a read-only text view: Forward/Reverse (classic) or Find (modern).
// Classic rows use dialog spacing unless given another value (-1: style default).
void findBar(QVBoxLayout*, QPlainTextEdit*, int classicSpacing = 5);
QString selectedMask(const QVector<QCheckBox*>&);
QString chooseFile(QWidget*, const QString& title, const QString& initial,
                   const QString& filters, bool save = false);
} // namespace alh
