#pragma once
#include "appearance.h"

namespace alh {
// One reusable, modeless guide per owning window. Modal file choosers own their
// guide so it remains usable while the chooser is open.
QDialog* showHelp(QWidget* parent, const QString& topic = {});
void contextHelp(QWidget*, const QString& topic);
void showAbout(QWidget* parent);
} // namespace alh
