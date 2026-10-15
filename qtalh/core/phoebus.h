// Conversion between ALH configurations and Phoebus alarm-server XML
// (the <config>/<component>/<pv> format read and written by Phoebus and NALMS).
#pragma once
#include "model.h"
namespace alh {
// Each conversion appends one note per kind of setting that has no
// equivalent in the target format, with example paths. Notes never stop a
// conversion; malformed input throws ParseError.
QByteArray writePhoebusXml(const Document&, QStringList* notes = nullptr);
void savePhoebusXml(const Document&, const QString& path, QStringList* notes = nullptr);
// XInclude elements are expanded relative to baseDir (or the file's directory).
Document parsePhoebusXml(const QByteArray& xml, const QString& baseDir = {},
                         QStringList* notes = nullptr);
Document loadPhoebusXml(const QString& path, QStringList* notes = nullptr);
} // namespace alh
