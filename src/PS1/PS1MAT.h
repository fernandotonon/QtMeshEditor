/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/
#ifndef PS1MAT_H
#define PS1MAT_H

#include <QColor>
#include <QString>
#include <QVector>

/**
 * Sony PlayStation / Psy-Q MAT (material list) support (ASCII).
 *
 * Common format:
 *   @MAT940801
 *   # Number of Items
 *   425
 *   # Materials
 *   0  0 F C 255 255 0
 *   1  0 F C 255 255 0
 *
 * We currently extract only RGB (last 3 ints) per entry.
 */
namespace PS1MAT {

struct MatEntry {
    QColor rgb; // 0..255
};

bool parseMatFile(const QString& matPath, QVector<MatEntry>& outEntries, QString* outError = nullptr);

/// Write a minimal Psy-Q MAT file with one RGB entry per face.
bool writeMatFile(const QString& matPath, const QVector<MatEntry>& entries, QString* outError = nullptr);

} // namespace PS1MAT

#endif

