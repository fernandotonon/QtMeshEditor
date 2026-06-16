#include "MultiViewTextureBaker.h"

#include "VertexColorBaker.h"  // for dilate()
#include "EditableMesh.h"

#include <OgreEntity.h>
#include <OgreNode.h>
#include <OgreMatrix3.h>
#include <OgreMatrix4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Project a world point through view*proj (clip space) and return the
// viewport UV in [0..1] (top-left origin, V down to match QImage). Sets
// `behind` when the point is at/behind the camera (clip w <= 0). The depth
// (NDC z) is returned for optional occlusion use later.
struct Projected {
    Ogre::Vector2 uv;   // [0..1], may be outside on off-screen verts
    float ndcZ = 0.0f;
    bool behind = false;
};

Projected projectToViewportUV(const Ogre::Vector3& worldP,
                              const Ogre::Matrix4& viewProj)
{
    Projected out;
    const Ogre::Vector4 clip = viewProj * Ogre::Vector4(worldP.x, worldP.y, worldP.z, 1.0f);
    if (clip.w <= 1e-6f) {        // at or behind the camera plane
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

// Bilinear sample of a QImage at uv in [0..1] (top-left origin). Returns the
// colour as ColourValue (0..1). Out-of-range uv is clamped to the edge.
Ogre::ColourValue sampleImage(const QImage& img, const Ogre::Vector2& uv)
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

        for (size_t vi = 0; vi < views.size(); ++vi) {
            const View& v = views[vi];
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
    if (!sawUv && errorOut) {
        // Not fatal here (caller decides) but flag it — a bake onto all-zero
        // UVs collapses to one texel.
        *errorOut = QStringLiteral("mesh has no usable UV0");
    }
    return out;
}
