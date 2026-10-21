#include <gtest/gtest.h>

#include "CharacterModel.h"

#include <cmath>
#include <set>

using namespace CharacterGen;

namespace {

/// A tiny synthetic asset set: a 4-joint skeleton, a strip "body" from the hips to the top of the head, and an ICT-like head of
/// 1000 vertices (the chin landmark index is fixed at 966 by the format) split into a few parts.
Assets syntheticAssets()
{
    Assets a;
    a.jointNames = {"body_world", "c_neck", "c_head", "c_jaw"};
    a.jointParents = {-1, 0, 1, 2};
    a.jointOffsets = {0, 100, 0,   0, 44, 0,   0, 10, 3,   0, -3, 2};   // cm: neck at y=144, head at y=154
    a.jointPrerot = {0, 0, 0, 1,  0, 0, 0, 1,  0, 0, 0, 1,  0, 0, 0, 1};
    a.mhrVertices = 24;
    a.mhrShapes = 2;
    for (int v = 0; v < a.mhrVertices; ++v) {   // two columns (x = -5, +5), 12 rows from y=100 to y=172, front of the head at z=9
        const float y = 100.0f + 72.0f * float(v / 2) / 11.0f;
        a.mhrBase.insert(a.mhrBase.end(), {(v % 2) ? 5.0f : -5.0f, y, y > 150.0f ? 9.0f : 0.0f});
    }
    a.mhrShapeVectors.assign(size_t(a.mhrShapes) * a.mhrBase.size(), 0.0f);
    for (int v = 0; v < a.mhrVertices; ++v) a.mhrShapeVectors[size_t(v) * 3] = (v % 2) ? 1.0f : -1.0f;   // shape 0 = widen x
    for (int r = 0; r + 1 < 12; ++r) {
        const int i = r * 2;
        for (int t : {i, i + 1, i + 2, i + 1, i + 3, i + 2}) { a.mhrFaces.push_back(t); a.mhrTexFaces.push_back(t); }
    }
    for (int v = 0; v < a.mhrVertices; ++v) a.mhrTexcoords.insert(a.mhrTexcoords.end(), {float(v % 2), float(v / 2) / 11.0f});
    a.skinCount.assign(size_t(a.mhrVertices), 1);
    for (int v = 0; v < a.mhrVertices; ++v) {
        const bool head = a.mhrBase[size_t(v) * 3 + 1] > 150.0f;
        a.skinJoint.push_back(head ? 2 : 1);
        a.skinWeight.push_back(1.0f);
    }
    a.skinStart.assign(size_t(a.mhrVertices) + 1, 0);
    for (int v = 0; v < a.mhrVertices; ++v) a.skinStart[size_t(v) + 1] = a.skinStart[size_t(v)] + 1;

    a.ictVertices = 1000;
    a.ictIdentities = 2;
    a.ictExpressions = 2;
    for (int v = 0; v < a.ictVertices; ++v) {   // y from +15 (v=0) down to -10 (v=999); chin landmark (966) lands near -9
        const float y = 15.0f - 25.0f * float(v) / 999.0f;
        a.ictNeutral.insert(a.ictNeutral.end(), {float((v % 7) - 3), y, float((v % 5) - 2) + 8.0f});
    }
    a.ictTexcoords = {0, 0, 1, 0, 1, 1, 0, 1};
    for (int v = 0; v + 2 < a.ictVertices; v += 3) {   // one triangle per vertex triple
        a.polySize.push_back(3);
        for (int k = 0; k < 3; ++k) { a.polyVertex.push_back(v + k); a.polyTexcoord.push_back(k); }
    }
    a.polyStart.assign(a.polySize.size() + 1, 0);
    for (size_t p = 0; p < a.polySize.size(); ++p) a.polyStart[p + 1] = a.polyStart[p] + 3;
    a.ictIdentity.assign(size_t(a.ictIdentities) * a.ictNeutral.size(), 0.0f);
    for (int v = 0; v < a.ictVertices; ++v) a.ictIdentity[size_t(v) * 3] = 0.5f;                       // mode 0: +x
    a.ictExpression.assign(size_t(a.ictExpressions) * a.ictNeutral.size(), 0.0f);
    for (int v = 0; v < 300; ++v) a.ictExpression[size_t(v) * 3 + 1] = -0.4f;                          // shape 0 moves 300 verts
    a.expressionNames = {"jawOpen", "mouthSmileLeft"};
    a.parts = {{"face", 0, 599, 0, 199}, {"head_neck", 600, 899, 200, 299}, {"teeth", 900, 999, 300, 332}};
    return a;
}

int vertexCountOf(const Dna::Rig& r, const QString& name)
{
    for (const auto& m : r.meshes)
        if (m.name == name) return int(m.positions.size() / 3);
    return -1;
}

}  // namespace

TEST(CharacterModel, BuildsABodyAndTheHeadGroupsAsSeparateMeshes)
{
    const Assets a = syntheticAssets();
    BuildReport rep;
    QString err;
    const Dna::Rig rig = buildRig(a, Params(), &rep, &err);
    ASSERT_FALSE(rig.meshes.empty()) << err.toStdString();
    EXPECT_EQ(rig.meshes.front().name, "body");
    // the default skeleton keeps only the standard-named joints (the fixture has Neck and Head among its 4)
    ASSERT_EQ(rig.joints.size(), 2u);
    EXPECT_EQ(rig.joints[0].name, "Neck");
    EXPECT_EQ(rig.joints[1].name, "Head");
    EXPECT_EQ(rig.joints[1].parent, 0);
    EXPECT_EQ(rig.blendShapeChannels, a.expressionNames);
    EXPECT_GT(vertexCountOf(rig, "head_skin"), 0);
    EXPECT_GT(vertexCountOf(rig, "head_teeth"), 0);
    EXPECT_EQ(rep.morphTargets, 2);
    std::set<QString> names;
    for (const auto& m : rig.meshes) EXPECT_TRUE(names.insert(m.name).second) << "duplicate mesh " << m.name.toStdString();
    // the rim sits below the head (top at 1.72 m on the fixture) and well above the hips
    EXPECT_LT(rep.neckCutY, 1.60f);
    EXPECT_GT(rep.neckCutY, 1.20f);
}

TEST(CharacterModel, TheFullSkeletonKeepsEveryJointAndItsOwnNames)
{
    const Assets a = syntheticAssets();
    Params p;
    p.standardSkeleton = false;
    p.standardNames = false;
    p.foldHelperJoints = false;
    p.identityBones = false;
    const Dna::Rig rig = buildRig(a, p);
    ASSERT_EQ(rig.joints.size(), 4u);
    EXPECT_EQ(rig.joints[1].name, "c_neck");
    EXPECT_EQ(rig.joints[3].parent, 2);
    // with the real prerotations kept the orientation is the asset's (identity in the fixture) and the offset is parent-relative cm -> m
    EXPECT_NEAR(rig.joints[1].translation[1], 0.44, 1e-5);
}

TEST(CharacterModel, FoldedJointsKeepTheirWorldPositionsForTheirChildren)
{
    // Neck (kept) -> c_jaw (folded away) -> Head (kept): Head's offset must now be measured from Neck, with c_jaw's included
    Assets a = syntheticAssets();
    a.jointNames = {"body_world", "c_neck", "c_jaw", "c_head"};
    a.jointParents = {-1, 0, 1, 2};
    a.jointOffsets = {0, 100, 0,   0, 44, 0,   0, 4, 1,   0, 6, 2};   // cm: Head ends up 10 cm above and 3 cm in front of the neck
    for (auto& w : a.skinJoint) w = (w == 2) ? 3 : w;
    const Dna::Rig rig = buildRig(a, Params());
    ASSERT_EQ(rig.joints.size(), 2u);
    EXPECT_NEAR(rig.joints[1].translation[1], 0.10, 1e-5);
    EXPECT_NEAR(std::fabs(rig.joints[1].translation[2]), 0.03, 1e-5);   // z may be mirrored by the facing turn
}

TEST(CharacterModel, TheBodyLosesItsHeadAboveTheNeckCut)
{
    const Assets a = syntheticAssets();
    BuildReport rep;
    const Dna::Rig rig = buildRig(a, Params(), &rep);
    const Dna::Mesh& body = rig.meshes.front();
    ASSERT_FALSE(body.faces.empty());
    EXPECT_LT(int(body.faces.size()), 22);   // 22 triangles in the strip; the upper ones are gone
    // yawRig turned the character to -Z, so heights are unchanged: no kept triangle has its centroid above the cut
    for (const auto& f : body.faces) {
        float cy = 0;
        for (uint32_t li : f) cy += body.positions[size_t(body.layouts[li].position) * 3 + 1];
        EXPECT_LE(cy / 3.0f, rep.neckCutY + 1e-4f);
    }
}

TEST(CharacterModel, BustDepthKeepsOnlyTheUpperBody)
{
    const Assets a = syntheticAssets();
    BuildReport full, bust;
    buildRig(a, Params(), &full);
    Params p;
    p.bustDepth = 0.20f;
    buildRig(a, p, &bust);
    EXPECT_LT(bust.bodyTriangles, full.bodyTriangles);
    EXPECT_GT(bust.bodyTriangles, 0);
    EXPECT_EQ(bust.headTriangles, full.headTriangles) << "the head is not affected";
}

TEST(CharacterModel, EverySkinRowSumsToOneAndNamesARealJoint)
{
    const Assets a = syntheticAssets();
    const Dna::Rig rig = buildRig(a, Params());
    for (const auto& m : rig.meshes) {
        ASSERT_EQ(m.skin.size(), m.positions.size() / 3) << m.name.toStdString();
        for (const auto& row : m.skin) {
            ASSERT_FALSE(row.empty());
            float sum = 0;
            for (const auto& i : row) { sum += i.weight; EXPECT_LT(i.joint, rig.joints.size()); EXPECT_GE(i.weight, 0.0f); }
            EXPECT_NEAR(sum, 1.0f, 1e-4f) << m.name.toStdString();
        }
    }
}

TEST(CharacterModel, BodyShapeModesMoveTheBodyAndDefaultsDoNot)
{
    const Assets a = syntheticAssets();
    Params p;
    const Dna::Rig base = buildRig(a, p);
    p.bodyShape = {2.0f};
    const Dna::Rig wide = buildRig(a, p);
    ASSERT_EQ(base.meshes.front().positions.size(), wide.meshes.front().positions.size());
    // a 2.0 coefficient of a +-1 cm mode moves the vertex by 2 cm = 0.02 m along x (sign follows the yaw turn)
    EXPECT_NEAR(std::fabs(wide.meshes.front().positions[0] - base.meshes.front().positions[0]), 0.02f, 1e-4f);
    Params q;
    const Dna::Rig again = buildRig(a, q);
    EXPECT_EQ(again.meshes.front().positions, base.meshes.front().positions) << "building is deterministic";
}

TEST(CharacterModel, FaceIdentityChangesTheHeadAndNotTheBody)
{
    const Assets a = syntheticAssets();
    Params p;
    const Dna::Rig base = buildRig(a, p);
    p.faceIdentity = {1.0f};
    const Dna::Rig other = buildRig(a, p);
    EXPECT_EQ(base.meshes.front().positions, other.meshes.front().positions);
    int idx = -1;
    for (size_t i = 0; i < base.meshes.size(); ++i) if (base.meshes[i].name == "head_skin") idx = int(i);
    ASSERT_GE(idx, 0);
    EXPECT_NE(base.meshes[size_t(idx)].positions, other.meshes[size_t(idx)].positions);
}

TEST(CharacterModel, ExpressionsBecomeSparseMorphTargetsScaledWithTheHead)
{
    const Assets a = syntheticAssets();
    BuildReport rep;
    const Dna::Rig rig = buildRig(a, Params(), &rep);
    const Dna::Mesh* face = nullptr;
    for (const auto& m : rig.meshes) if (m.name == "head_skin") face = &m;
    ASSERT_NE(face, nullptr);
    ASSERT_FALSE(face->blendShapeTargets.empty());
    const Dna::BlendShapeTarget& bt = face->blendShapeTargets.front();
    EXPECT_EQ(bt.channel, 0);                       // only the first expression carries deltas in the fixture
    EXPECT_EQ(bt.deltas.size(), bt.vertexIndices.size() * 3);
    for (uint32_t vi : bt.vertexIndices) EXPECT_LT(vi, face->positions.size() / 3);
    // |delta| = 0.4 cm * headScale, in metres
    float maxMag = 0;
    for (float d : bt.deltas) maxMag = std::max(maxMag, std::fabs(d));
    EXPECT_NEAR(maxMag, 0.4f * rep.headScale * 0.01f, 1e-5f);
}

TEST(CharacterModel, FacingPlusZKeepsTheModelsOwnOrientation)
{
    const Assets a = syntheticAssets();
    Params p;
    p.facePlusZ = true;
    const Dna::Rig plus = buildRig(a, p);
    p.facePlusZ = false;
    const Dna::Rig minus = buildRig(a, p);
    // turning 180 degrees about Y negates x and z of every position
    const auto& pp = plus.meshes.front().positions;
    const auto& mm = minus.meshes.front().positions;
    ASSERT_EQ(pp.size(), mm.size());
    for (size_t i = 0; i + 2 < pp.size(); i += 3) {
        EXPECT_NEAR(pp[i], -mm[i], 1e-5f);
        EXPECT_NEAR(pp[i + 1], mm[i + 1], 1e-5f);
        EXPECT_NEAR(pp[i + 2], -mm[i + 2], 1e-5f);
    }
}

TEST(CharacterModel, RandomCoefficientsAreSeededClampedAndLimitedToTheLeadingModes)
{
    const auto a = randomCoefficients(7, 40, 2.0f, 10);
    EXPECT_EQ(a, randomCoefficients(7, 40, 2.0f, 10));
    EXPECT_NE(a, randomCoefficients(8, 40, 2.0f, 10));
    ASSERT_EQ(a.size(), 40u);
    for (size_t i = 10; i < a.size(); ++i) EXPECT_EQ(a[i], 0.0f);
    for (float c : a) EXPECT_LE(std::fabs(c), 6.0f + 1e-5f);   // +-3 sigma with sigma 2
    EXPECT_EQ(randomCoefficients(1, 5, 1.0f, 0).size(), 5u);
}

TEST(CharacterModel, MeshNamesMapToTheirSurface)
{
    Surface s;
    EXPECT_TRUE(surfaceForMesh("body", &s)); EXPECT_EQ(s, Surface::Skin);
    EXPECT_TRUE(surfaceForMesh("head_iris", &s)); EXPECT_EQ(s, Surface::Iris);
    EXPECT_FALSE(surfaceForMesh("hair", &s));
}

TEST(CharacterModel, RefusesAnSkeletonWithoutTheNeckAndHeadJoints)
{
    Assets a = syntheticAssets();
    a.jointNames = {"a", "b", "c", "d"};
    QString err;
    const Dna::Rig rig = buildRig(a, Params(), nullptr, &err);
    EXPECT_TRUE(rig.meshes.empty());
    EXPECT_FALSE(err.isEmpty());
}

TEST(CharacterParsing, CoefficientListsAreParsedAndRangeChecked)
{
    std::vector<float> c;
    QString err;
    EXPECT_TRUE(CharacterGen::parseCoefficients(QStringLiteral("0=1.5, 3=-2"), c, 5, &err));
    ASSERT_EQ(c.size(), 5u);
    EXPECT_FLOAT_EQ(c[0], 1.5f);
    EXPECT_FLOAT_EQ(c[3], -2.0f);
    EXPECT_FLOAT_EQ(c[1], 0.0f);
    EXPECT_FALSE(CharacterGen::parseCoefficients(QStringLiteral("5=1"), c, 5, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(CharacterGen::parseCoefficients(QStringLiteral("x=1"), c, 5, &err));
    EXPECT_FALSE(CharacterGen::parseCoefficients(QStringLiteral("1=nan"), c, 5, &err));
    std::vector<float> none;
    EXPECT_TRUE(CharacterGen::parseCoefficients(QString(), none, 5, &err));
    EXPECT_TRUE(none.empty());
}

TEST(CharacterParsing, ColoursAcceptNamesAndHex)
{
    CharacterGen::Rgb c{};
    EXPECT_TRUE(CharacterGen::parseColour(QStringLiteral("Tan"), CharacterGen::ColourKind::Skin, &c));
    EXPECT_TRUE(CharacterGen::parseColour(QStringLiteral("#ff0000"), CharacterGen::ColourKind::Eye, &c));
    EXPECT_FLOAT_EQ(c[0], 1.0f);
    EXPECT_TRUE(CharacterGen::parseColour(QStringLiteral("blue"), CharacterGen::ColourKind::Skin, &c)) << "falls back to the CSS colour name";
    EXPECT_FALSE(CharacterGen::parseColour(QStringLiteral("zzz"), CharacterGen::ColourKind::Eye, &c));
    EXPECT_TRUE(CharacterGen::colourNames(CharacterGen::ColourKind::Eye).contains(QStringLiteral("hazel")));
}

TEST(CharacterModel, BodyOnlyBuildsNoHeadParts)
{
    const Assets a = syntheticAssets();
    Params p;
    p.bodyOnly = true;
    QString err;
    const Dna::Rig rig = buildRig(a, p, nullptr, &err);
    ASSERT_FALSE(rig.meshes.empty()) << err.toStdString();
    EXPECT_EQ(rig.meshes.size(), 1u);
    EXPECT_EQ(rig.meshes.front().name, "body");
}
