#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "commands/LipsyncClipCommand.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgrePose.h>

class LipsyncClipCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

// A generated character carries every ARKit shape on each part (body, teeth, eyes, lashes...) under the same name. The
// Inspector and MCP lipsync kept ONE pose index per name (QHash::insert keeps the last), so only one part was keyed and
// the face itself never moved, while the CLI (which writes by name) worked.
TEST_F(LipsyncClipCommandTest, EveryPartCarryingAnArkitNameIsKeyed)
{
    auto mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh("lipsync_two_parts");
    ASSERT_NE(mesh, nullptr);
    // handle 0 = the shared vertices (part 0), handle 2 = submesh 1's own vertices (part 1)
    mesh->createPose(0, "jawOpen")->addVertex(0, Ogre::Vector3(0, -0.1f, 0));
    mesh->createPose(2, "jawOpen")->addVertex(1, Ogre::Vector3(0, -0.1f, 0));
    mesh->createPose(0, "browInnerUp")->addVertex(2, Ogre::Vector3(0, 0.05f, 0));

    const auto byName = LipsyncClipCommand::poseIndicesByName(*mesh);
    ASSERT_TRUE(byName.contains(QStringLiteral("jawOpen")));
    EXPECT_EQ(byName.value(QStringLiteral("jawOpen")), (std::vector<unsigned short>{0, 1}));
    EXPECT_EQ(byName.value(QStringLiteral("browInnerUp")), (std::vector<unsigned short>{2}));

    auto* node = Manager::getSingleton()->addSceneNode("lipsync_two_parts_ent");
    auto* entity = Manager::getSingleton()->getSceneMgr()->createEntity(node->getName(), mesh->getName());
    node->attachObject(entity);

    std::vector<LipsyncClipCommand::Key> keys(2);
    keys[0].time = 0.0f;
    keys[1].time = 1.0f;
    for (auto& k : keys)
        for (unsigned short pi : byName.value(QStringLiteral("jawOpen")))
            k.poseRefs.emplace_back(pi, k.time > 0.5f ? 0.8f : 0.0f);
    LipsyncClipCommand cmd(entity->getName(), QStringLiteral("Lipsync"), keys);
    cmd.redo();

    ASSERT_TRUE(mesh->hasAnimation("Lipsync"));
    Ogre::Animation* anim = mesh->getAnimation("Lipsync");
    EXPECT_TRUE(anim->hasVertexTrack(0)) << "the shared part's jawOpen must be keyed";
    EXPECT_TRUE(anim->hasVertexTrack(2)) << "the second part's jawOpen must be keyed too";
    EXPECT_TRUE(entity->hasAnimationState("Lipsync"));

    cmd.undo();
    EXPECT_FALSE(mesh->hasAnimation("Lipsync"));
}
