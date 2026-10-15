#include "MeshGenCleanup.h"

#include "QuadRetopo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace MeshGenCleanup {

namespace {

struct TriKey {
    uint32_t a, b, c;   // sorted
    bool operator==(const TriKey& o) const { return a == o.a && b == o.b && c == o.c; }
};
struct TriKeyHash {
    size_t operator()(const TriKey& k) const
    {
        uint64_t h = k.a;
        h = h * 0x9E3779B97F4A7C15ull ^ k.b;
        h = h * 0x9E3779B97F4A7C15ull ^ k.c;
        return static_cast<size_t>(h ^ (h >> 29));
    }
};

bool zeroArea(const float* p0, const float* p1, const float* p2, float eps)
{
    const float e0[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const float e1[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    const float e2[3] = {p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]};
    const float cx = e0[1] * e1[2] - e0[2] * e1[1];
    const float cy = e0[2] * e1[0] - e0[0] * e1[2];
    const float cz = e0[0] * e1[1] - e0[1] * e1[0];
    const float twiceArea = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float l0 = e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2];
    const float l1 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
    const float l2 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
    const float longestSq = std::max({l0, l1, l2});
    if (longestSq <= 0.0f || !(twiceArea > 0.0f))
        return true;   // coincident corners (or NaN)
    // height over the longest edge = twiceArea / longest; compare to
    // eps * longest without the sqrt: twiceArea < eps * longest².
    return twiceArea < eps * longestSq;
}

} // namespace

DegenerateReport removeDegenerates(std::vector<float>& positions,
                                   std::vector<uint32_t>& indices,
                                   const std::vector<PerVertexStream>& perVertex,
                                   float relativeHeightEps)
{
    DegenerateReport rep;
    const size_t nv = positions.size() / 3;
    if (nv == 0 || indices.size() < 3)
        return rep;

    std::unordered_set<TriKey, TriKeyHash> seen;
    seen.reserve(indices.size() / 3);
    size_t w = 0;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= nv || b >= nv || c >= nv || a == b || b == c || a == c) {
            ++rep.repeatedIndex;
            continue;
        }
        if (zeroArea(&positions[size_t(a) * 3], &positions[size_t(b) * 3],
                     &positions[size_t(c) * 3], relativeHeightEps)) {
            ++rep.zeroArea;
            continue;
        }
        std::array<uint32_t, 3> s = {a, b, c};
        std::sort(s.begin(), s.end());
        if (!seen.insert(TriKey{s[0], s[1], s[2]}).second) {
            ++rep.duplicate;
            continue;
        }
        indices[w++] = a;
        indices[w++] = b;
        indices[w++] = c;
    }
    indices.resize(w);

    // Compact unreferenced vertices across every per-vertex stream.
    // Count first, remap second: renumbering the indices is only valid
    // together with reordering every stream, so a mesh with no orphans
    // must leave the indices untouched.
    std::vector<uint32_t> remap(nv, UINT32_MAX);
    uint32_t next = 0;
    for (uint32_t i : indices)
        if (remap[i] == UINT32_MAX)
            remap[i] = next++;
    if (next == nv)
        return rep;   // nothing orphaned: indices and streams already line up
    for (uint32_t& i : indices)
        i = remap[i];
    rep.removedVertices = static_cast<int>(nv - next);

    auto compactStream = [&](std::vector<float>& s, int comps) {
        if (comps <= 0 || s.size() != nv * size_t(comps))
            return;
        std::vector<float> out(size_t(next) * comps);
        for (size_t v = 0; v < nv; ++v) {
            if (remap[v] == UINT32_MAX)
                continue;
            std::memcpy(&out[size_t(remap[v]) * comps], &s[v * comps],
                        sizeof(float) * comps);
        }
        s.swap(out);
    };
    compactStream(positions, 3);
    for (const PerVertexStream& st : perVertex)
        if (st.data && st.data != &positions)
            compactStream(*st.data, st.components);
    return rep;
}

QuadReport pairIntoQuads(const std::vector<float>& positions,
                         const std::vector<uint32_t>& indices,
                         std::vector<std::vector<uint32_t>>& outFaces,
                         const QuadOptions& opts)
{
    QuadReport rep;
    outFaces.clear();
    const int nv = static_cast<int>(positions.size() / 3);
    const int nt = static_cast<int>(indices.size() / 3);
    rep.trianglesBefore = nt;
    if (nv < 3 || nt < 1)
        return rep;

    QuadRetopoOptions qo;
    qo.targetFaces = -1;   // pair everything that scores well
    qo.maxAngleDeg = opts.maxAngleDeg;
    qo.shapeToleranceDeg = opts.shapeToleranceDeg;
    qo.maxAspectRatio = opts.maxAspectRatio;
    std::vector<std::vector<unsigned int>> faces;
    const QuadRetopoReport qr = QuadRetopo::retopologizeMesh(
        positions.data(), nv,
        reinterpret_cast<const unsigned int*>(indices.data()), nt, qo, faces);
    if (!qr.error.isEmpty() || faces.empty())
        return rep;

    outFaces.reserve(faces.size());
    for (auto& f : faces) {
        if (f.size() == 4) ++rep.quads;
        else if (f.size() == 3) ++rep.triangles;
        else continue;
        outFaces.emplace_back(f.begin(), f.end());
    }
    rep.ok = true;
    return rep;
}

} // namespace MeshGenCleanup
