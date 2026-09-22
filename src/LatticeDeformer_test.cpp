// Pure-data tests for the lattice (free-form) deformer. No Ogre buffers / GL —
// positions in, positions out — so they run headless like the rest of the suite.

#include <gtest/gtest.h>

#include "LatticeDeformer.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>
#include <random>

using Lattice::Grid;
using Lattice::Interpolation;

namespace {

Grid unitGrid(int n, Interpolation interp)
{
    Grid g = Grid::fromBounds(Ogre::AxisAlignedBox(Ogre::Vector3(0, 0, 0), Ogre::Vector3(1, 1, 1)), n, n, n,
                              /*padding=*/0.0f);
    g.interpolation = interp;
    return g;
}

void expectNear(const Ogre::Vector3& a, const Ogre::Vector3& b, float eps = 1e-4f)
{
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

std::vector<Ogre::Vector3> randomPoints(size_t n, float lo, float hi, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<Ogre::Vector3> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.emplace_back(d(rng), d(rng), d(rng));
    return out;
}

const Interpolation kModes[] = {Interpolation::Linear, Interpolation::Smooth, Interpolation::Bezier};

} // namespace

// ---------------------------------------------------------------------------
// Bases
// ---------------------------------------------------------------------------

TEST(LatticeBasis, EveryBasisIsAPartitionOfUnityWithLinearPrecision)
{
    for (int n = 2; n <= 6; ++n) {
        for (float s = 0.0f; s <= 1.0f; s += 0.05f) {
            for (auto fn : {Lattice::basisLinear, Lattice::basisSmooth, Lattice::basisBezier}) {
                const auto b = fn(s, n);
                float sum = 0.0f, first = 0.0f;
                for (const auto& [i, w] : b) {
                    ASSERT_GE(i, 0);
                    ASSERT_LT(i, n);
                    sum += w;
                    first += w * (static_cast<float>(i) / (n - 1));
                }
                EXPECT_NEAR(sum, 1.0f, 1e-5f) << "n=" << n << " s=" << s;
                EXPECT_NEAR(first, s, 1e-5f) << "n=" << n << " s=" << s; // reproduces x → x
            }
        }
    }
}

TEST(LatticeBasis, InterpolatingBasesHitTheControlPointsExactly)
{
    for (int n = 2; n <= 5; ++n) {
        for (int c = 0; c < n; ++c) {
            const float s = static_cast<float>(c) / (n - 1);
            for (auto fn : {Lattice::basisLinear, Lattice::basisSmooth}) {
                const auto b = fn(s, n);
                float wc = 0.0f, others = 0.0f;
                for (const auto& [i, w] : b) (i == c ? wc : others) += std::fabs(w);
                EXPECT_NEAR(wc, 1.0f, 1e-5f);
                EXPECT_NEAR(others, 0.0f, 1e-5f);
            }
        }
    }
}

TEST(LatticeBasis, SmoothBasisIsContinuousAcrossCells)
{
    // Approaching a cell boundary from either side gives the same weights.
    const int n = 5;
    const float boundary = 0.5f; // between cell 1 and 2
    auto weights = [&](float s) {
        std::vector<float> w(n, 0.0f);
        for (const auto& [i, wi] : Lattice::basisSmooth(s, n)) w[static_cast<size_t>(i)] += wi;
        return w;
    };
    const auto lo = weights(boundary - 1e-4f), hi = weights(boundary + 1e-4f);
    for (int i = 0; i < n; ++i) EXPECT_NEAR(lo[static_cast<size_t>(i)], hi[static_cast<size_t>(i)], 2e-3f);
}

// ---------------------------------------------------------------------------
// Grid construction
// ---------------------------------------------------------------------------

TEST(LatticeGrid, FromBoundsPadsAndClampsResolution)
{
    const Grid g = Grid::fromBounds(Ogre::AxisAlignedBox(Ogre::Vector3(-1, 0, 2), Ogre::Vector3(1, 4, 3)), 1, 3, 99,
                                    0.1f);
    EXPECT_EQ(g.nx, 2);
    EXPECT_EQ(g.ny, 3);
    EXPECT_EQ(g.nz, 16);
    // largest extent is 4 (y) → pad 0.4 per side
    expectNear(g.origin, Ogre::Vector3(-1.4f, -0.4f, 1.6f));
    expectNear(g.size, Ogre::Vector3(2.8f, 4.8f, 1.8f));
    EXPECT_TRUE(g.isValid());
    EXPECT_TRUE(g.isAtRest());
    EXPECT_EQ(g.pointCount(), 2 * 3 * 16);
}

TEST(LatticeGrid, FlatBoundsGetThickness)
{
    const Grid g = Grid::fromBounds(Ogre::AxisAlignedBox(Ogre::Vector3(0, 0, 0), Ogre::Vector3(2, 0, 2)), 2, 2, 2, 0.0f);
    EXPECT_GT(g.size.y, 0.0f);
    EXPECT_TRUE(g.isValid());
    // A vertex on the plane still maps to a finite local coordinate.
    const Ogre::Vector3 l = g.toLocal(Ogre::Vector3(1, 0, 1));
    EXPECT_TRUE(std::isfinite(l.y));
    EXPECT_NEAR(l.y, 0.5f, 1e-5f);
}

TEST(LatticeGrid, IndexAndCoordsRoundTrip)
{
    const Grid g = unitGrid(4, Interpolation::Linear);
    for (int idx = 0; idx < g.pointCount(); ++idx) {
        int i, j, k;
        g.coordsOf(idx, i, j, k);
        EXPECT_EQ(g.index(i, j, k), idx);
    }
}

// ---------------------------------------------------------------------------
// Deformation
// ---------------------------------------------------------------------------

TEST(LatticeDeform, RestLatticeIsIdentityInEveryModeInsideAndOutside)
{
    const auto pts = randomPoints(300, -0.5f, 1.5f); // includes points well outside the box
    for (auto mode : kModes) {
        for (int n : {2, 3, 5}) {
            const Grid g = unitGrid(n, mode);
            for (const auto& p : pts) expectNear(g.deform(p), p, 1e-4f);
        }
    }
}

TEST(LatticeDeform, MovingACornerMovesTheCornerVertexExactly)
{
    for (auto mode : kModes) {
        Grid g = unitGrid(3, mode);
        const Ogre::Vector3 delta(0.3f, -0.2f, 0.1f);
        g.points[static_cast<size_t>(g.index(2, 2, 2))] += delta;
        expectNear(g.deform(Ogre::Vector3(1, 1, 1)), Ogre::Vector3(1, 1, 1) + delta);
        // The opposite corner is pinned.
        expectNear(g.deform(Ogre::Vector3(0, 0, 0)), Ogre::Vector3(0, 0, 0));
    }
}

TEST(LatticeDeform, TrilinearCentreGetsOneEighthOfACornerMove)
{
    Grid g = unitGrid(2, Interpolation::Linear);
    g.points[static_cast<size_t>(g.index(1, 1, 1))] += Ogre::Vector3(0.8f, 0, 0);
    expectNear(g.deform(Ogre::Vector3(0.5f, 0.5f, 0.5f)), Ogre::Vector3(0.6f, 0.5f, 0.5f));
}

TEST(LatticeDeform, InteriorPointOnlyAffectsItsNeighbourhoodInSmoothMode)
{
    // 7³ lattice (6 cells per axis), nudge the centre point (index 3). Catmull-Rom
    // has 4-point support — P[c-1..c+2] per cell — so index 3 reaches cells 1..4
    // and a vertex in cell 0 is untouched, while Bezier spreads globally.
    Grid smooth = unitGrid(7, Interpolation::Smooth);
    Grid bezier = unitGrid(7, Interpolation::Bezier);
    const Ogre::Vector3 delta(0, 0.5f, 0);
    smooth.points[static_cast<size_t>(smooth.index(3, 3, 3))] += delta;
    bezier.points[static_cast<size_t>(bezier.index(3, 3, 3))] += delta;

    const Ogre::Vector3 centre(0.5f, 0.5f, 0.5f);
    expectNear(smooth.deform(centre), centre + delta); // interpolating
    const Ogre::Vector3 far(0.05f, 0.5f, 0.5f);         // cell 0 — outside index 3's support
    expectNear(smooth.deform(far), far, 1e-6f);
    EXPECT_GT((bezier.deform(far) - far).length(), 1e-5f);
    // One cell over (cell 1) the smooth basis DOES reach — the influence tapers, not stops.
    const Ogre::Vector3 near(0.2f, 0.5f, 0.5f);
    EXPECT_GT((smooth.deform(near) - near).length(), 1e-4f);
}

TEST(LatticeDeform, OutsideVerticesFollowTheNearestShellPoint)
{
    Grid g = unitGrid(2, Interpolation::Linear);
    const Ogre::Vector3 delta(0, 0, 1.0f);
    // Lift the whole +Y face.
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < 2; ++k) g.points[static_cast<size_t>(g.index(i, 1, k))] += delta;
    // A vertex above the box moves with the top face, not extrapolated beyond it.
    expectNear(g.deform(Ogre::Vector3(0.5f, 3.0f, 0.5f)), Ogre::Vector3(0.5f, 3.0f, 0.5f) + delta);
    // Continuity at the shell.
    expectNear(g.deform(Ogre::Vector3(0.5f, 1.0f + 1e-4f, 0.5f)), g.deform(Ogre::Vector3(0.5f, 1.0f - 1e-4f, 0.5f)),
               1e-3f);
}

TEST(LatticeDeform, DeformAllMatchesDeformAndResetRestoresIdentity)
{
    Grid g = unitGrid(3, Interpolation::Smooth);
    for (auto& p : g.points) p += Ogre::Vector3(0.1f, 0.05f, -0.1f);
    EXPECT_FALSE(g.isAtRest());
    const auto rest = randomPoints(50, 0.0f, 1.0f);
    std::vector<Ogre::Vector3> out;
    g.deformAll(rest, out);
    ASSERT_EQ(out.size(), rest.size());
    for (size_t i = 0; i < rest.size(); ++i) expectNear(out[i], g.deform(rest[i]));
    g.reset();
    EXPECT_TRUE(g.isAtRest());
    g.deformAll(rest, out);
    for (size_t i = 0; i < rest.size(); ++i) expectNear(out[i], rest[i]);
}

TEST(LatticeDeform, InvalidGridIsAPassThrough)
{
    Grid g;
    g.points.clear(); // wrong point count → invalid
    EXPECT_FALSE(g.isValid());
    expectNear(g.deform(Ogre::Vector3(3, 4, 5)), Ogre::Vector3(3, 4, 5));
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

TEST(LatticeJson, RoundTripIsExact)
{
    Grid g = Grid::fromBounds(Ogre::AxisAlignedBox(Ogre::Vector3(-2, 0, 1), Ogre::Vector3(3, 5, 4)), 3, 2, 4, 0.05f);
    g.interpolation = Interpolation::Bezier;
    g.points[5] += Ogre::Vector3(0.25f, -1.0f, 2.0f);

    const QJsonObject o = g.toJson();
    EXPECT_EQ(o.value("schema").toString(), QString::fromLatin1(Lattice::kJsonSchema));
    Grid back;
    QString err;
    ASSERT_TRUE(Grid::fromJson(o, back, &err)) << err.toStdString();
    EXPECT_EQ(back.nx, 3); EXPECT_EQ(back.ny, 2); EXPECT_EQ(back.nz, 4);
    EXPECT_EQ(back.interpolation, Interpolation::Bezier);
    expectNear(back.origin, g.origin, 1e-6f);
    expectNear(back.size, g.size, 1e-6f);
    ASSERT_EQ(back.points.size(), g.points.size());
    for (size_t i = 0; i < g.points.size(); ++i) expectNear(back.points[i], g.points[i], 1e-6f);
}

TEST(LatticeJson, RejectsMalformedInput)
{
    Grid g;
    QString err;
    EXPECT_FALSE(Grid::fromJson(QJsonObject{}, g, &err));
    EXPECT_FALSE(err.isEmpty());

    QJsonObject o = unitGrid(2, Interpolation::Linear).toJson();
    o["points"] = QJsonArray{1.0, 2.0}; // short
    EXPECT_FALSE(Grid::fromJson(o, g, &err));

    o = unitGrid(2, Interpolation::Linear).toJson();
    o["interpolation"] = "wobbly";
    EXPECT_FALSE(Grid::fromJson(o, g, &err));

    o = unitGrid(2, Interpolation::Linear).toJson();
    o["size"] = QJsonArray{1.0, 0.0, 1.0};
    EXPECT_FALSE(Grid::fromJson(o, g, &err));
}

TEST(LatticeJson, RejectsNonNumericAndNonFiniteCoordinates)
{
    // QJsonValue::toDouble() would silently read these as 0 / inf and collapse
    // the control points; the parser must refuse them instead.
    Grid g;
    QString err;
    QJsonObject o = unitGrid(2, Interpolation::Linear).toJson();
    QJsonArray pts = o["points"].toArray();
    pts[4] = QStringLiteral("0.5");
    o["points"] = pts;
    EXPECT_FALSE(Grid::fromJson(o, g, &err));
    EXPECT_TRUE(err.contains("points")) << err.toStdString();

    o = unitGrid(2, Interpolation::Linear).toJson();
    pts = o["points"].toArray();
    pts[7] = QJsonValue::Null;
    o["points"] = pts;
    EXPECT_FALSE(Grid::fromJson(o, g, &err));

    o = unitGrid(2, Interpolation::Linear).toJson();
    pts = o["points"].toArray();
    pts[0] = 1e300; // overflows float → inf
    o["points"] = pts;
    EXPECT_FALSE(Grid::fromJson(o, g, &err));

    o = unitGrid(2, Interpolation::Linear).toJson();
    o["origin"] = QJsonArray{true, 0.0, 0.0};
    EXPECT_FALSE(Grid::fromJson(o, g, &err));

    // Sanity: the untouched document still parses.
    EXPECT_TRUE(Grid::fromJson(unitGrid(2, Interpolation::Linear).toJson(), g, &err)) << err.toStdString();
}

TEST(LatticeJson, InterpolationIdsRoundTripAndAcceptAliases)
{
    for (auto mode : kModes) {
        Interpolation back;
        ASSERT_TRUE(Lattice::interpolationFromId(Lattice::interpolationId(mode), back));
        EXPECT_EQ(back, mode);
    }
    Interpolation b;
    EXPECT_TRUE(Lattice::interpolationFromId("Catmull-Rom", b));
    EXPECT_EQ(b, Interpolation::Smooth);
    EXPECT_FALSE(Lattice::interpolationFromId("", b));
}
