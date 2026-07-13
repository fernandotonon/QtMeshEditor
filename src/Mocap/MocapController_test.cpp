#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include "Manager.h"
#include "MorphAnimationManager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"

#include "Mocap/MocapController.h"
#include "Mocap/MocapRecorder.h"

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

namespace {

// same recipe as MocapRecorder_test — a tiny mesh with two ARKit targets
Ogre::MeshPtr createFaceTestMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    sub->vertexData->vertexDeclaration->addElement(0, 0, Ogre::VET_FLOAT3,
                                                   Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        sub->vertexData->vertexDeclaration->getVertexSize(0), 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
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
        track->createVertexPoseKeyFrame(0.0f)->addPoseReference(pi, 1.0f);
    }
    return mesh;
}

FaceSample sample(double t, float jaw, float conf = 1.f)
{
    FaceSample s;
    s.timeSec = t;
    s.confidence = conf;
    s.weights[25] = jaw;  // jawOpen
    return s;
}

class MocapControllerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        static int counter = 0;
        m_meshName = "mocap_ctrl_test_" + std::to_string(counter++);
        m_mesh = createFaceTestMesh(m_meshName);
        auto* scene = Manager::getSingleton()->getSceneMgr();
        m_entity = scene->createEntity(m_meshName + "_ent", m_meshName);
        m_node = scene->getRootSceneNode()->createChildSceneNode();
        m_node->attachObject(m_entity);
        SelectionSet::getSingleton()->clearList();
        SelectionSet::getSingleton()->append(m_entity);
    }

    void TearDown() override
    {
        MocapController::instance()->stopPreview();
        SelectionSet::getSingleton()->clearList();
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
};

}  // namespace

TEST_F(MocapControllerTest, PreviewDrivesWeightsAndRestoresOnStop)
{
    auto* c = MocapController::instance();
    auto* morph = MorphAnimationManager::instance();
    morph->setWeight(m_entity, QStringLiteral("jawOpen"), 0.33f);

    ASSERT_TRUE(c->startPreviewWithSource(nullptr));  // any non-null seam token
    EXPECT_EQ(c->state(), MocapController::Previewing);
    EXPECT_EQ(c->matchedChannelCount(), 2);

    c->onSample(sample(0.0, 0.9f), QImage());
    EXPECT_NEAR(morph->weight(m_entity, QStringLiteral("jawOpen")), 0.9f, 1e-4);

    c->stopPreview();
    EXPECT_EQ(c->state(), MocapController::Idle);
    // the pre-preview weight came back exactly
    EXPECT_NEAR(morph->weight(m_entity, QStringLiteral("jawOpen")), 0.33f, 1e-4);
}

TEST_F(MocapControllerTest, RecordProducesUndoableClip)
{
    auto* c = MocapController::instance();
    ASSERT_TRUE(c->startPreviewWithSource(nullptr));
    c->onSample(sample(0.0, 0.1f), QImage());

    ASSERT_TRUE(c->startRecording());
    EXPECT_EQ(c->state(), MocapController::Recording);
    c->onSample(sample(0.1, 0.2f), QImage());
    c->onSample(sample(0.2, 0.8f), QImage());
    c->onSample(sample(0.3, 0.4f), QImage());
    c->stopRecording();
    EXPECT_EQ(c->state(), MocapController::Previewing);
    c->stopPreview();

    EXPECT_TRUE(m_mesh->hasAnimation("FaceCap"));
    UndoManager::getSingleton()->undo();  // ONE undo removes the whole take
    EXPECT_FALSE(m_mesh->hasAnimation("FaceCap"));
}

TEST_F(MocapControllerTest, StopDuringRecordingCommitsTheTake)
{
    auto* c = MocapController::instance();
    ASSERT_TRUE(c->startPreviewWithSource(nullptr));
    ASSERT_TRUE(c->startRecording());
    c->onSample(sample(0.0, 0.2f), QImage());
    c->onSample(sample(0.2, 0.7f), QImage());
    c->stopPreview();  // stop while recording
    EXPECT_EQ(c->state(), MocapController::Idle);
    EXPECT_TRUE(m_mesh->hasAnimation("FaceCap"));
    UndoManager::getSingleton()->undo();
}

TEST_F(MocapControllerTest, NoSelectionRefusesPreview)
{
    SelectionSet::getSingleton()->clearList();
    auto* c = MocapController::instance();
    EXPECT_FALSE(c->startPreviewWithSource(nullptr));
    EXPECT_EQ(c->state(), MocapController::Idle);
}

#endif  // ENABLE_MOCAP
