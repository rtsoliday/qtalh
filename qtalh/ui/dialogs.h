#pragma once
#include <QtWidgets>
namespace alh {
// Shared Motif presentation for auxiliary windows; no alarm state lives here.
QVBoxLayout* dialogColumn(QWidget*);
QVBoxLayout* dialogFrame(QVBoxLayout*, const QString& title = {});
QHBoxLayout* dialogRow(QVBoxLayout*);
QDialogButtonBox* dialogActions(QVBoxLayout*, QDialog*,
                                QDialogButtonBox::StandardButtons, const QString& help = {});
void dialogHeading(QVBoxLayout*, const QString& kind, const QString& name);
QVector<QCheckBox*> maskChoices(QVBoxLayout*, const QString& mask, bool passive = false);
QString selectedMask(const QVector<QCheckBox*>&);
QString chooseFile(QWidget*, const QString& title, const QString& initial,
                   const QString& filters, bool save = false);
} // namespace alh
