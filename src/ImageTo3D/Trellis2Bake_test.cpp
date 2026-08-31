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
