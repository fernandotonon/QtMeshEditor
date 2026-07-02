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
    if (!sampler) {
        r.error = QStringLiteral("bake: no colour sampler.");
        return r;
    }
    const int texSize = std::clamp(opts.textureSize, 64, 8192);

    // ---- 1. xatlas unwrap ---------------------------------------------------
    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::MeshDecl decl;
    decl.vertexCount          = static_cast<uint32_t>(nv);
    decl.vertexPositionData   = positions.data();
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
        if (!sampler(queryPts.data() + start * 3, n, rgb.data())) {
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

} // namespace MeshGenBaker
