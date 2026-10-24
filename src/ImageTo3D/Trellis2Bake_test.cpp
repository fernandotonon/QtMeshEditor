// Unit tests for Trellis2Bake — the QtMeshEditor-native game-ready processing
// + multi-channel PBR baker that replaces the upstream nvdiffrast/nvdiffrec
// path (docs/trellis2-dependencies.md). Pure data — no Ogre/GL/ONNX/Python.
#include "Trellis2Bake.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>

namespace {

// Append a unit-ish cube (12 tris) centred at (cx,cy,cz) with half-size h.
void appendCube(std::vector<float>& pos, std::vector<uint32_t>& idx,
                float cx, float cy, float cz, float h)
{
    const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
    const float v[8][3] = {
        {cx - h, cy - h, cz - h}, {cx + h, cy - h, cz - h},
        {cx + h, cy + h, cz - h}, {cx - h, cy + h, cz - h},
        {cx - h, cy - h, cz + h}, {cx + h, cy - h, cz + h},
        {cx + h, cy + h, cz + h}, {cx - h, cy + h, cz + h}};
    for (const auto& p : v) { pos.push_back(p[0]); pos.push_back(p[1]); pos.push_back(p[2]); }
    const uint32_t f[12][3] = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4}, {2, 3, 7}, {2, 7, 6},
        {1, 2, 6}, {1, 6, 5}, {3, 0, 4}, {3, 4, 7}};
    for (const auto& t : f) {
        idx.push_back(base + t[0]);
        idx.push_back(base + t[1]);
        idx.push_back(base + t[2]);
    }
}

// A dense volume block filled with a constant attribute row.
struct ConstVolume {
    std::vector<uint32_t> coords;
    std::vector<uint8_t> attrs;
    Trellis2Bake::SparseVolumeSampler sampler;
    void build(int res, const uint8_t row[6], float voxelSize,
               const float origin[3])
    {
        coords.clear();
        attrs.clear();
        for (int x = 0; x < res; ++x)
            for (int y = 0; y < res; ++y)
                for (int z = 0; z < res; ++z) {
                    coords.push_back(x); coords.push_back(y); coords.push_back(z);
                    for (int c = 0; c < 6; ++c) attrs.push_back(row[c]);
                }
        sampler.build(coords.data(), attrs.data(),
                      static_cast<int>(coords.size() / 3), voxelSize, origin);
    }
};

} // namespace

// ---- closestPointOnTriangle --------------------------------------------------

TEST(Trellis2BakeTest, ClosestPointOnTriangleRegions)
{
    const float a[3] = {0, 0, 0}, b[3] = {1, 0, 0}, c[3] = {0, 1, 0};
    float cp[3], bc[3];

    // Interior projection.
    const float pIn[3] = {0.25f, 0.25f, 1.0f};
    Trellis2Bake::closestPointOnTriangle(a, b, c, pIn, cp, bc);
    EXPECT_NEAR(cp[0], 0.25f, 1e-5f);
    EXPECT_NEAR(cp[1], 0.25f, 1e-5f);
    EXPECT_NEAR(cp[2], 0.0f, 1e-5f);
    EXPECT_NEAR(bc[0] + bc[1] + bc[2], 1.0f, 1e-5f);

    // Vertex region.
    const float pV[3] = {-1.0f, -1.0f, 0.0f};
    Trellis2Bake::closestPointOnTriangle(a, b, c, pV, cp, bc);
    EXPECT_NEAR(bc[0], 1.0f, 1e-5f);

    // Edge region (edge ab).
    const float pE[3] = {0.5f, -2.0f, 0.0f};
    Trellis2Bake::closestPointOnTriangle(a, b, c, pE, cp, bc);
    EXPECT_NEAR(cp[0], 0.5f, 1e-5f);
    EXPECT_NEAR(cp[1], 0.0f, 1e-5f);
    EXPECT_NEAR(bc[2], 0.0f, 1e-5f);
}

// ---- SparseVolumeSampler -------------------------------------------------------

TEST(Trellis2BakeTest, VolumeSamplerTrilinearAndFallback)
{
    // Two voxels along +x: red at (0,0,0), green at (1,0,0).
    const uint32_t coords[6] = {0, 0, 0, 1, 0, 0};
    const uint8_t attrs[12] = {255, 0, 0, 0, 255, 255,
                               0, 255, 0, 255, 0, 255};
    const float origin[3] = {0, 0, 0};
    Trellis2Bake::SparseVolumeSampler s;
    s.build(coords, attrs, 2, 1.0f, origin);

    float out[6];
    // At the centre of voxel 0 → exactly red.
    const float p0[3] = {0.5f, 0.5f, 0.5f};
    EXPECT_TRUE(s.sample(p0, out));
    EXPECT_NEAR(out[0], 1.0f, 1e-3f);
    EXPECT_NEAR(out[1], 0.0f, 1e-3f);
    // Halfway between the two centres → 50/50 blend.
    const float pMid[3] = {1.0f, 0.5f, 0.5f};
    EXPECT_TRUE(s.sample(pMid, out));
    EXPECT_NEAR(out[0], 0.5f, 2e-2f);
    EXPECT_NEAR(out[1], 0.5f, 2e-2f);
    // A couple of voxels away → nearest-occupied fallback still answers.
    const float pFar[3] = {3.2f, 0.5f, 0.5f};
    EXPECT_TRUE(s.sample(pFar, out));
    EXPECT_NEAR(out[1], 1.0f, 1e-3f);   // nearest is the green voxel
    // Nowhere near anything → neutral defaults, false.
    const float pNo[3] = {50.0f, 50.0f, 50.0f};
    EXPECT_FALSE(s.sample(pNo, out));
    EXPECT_NEAR(out[4], 0.8f, 1e-4f);   // default roughness
}

// ---- makeGameReady -------------------------------------------------------------

TEST(Trellis2BakeTest, GameReadyWeldsAndDropsDebris)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendCube(pos, idx, 0, 0, 0, 0.5f);          // main body, 12 tris
    appendCube(pos, idx, 3.0f, 0, 0, 0.01f);      // tiny floating debris
    // Duplicate the main cube's vertices by re-appending an identical cube in
    // place — every position collides, so welding should fuse them.
    appendCube(pos, idx, 0, 0, 0, 0.5f);

    // Defaults: threshold = max(16, 0.002×36) = 16 → the 12-triangle debris
    // cube is below it and gets dropped; the 24-tri welded main body stays.
    Trellis2Bake::GameReadyOptions opts;
    const auto r = Trellis2Bake::makeGameReady(pos, idx, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_GT(r.weldedVertices, 0);
    EXPECT_EQ(r.removedComponents, 1);            // the debris cube
    // The duplicated cube's triangles collapse onto the same welded verts and
    // survive as duplicate faces (dedup is not this pass's job), but the
    // debris' 12 triangles must be gone.
    EXPECT_EQ(r.outputTriangles, 24);
    EXPECT_EQ(r.positions.size() % 3, 0u);
    for (uint32_t i : r.indices)
        EXPECT_LT(i, r.positions.size() / 3);
}

TEST(Trellis2BakeTest, GameReadySimplifiesTowardTarget)
{
    // A tessellated plane: 32x32 quads = 2048 tris.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const int n = 33;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            pos.push_back(x / float(n - 1));
            pos.push_back(y / float(n - 1));
            pos.push_back(0.0f);
        }
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }

    Trellis2Bake::GameReadyOptions opts;
    opts.targetTriangles = 64;
    opts.simplifyTargetError = 0.5f;   // flat plane — everything collapsible
    const auto r = Trellis2Bake::makeGameReady(pos, idx, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_LT(r.outputTriangles, 300);            // dramatically reduced
    EXPECT_GE(r.outputTriangles, 2);
    EXPECT_EQ(r.inputTriangles, 2048);
}

// A platform preset (Roblox 4k / 20k) needs the count GUARANTEED under the
// target. The ordinary pass tolerates landing up to 2x over budget when the
// topology-preserving simplifier stalls; strict mode must not.
TEST(Trellis2BakeTest, GameReadyStrictBudgetIsAHardCeiling)
{
    // One connected surface: a free 32x32 plane (2048 tris — QEM takes it
    // down to a couple of triangles) plus a "book" of 60 pages hinged on ONE
    // edge of the plane's right border. That edge is then shared by 61
    // triangles: non-manifold, so meshoptimizer locks its two vertices, and a
    // border vertex may not collapse onto a locked one — the pages are frozen
    // under QEM. Target 40: the ordinary pass ends at ~65 tris — over budget
    // but under the 2x line where it already falls back to the sloppy
    // simplifier — which is exactly the window the strict flag closes. (The
    // book hangs off the plane rather than floating beside it because the
    // sloppy simplifier is a vertex-clustering grid: on disconnected pieces
    // its count jumps straight from "too many" to zero, whereas one surface
    // thins smoothly — the shape a real generation has.)
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const int n = 33;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            pos.insert(pos.end(), {x / float(n - 1), y / float(n - 1), 0.0f});
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    // Spine = the right-border edge between rows 16 and 17 at x = 1.
    const uint32_t spineA = 16 * n + (n - 1);
    const uint32_t spineB = 17 * n + (n - 1);
    const float spineY = 16.5f / float(n - 1);
    const int pages = 60;
    for (int k = 0; k < pages; ++k) {
        const float ang = 6.2831853f * k / pages;
        const uint32_t tip = static_cast<uint32_t>(pos.size() / 3);
        pos.insert(pos.end(), {1.0f + std::cos(ang), spineY, std::sin(ang)});
        idx.insert(idx.end(), {spineA, spineB, tip});
    }
    ASSERT_EQ(idx.size() / 3, static_cast<size_t>(2048 + pages));

    Trellis2Bake::GameReadyOptions soft;
    soft.targetTriangles = 40;
    soft.minComponentTriangles = 1;    // keep both pieces
    soft.minComponentFraction = 0.0f;
    const auto rs = Trellis2Bake::makeGameReady(pos, idx, soft);
    ASSERT_TRUE(rs.ok) << rs.error.toStdString();
    // The soft pass stalls on the frozen book: over budget, under 2x.
    EXPECT_GT(rs.outputTriangles, 40);
    EXPECT_LE(rs.outputTriangles, 80);

    Trellis2Bake::GameReadyOptions strict = soft;
    strict.strictTriangleBudget = true;
    const auto rt = Trellis2Bake::makeGameReady(pos, idx, strict);
    ASSERT_TRUE(rt.ok) << rt.error.toStdString();
    EXPECT_LE(rt.outputTriangles, 40);
    EXPECT_GE(rt.outputTriangles, 1);
    for (uint32_t i : rt.indices)
        EXPECT_LT(i, rt.positions.size() / 3);
}

// The mirror image of the case above: two DISCONNECTED pieces (the book
// floating beside the plane). QEM stalls at ~62, and the clustering fallback
// jumps straight to zero — the ceiling is unreachable, and a strict pass must
// say so rather than return ok with an over-budget or empty mesh.
TEST(Trellis2BakeTest, GameReadyStrictBudgetFailsWhenUnreachable)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const int n = 33;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            pos.insert(pos.end(), {x / float(n - 1), y / float(n - 1), 0.0f});
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    const uint32_t spineA = static_cast<uint32_t>(pos.size() / 3);
    pos.insert(pos.end(), {3.0f, 0.0f, 0.0f});
    pos.insert(pos.end(), {3.0f, 1.0f, 0.0f});
    const int pages = 60;
    for (int k = 0; k < pages; ++k) {
        const float ang = 6.2831853f * k / pages;
        const uint32_t tip = static_cast<uint32_t>(pos.size() / 3);
        pos.insert(pos.end(), {3.0f + std::cos(ang), 0.5f, std::sin(ang)});
        idx.insert(idx.end(), {spineA, spineA + 1, tip});
    }

    Trellis2Bake::GameReadyOptions soft;
    soft.targetTriangles = 40;
    soft.minComponentTriangles = 1;
    soft.minComponentFraction = 0.0f;
    const auto rs = Trellis2Bake::makeGameReady(pos, idx, soft);
    ASSERT_TRUE(rs.ok) << rs.error.toStdString();
    EXPECT_GT(rs.outputTriangles, 40);   // the soft pass tolerates this

    Trellis2Bake::GameReadyOptions strict = soft;
    strict.strictTriangleBudget = true;
    const auto rt = Trellis2Bake::makeGameReady(pos, idx, strict);
    EXPECT_FALSE(rt.ok);
    EXPECT_TRUE(rt.error.contains("strict triangle budget")) << rt.error.toStdString();
    EXPECT_TRUE(rt.error.contains("40")) << rt.error.toStdString();
}

TEST(Trellis2BakeTest, GameReadyRejectsGarbage)
{
    EXPECT_FALSE(Trellis2Bake::makeGameReady({}, {}, {}).ok);
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> bad = {0, 1, 9};
    EXPECT_FALSE(Trellis2Bake::makeGameReady(pos, bad, {}).ok);
}

// ---- bake ------------------------------------------------------------------------

TEST(Trellis2BakeTest, BakeTransfersVolumeAttributesAndFlatNormal)
{
    // Source = target = one cube; constant gold-ish metallic volume covering it.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendCube(pos, idx, 0.5f, 0.5f, 0.5f, 0.5f);   // cube spanning [0,1]^3

    const uint8_t row[6] = {255, 204, 51, 230, 64, 255};  // rgb, metal, rough, alpha
    const float origin[3] = {-0.5f, -0.5f, -0.5f};
    ConstVolume vol;
    vol.build(8, row, 0.25f, origin);   // 8^3 voxels of size 0.25 → covers [-0.5,1.5]

    Trellis2Bake::BakeOptions opts;
    opts.textureSize = 128;
    opts.dilatePx = 2;
    const auto r = Trellis2Bake::bake(pos, idx, pos, idx, vol.sampler, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_GT(r.vertexCount, 0);
    EXPECT_EQ(r.uvs.size(), static_cast<size_t>(r.vertexCount) * 2);
    ASSERT_FALSE(r.baseColor.isNull());
    ASSERT_FALSE(r.roughness.isNull());
    ASSERT_FALSE(r.metallic.isNull());
    ASSERT_FALSE(r.normalMap.isNull());

    // Sample the centre texel of some covered chart: hunt for a pixel whose
    // basecolor matches the constant volume row (most of the atlas should).
    int matches = 0, covered = 0;
    for (int y = 0; y < r.baseColor.height(); y += 4) {
        for (int x = 0; x < r.baseColor.width(); x += 4) {
            const QColor c = r.baseColor.pixelColor(x, y);
            if (c.alpha() == 0)
                continue;
            ++covered;
            if (std::abs(c.red() - 255) <= 2 && std::abs(c.green() - 204) <= 2
                && std::abs(c.blue() - 51) <= 2)
                ++matches;
        }
    }
    ASSERT_GT(covered, 0);
    EXPECT_GT(matches, covered / 2);

    // Metallic/roughness lanes carry the constant values on covered texels.
    bool sawMetal = false;
    for (int y = 0; y < r.metallic.height() && !sawMetal; ++y)
        for (int x = 0; x < r.metallic.width(); ++x)
            if (std::abs(int(r.metallic.scanLine(y)[x]) - 230) <= 2) {
                sawMetal = true;
                break;
            }
    EXPECT_TRUE(sawMetal);

    // Source == target ⇒ the baked detail normal is the flat (128,128,255)
    // tangent-space "up" on face interiors. Count near-flat texels.
    int flat = 0, normCovered = 0;
    for (int y = 0; y < r.normalMap.height(); y += 4) {
        for (int x = 0; x < r.normalMap.width(); x += 4) {
            const QColor a = r.baseColor.pixelColor(x, y);
            if (a.alpha() == 0)
                continue;
            const uchar* p = r.normalMap.scanLine(y) + size_t(x) * 3;
            ++normCovered;
            if (std::abs(int(p[0]) - 128) <= 6 && std::abs(int(p[1]) - 128) <= 6
                && p[2] >= 240)
                ++flat;
        }
    }
    ASSERT_GT(normCovered, 0);
    EXPECT_GT(flat, normCovered * 3 / 4);
}

TEST(Trellis2BakeTest, DetailNormalUsesTheViewportBitangentConvention)
{
    // The viewport shader rebuilds the bitangent from Ogre's tangent parity,
    // which TangentSpaceCalc defines as the REVERSE of the textbook glTF sign
    // ("the 'V' texture coordinate is actually left handed"). The baked detail
    // normal has to be expressed in that frame or every chart is lit as if its
    // relief were flipped along V (dark smears on jaws, shoulders, folds).
    // Target = a flat quad, source = the same quad tilted about a DIAGONAL axis
    // (xatlas picks the chart axes arbitrarily, so the tilt must show up in both
    // the tangent and the bitangent channel): the baked green channel must equal
    // the tilt projected on Ogre's bitangent.
    const std::vector<float> tpos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<uint32_t> tidx = {0, 1, 2, 0, 2, 3};
    const float th = 30.0f * 3.14159265f / 180.0f;
    const float k0 = 0.70710678f, k1 = 0.70710678f;           // axis (1,1,0)/sqrt2
    // Rodrigues rotation of (x,y,0) about k by th
    auto rot = [&](float x, float y, float out[3]) {
        const float c = std::cos(th), sn = std::sin(th);
        const float kx = k0 * x + k1 * y;                        // k . v
        // k x v = (k1*0 - 0*y, 0*x - k0*0, k0*y - k1*x)
        const float cx = 0.0f, cy = 0.0f, cz = k0 * y - k1 * x;
        out[0] = x * c + cx * sn + k0 * kx * (1 - c);
        out[1] = y * c + cy * sn + k1 * kx * (1 - c);
        out[2] = 0.0f * c + cz * sn;
    };
    // The source is a finely subdivided plane (a dense scan, not two huge
    // triangles — the closest-point grid is sized from triangle density).
    constexpr int kN = 24;
    std::vector<float> spos;
    std::vector<uint32_t> sidx;
    for (int j = 0; j <= kN; ++j)
        for (int i = 0; i <= kN; ++i) {
            float q[3];
            rot(float(i) / kN, float(j) / kN, q);
            spos.insert(spos.end(), {q[0], q[1], q[2]});
        }
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            const uint32_t a0 = uint32_t(j * (kN + 1) + i), b0 = a0 + 1,
                           c0 = a0 + kN + 1, d0 = c0 + 1;
            sidx.insert(sidx.end(), {a0, b0, d0, a0, d0, c0});
        }
    // rotated plane normal: z -> z*cos + (k x z)*sin, k x z = (k1, -k0, 0)
    const float Ns[3] = {k1 * std::sin(th), -k0 * std::sin(th), std::cos(th)};

    const uint8_t row[6] = {255, 204, 51, 230, 64, 255};
    const float origin[3] = {-0.5f, -0.5f, -0.5f};
    ConstVolume vol;
    vol.build(8, row, 0.25f, origin);
    Trellis2Bake::BakeOptions opts;
    opts.textureSize = 64;
    const auto r = Trellis2Bake::bake(tpos, tidx, spos, sidx, vol.sampler, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.normalMap.isNull());

    // First output triangle: T = dP/du, V = dP/dv, from the baked positions/UVs.
    const uint32_t i0 = r.indices[0], i1 = r.indices[1], i2 = r.indices[2];
    auto P = [&](uint32_t i, int k) { return r.positions[i * 3 + k]; };
    auto U = [&](uint32_t i, int k) { return r.uvs[i * 2 + k]; };
    float e1[3], e2[3];
    for (int k = 0; k < 3; ++k) { e1[k] = P(i1, k) - P(i0, k); e2[k] = P(i2, k) - P(i0, k); }
    const float du1 = U(i1, 0) - U(i0, 0), dv1 = U(i1, 1) - U(i0, 1);
    const float du2 = U(i2, 0) - U(i0, 0), dv2 = U(i2, 1) - U(i0, 1);
    const float det = du1 * dv2 - du2 * dv1;
    ASSERT_GT(std::fabs(det), 1e-12f);
    float T[3], V[3];
    for (int k = 0; k < 3; ++k) {
        T[k] = (e1[k] * dv2 - e2[k] * dv1) / det;     // dP/du
        V[k] = (e2[k] * du1 - e1[k] * du2) / det;     // dP/dv
    }
    auto len = [](const float* x) { return std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); };
    for (int k = 0; k < 3; ++k) T[k] /= len(T);
    const float N[3] = {0, 0, 1};
    const float cNT[3] = {N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2],
                          N[0] * T[1] - N[1] * T[0]};
    const float cTV[3] = {T[1] * V[2] - T[2] * V[1], T[2] * V[0] - T[0] * V[2],
                          T[0] * V[1] - T[1] * V[0]};
    // Ogre: parity = (cross(U,V) . N >= 0) ? -1 : +1; viewport B = cross(N,T) * parity
    const float parity = (cTV[0] * N[0] + cTV[1] * N[1] + cTV[2] * N[2]) >= 0.0f ? -1.0f : 1.0f;
    float B[3];
    for (int k = 0; k < 3; ++k) B[k] = cNT[k] * parity;
    const float expY = Ns[0] * B[0] + Ns[1] * B[1] + Ns[2] * B[2];
    ASSERT_GT(std::fabs(expY), 0.2f);   // the tilt must be visible in G

    // Texel at the triangle's UV centroid (no V flip in the bake).
    const float cu = (U(i0, 0) + U(i1, 0) + U(i2, 0)) / 3.0f;
    const float cv = (U(i0, 1) + U(i1, 1) + U(i2, 1)) / 3.0f;
    const int px = std::clamp(int(cu * r.normalMap.width()), 0, r.normalMap.width() - 1);
    const int py = std::clamp(int(cv * r.normalMap.height()), 0, r.normalMap.height() - 1);
    const uchar* g = r.normalMap.scanLine(py) + size_t(px) * 3;
    const float gotY = (float(g[1]) / 255.0f) * 2.0f - 1.0f;
    EXPECT_NEAR(gotY, expY, 0.08f)
        << "G=" << int(g[1]) << " expected for Ogre's bitangent: " << expY
        << " (textbook sign would give " << -expY << ")";
}

TEST(Trellis2BakeTest, BakeHonoursCancellation)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendCube(pos, idx, 0.5f, 0.5f, 0.5f, 0.5f);
    const uint8_t row[6] = {200, 200, 200, 0, 128, 255};
    const float origin[3] = {-0.5f, -0.5f, -0.5f};
    ConstVolume vol;
    vol.build(4, row, 0.5f, origin);

    Trellis2Bake::BakeOptions opts;
    opts.textureSize = 512;
    opts.progress = [](int, int) { return false; };   // cancel immediately
    const auto r = Trellis2Bake::bake(pos, idx, pos, idx, vol.sampler, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
}

TEST(Trellis2BakeTest, BakeRejectsBadInput)
{
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> idx = {0, 1, 2};
    Trellis2Bake::SparseVolumeSampler empty;
    EXPECT_FALSE(Trellis2Bake::bake({}, {}, pos, idx, empty, {}).ok);
    const std::vector<uint32_t> oob = {0, 1, 7};
    EXPECT_FALSE(Trellis2Bake::bake(pos, oob, pos, idx, empty, {}).ok);
    EXPECT_FALSE(Trellis2Bake::bake(pos, idx, pos, oob, empty, {}).ok);
}

TEST(Trellis2BakeTest, DetailNormalIdentityIsFlat)
{
    // A unit quad, target == source, planar UVs: the detail map must be flat
    // (128,128,255) on every covered texel.
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    const std::vector<float> uvs = {0, 0, 1, 0, 1, 1, 0, 1};

    const auto r = Trellis2Bake::bakeDetailNormal(pos, idx, uvs, 64, 64,
                                                  pos, idx, {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.normalMap.isNull());
    int flat = 0, total = 0;
    for (int y = 4; y < 60; y += 4) {
        for (int x = 4; x < 60; x += 4) {
            const uchar* p = r.normalMap.constScanLine(y) + size_t(x) * 3;
            ++total;
            if (std::abs(int(p[0]) - 128) <= 2 && std::abs(int(p[1]) - 128) <= 2
                && p[2] >= 250)
                ++flat;
        }
    }
    EXPECT_EQ(flat, total);
}

TEST(Trellis2BakeTest, DetailNormalEncodesSourceRelief)
{
    // Target = flat quad; source = the same quad "tented" along its middle
    // (centre row of vertices raised). The baked texels must tilt away from
    // flat where the source normal disagrees with the target normal.
    const std::vector<float> tpos = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<uint32_t> tidx = {0, 1, 2, 0, 2, 3};
    const std::vector<float> tuvs = {0, 0, 1, 0, 1, 1, 0, 1};
    // Source: 2x1 strip with a ridge at x=0.5 raised by z=0.15.
    const std::vector<float> spos = {
        0, 0, 0,   0.5f, 0, 0.15f,   1, 0, 0,
        0, 1, 0,   0.5f, 1, 0.15f,   1, 1, 0};
    const std::vector<uint32_t> sidx = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4};

    const auto r = Trellis2Bake::bakeDetailNormal(tpos, tidx, tuvs, 64, 64,
                                                  spos, sidx, {});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    int tilted = 0;
    for (int y = 4; y < 60; y += 4)
        for (int x = 4; x < 60; x += 4) {
            const uchar* p = r.normalMap.constScanLine(y) + size_t(x) * 3;
            if (std::abs(int(p[0]) - 128) > 8)   // red channel = tangent-x tilt
                ++tilted;
        }
    EXPECT_GT(tilted, 20);

    // And the (opt-in) source-normal field smoothing actually smooths: the
    // same bake with smoothing passes must tilt strictly fewer texels —
    // this fixture's single-edge-scale ridge is exactly the "noise" scale
    // the option exists to flatten on raw dual-grid sources.
    Trellis2Bake::BakeOptions bo;
    bo.sourceNormalSmoothIterations = 8;
    const auto rs = Trellis2Bake::bakeDetailNormal(tpos, tidx, tuvs, 64, 64,
                                                   spos, sidx, bo);
    ASSERT_TRUE(rs.ok) << rs.error.toStdString();
    int tiltedSmoothed = 0;
    for (int y = 4; y < 60; y += 4)
        for (int x = 4; x < 60; x += 4) {
            const uchar* p = rs.normalMap.constScanLine(y) + size_t(x) * 3;
            if (std::abs(int(p[0]) - 128) > 8)
                ++tiltedSmoothed;
        }
    EXPECT_LT(tiltedSmoothed, tilted);
}

TEST(Trellis2BakeTest, DetailNormalRejectsBadInput)
{
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> idx = {0, 1, 2};
    const std::vector<float> uvs = {0, 0, 1, 0, 0, 1};
    EXPECT_FALSE(Trellis2Bake::bakeDetailNormal({}, {}, {}, 64, 64, pos, idx, {}).ok);
    EXPECT_FALSE(Trellis2Bake::bakeDetailNormal(pos, idx, {0, 0}, 64, 64,
                                                pos, idx, {}).ok);   // uv size
    EXPECT_FALSE(Trellis2Bake::bakeDetailNormal(pos, idx, uvs, 2, 2,
                                                pos, idx, {}).ok);   // atlas size
    const std::vector<uint32_t> oob = {0, 1, 9};
    EXPECT_FALSE(Trellis2Bake::bakeDetailNormal(pos, oob, uvs, 64, 64,
                                                pos, idx, {}).ok);
    EXPECT_FALSE(Trellis2Bake::bakeDetailNormal(pos, idx, uvs, 64, 64,
                                                pos, oob, {}).ok);
}

TEST(Trellis2BakeTest, UnifyWindingFixesFlippedPatchesAndOrientsOutward)
{
    // A cube with half its faces deliberately flipped must come out fully
    // consistent AND outward (positive signed volume).
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendCube(pos, idx, 0, 0, 0, 0.5f);
    for (size_t t = 0; t < idx.size(); t += 6)          // flip every other tri
        std::swap(idx[t + 1], idx[t + 2]);

    const int flips = Trellis2Bake::unifyWinding(pos, idx);
    EXPECT_GT(flips, 0);

    // Consistency: every interior directed edge appears exactly once.
    std::map<std::pair<uint32_t, uint32_t>, int> dir;
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k)
            ++dir[{idx[t + k], idx[t + (k + 1) % 3]}];
    for (const auto& e : dir)
        EXPECT_EQ(e.second, 1);

    // Outward: positive signed volume.
    double vol = 0.0;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const float* a = &pos[idx[t] * 3];
        const float* b = &pos[idx[t + 1] * 3];
        const float* c = &pos[idx[t + 2] * 3];
        vol += a[0] * (b[1] * c[2] - b[2] * c[1])
             + a[1] * (b[2] * c[0] - b[0] * c[2])
             + a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    EXPECT_GT(vol, 0.0);
}

namespace {
constexpr double kPi = 3.14159265358979323846;   // M_PI is not defined under MSVC

// UV sphere of radius r (closed, outward winding), appended to pos/idx.
void appendSphere(std::vector<float>& pos, std::vector<uint32_t>& idx, float r,
                  int rings = 40, int segs = 56)
{
    const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
    pos.insert(pos.end(), {0.f, r, 0.f});
    for (int i = 1; i < rings; ++i) {
        const float phi = float(kPi) * i / rings;
        for (int s = 0; s < segs; ++s) {
            const float th = 2.f * float(kPi) * s / segs;
            pos.insert(pos.end(), {r * std::sin(phi) * std::cos(th), r * std::cos(phi),
                                   r * std::sin(phi) * std::sin(th)});
        }
    }
    pos.insert(pos.end(), {0.f, -r, 0.f});
    const uint32_t south = uint32_t(pos.size() / 3 - 1);
    auto v = [&](int i, int s) { return base + uint32_t(1 + (i - 1) * segs + (s % segs)); };
    for (int s = 0; s < segs; ++s) {
        idx.insert(idx.end(), {base, v(1, s + 1), v(1, s)});
        idx.insert(idx.end(), {south, v(rings - 1, s), v(rings - 1, s + 1)});
    }
    for (int i = 1; i < rings - 1; ++i)
        for (int s = 0; s < segs; ++s) {
            idx.insert(idx.end(), {v(i, s), v(i, s + 1), v(i + 1, s)});
            idx.insert(idx.end(), {v(i, s + 1), v(i + 1, s + 1), v(i + 1, s)});
        }
}
} // namespace

TEST(Trellis2BakeTest, RemeshMakesDoubleWalledShellManifold)
{
    // Two concentric spheres closer than a voxel: a pinched double wall.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f);
    appendSphere(pos, idx, 0.995f);
    std::vector<float> rp;
    std::vector<uint32_t> ri;
    ASSERT_TRUE(Trellis2Bake::remeshManifold(pos, idx, 64, rp, ri));
    const auto s = Trellis2Bake::topologyStats(rp.size() / 3, ri);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.flippedEdges, 0);
    EXPECT_EQ(s.components, 1);
    // outward winding: signed volume positive and close to the sphere's
    double vol = 0;
    for (size_t t = 0; t + 2 < ri.size(); t += 3) {
        const float* a = &rp[ri[t] * 3]; const float* b = &rp[ri[t + 1] * 3];
        const float* c = &rp[ri[t + 2] * 3];
        vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    EXPECT_NEAR(vol, 4.0 / 3.0 * kPi, 0.5);   // + ~half a voxel of outward bias
}

TEST(Trellis2BakeTest, RemeshKeepsNarrowGapBetweenParts)
{
    // Two closed boxes ("fingers") a few cells apart must stay two separate
    // manifold components: the remesh may not bridge the gap.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    auto addBox = [&](float x0, float x1) {
        const uint32_t b = uint32_t(pos.size() / 3);
        const float y0 = 0.f, y1 = 0.6f, z0 = 0.f, z1 = 0.3f;
        const float v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                               {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
        for (const auto& p : v)
            pos.insert(pos.end(), {p[0], p[1], p[2]});
        const uint32_t f[12][3] = {{0, 3, 2}, {0, 2, 1}, {4, 5, 6}, {4, 6, 7},
                                   {0, 1, 5}, {0, 5, 4}, {2, 3, 7}, {2, 7, 6},
                                   {1, 2, 6}, {1, 6, 5}, {0, 4, 7}, {0, 7, 3}};
        for (const auto& t : f)
            idx.insert(idx.end(), {b + t[0], b + t[1], b + t[2]});
    };
    addBox(0.0f, 0.2f);
    addBox(0.22f, 0.42f);   // 0.02 gap; the grid cell is ~0.0094 at res 64
    std::vector<float> rp;
    std::vector<uint32_t> ri;
    ASSERT_TRUE(Trellis2Bake::remeshManifold(pos, idx, 64, rp, ri));
    const auto s = Trellis2Bake::topologyStats(rp.size() / 3, ri);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.components, 2) << "the gap between the parts was bridged";
    // and the surface stays within ~one cell of the source box faces
    float minX = 1e9f, maxX = -1e9f;
    for (size_t v = 0; v < rp.size() / 3; ++v) {
        minX = std::min(minX, rp[v * 3]);
        maxX = std::max(maxX, rp[v * 3]);
    }
    EXPECT_NEAR(minX, 0.0f, 0.012f);
    EXPECT_NEAR(maxX, 0.42f, 0.012f);
}

TEST(Trellis2BakeTest, RemeshKeepsThinSheetAsClosedSlab)
{
    // A flat 1x1 sheet (no thickness) must survive the remesh as a thin closed
    // slab instead of vanishing under the iso level.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const int n = 32;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            pos.insert(pos.end(), {float(x) / n, 0.0f, float(y) / n});
    // the sheet is open, which the flood fill tolerates (it only needs the
    // exterior to stay connected around it)
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = uint32_t(y * (n + 1) + x), b = a + 1, c = a + n + 1, d = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    std::vector<float> rp;
    std::vector<uint32_t> ri;
    ASSERT_TRUE(Trellis2Bake::remeshManifold(pos, idx, 48, rp, ri));
    const auto s = Trellis2Bake::topologyStats(rp.size() / 3, ri);
    EXPECT_GT(s.triangles, 100);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.nonManifoldEdges, 0);
}

TEST(Trellis2BakeTest, SimplifyManifoldNeverCollapsesBelowATetrahedron)
{
    // Every edge of a closed tetrahedron passes the link test, but collapsing
    // one leaves two coincident faces — the floor must stop it.
    std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<uint32_t> idx = {0, 2, 1, 0, 1, 3, 1, 2, 3, 0, 3, 2};
    ASSERT_TRUE(Trellis2Bake::simplifyManifold(pos, idx, 1));
    EXPECT_EQ(idx.size(), 12u);
    const auto s = Trellis2Bake::topologyStats(pos.size() / 3, idx);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.nonManifoldEdges, 0);
}

TEST(Trellis2BakeTest, SimplifyManifoldKeepsClosedManifold)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f, 60, 80);
    ASSERT_TRUE(Trellis2Bake::simplifyManifold(pos, idx, 800));
    const auto s = Trellis2Bake::topologyStats(pos.size() / 3, idx);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.flippedEdges, 0);
    EXPECT_EQ(s.components, 1);
    EXPECT_LE(s.triangles, 820);
    EXPECT_GE(s.triangles, 400);
}

TEST(Trellis2BakeTest, SimplifyManifoldStopsAtCostCap)
{
    // With a tiny error cap the pass must stop early (above the target)
    // instead of forcing destructive collapses to reach the count.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f, 60, 80);
    std::vector<float> pos2 = pos;
    std::vector<uint32_t> idx2 = idx;
    ASSERT_TRUE(Trellis2Bake::simplifyManifold(pos, idx, 100, 1e-12));
    const auto capped = Trellis2Bake::topologyStats(pos.size() / 3, idx);
    ASSERT_TRUE(Trellis2Bake::simplifyManifold(pos2, idx2, 100));
    const auto free = Trellis2Bake::topologyStats(pos2.size() / 3, idx2);
    EXPECT_GT(capped.triangles, free.triangles * 2);
    EXPECT_EQ(capped.boundaryEdges, 0);
    EXPECT_EQ(capped.nonManifoldEdges, 0);
}

namespace {
// Closed tetrahedron with outward faces whatever the vertex order.
void appendTetra(std::vector<float>& pos, std::vector<uint32_t>& idx,
                 const float p[4][3], const std::vector<uint32_t>& sharedWith = {})
{
    const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
    uint32_t id[4];
    for (int i = 0; i < 4; ++i) {
        if (!sharedWith.empty() && sharedWith[i] != UINT32_MAX) {
            id[i] = sharedWith[i];
        } else {
            id[i] = static_cast<uint32_t>(pos.size() / 3);
            pos.insert(pos.end(), {p[i][0], p[i][1], p[i][2]});
        }
    }
    (void)base;
    float c[3] = {0, 0, 0};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k)
            c[k] += p[i][k] / 4.0f;
    const int f[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : f) {
        const float* a = p[t[0]]; const float* b = p[t[1]]; const float* d = p[t[2]];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
        const float out = n[0] * (a[0] - c[0]) + n[1] * (a[1] - c[1]) + n[2] * (a[2] - c[2]);
        if (out >= 0)
            idx.insert(idx.end(), {id[t[0]], id[t[1]], id[t[2]]});
        else
            idx.insert(idx.end(), {id[t[0]], id[t[2]], id[t[1]]});
    }
}
} // namespace

TEST(Trellis2BakeTest, SplitNonManifoldSeparatesTwoBodiesSharingAnEdge)
{
    // Two solids touching along one edge: that edge has 4 faces. Splitting
    // must leave two clean closed bodies — no holes, no moved vertices.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const float a[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float b[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    appendTetra(pos, idx, a);
    appendTetra(pos, idx, b, {0, 1, UINT32_MAX, UINT32_MAX});
    ASSERT_EQ(Trellis2Bake::topologyStats(pos.size() / 3, idx).nonManifoldEdges, 1);
    const size_t trisBefore = idx.size() / 3;
    const std::vector<float> posBefore = pos;
    EXPECT_GT(Trellis2Bake::splitNonManifold(pos, idx), 0);
    const auto s = Trellis2Bake::topologyStats(pos.size() / 3, idx);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.components, 2);
    EXPECT_EQ(idx.size() / 3, trisBefore);
    // original vertices keep their positions (only copies are appended)
    for (size_t i = 0; i < posBefore.size(); ++i)
        EXPECT_EQ(pos[i], posBefore[i]);
}

TEST(Trellis2BakeTest, SplitNonManifoldSeparatesTwoBodiesSharingAVertex)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    const float a[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const float b[4][3] = {{0, 0, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    appendTetra(pos, idx, a);
    appendTetra(pos, idx, b, {0, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    EXPECT_GT(Trellis2Bake::splitNonManifold(pos, idx), 0);
    const auto s = Trellis2Bake::topologyStats(pos.size() / 3, idx);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.components, 2);
}

TEST(Trellis2BakeTest, SplitNonManifoldLeavesAManifoldMeshAlone)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f, 20, 30);
    const auto before = idx;
    EXPECT_EQ(Trellis2Bake::splitNonManifold(pos, idx), 0);
    EXPECT_EQ(idx, before);
}

TEST(Trellis2BakeTest, CreaseAwareNormalsIgnoreTheOppositeSheet)
{
    // A double-sided triangle: two faces with opposite winding on the same three
    // vertices (a thin slab collapsed flat). The original vertices average +Z
    // and -Z into nothing; each output vertex (one per face, as after an xatlas
    // chart split) must keep its own face's normal.
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> targetIdx = {0, 1, 2, 0, 2, 1};
    const std::vector<uint32_t> xref = {0, 1, 2, 0, 1, 2};   // output -> original
    const std::vector<uint32_t> outIdx = {0, 1, 2, 3, 5, 4};
    const auto n = Trellis2Bake::creaseAwareVertexNormals(pos, targetIdx, xref, outIdx, 6);
    ASSERT_EQ(n.size(), 18u);
    for (int v = 0; v < 3; ++v) {
        EXPECT_NEAR(n[v * 3 + 2], 1.0f, 1e-5f) << "top vertex " << v;
        EXPECT_NEAR(n[(v + 3) * 3 + 2], -1.0f, 1e-5f) << "bottom vertex " << v;
    }
}

TEST(Trellis2BakeTest, CreaseAwareNormalsStillSmoothAcrossASeam)
{
    // Two coplanar triangles whose shared vertices are split (a chart seam):
    // the seam must stay smooth, i.e. both copies get the same normal.
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    const std::vector<uint32_t> targetIdx = {0, 1, 2, 1, 3, 2};
    const std::vector<uint32_t> xref = {0, 1, 2, 1, 3, 2};
    const std::vector<uint32_t> outIdx = {0, 1, 2, 3, 4, 5};
    const auto n = Trellis2Bake::creaseAwareVertexNormals(pos, targetIdx, xref, outIdx, 6);
    for (int v = 0; v < 6; ++v)
        EXPECT_NEAR(n[v * 3 + 2], 1.0f, 1e-5f);
}

TEST(Trellis2BakeTest, GameReadyRemeshOptionYieldsManifold)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f, 80, 100);
    appendSphere(pos, idx, 0.995f, 80, 100);   // double wall
    Trellis2Bake::GameReadyOptions o;
    o.targetTriangles = 1500;
    o.remeshResolution = 96;
    const auto r = Trellis2Bake::makeGameReady(pos, idx, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.remeshed);
    const auto s = Trellis2Bake::topologyStats(r.positions.size() / 3, r.indices);
    EXPECT_EQ(s.nonManifoldEdges, 0);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_LE(r.outputTriangles, 1600);
}

TEST(Trellis2BakeTest, StrictBudgetCeilingHoldsThroughRemeshRepairs)
{
    // A strict budget is a platform upload limit: the remesh repair passes (hole
    // fans in particular, one triangle per loop edge) must never push the final
    // count back over it.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    appendSphere(pos, idx, 1.0f, 60, 80);
    appendSphere(pos, idx, 0.995f, 60, 80);   // double wall -> pinches after simplify
    Trellis2Bake::GameReadyOptions o;
    o.targetTriangles = 600;
    o.strictTriangleBudget = true;
    o.remeshResolution = 80;
    const auto r = Trellis2Bake::makeGameReady(pos, idx, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_LE(r.outputTriangles, 600);
}

TEST(Trellis2BakeTest, SimplifiedClosedMeshStaysClosedAndConsistent)
{
    // A closed UV sphere simplified hard must come out with no open rims, no
    // winding conflicts and no floating fragments (post-simplify repair, 5b).
    const int rings = 48, segs = 64;
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    pos.insert(pos.end(), {0.f, 1.f, 0.f});
    for (int r = 1; r < rings; ++r) {
        const float phi = float(kPi) * r / rings;
        for (int s = 0; s < segs; ++s) {
            const float th = 2.f * float(kPi) * s / segs;
            pos.insert(pos.end(), {std::sin(phi) * std::cos(th), std::cos(phi),
                                   std::sin(phi) * std::sin(th)});
        }
    }
    pos.insert(pos.end(), {0.f, -1.f, 0.f});
    const uint32_t south = static_cast<uint32_t>(pos.size() / 3 - 1);
    auto v = [&](int r, int s) { return uint32_t(1 + (r - 1) * segs + (s % segs)); };
    for (int s = 0; s < segs; ++s) {
        idx.insert(idx.end(), {0u, v(1, s + 1), v(1, s)});
        idx.insert(idx.end(), {south, v(rings - 1, s), v(rings - 1, s + 1)});
    }
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segs; ++s) {
            idx.insert(idx.end(), {v(r, s), v(r, s + 1), v(r + 1, s)});
            idx.insert(idx.end(), {v(r, s + 1), v(r + 1, s + 1), v(r + 1, s)});
        }

    Trellis2Bake::GameReadyOptions opts;
    opts.targetTriangles = 600;
    const auto r = Trellis2Bake::makeGameReady(pos, idx, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const auto s = Trellis2Bake::topologyStats(r.positions.size() / 3, r.indices);
    EXPECT_EQ(s.boundaryEdges, 0);
    EXPECT_EQ(s.flippedEdges, 0);
    EXPECT_EQ(s.components, 1);
}

