/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — per-vertex ambient occlusion from depth maps
(Paint v2 Slice G, issue #550)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "DerivedMapOcclusion.h"

#include "ProjectionMath.h"

#include <algorithm>
#include <cmath>

namespace DerivedMapOcclusion {

bool isVisibleInView(const Ogre::Vector3& worldPos, const DepthView& view)
{
    if (view.depth.isNull()) return false;

    const ProjectionMath::Projected p =
        ProjectionMath::projectToViewportUV(worldPos, view.viewProj);
    if (p.behind) return false;
    if (p.uv.x < 0.0f || p.uv.x > 1.0f || p.uv.y < 0.0f || p.uv.y > 1.0f) return false;

    // Reconstruct the nearest recorded surface distance: grayscale is linear in
    // world distance over [depthNear, depthFar], near = bright.
    const Ogre::ColourValue d = ProjectionMath::sampleImage(view.depth, p.uv);
    const float dMap = view.depthNear + (1.0f - d.r) * (view.depthFar - view.depthNear);

    // Camera-AXIS distance, matching how the fog encoded it. Euclidean would
    // over-read off-axis points and make them self-occlude.
    float dPoint;
    if (!view.camDirection.isZeroLength())
        dPoint = (worldPos - view.camPosition).dotProduct(view.camDirection);
    else
        dPoint = (worldPos - view.camPosition).length();

    // A NEGATIVE axis distance means the point is behind the eye plane. The
    // perspective `behind` flag above only catches this when w <= 0, which never
    // happens under an ORTHOGRAPHIC viewProj (w stays 1), so test it explicitly
    // rather than relying on the projection type.
    if (dPoint < 0.0f) return false;

    return dPoint <= dMap + view.biasWorld;
}

float occlusionAt(const Ogre::Vector3& worldPos,
                  const Ogre::Vector3& worldNormal,
                  const std::vector<DepthView>& views)
{
    if (views.empty()) return 0.0f;

    Ogre::Vector3 n = worldNormal;
    const bool haveNormal = !n.isZeroLength();
    if (haveNormal) n.normalise();

    int considered = 0;
    int blocked = 0;
    for (const DepthView& v : views) {
        if (v.depth.isNull()) continue;
        if (haveNormal && !v.camDirection.isZeroLength()) {
            // The camera looks along camDirection, so it sits on the -camDirection
            // side. Only count views on the OUTWARD side of the surface: a view
            // from behind the face cannot contribute to how lit that face is,
            // and counting it would darken everything by ~half uniformly.
            const float facing = n.dotProduct(-v.camDirection);
            if (facing <= 0.0f) continue;
        }
        ++considered;
        if (!isVisibleInView(worldPos, v)) ++blocked;
    }
    if (considered == 0) return 0.0f;   // nothing could see it either way
    return static_cast<float>(blocked) / static_cast<float>(considered);
}

std::vector<float> occlusionForVertices(const std::vector<Ogre::Vector3>& positions,
                                        const std::vector<Ogre::Vector3>& normals,
                                        const std::vector<DepthView>& views)
{
    std::vector<float> out(positions.size(), 0.0f);
    for (size_t i = 0; i < positions.size(); ++i) {
        const Ogre::Vector3 n = (i < normals.size()) ? normals[i] : Ogre::Vector3::ZERO;
        out[i] = occlusionAt(positions[i], n, views);
    }
    return out;
}

std::vector<Ogre::Vector3> sampleDirections(int count)
{
    const int n = std::max(1, count);
    std::vector<Ogre::Vector3> dirs;
    dirs.reserve(static_cast<size_t>(n));

    // Fibonacci lattice: near-uniform over the sphere with no clustering at the
    // poles (which a naive lat/long grid would give, biasing AO vertically).
    const float golden = static_cast<float>(M_PI) * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < n; ++i) {
        // y from +1 to -1, evenly in the z-axis-projected sense.
        const float y = (n == 1) ? 0.0f
                                 : 1.0f - 2.0f * (static_cast<float>(i) / static_cast<float>(n - 1));
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float theta = golden * static_cast<float>(i);
        dirs.emplace_back(std::cos(theta) * r, y, std::sin(theta) * r);
        dirs.back().normalise();
    }
    return dirs;
}

} // namespace DerivedMapOcclusion
