#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreSceneManager.h>

#include "Manager.h"
#include "commands/ApplyMaterialCommand.h"
#include "TestHelpers.h"

class ApplyMaterialCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());

        // Two test materials so we can swap and assert.
        auto& mm = Ogre::MaterialManager::getSingleton();
        const auto group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
        if (!mm.getByName("MatA", group)) mm.create("MatA", group);
        if (!mm.getByName("MatB", group)) mm.create("MatB", group);
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

TEST_F(ApplyMaterialCommandTest, RedoAppliesNewMaterial)
{
    auto mesh = createInMemoryTriangleMesh("AMC_redo_mesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* entity = sceneMgr->createEntity("AMC_redo", mesh);

    ASSERT_GT(entity->getNumSubEntities(), 0u);
    auto* sub = entity->getSubEntity(0);
    sub->setMaterialName("MatA");
    EXPECT_EQ(sub->getMaterialName(), std::string("MatA"));

    std::vector<ApplyMaterialCommand::Target> targets = {{sub, "MatA"}};
    ApplyMaterialCommand cmd(std::move(targets), std::string("MatB"));
    cmd.redo();
    EXPECT_EQ(sub->getMaterialName(), std::string("MatB"));
}

TEST_F(ApplyMaterialCommandTest, UndoRestoresOriginalMaterial)
{
    auto mesh = createInMemoryTriangleMesh("AMC_undo_mesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* entity = sceneMgr->createEntity("AMC_undo", mesh);

    auto* sub = entity->getSubEntity(0);
    sub->setMaterialName("MatA");

    std::vector<ApplyMaterialCommand::Target> targets = {{sub, "MatA"}};
    ApplyMaterialCommand cmd(std::move(targets), std::string("MatB"));
    cmd.redo();
    EXPECT_EQ(sub->getMaterialName(), std::string("MatB"));
    cmd.undo();
    EXPECT_EQ(sub->getMaterialName(), std::string("MatA"));
}

TEST_F(ApplyMaterialCommandTest, NullTargetIsSkippedNotCrash)
{
    // The command iterates and skips nullptr targets.
    std::vector<ApplyMaterialCommand::Target> targets = {
        {nullptr, std::string{"WhateverOld"}},
    };
    ApplyMaterialCommand cmd(std::move(targets), std::string("MatA"));
    cmd.redo();
    cmd.undo();
    // No crash = success.
}

TEST_F(ApplyMaterialCommandTest, MultipleTargetsApplyIndependently)
{
    // Two sub-entities should each remember their own old name.
    auto mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh("AMC_multi_mesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* entity = sceneMgr->createEntity("AMC_multi", mesh);
    ASSERT_GE(entity->getNumSubEntities(), 2u);
    auto* sub0 = entity->getSubEntity(0);
    auto* sub1 = entity->getSubEntity(1);
    sub0->setMaterialName("MatA");
    sub1->setMaterialName("MatB");

    std::vector<ApplyMaterialCommand::Target> targets = {
        {sub0, "MatA"},
        {sub1, "MatB"},
    };
    ApplyMaterialCommand cmd(std::move(targets), std::string("MatA"));
    cmd.redo();
    EXPECT_EQ(sub0->getMaterialName(), std::string("MatA"));
    EXPECT_EQ(sub1->getMaterialName(), std::string("MatA"));

    cmd.undo();
    EXPECT_EQ(sub0->getMaterialName(), std::string("MatA"));
    EXPECT_EQ(sub1->getMaterialName(), std::string("MatB"));
}

TEST_F(ApplyMaterialCommandTest, ConstructorSetsCommandText)
{
    std::vector<ApplyMaterialCommand::Target> oneTarget = {{nullptr, "MatA"}};
    ApplyMaterialCommand cmdOne(std::move(oneTarget), std::string("MatB"));
    EXPECT_FALSE(cmdOne.text().isEmpty());
    EXPECT_TRUE(cmdOne.text().contains("MatB"));

    std::vector<ApplyMaterialCommand::Target> twoTargets = {
        {nullptr, "MatA"}, {nullptr, "MatA"}};
    ApplyMaterialCommand cmdTwo(std::move(twoTargets), std::string("MatB"));
    EXPECT_FALSE(cmdTwo.text().isEmpty());
}
