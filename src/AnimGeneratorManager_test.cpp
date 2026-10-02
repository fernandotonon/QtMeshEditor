// #524 — AnimGeneratorManager: binding generators to the live scene,
// mute / bake / undo, runtime targets, sidecar round trip, and the panel QML.

#include <gtest/gtest.h>

#include "AnimGeneratorManager.h"
#include "AnimGenerators.h"
#include "AnimationControlController.h"
#include "LightManager.h"
#include "Manager.h"
#include "MaterialEditorQML.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "PropertiesPanelController.h"
#include "ThemeManager.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/AnimGeneratorCommands.h"

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QJsonArray>
#include <QSet>
#include <QTemporaryDir>

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreTechnique.h>

#include <cmath>

using namespace AnimGen;

namespace {

AnimGeneratorManager* mgr() { return AnimGeneratorManager::instance(); }

// The track holds 30 fps samples and Ogre interpolates linearly between them,
// so check at exact sample times: 9/30 s and 21/30 s, where sin(2πt) = ±0.951.
constexpr float kT = 9.0f / 30.0f;
constexpr float kT2 = 21.0f / 30.0f;
const float kS = float(std::sin(2.0 * 3.14159265358979323846 * 0.3));

struct Key { float t; Ogre::Vector3 p; Ogre::Quaternion r; };

std::vector<Key> boneKeys(Ogre::Entity* e, const char* clip, const char* bone)
{
    std::vector<Key> out;
    Ogre::Animation* a = e->getSkeleton()->getAnimation(clip);
    const unsigned short h = e->getSkeleton()->getBone(bone)->getHandle();
    if (!a->hasNodeTrack(h)) return out;
    Ogre::NodeAnimationTrack* t = a->getNodeTrack(h);
    for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i) {
        auto* k = t->getNodeKeyFrame(i);
        out.push_back({k->getTime(), k->getTranslate(), k->getRotation()});
    }
    return out;
}

Ogre::Vector3 boneTranslateAt(Ogre::Entity* e, const char* clip, const char* bone, float t)
{
    Ogre::Animation* a = e->getSkeleton()->getAnimation(clip);
    Ogre::NodeAnimationTrack* tr = a->getNodeTrack(e->getSkeleton()->getBone(bone)->getHandle());
    Ogre::TransformKeyFrame kf(nullptr, t);
    tr->getInterpolatedKeyFrame(a->_getTimeIndex(t), &kf);
    return kf.getTranslate();
}

Generator sineOnChildY(const QString& entity, double amp = 0.5)
{
    Generator g;
    g.type = Type::Sine;
    EXPECT_TRUE(parseTarget(QStringLiteral("bone:%1/Child/position.y@TestAnim").arg(entity), &g.target));
    g.amplitude = amp;
    g.frequency = 1.0;
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// No scene needed
// ---------------------------------------------------------------------------

TEST(AnimGeneratorManagerStandalone, RejectsMissingTargetsWithoutTouchingState)
{
    mgr()->clear();
    Generator g;
    g.type = Type::Sine;
    ASSERT_TRUE(parseTarget(QStringLiteral("node:NoSuchNode/position.y"), &g.target));
    const auto r = mgr()->add(g, false);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("NoSuchNode"))) << r.error.toStdString();
    EXPECT_EQ(mgr()->count(), 0);
    EXPECT_FALSE(mgr()->lastOk());
    EXPECT_FALSE(mgr()->remove(QStringLiteral("nope"), false).ok);
    EXPECT_FALSE(mgr()->bake(QStringLiteral("nope"), false).ok);
}

TEST(AnimGeneratorManagerStandalone, TrackKeysGroupGeneratorsThatShareATrack)
{
    Target a, b, c;
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:E/B/position.y@C"), &a));
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:E/B/rotation.z@C"), &b));
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:E/B/rotation.z@Other"), &c));
    EXPECT_EQ(AnimGeneratorManager::trackKey(a), AnimGeneratorManager::trackKey(b))
        << "two channels of one bone track share one base";
    EXPECT_NE(AnimGeneratorManager::trackKey(a), AnimGeneratorManager::trackKey(c));
    Target m1, m2;
    ASSERT_TRUE(parseTarget(QStringLiteral("morph:E/smile/weight"), &m1));
    ASSERT_TRUE(parseTarget(QStringLiteral("morph:E/jaw/weight"), &m2));
    EXPECT_EQ(AnimGeneratorManager::trackKey(m1), AnimGeneratorManager::trackKey(m2))
        << "morph targets share the weight clip's submesh tracks";
    EXPECT_TRUE(AnimGeneratorManager::sidecarPath(QStringLiteral("/a/b/hero.glb")).endsWith(QStringLiteral("/a/b/hero.generators.json")));
}

TEST(AnimGeneratorManagerStandalone, DocCommandSkipsItsFirstRedo)
{
    mgr()->clear();
    const QJsonObject empty = mgr()->document();
    AnimGeneratorDocCommand cmd(QStringLiteral("x"), empty, empty);
    cmd.redo(); // skipped — must not crash or change anything
    cmd.undo();
    cmd.redo();
    EXPECT_EQ(mgr()->count(), 0);
}

// ---------------------------------------------------------------------------
// Live scene
// ---------------------------------------------------------------------------

class AnimGeneratorSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        mgr()->clear();
    }
    void TearDown() override
    {
        mgr()->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
};

TEST_F(AnimGeneratorSceneTest, BoneSineIsWrittenIntoTheTrackAndMuteRestoresTheBaseExactly)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Bone");
    ASSERT_NE(e, nullptr);
    const auto before = boneKeys(e, "TestAnim", "Child");
    ASSERT_EQ(before.size(), 3u);

    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Bone")), false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(mgr()->isBound(r.id));
    const auto live = boneKeys(e, "TestAnim", "Child");
    EXPECT_EQ(live.size(), 31u) << "dense samples over the 1 s clip at 30 fps";
    // base(t) + 0.5·sin(2πt): the base translation is x-only, so y is the sine.
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT2).y, -0.5f * kS, 1e-4f);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", 0.5f).x, 0.5f, 1e-4f) << "the base motion is kept";

    ASSERT_TRUE(mgr()->setEnabled(r.id, false, false).ok);
    const auto muted = boneKeys(e, "TestAnim", "Child");
    ASSERT_EQ(muted.size(), before.size()) << "muting restores the ORIGINAL keys, not resampled ones";
    for (size_t i = 0; i < before.size(); ++i) {
        EXPECT_FLOAT_EQ(muted[i].t, before[i].t);
        EXPECT_LT((muted[i].p - before[i].p).length(), 1e-6f);
        // Component-wise: Quaternion::equals takes acos of a float dot product,
        // which reports ~5e-4 rad between two BIT-IDENTICAL quaternions.
        EXPECT_FLOAT_EQ(muted[i].r.w, before[i].r.w);
        EXPECT_FLOAT_EQ(muted[i].r.x, before[i].r.x);
        EXPECT_FLOAT_EQ(muted[i].r.y, before[i].r.y);
        EXPECT_FLOAT_EQ(muted[i].r.z, before[i].r.z);
    }
    ASSERT_TRUE(mgr()->setEnabled(r.id, true, false).ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f) << "un-mute re-materialises";
}

TEST_F(AnimGeneratorSceneTest, ParameterEditsNeverAccumulate)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Edit");
    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Edit")), false);
    ASSERT_TRUE(r.ok);
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(mgr()->setParams(r.id, {{QStringLiteral("amplitude"), QStringLiteral("0.2")}}, false).ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.2f * kS, 1e-4f)
        << "every edit re-materialises from the base, not from the previous result";
}

TEST_F(AnimGeneratorSceneTest, TwoGeneratorsOnOneTrackShareTheBase)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Two");
    const auto a = mgr()->add(sineOnChildY(QStringLiteral("GEN_Two"), 0.5), false);
    Generator ramp;
    ramp.type = Type::Ramp;
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:GEN_Two/Child/position.z@TestAnim"), &ramp.target));
    ramp.rampFrom = 0.0;
    ramp.rampTo = 2.0;
    const auto b = mgr()->add(ramp, false);
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", 0.5f).z, 1.0f, 1e-4f);
    // Muting the sine must not wipe the ramp (the second generator's base is
    // the ORIGINAL track, not the first generator's output).
    ASSERT_TRUE(mgr()->setEnabled(a.id, false, false).ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", 0.5f).z, 1.0f, 1e-4f);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.0f * kS, 1e-4f) << "the sine is gone";
}

TEST_F(AnimGeneratorSceneTest, EveryOperationIsOneUndoStep)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Undo");
    auto* um = UndoManager::getSingleton();
    ASSERT_NE(um, nullptr);
    const auto pristine = boneKeys(e, "TestAnim", "Child");

    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Undo")));
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(mgr()->setParams(r.id, {{QStringLiteral("amplitude"), QStringLiteral("1.0")}}).ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 1.0f * kS, 1e-4f);

    um->undo(); // edit
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f);
    um->undo(); // add
    EXPECT_EQ(mgr()->count(), 0);
    EXPECT_EQ(boneKeys(e, "TestAnim", "Child").size(), pristine.size()) << "undoing the add restores the track";
    um->redo(); // add
    EXPECT_EQ(mgr()->count(), 1);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f);
    um->redo(); // edit
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 1.0f * kS, 1e-4f);
    um->undo();
    um->undo();
}

TEST_F(AnimGeneratorSceneTest, BakeKeepsTheMotionWhenTheGeneratorIsMutedOrRemoved)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Bake");
    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Bake")));
    ASSERT_TRUE(r.ok);
    const float liveY = boneTranslateAt(e, "TestAnim", "Child", kT).y;
    ASSERT_TRUE(mgr()->bake(r.id).ok);
    const Generator* g = mgr()->find(r.id);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->baked);
    EXPECT_FALSE(g->enabled) << "baked generators stay attached but inactive";
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, liveY, 1e-5f)
        << "the baked keys reproduce the generator with the generator muted";
    EXPECT_FALSE(mgr()->setEnabled(r.id, true, false).ok) << "a baked generator cannot be re-enabled";

    // Undo the bake → live again; redo → baked again; both keep the motion.
    UndoManager::getSingleton()->undo();
    EXPECT_FALSE(mgr()->find(r.id)->baked);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, liveY, 1e-5f);
    UndoManager::getSingleton()->redo();
    EXPECT_TRUE(mgr()->find(r.id)->baked);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, liveY, 1e-5f);

    ASSERT_TRUE(mgr()->remove(r.id, false).ok);
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, liveY, 1e-5f)
        << "removing a BAKED generator leaves its keyframes (they are the track now)";
}

TEST_F(AnimGeneratorSceneTest, NodePathCreatesAClipThatFollowsTheCurveAndIsRemovedWithIt)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Node");
    const QString nodeName = QString::fromStdString(e->getParentSceneNode()->getName());
    Generator g;
    g.type = Type::FollowPath;
    ASSERT_TRUE(parseTarget(QStringLiteral("node:%1/position").arg(nodeName), &g.target));
    g.pathPoints = {{0, 0, 0}, {4, 0, 0}, {4, 0, 4}};
    g.duration = 2.0;
    g.orientToPath = true;
    const auto r = mgr()->add(g, false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(NodeAnimationManager::instance()->listClips().contains(QStringLiteral("Generators")));
    Ogre::Animation* anim = Manager::getSingleton()->getSceneMgr()->getAnimation("Generators");
    ASSERT_NE(anim, nullptr);
    EXPECT_GE(anim->getLength(), 2.0f);
    Ogre::NodeAnimationTrack* track = nullptr;
    for (const auto& kv : anim->_getNodeTrackList())
        if (kv.second->getAssociatedNode() == e->getParentSceneNode()) track = kv.second;
    ASSERT_NE(track, nullptr);
    Ogre::TransformKeyFrame end(nullptr, 2.0f);
    track->getInterpolatedKeyFrame(anim->_getTimeIndex(2.0f), &end);
    EXPECT_LT((end.getTranslate() - Ogre::Vector3(4, 0, 4)).length(), 1e-3f) << "ends on the last point";
    // Facing along the last chord (+Z): local -Z points to +Z → yaw 180°.
    const Ogre::Vector3 fwd = end.getRotation() * Ogre::Vector3::NEGATIVE_UNIT_Z;
    EXPECT_GT(fwd.z, 0.9f);

    ASSERT_TRUE(mgr()->remove(r.id, false).ok);
    EXPECT_FALSE(NodeAnimationManager::instance()->listClips().contains(QStringLiteral("Generators")))
        << "the clip the generator created goes with it";
}

TEST_F(AnimGeneratorSceneTest, MorphWeightKeysCarryEveryPoseOfTheSubmesh)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Morph");
    Ogre::MeshPtr mesh = e->getMesh();
    Ogre::Pose* smile = mesh->createPose(0, "smile");
    smile->addVertex(0, Ogre::Vector3(0, 0, 0.1f));
    Ogre::Pose* jaw = mesh->createPose(0, "jaw");
    jaw->addVertex(1, Ogre::Vector3(0, -0.1f, 0));
    // A base clip keying only 'jaw' at 0.7 for the whole second.
    ASSERT_TRUE(MorphAnimationManager::writeWeightKeyOn(e, "MorphAnim", "jaw", 0.0f, 0.7f));
    ASSERT_TRUE(MorphAnimationManager::writeWeightKeyOn(e, "MorphAnim", "jaw", 1.0f, 0.7f));

    Generator g;
    g.type = Type::Ramp;
    ASSERT_TRUE(parseTarget(QStringLiteral("morph:GEN_Morph/smile/weight"), &g.target));
    g.rampFrom = 0.0;
    g.rampTo = 1.0;
    const auto r = mgr()->add(g, false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    Ogre::Animation* anim = mesh->getAnimation("MorphAnim");
    ASSERT_TRUE(anim->hasVertexTrack(0));
    Ogre::VertexAnimationTrack* track = anim->getVertexTrack(0);
    ASSERT_GT(track->getNumKeyFrames(), 10);
    const unsigned short smileIdx = 0, jawIdx = 1;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = track->getVertexPoseKeyFrame(i);
        float s = -1, j = -1;
        for (const auto& ref : kf->getPoseReferences()) {
            if (ref.poseIndex == smileIdx) s = ref.influence;
            if (ref.poseIndex == jawIdx) j = ref.influence;
        }
        EXPECT_NEAR(j, 0.7f, 1e-4f) << "jaw kept on every key (Ogre would drop a missing pose to 0), t=" << kf->getTime();
        EXPECT_NEAR(s, kf->getTime(), 1e-3f) << "smile ramps 0→1";
    }
    ASSERT_TRUE(mgr()->remove(r.id, false).ok);
    EXPECT_EQ(anim->getVertexTrack(0)->getNumKeyFrames(), 2) << "removal restores the base keys";
}

TEST_F(AnimGeneratorSceneTest, RuntimeMaterialTargetIsDrivenByTheClockAndRestored)
{
    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        "GEN_Mat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setDiffuse(Ogre::ColourValue(0.2f, 0.3f, 0.4f, 1.0f));

    Generator g;
    g.type = Type::Ramp;
    ASSERT_TRUE(parseTarget(QStringLiteral("material:GEN_Mat/diffuse.r"), &g.target));
    g.rampFrom = 0.0;
    g.rampTo = 0.5;
    g.duration = 2.0;
    const auto r = mgr()->add(g, false);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(mgr()->runtimeValue(g.target, 1.0), 0.2 + 0.25, 1e-6) << "base + ramp";

    mgr()->setClock(0.0);
    mgr()->tick(1.0, true); // playing: the clock advances to 1 s
    EXPECT_NEAR(pass->getDiffuse().r, 0.45f, 1e-4f);
    EXPECT_NEAR(pass->getDiffuse().g, 0.3f, 1e-6f) << "only the bound channel changes";

    // Bake → the curve keeps driving the property with the generator inactive.
    ASSERT_TRUE(mgr()->bake(r.id, false).ok);
    mgr()->setClock(0.0);
    mgr()->tick(1.5, true);
    EXPECT_NEAR(pass->getDiffuse().r, float(mgr()->runtimeValue(g.target, 1.5)), 1e-4f);
    EXPECT_NEAR(pass->getDiffuse().r, 0.2f + 0.375f, 2e-3f);

    ASSERT_TRUE(mgr()->remove(r.id, false).ok);
    EXPECT_EQ(mgr()->count(), 0);
}

TEST_F(AnimGeneratorSceneTest, RuntimeLightNoiseMovesTheIntensity)
{
    LightManager* lm = LightManager::getSingletonPtr();
    ASSERT_NE(lm, nullptr);
    const LightHandle h = lm->createLight(Ogre::Light::LT_POINT, QStringLiteral("GEN_Light"));
    ASSERT_TRUE(h.isValid());
    h.light->setPowerScale(1.0f);
    Generator g;
    g.type = Type::Noise;
    ASSERT_TRUE(parseTarget(QStringLiteral("light:%1/intensity").arg(h.name), &g.target));
    g.amplitude = 0.5;
    g.noiseFrequency = 3.0;
    g.duration = 4.0;
    ASSERT_TRUE(mgr()->add(g, false).ok);
    double lo = 1e9, hi = -1e9;
    mgr()->setClock(0.0);
    for (int i = 0; i < 60; ++i) {
        mgr()->tick(1.0 / 30.0, true);
        lo = std::min(lo, double(h.light->getPowerScale()));
        hi = std::max(hi, double(h.light->getPowerScale()));
    }
    EXPECT_GT(hi - lo, 0.1) << "the intensity actually flickers";
    EXPECT_LE(hi, 1.5 + 1e-6);
    EXPECT_GE(lo, 0.5 - 1e-6);
    mgr()->clear();
    EXPECT_NEAR(h.light->getPowerScale(), 1.0f, 1e-6f) << "clearing restores the base intensity";
}

TEST_F(AnimGeneratorSceneTest, PoseWeightBlendsTowardTheSavedPose)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Pose");
    Ogre::Bone* child = e->getSkeleton()->getBone("Child");
    const Ogre::Vector3 bind = child->getInitialPosition();
    child->setPosition(bind + Ogre::Vector3(2, 0, 0));
    ASSERT_TRUE(PoseLibrary::instance()->savePose(e, QStringLiteral("Lean")));
    child->setPosition(bind);

    Generator g;
    g.type = Type::Ramp;
    ASSERT_TRUE(parseTarget(QStringLiteral("pose:GEN_Pose/Lean/weight"), &g.target));
    g.duration = 2.0;
    ASSERT_TRUE(mgr()->add(g, false).ok);
    mgr()->setClock(0.0);
    mgr()->tick(1.0, true); // weight 0.5
    EXPECT_NEAR(child->getPosition().x, bind.x + 1.0f, 1e-4f);
    mgr()->clear();
    PoseLibrary::instance()->forgetEntity(e);
}

TEST_F(AnimGeneratorSceneTest, DocumentRoundTripsAndSidecarRebindsRenamedObjects)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Doc");
    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Doc")), false);
    ASSERT_TRUE(r.ok);
    const QJsonObject doc = mgr()->document();
    ASSERT_TRUE(mgr()->applyDocument(doc));
    EXPECT_EQ(mgr()->document(), doc) << "applying the current document is a no-op";
    EXPECT_NEAR(boneTranslateAt(e, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f);

    QTemporaryDir dir;
    const QString asset = QDir(dir.path()).filePath(QStringLiteral("hero.glb"));
    QJsonObject meta{{QStringLiteral("entity"), QStringLiteral("OldHero")}};
    ASSERT_TRUE(mgr()->writeSidecar(asset, {QStringLiteral("GEN_Doc")}, meta));
    EXPECT_TRUE(QFile::exists(AnimGeneratorManager::sidecarPath(asset)));
    EXPECT_EQ(AnimGeneratorManager::sidecarMeta(asset).value(QStringLiteral("entity")).toString(),
              QStringLiteral("OldHero"));
    EXPECT_FALSE(mgr()->writeSidecar(asset, {QStringLiteral("Unrelated")}))
        << "nothing aimed at that object: no file, and the stale one is removed";
    EXPECT_FALSE(QFile::exists(AnimGeneratorManager::sidecarPath(asset)));

    // Re-import under a different entity name.
    ASSERT_TRUE(mgr()->writeSidecar(asset, {}, meta));
    mgr()->clear();
    Ogre::Entity* e2 = createAnimatedTestEntity("GEN_Doc2");
    const int n = mgr()->loadSidecar(asset, {{QStringLiteral("GEN_Doc"), QStringLiteral("GEN_Doc2")}});
    EXPECT_EQ(n, 1);
    ASSERT_EQ(mgr()->count(), 1);
    EXPECT_EQ(mgr()->generators().front().target.object, QStringLiteral("GEN_Doc2"));
    EXPECT_TRUE(mgr()->isBound(mgr()->generators().front().id));
    EXPECT_NEAR(boneTranslateAt(e2, "TestAnim", "Child", kT).y, 0.5f * kS, 1e-4f);
}

TEST_F(AnimGeneratorSceneTest, LoadingTwoSidecarsNeverCreatesDuplicateIds)
{
    createAnimatedTestEntity("GEN_Ids");
    QTemporaryDir dir;
    const QString asset = QDir(dir.path()).filePath(QStringLiteral("a.glb"));
    // A file holding gen_1 + gen_2 (two channels of one bone).
    ASSERT_TRUE(mgr()->add(sineOnChildY(QStringLiteral("GEN_Ids")), false).ok);
    Generator z = sineOnChildY(QStringLiteral("GEN_Ids"));
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:GEN_Ids/Child/position.z@TestAnim"), &z.target));
    ASSERT_TRUE(mgr()->add(z, false).ok);
    ASSERT_TRUE(mgr()->writeSidecar(asset));
    // Load it into a document that already holds gen_1 + gen_2: the old code
    // renamed file gen_1 → gen_2 and kept file gen_2, so the merged document
    // had two gen_2 and the whole load was silently dropped.
    const int n = mgr()->loadSidecar(asset);
    EXPECT_EQ(n, 2);
    ASSERT_EQ(mgr()->count(), 4) << "all four generators present";
    QSet<QString> ids;
    for (const auto& g : mgr()->generators()) ids.insert(g.id);
    EXPECT_EQ(ids.size(), 4) << "ids are unique";
}

TEST_F(AnimGeneratorSceneTest, ReplacingTheSceneDropsItsGenerators)
{
    createAnimatedTestEntity("GEN_Clear");
    ASSERT_TRUE(mgr()->add(sineOnChildY(QStringLiteral("GEN_Clear")), false).ok);
    ASSERT_EQ(mgr()->count(), 1);
    emit Manager::getSingleton()->sceneClearing();
    EXPECT_EQ(mgr()->count(), 0) << "a replaced scene takes its generators with it";
    EXPECT_TRUE(mgr()->document().value(QStringLiteral("bases")).toArray().isEmpty());
}

TEST_F(AnimGeneratorSceneTest, UpdateIsAtomic)
{
    Ogre::Entity* e = createAnimatedTestEntity("GEN_Atomic");
    const auto r = mgr()->add(sineOnChildY(QStringLiteral("GEN_Atomic")), false);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(mgr()->bake(r.id, false).ok);
    const bool on = true;
    const auto u = mgr()->update(r.id, {{QStringLiteral("amplitude"), QStringLiteral("2")}}, &on, false);
    EXPECT_FALSE(u.ok) << "re-enabling a baked generator is refused";
    EXPECT_DOUBLE_EQ(mgr()->find(r.id)->amplitude, 0.5) << "…and the params in the same request were NOT applied";

    // params + mute land as ONE undo step.
    const auto r2 = mgr()->add(sineOnChildY(QStringLiteral("GEN_Atomic"), 0.3));
    ASSERT_TRUE(r2.ok);
    const bool off = false;
    ASSERT_TRUE(mgr()->update(r2.id, {{QStringLiteral("amplitude"), QStringLiteral("0.9")}}, &off).ok);
    EXPECT_FALSE(mgr()->find(r2.id)->enabled);
    UndoManager::getSingleton()->undo();
    EXPECT_TRUE(mgr()->find(r2.id)->enabled);
    EXPECT_DOUBLE_EQ(mgr()->find(r2.id)->amplitude, 0.3) << "one undo reverts both";
    (void)e;
}

TEST_F(AnimGeneratorSceneTest, PanelQmlLoadsWithoutErrors)
{
    qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimationControlController::qmlInstance(e, s); });
    qmlRegisterSingletonType<AnimGeneratorManager>("PropertiesPanel", 1, 0, "AnimGeneratorManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimGeneratorManager::qmlInstance(e, s); });
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AnimationControl/GeneratorsPanel.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}
