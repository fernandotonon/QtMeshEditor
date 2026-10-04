#ifdef ENABLE_MOCAP
#include <gtest/gtest.h>
#include "AnimationMerger.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "ModelTurntableRenderer.h"
#include "MotionInbetween.h"
#include "TestHelpers.h"
#include "Mocap/BodyPoseGeometry.h"
#include "Mocap/BodyPoseStream.h"
#include "Mocap/BodyRootMotion.h"
#include "Mocap/MocapRecorder.h"
#include "Mocap/MocapPoseIkFk.h"
#include "commands/RecordMocapClipCommand.h"
#include <OgreBone.h>
#include <OgreKeyFrame.h>
#include <OgreSkeletonManager.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

namespace {
using Landmarks = std::array<float, 99>;
void point(Landmarks& p, int lm, const Ogre::Vector3& canonical)
{
    p[lm * 3] = canonical.x;
    p[lm * 3 + 1] = -canonical.y;
    p[lm * 3 + 2] = -canonical.z;
}
Landmarks standing()
{
    Landmarks p{};
    point(p, 23, {0.1f, 0, 0}); point(p, 24, {-0.1f, 0, 0});
    point(p, 11, {0.2f, 0.5f, 0}); point(p, 12, {-0.2f, 0.5f, 0});
    point(p, 13, {0.23f, 0.23f, 0}); point(p, 14, {-0.23f, 0.23f, 0});
    point(p, 15, {0.24f, 0, 0}); point(p, 16, {-0.24f, 0, 0});
    point(p, 25, {0.1f, -0.4f, 0}); point(p, 26, {-0.1f, -0.4f, 0});
    point(p, 27, {0.1f, -0.8f, 0}); point(p, 28, {-0.1f, -0.8f, 0});
    point(p, 31, {0.1f, -0.8f, 0.15f}); point(p, 32, {-0.1f, -0.8f, 0.15f});
    point(p, 7, {0.08f, 0.65f, 0}); point(p, 8, {-0.08f, 0.65f, 0});
    point(p, 0, {0, 0.68f, 0.12f});
    return p;
}
BodyLiveFrame sample(const Landmarks& p, double time = 0.0)
{
    BodyLiveFrame f;
    f.valid = true; f.timeSec = time; f.world = p; f.visibility.fill(1.f);
    const auto result = PoseIK::Solver{}.solveFrame(p.data(), f.visibility.data());
    f.quats = result.quats; f.resolvedMask = result.resolvedMask;
    return f;
}
void project(BodyLiveFrame& f, float scale = 200.f, float dx = 0.f, float dy = 0.f)
{
    f.imageWidth = 640; f.imageHeight = 480;
    for (int lm = 0; lm < 33; ++lm) {
        f.imageXy[lm * 2] = 320.f + dx + f.world[lm * 3] * scale;
        f.imageXy[lm * 2 + 1] = 240.f + dy + f.world[lm * 3 + 1] * scale;
    }
}

class BodyRetargeterGeometryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        static int serial = 0;
        name = "body_geometry_" + std::to_string(serial++);
    }
    void TearDown() override
    {
        ModelTurntableRenderer::shutdown();
        if (entity) Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
        if (node) Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
        if (mesh) Ogre::MeshManager::getSingleton().remove(mesh->getHandle());
        if (skeleton) Ogre::SkeletonManager::getSingleton().remove(skeleton->getHandle());
    }
    void rig(float yaw = 0.f, bool rolled = false, bool rooted = false)
    {
        skeleton = Ogre::SkeletonManager::getSingleton().create(name,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        const Ogre::Quaternion rotate(Ogre::Degree(yaw), Ogre::Vector3::UNIT_Y);
        if (rooted) {
            auto* root = skeleton->createBone("ArmatureRoot");
            root->setOrientation(Ogre::Quaternion(Ogre::Degree(55), Ogre::Vector3::UNIT_Z));
            root->setScale(2,2,2);
            root->_update(true, true);
        }
        auto bone = [&](const char* n, Ogre::Vector3 world, const char* parent) {
            auto* b = skeleton->createBone(n);
            const Ogre::Quaternion orientation = rotate * (rolled
                ? Ogre::Quaternion(Ogre::Degree(17.f + 7.f * b->getHandle()), Ogre::Vector3::UNIT_Z)
                : Ogre::Quaternion::IDENTITY);
            world = rotate * world;
            if (parent) {
                auto* p = skeleton->getBone(parent);
                p->addChild(b);
                b->setPosition((p->_getDerivedOrientation().Inverse() * (world - p->_getDerivedPosition()))
                               / p->_getDerivedScale());
                b->setOrientation(p->_getDerivedOrientation().Inverse() * orientation);
            } else { b->setPosition(world); b->setOrientation(orientation); }
            if (rooted && std::string(n) == "Hips") b->setScale(.5f,.5f,.5f);
            b->_update(true, true);
        };
        bone("Hips", {0, 1, 0}, rooted ? "ArmatureRoot" : nullptr);
        bone("Spine", {0, 1.2f, 0}, "Hips");
        bone("Spine2", {0, 1.5f, 0}, "Spine");
        bone("Neck", {0, 1.6f, 0}, "Spine2");
        bone("Head", {0, 1.75f, 0}, "Neck");
        bone("LeftArm", {.2f, 1.5f, 0}, "Spine2");
        bone("LeftForeArm", {.5f, 1.5f, 0}, "LeftArm");
        bone("LeftHand", {.75f, 1.5f, 0}, "LeftForeArm");
        bone("RightArm", {-.2f, 1.5f, 0}, "Spine2");
        bone("RightForeArm", {-.5f, 1.5f, 0}, "RightArm");
        bone("RightHand", {-.75f, 1.5f, 0}, "RightForeArm");
        bone("LeftUpLeg", {.1f, 1, 0}, "Hips");
        bone("LeftLeg", {.1f, .6f, 0}, "LeftUpLeg");
        bone("LeftFoot", {.1f, .2f, 0}, "LeftLeg");
        bone("LeftToeBase", {.1f, .2f, .15f}, "LeftFoot");
        bone("RightUpLeg", {-.1f, 1, 0}, "Hips");
        bone("RightLeg", {-.1f, .6f, 0}, "RightUpLeg");
        bone("RightFoot", {-.1f, .2f, 0}, "RightLeg");
        bone("RightToeBase", {-.1f, .2f, .15f}, "RightFoot");
        skeleton->setBindingPose();
        mesh = Ogre::MeshManager::getSingleton().createManual(name,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        auto vb = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        const float vertices[] = {0,0,0, 1,0,0, 0,1,0};
        vb->writeData(0, sizeof(vertices), vertices);
        mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vb);
        mesh->sharedVertexData->vertexCount = 3;
        auto ib = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        const uint16_t indices[] = {0,1,2};
        ib->writeData(0, sizeof(indices), indices);
        sub->indexData->indexBuffer = ib; sub->indexData->indexCount = 3;
        mesh->_notifySkeleton(skeleton);
        for (size_t vertex = 0; vertex < 3; ++vertex) {
            Ogre::VertexBoneAssignment assignment;
            assignment.vertexIndex = vertex;
            assignment.boneIndex = skeleton->getBone("Hips")->getHandle();
            assignment.weight = 1.f;
            mesh->addBoneAssignment(assignment);
        }
        mesh->_compileBoneAssignments();
        mesh->_setBounds(Ogre::AxisAlignedBox(-1,0,-1,1,2,1)); mesh->load();
        entity = Manager::getSingleton()->getSceneMgr()->createEntity(name, mesh);
        node = Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->createChildSceneNode();
        node->attachObject(entity);
    }
    void apply(BodyRetargeter& rt, const BodyLiveFrame& f)
    {
        auto* skel = entity->getSkeleton();
        for (const auto& [handle, local] : rt.evaluateFrame(f.quats, f.resolvedMask, 0,
                f.world.data(), f.visibility.data())) {
            auto* bone = skel->getBone(handle);
            bone->setManuallyControlled(true); bone->setOrientation(local);
        }
        for (auto* root : skel->getRootBones()) root->_update(true, true);
        skel->_updateTransforms();
        skel->_notifyManualBonesDirty();
        if (auto* states = entity->getAllAnimationStates()) states->_notifyDirty();
        entity->_updateAnimation();
    }
    void calibrate(BodyRetargeter& rt, const BodyLiveFrame& f)
    {
        ASSERT_TRUE(rt.valid());
        rt.setNeutralReference(f.quats, f.resolvedMask, f.world.data(), f.visibility.data());
        ASSERT_TRUE(rt.hasNeutralReference());
    }
    void direction(const char* a, const char* b, const BodyLiveFrame& f, int from, int to,
                   const Ogre::Quaternion& basis = Ogre::Quaternion::IDENTITY)
    {
        auto* skel = entity->getSkeleton();
        const auto actual = (skel->getBone(b)->_getDerivedPosition() - skel->getBone(a)->_getDerivedPosition()).normalisedCopy();
        const auto expected = basis * Ogre::Vector3(f.world[to*3] - f.world[from*3],
            f.world[from*3+1] - f.world[to*3+1], f.world[from*3+2] - f.world[to*3+2]).normalisedCopy();
        EXPECT_GT(actual.dotProduct(expected), 0.9999f) << a << " actual=" << actual << " expected=" << expected;
    }
    std::string name;
    Ogre::SkeletonPtr skeleton;
    Ogre::MeshPtr mesh;
    Ogre::Entity* entity = nullptr;
    Ogre::SceneNode* node = nullptr;
};
} // namespace

TEST_F(BodyRetargeterGeometryTest, RelaxedCalibrationDoesNotSnapArmsToTPose)
{
    rig(); BodyRetargeter rt(entity->getSkeleton());
    auto f = sample(standing()); calibrate(rt, f);
    for (int i = 0; i < 3; ++i) {
        apply(rt, f);
        direction("LeftArm", "LeftForeArm", f, 11, 13);
        direction("LeftForeArm", "LeftHand", f, 13, 15);
        direction("RightArm", "RightForeArm", f, 12, 14);
    }
}

TEST_F(BodyRetargeterGeometryTest, HighKneeAndShinFollowCapturedDirectionsExactly)
{
    rig(); BodyRetargeter rt(entity->getSkeleton());
    auto f = sample(standing()); calibrate(rt, f);
    for (int side = 0; side < 2; ++side) {
        auto landmarks = standing();
        const float x = side == 0 ? .1f : -.1f;
        point(landmarks, 25 + side, {x, .25f, .3f});
        point(landmarks, 27 + side, {x, -.15f, .3f});
        point(landmarks, 31 + side, {x, -.15f, .45f});
        f = sample(landmarks); apply(rt, f);
        direction(side == 0 ? "LeftUpLeg" : "RightUpLeg", side == 0 ? "LeftLeg" : "RightLeg", f, 23 + side, 25 + side);
        direction(side == 0 ? "LeftLeg" : "RightLeg", side == 0 ? "LeftFoot" : "RightFoot", f, 25 + side, 27 + side);
        direction(side == 0 ? "LeftFoot" : "RightFoot", side == 0 ? "LeftToeBase" : "RightToeBase", f, 27 + side, 31 + side);
    }
}

TEST_F(BodyRetargeterGeometryTest, FullTurnsPreserveAnatomicalSidesOnRolledRotatedRig)
{
    rig(73.f, true); BodyRetargeter rt(entity->getSkeleton());
    auto neutral = sample(standing()); calibrate(rt, neutral);
    const Ogre::Quaternion rigYaw(Ogre::Degree(73), Ogre::Vector3::UNIT_Y);
    const auto bindHip = entity->getSkeleton()->getBone("Hips")->getInitialOrientation();
    for (int angle = 0; angle <= 360; angle += 15) {
        auto p = standing();
        const Ogre::Quaternion yaw(Ogre::Degree(static_cast<float>(angle)), Ogre::Vector3::UNIT_Y);
        for (int lm = 0; lm < 33; ++lm) {
            const auto v = yaw * Ogre::Vector3(p[lm*3], -p[lm*3+1], -p[lm*3+2]);
            point(p, lm, v);
        }
        const auto f = sample(p); apply(rt, f);
        direction("LeftUpLeg", "LeftLeg", f, 23, 25, rigYaw);
        direction("RightUpLeg", "RightLeg", f, 24, 26, rigYaw);
        direction("LeftArm", "LeftForeArm", f, 11, 13, rigYaw);
        const auto actual = entity->getSkeleton()->getBone("Hips")->_getDerivedOrientation();
        const auto expected = rigYaw * yaw * rigYaw.Inverse() * bindHip;
        EXPECT_GT(std::abs(actual.Dot(expected)), .9999f) << "turn " << angle;
    }
}

TEST_F(BodyRetargeterGeometryTest, OccludedOrNonfiniteLegHoldsLastLocalPose)
{
    rig(); BodyRetargeter rt(entity->getSkeleton());
    auto f = sample(standing()); calibrate(rt, f);
    point(f.world, 25, {.1f, 0, .4f}); apply(rt, f);
    auto* thigh = entity->getSkeleton()->getBone("LeftUpLeg");
    const auto previous = thigh->getOrientation();
    f.visibility[25] = 0.f;
    f.world[25*3] = std::numeric_limits<float>::quiet_NaN();
    apply(rt, f);
    EXPECT_GT(std::abs(previous.Dot(thigh->getOrientation())), .9999f);
    rt.resetLiveNeutral();
    EXPECT_FALSE(rt.hasNeutralReference());
}

TEST_F(BodyRetargeterGeometryTest, HeadingCalibrationDoesNotEraseCapturedTorsoLean)
{
    rig(); BodyRetargeter rt(entity->getSkeleton());
    auto p = standing();
    const Ogre::Quaternion lean(Ogre::Degree(25), Ogre::Vector3::UNIT_X);
    for (int lm = 0; lm < 33; ++lm)
        point(p, lm, lean * Ogre::Vector3(p[lm*3], -p[lm*3+1], -p[lm*3+2]));
    const auto f = sample(p); calibrate(rt, f); apply(rt, f);
    EXPECT_GT(std::abs(entity->getSkeleton()->getBone("Hips")->_getDerivedOrientation().Dot(lean)), .9999f);
    direction("LeftUpLeg", "LeftLeg", f,23,25);
}

TEST_F(BodyRetargeterGeometryTest, InvalidNeutralCannotReplaceWorkingCalibration)
{
    rig(); BodyRetargeter rt(entity->getSkeleton());
    auto invalid = sample(Landmarks{});
    rt.setNeutralReference(invalid.quats, invalid.resolvedMask, invalid.world.data(), invalid.visibility.data());
    EXPECT_FALSE(rt.hasNeutralReference());
    auto valid = sample(standing()); calibrate(rt, valid);
    rt.setNeutralReference(invalid.quats, invalid.resolvedMask, invalid.world.data(), invalid.visibility.data());
    EXPECT_TRUE(rt.hasNeutralReference());
    apply(rt, valid); direction("LeftUpLeg", "LeftLeg", valid, 23,25);
}

TEST_F(BodyRetargeterGeometryTest, RootTranslationUsesRotatedScaledParentNotHipAxes)
{
    rig(0.f, false, true);
    auto* hip = entity->getSkeleton()->getBone("Hips");
    const auto originalPosition = hip->_getDerivedPosition();
    BodyRetargeter rt(entity->getSkeleton());
    const auto reference = sample(standing()); calibrate(rt, reference);
    auto* instanceHip = entity->getSkeleton()->getBone("Hips");
    ASSERT_NE(instanceHip->getParent(), nullptr);
    const Ogre::Vector3 movement(.2f,.3f,.4f);
    instanceHip->setPosition(instanceHip->getInitialPosition() + rt.rootOffset(movement));
    auto pose = reference;
    const Ogre::Quaternion turn(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    for (int lm = 0; lm < 33; ++lm)
        point(pose.world, lm, turn * Ogre::Vector3(reference.world[lm*3], -reference.world[lm*3+1], -reference.world[lm*3+2]));
    apply(rt, pose);
    const auto actual = instanceHip->_getDerivedPosition() - originalPosition;
    EXPECT_LT((actual - movement).length(), 1e-5f)
        << "actual=" << actual << " initial=" << instanceHip->getInitialPosition()
        << " offset=" << rt.rootOffset(movement)
        << " parent=" << instanceHip->getParent()->_getDerivedOrientation()
        << " parentScale=" << instanceHip->getParent()->_getDerivedScale();
}

TEST(BodyPoseStream, QuaternionAndLandmarkGeometryShareOneFilter)
{
    BodyPoseStream stream; stream.setSmoothing(1.0);
    PoseSample pose; pose.confidence = 1; pose.visibility.fill(1); pose.world = standing();
    const auto first = stream.process(pose,640,480);
    point(pose.world,25,{.1f,0,.4f}); pose.timeSec = 1.0 / 30.0;
    const auto frame = stream.process(pose,640,480);
    const auto& q = frame.quats[PoseIK::LHip];
    const Ogre::Vector3 aim = Ogre::Quaternion(q[3],q[0],q[1],q[2]) * Ogre::Vector3::UNIT_Y;
    const Ogre::Vector3 expected(frame.world[75] - frame.world[69],
        frame.world[70] - frame.world[76], frame.world[71] - frame.world[77]);
    EXPECT_GT(aim.dotProduct(expected.normalisedCopy()), .9999f);
    EXPECT_LT(frame.world[77], first.world[77]);
    EXPECT_GT(frame.world[77], pose.world[77]); // filtered but still in the same direction
}

TEST(BodyRootMotion, TranslationDepthAndOcclusion)
{
    auto f = sample(standing()); project(f);
    BodyRootMotion root; ASSERT_TRUE(root.calibrate(f));
    project(f, 200.f, 40.f, -20.f); f.timeSec = 1;
    const auto moved = root.evaluate(f);
    EXPECT_NEAR(moved.x, .2f, .002f); EXPECT_NEAR(moved.y, .1f, .002f);
    EXPECT_NEAR(moved.z, 0.f, .002f);
    f.visibility[23] = 0.f; f.timeSec = 2;
    EXPECT_LT((root.evaluate(f) - moved).length(), 1e-5f);
    f.visibility[23] = 1.f; f.timeSec = 3; project(f, 250.f);
    EXPECT_GT(root.evaluate(f).z, .45f);
}

TEST_F(BodyRetargeterGeometryTest, RecordingKeepsCalibrationRootMovementAndSampleTimes)
{
    rig(45.f, true);
    auto reference = sample(standing(), 0); project(reference);
    auto first = reference; first.timeSec = 10; project(first, 200, 40, 0);
    auto last = first; last.timeSec = 12; project(last, 200, 80, 0);
    MocapRecorder::BodyRecordOptions options; options.clipName = "Captured"; options.neutralFrame = reference;
    RecordBodyClipCommand command(entity->getName(), std::vector<BodyLiveFrame>{first,last}, 30, options);
    command.redo(); ASSERT_TRUE(command.report().ok()) << command.report().error.toStdString();
    auto* animation = skeleton->getAnimation("Captured");
    ASSERT_NE(animation, nullptr); EXPECT_NEAR(animation->getLength(), 2, 1e-5f);
    auto* track = animation->getNodeTrack(skeleton->getBone("Hips")->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 2);
    EXPECT_GT(track->getNodeKeyFrame(0)->getTranslate().length(), .15f);
    EXPECT_GT(track->getNodeKeyFrame(1)->getTranslate().length(), .30f);
    command.undo(); EXPECT_FALSE(skeleton->hasAnimation("Captured"));
    command.redo(); EXPECT_TRUE(skeleton->hasAnimation("Captured"));
    options.rootMotion = false; options.clipName = "InPlace";
    ASSERT_TRUE(MocapRecorder::recordBodyLive(entity, {first,last}, 30, options).ok());
    const auto* inplace = skeleton->getAnimation("InPlace")->getNodeTrack(skeleton->getBone("Hips")->getHandle());
    EXPECT_LT(inplace->getNodeKeyFrame(1)->getTranslate().length(), 1e-6f);
}

TEST_F(BodyRetargeterGeometryTest, RecordedFaceHeadDoesNotDoubleBodyYaw)
{
    rig();
    auto first = sample(standing(), 0); auto last = first; last.timeSec = 1;
    const Ogre::Quaternion turn(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    for (int lm = 0; lm < 33; ++lm)
        point(last.world, lm, turn * Ogre::Vector3(first.world[lm*3], -first.world[lm*3+1], -first.world[lm*3+2]));
    first.headWorldValid = last.headWorldValid = true;
    first.headWorldRotation = {0,0,0,1};
    last.headWorldRotation = {turn.x,turn.y,turn.z,turn.w};
    MocapRecorder::BodyRecordOptions options; options.clipName = "TurnWithHead";
    ASSERT_TRUE(MocapRecorder::recordBodyLive(entity, {first,last}, 30, options).ok());
    EXPECT_GT(std::abs(skeleton->getBone("Hips")->_getDerivedOrientation().Dot(Ogre::Quaternion::IDENTITY)), .9999f);
    EXPECT_GT(std::abs(skeleton->getBone("Head")->_getDerivedOrientation().Dot(Ogre::Quaternion::IDENTITY)), .9999f);
    auto* animation = skeleton->getAnimation("TurnWithHead");
    skeleton->reset(true);
    animation->apply(skeleton.get(), 1.f);
    for (auto* root : skeleton->getRootBones()) root->_update(true, true);
    EXPECT_GT(std::abs(skeleton->getBone("Head")->_getDerivedOrientation().Dot(turn)), .9999f);
}

TEST_F(BodyRetargeterGeometryTest, InvalidTakeCannotOverwriteExistingAnimation)
{
    rig();
    auto* original = skeleton->createAnimation("KeepMe", 3.f);
    MocapRecorder::BodyRecordOptions options; options.clipName = "KeepMe";
    options.replaceExisting = true;
    auto first = sample(standing(), 0), last = sample(standing(), 1);
    first.visibility[23] = last.visibility[23] = 0.f;
    EXPECT_FALSE(MocapRecorder::recordBodyLive(entity, {first,last}, 30, options).ok());
    ASSERT_TRUE(skeleton->hasAnimation("KeepMe"));
    EXPECT_EQ(skeleton->getAnimation("KeepMe"), original);
    first = sample(standing(), 0); last = sample(standing(), -1);
    EXPECT_FALSE(MocapRecorder::recordBodyLive(entity, {first,last}, 30, options).ok());
    EXPECT_EQ(skeleton->getAnimation("KeepMe"), original);
}

TEST_F(BodyRetargeterGeometryTest, RecordedRootMovementUpdatesCullingBounds)
{
    rig();
    auto first = sample(standing(),0), last = sample(standing(),1);
    ASSERT_TRUE(MocapRecorder::recordBodyLive(entity, {first,last}, 30).ok());
    ASSERT_TRUE(entity->getUpdateBoundingBoxFromSkeleton());
    const auto original = entity->getBoundingBox().getCenter();
    BodyRetargeter rt(entity->getSkeleton()); calibrate(rt, first);
    auto* hip = entity->getSkeleton()->getBone("Hips");
    hip->setPosition(hip->getInitialPosition() + rt.rootOffset({10,0,0}));
    apply(rt, first);
    EXPECT_NEAR(entity->getBoundingBox().getCenter().x - original.x, 10.f, 1e-4f);
}

TEST(PoseIKDebug, ForwardKinematicsMatchesCapturedJointsAfterTurnAndKneeLift)
{
    auto p = standing();
    point(p, 25, {.1f, .25f, .3f}); point(p, 27, {.1f, -.15f, .3f});
    const Ogre::Quaternion turn(Ogre::Degree(115), Ogre::Vector3::UNIT_Y);
    for (int lm = 0; lm < 33; ++lm)
        point(p, lm, turn * Ogre::Vector3(p[lm*3], -p[lm*3+1], -p[lm*3+2]));
    const auto f = sample(p);
    std::array<std::array<float,3>,33> canonical;
    PoseIK::Solver::canonicalizeMediaPipeWorld(p.data(), canonical);
    std::array<std::array<float,3>,22> fk;
    MocapPoseIkFk::fkPoseIkJoints(f.quats, f.resolvedMask, canonical, fk);
    for (const auto& pair : {std::array<int,2>{PoseIK::LKnee,25}, {PoseIK::LFoot,27},
                            {PoseIK::RKnee,26}, {PoseIK::RFoot,28},
                            {PoseIK::LElbow,13}, {PoseIK::LHand,15}})
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(fk[pair[0]][axis], canonical[pair[1]][axis], 1e-5f);
}

// Opt-in real inference: extract a video's frames with ffmpeg, then set
// QTMESH_MOCAP_TEST_FRAMES and QTMESH_MOCAP_TEST_MODELS. No camera/downloads.
TEST_F(BodyRetargeterGeometryTest, ReplayVideoFrames)
{
    const auto directory = qEnvironmentVariable("QTMESH_MOCAP_TEST_FRAMES");
    if (directory.isEmpty()) GTEST_SKIP() << "set QTMESH_MOCAP_TEST_FRAMES for real inference";
    PoseCapPredictor predictor;
    ASSERT_TRUE(predictor.load(qEnvironmentVariable("QTMESH_MOCAP_TEST_MODELS"))) << predictor.lastError().toStdString();
    const auto model = qEnvironmentVariable("QTMESH_MOCAP_TEST_MESH");
    if (model.isEmpty()) rig(0.f, true);
    else {
        const auto before = Manager::getSingleton()->getEntities();
        MeshImporterExporter::importer({model});
        for (auto* candidate : Manager::getSingleton()->getEntities())
            if (!before.contains(candidate) && candidate->hasSkeleton()) { entity = candidate; break; }
        ASSERT_NE(entity, nullptr) << "unable to import humanoid rig";
        mesh = entity->getMesh(); skeleton = mesh->getSkeleton();
        if (auto* states = entity->getAllAnimationStates())
            for (const auto& [n, state] : states->getAnimationStates()) state->setEnabled(false);
        entity->getSkeleton()->reset(true);
        for (auto* root : entity->getSkeleton()->getRootBones()) root->_update(true, true);
        entity->addSoftwareAnimationRequest(true);
    }
    BodyRetargeter rt(entity->getSkeleton());
    ASSERT_TRUE(rt.valid());
    entity->setUpdateBoundingBoxFromSkeleton(true);
    BodyPoseStream stream; stream.setSmoothing(1.0, false);
    BodyRootMotion rootMotion;
    std::vector<BodyLiveFrame> take;
    const QDir dir(directory);
    const auto files = dir.entryList({"*.png"}, QDir::Files, QDir::Name);
    ASSERT_GE(files.size(), 2);
    int tracked = 0, legs = 0, segments = 0;
    float maxSegmentErrorDegrees = 0.f;
    Ogre::Quaternion cameraBasis = Ogre::Quaternion::IDENTITY;
    std::array<Ogre::Bone*,22> roles{};
    for (unsigned short b = 0; b < entity->getSkeleton()->getNumBones(); ++b) {
        auto* bone = entity->getSkeleton()->getBone(b);
        const int role = MotionInbetween::canonicalIndexForBone(QString::fromStdString(bone->getName()));
        if (role >= 0 && role < 22 && !roles[role]) roles[role] = bone;
    }
    ASSERT_NE(roles[PoseIK::Hip], nullptr); ASSERT_NE(roles[PoseIK::Head], nullptr);
    ASSERT_NE(roles[PoseIK::LHip], nullptr); ASSERT_NE(roles[PoseIK::RHip], nullptr);
    Ogre::Quaternion rigBasis;
    ASSERT_TRUE(BodyPoseGeometry::frame(roles[PoseIK::Head]->_getDerivedPosition() - roles[PoseIK::Hip]->_getDerivedPosition(),
        roles[PoseIK::LHip]->_getDerivedPosition() - roles[PoseIK::RHip]->_getDerivedPosition(), rigBasis));
    QJsonArray frames;
    for (int i = 0; i < files.size(); ++i) {
        const QImage image(dir.filePath(files[i]));
        ASSERT_FALSE(image.isNull());
        const auto f = stream.process(predictor.predict(image, i / 10.0), image.width(), image.height());
        if (!f.valid || !BodyPoseGeometry::solve(f.world.data(), f.visibility.data()).resolved(PoseIK::Hip)) continue;
        if (!rt.hasNeutralReference()) {
            calibrate(rt, f);
            ASSERT_TRUE(rootMotion.calibrate(f));
            Ogre::Quaternion heading;
            ASSERT_TRUE(BodyPoseGeometry::cameraHeading(f.world.data(), heading));
            cameraBasis = rigBasis * heading.Inverse();
        }
        entity->getSkeleton()->getBone(static_cast<unsigned short>(rt.rootBoneHandle()))->setPosition(
            entity->getSkeleton()->getBone(static_cast<unsigned short>(rt.rootBoneHandle()))->getInitialPosition()
            + rt.rootOffset(rootMotion.evaluate(f)));
        apply(rt, f); ++tracked;
        take.push_back(f);
        // Remove initial camera heading from the independently computed
        // expected directions; do not derive the oracle from bone quats.
        QJsonArray landmarks, positions;
        for (float value : f.world) landmarks.append(value);
        for (unsigned short b = 0; b < entity->getSkeleton()->getNumBones(); ++b) {
            const auto p = entity->getSkeleton()->getBone(b)->_getDerivedPosition();
            positions.append(QJsonArray{p.x,p.y,p.z});
        }
        const auto geometry = BodyPoseGeometry::solve(f.world.data(), f.visibility.data());
        auto check = [&](int role, int child, int a, int b) {
            if (geometry.resolved(role) && roles[role] && roles[child]) {
                direction(roles[role]->getName().c_str(), roles[child]->getName().c_str(), f, a, b, cameraBasis);
                const auto actual = (roles[child]->_getDerivedPosition() - roles[role]->_getDerivedPosition()).normalisedCopy();
                const auto expected = cameraBasis * Ogre::Vector3(f.world[b*3] - f.world[a*3],
                    f.world[a*3+1] - f.world[b*3+1], f.world[a*3+2] - f.world[b*3+2]).normalisedCopy();
                maxSegmentErrorDegrees = std::max(maxSegmentErrorDegrees,
                    Ogre::Math::ACos(std::clamp(actual.dotProduct(expected), -1.f, 1.f)).valueDegrees());
                ++segments;
            }
        };
        check(PoseIK::LHip, PoseIK::LKnee, 23,25); check(PoseIK::LKnee, PoseIK::LFoot, 25,27);
        check(PoseIK::RHip, PoseIK::RKnee, 24,26); check(PoseIK::RKnee, PoseIK::RFoot, 26,28);
        const auto renderDir = qEnvironmentVariable("QTMESH_MOCAP_TEST_RENDER");
        if (!renderDir.isEmpty() && !model.isEmpty() && i % 10 == 0) {
            TurntableOptions options; options.frameCount = 1; options.width = 480; options.height = 640;
            options.elevationDegrees = 5; options.studio = true;
            QList<QImage> rendered; QString error;
            ASSERT_TRUE(ModelTurntableRenderer::renderToImages({entity}, options, &rendered, &error)) << error.toStdString();
            ASSERT_FALSE(rendered.empty());
            ASSERT_TRUE(rendered.front().save(QDir(renderDir).filePath(QString("pose-%1.png").arg(i,4,10,QChar('0')))));
        }
        if (geometry.resolved(PoseIK::LHip) && geometry.resolved(PoseIK::LKnee)) ++legs;
        frames.append(QJsonObject{{"index",i},{"world",landmarks},{"positions",positions}});
    }
    EXPECT_GT(tracked, files.size() / 2); EXPECT_GT(legs, files.size() / 3);
    MocapRecorder::BodyRecordOptions recordOptions; recordOptions.clipName = "VideoReplay";
    const auto report = MocapRecorder::recordBodyLive(entity, take, 10, recordOptions);
    ASSERT_TRUE(report.ok()) << report.error.toStdString();
    for (unsigned short b = 0; b < entity->getSkeleton()->getNumBones(); ++b)
        entity->getSkeleton()->getBone(b)->setManuallyControlled(false);
    auto* animation = entity->getAnimationState("VideoReplay");
    animation->setEnabled(true); animation->setLoop(false);
    for (size_t index : {size_t(0), take.size()/2, take.size()-1}) {
        const auto& f = take[index];
        animation->setTimePosition(static_cast<float>(f.timeSec - take.front().timeSec));
        // No render loop advances Ogre's frame counter in this test. Apply
        // states explicitly before updating the software-skinned vertices.
        entity->getSkeleton()->setAnimationState(*entity->getAllAnimationStates());
        entity->getSkeleton()->_notifyManualBonesDirty();
        entity->_updateAnimation();
        for (auto* root : entity->getSkeleton()->getRootBones()) root->_update(true, true);
        const auto geometry = BodyPoseGeometry::solve(f.world.data(), f.visibility.data());
        for (const auto& chain : {std::array<int,4>{PoseIK::LHip,PoseIK::LKnee,23,25},
                                 {PoseIK::LKnee,PoseIK::LFoot,25,27},
                                 {PoseIK::RHip,PoseIK::RKnee,24,26},
                                 {PoseIK::RKnee,PoseIK::RFoot,26,28}})
            if (geometry.resolved(chain[0]) && roles[chain[0]] && roles[chain[1]])
                direction(roles[chain[0]]->getName().c_str(), roles[chain[1]]->getName().c_str(), f, chain[2],chain[3],cameraBasis);
    }
    animation->setEnabled(false);
    const auto exportPath = qEnvironmentVariable("QTMESH_MOCAP_TEST_EXPORT");
    if (!exportPath.isEmpty() && !model.isEmpty())
        ASSERT_EQ(MeshImporterExporter::exporter(entity->getParentSceneNode(), exportPath, "glb2"), 0);
    const auto output = qEnvironmentVariable("QTMESH_MOCAP_TEST_OUTPUT");
    if (!output.isEmpty()) {
        QFile file(output); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(frames).toJson(QJsonDocument::Compact));
    }
    std::cout << "Video replay: " << tracked << " torso frames, " << legs << " full left-leg frames out of " << files.size()
              << "; " << segments << " reliable leg segments, maximum aim error " << maxSegmentErrorDegrees << " degrees\n";
}
#endif
