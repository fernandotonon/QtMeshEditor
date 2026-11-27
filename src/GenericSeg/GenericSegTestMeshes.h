#pragma once
// Synthetic meshes for the generic-segmentation tests (#1179). Test-only.

#include <array>
#include <cstdint>
#include <vector>

namespace GenericSegTest {

struct TestMesh {
    std::vector<float> positions;    // xyz
    std::vector<uint32_t> indices;
    std::vector<int> truth;          // per triangle: ground-truth part id

    int vertexCount() const { return int(positions.size() / 3); }
    int indexCount() const { return int(indices.size()); }
    int faceCount() const { return int(indices.size() / 3); }
    std::array<float, 3> centroid(int f) const
    {
        std::array<float, 3> c{0, 0, 0};
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) c[a] += positions[3 * indices[3 * f + k] + a] / 3.0f;
        return c;
    }
};

// Axis-aligned box, each side an n x n grid of quads, outward winding. Sides
// get their own vertices (split at the edges, like an exporter does), so the
// pipeline has to weld to see the box as connected. `partOf(cx, cy, cz)`
// assigns the ground-truth part from the triangle centroid.
template <typename PartFn>
void addBox(TestMesh& m, std::array<float, 3> lo, std::array<float, 3> hi, int n, PartFn partOf)
{
    // For each side: fixed axis, sign, and the two in-plane axes (u, v) chosen
    // so that u x v points outward.
    struct Side { int axis; bool positive; int u; int v; };
    const Side sides[6] = {{0, true, 1, 2}, {0, false, 2, 1}, {1, true, 2, 0},
                           {1, false, 0, 2}, {2, true, 0, 1}, {2, false, 1, 0}};
    for (const Side& s : sides) {
        const uint32_t base = uint32_t(m.positions.size() / 3);
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                float p[3];
                p[s.axis] = s.positive ? hi[s.axis] : lo[s.axis];
                p[s.u] = lo[s.u] + (hi[s.u] - lo[s.u]) * float(i) / n;
                p[s.v] = lo[s.v] + (hi[s.v] - lo[s.v]) * float(j) / n;
                m.positions.insert(m.positions.end(), {p[0], p[1], p[2]});
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const uint32_t a = base + j * (n + 1) + i, b = a + 1, c = a + (n + 1), d = c + 1;
                for (const auto& tri : {std::array<uint32_t, 3>{a, b, d}, std::array<uint32_t, 3>{a, d, c}}) {
                    m.indices.insert(m.indices.end(), tri.begin(), tri.end());
                    const int f = m.faceCount() - 1;
                    const auto ct = m.centroid(f);
                    m.truth.push_back(partOf(ct[0], ct[1], ct[2]));
                }
            }
    }
}

inline TestMesh makeBox(std::array<float, 3> lo, std::array<float, 3> hi, int n, int part = 0)
{
    TestMesh m;
    addBox(m, lo, hi, n, [part](float, float, float) { return part; });
    return m;
}

// A table: one top slab and four legs, five separate closed boxes.
inline TestMesh makeTable(int n = 6)
{
    TestMesh m;
    addBox(m, {-1.0f, 0.8f, -0.6f}, {1.0f, 1.0f, 0.6f}, n, [](float, float, float) { return 0; });
    int leg = 1;
    for (float x : {-0.85f, 0.75f})
        for (float z : {-0.5f, 0.4f}) {
            const int id = leg++;
            addBox(m, {x, -1.0f, z}, {x + 0.1f, 0.8f, z + 0.1f}, n, [id](float, float, float) { return id; });
        }
    return m;
}

// One connected bar along X, ground truth split at x = 0 (part 0 for x < 0).
inline TestMesh makeBar(int n = 8)
{
    TestMesh m;
    addBox(m, {-1.0f, -0.25f, -0.25f}, {1.0f, 0.25f, 0.25f}, n,
           [](float x, float, float) { return x < 0 ? 0 : 1; });
    return m;
}

} // namespace GenericSegTest
