#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include "Manager.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/RecordMocapClipCommand.h"

#include "Mocap/FaceCapCanonicalData.h"
#include "Mocap/FaceCapMapper.h"
#include "Mocap/MocapRecorder.h"

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <vector>

namespace {

// Tiny mesh with two ARKit-named morph targets (the MorphAnimationManager
// fixture recipe).
Ogre::MeshPtr createFaceTestMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* decl = sub->vertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    vbuf->writeData(0, sizeof(verts), verts);
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 2, 2, 2));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();

    {
        Ogre::Pose* p = mesh->createPose(1, "jawOpen");
        p->addVertex(0, Ogre::Vector3(0, -0.1f, 0));
    }
    {
        Ogre::Pose* p = mesh->createPose(1, "mouthSmileLeft");
        p->addVertex(1, Ogre::Vector3(0.05f, 0.02f, 0));
    }
    const auto& poses = mesh->getPoseList();
    for (unsigned short pi = 0; pi < poses.size(); ++pi) {
        Ogre::Animation* a = mesh->createAnimation(poses[pi]->getName(), 0.0f);
        auto* track = a->createVertexTrack(poses[pi]->getTarget(), Ogre::VAT_POSE);
        auto* kf = track->createVertexPoseKeyFrame(0.0f);
        kf->addPoseReference(pi, 1.0f);
    }
    return mesh;
}

FaceSample makeSample(double t, float jawOpen, float smile,
                      float confidence = 1.f)
{
    FaceSample s;
    s.timeSec = t;
    s.confidence = confidence;
    s.weights[25] = jawOpen;         // jawOpen canonical index
    s.weights[44] = smile;           // mouthSmileLeft canonical index
    return s;
}

// weight of `poseName` at (approx) time t on the recorded clip; -1 if unkeyed
float weightAt(Ogre::MeshPtr mesh, const std::string& clip,
               const std::string& poseName, float t)
{
    if (!mesh->hasAnimation(clip))
        return -1.f;
    int poseIndex = -1;
    const auto& poses = mesh->getPoseList();
    for (unsigned short i = 0; i < poses.size(); ++i)
        if (poses[i]->getName() == poseName) poseIndex = i;
    if (poseIndex < 0)
        return -1.f;
    Ogre::Animation* anim = mesh->getAnimation(clip);
    for (const auto& [handle, track] : anim->_getVertexTrackList()) {
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
            if (std::abs(kf->getTime() - t) > 1e-3f)
                continue;
            for (const auto& ref : kf->getPoseReferences())
                if (ref.poseIndex == poseIndex)
                    return ref.influence;
        }
    }
    return -1.f;
}

int keyCountFor(Ogre::MeshPtr mesh, const std::string& clip,
                const std::string& poseName)
{
    if (!mesh->hasAnimation(clip))
        return 0;
    int poseIndex = -1;
    const auto& poses = mesh->getPoseList();
    for (unsigned short i = 0; i < poses.size(); ++i)
        if (poses[i]->getName() == poseName) poseIndex = i;
    int count = 0;
    Ogre::Animation* anim = mesh->getAnimation(clip);
    for (const auto& [handle, track] : anim->_getVertexTrackList()) {
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(i));
            for (const auto& ref : kf->getPoseReferences())
                if (ref.poseIndex == poseIndex)
                    ++count;
        }
    }
    return count;
}

class MocapRecorderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        static int counter = 0;
        m_meshName = "mocap_rec_test_" + std::to_string(counter++);
        m_mesh = createFaceTestMesh(m_meshName);
        auto* scene = Manager::getSingleton()->getSceneMgr();
        m_entity = scene->createEntity(m_meshName + "_ent", m_meshName);
        m_node = scene->getRootSceneNode()->createChildSceneNode();
        m_node->attachObject(m_entity);
        m_mapping = FaceCapMapper::build(
            {QStringLiteral("jawOpen"), QStringLiteral("mouthSmileLeft")});
        ASSERT_EQ(m_mapping.channels.size(), 2);
    }

    void TearDown() override
    {
        if (m_entity) {
            m_node->detachObject(m_entity);
            Manager::getSingleton()->getSceneMgr()->destroyEntity(m_entity);
        }
        Ogre::MeshManager::getSingleton().remove(
            m_meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    std::string m_meshName;
    Ogre::MeshPtr m_mesh;
    Ogre::Entity* m_entity = nullptr;
    Ogre::SceneNode* m_node = nullptr;
    FaceCapMapper::Mapping m_mapping;
};

}  // namespace

TEST_F(MocapRecorderTest, KeyframesLandAtTimesAndValues)
{
    std::vector<FaceSample> samples = {
        makeSample(10.0, 0.1f, 0.9f),   // times re-base to 0
        makeSample(10.5, 0.5f, 0.9f),
        makeSample(11.0, 0.9f, 0.9f),
    };
    MocapRecorder::FaceRecordOptions options;
    options.head = false;
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, m_mapping, options);
    ASSERT_TRUE(report.ok()) << report.error.toStdString();
    EXPECT_EQ(report.framesProcessed, 3);
    EXPECT_EQ(report.framesNoFace, 0);

    const std::string clip = "FaceCap";
    EXPECT_FLOAT_EQ(weightAt(m_mesh, clip, "jawOpen", 0.0f), 0.1f);
    EXPECT_FLOAT_EQ(weightAt(m_mesh, clip, "jawOpen", 0.5f), 0.5f);
    EXPECT_FLOAT_EQ(weightAt(m_mesh, clip, "jawOpen", 1.0f), 0.9f);
    EXPECT_NEAR(report.clipLength, 1.0, 1e-4);
    // the entity can play it
    EXPECT_TRUE(m_entity->getAnimationState("FaceCap") != nullptr);
}

TEST_F(MocapRecorderTest, EpsilonSuppressionKeepsConstantChannelsSparse)
{
    std::vector<FaceSample> samples;
    for (int i = 0; i < 60; ++i)
        samples.push_back(makeSample(i / 30.0, 0.5f, 0.5f));  // constant
    MocapRecorder::FaceRecordOptions options;
    options.head = false;
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, m_mapping, options);
    ASSERT_TRUE(report.ok());
    EXPECT_LE(keyCountFor(m_mesh, "FaceCap", "jawOpen"), 2);   // first + last
    EXPECT_LE(report.keyframesWritten, 4);
}

TEST_F(MocapRecorderTest, GapGetsHoldKeysAtEdges)
{
    std::vector<FaceSample> samples;
    samples.push_back(makeSample(0.0, 0.2f, 0.0f));
    samples.push_back(makeSample(0.1, 0.2f, 0.0f));
    // face lost for 1.5 s
    samples.push_back(makeSample(0.2, 0.0f, 0.0f, /*confidence=*/0.f));
    samples.push_back(makeSample(1.6, 0.8f, 0.0f));
    samples.push_back(makeSample(1.7, 0.8f, 0.0f));
    MocapRecorder::FaceRecordOptions options;
    options.head = false;
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, m_mapping, options);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.framesNoFace, 1);
    // hold key at the gap's leading edge (t=0.1) and landing key at t=1.6
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 0.1f), 0.2f);
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 1.6f), 0.8f);
}

TEST_F(MocapRecorderTest, NoConfidentFramesFailsCleanly)
{
    std::vector<FaceSample> samples = {makeSample(0.0, 0.f, 0.f, 0.f)};
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, m_mapping, {});
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(m_mesh->hasAnimation("FaceCap"), false);
}

TEST_F(MocapRecorderTest, UnmatchedChannelsAreReported)
{
    const auto mapping =
        FaceCapMapper::build({QStringLiteral("jawOpen"),
                              QStringLiteral("SomethingCustom")});
    std::vector<FaceSample> samples = {makeSample(0.0, 0.3f, 0.f),
                                       makeSample(1.0, 0.6f, 0.f)};
    MocapRecorder::FaceRecordOptions options;
    options.head = false;
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, mapping, options);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.matchedChannels, QStringList{QStringLiteral("jawOpen")});
    EXPECT_TRUE(report.unmatchedCanonical.contains(QStringLiteral("mouthSmileLeft")));
    EXPECT_EQ(report.unmatchedMesh,
              QStringList{QStringLiteral("SomethingCustom")});
}

TEST_F(MocapRecorderTest, HeadTargetIsNoneForStaticMeshWithoutNode)
{
    // static (skeleton-less) entity: head lands on the node path
    std::vector<FaceSample> samples = {makeSample(0.0, 0.1f, 0.f),
                                       makeSample(1.0, 0.9f, 0.f)};
    samples[1].headRotation = {0.f, 0.2588f, 0.f, 0.9659f};  // 30 deg yaw
    const auto report =
        MocapRecorder::recordFace(m_entity, samples, m_mapping, {});
    ASSERT_TRUE(report.ok());
    // skeleton-less mesh + a parent node exists -> node-TRS head clip
    EXPECT_EQ(report.headTarget, QStringLiteral("node"));
    EXPECT_GT(report.headKeyframesWritten, 0);
    EXPECT_TRUE(MocapRecorder::resolveHeadBone(m_entity).isEmpty());
}

TEST_F(MocapRecorderTest, RecordCommandIsSingleUndoStep)
{
    std::vector<FaceSample> take1 = {makeSample(0.0, 0.1f, 0.1f),
                                     makeSample(1.0, 0.9f, 0.9f)};
    MocapRecorder::FaceRecordOptions options;
    options.head = false;

    // pre-existing clip state to restore: record take 0 directly
    std::vector<FaceSample> take0 = {makeSample(0.0, 0.4f, 0.4f),
                                     makeSample(0.5, 0.6f, 0.6f)};
    ASSERT_TRUE(MocapRecorder::recordFace(m_entity, take0, m_mapping, options).ok());
    ASSERT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 0.5f), 0.6f);

    auto* cmd = new RecordMocapClipCommand(m_entity->getName(), take1,
                                           m_mapping, options);
    UndoManager::getSingleton()->push(cmd);  // runs redo()
    ASSERT_TRUE(cmd->report().ok()) << cmd->report().error.toStdString();
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 1.0f), 0.9f);
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 0.5f), -1.f);  // replaced

    UndoManager::getSingleton()->undo();
    // take 0 is back, take 1 is gone
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 0.5f), 0.6f);
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 1.0f), -1.f);

    UndoManager::getSingleton()->redo();
    EXPECT_FLOAT_EQ(weightAt(m_mesh, "FaceCap", "jawOpen", 1.0f), 0.9f);
}

#endif  // ENABLE_MOCAP
