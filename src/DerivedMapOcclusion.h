/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — per-vertex ambient occlusion from depth maps
(Paint v2 Slice G, issue #550)

The issue proposed "short-ray hemispherical occlusion" on the CPU. There is no
BVH/kd-tree anywhere in the repo and every existing ray query is a brute-force
linear scan, so instead of adding an acceleration structure this reuses the
depth-map visibility test already proven by ProjectionPainter::OcclusionMap
(#549): render the mesh's depth from N directions, then for each vertex count
how many of those directions can actually see it. That fraction, inverted, is
the occlusion.

The MATH is pure data (a vertex + a set of DepthView records -> a 0..1 scalar),
so it is unit-testable headlessly with synthetic depth images. The RENDERING of
those views touches the Ogre scene and lives in the controller.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef DERIVEDMAPOCCLUSION_H
#define DERIVEDMAPOCCLUSION_H

#include <OgreMatrix4.h>
#include <OgreVector3.h>

#include <QImage>

#include <vector>

/// One rendered depth view: the grayscale depth image plus the exact matrices
/// used, so a world point can be projected back into it. Mirrors the fields of
/// MeshDepthRenderer::RenderResult / ProjectionPainter::OcclusionMap that the
/// visibility test actually needs.
struct DepthView {
    QImage        depth;                ///< grayscale, near=bright / far=dark
    Ogre::Matrix4 viewProj;             ///< proj * view used to render `depth`
    Ogre::Vector3 camPosition = Ogre::Vector3::ZERO;
    Ogre::Vector3 camDirection = Ogre::Vector3::ZERO;   ///< normalised
    float depthNear = 0.0f;
    float depthFar = 0.0f;
    /// World-space slop before a texel counts as occluded. Without this a
    /// surface occludes ITSELF at grazing angles (depth acne) — the same
    /// problem, and the same fix, as ProjectionPainter's occlusion path.
    float biasWorld = 0.0f;
};

namespace DerivedMapOcclusion {

/// Is `worldPos` visible in `view`? False when it is behind the camera, falls
/// outside the depth image, or sits further along the camera axis than the
/// nearest recorded surface (plus `biasWorld`).
///
/// Distance is measured along the CAMERA AXIS (dot of (p - eye) with the view
/// direction), NOT Euclidean: the depth map encodes linear fog distance along
/// that axis, so a Euclidean comparison reads off-axis points as further than
/// they were encoded and self-occludes them.
bool isVisibleInView(const Ogre::Vector3& worldPos, const DepthView& view);

/// Fraction of `views` that CANNOT see `worldPos`, in 0..1 (1 = fully
/// occluded). Views whose direction faces away from `worldNormal` are skipped —
/// they are behind the surface, so counting them would darken every vertex by
/// roughly half regardless of geometry. If no view faces the normal, returns 0
/// (unoccluded) rather than a misleading 1.
float occlusionAt(const Ogre::Vector3& worldPos,
                  const Ogre::Vector3& worldNormal,
                  const std::vector<DepthView>& views);

/// Convenience: occlusion for every vertex, in the same order as the input.
/// `positions` and `normals` must be the same length.
std::vector<float> occlusionForVertices(const std::vector<Ogre::Vector3>& positions,
                                        const std::vector<Ogre::Vector3>& normals,
                                        const std::vector<DepthView>& views);

/// Evenly distributed unit directions over the sphere (Fibonacci lattice), used
/// as the set of view directions to render. `count` is clamped to >= 1.
/// Sphere rather than hemisphere because a mesh is sampled from all sides; the
/// per-vertex normal test then selects the relevant half for each vertex.
std::vector<Ogre::Vector3> sampleDirections(int count);

} // namespace DerivedMapOcclusion

#endif // DERIVEDMAPOCCLUSION_H
