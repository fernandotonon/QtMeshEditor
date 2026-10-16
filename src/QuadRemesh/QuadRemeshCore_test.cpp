#include "QuadRemeshCore.h"
#include "QuadriFlowRunner.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <cmath>
#include <random>

using namespace QuadRemesh;

namespace {

// Unit cube with its 6 faces split into separate vertex blocks (the way a
// UV-seamed export arrives): 24 vertices, 12 triangles, outward winding.
void splitCube(std::vector<float>& pos, std::vector<uint32_t>& idx, float s = 1.0f)
{
    pos.clear();
    idx.clear();
    const float c[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    for (const auto& q : quads) {
        const uint32_t base = uint32_t(pos.size() / 3);
        for (int k = 0; k < 4; ++k)
            pos.insert(pos.end(), {c[q[k]][0] * s, c[q[k]][1] * s, c[q[k]][2] * s});
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

// n×n grid on the XY plane over [0,1]², UV = (x, y).
void plane(int n, std::vector<float>& pos, std::vector<uint32_t>& idx, std::vector<float>* uv = nullptr)
{
    pos.clear();
    idx.clear();
    if (uv) uv->clear();
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x) {
            pos.insert(pos.end(), {float(x) / n, float(y) / n, 0.0f});
            if (uv) uv->insert(uv->end(), {float(x) / n, float(y) / n});
        }
    auto v = [n](int x, int y) { return uint32_t(y * (n + 1) + x); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            idx.insert(idx.end(), {v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y), v(x + 1, y + 1), v(x, y + 1)});
}

// Quad faces of an m×m grid over the same plane (a different tessellation).
void quadPlane(int m, std::vector<float>& pos, std::vector<std::vector<uint32_t>>& faces)
{
    pos.clear();
    faces.clear();
    for (int y = 0; y <= m; ++y)
        for (int x = 0; x <= m; ++x)
            pos.insert(pos.end(), {float(x) / m, float(y) / m, 0.0f});
    auto v = [m](int x, int y) { return uint32_t(y * (m + 1) + x); };
    for (int y = 0; y < m; ++y)
        for (int x = 0; x < m; ++x)
            faces.push_back({v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y + 1)});
}

// Colour that encodes the UV: R = u, G = v (wrap-free inside [0,1]).
QImage uvGradient(int size)
{
    QImage img(size, size, QImage::Format_ARGB32);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            img.setPixel(x, y, qRgba(int(255.0 * (x + 0.5) / size), int(255.0 * (y + 0.5) / size), 40, 255));
    return img;
}

void uvSphere(int rings, int segs, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
    pos.clear();
    idx.clear();
    pos.insert(pos.end(), {0, 1, 0});
    for (int i = 1; i < rings; ++i) {
        const double t = M_PI * i / rings;
        for (int j = 0; j < segs; ++j) {
            const double p = 2 * M_PI * j / segs;
            pos.insert(pos.end(), {float(std::sin(t) * std::cos(p)), float(std::cos(t)), float(std::sin(t) * std::sin(p))});
        }
    }
    pos.insert(pos.end(), {0, -1, 0});
    auto id = [segs](int i, int j) { return uint32_t(1 + (i - 1) * segs + (j % segs)); };
    for (int j = 0; j < segs; ++j) idx.insert(idx.end(), {0, id(1, j + 1), id(1, j)});
    for (int i = 1; i < rings - 1; ++i)
        for (int j = 0; j < segs; ++j)
            idx.insert(idx.end(), {id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i, j), id(i + 1, j + 1), id(i + 1, j)});
    const uint32_t last = uint32_t(pos.size() / 3 - 1);
    for (int j = 0; j < segs; ++j) idx.insert(idx.end(), {last, id(rings - 1, j), id(rings - 1, j + 1)});
}

} // namespace

TEST(QuadRemeshCore, WeldJoinsSeamSplitCube)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    splitCube(pos, idx);
    EXPECT_FALSE(analyzeTopology(idx, int(pos.size() / 3)).boundaryEdges == 0);
    const int merged = weldByPosition(pos, idx);
    EXPECT_EQ(merged, 16);
    EXPECT_EQ(pos.size() / 3, 8u);
    EXPECT_EQ(idx.size(), 36u);
    const Topology t = analyzeTopology(idx, 8);
    EXPECT_EQ(t.boundaryEdges, 0);
    EXPECT_TRUE(t.manifold());
    EXPECT_EQ(t.inconsistentEdges, 0);
}

TEST(QuadRemeshCore, TopologyFlagsNonManifoldEdgeAndBowtieVertex)
{
    // Three triangles hinged on edge 0-1.
    const std::vector<uint32_t> fin = {0, 1, 2, 1, 0, 3, 0, 1, 4};
    EXPECT_EQ(analyzeTopology(fin, 5).nonManifoldEdges, 1);
    // Two triangles touching only at vertex 0 (bowtie).
    const std::vector<uint32_t> bowtie = {0, 1, 2, 0, 3, 4};
    const Topology t = analyzeTopology(bowtie, 5);
    EXPECT_EQ(t.nonManifoldEdges, 0);
    EXPECT_EQ(t.nonManifoldVertices, 1);
    EXPECT_FALSE(t.manifold());
}

TEST(QuadRemeshCore, VoxelRemeshClosesNonManifoldSoup)
{
    // Cube + a fin poking out of one face: non-manifold, open fin.
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    splitCube(pos, idx);
    weldByPosition(pos, idx);
    const uint32_t b = uint32_t(pos.size() / 3);
    pos.insert(pos.end(), {0.5f, 0.5f, 1.0f, 0.5f, 0.5f, 1.6f, 0.9f, 0.5f, 1.3f});
    idx.insert(idx.end(), {b, b + 1, b + 2});
    ASSERT_FALSE(analyzeTopology(idx, int(pos.size() / 3)).boundaryEdges == 0);

    std::vector<float> vp;
    std::vector<uint32_t> vi;
    QString err;
    ASSERT_TRUE(voxelRemesh(pos, idx, 48, vp, vi, &err)) << err.toStdString();
    const Topology t = analyzeTopology(vi, int(vp.size() / 3));
    EXPECT_EQ(t.boundaryEdges, 0);
    EXPECT_EQ(t.nonManifoldEdges, 0);
    // Bounds stay within ~one voxel of the input box (h = 1.6/48).
    float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t i = 0; i < vp.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], vp[i + k]);
            mx[k] = std::max(mx[k], vp[i + k]);
        }
    const float h = 1.6f / 48.0f;
    EXPECT_NEAR(mn[0], 0.0f, 1.5f * h);
    EXPECT_NEAR(mx[0], 1.0f, 1.5f * h);
    EXPECT_NEAR(mn[2], 0.0f, 1.5f * h);
    // Outward winding: positive signed volume.
    double vol = 0;
    for (size_t f = 0; f + 2 < vi.size(); f += 3) {
        const float* a = &vp[3 * vi[f]];
        const float* c1 = &vp[3 * vi[f + 1]];
        const float* c2 = &vp[3 * vi[f + 2]];
        vol += a[0] * (c1[1] * c2[2] - c1[2] * c2[1]) - a[1] * (c1[0] * c2[2] - c1[2] * c2[0])
               + a[2] * (c1[0] * c2[1] - c1[1] * c2[0]);
    }
    EXPECT_GT(vol / 6.0, 0.8);
}

TEST(QuadRemeshCore, SurfaceLocatorMatchesBruteForce)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    uvSphere(12, 24, pos, idx);
    SurfaceLocator loc;
    loc.build(pos, idx);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> d(-1.5f, 1.5f);
    for (int i = 0; i < 200; ++i) {
        const float p[3] = {d(rng), d(rng), d(rng)};
        int t;
        float b[3], cp[3];
        ASSERT_TRUE(loc.closest(p, t, b, cp));
        const float got = (cp[0] - p[0]) * (cp[0] - p[0]) + (cp[1] - p[1]) * (cp[1] - p[1]) + (cp[2] - p[2]) * (cp[2] - p[2]);
        float best = 1e30f;
        for (size_t f = 0; f + 2 < idx.size(); f += 3) {
            float c2[3], b2[3];
            SurfaceLocator one;
            std::vector<float> tp = {pos[3 * idx[f]], pos[3 * idx[f] + 1], pos[3 * idx[f] + 2],
                                     pos[3 * idx[f + 1]], pos[3 * idx[f + 1] + 1], pos[3 * idx[f + 1] + 2],
                                     pos[3 * idx[f + 2]], pos[3 * idx[f + 2] + 1], pos[3 * idx[f + 2] + 2]};
            std::vector<uint32_t> ti = {0, 1, 2};
            one.build(tp, ti);
            int tt;
            one.closest(p, tt, b2, c2);
            best = std::min(best, (c2[0] - p[0]) * (c2[0] - p[0]) + (c2[1] - p[1]) * (c2[1] - p[1])
                                      + (c2[2] - p[2]) * (c2[2] - p[2]));
        }
        EXPECT_NEAR(got, best, 1e-6f) << "query " << i;
    }
}

TEST(QuadRemeshCore, BlendBonesKeepsTopFourAndRenormalises)
{
    const std::vector<BoneWeight> a = {{0, 0.5f}, {1, 0.5f}};
    const std::vector<BoneWeight> b = {{2, 0.6f}, {3, 0.4f}};
    const std::vector<BoneWeight> c = {{4, 1.0f}};
    const std::vector<BoneWeight>* corners[3] = {&a, &b, &c};
    const float bary[3] = {0.5f, 0.3f, 0.2f};
    const auto out = blendBones(corners, bary);
    ASSERT_EQ(out.size(), 4u);
    float sum = 0;
    for (const auto& bw : out) sum += bw.weight;
    EXPECT_NEAR(sum, 1.0f, 1e-5f);
    // The 0.12 influence of bone 3 is the one dropped (smallest of five).
    for (const auto& bw : out) EXPECT_NE(bw.bone, 3);
}

TEST(QuadRemeshCore, TransferCarriesTextureAndWeightsOntoNewTopology)
{
    SourceMesh src;
    plane(10, src.positions, src.indices, &src.uvs);
    // Left half on bone 0, right half on bone 1.
    for (size_t v = 0; v < src.positions.size() / 3; ++v)
        src.bones.push_back({{uint16_t(src.positions[3 * v] < 0.5f ? 0 : 1), 1.0f}});
    SourceMaterial m;
    m.maps.insert(QStringLiteral("diffuse_map"), uvGradient(128));
    src.materials.push_back(m);

    std::vector<float> qp;
    std::vector<std::vector<uint32_t>> qf;
    quadPlane(7, qp, qf);
    Options o;
    o.textureSize = 256;
    o.bakeDetailNormal = true;
    const Result r = transferOnto(src, qp, qf, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.quads, 49);
    EXPECT_EQ(r.triangles, 0);
    ASSERT_EQ(r.uvs.size(), r.positions.size() / 3 * 2);
    ASSERT_TRUE(r.maps.contains(QStringLiteral("diffuse_map")));
    ASSERT_TRUE(r.maps.contains(QStringLiteral("normal_map")));
    const QImage& tex = r.maps.value(QStringLiteral("diffuse_map"));
    const QImage& nrm = r.maps.value(QStringLiteral("normal_map"));
    if (qEnvironmentVariableIsSet("QTMESH_QR_DUMP")) { tex.save(qEnvironmentVariable("QTMESH_QR_DUMP") + "/tex.png"); nrm.save(qEnvironmentVariable("QTMESH_QR_DUMP") + "/nrm.png"); }
    ASSERT_EQ(r.bones.size(), r.positions.size() / 3);

    // Every face centre: the baked colour at its NEW uv must equal the source
    // texture at the face's world (x, y) — the source UV.
    for (const auto& f : r.faces) {
        float u = 0, v = 0, x = 0, y = 0;
        for (uint32_t c : f) {
            u += r.uvs[2 * c] / f.size();
            v += r.uvs[2 * c + 1] / f.size();
            x += r.positions[3 * c] / f.size();
            y += r.positions[3 * c + 1] / f.size();
        }
        const QRgb got = tex.pixel(std::min(tex.width() - 1, int(u * tex.width())),
                                   std::min(tex.height() - 1, int(v * tex.height())));
        EXPECT_NEAR(qRed(got), 255.0 * x, 6.0);
        EXPECT_NEAR(qGreen(got), 255.0 * y, 6.0);
        // Flat source, no normal map → the detail map is the identity normal.
        const QRgb n = nrm.pixel(std::min(nrm.width() - 1, int(u * nrm.width())),
                                 std::min(nrm.height() - 1, int(v * nrm.height())));
        EXPECT_NEAR(qBlue(n), 255, 3);
        EXPECT_NEAR(qRed(n), 128, 3);
    }
    for (size_t vtx = 0; vtx < r.bones.size(); ++vtx) {
        const float x = r.positions[3 * vtx];
        if (std::fabs(x - 0.5f) < 0.11f) continue;   // the bone boundary
        ASSERT_FALSE(r.bones[vtx].empty());
        EXPECT_EQ(r.bones[vtx][0].bone, x < 0.5f ? 0 : 1);
        EXPECT_NEAR(r.bones[vtx][0].weight, 1.0f, 1e-4f);
    }
}

TEST(QuadRemeshCore, TransferWithoutTexturesKeepsTopologyUnsplit)
{
    SourceMesh src;
    plane(4, src.positions, src.indices);
    std::vector<float> qp;
    std::vector<std::vector<uint32_t>> qf;
    quadPlane(3, qp, qf);
    Options o;
    o.bakeTextures = false;
    const Result r = transferOnto(src, qp, qf, o);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.positions, qp);
    EXPECT_TRUE(r.uvs.empty());
    EXPECT_TRUE(r.maps.isEmpty());
    EXPECT_EQ(r.indices.size(), 9u * 6u);
}

TEST(QuadRemeshCore, ReadQuadObjCollapsesRepeatedCorner)
{
    QTemporaryDir dir;
    const QString p = dir.filePath(QStringLiteral("q.obj"));
    QFile f(p);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\nf 1 2 2 3\nf 1 1 1 2\n");
    f.close();
    std::vector<float> pos;
    std::vector<std::vector<uint32_t>> faces;
    ASSERT_TRUE(QuadriFlowRunner::readQuadObj(p, pos, faces));
    ASSERT_EQ(faces.size(), 2u);   // the 2-corner face is dropped
    EXPECT_EQ(faces[0].size(), 4u);
    EXPECT_EQ(faces[1].size(), 3u);
}

// End to end through the real helper process (built alongside the tests).
TEST(QuadRemeshCore, RemeshSphereThroughHelper)
{
    if (QuadriFlowRunner::defaultHelperPath().isEmpty())
        GTEST_SKIP() << "qtmesh_quadriflow helper not built";
    SourceMesh src;
    uvSphere(24, 48, src.positions, src.indices);
    Options o;
    o.targetFaces = 600;
    o.bakeTextures = false;
    int stages = 0;
    o.progress = [&](const QString& s, double f) {
        if (f >= 0 && !s.isEmpty()) ++stages;
        return true;
    };
    const Result r = remesh(src, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_FALSE(r.voxelized);
    EXPECT_GT(r.quads, 400);
    EXPECT_LT(r.quads, 900);
    EXPECT_LE(r.triangles, r.quads / 20);
    EXPECT_GE(stages, 6);
    // Every output vertex lies on the unit sphere (within tessellation error).
    for (size_t i = 0; i < r.positions.size(); i += 3) {
        const float l = std::sqrt(r.positions[i] * r.positions[i] + r.positions[i + 1] * r.positions[i + 1]
                                  + r.positions[i + 2] * r.positions[i + 2]);
        EXPECT_NEAR(l, 1.0f, 0.03f);
    }
}

TEST(QuadRemeshCore, RemeshNonManifoldInputGoesThroughVoxelPass)
{
    if (QuadriFlowRunner::defaultHelperPath().isEmpty())
        GTEST_SKIP() << "qtmesh_quadriflow helper not built";
    SourceMesh src;
    splitCube(src.positions, src.indices, 2.0f);
    const uint32_t b = uint32_t(src.positions.size() / 3);
    src.positions.insert(src.positions.end(), {1, 1, 2, 1, 1, 3, 1.8f, 1, 2.5f});
    src.indices.insert(src.indices.end(), {b, b + 1, b + 2});
    // A second cube sharing one EDGE with the first → non-manifold edge after weld.
    std::vector<float> p2;
    std::vector<uint32_t> i2;
    splitCube(p2, i2, 2.0f);
    const uint32_t off = uint32_t(src.positions.size() / 3);
    for (size_t i = 0; i < p2.size(); i += 3)
        src.positions.insert(src.positions.end(), {p2[i] + 2.0f, p2[i + 1] + 2.0f, p2[i + 2]});
    for (uint32_t i : i2) src.indices.push_back(i + off);
    Options o;
    o.targetFaces = 800;
    o.bakeTextures = false;
    const Result r = remesh(src, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.voxelized);
    EXPECT_GT(r.quads, 300);
}

TEST(QuadRemeshCore, MissingHelperIsAClearError)
{
    SourceMesh src;
    uvSphere(6, 8, src.positions, src.indices);
    Options o;
    o.helperPath = QStringLiteral("/nonexistent/qtmesh_quadriflow");
    const Result r = remesh(src, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("not found")));
}

TEST(QuadRemeshCore, DefaultTargetScalesWithInputAndClamps)
{
    EXPECT_EQ(defaultTargetFaces(10000), 6000);
    EXPECT_EQ(defaultTargetFaces(100), 1000);       // floor
    EXPECT_EQ(defaultTargetFaces(1000000), 20000);  // ceiling
}

TEST(QuadRemeshCore, FitToSurfaceSnapsOntoSourceAndDeviationMeasuresIt)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    uvSphere(24, 48, pos, idx);
    SurfaceLocator loc;
    loc.build(pos, idx);

    // A coarse quad cage pushed 15% outward: fitting must bring every vertex
    // back to the unit sphere.
    std::vector<float> q;
    std::vector<uint32_t> qi;
    uvSphere(8, 16, q, qi);
    for (float& c : q) c *= 1.15f;
    std::vector<std::vector<uint32_t>> faces;
    for (size_t t = 0; t + 2 < qi.size(); t += 3) faces.push_back({qi[t], qi[t + 1], qi[t + 2]});

    float mean0, max0;
    surfaceDeviation(q, pos, idx, mean0, max0);
    EXPECT_GT(mean0, 0.1f);
    fitToSurface(q, faces, loc, 2, 0.5f);
    for (size_t i = 0; i < q.size(); i += 3) {
        const float l = std::sqrt(q[i] * q[i] + q[i + 1] * q[i + 1] + q[i + 2] * q[i + 2]);
        EXPECT_NEAR(l, 1.0f, 0.01f);
    }
    float mean1, max1;
    surfaceDeviation(q, pos, idx, mean1, max1);
    EXPECT_LT(mean1, 0.01f);
    // A mesh against itself has no deviation.
    float m, x;
    surfaceDeviation(pos, pos, idx, m, x);
    EXPECT_LT(x, 1e-5f);
}

TEST(QuadRemeshCore, FitToSurfaceRespectsSnapLimit)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    uvSphere(12, 24, pos, idx);
    SurfaceLocator loc;
    loc.build(pos, idx);
    std::vector<float> far = {5, 0, 0, 5, 1, 0, 5, 0, 1};   // 4 units off the sphere
    const std::vector<std::vector<uint32_t>> f = {{0, 1, 2}};
    const std::vector<float> before = far;
    fitToSurface(far, f, loc, 0, 0.5f);
    EXPECT_EQ(far, before);   // beyond maxSnap: left alone
}

TEST(QuadRemeshCore, BakeIsDeterministicAcrossRuns)
{
    // Texel ownership (lowest triangle index) makes shared-edge texels
    // exclusive, so two runs produce byte-identical images.
    SourceMesh src;
    plane(10, src.positions, src.indices, &src.uvs);
    SourceMaterial m;
    m.maps.insert(QStringLiteral("diffuse_map"), uvGradient(64));
    src.materials.push_back(m);
    std::vector<float> qp;
    std::vector<std::vector<uint32_t>> qf;
    quadPlane(9, qp, qf);
    Options o;
    o.textureSize = 256;
    const Result a = transferOnto(src, qp, qf, o);
    const Result b = transferOnto(src, qp, qf, o);
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(a.maps.value(QStringLiteral("diffuse_map")), b.maps.value(QStringLiteral("diffuse_map")));
    EXPECT_EQ(a.maps.value(QStringLiteral("normal_map")), b.maps.value(QStringLiteral("normal_map")));
}

TEST(QuadRemeshCore, PerMaterialColoursAreBakedWhenTheyDiffer)
{
    // Two untextured materials: left half red, right half blue. Collapsing to
    // one material must not paint everything with the first colour.
    SourceMesh src;
    plane(10, src.positions, src.indices, &src.uvs);
    for (size_t t = 0; t < src.indices.size() / 3; ++t) {
        const float cx = (src.positions[3 * src.indices[3 * t]] + src.positions[3 * src.indices[3 * t + 1]]
                          + src.positions[3 * src.indices[3 * t + 2]]) / 3.0f;
        src.triangleMaterial.push_back(cx < 0.5f ? 0 : 1);
    }
    SourceMaterial red, blue;
    red.diffuse[0] = 1; red.diffuse[1] = 0; red.diffuse[2] = 0;
    blue.diffuse[0] = 0; blue.diffuse[1] = 0; blue.diffuse[2] = 1;
    src.materials = {red, blue};
    std::vector<float> qp;
    std::vector<std::vector<uint32_t>> qf;
    quadPlane(8, qp, qf);
    Options o;
    o.textureSize = 128;
    o.bakeDetailNormal = false;
    const Result r = transferOnto(src, qp, qf, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.diffuseColorBaked);
    ASSERT_TRUE(r.maps.contains(QStringLiteral("diffuse_map")));
    const QImage& tex = r.maps.value(QStringLiteral("diffuse_map"));
    for (const auto& f : r.faces) {
        float u = 0, v = 0, x = 0;
        for (uint32_t c : f) {
            u += r.uvs[2 * c] / f.size();
            v += r.uvs[2 * c + 1] / f.size();
            x += r.positions[3 * c] / f.size();
        }
        if (std::fabs(x - 0.5f) < 0.15f) continue;   // the material boundary
        const QRgb px = tex.pixel(std::min(tex.width() - 1, int(u * tex.width())),
                                  std::min(tex.height() - 1, int(v * tex.height())));
        if (x < 0.5f) { EXPECT_GT(qRed(px), 200); EXPECT_LT(qBlue(px), 50); }
        else          { EXPECT_GT(qBlue(px), 200); EXPECT_LT(qRed(px), 50); }
    }
}

TEST(QuadRemeshCore, CancelDuringBakeIsHonoured)
{
    // A small mesh baked into a large atlas: workers claim every triangle at
    // once, so the poll loop must keep running until the work is DONE for a
    // cancel to land.
    SourceMesh src;
    plane(10, src.positions, src.indices, &src.uvs);
    SourceMaterial m;
    m.maps.insert(QStringLiteral("diffuse_map"), uvGradient(64));
    src.materials.push_back(m);
    std::vector<float> qp;
    std::vector<std::vector<uint32_t>> qf;
    quadPlane(4, qp, qf);
    Options o;
    o.textureSize = 4096;
    int bakePolls = 0;
    o.progress = [&](const QString& stage, double) {
        if (stage == QLatin1String("bake")) ++bakePolls;
        return bakePolls < 2;   // cancel on the second bake poll
    };
    const Result r = transferOnto(src, qp, qf, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
}
