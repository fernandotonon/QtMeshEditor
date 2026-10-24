#include "Eyelashes.h"

#include <QBuffer>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace CharacterGen {
namespace {
using V3 = std::array<float, 3>;
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float len(const V3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
V3 unit(const V3& a) { const float l = len(a); return l > 1e-9f ? mul(a, 1.0f / l) : V3{0, 0, 1}; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

struct Loop {
    std::vector<uint32_t> vertices;
    V3 centroid{};
};

// Connected components of the opening's boundary edges (dropped triangle next to a kept one).
std::vector<Loop> openingLoops(const std::vector<float>& positions, const std::vector<int32_t>& faces, const std::vector<uint8_t>& dropped)
{
    std::unordered_map<uint64_t, std::pair<int, int>> edges;   // key -> (dropped faces, kept faces)
    const size_t nf = faces.size() / 3;
    for (size_t f = 0; f < nf; ++f) {
        const bool drop = f < dropped.size() && dropped[f];
        for (int k = 0; k < 3; ++k) {
            auto& e = edges[edgeKey(uint32_t(faces[f * 3 + size_t(k)]), uint32_t(faces[f * 3 + size_t((k + 1) % 3)]))];
            (drop ? e.first : e.second)++;
        }
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    for (const auto& kv : edges)
        if (kv.second.first > 0 && kv.second.second > 0) {
            const uint32_t a = uint32_t(kv.first >> 32), b = uint32_t(kv.first & 0xFFFFFFFFu);
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    std::vector<Loop> loops;
    std::unordered_map<uint32_t, bool> seen;
    for (const auto& kv : adj) {
        if (seen[kv.first]) continue;
        Loop loop;
        std::vector<uint32_t> stack{kv.first};
        seen[kv.first] = true;
        while (!stack.empty()) {
            const uint32_t v = stack.back();
            stack.pop_back();
            loop.vertices.push_back(v);
            for (uint32_t w : adj[v])
                if (!seen[w]) { seen[w] = true; stack.push_back(w); }
        }
        for (uint32_t v : loop.vertices)
            for (size_t k = 0; k < 3; ++k) loop.centroid[k] += positions[size_t(v) * 3 + k] / float(loop.vertices.size());
        loops.push_back(std::move(loop));
    }
    return loops;
}

float bump(float t) { const float d = (t - 0.62f) / 0.38f; return std::exp(-d * d); }

// One strip along `margin` (ordered inner -> outer canthus). `upper` curls up, otherwise down.
void addStrip(LashMesh& out, const std::vector<float>& positions, const std::vector<uint32_t>& margin, const V3& eyeCentre, bool upper,
              float length, float curl)
{
    if (margin.size() < 2 || length <= 0.0f) return;
    const uint32_t base = uint32_t(out.positions.size() / 3);
    const float n = float(margin.size() - 1);
    for (size_t i = 0; i < margin.size(); ++i) {
        const uint32_t src = margin[i];
        const V3 root{positions[size_t(src) * 3], positions[size_t(src) * 3 + 1], positions[size_t(src) * 3 + 2]};
        const V3 radial = unit(sub(root, eyeCentre));                 // away from the eyeball: forward and up/down along the lid
        const V3 up{0.0f, upper ? 1.0f : -1.0f, 0.0f};
        const V3 dir = unit(add(radial, mul(up, curl * 0.8f)));
        const float t = float(i) / n;
        const float l = length * (0.40f + 0.60f * bump(t));
        const V3 tip = add(root, mul(dir, l));
        for (const V3& p : {root, tip}) {
            out.positions.insert(out.positions.end(), p.begin(), p.end());
            out.normals.insert(out.normals.end(), radial.begin(), radial.end());
            out.sourceVertex.push_back(src);
        }
        out.uvs.push_back(t); out.uvs.push_back(1.0f);   // root
        out.uvs.push_back(t); out.uvs.push_back(0.0f);   // tip
    }
    for (size_t i = 0; i + 1 < margin.size(); ++i) {
        const uint32_t r0 = base + uint32_t(i) * 2, t0 = r0 + 1, r1 = r0 + 2, t1 = r0 + 3;
        out.indices.insert(out.indices.end(), {r0, r1, t0, r1, t1, t0});
    }
}
}  // namespace

LashMesh buildLashes(const std::vector<float>& positions, const std::vector<int32_t>& faces, const std::vector<uint8_t>& dropped,
                     const std::array<LashEye, 2>& eyes, const LashOptions& options)
{
    LashMesh out;
    if (positions.empty() || faces.size() < 3) return out;
    const std::vector<Loop> loops = openingLoops(positions, faces, dropped);
    if (loops.empty()) return out;
    for (int side = 0; side < 2; ++side) {
        if (!eyes[size_t(side)].seated) continue;
        const V3 centre = eyes[size_t(side)].centre;
        // the opening of THIS eye: the loop whose centroid is nearest to the eyeball centre (and big enough to be an eyelid)
        const Loop* best = nullptr;
        float bestD = 1e30f;
        for (const Loop& l : loops) {
            if (l.vertices.size() < 8) continue;
            const float d = len(sub(l.centroid, centre));
            if (d < bestD) { bestD = d; best = &l; }
        }
        if (!best || bestD > 0.04f) continue;   // 4 cm: not an eyelid opening
        // canthi = the extremes along x; the chord between them separates the upper lid from the lower lid
        auto px = [&](uint32_t v) { return positions[size_t(v) * 3]; };
        auto py = [&](uint32_t v) { return positions[size_t(v) * 3 + 1]; };
        const uint32_t vmin = *std::min_element(best->vertices.begin(), best->vertices.end(), [&](uint32_t a, uint32_t b) { return px(a) < px(b); });
        const uint32_t vmax = *std::max_element(best->vertices.begin(), best->vertices.end(), [&](uint32_t a, uint32_t b) { return px(a) < px(b); });
        const float x0 = px(vmin), x1 = px(vmax), y0 = py(vmin), y1 = py(vmax);
        if (x1 - x0 < 1e-5f) continue;
        std::vector<uint32_t> upper, lower;
        for (uint32_t v : best->vertices) {
            const float t = (px(v) - x0) / (x1 - x0);
            const float chordY = y0 + (y1 - y0) * t;
            (py(v) >= chordY ? upper : lower).push_back(v);
        }
        // inner canthus (nose side) first: the left eye (+X) has it at the smaller x
        const bool leftEye = side == 0;
        auto order = [&](std::vector<uint32_t>& v) {
            std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return leftEye ? px(a) < px(b) : px(a) > px(b); });
        };
        order(upper);
        order(lower);
        // the canthus vertices belong to both lids: the strips meet there
        if (!upper.empty() && !lower.empty()) {
            const uint32_t inner = leftEye ? vmin : vmax, outer = leftEye ? vmax : vmin;
            for (auto* v : {&upper, &lower}) {
                if (std::find(v->begin(), v->end(), inner) == v->end()) v->insert(v->begin(), inner);
                if (std::find(v->begin(), v->end(), outer) == v->end()) v->push_back(outer);
            }
        }
        const size_t before = out.indices.size();
        addStrip(out, positions, upper, centre, true, options.upperLength, options.curl);
        if (out.indices.size() > before) ++out.upperStrips;
        const size_t before2 = out.indices.size();
        addStrip(out, positions, lower, centre, false, options.lowerLength, options.curl);
        if (out.indices.size() > before2) ++out.lowerStrips;
    }
    return out;
}

QImage bakeLashTexture(const Rgb& colour, int width, int height, uint32_t seed)
{
    width = std::max(width, 64);
    height = std::max(height, 16);
    std::vector<float> alpha(size_t(width) * size_t(height), 0.0f);
    auto splat = [&](float cx, float cy, float r) {   // an anti-aliased disc, max-combined
        const int x0 = std::max(0, int(std::floor(cx - r - 1))), x1 = std::min(width - 1, int(std::ceil(cx + r + 1)));
        const int y0 = std::max(0, int(std::floor(cy - r - 1))), y1 = std::min(height - 1, int(std::ceil(cy + r + 1)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float d = std::sqrt((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) + (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy));
                float& a = alpha[size_t(y) * size_t(width) + size_t(x)];
                a = std::max(a, clamp01(r - d + 0.5f));
            }
    };
    uint32_t state = seed * 2654435761u + 12345u;
    auto rnd = [&state]() { state = state * 1664525u + 1013904223u; return float(state >> 8) / float(1u << 24); };
    const int strands = 34;
    const float rootBand = 0.18f;   // fraction of the height: the dense "lash line" at the root
    for (int k = 0; k < strands; ++k) {
        const float u0 = (float(k) + 0.25f + 0.5f * rnd()) / float(strands);
        const float lengthFrac = 0.78f + 0.22f * rnd();
        const float drift = (0.012f + 0.03f * rnd()) * (rnd() < 0.5f ? 1.0f : 1.4f);   // strands lean towards the outer canthus
        const float w0 = (0.55f + 0.35f * rnd()) * float(width) / float(strands) * 0.5f;   // root half width, px
        const int steps = 96;
        for (int s = 0; s <= steps; ++s) {
            const float t = float(s) / float(steps);
            const float u = u0 + drift * t * t;
            const float r = std::max(1.0f, w0 * std::pow(1.0f - t, 0.6f));
            splat(u * float(width), t * lengthFrac * float(height - 1) + 0.5f, r);   // the root is the TOP row (OBJ v = 1 after the importer's flip)
        }
    }
    // the root band: a continuous dark line along the lid margin (lashes are dense there), fading into the strands
    for (int y = 0; y < height; ++y) {
        const float band = clamp01((rootBand - float(y) / float(height - 1)) / rootBand);   // 1 at the root (top) row
        if (band <= 0.0f) continue;
        for (int x = 0; x < width; ++x) {
            float& a = alpha[size_t(y) * size_t(width) + size_t(x)];
            a = std::max(a, std::min(1.0f, band * 1.4f));
        }
    }
    QImage img(width, height, QImage::Format_ARGB32);
    const int r = int(std::lround(clamp01(colour[0]) * 255.0f)), g = int(std::lround(clamp01(colour[1]) * 255.0f)), b = int(std::lround(clamp01(colour[2]) * 255.0f));
    for (int y = 0; y < height; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) row[x] = qRgba(r, g, b, int(std::lround(alpha[size_t(y) * size_t(width) + size_t(x)] * 255.0f)));
    }
    return img;
}

QByteArray bakeLashTexturePng(const Rgb& colour, int width, int height, uint32_t seed)
{
    const QImage img = bakeLashTexture(colour, width, height, seed);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG")) return QByteArray();
    return bytes;
}

}  // namespace CharacterGen
