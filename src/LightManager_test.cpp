#include <gtest/gtest.h>

#include "LightManager.h"
#include "Manager.h"
#include "MaterialPreviewRenderer.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreLight.h>
#include <OgreSceneManager.h>

#include <QSignalSpy>

class LightManagerTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        LightManager::kill();
        Manager::kill();
    }
};

class LightManagerOgreTest : public LightManagerTest {
protected:
    void SetUp() override
    {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
    }
};

TEST_F(LightManagerTest, KillAndRecreate)
{
    auto* mgr = LightManager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    LightManager::kill();
    auto* mgr2 = LightManager::getSingleton();
    ASSERT_NE(mgr2, nullptr);
}

TEST_F(LightManagerOgreTest, DefaultSceneLighting_UsesThreePointStudio)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    mgr->CreateEmptyScene();

    const QList<LightHandle> lights = LightManager::getSingleton()->lights();
    ASSERT_EQ(lights.size(), 3);

    QStringList names;
    for (const LightHandle& handle : lights)
        names.append(handle.name);
    EXPECT_TRUE(names.contains(QStringLiteral("KeyLight")));
    EXPECT_TRUE(names.contains(QStringLiteral("FillLight")));
    EXPECT_TRUE(names.contains(QStringLiteral("BackLight")));

    const Ogre::ColourValue ambient = mgr->getSceneMgr()->getAmbientLight();
    EXPECT_EQ(ambient, Ogre::ColourValue(0.15f, 0.15f, 0.18f));
}

TEST_F(LightManagerOgreTest, CreateDeleteRename)
{
    auto* lights = LightManager::getSingleton();
    QSignalSpy createdSpy(lights, &LightManager::lightCreated);
    QSignalSpy deletedSpy(lights, &LightManager::lightDeleted);

    const LightHandle point = lights->createLight(Ogre::Light::LT_POINT, QStringLiteral("Fill"));
    ASSERT_TRUE(point.isValid());
    EXPECT_EQ(createdSpy.count(), 1);

    EXPECT_TRUE(LightManager::isUserLight(point.light));

    EXPECT_TRUE(lights->renameLight(QStringLiteral("Fill"), QStringLiteral("FillRenamed")));
    LightHandle* renamed = lights->findLight(QStringLiteral("FillRenamed"));
    ASSERT_NE(renamed, nullptr);
    EXPECT_EQ(renamed->sceneNode->getName(), std::string("FillRenamed"));
    EXPECT_EQ(renamed->light->getName(), std::string("FillRenamed"));

    EXPECT_TRUE(lights->deleteLight(QStringLiteral("FillRenamed")));
    EXPECT_GE(deletedSpy.count(), 1);
    EXPECT_TRUE(lights->lights().isEmpty());
}

TEST_F(LightManagerOgreTest, UntaggedSceneLight_NotListed)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    Ogre::Light* internal = sceneMgr->createLight(QStringLiteral("InternalOnly").toStdString());
    internal->setType(Ogre::Light::LT_DIRECTIONAL);
    Ogre::SceneNode* node = sceneMgr->getRootSceneNode()->createChildSceneNode(
        QStringLiteral("InternalLightNode").toStdString());
    node->attachObject(internal);

    EXPECT_FALSE(LightManager::isUserLight(internal));
    EXPECT_TRUE(LightManager::getSingleton()->lights().isEmpty());

    sceneMgr->destroySceneNode(node);
}

TEST_F(LightManagerOgreTest, MaterialPreviewRendererLight_NotListed)
{
    auto* preview = MaterialPreviewRenderer::instance();
    ASSERT_NE(preview, nullptr);
    preview->renderPreviewAsDataUri(QStringLiteral("BaseWhite"));

    EXPECT_TRUE(LightManager::getSingleton()->lights().isEmpty());
}

TEST_F(LightManagerOgreTest, DefaultLightSelectableInOutliner)
{
    Manager::getSingletonPtr()->CreateEmptyScene();

    const QList<LightHandle> lights = LightManager::getSingleton()->lights();
    ASSERT_GE(lights.size(), 1);

    Ogre::SceneNode* node = lights.first().sceneNode;
    ASSERT_NE(node, nullptr);

    SelectionSet::getSingleton()->selectOne(node);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(node));
}

TEST_F(LightManagerOgreTest, CreateEmptySceneTwice_ReplacesDefaultLights)
{
    Manager::getSingletonPtr()->CreateEmptyScene();
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 3);

    Manager::getSingletonPtr()->CreateEmptyScene();
    const QList<LightHandle> lights = LightManager::getSingleton()->lights();
    ASSERT_EQ(lights.size(), 3);
}
