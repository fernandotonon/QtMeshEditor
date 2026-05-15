#include "VertexColorBaker.h"

#include <algorithm>
#include <cmath>

namespace {

inline float edgeFn(const Ogre::Vector2& a, const Ogre::Vector2& b, const Ogre::Vector2& c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

} // namespace

int VertexColorBaker::rasterizeTriangle(TexturePaintBuffer& buffer,
                                        const Ogre::Vector2& uv0,
                                        const Ogre::Vector2& uv1,
                                        const Ogre::Vector2& uv2,
                                        const Ogre::ColourValue& c0,
                                        const Ogre::ColourValue& c1,
                                        const Ogre::ColourValue& c2,
                                        std::vector<uint8_t>* outCoverage)
{
    const int W = buffer.width();
    const int H = buffer.height();
    if (W <= 0 || H <= 0) return 0;
    if (outCoverage && static_cast<int>(outCoverage->size()) != W * H)
        outCoverage = nullptr;

    // UV origin = top-left (matches TexturePaintBuffer::uvToPixel).
    auto toPix = [&](const Ogre::Vector2& uv) {
        return Ogre::Vector2(uv.x * W, uv.y * H);
    };

    const Ogre::Vector2 p0 = toPix(uv0);
    const Ogre::Vector2 p1 = toPix(uv1);
    const Ogre::Vector2 p2 = toPix(uv2);

    const float minX = std::min({p0.x, p1.x, p2.x});
    const float maxX = std::max({p0.x, p1.x, p2.x});
    const float minY = std::min({p0.y, p1.y, p2.y});
    const float maxY = std::max({p0.y, p1.y, p2.y});

    int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    int x1 = std::min(W, static_cast<int>(std::ceil(maxX)));
    int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    int y1 = std::min(H, static_cast<int>(std::ceil(maxY)));
    if (x0 >= x1 || y0 >= y1) return 0;

    const float area = edgeFn(p0, p1, p2);
    if (std::abs(area) < 1e-7f) return 0;
    const float invArea = 1.0f / area;
    const bool flip = area < 0.0f;

    int painted = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const Ogre::Vector2 sample(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            float w0 = edgeFn(p1, p2, sample);
            float w1 = edgeFn(p2, p0, sample);
            float w2 = edgeFn(p0, p1, sample);
            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float b0 = w0 * (flip ? -invArea : invArea);
            const float b1 = w1 * (flip ? -invArea : invArea);
            const float b2 = 1.0f - b0 - b1;
            const Ogre::ColourValue color(
                c0.r * b0 + c1.r * b1 + c2.r * b2,
                c0.g * b0 + c1.g * b1 + c2.g * b2,
                c0.b * b0 + c1.b * b1 + c2.b * b2,
                c0.a * b0 + c1.a * b1 + c2.a * b2);
            buffer.setPixel(x, y, color);
            if (outCoverage)
                (*outCoverage)[static_cast<size_t>(y) * W + x] = 1;
            ++painted;
        }
    }
    return painted;
}

int VertexColorBaker::dilate(TexturePaintBuffer& buffer,
                             std::vector<uint8_t>& coverage,
                             int iterations)
{
    const int W = buffer.width();
    const int H = buffer.height();
    if (W <= 0 || H <= 0 || iterations <= 0) return 0;
    if (static_cast<int>(coverage.size()) != W * H) return 0;

    int totalFlipped = 0;
    std::vector<uint8_t> nextCov(coverage);
    auto& pixels = buffer.data();
    std::vector<uint8_t> nextPixels(pixels);

    for (int it = 0; it < iterations; ++it) {
        int flippedThisPass = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int idx = y * W + x;
                if (coverage[idx]) continue;
                // Find first filled neighbor.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const int nidx = ny * W + nx;
                        if (!coverage[nidx]) continue;
                        const size_t srcOff = static_cast<size_t>(nidx) * 4u;
                        const size_t dstOff = static_cast<size_t>(idx) * 4u;
                        nextPixels[dstOff + 0] = pixels[srcOff + 0];
                        nextPixels[dstOff + 1] = pixels[srcOff + 1];
                        nextPixels[dstOff + 2] = pixels[srcOff + 2];
                        nextPixels[dstOff + 3] = pixels[srcOff + 3];
                        nextCov[idx] = 1;
                        ++flippedThisPass;
                        dx = 2; // break inner loops
                        dy = 2;
                    }
                }
            }
        }
        if (flippedThisPass == 0) break;
        pixels = nextPixels;
        coverage = nextCov;
        totalFlipped += flippedThisPass;
    }

    if (totalFlipped > 0)
        buffer.markDirty(0, 0, W, H);
    return totalFlipped;
}

int VertexColorBaker::bake(const EditableMesh& mesh, TexturePaintBuffer& buffer)
{
    return bake(mesh, buffer, Options{});
}

int VertexColorBaker::bake(const EditableMesh& mesh,
                           TexturePaintBuffer& buffer,
                           const Options& options)
{
    const int res = std::max(1, options.resolution);
    buffer.resize(res, res);
    buffer.clear(options.background);

    const auto& subs = mesh.subMeshes();
    if (subs.empty()) return 0;

    std::vector<uint8_t> coverage(static_cast<size_t>(res) * static_cast<size_t>(res), 0);

    int totalPainted = 0;
    for (const auto& sub : subs) {
        for (const auto& tri : sub.triangles) {
            if (tri.indices[0] >= sub.vertices.size() ||
                tri.indices[1] >= sub.vertices.size() ||
                tri.indices[2] >= sub.vertices.size())
                continue;
            const auto& v0 = sub.vertices[tri.indices[0]];
            const auto& v1 = sub.vertices[tri.indices[1]];
            const auto& v2 = sub.vertices[tri.indices[2]];
            if (!v0.hasUV || !v1.hasUV || !v2.hasUV)
                continue;
            Ogre::ColourValue c0 = v0.hasColor ? v0.color : Ogre::ColourValue::White;
            Ogre::ColourValue c1 = v1.hasColor ? v1.color : Ogre::ColourValue::White;
            Ogre::ColourValue c2 = v2.hasColor ? v2.color : Ogre::ColourValue::White;
            totalPainted += rasterizeTriangle(buffer, v0.uv, v1.uv, v2.uv,
                                              c0, c1, c2, &coverage);
        }
    }

    // Coverage is built directly by the rasterizer as it writes each
    // pixel — the old "differs from background" inference silently
    // dropped triangles whose interpolated color equalled the
    // background (e.g. vertex-colors==white on a white background).
    if (totalPainted > 0)
        dilate(buffer, coverage, options.dilationPixels);

    return totalPainted;
}
