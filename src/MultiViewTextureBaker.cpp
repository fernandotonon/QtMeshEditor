#include "MultiViewTextureBaker.h"

#include "VertexColorBaker.h"  // for dilate()
#include "EditableMesh.h"
#include "ProjectionMath.h"    // shared projectToViewportUV / sampleImage / Projected

#include <OgreEntity.h>
#include <OgreNode.h>
#include <OgreMatrix3.h>
#include <OgreMatrix4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// The projection/sampling math is shared with ProjectionPainter (#549); it now
// lives in ProjectionMath.h. Alias the names so the rest of this file (and its
// tests) is unchanged.
using ProjectionMath::Projected;
using ProjectionMath::projectToViewportUV;
using ProjectionMath::sampleImage;

// Per-channel mean of a view image over reasonably-opaque, non-near-white
// pixels (near-white is usually the generator's empty background, which would
// skew the target). Returns mean RGB in 0..1.
Ogre::Vector3 imageMeanRGB(const QImage& img)
{
    double sr = 0, sg = 0, sb = 0; long count = 0;
    const int w = img.width(), h = img.height();
    // Sample on a grid (cap work on large images) — mean is stable from a subset.
    const int step = std::max(1, (w * h) / (256 * 256));
    long idx = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x, ++idx) {
            if (idx % step) continue;
            const QRgb c = img.pixel(x, y);
            if (qAlpha(c) < 8) continue;
            if (qRed(c) > 247 && qGreen(c) > 247 && qBlue(c) > 247) continue; // bg
            sr += qRed(c); sg += qGreen(c); sb += qBlue(c); ++count;
        }
    }
    if (count == 0) return Ogre::Vector3(0.5f, 0.5f, 0.5f);
    return Ogre::Vector3(float(sr / count) / 255.0f,
                         float(sg / count) / 255.0f,
                         float(sb / count) / 255.0f);
}

// Shift `img`'s per-channel mean toward `target` (mean-matching). A gentle
// additive correction — robust and avoids the contrast over-stretch a full
// mean+std match can cause on flat textures.
QImage colorMatched(const QImage& img, const Ogre::Vector3& target)
{
    const Ogre::Vector3 m = imageMeanRGB(img);
    const float dr = (target.x - m.x) * 255.0f;
    const float dg = (target.y - m.y) * 255.0f;
    const float db = (target.z - m.z) * 255.0f;
    if (std::abs(dr) < 1.0f && std::abs(dg) < 1.0f && std::abs(db) < 1.0f)
        return img;  // already close — skip the copy
    QImage outImg = img.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < outImg.height(); ++y) {
        uchar* line = outImg.scanLine(y);
        for (int x = 0; x < outImg.width(); ++x) {
            uchar* p = line + x * 3;
            p[0] = static_cast<uchar>(std::clamp(p[0] + dr, 0.0f, 255.0f));
            p[1] = static_cast<uchar>(std::clamp(p[1] + dg, 0.0f, 255.0f));
            p[2] = static_cast<uchar>(std::clamp(p[2] + db, 0.0f, 255.0f));
        }
    }
    return outImg;
}

} // namespace

MultiViewTextureBaker::Report MultiViewTextureBaker::bake(
    const std::vector<Triangle>& tris,
    const std::vector<View>& views,
    TexturePaintBuffer& out)
{
    return bake(tris, views, out, Options{});
}

MultiViewTextureBaker::Report MultiViewTextureBaker::bake(
    const std::vector<Triangle>& tris,
    const std::vector<View>& views,
    TexturePaintBuffer& out,
    const Options& opts)
{
    Report rep;
    if (tris.empty()) { rep.error = QStringLiteral("no triangles"); return rep; }
    if (views.empty()) { rep.error = QStringLiteral("no views"); return rep; }
    for (const View& v : views) {
        if (v.image.isNull() || v.image.width() <= 0 || v.image.height() <= 0) {
            rep.error = QStringLiteral("a view image is empty");
            return rep;
        }
    }
    const int res = std::clamp(opts.resolution, 16, 8192);
    rep.perViewTriangleCount.assign(views.size(), 0);

    // Color-match later views to the first view's mean so independent diffusion
    // runs (front vs back) don't show a global hue/brightness jump at the seam.
    // Work on a local copy; the first view is the reference (unchanged).
    std::vector<View> matched;
    const std::vector<View>* useViews = &views;
    if (opts.colorMatchToFirstView && views.size() > 1) {
        const Ogre::Vector3 target = imageMeanRGB(views[0].image);
        matched = views;
        for (size_t i = 1; i < matched.size(); ++i)
            matched[i].image = colorMatched(matched[i].image, target);
        useViews = &matched;
    }
    const std::vector<View>& V = *useViews;

    // Weighted accumulation buffers (RGB * weight, and weight sum) per texel.
    const size_t n = static_cast<size_t>(res) * res;
    std::vector<float> accR(n, 0.0f), accG(n, 0.0f), accB(n, 0.0f), accW(n, 0.0f);

    // Rasterize one triangle in UV0 space into the accumulators, sampling
    // `img` at the per-texel barycentric-interpolated screen UV (s0,s1,s2),
    // weighted by `weight`.
    auto rasterTriIntoAcc = [&](const Ogre::Vector2& uv0, const Ogre::Vector2& uv1,
                                const Ogre::Vector2& uv2,
                                const Ogre::Vector2& s0, const Ogre::Vector2& s1,
                                const Ogre::Vector2& s2,
                                const QImage& img, float weight) -> int {
        // UV -> pixel centre coords. V is top-left origin already.
        auto toPix = [&](const Ogre::Vector2& uv) {
            return Ogre::Vector2(uv.x * res, uv.y * res);
        };
        const Ogre::Vector2 a = toPix(uv0), b = toPix(uv1), c = toPix(uv2);
        const float minXf = std::min({a.x, b.x, c.x});
        const float maxXf = std::max({a.x, b.x, c.x});
        const float minYf = std::min({a.y, b.y, c.y});
        const float maxYf = std::max({a.y, b.y, c.y});
        int x0 = std::max(0, static_cast<int>(std::floor(minXf)));
        int x1 = std::min(res - 1, static_cast<int>(std::ceil(maxXf)));
        int y0 = std::max(0, static_cast<int>(std::floor(minYf)));
        int y1 = std::min(res - 1, static_cast<int>(std::ceil(maxYf)));
        // Edge-function denominator (twice the signed area in pixel space).
        const float denom = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(denom) < 1e-9f) return 0;   // degenerate in UV space
        const float invDen = 1.0f / denom;
        int written = 0;
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                const float fx = px + 0.5f, fy = py + 0.5f;
                // Barycentric coords w.r.t. UV-space triangle.
                float l0 = ((b.y - c.y) * (fx - c.x) + (c.x - b.x) * (fy - c.y)) * invDen;
                float l1 = ((c.y - a.y) * (fx - c.x) + (a.x - c.x) * (fy - c.y)) * invDen;
                float l2 = 1.0f - l0 - l1;
                const float eps = -1e-4f;
                if (l0 < eps || l1 < eps || l2 < eps) continue;  // outside triangle
                // Interpolated screen UV → sample the view image.
                const Ogre::Vector2 suv = s0 * l0 + s1 * l1 + s2 * l2;
                const Ogre::ColourValue col = sampleImage(img, suv);
                const size_t idx = static_cast<size_t>(py) * res + px;
                accR[idx] += col.r * weight;
                accG[idx] += col.g * weight;
                accB[idx] += col.b * weight;
                accW[idx] += weight;
                ++written;
            }
        }
        return written;
    };

    for (const Triangle& t : tris) {
        Ogre::Vector3 nrm = t.normal;
        if (nrm.isZeroLength()) {
            nrm = (t.p[1] - t.p[0]).crossProduct(t.p[2] - t.p[0]);
        }
        if (nrm.isZeroLength()) continue;   // degenerate triangle
        nrm.normalise();

        for (size_t vi = 0; vi < V.size(); ++vi) {
            const View& v = V[vi];
            // Facing weight: the surface normal pointing back toward the camera
            // means -viewDir (camera looks ALONG camDirection into the scene).
            float facing = -nrm.dotProduct(v.camDirection);
            if (facing < opts.minFacing) continue;
            facing = std::pow(facing, std::max(0.01f, opts.facingPower));

            const Projected pa = projectToViewportUV(t.p[0], v.viewProj);
            const Projected pb = projectToViewportUV(t.p[1], v.viewProj);
            const Projected pc = projectToViewportUV(t.p[2], v.viewProj);
            if (pa.behind || pb.behind || pc.behind) continue;  // clipped

            const int w = rasterTriIntoAcc(t.uv[0], t.uv[1], t.uv[2],
                                           pa.uv, pb.uv, pc.uv, v.image, facing);
            if (w > 0) {
                ++rep.trianglesProjected;
                ++rep.perViewTriangleCount[vi];
            }
        }
    }

    // Resolve accumulators → RGBA8, build a coverage mask for dilation.
    out.resize(res, res);
    out.clear(opts.background);
    std::vector<uint8_t> coverage(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (accW[i] <= 0.0f) continue;
        const float inv = 1.0f / accW[i];
        const int x = static_cast<int>(i % res);
        const int y = static_cast<int>(i / res);
        out.setPixel(x, y, Ogre::ColourValue(std::clamp(accR[i] * inv, 0.0f, 1.0f),
                                             std::clamp(accG[i] * inv, 0.0f, 1.0f),
                                             std::clamp(accB[i] * inv, 0.0f, 1.0f),
                                             1.0f));
        coverage[i] = 1;
        ++rep.pixelsWritten;
    }

    if (opts.dilationPixels > 0 && rep.pixelsWritten > 0) {
        rep.pixelsDilated = VertexColorBaker::dilate(out, coverage, opts.dilationPixels);
    }

    rep.ok = true;
    return rep;
}

std::vector<MultiViewTextureBaker::Triangle>
MultiViewTextureBaker::fromEntity(Ogre::Entity* entity, QString* errorOut)
{
    std::vector<Triangle> out;
    if (!entity || !entity->getMesh()) {
        if (errorOut) *errorOut = QStringLiteral("null entity/mesh");
        return out;
    }

    EditableMesh em;
    if (!em.loadFromEntity(entity)) {
        if (errorOut) *errorOut = QStringLiteral("failed to read mesh geometry");
        return out;
    }

    // World transform of the entity's parent node (positions to world, normals
    // via the inverse-transpose to stay correct under non-uniform scale).
    Ogre::Matrix4 world = Ogre::Matrix4::IDENTITY;
    if (Ogre::Node* node = entity->getParentNode())
        world = Ogre::Matrix4(node->_getFullTransform());  // derived world matrix
    Ogre::Matrix3 world3;
    world.extract3x3Matrix(world3);
    Ogre::Matrix3 normalMat = world3.Inverse().Transpose();

    bool sawUv = false;
    for (const EditableSubMesh& sub : em.subMeshes()) {
        for (const EditableTriangle& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size()
                || tri.indices[1] >= sub.vertices.size()
                || tri.indices[2] >= sub.vertices.size())
                continue;
            Triangle t;
            for (int k = 0; k < 3; ++k) {
                const EditableVertex& v = sub.vertices[tri.indices[k]];
                t.p[k]  = world * v.position;       // world-space position
                t.uv[k] = v.uv;
                if (v.uv != Ogre::Vector2::ZERO) sawUv = true;
            }
            // World face normal from the geometry (robust even if per-vertex
            // normals are absent); fall back to the transformed vertex normal.
            Ogre::Vector3 fn = (t.p[1] - t.p[0]).crossProduct(t.p[2] - t.p[0]);
            if (fn.isZeroLength()) {
                fn = normalMat * sub.vertices[tri.indices[0]].normal;
            }
            t.normal = fn;
            out.push_back(t);
        }
    }

    if (out.empty()) {
        if (errorOut) *errorOut = QStringLiteral("mesh has no triangles");
        return out;
    }
    if (!sawUv) {
        // No usable UV0 → a bake would collapse onto a single texel. Fail hard
        // (return empty) so the caller aborts rather than producing garbage.
        // The slice-3 UV-unwrap gate will handle this case by unwrapping first.
        if (errorOut) *errorOut = QStringLiteral("mesh has no usable UV0 (run UV unwrap first)");
        out.clear();
        return out;
    }
    return out;
}
