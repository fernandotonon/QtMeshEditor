// Epic #1095 phase 3: ARKit mapping, the face bake, and the scene import.
//
//  * ArkitMap / FaceBake maths run in every build, against a fake rig with a
//    closed-form answer (a 90 degree bend).
//  * Real MetaHuman face checks use Epic's SKM_Face.dna from QTMESH_TEST_DNA_FACE
//    or a local Unreal install and SKIP when absent (it cannot be redistributed).
//  * The scene regression (POSITION in its own buffer) uses the synthetic DNA, so
//    it runs on the Linux lane where a GL context exists.

#include <gtest/gtest.h>

#include "DnaArkitMap.h"
#include "DnaFaceBake.h"
#include "DnaReader.h"
#include "DnaRigLogic.h"
#include "DnaScene.h"

#include "../Manager.h"
#include "../Mocap/FaceCapCanonicalData.h"
#include "../Mocap/FaceCapMapper.h"
#include "../TestHelpers.h"

#ifdef ENABLE_OPENRIGLOGIC
#include "DnaTestFixtures.h"
#endif

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <map>

namespace { constexpr double kPi = 3.14159265358979323846; }  // M_PI is not portable (MSVC/MinGW)

using namespace Dna;

namespace {

// root(0) -> child(1) at (0,1,0). Control "CTRL_expressions.bend" rotates the
// child about Z by 90 degrees * value; "CTRL_expressions.other" does the same, so
// two of them together exceed what the sum of their baked shapes can reproduce.
class BendRig : public RigEvaluator
{
public:
    int jointCount() const override { return 2; }
    int rawControlCount() const override { return 2; }
    QString rawControlName(int i) const override
    {
        return i == 0 ? QStringLiteral("CTRL_expressions.bend") : QStringLiteral("CTRL_expressions.other");
    }
    int rawControlIndex(const QString& n) const override
    {
        for (int i = 0; i < 2; ++i) if (rawControlName(i) == n) return i;
        return -1;
    }
    void resetControls() override { v[0] = v[1] = 0.0f; }
    void setRawControl(int i, float value) override { if (i >= 0 && i < 2) v[i] = value; }
    double calculate() override { return 0.0; }
    JointPose jointPose(int j) const override
    {
        JointPose p;
        p.joint = j;
        if (j == 1) {
            p.translation = {0, 1, 0};
            const double a = (v[0] + v[1]) * kPi / 2.0;     // radians about Z
            p.orientation = {std::cos(a / 2), 0, 0, std::sin(a / 2)};
        }
        return p;
    }
    float v[2] = {0, 0};
};

Rig bendRig()
{
    Rig rig;
    rig.joints.resize(2);
    rig.joints[0].name = "root";
    rig.joints[1].name = "child";
    rig.joints[1].parent = 0;
    Mesh m;
    m.name = "m";
    m.positions = {0, 0, 0,   1, 1, 0,   0, 2, 0,   5, 5, 5};
    m.skin = {{{0, 1.0f}}, {{1, 1.0f}}, {{0, 0.5f}, {1, 0.5f}}, {}};   // last vertex unskinned
    rig.meshes.push_back(m);
    return rig;
}

QString faceDna()
{
    const QByteArray env = qgetenv("QTMESH_TEST_DNA_FACE");
    if (!env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env)))
        return QString::fromLocal8Bit(env);
    const QString ue = QStringLiteral(
        "/Users/Shared/Epic Games/UE_5.7/Engine/Plugins/MetaHuman/MetaHumanCoreTechLib/Content/"
        "ArchetypeDNA/SKM_Face.dna");
    return QFileInfo::exists(ue) ? ue : QString();
}

const FaceBake::BakedShape* find(const FaceBake::Result& r, const char* name)
{
    for (const auto& s : r.shapes) if (s.name == QLatin1String(name)) return &s;
    return nullptr;
}

// The vertex that moved furthest, as (mesh, vertex, delta).
struct Peak { size_t mesh = 0; uint32_t vertex = 0; std::array<float, 3> delta{}; double len = 0; };
Peak peakOf(const FaceBake::BakedShape& s)
{
    Peak p;
    for (size_t m = 0; m < s.perMesh.size(); ++m)
        for (const auto& d : s.perMesh[m]) {
            const double l = std::sqrt(double(d.delta[0]) * d.delta[0] + double(d.delta[1]) * d.delta[1] +
                                       double(d.delta[2]) * d.delta[2]);
            if (l > p.len) p = {m, d.vertex, d.delta, l};
        }
    return p;
}

}  // namespace

// ---- ARKit map -------------------------------------------------------------

TEST(DnaArkitMap, CoversEveryArkitChannelExactlyOnce)
{
    const auto map = ArkitMap::defaultMap();
    QSet<QString> want;
    for (int i = 1; i < FaceCap::kBlendshapeCount; ++i)
        want.insert(QString::fromLatin1(FaceCap::kBlendshapeNames[size_t(i)]));
    QSet<QString> got;
    for (const auto& e : map) {
        EXPECT_FALSE(got.contains(e.arkit)) << e.arkit.toStdString();
        got.insert(e.arkit);
        EXPECT_FALSE(e.controls.isEmpty()) << e.arkit.toStdString();
        for (const auto& c : e.controls) {
            EXPECT_FALSE(c.first.isEmpty());
            EXPECT_TRUE(std::isfinite(c.second) && c.second > 0.0) << e.arkit.toStdString();
        }
        EXPECT_EQ(e.approximate, !e.note.isEmpty()) << e.arkit.toStdString();
    }
    EXPECT_EQ(got, want);
    EXPECT_EQ(map.size(), 51);
    EXPECT_TRUE(ArkitMap::approximateChannels(map).contains("mouthClose"));
    EXPECT_FALSE(ArkitMap::exactChannels(map).isEmpty());
}

TEST(DnaArkitMap, SidesAndEyeDirectionsAreMirroredCorrectly)
{
    auto controlsOf = [](const char* arkit) {
        for (const auto& e : ArkitMap::defaultMap())
            if (e.arkit == QLatin1String(arkit)) return e.controls;
        return QList<QPair<QString, double>>();
    };
    // Left eye looking IN (toward the nose) is MetaHuman's left eye looking RIGHT.
    EXPECT_EQ(controlsOf("eyeLookInLeft").first().first, "eyeLookRightL");
    EXPECT_EQ(controlsOf("eyeLookOutLeft").first().first, "eyeLookLeftL");
    EXPECT_EQ(controlsOf("eyeLookInRight").first().first, "eyeLookLeftR");
    EXPECT_EQ(controlsOf("mouthSmileLeft").first().first, "mouthCornerPullL");
    EXPECT_EQ(controlsOf("mouthSmileRight").first().first, "mouthCornerPullR");
}

TEST(DnaArkitMap, OverrideReplacesDropsAndRejectsTypos)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto write = [&](const char* name, const char* json) {
        QFile f(dir.filePath(name));
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(json);
        return f.fileName();
    };
    const auto ok = ArkitMap::loadOverride(write("ok.json",
        R"({"schema":"qtmesh-dna-arkit-map-v1","map":{"jawOpen":{"jawOpen":1.0,"jawOpenExtreme":0.25},"mouthClose":{}}})"));
    ASSERT_TRUE(ok.ok) << ok.error.toStdString();
    EXPECT_EQ(ok.overridden, QStringList{"jawOpen"});
    EXPECT_EQ(ok.dropped, QStringList{"mouthClose"});
    for (const auto& e : ok.entries) if (e.arkit == "jawOpen") EXPECT_EQ(e.controls.size(), 2);

    // A misspelled top-level key must not silently apply nothing.
    const auto noMap = ArkitMap::loadOverride(write("nomap.json", R"({"schema":"qtmesh-dna-arkit-map-v1","maps":{}})"));
    EXPECT_FALSE(noMap.ok);
    EXPECT_TRUE(noMap.error.contains("map"));
    EXPECT_FALSE(ArkitMap::loadOverride(write("typo.json", R"({"map":{"jawOpne":{"jawOpen":1}}})")).ok);
    EXPECT_FALSE(ArkitMap::loadOverride(write("bad.json", R"({"map":{"jawOpen":5}})")).ok);
    EXPECT_FALSE(ArkitMap::loadOverride(write("schema.json", R"({"schema":"nope","map":{}})")).ok);
    EXPECT_FALSE(ArkitMap::loadOverride(write("nan.json", R"({"map":{"jawOpen":{"jawOpen":"x"}}})")).ok);
    EXPECT_FALSE(ArkitMap::loadOverride(dir.filePath("missing.json")).ok);
}

TEST(DnaArkitMap, EveryBakedNameIsDrivableByFaceCaptureAndLipsync)
{
    // The baked morph targets carry the ARKit names, so the existing mapper must
    // resolve every channel with nothing left over.
    QStringList names;
    for (const auto& e : ArkitMap::defaultMap()) names << e.arkit;
    const FaceCapMapper::Mapping m = FaceCapMapper::build(names);
    EXPECT_EQ(m.channels.size(), 51);
    EXPECT_TRUE(m.unmatchedCanonical.isEmpty()) << m.unmatchedCanonical.join(",").toStdString();
    EXPECT_TRUE(m.unmatchedMesh.isEmpty());
}

// ---- bake maths (fake rig) -------------------------------------------------

TEST(DnaFaceBake, BendMatchesTheClosedFormAndStaysSparse)
{
    BendRig ev;
    const Rig rig = bendRig();
    const auto r = FaceBake::bake(rig, ev, {{"bend", {{"bend", 1.0}}}});
    ASSERT_EQ(r.shapes.size(), 1u);
    const auto& s = r.shapes[0];
    ASSERT_EQ(s.perMesh.size(), 1u);
    std::map<uint32_t, std::array<float, 3>> d;
    for (const auto& e : s.perMesh[0]) d[e.vertex] = e.delta;
    EXPECT_EQ(d.count(0), 0u);           // on the root: never moves
    EXPECT_EQ(d.count(3), 0u);           // unskinned: never moves
    ASSERT_EQ(d.count(1), 1u);
    EXPECT_NEAR(d[1][0], -1.0f, 1e-5f);  // (1,1,0) about pivot (0,1,0) by +90 -> (0,2,0)
    EXPECT_NEAR(d[1][1], 1.0f, 1e-5f);
    ASSERT_EQ(d.count(2), 1u);           // half-weighted vertex gets half the displacement
    EXPECT_NEAR(d[2][0], -0.5f, 1e-5f);
    EXPECT_NEAR(d[2][1], -0.5f, 1e-5f);
    EXPECT_EQ(s.vertices, 2);
    EXPECT_NEAR(s.maxDeltaMm, std::sqrt(2.0) * 1000.0, 1e-3);
}

TEST(DnaFaceBake, FullNameAndShortNameBothResolve)
{
    BendRig ev;
    const Rig rig = bendRig();
    const auto r = FaceBake::bake(rig, ev, {{"a", {{"bend", 1.0}}}, {"b", {{"CTRL_expressions.bend", 1.0}}}});
    ASSERT_EQ(r.shapes.size(), 2u);
    EXPECT_EQ(r.shapes[0].vertices, r.shapes[1].vertices);
}

TEST(DnaFaceBake, MissingControlsAreReportedAndAnAllMissingShapeIsSkipped)
{
    BendRig ev;
    const Rig rig = bendRig();
    const auto r = FaceBake::bake(rig, ev, {{"half", {{"bend", 1.0}, {"ghost", 1.0}}}, {"none", {{"ghost", 1.0}}}});
    ASSERT_EQ(r.shapes.size(), 1u);
    EXPECT_EQ(r.shapes[0].missingControls, QStringList{"ghost"});
    EXPECT_FALSE(r.warnings.isEmpty());
}

TEST(DnaFaceBake, AppendAsBlendShapesAddsChannelsOnceWithMatchingTargets)
{
    BendRig ev;
    Rig rig = bendRig();
    const auto r = FaceBake::bake(rig, ev, {{"bend", {{"bend", 1.0}}}, {"other", {{"other", 0.5}}}});
    EXPECT_EQ(FaceBake::appendAsBlendShapes(rig, r), 2);
    EXPECT_EQ(rig.blendShapeChannels, (QStringList{"bend", "other"}));
    ASSERT_EQ(rig.meshes[0].blendShapeTargets.size(), 2u);
    EXPECT_EQ(rig.meshes[0].blendShapeTargets[0].channel, 0);
    EXPECT_EQ(rig.meshes[0].blendShapeTargets[0].deltas.size(),
              rig.meshes[0].blendShapeTargets[0].vertexIndices.size() * 3);
    EXPECT_EQ(FaceBake::appendAsBlendShapes(rig, r), 0);      // no duplicates
    EXPECT_EQ(rig.blendShapeChannels.size(), 2);
}

TEST(DnaFaceBake, LinearityErrorSeesTheNonlinearityOfRotations)
{
    BendRig ev;
    const Rig rig = bendRig();
    // Two controls that each rotate the same joint 90 degrees: the rig composes
    // them into 180, but the sum of two baked deltas stays at 2 * the 90 delta.
    const auto lr = FaceBake::linearityError(rig, ev, {{"a", {{"bend", 1.0}}}, {"b", {{"other", 1.0}}}});
    EXPECT_GT(lr.maxMm, 100.0);
    EXPECT_GT(lr.combinedMaxMm, 100.0);
    // A single control is exactly linear with itself.
    const auto one = FaceBake::linearityError(rig, ev, {{"a", {{"bend", 1.0}}}});
    EXPECT_NEAR(one.maxMm, 0.0, 1e-3);
}

// ---- real MetaHuman face (skips without Epic's asset) ------------------------

#ifdef ENABLE_OPENRIGLOGIC

namespace {
struct FaceFixture {
    bool present = false;       // an asset path was found
    bool ok = false;            // ... and it loaded, evaluated and baked
    QString problem;
    Rig rig;
    std::unique_ptr<RigEvaluator> ev;
    FaceBake::Result bake;
    FaceFixture()
    {
        const QString path = faceDna();
        if (path.isEmpty()) return;
        present = true;
        ReadOptions o;
        const ReadResult r = read(path, o);
        if (!r.ok) { problem = QStringLiteral("read failed: ") + r.error; return; }
        rig = r.rig;
        QString err;
        ev = RigEvaluator::create(path, o, &err);
        if (!ev) { problem = QStringLiteral("evaluator failed: ") + err; return; }
        bake = FaceBake::bake(rig, *ev, FaceBake::arkitSpecs(ArkitMap::defaultMap()));
        ok = true;
    }
};
FaceFixture& face() { static FaceFixture f; return f; }

// False (with a note) only when NO asset exists. An asset that is present but does
// not load is a real failure, not a reason to pass quietly.
bool haveFace()
{
    if (!face().present) {
        SUCCEED() << "no MetaHuman face DNA available — Epic's asset is not redistributable; "
                     "set QTMESH_TEST_DNA_FACE to run";
        return false;
    }
    EXPECT_TRUE(face().ok) << face().problem.toStdString();
    return face().ok;
}
}  // namespace

TEST(DnaFaceBakeReal, EveryArkitControlExistsInTheMetaHumanFace)
{
    if (!haveFace()) return;
    for (const auto& s : face().bake.shapes)
        EXPECT_TRUE(s.missingControls.isEmpty()) << s.name.toStdString() << " : "
                                                 << s.missingControls.join(",").toStdString();
    EXPECT_GE(face().bake.shapes.size(), 50u);       // mouthClose may drive nothing at LOD0
}

TEST(DnaFaceBakeReal, JawOpenMovesTheChinDownByAFewCentimetresAtMost)
{
    if (!haveFace()) return;
    const auto* s = find(face().bake, "jawOpen");
    ASSERT_NE(s, nullptr);
    const Peak p = peakOf(*s);
    EXPECT_LT(p.delta[1], 0.0f);                     // moves DOWN
    EXPECT_GT(s->maxDeltaMm, 8.0);
    EXPECT_LT(s->maxDeltaMm, 60.0);
}

TEST(DnaFaceBakeReal, LeftAndRightShapesAreMirroredAcrossTheFace)
{
    if (!haveFace()) return;
    auto peakX = [&](const char* name) {
        const auto* s = find(face().bake, name);
        EXPECT_NE(s, nullptr) << name;
        if (!s) return 0.0f;
        const Peak p = peakOf(*s);
        return face().rig.meshes[p.mesh].positions[size_t(p.vertex) * 3];
    };
    // The character's left is +X (it faces +Z, Y up).
    EXPECT_GT(peakX("mouthSmileLeft"), 0.0f);
    EXPECT_LT(peakX("mouthSmileRight"), 0.0f);
    EXPECT_GT(peakX("eyeBlinkLeft"), 0.0f);
    EXPECT_LT(peakX("eyeBlinkRight"), 0.0f);
    const auto* l = find(face().bake, "mouthSmileLeft");
    const auto* r = find(face().bake, "mouthSmileRight");
    ASSERT_TRUE(l && r);
    EXPECT_NEAR(l->maxDeltaMm, r->maxDeltaMm, 0.3 * std::max(l->maxDeltaMm, r->maxDeltaMm));
}

TEST(DnaFaceBakeReal, SummedShapesStayCloseToTheJointRigForTypicalExpressions)
{
    if (!haveFace()) return;
    const auto specs = FaceBake::arkitSpecs(ArkitMap::defaultMap());
    auto pick = [&](std::initializer_list<const char*> names) {
        QList<FaceBake::ShapeSpec> v;
        for (const char* n : names) for (const auto& s : specs) if (s.name == QLatin1String(n)) v << s;
        return v;
    };
    const auto smileOpen = FaceBake::linearityError(face().rig, *face().ev,
                                                    pick({"jawOpen", "mouthSmileLeft", "mouthSmileRight"}));
    EXPECT_LT(smileOpen.maxMm, 8.0);                  // measured 4.2 mm
    EXPECT_LT(smileOpen.meanMm, 1.0);                 // measured 0.36 mm
    const auto blinkBrow = FaceBake::linearityError(face().rig, *face().ev, pick({"eyeBlinkLeft", "browDownLeft"}));
    EXPECT_LT(blinkBrow.maxMm, 1.0);                  // measured 0.07 mm
    // Documented limit: pucker + jaw is the worst combination and clearly nonlinear.
    const auto puckerOpen = FaceBake::linearityError(face().rig, *face().ev, pick({"mouthPucker", "jawOpen"}));
    EXPECT_GT(puckerOpen.maxMm, 2.0);
    EXPECT_LT(puckerOpen.maxMm, 40.0);
}

TEST(DnaFaceBakeReal, BakingAllArkitShapesIsFast)
{
    if (!haveFace()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = FaceBake::bake(face().rig, *face().ev, FaceBake::arkitSpecs(ArkitMap::defaultMap()));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    RecordProperty("bake_ms", std::to_string(ms));
    EXPECT_GE(r.shapes.size(), 50u);
    EXPECT_LT(ms, 3000.0);                            // ~22 ms measured
}

// ---- scene (needs GL; synthetic DNA, so it runs wherever GL exists) ----------

class DnaSceneMorphTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()); }
    void TearDown() override
    {
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
};

TEST_F(DnaSceneMorphTest, MorphedMeshKeepsPositionInItsOwnTightlyPackedBuffer)
{
    // Ogre's software pose animation addresses positions as base + vertex * 3
    // floats. EditableMesh interleaves position/normal/uv, which made every pose
    // offset garbage ("the mesh explodes the moment a morph state is enabled").
    const ReadResult rr = readFromBuffer(DnaTest::makeSyntheticDna());
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    ImportReport rep;
    QString err;
    Ogre::SceneNode* node = importRigToScene(rr.rig, "dna_morph_layout_test", &rep, &err);
    ASSERT_NE(node, nullptr) << err.toStdString();
    EXPECT_EQ(rep.morphTargets, 1);

    auto* entity = static_cast<Ogre::Entity*>(node->getAttachedObject(0));
    ASSERT_NE(entity, nullptr);
    const Ogre::MeshPtr mesh = entity->getMesh();
    ASSERT_GT(mesh->getNumSubMeshes(), 0);
    EXPECT_GT(mesh->getPoseCount(), 0u);
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        const Ogre::VertexData* vd = mesh->getSubMesh(i)->vertexData;
        ASSERT_NE(vd, nullptr);
        const Ogre::VertexElement* pos = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        ASSERT_NE(pos, nullptr);
        EXPECT_EQ(pos->getOffset(), 0u);
        EXPECT_EQ(pos->getType(), Ogre::VET_FLOAT3);
        for (const Ogre::VertexElement& e : vd->vertexDeclaration->getElements())
            if (e.getSemantic() != Ogre::VES_POSITION)
                EXPECT_NE(e.getSource(), pos->getSource()) << "element shares the position buffer";
        // Normals / UVs must have survived the split.
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL), nullptr);
        EXPECT_NE(vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES), nullptr);
    }
}

TEST_F(DnaSceneMorphTest, ASkinnedMorphedImportKeepsItsSkeletonAndBoneAssignments)
{
    const ReadResult rr = readFromBuffer(DnaTest::makeSyntheticDna());
    ASSERT_TRUE(rr.ok);
    Ogre::SceneNode* node = importRigToScene(rr.rig, "dna_morph_skin_test");
    ASSERT_NE(node, nullptr);
    auto* entity = static_cast<Ogre::Entity*>(node->getAttachedObject(0));
    EXPECT_TRUE(entity->hasSkeleton());
    EXPECT_GT(entity->getMesh()->getSubMesh(0)->getBoneAssignments().size(), 0u);
}

#endif  // ENABLE_OPENRIGLOGIC
