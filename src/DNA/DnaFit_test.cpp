// Epic #1095 phase 4: rig fitting. The maths is pinned on a synthetic tube rig with
// closed-form answers; the real MetaHuman body (Epic's SKM_Body.dna via
// QTMESH_TEST_DNA_BODY or a local Unreal install — not redistributable) is checked
// when present and passes quietly otherwise.

#include <gtest/gtest.h>

#include "DnaFit.h"
#include "DnaRegister.h"
#include "DnaReader.h"
#include "DnaRigLogic.h"

#ifdef ENABLE_OPENRIGLOGIC
#include "DnaTestFixtures.h"
#endif

#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>

using namespace Dna;
using namespace Dna::Fit;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSegs = 12, kRings = 31;

// root(0,0,0) -> mid(0,1,0) -> tip(0,2,0); a cylinder of radius .3 from y=-0.5 to 2.5,
// skinned by height, with a UV seam (positions at angle 0 appear twice, with u=0 and u=1).
Rig tubeRig(int rings = kRings, int ringStep = 1)
{
    Rig rig;
    rig.joints.resize(3);
    rig.joints[0].name = "root";
    rig.joints[1].name = "mid";   rig.joints[1].parent = 0;  rig.joints[1].translation = {0, 1, 0};
    rig.joints[2].name = "tip";   rig.joints[2].parent = 1;  rig.joints[2].translation = {0, 1, 0};
    Mesh m;
    m.name = "tube";
    const int nr = (rings - 1) / ringStep + 1;
    for (int ri = 0; ri < nr; ++ri) {
        const int r = ri * ringStep;
        const double y = -0.5 + 3.0 * r / (kRings - 1);
        for (int a = 0; a < kSegs; ++a) {
            const double ang = 2 * kPi * a / kSegs;
            m.positions.insert(m.positions.end(), {float(0.3 * std::cos(ang)), float(y), float(0.3 * std::sin(ang))});
            double wr = 0, wm = 0, wt = 0;
            if (y < 0.7) wr = 1; else if (y < 1.3) { const double t = (y - 0.7) / 0.6; wr = 1 - t; wm = t; }
            else if (y < 1.7) wm = 1; else if (y < 2.3) { const double t = (y - 1.7) / 0.6; wm = 1 - t; wt = t; }
            else wt = 1;
            std::vector<SkinInfluence> inf;
            if (wr > 0) inf.push_back({0, float(wr)});
            if (wm > 0) inf.push_back({1, float(wm)});
            if (wt > 0) inf.push_back({2, float(wt)});
            m.skin.push_back(inf);
        }
        for (int a = 0; a <= kSegs; ++a)
            {
            const double vr = double(r) / (kRings - 1);
            // Deliberately NOT symmetric under v -> 1 - v (a real UV layout is not either), so the
            // V-orientation detection has something to find.
            m.uvs.insert(m.uvs.end(), {float(double(a) / kSegs), float(vr * vr * 0.8 + 0.05)});
        }
    }
    m.normals = {0, 0, 1};
    for (int ri = 0; ri < nr; ++ri)
        for (int a = 0; a <= kSegs; ++a) {
            Mesh::Layout l;
            l.position = uint32_t(ri * kSegs + a % kSegs);
            l.uv = uint32_t(ri * (kSegs + 1) + a);
            l.normal = 0;
            m.layouts.push_back(l);
        }
    for (int ri = 0; ri + 1 < nr; ++ri)
        for (int a = 0; a < kSegs; ++a) {
            const uint32_t i00 = uint32_t(ri * (kSegs + 1) + a), i01 = i00 + 1;
            const uint32_t i10 = uint32_t((ri + 1) * (kSegs + 1) + a), i11 = i10 + 1;
            m.faces.push_back({i00, i01, i11, i10});
        }
    rig.meshes.push_back(m);
    return rig;
}

std::vector<float> mapped(const std::vector<float>& p, const std::function<std::array<double, 3>(std::array<double, 3>)>& f)
{
    std::vector<float> out(p.size());
    for (size_t i = 0; i + 2 < p.size(); i += 3) {
        const auto q = f({p[i], p[i + 1], p[i + 2]});
        out[i] = float(q[0]); out[i + 1] = float(q[1]); out[i + 2] = float(q[2]);
    }
    return out;
}

std::array<double, 3> worldOf(const Rig& rig, int j)
{
    std::array<double, 3> t{0, 0, 0};   // orientations are identity in these tests
    for (int k = j; k >= 0; k = rig.joints[size_t(k)].parent)
        for (int a = 0; a < 3; ++a) t[size_t(a)] += rig.joints[size_t(k)].translation[size_t(a)];
    return t;
}

QString bodyDna()
{
    const QByteArray env = qgetenv("QTMESH_TEST_DNA_BODY");
    if (!env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env)))
        return QString::fromLocal8Bit(env);
    const QString ue = QStringLiteral(
        "/Users/Shared/Epic Games/UE_5.7/Engine/Plugins/MetaHuman/MetaHumanCoreTechLib/Content/"
        "ArchetypeDNA/SKM_Body.dna");
    return QFileInfo::exists(ue) ? ue : QString();
}

}  // namespace

// ---- warp ----------------------------------------------------------------------

TEST(DnaFitWarp, ReproducesAnyAffineEditExactly)
{
    // Non-uniform scale + shear + translation: the polynomial term must absorb it.
    auto edit = [](std::array<double, 3> p) {
        return std::array<double, 3>{1.15 * p[0] + 0.2 * p[1] + 0.05, 0.9 * p[1] - 0.1 * p[2] + 0.3, 1.3 * p[2] - 0.02 * p[0] - 0.1};
    };
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    std::vector<std::array<double, 3>> src, dst;
    for (int i = 0; i < 400; ++i) { std::array<double, 3> p{U(rng), U(rng) * 1.5, U(rng) * 0.4}; src.push_back(p); dst.push_back(edit(p)); }
    QString err;
    const Warp w = buildWarp(src, dst, Options(), &err);
    ASSERT_TRUE(w.valid) << err.toStdString();
    // Held-out points, including ones OUTSIDE the sampled volume.
    for (int i = 0; i < 60; ++i) {
        std::array<double, 3> p{U(rng) * 1.4, U(rng) * 2.0, U(rng) * 0.7};
        const auto d = w.displacement(p);
        const auto want = edit(p);
        for (int a = 0; a < 3; ++a) EXPECT_NEAR(p[size_t(a)] + d[size_t(a)], want[size_t(a)], 1e-6);
    }
}

TEST(DnaFitWarp, FollowsASmoothNonAffineEditAtItsControlPoints)
{
    auto bulge = [](std::array<double, 3> p) {
        const double k = 0.15 * std::exp(-((p[1] - 0.5) * (p[1] - 0.5)) / 0.2);
        return std::array<double, 3>{p[0] * (1 + k), p[1], p[2] * (1 + k)};
    };
    std::vector<std::array<double, 3>> src, dst;
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> U(-1.0, 1.0);
    for (int i = 0; i < 600; ++i) { std::array<double, 3> p{U(rng) * 0.4, U(rng), U(rng) * 0.4}; src.push_back(p); dst.push_back(bulge(p)); }
    const Warp w = buildWarp(src, dst, Options());
    ASSERT_TRUE(w.valid);
    double worst = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const auto d = w.displacement(src[i]);
        for (int a = 0; a < 3; ++a) worst = std::max(worst, std::abs(src[i][size_t(a)] + d[size_t(a)] - dst[i][size_t(a)]));
    }
    EXPECT_LT(worst, 0.02);   // within 2 cm of a 15 % bulge on a 1 m span (measured 1.04 cm)
}

TEST(DnaFitWarp, RefusesTooFewOrDegeneratePoints)
{
    QString err;
    EXPECT_FALSE(buildWarp({{0, 0, 0}, {1, 0, 0}}, {{0, 0, 0}, {1, 0, 0}}, Options(), &err).valid);
    EXPECT_FALSE(err.isEmpty());
    std::vector<std::array<double, 3>> plane;
    for (int i = 0; i < 40; ++i) plane.push_back({double(i % 8), double(i / 8), 0.0});   // coplanar
    err.clear();
    EXPECT_FALSE(buildWarp(plane, plane, Options(), &err).valid);
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(buildWarp({{0, 0, 0}}, {}, Options(), &err).valid);   // size mismatch
}

// ---- fit -----------------------------------------------------------------------

TEST(DnaFit, IdentityEditChangesNothing)
{
    const Rig rig = tubeRig();
    const Result r = fit(rig, {rig.meshes[0].positions});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(r.maxJointShiftMm, 0.0, 1e-6);
    EXPECT_NEAR(r.maxVertexShiftMm, 0.0, 1e-9);
    for (size_t j = 0; j < rig.joints.size(); ++j)
        for (int a = 0; a < 3; ++a)
            EXPECT_NEAR(r.fitted.joints[j].translation[size_t(a)], rig.joints[j].translation[size_t(a)], 1e-9);
}

TEST(DnaFit, UniformScaleScalesTheSkeletonAndKeepsOrientations)
{
    const Rig rig = tubeRig();
    const double s = 1.25;
    const Result r = fit(rig, {mapped(rig.meshes[0].positions, [&](auto p) { return std::array<double, 3>{p[0] * s, p[1] * s, p[2] * s}; })});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    for (size_t j = 0; j < rig.joints.size(); ++j) {
        for (int a = 0; a < 3; ++a)
            EXPECT_NEAR(r.fitted.joints[j].translation[size_t(a)], s * rig.joints[j].translation[size_t(a)], 1e-6) << "joint " << j;
        for (int a = 0; a < 4; ++a)
            EXPECT_DOUBLE_EQ(r.fitted.joints[j].orientation[size_t(a)], rig.joints[j].orientation[size_t(a)]);
    }
    EXPECT_LT(r.warpResidualMm, 1e-3);
    // The bind pose IS the target.
    for (size_t i = 0; i < rig.meshes[0].positions.size(); ++i)
        EXPECT_NEAR(r.fitted.meshes[0].positions[i], s * rig.meshes[0].positions[i], 1e-6);
    // Skin weights are untouched.
    EXPECT_EQ(r.fitted.meshes[0].skin.size(), rig.meshes[0].skin.size());
}

TEST(DnaFit, TranslationMovesTheRootAndNothingElseLocally)
{
    const Rig rig = tubeRig();
    const Result r = fit(rig, {mapped(rig.meshes[0].positions, [](auto p) { return std::array<double, 3>{p[0] + 0.3, p[1], p[2] - 0.1}; })});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(r.fitted.joints[0].translation[0], 0.3, 1e-6);
    EXPECT_NEAR(r.fitted.joints[0].translation[2], -0.1, 1e-6);
    for (size_t j = 1; j < 3; ++j)
        for (int a = 0; a < 3; ++a)
            EXPECT_NEAR(r.fitted.joints[j].translation[size_t(a)], rig.joints[j].translation[size_t(a)], 1e-6);
    const auto w2 = worldOf(r.fitted, 2);
    EXPECT_NEAR(w2[0], 0.3, 1e-6);
    EXPECT_NEAR(w2[1], 2.0, 1e-6);
}

TEST(DnaFit, UniformScaleReproducesTheIdealScaledPoseExactly)
{
    const Rig rig = tubeRig();
    const double s = 1.3;
    const Result r = fit(rig, {mapped(rig.meshes[0].positions, [&](auto p) { return std::array<double, 3>{p[0] * s, p[1] * s, p[2] * s}; })});
    ASSERT_TRUE(r.ok);
    PoseSpec bend;
    bend.name = "bend";
    bend.rotations = {{"mid", {0, 0, 60}}, {"tip", {0, 0, 30}}};
    const auto rep = evaluatePoses(rig, r.fitted, {bend}, s);
    ASSERT_EQ(rep.size(), 1u);
    EXPECT_TRUE(rep[0].joints);
    EXPECT_GT(rep[0].maxMovementMm, 100.0);          // the pose really moves the mesh
    EXPECT_LT(rep[0].similarityMaxMm, 1e-3);          // ... and matches s * posed original
    EXPECT_LT(rep[0].stretchMaxDelta, 1e-4);          // ... with the source rig's stretch
}

TEST(DnaFit, EdgeStretchCatchesAFitThatLeftTheSkeletonBehind)
{
    // Control: scale the mesh but keep the ORIGINAL joints. The skinning is then wrong,
    // and the edge-stretch metric must say so (this is why it is the quality measure).
    const Rig rig = tubeRig();
    Rig bad = rig;
    bad.meshes[0].positions = mapped(rig.meshes[0].positions, [](auto p) { return std::array<double, 3>{p[0] * 1.6, p[1] * 1.6, p[2] * 1.6}; });
    PoseSpec bend;
    bend.name = "bend";
    bend.rotations = {{"mid", {0, 0, 80}}};
    const auto badRep = evaluatePoses(rig, bad, {bend});
    ASSERT_EQ(badRep.size(), 1u);
    EXPECT_GT(badRep[0].stretchMaxDelta, 0.15);
    const Result good = fit(rig, {bad.meshes[0].positions});
    ASSERT_TRUE(good.ok);
    const auto goodRep = evaluatePoses(rig, good.fitted, {bend});
    EXPECT_LT(goodRep[0].stretchMaxDelta, 1e-4);
}

TEST(DnaFit, ReportsAMissingPoseJointInsteadOfInventingOne)
{
    const Rig rig = tubeRig();
    PoseSpec p;
    p.name = "ghost";
    p.rotations = {{"no_such_joint", {0, 0, 10}}};
    EXPECT_FALSE(evaluatePoses(rig, rig, {p})[0].joints);
}

TEST(DnaFit, RefusesATargetWithTheWrongTopologyWithAnActionableMessage)
{
    const Rig rig = tubeRig();
    std::vector<float> fewer(rig.meshes[0].positions.begin(), rig.meshes[0].positions.end() - 30);
    Result r = fit(rig, {fewer});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("topology")) << r.error.toStdString();
    EXPECT_TRUE(r.error.contains("tube"));
    EXPECT_FALSE(fit(rig, {}).ok);                                  // wrong mesh-array count
    std::vector<float> nan = rig.meshes[0].positions;
    nan[4] = std::nanf("");
    EXPECT_FALSE(fit(rig, {nan}).ok);
    EXPECT_FALSE(fit(rig, {std::vector<float>()}).ok);              // nothing to fit
}

TEST(DnaFit, AMeshOutsideTheTargetFollowsTheWarp)
{
    Rig rig = tubeRig();
    Rig two = rig;
    two.meshes.push_back(rig.meshes[0]);
    two.meshes[1].name = "second";
    const double s = 1.2;
    auto scale = [&](auto p) { return std::array<double, 3>{p[0] * s, p[1] * s, p[2] * s}; };
    const Result r = fit(two, {mapped(rig.meshes[0].positions, scale), {}});
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    for (size_t i = 0; i < rig.meshes[0].positions.size(); ++i)
        EXPECT_NEAR(r.fitted.meshes[1].positions[i], s * rig.meshes[0].positions[i], 1e-4);
    EXPECT_FALSE(r.warnings.isEmpty());
}

// ---- correspondence ---------------------------------------------------------------

namespace {
// The source mesh as the importer would hand it back: expanded layout vertices.
ImportedSubmesh importedOf(const Rig& rig, bool flipSourceV)
{
    const ExpandedMesh ex = expand(rig.meshes[0], flipSourceV);
    return {ex.positions, ex.uvs};
}
}  // namespace

TEST(DnaFitMatch, RecoversPositionsFromShuffledVerticesWithAFlippedVAxis)
{
    const Rig rig = tubeRig();
    ImportedSubmesh sub = importedOf(rig, /*flipSourceV=*/true);
    const size_t n = sub.positions.size() / 3;
    // The edit: scale by 1.1, then the importer hands the vertices back in another ORDER
    // and with the opposite V convention.
    std::vector<uint32_t> perm(n);
    for (uint32_t i = 0; i < n; ++i) perm[i] = i;
    std::shuffle(perm.begin(), perm.end(), std::mt19937(11));
    ImportedSubmesh edited;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t i = perm[k];
        for (int a = 0; a < 3; ++a) edited.positions.push_back(sub.positions[i * 3 + size_t(a)] * 1.1f);
        edited.uvs.push_back(sub.uvs[i * 2]);
        edited.uvs.push_back(1.0f - sub.uvs[i * 2 + 1]);
    }
    const Correspondence c = matchImported(rig, {edited}, /*flipV=*/true);
    ASSERT_TRUE(c.ok) << c.error.toStdString();
    EXPECT_TRUE(c.matchedByUv);
    EXPECT_TRUE(c.flippedV);
    EXPECT_EQ(c.matchedFraction, 1.0);
    ASSERT_EQ(c.positions.size(), 1u);
    for (size_t i = 0; i < rig.meshes[0].positions.size(); ++i)
        EXPECT_NEAR(c.positions[0][i], 1.1f * rig.meshes[0].positions[i], 1e-5f);
    EXPECT_LT(c.maxSeamSplitMm, 1e-3);     // seam duplicates moved together
}

TEST(DnaFitMatch, FailsInsteadOfGuessingWhenTheUvsWereChanged)
{
    const Rig rig = tubeRig();
    ImportedSubmesh sub = importedOf(rig, true);
    const size_t n = sub.positions.size() / 3;
    for (size_t v = 0; v < n; v += 3) { sub.uvs[v * 2] += 0.37f; sub.uvs[v * 2 + 1] += 0.21f; }   // a third of the UVs moved
    const Correspondence c = matchImported(rig, {sub});
    EXPECT_FALSE(c.ok);
    EXPECT_TRUE(c.error.contains("UV")) << c.error.toStdString();
}

TEST(DnaFitMatch, RefusesADecimatedTargetThatKeepsTheOriginalUvs)
{
    const Rig rig = tubeRig();
    ImportedSubmesh sub = importedOf(rig, true);
    const size_t n = sub.positions.size() / 3;
    // keep only the first 80 % of the vertices: every survivor still matches by UV, but a fifth of
    // the character has no counterpart (those would silently keep their unedited position)
    const size_t keep = n * 8 / 10;
    sub.positions.resize(keep * 3);
    sub.uvs.resize(keep * 2);
    const Correspondence c = matchImported(rig, {sub});
    EXPECT_FALSE(c.ok);
    EXPECT_LT(c.sourceCoverage, 0.995);
    EXPECT_TRUE(c.error.contains("counterpart")) << c.error.toStdString();
}

TEST(DnaFitMatch, SubmeshStructureAndOrderFallback)
{
    const Rig rig = tubeRig();
    EXPECT_FALSE(matchImported(rig, {}).ok);                       // wrong submesh count
    EXPECT_TRUE(matchImported(rig, {}).error.contains("submeshes"));
    // Without UVs the legacy order-based path is used, and a count mismatch is named.
    ImportedSubmesh noUv = importedOf(rig, true);
    noUv.uvs.clear();
    EXPECT_TRUE(matchImported(rig, {noUv}).ok);
    noUv.positions.resize(noUv.positions.size() - 9);
    const Correspondence bad = matchImported(rig, {noUv});
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(bad.error.contains("tube"));
}

// ---- LOD propagation --------------------------------------------------------------

TEST(DnaFitLod, TheSameWarpMovesACoarserLodConsistently)
{
    const Rig lod0 = tubeRig();
    const Rig lod1 = tubeRig(kRings, 2);                 // every other ring: a decimated LOD
    auto scale = [](auto p) { return std::array<double, 3>{p[0] * 1.15, p[1] * 1.15, p[2] * 1.15}; };
    const Result r = fit(lod0, {mapped(lod0.meshes[0].positions, scale)});
    ASSERT_TRUE(r.ok);
    const LodPropagation lp = evaluateLodPropagation(lod0, r.fitted, lod1, r.warp);
    EXPECT_GT(lp.vertices, 100);
    // The warped coarse LOD sits as close to the fitted fine LOD as the unwarped one sat to the original.
    EXPECT_LT(std::abs(lp.meanGapMm - lp.baselineMeanGapMm), 2.0);
}

// ---- real body (quiet when Epic's asset is absent) -------------------------------------

#ifdef ENABLE_OPENRIGLOGIC

TEST(DnaFitReal, UniformScaleOfTheMetaHumanBodyFitsExactlyInEveryReferencePose)
{
    const QString path = bodyDna();
    if (path.isEmpty()) { SUCCEED() << "no MetaHuman body DNA available — set QTMESH_TEST_DNA_BODY to run"; return; }
    const ReadResult src = read(path, ReadOptions());
    ASSERT_TRUE(src.ok) << src.error.toStdString();
    const double s = 1.1;
    std::vector<std::vector<float>> target;
    for (const Mesh& m : src.rig.meshes)
        target.push_back(mapped(m.positions, [&](auto p) { return std::array<double, 3>{p[0] * s, p[1] * s, p[2] * s}; }));
    const Result r = fit(src.rig, target);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_LT(r.warpResidualMm, 0.01);
    int checked = 0;
    for (const PoseReport& p : evaluatePoses(src.rig, r.fitted, referencePoses(), s)) {
        EXPECT_TRUE(p.joints) << p.name.toStdString();
        EXPECT_LT(p.similarityMaxMm, 0.05) << p.name.toStdString();       // measured 0.000
        EXPECT_LT(p.stretchMaxDelta, 1e-3) << p.name.toStdString();
        EXPECT_GT(p.maxMovementMm, 100.0) << p.name.toStdString();
        ++checked;
    }
    EXPECT_EQ(checked, 8);
}

TEST(DnaFitReal, ALocalEditKeepsDeformationUsable)
{
    const QString path = bodyDna();
    if (path.isEmpty()) { SUCCEED() << "no MetaHuman body DNA available — set QTMESH_TEST_DNA_BODY to run"; return; }
    const ReadResult src = read(path, ReadOptions());
    ASSERT_TRUE(src.ok);
    std::vector<std::vector<float>> target;
    for (const Mesh& m : src.rig.meshes)       // wider hips: x * 1.25 below y = 0.9
        target.push_back(mapped(m.positions, [](auto p) { return std::array<double, 3>{p[0] * (p[1] < 0.9 ? 1.25 : 1.0), p[1], p[2]}; }));
    const Result r = fit(src.rig, target);
    ASSERT_TRUE(r.ok);
    EXPECT_GT(r.maxJointShiftMm, 5.0);          // the skeleton really followed
    EXPECT_LT(r.maxJointShiftMm, 120.0);
    for (const PoseReport& p : evaluatePoses(src.rig, r.fitted, referencePoses())) {
        EXPECT_LT(p.stretchP99Delta, 0.05) << p.name.toStdString();         // measured <= 0.021
        EXPECT_LT(p.stretchMaxDelta, 0.5) << p.name.toStdString();          // measured <= 0.148
    }
}

TEST(DnaFitReal, FittedBodyRoundTripsThroughAWrittenDnaAndRigLogicStillLoadsIt)
{
    const QString path = bodyDna();
    if (path.isEmpty()) { SUCCEED() << "no MetaHuman body DNA available — set QTMESH_TEST_DNA_BODY to run"; return; }
    const ReadResult src = read(path, ReadOptions());
    ASSERT_TRUE(src.ok);
    std::vector<std::vector<float>> target;
    for (const Mesh& m : src.rig.meshes)
        target.push_back(mapped(m.positions, [](auto p) { return std::array<double, 3>{p[0] * 1.08, p[1] * 1.12, p[2] * 1.08}; }));
    const Result r = fit(src.rig, target);
    ASSERT_TRUE(r.ok);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath("fitted.dna");
    const WriteResult w = writeFittedDna(path, src.rig, r.fitted, r.warp, out);
    ASSERT_TRUE(w.ok) << w.error.toStdString();
    EXPECT_GT(w.meshesWritten, 1);
    EXPECT_GT(w.lodMeshesWarped, 0);          // lower LODs followed the warp

    const ReadResult back = read(out, ReadOptions());
    ASSERT_TRUE(back.ok) << back.error.toStdString();
    ASSERT_EQ(back.rig.meshes.size(), r.fitted.meshes.size());
    double pm = 0;
    for (size_t m = 0; m < back.rig.meshes.size(); ++m)
        for (size_t i = 0; i < back.rig.meshes[m].positions.size(); ++i)
            pm = std::max(pm, double(std::abs(back.rig.meshes[m].positions[i] - r.fitted.meshes[m].positions[i])));
    EXPECT_LT(pm * 1000.0, 0.01);             // mm
    double jm = 0;
    for (size_t j = 0; j < back.rig.joints.size(); ++j)
        for (int a = 0; a < 3; ++a)
            jm = std::max(jm, std::abs(back.rig.joints[j].translation[size_t(a)] - r.fitted.joints[j].translation[size_t(a)]));
    EXPECT_LT(jm * 1000.0, 0.01);

    // The behaviour layers survived: RigLogic still evaluates it, and its rest pose is the fitted skeleton.
    QString err;
    auto ev = RigEvaluator::create(out, ReadOptions(), &err);
    ASSERT_NE(ev, nullptr) << err.toStdString();
    ev->resetControls();
    ev->calculate();
    for (const int j : ev->drivenJoints()) {
        const JointPose p = ev->jointPose(j);
        for (int a = 0; a < 3; ++a)
            EXPECT_NEAR(p.translation[size_t(a)], r.fitted.joints[size_t(j)].translation[size_t(a)], 1e-4);
    }
    // And the source is never overwritten.
    EXPECT_FALSE(writeFittedDna(path, src.rig, r.fitted, r.warp, path).ok);
}

TEST(DnaFitWrite, SyntheticDnaRoundTripsInTheFilesOwnUnits)
{
    // Needs no Epic asset: the library's own writer builds a small DNA.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString srcPath = dir.filePath("src.dna");
    {
        const auto bytes = DnaTest::makeSyntheticDna();
        QFile f(srcPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size()));
    }
    ReadOptions ro;   // default: joint translations are the file's own (parent-relative) values
    const ReadResult src = read(srcPath, ro);
    ASSERT_TRUE(src.ok) << src.error.toStdString();
    Rig fitted = src.rig;
    for (auto& v : fitted.meshes[0].positions) v *= 1.5f;
    fitted.joints[1].translation[1] *= 1.5;
    // A trivially valid warp (any non-degenerate cloud) for the lower-LOD path.
    std::vector<std::array<double, 3>> a, b;
    for (int i = 0; i < 27; ++i) { std::array<double, 3> p{double(i % 3), double((i / 3) % 3), double(i / 9)}; a.push_back(p); b.push_back({p[0] * 1.5, p[1] * 1.5, p[2] * 1.5}); }
    const Warp w = buildWarp(a, b, Options());
    ASSERT_TRUE(w.valid);
    const QString out = dir.filePath("out.dna");
    const WriteResult wr = writeFittedDna(srcPath, src.rig, fitted, w, out);
    ASSERT_TRUE(wr.ok) << wr.error.toStdString();
    const ReadResult back = read(out, ro);
    ASSERT_TRUE(back.ok) << back.error.toStdString();
    ASSERT_EQ(back.rig.meshes[0].positions.size(), fitted.meshes[0].positions.size());
    for (size_t i = 0; i < back.rig.meshes[0].positions.size(); ++i)
        EXPECT_NEAR(back.rig.meshes[0].positions[i], fitted.meshes[0].positions[i], 1e-5f) << i;
    EXPECT_NEAR(back.rig.joints[1].translation[1], fitted.joints[1].translation[1], 1e-6);
}

TEST(DnaFitWrite, RefusesToOverwriteTheSourceThroughASymlink)
{
#ifndef Q_OS_WIN
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString srcPath = dir.filePath("src.dna");
    {
        const auto bytes = DnaTest::makeSyntheticDna();
        QFile f(srcPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size()));
    }
    const qint64 before = QFileInfo(srcPath).size();
    const QString link = dir.filePath("alias.dna");
    ASSERT_TRUE(QFile::link(srcPath, link));      // QFile::link on unix makes a symlink
    ReadOptions ro;
    const ReadResult src = read(srcPath, ro);
    ASSERT_TRUE(src.ok);
    std::vector<std::array<double, 3>> a, b;
    for (int i = 0; i < 27; ++i) { std::array<double, 3> p{double(i % 3), double((i / 3) % 3), double(i / 9)}; a.push_back(p); b.push_back(p); }
    const Warp w = buildWarp(a, b, Options());
    const WriteResult wr = writeFittedDna(srcPath, src.rig, src.rig, w, link);
    EXPECT_FALSE(wr.ok);
    EXPECT_TRUE(wr.error.contains("overwrite")) << wr.error.toStdString();
    EXPECT_EQ(QFileInfo(srcPath).size(), before);   // the source is untouched
#endif
}


// ---- registration onto another character --------------------------------------------

namespace {
// A stick rig with MetaHuman body-joint names, all children of the pelvis, plus the tube mesh.
Rig stickRig(std::vector<std::array<double, 3>>& world)
{
    Rig rig = tubeRig();
    const std::vector<std::pair<const char*, std::array<double, 3>>> j = {
        {"pelvis", {0, 1.0, 0}}, {"spine_03", {0, 1.4, 0.05}}, {"spine_05", {0, 1.6, 0.05}}, {"neck_01", {0, 1.75, 0.04}},
        {"head", {0, 1.85, 0.06}}, {"upperarm_l", {0.2, 1.6, 0}}, {"upperarm_r", {-0.2, 1.6, 0}},
        {"hand_l", {0.7, 1.5, 0.1}}, {"hand_r", {-0.7, 1.5, 0.1}}, {"thigh_l", {0.1, 0.95, 0}}, {"thigh_r", {-0.1, 0.95, 0}},
        {"foot_l", {0.1, 0.05, 0.05}}, {"foot_r", {-0.1, 0.05, 0.05}}};
    rig.joints.clear();
    world.clear();
    for (size_t i = 0; i < j.size(); ++i) {
        Joint jt;
        jt.name = j[i].first;
        jt.parent = i == 0 ? -1 : 0;
        jt.translation = i == 0 ? j[i].second : std::array<double, 3>{j[i].second[0] - j[0].second[0], j[i].second[1] - j[0].second[1], j[i].second[2] - j[0].second[2]};
        rig.joints.push_back(jt);
        world.push_back(j[i].second);
    }
    for (auto& v : rig.meshes[0].skin) v = {{0, 1.f}};   // everything on the pelvis; skinning is not under test
    return rig;
}
}  // namespace

TEST(DnaRegister, JointNamesAreMatchedAcrossConventionsAndTheRestReported)
{
    std::vector<std::pair<QString, std::array<double, 3>>> bones = {
        {"Hips", {0, 1, 0}}, {"mixamorig:LeftArm", {1, 2, 0}}, {"RIGHTHAND", {-2, 2, 0}}, {"Head", {0, 3, 0}}, {"prop_sword", {9, 9, 9}}};
    QStringList unmatched;
    const auto a = matchJointAnchors(bones, &unmatched);
    ASSERT_EQ(a.size(), 4u);
    EXPECT_TRUE(unmatched.contains("foot_l"));
    EXPECT_FALSE(unmatched.contains("pelvis"));
    bool sawHand = false;
    for (const auto& x : a) if (x.bodyJoint == "hand_r") { sawHand = true; EXPECT_EQ(x.target[0], -2.0); }   // case-insensitive
    EXPECT_TRUE(sawHand);
}

TEST(DnaRegister, TooFewAnchorsIsRefusedWithAnActionableMessage)
{
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    const JointRegisterResult r = registerByJoints(rig, {{"pelvis", {0, 1, 0}}, {"head", {0, 2, 0}}});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("at least 6")) << r.error.toStdString();
}

TEST(DnaRegister, AUniformlyScaledCoplanarTargetKeepsItsDepth)
{
    // The target skeleton is flat (T-pose, every joint at z = 0): without depth anchors the
    // warp has no depth information and squashes the body to a sheet.
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    std::vector<JointAnchor> anchors;
    for (size_t i = 0; i < w.size(); ++i) anchors.push_back({rig.joints[i].name, {1.5 * w[i][0], 1.5 * w[i][1], 0.0}});
    const JointRegisterResult r = registerByJoints(rig, anchors);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.positions[0].empty());
    double zmin = 1e9, zmax = -1e9, ymax = -1e9;
    const auto& P = r.positions[0];
    for (size_t i = 0; i + 2 < P.size(); i += 3) { zmin = std::min(zmin, double(P[i + 2])); zmax = std::max(zmax, double(P[i + 2])); ymax = std::max(ymax, double(P[i + 1])); }
    const double depth = zmax - zmin;          // source tube is 0.6 deep -> ~0.9 after a 1.5x scale
    EXPECT_GT(depth, 0.6) << "depth collapsed: " << depth;
    EXPECT_NEAR(ymax, 1.5 * 2.5, 0.35);        // height followed the scale (tube top at y = 2.5)
}

TEST(DnaRegister, SurfaceRegistrationLandsOnTheTargetSurface)
{
    // Register the tube onto a copy of itself scaled 1.3x taller/wider: the fit should land on
    // the surface (residual tiny) and report the pre-align scale it used.
    const Rig rig = tubeRig();
    const Mesh& m = rig.meshes[0];
    std::vector<float> tv;
    for (float v : m.positions) tv.push_back(v * 1.3f);
    std::vector<int> tf;
    for (const auto& f : m.faces)
        for (size_t k = 1; k + 1 < f.size(); ++k) { tf.push_back(int(m.layouts[f[0]].position)); tf.push_back(int(m.layouts[f[k]].position)); tf.push_back(int(m.layouts[f[k + 1]].position)); }
    const RegisterResult r = registerToSurface(rig, tv, tf);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(r.alignScale, 1.3, 1e-6);
    EXPECT_LT(r.meanResidualPct, 0.5);
}

TEST(DnaRegister, AnEmptyOrBrokenTargetSurfaceIsRefused)
{
    const Rig rig = tubeRig();
    EXPECT_FALSE(registerToSurface(rig, {}, {}).ok);
    EXPECT_FALSE(registerToSurface(rig, {0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 1, 7}).ok);   // index out of range
}

TEST(DnaAlignLimbs, ArmChainRotatesOntoTheTargetDirectionsThroughAParentWithAnOrientation)
{
    Rig rig = tubeRig();
    auto J = [&](const char* n, int parent, std::array<double, 3> t, std::array<double, 4> q = {1, 0, 0, 0}) {
        Joint j; j.name = n; j.parent = parent; j.translation = t; j.orientation = q; rig.joints.push_back(j); return int(rig.joints.size() - 1);
    };
    rig.joints.clear();
    const double h = std::sqrt(0.5);
    const int pel = J("pelvis", -1, {0, 1, 0}, {h, 0, h, 0});            // rooted frame turned 90 deg about Y
    const int ua = J("upperarm_l", pel, {0.0, 0.6, 0.2}, {h, h, 0, 0});  // with its own orientation too
    const int la = J("lowerarm_l", ua, {0.0, 0.3, 0.0});
    J("hand_l", la, {0.0, 0.3, 0.0});
    for (auto& v : rig.meshes[0].skin) v = {{uint16_t(ua), 1.f}};
    const auto before = jointWorldPositions(rig);
    // target: a straight arm along +X from the upper-arm position
    const std::vector<std::pair<QString, std::array<double, 3>>> tj = {
        {"upperarm_l", {before[size_t(ua)][0], before[size_t(ua)][1], before[size_t(ua)][2]}},
        {"lowerarm_l", {before[size_t(ua)][0] + 0.3, before[size_t(ua)][1], before[size_t(ua)][2]}},
        {"hand_l", {before[size_t(ua)][0] + 0.6, before[size_t(ua)][1], before[size_t(ua)][2]}}};
    const LimbAlignment la2 = alignLimbs(rig, tj);
    EXPECT_EQ(la2.chainsAligned, 2);
    const auto after = jointWorldPositions(la2.rig);
    auto dir = [&](int a, int b) { std::array<double, 3> d{after[size_t(b)][0] - after[size_t(a)][0], after[size_t(b)][1] - after[size_t(a)][1], after[size_t(b)][2] - after[size_t(a)][2]}; double l = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); for (double& x : d) x /= l; return d; };
    const auto d1 = dir(ua, la), d2 = dir(la, la + 1);
    EXPECT_NEAR(d1[0], 1.0, 1e-6) << d1[0] << "," << d1[1] << "," << d1[2];
    EXPECT_NEAR(d2[0], 1.0, 1e-6) << d2[0] << "," << d2[1] << "," << d2[2];
    // bone lengths and the upper-arm joint position are untouched; the pelvis does not move
    EXPECT_NEAR(std::abs(after[size_t(la)][0] - after[size_t(ua)][0]), 0.3, 1e-6);
    EXPECT_NEAR(after[size_t(ua)][1], before[size_t(ua)][1], 1e-9);
    // skin follows: a vertex weighted 100 % to the upper arm moved rigidly with that bone
    const auto& P0 = rig.meshes[0].positions; const auto& P1 = la2.rig.meshes[0].positions;
    ASSERT_EQ(P0.size(), P1.size());
    auto dist = [&](const std::vector<float>& P, size_t a, size_t b) { return std::sqrt(std::pow(P[a*3]-P[b*3],2)+std::pow(P[a*3+1]-P[b*3+1],2)+std::pow(P[a*3+2]-P[b*3+2],2)); };
    EXPECT_NEAR(dist(P0, 0, 7), dist(P1, 0, 7), 1e-5);   // rigid: pairwise distances preserved
}

TEST(DnaRegister, ATargetFacingTheOtherWayTurnsTheBodyInsteadOfCrossingSides)
{
    // The target is the rig turned 180 degrees about Y (it faces the other way, so its LEFT
    // bones sit at -X): pairing by name stays right, but the body must be turned first.
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    std::vector<JointAnchor> anchors;
    for (size_t i = 0; i < w.size(); ++i) anchors.push_back({rig.joints[i].name, {-w[i][0], w[i][1], -w[i][2]}});
    const JointRegisterResult r = registerByJoints(rig, anchors, /*alignLimbPose=*/false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(std::abs(r.yawDegrees), 180.0, 1.0);
    bool warned = false;
    for (const QString& m : r.warnings) if (m.contains("turned about Y")) warned = true;
    EXPECT_TRUE(warned);
    // the registered body is the source turned 180 degrees: (x, y, z) -> (-x, y, -z)
    double maxErr = 0;
    const auto& P0 = rig.meshes[0].positions; const auto& P1 = r.positions[0];
    for (size_t i = 0; i + 2 < P0.size(); i += 3)
        maxErr = std::max({maxErr, double(std::abs(P1[i] + P0[i])), double(std::abs(P1[i + 2] + P0[i + 2]))});
    EXPECT_LT(maxErr, 0.05);
    // and the aligned rig faces the same way: its left upper arm is now on -X
    const auto jp = jointWorldPositions(r.alignedRig);
    size_t ul = 0; for (size_t j = 0; j < rig.joints.size(); ++j) if (rig.joints[j].name == "upperarm_l") ul = j;
    EXPECT_LT(jp[ul][0], 0.0);
}

TEST(DnaRegister, AlreadyFacingTheSameWayLeavesTheBodyAlone)
{
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    std::vector<JointAnchor> anchors;
    for (size_t i = 0; i < w.size(); ++i) anchors.push_back({rig.joints[i].name, {w[i][0] * 1.2, w[i][1] * 1.2, w[i][2] * 1.2}});
    const JointRegisterResult r = registerByJoints(rig, anchors, false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.yawDegrees, 0.0);
}

TEST(DnaFitYaw, TurnsRootsMeshesAndNormalsAndIsInvertible)
{
    Rig rig = tubeRig();
    const Rig before = rig;
    yawRig(rig, 3.14159265358979);
    EXPECT_NEAR(rig.meshes[0].positions[0], -before.meshes[0].positions[0], 1e-5f);
    EXPECT_NEAR(rig.meshes[0].positions[1], before.meshes[0].positions[1], 1e-6f);   // height untouched
    EXPECT_NEAR(rig.meshes[0].positions[2], -before.meshes[0].positions[2], 1e-5f);
    yawRig(rig, -3.14159265358979);
    for (size_t i = 0; i < before.meshes[0].positions.size(); ++i)
        EXPECT_NEAR(rig.meshes[0].positions[i], before.meshes[0].positions[i], 1e-5f);
}

namespace {
// A cylinder (radius r, y in [y0,y1]) with a chosen ring/segment count — the SAME shape in a DIFFERENT
// triangulation, outward-facing.
void cylinder(int rings, int segs, std::vector<float>& v, std::vector<int>& f, const std::function<std::array<double, 3>(double, double, double)>& edit)
{
    const double r = 0.3, y0 = -0.5, y1 = 2.5;
    for (int i = 0; i < rings; ++i) {
        const double y = y0 + (y1 - y0) * i / (rings - 1);
        for (int s = 0; s < segs; ++s) {
            const double a = 2 * kPi * (s + 0.5 * (i % 2)) / segs;   // staggered rows: a different vertex layout
            const auto p = edit(r * std::cos(a), y, r * std::sin(a));
            v.insert(v.end(), {float(p[0]), float(p[1]), float(p[2])});
        }
    }
    for (int i = 0; i + 1 < rings; ++i)
        for (int s = 0; s < segs; ++s) {
            const int a = i * segs + s, b = i * segs + (s + 1) % segs, c = (i + 1) * segs + s, d = (i + 1) * segs + (s + 1) % segs;
            f.insert(f.end(), {a, c, b, b, c, d});
        }
}
}  // namespace

TEST(DnaRegister, SmoothWrapRecoversAKnownEditOnAReMeshedTarget)
{
    // Ground truth: the source tube, a smooth non-uniform edit, then the edited shape RE-MESHED (other ring
    // and segment counts, staggered rows). A good wrap puts every ORIGINAL vertex where the edit put it —
    // landing on the surface is not enough (a surface fit can slide along it; that was the first solver).
    auto E = [](double x, double y, double z) {
        const double t = (y + 0.5) / 3.0;
        const double gx = 1.0 + 0.25 * std::exp(-std::pow((t - 0.5) / 0.2, 2));
        return std::array<double, 3>{x * gx, y * 1.05, z * (1.0 + 0.1 * t)};
    };
    const Rig rig = tubeRig();
    std::vector<float> tv;
    std::vector<int> tf;
    cylinder(61, 40, tv, tf, E);
    const RegisterResult r = registerToSurface(rig, tv, tf);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const auto& P = rig.meshes[0].positions;
    const auto& W = r.positions[0];
    ASSERT_EQ(P.size(), W.size());
    double sum = 0, worst = 0;
    for (size_t i = 0; i + 2 < P.size(); i += 3) {
        const auto want = E(P[i], P[i + 1], P[i + 2]);
        const double d = std::sqrt(std::pow(W[i] - want[0], 2) + std::pow(W[i + 1] - want[1], 2) + std::pow(W[i + 2] - want[2], 2));
        sum += d;
        worst = std::max(worst, d);
    }
    const double mean = sum / double(P.size() / 3);
    EXPECT_LT(mean, 0.03) << "mean error " << mean << " (radius 0.3, height 3.0)";
    EXPECT_LT(worst, 0.12) << "worst error " << worst;
}

TEST(DnaRegister, SmoothWrapBeatsDoingNothingByAWideMargin)
{
    auto E = [](double x, double y, double z) { return std::array<double, 3>{x * 1.3, y, z * 0.9}; };
    const Rig rig = tubeRig();
    std::vector<float> tv;
    std::vector<int> tf;
    cylinder(41, 30, tv, tf, E);
    const RegisterResult r = registerToSurface(rig, tv, tf);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const auto& P = rig.meshes[0].positions;
    double fitted = 0, none = 0;
    for (size_t i = 0; i + 2 < P.size(); i += 3) {
        const auto want = E(P[i], P[i + 1], P[i + 2]);
        fitted += std::sqrt(std::pow(r.positions[0][i] - want[0], 2) + std::pow(r.positions[0][i + 1] - want[1], 2) + std::pow(r.positions[0][i + 2] - want[2], 2));
        none += std::sqrt(std::pow(P[i] - want[0], 2) + std::pow(P[i + 1] - want[1], 2) + std::pow(P[i + 2] - want[2], 2));
    }
    EXPECT_LT(fitted, none * 0.25) << "fitted " << fitted << " vs unfitted " << none;
}

TEST(DnaRegister, NricpRemainsAvailableAsAnOption)
{
    const Rig rig = tubeRig();
    std::vector<float> tv;
    std::vector<int> tf;
    cylinder(41, 30, tv, tf, [](double x, double y, double z) { return std::array<double, 3>{x * 1.2, y, z}; });
    RegisterOptions o;
    o.method = RegisterOptions::Method::Nricp;
    const RegisterResult r = registerToSurface(rig, tv, tf, o);
    EXPECT_TRUE(r.ok) << r.error.toStdString();
}

TEST(DnaRegister, JointsThenSurfaceRecoversAnEditThatNeitherRouteAloneDoes)
{
    // The target is an edited, RE-MESHED tube plus a skeleton: the joints carry the proportions (a taller,
    // wider body), the surface carries a bulge the joints cannot express. Skeleton first, then surface.
    auto E = [](double x, double y, double z) {
        const double t = (y + 0.5) / 3.0;
        const double bulge = 1.0 + 0.30 * std::exp(-std::pow((t - 0.5) / 0.12, 2));   // not expressible by a few joints
        return std::array<double, 3>{x * 1.2 * bulge, y * 1.1, z * bulge};
    };
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    std::vector<JointAnchor> anchors;
    for (size_t i = 0; i < w.size(); ++i) {
        // joints sit on the axis: only the global proportions apply to them
        const auto e = E(w[i][0], w[i][1], w[i][2]);
        anchors.push_back({rig.joints[i].name, {w[i][0] * 1.2, w[i][1] * 1.1, w[i][2]}});
        (void)e;
    }
    std::vector<float> tv;
    std::vector<int> tf;
    cylinder(61, 40, tv, tf, E);
    const JointRegisterResult jointsOnly = registerByJoints(rig, anchors, false);
    const JointRegisterResult both = registerJointsThenSurface(rig, anchors, tv, tf, false, 6);
    ASSERT_TRUE(jointsOnly.ok) << jointsOnly.error.toStdString();
    ASSERT_TRUE(both.ok) << both.error.toStdString();
    EXPECT_TRUE(both.surfaceRefined);
    auto meanError = [&](const std::vector<float>& W) {
        const auto& P = rig.meshes[0].positions;
        double sum = 0;
        for (size_t i = 0; i + 2 < P.size(); i += 3) {
            const auto want = E(P[i], P[i + 1], P[i + 2]);
            sum += std::sqrt(std::pow(W[i] - want[0], 2) + std::pow(W[i + 1] - want[1], 2) + std::pow(W[i + 2] - want[2], 2));
        }
        return sum / double(P.size() / 3);
    };
    const double e1 = meanError(jointsOnly.positions[0]), e2 = meanError(both.positions[0]);
    EXPECT_LT(e2, e1 * 0.5) << "joints only " << e1 << ", joints + surface " << e2;
    EXPECT_LT(e2, 0.03);
}

TEST(DnaRegister, JointsThenSurfaceRefusesAnEmptyTarget)
{
    std::vector<std::array<double, 3>> w;
    const Rig rig = stickRig(w);
    std::vector<JointAnchor> anchors;
    for (size_t i = 0; i < w.size(); ++i) anchors.push_back({rig.joints[i].name, w[i]});
    const JointRegisterResult r = registerJointsThenSurface(rig, anchors, {}, {}, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("empty")) << r.error.toStdString();
}

#endif  // ENABLE_OPENRIGLOGIC
