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

#include <array>

/**
 * Sony PlayStation / Psy-Q MAT (material list) support (ASCII).
 *
 * Common format (per polygon, one entry per PLY face, in order):
 *   <polyIndex>  <flag> <normalMode> <typeChar> <payload...>
 *
 * Where:
 *   - Integer after poly index: packed flags from the Blender RSD exporter's
 *     `materialFlag("000",0,0,unlit)` — the **least significant bit is unlit** (no
 *     hardware lighting / "full bright") when other bits are zero. Omitted legacy
 *     lines (`0 F G ...`) default to **lit** (unlit=false).
 *   - Next letter: Blender mesh smooth (`G`) vs flat (`F`) shading — stored as `shadingChar`.
 *   - typeChar 'C' = flat solid color           (payload: R G B)
 *   - typeChar 'G' = smooth (Gouraud) color     (payload: 3*RGB for tri or 4*RGB for quad)
 *   - typeChar 'T' = textured, no color         (payload: texIndex u0 v0 u1 v1 u2 v2 u3 v3)
 *   - typeChar 'D' = textured + flat color      (payload: texIndex 8*UV R G B)
 *   - typeChar 'H' = textured + smooth color    (payload: texIndex 8*UV 12*RGB for quad / 9*RGB padded for tri)
 *
 * UV coordinates are PS1 raw pixel offsets (top-origin); the importer divides by
 * texture width/height to get normalized 0..1 coords (no V flip needed - the
 * Blender exporter already flipped V into PS1 conventions on the way out).
 *
 * The legacy `rgb` field on MatEntry is filled with a representative colour
 * (single colour for C; first vertex colour for G/D/H) so callers that only
 * care about a per-face flat colour keep working.
 */
namespace PS1MAT {

struct UV {
    int u = 0;
    int v = 0;
};

struct MatEntry {
    QColor rgb;                ///< representative RGB (back-compat); first vert colour for smooth shaded entries.
    char shadingChar = 'F';    ///< Blender exporter: poly flat (`F`) vs smooth (`G`) normals — kept verbatim.
    char typeChar = 'C';       ///< 'C', 'G', 'T', 'H', or 'D' - see header doc.
    bool unlit = false;        ///< MAT flag LSB: no scene lighting (Blender "Unlit" / PS1 no-light style).
    bool textured = false;     ///< true for T/H/D.
    int  textureIndex = -1;    ///< RSD TEX[i] index for textured polygons; -1 otherwise.
    QVector<UV> uvs;           ///< 0, 3 or 4 entries (textured polygons only).
    QVector<QColor> vertColors;///< 1 (C/D), 3 (G/H tri), or 4 (G/H quad). Always populated when the entry has colour data.
};

bool parseMatFile(const QString& matPath, QVector<MatEntry>& outEntries, QString* outError = nullptr);

/// Write a minimal Psy-Q MAT file with one RGB entry per face.
///
/// Entries are serialised as `<idx> <flag> <shading> <type> <payload>` where `<flag>`
/// matches the Blender exporter's `materialFlag(..., unlit)` (LSB = unlit).
/// Textured entries (`textureIndex >= 0`
/// and `uvs.size() >= 3`) write the texture index + 8 UV ints, padding with the
/// final UV (or `0 0`) for triangles to keep the on-disk shape stable.
bool writeMatFile(const QString& matPath, const QVector<MatEntry>& entries, QString* outError = nullptr);

} // namespace PS1MAT

#endif

