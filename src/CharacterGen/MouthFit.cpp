#include "MouthFit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace CharacterGen {
namespace MouthFit {

namespace {

const Dna::Mesh* findMesh(const Dna::Rig& rig, const QString& name)
{
    for (const auto& m : rig.meshes)
        if (m.name == name) return &m;
    return nullptr;
}

std::vector<float> shaped(const Dna::Mesh& m, const QStringList& channels, const std::vector<std::pair<QString, float>>& shapes)
{
    std::vector<float> out = m.positions;
    for (const auto& s : shapes) {
        const int ch = channels.indexOf(s.first);
        if (ch < 0 || s.second == 0.0f) continue;
        for (const auto& t : m.blendShapeTargets) {
            if (t.channel != ch) continue;
            for (size_t k = 0; k < t.vertexIndices.size(); ++k)
                for (size_t c = 0; c < 3; ++c) out[size_t(t.vertexIndices[k]) * 3 + c] += t.deltas[k * 3 + c] * s.second;
        }
    }
    return out;
}

std::vector<uint32_t> triangulate(const Dna::Mesh& m)
{
    std::vector<uint32_t> tris;
    for (const auto& f : m.faces)
        for (size_t i = 1; i + 1 < f.size(); ++i)
            for (uint32_t c : {f[0], f[i], f[i + 1]}) tris.push_back(c < m.layouts.size() ? m.layouts[c].position : 0u);
    return tris;
}

}  // namespace

int rayCrossings(const std::vector<float>& P, const std::vector<uint32_t>& T, const float o[3], const float d[3])
{
    int n = 0;
    for (size_t t = 0; t + 2 < T.size(); t += 3) {
        const float* a = &P[size_t(T[t]) * 3];
        const float* b = &P[size_t(T[t + 1]) * 3];
        const float* c = &P[size_t(T[t + 2]) * 3];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
        const float u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        if ((e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv > 1e-6f) ++n;
    }
    return n;
}

namespace {
/// True when the triangle's bounding box overlaps the cube of half-size `r` around `c`.
bool triangleNear(const std::vector<float>& P, uint32_t a, uint32_t b, uint32_t c3, const float c[3], float r)
{
    const float* v[3] = {&P[size_t(a) * 3], &P[size_t(b) * 3], &P[size_t(c3) * 3]};
    for (int k = 0; k < 3; ++k) {
        const float lo = std::min({v[0][k], v[1][k], v[2][k]}), hi = std::max({v[0][k], v[1][k], v[2][k]});
        if (hi < c[k] - r || lo > c[k] + r) return false;
    }
    return true;
}
}  // namespace

namespace {
bool insideMajority(const std::vector<float>& skinPos, const std::vector<uint32_t>& tris, const float* p)
{
    // slightly skewed axes: a ray exactly along an axis can hit the shared edge of two triangles and be counted twice or not at all
    static const float dirs[3][3] = {{0.0123f, 0.0271f, -1.0f}, {-0.0197f, 0.0113f, 1.0f}, {0.0151f, 1.0f, -0.0223f}};
    int odd = 0;
    for (const auto& d : dirs) odd += rayCrossings(skinPos, tris, p, d) & 1;
    return odd >= 2;
}
}  // namespace

std::array<float, 3> clearanceShift(const Dna::Mesh& skin, const std::vector<float>& interior, float margin, float maxShift, int maxIterations, int stride,
                                    int* outsideBefore, int* outsideAfter, float nearRadius)
{
    std::array<float, 3> shift{0, 0, 0};
    if (outsideBefore) *outsideBefore = 0;
    if (outsideAfter) *outsideAfter = 0;
    if (interior.size() < 3 || skin.positions.empty()) return shift;
    float c[3] = {0, 0, 0};
    for (size_t i = 0; i + 2 < interior.size(); i += 3) for (int k = 0; k < 3; ++k) c[k] += interior[i + size_t(k)];
    for (float& v : c) v /= float(interior.size() / 3);
    const std::vector<uint32_t> all = triangulate(skin);
    // the skin near the mouth only: the rays leave through the face in front, the head above and the back of the skull
    std::vector<uint32_t> tris;
    std::vector<uint32_t> verts;
    for (size_t t = 0; t + 2 < all.size(); t += 3) {
        if (triangleNear(skin.positions, all[t], all[t + 1], all[t + 2], c, nearRadius)) {
            tris.insert(tris.end(), {all[t], all[t + 1], all[t + 2]});
            verts.insert(verts.end(), {all[t], all[t + 1], all[t + 2]});
        }
    }
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    const size_t step = size_t(std::max(1, stride)) * 3;
    for (int iter = 0; iter <= maxIterations; ++iter) {
        double dir[3] = {0, 0, 0};
        int outside = 0;
        for (size_t i = 0; i + 2 < interior.size(); i += step) {
            const float p[3] = {interior[i] + shift[0], interior[i + 1] + shift[1], interior[i + 2] + shift[2]};
            if (insideMajority(skin.positions, tris, p)) continue;
            ++outside;
            float best = std::numeric_limits<float>::max();
            const float* bp = nullptr;
            for (uint32_t v : verts) {
                const float* q = &skin.positions[size_t(v) * 3];
                const float dx = q[0] - p[0], dy = q[1] - p[1], dz = q[2] - p[2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < best) { best = d; bp = q; }
            }
            if (bp) for (int k = 0; k < 3; ++k) dir[k] += double(bp[k] - p[k]);
        }
        if (iter == 0 && outsideBefore) *outsideBefore = outside;
        if (outsideAfter) *outsideAfter = outside;
        if (outside == 0 || iter == maxIterations) break;
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len < 1e-9) break;
        const double mean = len / double(outside);   // distance to the skin, averaged over the outside vertices
        for (int k = 0; k < 3; ++k) shift[size_t(k)] = std::clamp(shift[size_t(k)] + float(dir[k] / len * (mean + double(margin))), -maxShift, maxShift);
    }
    return shift;
}

float chooseJawFollow(const Dna::Mesh& skin, const std::vector<float>& skinDelta, const std::vector<float>& interior, const std::vector<float>& interiorDelta,
                      const std::vector<float>& candidates, float fallback, std::vector<int>* outsideCounts, float nearRadius)
{
    if (outsideCounts) outsideCounts->clear();
    if (candidates.empty() || interior.size() != interiorDelta.size() || interior.size() < 3 || skinDelta.size() != skin.positions.size()) return fallback;
    // the vertices that really travel with the jaw (the rest are tested by the rest-pose clearance)
    std::vector<size_t> moving;
    for (size_t i = 0; i + 2 < interior.size(); i += 3)
        if (std::fabs(interiorDelta[i + 1]) > 0.002f && (moving.size() * 24 <= i / 3 + 24)) moving.push_back(i);
    if (moving.empty()) return fallback;
    float cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i + 2 < interior.size(); i += 3) { cx += interior[i]; cy += interior[i + 1]; cz += interior[i + 2]; }
    const float n = float(interior.size() / 3);
    cx /= n; cy /= n; cz /= n;
    const std::vector<uint32_t> all = triangulate(skin);
    // the skin near the mouth (the deformed skin moves by centimetres, the box is generous), and its two deformed copies: none of it
    // depends on the candidate factor
    std::vector<uint32_t> tris;
    for (size_t t = 0; t + 2 < all.size(); t += 3) {
        const float c[3] = {cx, cy, cz};
        if (triangleNear(skin.positions, all[t], all[t + 1], all[t + 2], c, nearRadius)) tris.insert(tris.end(), {all[t], all[t + 1], all[t + 2]});
    }
    std::vector<uint32_t> used(tris.begin(), tris.end());
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    const float weights[2] = {1.0f, 0.8f};
    std::vector<float> deformed[2] = {skin.positions, skin.positions};
    for (int wi = 0; wi < 2; ++wi)
        for (uint32_t v : used)
            for (size_t k = 0; k < 3; ++k) deformed[wi][size_t(v) * 3 + k] += skinDelta[size_t(v) * 3 + k] * weights[wi];
    float bestCount = std::numeric_limits<float>::max();
    float best = fallback;
    for (float f : candidates) {
        int count = 0;
        for (int wi = 0; wi < 2; ++wi) {
            const float w = weights[wi];
            for (size_t i : moving) {
                const float p[3] = {interior[i] + interiorDelta[i] * f * w, interior[i + 1] + interiorDelta[i + 1] * f * w, interior[i + 2] + interiorDelta[i + 2] * f * w};
                if (!insideMajority(deformed[wi], tris, p)) ++count;
            }
        }
        if (outsideCounts) outsideCounts->push_back(count);
        // strictly fewer wins; on a tie the earlier (closer to the fallback, listed first) candidate stays
        if (float(count) < bestCount) { bestCount = float(count); best = f; }
    }
    return best;
}

std::vector<Sample> measure(const Dna::Rig& rig, const std::vector<std::pair<QString, float>>& shapes, const QString& skinName, const QStringList& interiorNames)
{
    std::vector<Sample> out;
    const Dna::Mesh* skin = findMesh(rig, skinName);
    if (!skin) return out;
    const std::vector<uint32_t> allTris = triangulate(*skin);
    std::vector<std::pair<QString, float>> steps;
    steps.push_back({QString(), 0.0f});
    for (const auto& s : shapes) steps.push_back(s);
    for (const auto& step : steps) {
        Sample sample;
        sample.shape = step.first;
        sample.weight = step.second;
        const std::vector<std::pair<QString, float>> apply = step.first.isEmpty() ? std::vector<std::pair<QString, float>>{} : std::vector<std::pair<QString, float>>{step};
        const std::vector<float> skinPos = shaped(*skin, rig.blendShapeChannels, apply);
        // only the head's triangles matter for a ray from the mouth: keep those within 0.3 m above/below the interior's height
        float lo = 1e9f, hi = -1e9f;
        std::vector<std::vector<float>> interior;
        for (const QString& name : interiorNames)
            if (const Dna::Mesh* m = findMesh(rig, name)) {
                interior.push_back(shaped(*m, rig.blendShapeChannels, apply));
                for (size_t i = 1; i < interior.back().size(); i += 3) { lo = std::min(lo, interior.back()[i]); hi = std::max(hi, interior.back()[i]); }
            }
        std::vector<uint32_t> tris;
        for (size_t t = 0; t + 2 < allTris.size(); t += 3) {
            const float y0 = skinPos[size_t(allTris[t]) * 3 + 1], y1 = skinPos[size_t(allTris[t + 1]) * 3 + 1], y2 = skinPos[size_t(allTris[t + 2]) * 3 + 1];
            if (std::max({y0, y1, y2}) >= lo - 0.3f && std::min({y0, y1, y2}) <= hi + 0.3f) tris.insert(tris.end(), {allTris[t], allTris[t + 1], allTris[t + 2]});
        }
        // slightly skewed axes: a ray exactly along an axis can hit the shared edge of two triangles and be counted twice or not at all
        float centre[3] = {0, 0, 0};
        {
            float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
            for (const auto& pos : interior) for (size_t i = 0; i + 2 < pos.size(); i += 3) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], pos[i + size_t(k)]); mx[k] = std::max(mx[k], pos[i + size_t(k)]); }
            for (int k = 0; k < 3; ++k) centre[k] = 0.5f * (mn[k] + mx[k]);
        }
        bool firstOutside = true;
        for (const auto& pos : interior)
            for (size_t i = 0; i + 2 < pos.size(); i += 3) {
                ++sample.interiorVertices;
                if (insideMajority(skinPos, tris, &pos[i])) continue;
                ++sample.outside;
                for (int k = 0; k < 3; ++k) {
                    const float rel = pos[i + size_t(k)] - centre[k];
                    sample.outsideMin[k] = firstOutside ? rel : std::min(sample.outsideMin[k], rel);
                    sample.outsideMax[k] = firstOutside ? rel : std::max(sample.outsideMax[k], rel);
                }
                firstOutside = false;
                float best = std::numeric_limits<float>::max();
                for (size_t k = 0; k + 2 < skinPos.size(); k += 3) {
                    const float dx = skinPos[k] - pos[i], dy = skinPos[k + 1] - pos[i + 1], dz = skinPos[k + 2] - pos[i + 2];
                    best = std::min(best, dx * dx + dy * dy + dz * dz);
                }
                sample.worst = std::max(sample.worst, std::sqrt(best));
            }
        out.push_back(sample);
    }
    return out;
}

}  // namespace MouthFit
}  // namespace CharacterGen
