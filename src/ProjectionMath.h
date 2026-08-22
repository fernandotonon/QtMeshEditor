/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Shared world→viewport projection + image-sampling math, used by both
MultiViewTextureBaker (#403 multi-view AI bake) and ProjectionPainter
(Paint v2 Slice F, #549). Header-only + pure math (no Ogre scene / GL / Qt
widgets), so both consumers share one definition and the core stays
unit-testable headlessly.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef PROJECTIONMATH_H
#define PROJECTIONMATH_H

#include <QImage>

#include <OgreColourValue.h>
#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <OgreVector3.h>
#include <OgreVector4.h>

#include <algorithm>
#include <cmath>

namespace ProjectionMath {

/// Result of projecting a world point through a view*proj matrix.
struct Projected {
    Ogre::Vector2 uv;        ///< viewport UV [0..1], top-left origin, V down (may be off-screen)
    float ndcZ = 0.0f;       ///< NDC z (for depth comparisons)
    bool  behind = false;    ///< true when the point is at/behind the camera (clip.w <= 0)
};

/// Project a world point through `viewProj` (world→clip) → viewport UV in
/// [0..1] with a top-left origin and V pointing down (matches QImage rows).
/// Sets `behind` when the point is at/behind the camera plane.
inline Projected projectToViewportUV(const Ogre::Vector3& worldP,
                                     const Ogre::Matrix4& viewProj)
{
    Projected out;
    const Ogre::Vector4 clip =
        viewProj * Ogre::Vector4(worldP.x, worldP.y, worldP.z, 1.0f);
    if (clip.w <= 1e-6f) {          // at or behind the camera plane
        out.behind = true;
        return out;
    }
    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;   // [-1..1]
    const float ndcY = clip.y * invW;   // [-1..1], +Y up
    out.ndcZ = clip.z * invW;
    // NDC -> viewport UV. Flip Y so v=0 is the top of the image (QImage row 0).
    out.uv = Ogre::Vector2(ndcX * 0.5f + 0.5f, 1.0f - (ndcY * 0.5f + 0.5f));
    return out;
}

/// Bilinear sample of a QImage at `uv` in [0..1] (top-left origin). Returns the
/// colour as an Ogre::ColourValue (0..1). Out-of-range UV is clamped to the edge.
inline Ogre::ColourValue sampleImage(const QImage& img, const Ogre::Vector2& uv)
{
    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0) return Ogre::ColourValue(0, 0, 0, 1);
    const float fx = std::clamp(uv.x, 0.0f, 1.0f) * (w - 1);
    const float fy = std::clamp(uv.y, 0.0f, 1.0f) * (h - 1);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float tx = fx - x0;
    const float ty = fy - y0;
    auto px = [&](int x, int y) {
        const QRgb c = img.pixel(x, y);
        return Ogre::ColourValue(qRed(c) / 255.0f, qGreen(c) / 255.0f,
                                 qBlue(c) / 255.0f, qAlpha(c) / 255.0f);
    };
    const Ogre::ColourValue c00 = px(x0, y0), c10 = px(x1, y0);
    const Ogre::ColourValue c01 = px(x0, y1), c11 = px(x1, y1);
    const Ogre::ColourValue top = c00 * (1 - tx) + c10 * tx;
    const Ogre::ColourValue bot = c01 * (1 - tx) + c11 * tx;
    return top * (1 - ty) + bot * ty;
}

} // namespace ProjectionMath

#endif // PROJECTIONMATH_H
