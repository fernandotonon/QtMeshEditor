#ifndef UV_TRANSFORM_H
#define UV_TRANSFORM_H

#include <OgreVector2.h>

#include <cmath>
#include <vector>

/// Pure UV-space transform helpers for the UV editor (issue #461).
namespace UVTransform {

enum class PivotMode {
    Median = 0,
    IndividualOrigins = 1,
    Cursor = 2
};

enum class SnapMode {
    Grid = 0,
    Vertex = 1,
    Pixel = 2
};

enum class TransformOp {
    Move = 0,
    Rotate = 1,
    Scale = 2,
    MirrorX = 3,
    MirrorY = 4
};

struct VertRef {
    int id = -1;
    Ogre::Vector2 uv;
};

struct Settings {
    PivotMode pivot = PivotMode::Median;
    SnapMode snap = SnapMode::Grid;
    bool snapEnabled = false;
    bool invertSnap = false;
    float gridSize = 0.125f;
    float snapThreshold = 0.02f;
    Ogre::Vector2 cursor{0.5f, 0.5f};
    int texturePixelSize = 0; // largest dimension when texture preview is active
};

Ogre::Vector2 medianPivot(const std::vector<VertRef>& verts);

Ogre::Vector2 snapUv(Ogre::Vector2 uv, const Settings& settings,
                     const std::vector<VertRef>& allVerts,
                     const std::vector<int>& selectedIds);

Ogre::Vector2 transformPoint(TransformOp op, const Ogre::Vector2& uv,
                             const Ogre::Vector2& pivot, const Ogre::Vector2& delta,
                             float numericValue, bool numericInput);

std::vector<VertRef> applyTransform(TransformOp op,
                                      const std::vector<VertRef>& input,
                                      const Settings& settings,
                                      const std::vector<VertRef>& allVertsForSnap,
                                      const Ogre::Vector2& delta,
                                      float numericValue,
                                      bool numericInput);

} // namespace UVTransform

#endif // UV_TRANSFORM_H
