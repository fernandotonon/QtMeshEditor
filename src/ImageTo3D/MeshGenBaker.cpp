#include "MeshGenBaker.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace MeshGenBaker {

namespace {

inline uint8_t toByte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

Result bake(const std::vector<float>& positions,
            const std::vector<uint32_t>& indices,
            const ColorSampler& sampler,
            const Options& opts)
{
    Result r;
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3 || indices.size() % 3 != 0) {
        r.error = QStringLiteral("bake: empty/degenerate input mesh.");
        return r;
    }
    // Validate every index before it is used to address `positions` (in the
    // xatlas xref rebuild and the rasterizer's barycentric interpolation).
    for (uint32_t i : indices) {
        if (i >= nv) {
            r.error = QStringLiteral("bake: index out of range (%1 >= %2).")
                          .arg(i).arg(nv);
            return r;
        }
    }
    if (!sampler) {
        r.error = QStringLiteral("bake: no colour sampler.");
        return r;
    }
    const int texSize = std::clamp(opts.textureSize, 64, 8192);

    // ---- 1. xatlas unwrap ---------------------------------------------------
    // xatlas IGNORES every face whose 3D area is <= FLT_EPSILON (1.19e-7)
    // in ABSOLUTE units, and parks it at UV (0,0) outside every chart. A
    // generated mesh is ~1 unit across, so at 150k triangles (mean area
    // ~1e-5) ordinary small faces fall under that: measured 2054 vertices at
    // (0,0) on a 1024^3 TripoSplat goblin. Those faces get no texture and an
    // arbitrary tangent frame — the dark speckle after a GLB round trip.
    // Charting and packing are scale-invariant (texels per unit is derived
    // from the requested resolution), and the output maps back to the
    // ORIGINAL positions through xref, so unwrap a copy scaled to a ~1000
    // unit diagonal.
    float lo[3] = {positions[0], positions[1], positions[2]};
    float hi[3] = {positions[0], positions[1], positions[2]};
    for (size_t v = 0; v < nv; ++v)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], positions[3 * v + a]);
            hi[a] = std::max(hi[a], positions[3 * v + a]);
        }
    const float diag = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0])
                                 + (hi[1] - lo[1]) * (hi[1] - lo[1])
                                 + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    const float unwrapScale = diag > 0.0f ? 1000.0f / diag : 1.0f;
    std::vector<float> unwrapPositions(positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
        unwrapPositions[i] = positions[i] * unwrapScale;

    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexCount          = static_cast<uint32_t>(nv);
    decl.vertexPositionData   = unwrapPositions.data();
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.indexCount           = static_cast<uint32_t>(indices.size());
    decl.indexData            = indices.data();
    decl.indexFormat          = xatlas::IndexFormat::UInt32;
    const auto err = xatlas::AddMesh(atlas, decl);
    if (err != xatlas::AddMeshError::Success) {
        r.error = QStringLiteral("bake: xatlas::AddMesh failed: %1")
                      .arg(QString::fromLatin1(xatlas::StringForEnum(err)));
        xatlas::Destroy(atlas);
        return r;
    }
    xatlas::PackOptions pack;
    pack.resolution = static_cast<uint32_t>(texSize);
    pack.padding    = std::max(1, opts.dilatePx);
    pack.bilinear   = true;
    xatlas::Generate(atlas, /*chartOptions=*/{}, pack);
    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        r.error = QStringLiteral("bake: xatlas produced no atlas.");
        xatlas::Destroy(atlas);
        return r;
    }

    // ---- 2. Re-indexed mesh with UVs ---------------------------------------
    // xatlas may output an atlas larger than `resolution` (it guarantees a
    // *minimum*); the UVs are normalized against the actual atlas size and the
    // bake target follows it so texel density matches the pack.
    const xatlas::Mesh& xm = atlas->meshes[0];
    const int W = static_cast<int>(atlas->width);
    const int H = static_cast<int>(atlas->height);
    r.positions.resize(static_cast<size_t>(xm.vertexCount) * 3);
    r.uvs.resize(static_cast<size_t>(xm.vertexCount) * 2);
    for (uint32_t v = 0; v < xm.vertexCount; ++v) {
        const xatlas::Vertex& xv = xm.vertexArray[v];
        const size_t src = static_cast<size_t>(xv.xref) * 3;
        r.positions[v * 3 + 0] = positions[src + 0];
        r.positions[v * 3 + 1] = positions[src + 1];
        r.positions[v * 3 + 2] = positions[src + 2];
        r.uvs[v * 2 + 0] = xv.uv[0] / float(W);
        r.uvs[v * 2 + 1] = xv.uv[1] / float(H);
    }
    r.indices.assign(xm.indexArray, xm.indexArray + xm.indexCount);
    r.vertexCount   = static_cast<int>(xm.vertexCount);
    r.triangleCount = static_cast<int>(xm.indexCount / 3);

    // ---- 3. Rasterize charts, collect texel -> 3D-point queries -------------
    std::vector<int32_t>  texelOwner(static_cast<size_t>(W) * H, -1);
    std::vector<float>    queryPts;                     // xyz per covered texel
    std::vector<uint32_t> queryTexel;                   // texel linear index
    queryPts.reserve(static_cast<size_t>(W) * H / 2 * 3);
    queryTexel.reserve(static_cast<size_t>(W) * H / 2);

    for (uint32_t t = 0; t < xm.indexCount; t += 3) {
        const uint32_t i0 = xm.indexArray[t + 0];
        const uint32_t i1 = xm.indexArray[t + 1];
        const uint32_t i2 = xm.indexArray[t + 2];
        const float* uv0 = xm.vertexArray[i0].uv;
        const float* uv1 = xm.vertexArray[i1].uv;
        const float* uv2 = xm.vertexArray[i2].uv;
        // Triangle bounding box in texels (uv is already in atlas pixels).
        const int minX = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[0], uv1[0], uv2[0]}))));
        const int maxX = std::min(W - 1, static_cast<int>(std::ceil(
            std::max({uv0[0], uv1[0], uv2[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(
            std::min({uv0[1], uv1[1], uv2[1]}))));
        const int maxY = std::min(H - 1, static_cast<int>(std::ceil(
            std::max({uv0[1], uv1[1], uv2[1]}))));
        const float denom = (uv1[1] - uv2[1]) * (uv0[0] - uv2[0])
                          + (uv2[0] - uv1[0]) * (uv0[1] - uv2[1]);
        if (std::fabs(denom) < 1e-12f)
            continue;   // degenerate UV triangle
        const float inv = 1.0f / denom;
        const size_t p0 = static_cast<size_t>(xm.vertexArray[i0].xref) * 3;
        const size_t p1 = static_cast<size_t>(xm.vertexArray[i1].xref) * 3;
        const size_t p2 = static_cast<size_t>(xm.vertexArray[i2].xref) * 3;
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (texelOwner[lin] >= 0)
                    continue;   // xatlas charts don't overlap; first hit wins
                const float px = x + 0.5f, py = y + 0.5f;
                const float w0 = ((uv1[1] - uv2[1]) * (px - uv2[0])
                                + (uv2[0] - uv1[0]) * (py - uv2[1])) * inv;
                const float w1 = ((uv2[1] - uv0[1]) * (px - uv2[0])
                                + (uv0[0] - uv2[0]) * (py - uv2[1])) * inv;
                const float w2 = 1.0f - w0 - w1;
                // Small tolerance keeps texels whose centre sits a hair
                // outside a chart edge (the dilation pass would otherwise
                // fill them with a neighbour's colour anyway).
                const float eps = -0.001f;
                if (w0 < eps || w1 < eps || w2 < eps)
                    continue;
                texelOwner[lin] = 1;
                queryTexel.push_back(static_cast<uint32_t>(lin));
                queryPts.push_back(w0 * positions[p0 + 0] + w1 * positions[p1 + 0]
                                   + w2 * positions[p2 + 0]);
                queryPts.push_back(w0 * positions[p0 + 1] + w1 * positions[p1 + 1]
                                   + w2 * positions[p2 + 1]);
                queryPts.push_back(w0 * positions[p0 + 2] + w1 * positions[p1 + 2]
                                   + w2 * positions[p2 + 2]);
            }
        }
    }
    xatlas::Destroy(atlas);

    if (queryTexel.empty()) {
        // Honour the "no partial data on failure" contract.
        r = Result{};
        r.error = QStringLiteral("bake: no texels covered (unwrap failed?).");
        return r;
    }

    // ---- 4. Batch-sample colours through the callback ------------------------
    QImage tex(W, H, QImage::Format_RGB888);
    tex.fill(Qt::darkGray);
    const size_t total = queryTexel.size();
    const size_t chunk = opts.chunkPoints > 0
        ? static_cast<size_t>(opts.chunkPoints) : total;
    std::vector<float> rgb;
    for (size_t start = 0; start < total; start += chunk) {
        const size_t n = std::min(chunk, total - start);
        rgb.resize(n * 3);
        if (!sampler(queryPts.data() + start * 3, n, rgb.data())
            || (opts.progress
                && !opts.progress(static_cast<int>(start + n),
                                  static_cast<int>(total)))) {
            // Honour the "no partial data on failure" contract; typed cancel
            // flag so callers don't have to string-match the error.
            r = Result{};
            r.cancelled = true;
            r.error = QStringLiteral("cancelled");
            return r;
        }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t lin = queryTexel[start + i];
            uchar* line = tex.scanLine(static_cast<int>(lin / W));
            uchar* px   = line + (lin % W) * 3;
            px[0] = toByte(rgb[i * 3 + 0]);
            px[1] = toByte(rgb[i * 3 + 1]);
            px[2] = toByte(rgb[i * 3 + 2]);
        }
    }

    // ---- 5. Dilate chart borders so filtering doesn't bleed background ------
    // Each pass copies every uncovered texel that has a covered 8-neighbour.
    std::vector<uint8_t> covered(static_cast<size_t>(W) * H, 0);
    for (uint32_t lin : queryTexel) covered[lin] = 1;
    // Publish the PRE-dilation coverage (#1017): the dilated ring is smeared
    // border colour, so an inpaint seam-fill wants the true chart coverage.
    r.coverage = covered;
    for (int pass = 0; pass < opts.dilatePx; ++pass) {
        std::vector<uint8_t> next = covered;
        for (int y = 0; y < H; ++y) {
            uchar* line = tex.scanLine(y);
            for (int x = 0; x < W; ++x) {
                const size_t lin = static_cast<size_t>(y) * W + x;
                if (covered[lin])
                    continue;
                for (int dy = -1; dy <= 1 && !next[lin]; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                            continue;
                        const size_t nlin = static_cast<size_t>(sy) * W + sx;
                        if (!covered[nlin])
                            continue;
                        const uchar* src = tex.scanLine(sy) + sx * 3;
                        std::memcpy(line + x * 3, src, 3);
                        next[lin] = 1;
                        break;
                    }
                }
            }
        }
        covered.swap(next);
    }

    r.texture = std::move(tex);
    r.ok = true;
    return r;
}

std::vector<float> computeTangents(const std::vector<float>& positions,
                                   const std::vector<uint32_t>& indices,
                                   const std::vector<float>& uvs,
                                   const std::vector<float>& normals)
{
    const size_t nv = positions.size() / 3;
    std::vector<double> t(nv * 3, 0.0), b(nv * 3, 0.0);
    if (uvs.size() >= nv * 2) {
        for (size_t f = 0; f + 2 < indices.size(); f += 3) {
            const uint32_t i0 = indices[f], i1 = indices[f + 1], i2 = indices[f + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv)
                continue;
            double e1[3], e2[3];
            for (int a = 0; a < 3; ++a) {
                e1[a] = double(positions[3 * i1 + a]) - positions[3 * i0 + a];
                e2[a] = double(positions[3 * i2 + a]) - positions[3 * i0 + a];
            }
            const double du1 = double(uvs[2 * i1]) - uvs[2 * i0];
            const double dv1 = double(uvs[2 * i1 + 1]) - uvs[2 * i0 + 1];
            const double du2 = double(uvs[2 * i2]) - uvs[2 * i0];
            const double dv2 = double(uvs[2 * i2 + 1]) - uvs[2 * i0 + 1];
            const double r = du1 * dv2 - du2 * dv1;   // 2 x signed UV area
            if (r == 0.0 || !std::isfinite(r))
                continue;
            double ft[3], fb[3], lt = 0.0, lb = 0.0;
            for (int a = 0; a < 3; ++a) {
                ft[a] = (e1[a] * dv2 - e2[a] * dv1) / r;
                fb[a] = (e2[a] * du1 - e1[a] * du2) / r;
                lt += ft[a] * ft[a];
                lb += fb[a] * fb[a];
            }
            lt = std::sqrt(lt);
            lb = std::sqrt(lb);
            if (!(lt > 0.0) || !(lb > 0.0) || !std::isfinite(lt) || !std::isfinite(lb))
                continue;
            // Unit directions weighted by UV area, as Ogre does, so a sliver
            // cannot dominate its neighbours.
            const double w = std::abs(r) * 0.5;
            for (uint32_t v : {i0, i1, i2})
                for (int a = 0; a < 3; ++a) {
                    t[3 * v + a] += ft[a] / lt * w;
                    b[3 * v + a] += fb[a] / lb * w;
                }
        }
    }
    std::vector<float> out(nv * 4);
    for (size_t v = 0; v < nv; ++v) {
        double n[3] = {0.0, 0.0, 1.0};
        if (normals.size() >= 3 * (v + 1)) {
            const double len = std::sqrt(double(normals[3 * v]) * normals[3 * v]
                                         + double(normals[3 * v + 1]) * normals[3 * v + 1]
                                         + double(normals[3 * v + 2]) * normals[3 * v + 2]);
            if (len > 0.0)
                for (int a = 0; a < 3; ++a)
                    n[a] = normals[3 * v + a] / len;
        }
        double tv[3] = {t[3 * v], t[3 * v + 1], t[3 * v + 2]};
        double d = tv[0] * n[0] + tv[1] * n[1] + tv[2] * n[2];
        for (int a = 0; a < 3; ++a)
            tv[a] -= n[a] * d;
        double len = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
        if (!(len > 1e-12)) {
            // No usable UV gradient: any direction perpendicular to n.
            const double ax[3] = {std::abs(n[0]) < 0.9 ? 1.0 : 0.0,
                                  std::abs(n[0]) < 0.9 ? 0.0 : 1.0, 0.0};
            d = ax[0] * n[0] + ax[1] * n[1] + ax[2] * n[2];
            for (int a = 0; a < 3; ++a)
                tv[a] = ax[a] - n[a] * d;
            len = std::sqrt(tv[0] * tv[0] + tv[1] * tv[1] + tv[2] * tv[2]);
        }
        for (int a = 0; a < 3; ++a)
            out[4 * v + a] = float(tv[a] / len);
        // Handedness: does (n x t) point along the accumulated bitangent?
        const double c[3] = {n[1] * tv[2] - n[2] * tv[1], n[2] * tv[0] - n[0] * tv[2],
                             n[0] * tv[1] - n[1] * tv[0]};
        const double hb = c[0] * b[3 * v] + c[1] * b[3 * v + 1] + c[2] * b[3 * v + 2];
        out[4 * v + 3] = hb < 0.0 ? -1.0f : 1.0f;
    }
    return out;
}

} // namespace MeshGenBaker
