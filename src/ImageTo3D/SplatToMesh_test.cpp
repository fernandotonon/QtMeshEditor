// Pure-data tests for SplatToMesh (TripoSplat backend): splats -> closed mesh,
// slab welding, frame mapping, colour lookup and the 3DGS .ply round trip.

#include <gtest/gtest.h>

#include "SplatToMesh.h"

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace {

// Gaussians on a sphere surface (a thin shell, like TripoSplat's output).
SplatToMesh::Splats sphereSplats(float radius, int n, float scale = 0.01f)
{
    SplatToMesh::Splats s;
    const float golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < n; ++i) {
        const float y = 1.0f - 2.0f * (i + 0.5f) / n;
        const float r = std::sqrt(1.0f - y * y);
        const float a = golden * i;
        s.xyz.insert(s.xyz.end(), {radius * r * std::cos(a), radius * y, radius * r * std::sin(a)});
        s.fdc.insert(s.fdc.end(), {1.0f, 0.0f, -1.0f});
        s.opacity.push_back(1.0f);
        s.scale.insert(s.scale.end(), {scale, scale, scale});
        s.rotation.insert(s.rotation.end(), {1.0f, 0.0f, 0.0f, 0.0f});
    }
    return s;
}

double signedVolume(const std::vector<float>& p, const std::vector<uint32_t>& t)
{
    double v = 0.0;
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
        const float* a = &p[3 * t[i]];
        const float* b = &p[3 * t[i + 1]];
        const float* c = &p[3 * t[i + 2]];
        v += a[0] * (double(b[1]) * c[2] - double(b[2]) * c[1])
           - a[1] * (double(b[0]) * c[2] - double(b[2]) * c[0])
           + a[2] * (double(b[0]) * c[1] - double(b[1]) * c[0]);
    }
    return v / 6.0;
}

} // namespace

TEST(SplatToMesh, ShellOfSplatsBecomesClosedOutwardSphere)
{
    const auto splats = sphereSplats(0.4f, 20000);
    SplatToMesh::Options o;
    o.resolution = 96;   // two z-slabs: exercises the seam weld
    const auto r = SplatToMesh::extract(splats, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.splatsUsed, 20000);
    EXPECT_GT(signedVolume(r.positions, r.indices), 0.0) << "winding must face outward";
    // Marching cubes itself leaves a few cracks at ambiguous cube faces
    // (measured: 288 open edges of 96804 on a single slab), so the surface is
    // checked for being NEARLY closed — and EXACTLY closed along the slab
    // seam, which is what this module adds. The seam is grid plane z = 64:
    // world z = org + (64 + 0.5) * voxel, org computed as extract() does.
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 2; i < splats.xyz.size(); i += 3) {
        lo = std::min(lo, splats.xyz[i]);
        hi = std::max(hi, splats.xyz[i]);
    }
    float ext = 0.0f;
    for (int a = 0; a < 3; ++a) {
        float l = 1e9f, h = -1e9f;
        for (size_t i = size_t(a); i < splats.xyz.size(); i += 3) {
            l = std::min(l, splats.xyz[i]);
            h = std::max(h, splats.xyz[i]);
        }
        ext = std::max(ext, h - l);
    }
    ext *= 1.06f;
    const float orgZ = 0.5f * (lo + hi) - 0.5f * ext;
    const float seamZ = orgZ + (64.0f + 0.5f) * r.voxelSize;
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t i = 0; i + 2 < r.indices.size(); i += 3)
        for (int k = 0; k < 3; ++k)
            ++edges[{r.indices[i + k], r.indices[i + (k + 1) % 3]}];
    int open = 0, openOnSeam = 0;
    for (const auto& [e, n] : edges) {
        if (n == 1 && edges.count({e.second, e.first}))
            continue;
        ++open;
        if (std::abs(r.positions[3 * e.first + 2] - seamZ) < 1e-5f
            && std::abs(r.positions[3 * e.second + 2] - seamZ) < 1e-5f)
            ++openOnSeam;
    }
    EXPECT_EQ(openOnSeam, 0) << "the slab seam must weld exactly";
    EXPECT_LT(double(open) / double(edges.size()), 0.005);
    // One surface on the shell, not an inner + outer double wall.
    double meanR = 0.0;
    const size_t nv = r.positions.size() / 3;
    for (size_t v = 0; v < nv; ++v)
        meanR += std::sqrt(r.positions[3 * v] * r.positions[3 * v]
                           + r.positions[3 * v + 1] * r.positions[3 * v + 1]
                           + r.positions[3 * v + 2] * r.positions[3 * v + 2]);
    meanR /= double(nv);
    EXPECT_NEAR(meanR, 0.4, 0.03);
    const double sphereVolume = 4.0 / 3.0 * 3.14159265 * 0.4 * 0.4 * 0.4;
    // The surface sits on the OUTER side of the splat shell (mean radius
    // ~0.413 for 0.4), so the enclosed volume runs ~15% high.
    EXPECT_NEAR(signedVolume(r.positions, r.indices), sphereVolume, sphereVolume * 0.25);
}

TEST(SplatToMesh, TransparentAndEmptyInputFail)
{
    auto splats = sphereSplats(0.4f, 100);
    for (float& o : splats.opacity) o = 0.01f;
    EXPECT_FALSE(SplatToMesh::extract(splats).ok);
    EXPECT_FALSE(SplatToMesh::extract(SplatToMesh::Splats{}).ok);
}

TEST(SplatToMesh, ProgressCanCancel)
{
    const auto splats = sphereSplats(0.4f, 2000);
    SplatToMesh::Options o;
    o.resolution = 64;
    o.progress = [](int, int) { return false; };
    const auto r = SplatToMesh::extract(splats, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
}

TEST(SplatToMesh, EditorFrameIsAProperRotation)
{
    // Model frame: Z up, subject faces +X. Editor frame: Y up, faces +Z.
    std::vector<float> p{0.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f};
    SplatToMesh::toEditorFrame(p);
    EXPECT_FLOAT_EQ(p[1], 1.0f);   // up -> +Y
    EXPECT_FLOAT_EQ(p[5], 1.0f);   // facing -> +Z
    // (x,y,z) -> (y,z,x) is a cyclic permutation: det +1, no mirroring.
}

TEST(SplatToMesh, ColorFieldReturnsNearestSplatColour)
{
    const std::vector<float> pos{0, 0, 0,   1, 0, 0};
    const std::vector<float> rgb{1, 0, 0,   0, 0, 1};
    const std::vector<float> op{1, 1};
    const SplatToMesh::ColorField field(pos, rgb, op, 0.1f);
    const float q[6] = {0.01f, 0, 0,   0.99f, 0, 0};
    float out[6] = {};
    field.sample(q, 2, out);
    EXPECT_GT(out[0], 0.9f);
    EXPECT_LT(out[2], 0.1f);
    EXPECT_GT(out[5], 0.9f);
    // Nothing within reach: neutral grey rather than garbage.
    const float far[3] = {50.0f, 50.0f, 50.0f};
    field.sample(far, 1, out);
    EXPECT_FLOAT_EQ(out[0], 0.5f);
}

TEST(SplatToMesh, PlyRoundTripKeepsTheSplats)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto s = sphereSplats(0.3f, 64, 0.02f);
    s.opacity[5] = 0.25f;
    // A non-trivial rotation: 90 degrees about Z.
    s.rotation[4 * 7 + 0] = std::sqrt(0.5f);
    s.rotation[4 * 7 + 3] = std::sqrt(0.5f);
    const QString path = dir.filePath(QStringLiteral("s.ply"));
    QString err;
    ASSERT_TRUE(SplatToMesh::writePly(path, s, &err)) << err.toStdString();
    const auto back = SplatToMesh::readPly(path, &err);
    ASSERT_EQ(back.count(), s.count()) << err.toStdString();
    for (size_t i = 0; i < s.xyz.size(); ++i)
        EXPECT_NEAR(back.xyz[i], s.xyz[i], 1e-6f);
    EXPECT_NEAR(back.opacity[5], 0.25f, 1e-5f);
    EXPECT_NEAR(back.scale[0], 0.02f, 1e-6f);
    // q and -q are the same rotation.
    float dot = 0.0f;
    for (int k = 0; k < 4; ++k)
        dot += back.rotation[4 * 7 + k] * s.rotation[4 * 7 + k];
    EXPECT_NEAR(std::abs(dot), 1.0f, 1e-5f);
}

TEST(SplatToMesh, ReadPlyRejectsNonSplatFiles)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("x.ply"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n0\n");
    f.close();
    QString err;
    EXPECT_EQ(SplatToMesh::readPly(path, &err).count(), 0u);
    EXPECT_FALSE(err.isEmpty());
}
