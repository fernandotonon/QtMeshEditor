#ifndef MULTIVIEWTEXTUREBAKER_H
#define MULTIVIEWTEXTUREBAKER_H

#include "TexturePaintBuffer.h"

#include <QImage>
#include <QString>

#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <OgreVector3.h>

#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Project per-view generated images onto a mesh's UV0 atlas (slice 1 of
 *        the multi-view AI texture bake — see MULTIVIEW_TEXTURE_BAKE_DESIGN.md).
 *
 * The mesh's UV0 layout is FIXED. For every triangle, each view's camera
 * (view/projection matrix) projects the triangle's world-space vertices to
 * screen space; the triangle is then rasterized in UV0 space, and each covered
 * texel is coloured by sampling that view's image at the barycentric-
 * interpolated screen position. Views are blended by a facing weight
 * (max(0, -faceNormal . viewDir)^facingPower) so a front view dominates
 * front-facing triangles and a back view the back, with a feathered seam.
 *
 * Pure data: inputs are plain geometry arrays + QImages + matrices. No Ogre
 * scene-manager / GL state is touched, so the core is unit-testable headlessly.
 * Build an instance from an Ogre::Entity via MultiViewTextureBaker::fromEntity()
 * (defined in the .cpp) when wiring the live path.
 */
class MultiViewTextureBaker
{
public:
    /// One mesh triangle in WORLD space + its UV0 coordinates. `normal` is the
    /// world-space face normal (need not be normalised; the baker normalises).
    struct Triangle {
        Ogre::Vector3 p[3];     // world-space positions
        Ogre::Vector2 uv[3];    // UV0 in [0..1]^2 (top-left origin, V down)
        Ogre::Vector3 normal;   // world-space face normal
    };

    /// One generated view: the image plus the exact camera it was rendered
    /// with (matrices from MeshDepthRenderer::RenderResult).
    struct View {
        QImage        image;        // RGB(A) generated texture for this view
        Ogre::Matrix4 viewProj;     // projMatrix * viewMatrix (world -> clip)
        Ogre::Vector3 camDirection; // normalised dir camera looks ALONG (into scene)
    };

    struct Options {
        int   resolution     = 1024;  // output atlas size (square)
        int   dilationPixels = 4;     // seam-dilation passes after raster
        float facingPower    = 1.0f;  // exponent on the facing weight
        /// Triangles whose facing weight to a view is below this are not
        /// projected from that view (back-face / grazing rejection).
        float minFacing      = 0.05f;
        /// Background for texels no view covered (alpha 0 = transparent seam mask).
        Ogre::ColourValue background = Ogre::ColourValue(1.0f, 1.0f, 1.0f, 0.0f);
    };

    struct Report {
        bool ok = false;
        QString error;
        int pixelsWritten = 0;            // texels coloured by projection (pre-dilate)
        int pixelsDilated = 0;            // texels filled by seam dilation
        int trianglesProjected = 0;       // (triangle,view) pairs that contributed
        std::vector<int> perViewTriangleCount;  // contributing tris per view
    };

    /**
     * @brief Bake `views` onto `tris` (UV0 space) into `out`.
     *
     * `out` is resized to opts.resolution and filled with opts.background, then
     * projection-painted and dilated. Returns a Report; on a usage error
     * (no triangles / no views / empty images) ok=false and error is set.
     */
    static Report bake(const std::vector<Triangle>& tris,
                       const std::vector<View>& views,
                       TexturePaintBuffer& out,
                       const Options& opts);

    /// Convenience overload with default options.
    static Report bake(const std::vector<Triangle>& tris,
                       const std::vector<View>& views,
                       TexturePaintBuffer& out);

    /// Extract world-space triangles + UV0 from a live entity (positions and
    /// normals transformed by the entity's parent-node derived world matrix).
    /// Uses EditableMesh::loadFromEntity under the hood. Returns an empty
    /// vector if the entity/mesh is null or has no UV0. The `errorOut` (if
    /// given) is set on failure.
    static std::vector<Triangle> fromEntity(Ogre::Entity* entity,
                                            QString* errorOut = nullptr);
};

#endif // MULTIVIEWTEXTUREBAKER_H
