#include "QuadRetopo.h"

#include <gtest/gtest.h>

#include <vector>

// Unit tests for the triangle-pairing quad retopology (issue #401).
// The Ogre-backed entry point `retopologize(Ogre::Entity*, ...)` is
// covered by integration runs against real mesh files; the headless
// CI builds skip it via `tryInitOgre()`. The pure-data
// `retopologizeMesh(positions, indices, ...)` overload IS exercised
// here against synthetic input — it has no Ogre dependency.

namespace {

// Two coplanar right triangles sharing the diagonal (0,2):
//
//   v3 ─── v2
//   │  ╲   │
//   │   ╲  │
//   v0 ─── v1
//
// Should pair into one quad (v3, v0, v2, v1) — opposing-corner
// quad winding emitted by buildQuadWinding when the shared edge
// is v0→v2.
std::vector<float> kSquarePositions = {
    0.0f, 0.0f, 0.0f,   // v0
    1.0f, 0.0f, 0.0f,   // v1
    1.0f, 1.0f, 0.0f,   // v2
    0.0f, 1.0f, 0.0f,   // v3
};
std::vector<unsigned int> kSquareTriangles = {
    0, 1, 2,    // tri A
    0, 2, 3,    // tri B
};

} // namespace

TEST(QuadRetopoTest, PairsTwoCoplanarRightTrianglesIntoOneQuad)
{
    QuadRetopoOptions opts;
    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        kSquarePositions.data(), 4,
        kSquareTriangles.data(), 2,
        opts, faces);

    ASSERT_TRUE(report.applied);
    EXPECT_EQ(report.totalTrianglesBefore, 2);
    EXPECT_EQ(report.totalFacesAfter, 1);
    EXPECT_EQ(report.totalQuadsAfter, 1);
    EXPECT_EQ(report.totalTrianglesAfterRetopo, 0);
    ASSERT_EQ(faces.size(), 1u);
    EXPECT_EQ(faces[0].size(), 4u);
}

TEST(QuadRetopoTest, RejectsNonCoplanarTrianglesByDefault)
{
    // Bend tri B upward: v3 goes from z=0 to z=1, making the
    // dihedral angle 45°. Default maxAngleDeg=25° rejects this.
    std::vector<float> pos = kSquarePositions;
    pos[3 * 3 + 2] = 1.0f;  // v3.z = 1

    QuadRetopoOptions opts;  // defaults
    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        pos.data(), 4,
        kSquareTriangles.data(), 2,
        opts, faces);

    ASSERT_TRUE(report.applied);
    EXPECT_EQ(report.totalQuadsAfter, 0);
    EXPECT_EQ(report.totalTrianglesAfterRetopo, 2);  // both kept as tris
}

TEST(QuadRetopoTest, AcceptsBentTrianglesWhenMaxAngleIsRelaxed)
{
    std::vector<float> pos = kSquarePositions;
    pos[3 * 3 + 2] = 1.0f;  // ~45° dihedral

    QuadRetopoOptions opts;
    opts.maxAngleDeg = 90.0;     // accept anything up to 90°
    opts.shapeToleranceDeg = 90; // and any quasi-square shape

    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        pos.data(), 4,
        kSquareTriangles.data(), 2,
        opts, faces);

    ASSERT_TRUE(report.applied);
    EXPECT_EQ(report.totalQuadsAfter, 1);
}

TEST(QuadRetopoTest, EmptyInputReturnsErrorReport)
{
    QuadRetopoOptions opts;
    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        nullptr, 0, nullptr, 0, opts, faces);
    EXPECT_FALSE(report.applied);
    EXPECT_FALSE(report.error.isEmpty());
    EXPECT_TRUE(faces.empty());
}

TEST(QuadRetopoTest, AspectRatioGateRejectsElongatedQuads)
{
    // Stretch v1 way out to the right — the resulting quad has
    // aspect ratio 10:1. Default opts.maxAspectRatio=6.0 rejects.
    std::vector<float> pos = kSquarePositions;
    pos[3 * 1 + 0] = 10.0f;
    pos[3 * 2 + 0] = 10.0f;

    QuadRetopoOptions opts;
    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        pos.data(), 4, kSquareTriangles.data(), 2, opts, faces);
    ASSERT_TRUE(report.applied);
    EXPECT_EQ(report.totalQuadsAfter, 0);
}

TEST(QuadRetopoTest, TargetFacesStopsPairingEarly)
{
    // Two unit squares side by side (8 tris total). With
    // target_faces=8 we want no pairing; with target_faces=6 we
    // want exactly one pair (2 tris → 1 quad, 8→7).
    std::vector<float> pos = {
        0,0,0,  1,0,0,  2,0,0,
        0,1,0,  1,1,0,  2,1,0,
    };
    std::vector<unsigned int> tris = {
        0,1,4,  0,4,3,
        1,2,5,  1,5,4,
    };  // 4 tris

    QuadRetopoOptions opts;
    opts.targetFaces = 4;  // no pairing wanted

    std::vector<std::vector<unsigned int>> faces;
    auto report = QuadRetopo::retopologizeMesh(
        pos.data(), 6, tris.data(), 4, opts, faces);
    ASSERT_TRUE(report.applied);
    EXPECT_EQ(report.totalFacesAfter, 4);
    EXPECT_EQ(report.totalQuadsAfter, 0);
    EXPECT_EQ(report.totalTrianglesAfterRetopo, 4);
}

TEST(QuadRetopoTest, AlgorithmStringRoundTrip)
{
    EXPECT_EQ(QuadRetopo::algorithmToString(QuadRetopo::Algorithm::TrianglePair),
              QStringLiteral("pair-tris"));
    EXPECT_EQ(QuadRetopo::algorithmFromString("pair-tris"),
              QuadRetopo::Algorithm::TrianglePair);
    EXPECT_EQ(QuadRetopo::algorithmFromString("pair"),
              QuadRetopo::Algorithm::TrianglePair);
    EXPECT_EQ(QuadRetopo::algorithmFromString("unknown-fallback"),
              QuadRetopo::Algorithm::TrianglePair);  // safe default
}

TEST(QuadRetopoTest, ReportToJsonRoundTrip)
{
    QuadRetopoReport report;
    report.meshName = QStringLiteral("test");
    report.totalTrianglesBefore = 100;
    report.totalFacesAfter = 60;
    report.totalQuadsAfter = 40;
    report.totalTrianglesAfterRetopo = 20;
    report.applied = true;

    auto json = QuadRetopo::reportToJson(report);
    EXPECT_EQ(json["meshName"].toString(), QStringLiteral("test"));
    EXPECT_EQ(json["totalTrianglesBefore"].toInt(), 100);
    EXPECT_EQ(json["totalQuadsAfter"].toInt(), 40);
    EXPECT_TRUE(json["applied"].toBool());
    EXPECT_NEAR(json["quadDominance"].toDouble(), 0.8, 1e-6);
}
