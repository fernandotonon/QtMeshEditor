#include "UVTransform.h"

#include <algorithm>
#include <limits>

namespace UVTransform {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float snapToGrid(float value, float gridSize)
{
    if (gridSize <= 0.f)
        return value;
    return std::round(value / gridSize) * gridSize;
}

} // namespace

Ogre::Vector2 medianPivot(const std::vector<VertRef>& verts)
{
    if (verts.empty())
        return Ogre::Vector2::ZERO;
    Ogre::Vector2 sum = Ogre::Vector2::ZERO;
    for (const auto& v : verts)
        sum += v.uv;
    return sum / static_cast<float>(verts.size());
}

Ogre::Vector2 snapUv(Ogre::Vector2 uv, const Settings& settings,
                     const std::vector<VertRef>& allVerts,
                     const std::vector<int>& selectedIds)
{
    const bool doSnap = settings.snapEnabled != settings.invertSnap;
    if (!doSnap)
        return uv;

    switch (settings.snap) {
    case SnapMode::Grid:
        return {snapToGrid(uv.x, settings.gridSize), snapToGrid(uv.y, settings.gridSize)};
    case SnapMode::Vertex: {
        float bestDistSq = settings.snapThreshold * settings.snapThreshold;
        Ogre::Vector2 best = uv;
        for (const auto& v : allVerts) {
            if (std::find(selectedIds.begin(), selectedIds.end(), v.id) != selectedIds.end())
                continue;
            const float dx = v.uv.x - uv.x;
            const float dy = v.uv.y - uv.y;
            const float d = dx * dx + dy * dy;
            if (d <= bestDistSq) {
                bestDistSq = d;
                best = v.uv;
            }
        }
        return best;
    }
    case SnapMode::Pixel:
        if (settings.texturePixelSize <= 0)
            return uv;
        {
            const float step = 1.f / static_cast<float>(settings.texturePixelSize);
            return {snapToGrid(uv.x, step), snapToGrid(uv.y, step)};
        }
    }
    return uv;
}

Ogre::Vector2 transformPoint(TransformOp op, const Ogre::Vector2& uv,
                             const Ogre::Vector2& pivot, const Ogre::Vector2& delta,
                             float numericValue, bool numericInput)
{
    switch (op) {
    case TransformOp::Move:
        if (numericInput)
            return {uv.x + numericValue, uv.y};
        return {uv.x + delta.x, uv.y + delta.y};
    case TransformOp::Rotate: {
        const float angleDeg = numericInput ? numericValue : delta.x;
        const float rad = angleDeg * (kPi / 180.f);
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float dx = uv.x - pivot.x;
        const float dy = uv.y - pivot.y;
        return {pivot.x + dx * c - dy * s, pivot.y + dx * s + dy * c};
    }
    case TransformOp::Scale: {
        const float factor = numericInput ? numericValue : std::max(1e-4f, delta.x);
        return {pivot.x + (uv.x - pivot.x) * factor, pivot.y + (uv.y - pivot.y) * factor};
    }
    case TransformOp::MirrorX:
        return {pivot.x - (uv.x - pivot.x), uv.y};
    case TransformOp::MirrorY:
        return {uv.x, pivot.y - (uv.y - pivot.y)};
    }
    return uv;
}

std::vector<VertRef> applyTransform(TransformOp op,
                                    const std::vector<VertRef>& input,
                                    const Settings& settings,
                                    const std::vector<VertRef>& allVertsForSnap,
                                    const Ogre::Vector2& delta,
                                    float numericValue,
                                    bool numericInput)
{
    if (input.empty())
        return {};

    std::vector<int> selectedIds;
    selectedIds.reserve(input.size());
    for (const auto& v : input)
        selectedIds.push_back(v.id);

    const Ogre::Vector2 median = medianPivot(input);
    const Ogre::Vector2 cursorPivot = settings.cursor;

    std::vector<VertRef> out;
    out.reserve(input.size());

    for (const auto& src : input) {
        Ogre::Vector2 pivot = median;
        if (settings.pivot == PivotMode::IndividualOrigins)
            pivot = src.uv;
        else if (settings.pivot == PivotMode::Cursor)
            pivot = cursorPivot;

        Ogre::Vector2 uv = transformPoint(op, src.uv, pivot, delta, numericValue, numericInput);
        uv = snapUv(uv, settings, allVertsForSnap, selectedIds);
        out.push_back({src.id, uv});
    }

    return out;
}

} // namespace UVTransform
