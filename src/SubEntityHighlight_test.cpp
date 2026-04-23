#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "Manager.h"
#include "SelectionSet.h"
#include "SubEntityHighlight.h"
#include "TestHelpers.h"

class SubEntityHighlightTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        SubEntityHighlight::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }

        createStandardOgreMaterials();
        SelectionSet::getSingleton()->clear();
    }

    void TearDown() override
    {
        SelectionSet::getSingleton()->clear();
        SubEntityHighlight::kill();
        SelectionSet::kill();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }

    Ogre::Entity* createEntityWithTriangleMesh(const QString& baseName)
    {
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh((baseName + "_mesh").toStdString());
        if (!mesh) return nullptr;

        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(baseName + "_node");
        if (!node) return nullptr;

        Ogre::Entity* entity = Manager::getSingleton()->createEntity(node, mesh);
        SelectionSet::getSingleton()->clear();
        return entity;
    }
};

TEST_F(SubEntityHighlightTests, SingletonLifecycle)
{
    SubEntityHighlight* first = SubEntityHighlight::getSingleton();
    ASSERT_NE(first, nullptr);

    SubEntityHighlight* again = SubEntityHighlight::getSingleton();
    EXPECT_EQ(first, again);

    SubEntityHighlight::kill();

    SubEntityHighlight* recreated = SubEntityHighlight::getSingleton();
    ASSERT_NE(recreated, nullptr);
}

TEST_F(SubEntityHighlightTests, SubEntitySelectionAppliesAndClearsHighlight)
{
    Ogre::Entity* entity = createEntityWithTriangleMesh("subhl_basic");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    Ogre::SubEntity* sub = entity->getSubEntity(0);
    ASSERT_NE(sub, nullptr);

    const std::string originalMat = sub->getMaterialName();
    ASSERT_FALSE(originalMat.empty());

    SubEntityHighlight::getSingleton();
    SelectionSet::getSingleton()->selectOne(sub);
    if (app) app->processEvents();

    EXPECT_EQ(sub->getMaterialName(), originalMat + "_SubMeshHighlight");

    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();

    EXPECT_EQ(sub->getMaterialName(), originalMat);
}

TEST_F(SubEntityHighlightTests, MissingOriginalMaterialUsesFallbackHighlightMaterial)
{
    Ogre::Entity* entity = createEntityWithTriangleMesh("subhl_missing_mat");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    Ogre::SubEntity* sub = entity->getSubEntity(0);
    ASSERT_NE(sub, nullptr);

    const std::string originalMat = sub->getMaterialName();
    ASSERT_FALSE(originalMat.empty());

    auto& matMgr = Ogre::MaterialManager::getSingleton();
    if (matMgr.resourceExists(originalMat)) {
        matMgr.remove(originalMat);
    }

    SubEntityHighlight::getSingleton();
    SelectionSet::getSingleton()->selectOne(sub);
    if (app) app->processEvents();

    const std::string highlightMat = originalMat + "_SubMeshHighlight";
    EXPECT_EQ(sub->getMaterialName(), highlightMat);
    EXPECT_TRUE(matMgr.resourceExists(highlightMat));

    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();
}

TEST_F(SubEntityHighlightTests, SwitchingSelectionTypeClearsSubEntityHighlight)
{
    Ogre::Entity* entity = createEntityWithTriangleMesh("subhl_switch");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    Ogre::SubEntity* sub = entity->getSubEntity(0);
    ASSERT_NE(sub, nullptr);

    const std::string originalMat = sub->getMaterialName();

    SubEntityHighlight::getSingleton();

    SelectionSet::getSingleton()->selectOne(sub);
    if (app) app->processEvents();
    ASSERT_EQ(sub->getMaterialName(), originalMat + "_SubMeshHighlight");

    // Switching to entity selection emits entitySelectionChanged and should
    // clear tracked sub-entity highlights.
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    EXPECT_EQ(sub->getMaterialName(), originalMat);
}
