/*
-----------------------------------------------------------------------------------
This source file is part of QtMeshEditor.

Paint v2 Slice F (#549) — project an image onto a mesh's UV0 through a camera
and rasterize it into a paint buffer, with per-texel occlusion, a depth limit,
backface culling and a soft edge. Shared by the projection/stencil paint modes
and the decal tool.

Pure data (no Ogre scene / GL): inputs are world-space triangles + matrices +
QImages, so the core is unit-testable headlessly. Build the triangle list from
a live entity via MultiViewTextureBaker::fromEntity().

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef PROJECTIONPAINTER_H
#define PROJECTIONPAINTER_H

#include "MultiViewTextureBaker.h"   // reuse Triangle + fromEntity
#include "TexturePaintBuffer.h"

#include <QImage>
#include <QString>

#include <OgreColourValue.h>
#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <OgreVector3.h>

#include <vector>

class ProjectionPainter
{
public:
    /// One mesh triangle in WORLD space + UV0 + world normal — same layout the
    /// baker uses, so MultiViewTextureBaker::fromEntity() feeds this directly.
    using Triangle = MultiViewTextureBaker::Triangle;

    /// The camera the image is projected through (the live viewport camera, or
    /// a decal's orthographic frame).
    struct View {
        Ogre::Matrix4 viewProj;      ///< projMatrix * viewMatrix (world -> clip)
        Ogre::Vector3 camDirection;  ///< normalised dir the camera looks ALONG
        Ogre::Vector3 camPosition;   ///< world eye position
    };

    /// Per-texel visible-surface test. `depth` is a MeshDepthRenderer depth map
    /// (grayscale, near=bright/far=dark, LINEAR world distance via fog) rendered
    /// with `viewProj`; a texel is occluded when its camera distance exceeds the
    /// sampled surface distance by more than `biasWorld`.
    struct OcclusionMap {
        QImage        depth;
        Ogre::Matrix4 viewProj;      ///< proj*view used to render `depth`
        Ogre::Vector3 camPosition;   ///< eye the depth map was rendered from
        float         depthNear = 0.0f;
        float         depthFar  = 0.0f;
        float         biasWorld = 0.0f;   ///< slop (world units) to avoid depth acne
    };

    struct Options {
        int   resolution     = 0;      ///< 0 = keep the size of the `out` buffer
        bool  backfaceCull   = true;
        float minFacing      = 0.05f;  ///< grazing/back rejection (facing weight floor)
        float facingPower    = 0.5f;
        bool  useOcclusion   = false;
        float depthLimit     = 0.0f;   ///< 0 = off; reject texels this many WORLD units behind the nearest visible surface
        float softEdge       = 0.0f;   ///< 0..1 border feather in source-UV space
        float strength       = 1.0f;   ///< global alpha multiplier
        int   dilationPixels = 0;      ///< seam dilation after raster (0 = hard mask)
        Ogre::ColourValue tint = Ogre::ColourValue(1, 1, 1, 1);  ///< multiply the sampled colour
    };

    struct Report {
        bool ok = false;
        QString error;
        int texelsWritten    = 0;
        int texelsBackface   = 0;
        int texelsOccluded   = 0;
        int texelsDepthCulled = 0;
    };

    /// Project ONE image through `view` onto `tris` (UV0 space) into `out`.
    /// `out` is CLEARED to transparent first (it becomes a new layer's buffer).
    /// Each covered texel is composited src-over with
    /// alpha = facing * source.alpha * softEdge * strength, gated by occlusion +
    /// depth-limit. Returns a Report (ok=false + error on usage error).
    static Report project(const std::vector<Triangle>& tris,
                          const View& view,
                          const QImage& source,
                          TexturePaintBuffer& out,
                          const Options& opts,
                          const OcclusionMap* occ = nullptr);

    /// Per-dab variant for the stencil-brush stroke path: splat only the texels
    /// inside the brush footprint (centre `brushUv`, radius `brushRadiusUv` in
    /// UV units), each texel = `brushColor` weighted by the projected stencil
    /// alpha * round falloff * strength. ACCUMULATES into `out` (no clear).
    /// Returns the number of texels touched.
    static int projectDab(const std::vector<Triangle>& tris,
                          const View& view,
                          const QImage& stencil,
                          const Ogre::Vector2& brushUv,
                          float brushRadiusUv,
                          const Ogre::ColourValue& brushColor,
                          float strength,
                          TexturePaintBuffer& out,
                          const Options& opts,
                          const OcclusionMap* occ = nullptr);
};

#endif // PROJECTIONPAINTER_H
