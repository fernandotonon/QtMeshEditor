#include <gtest/gtest.h>

#include "LightManager.h"
#include "LightRigLibrary.h"
#include "LightsController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "Manager.h"
#include "commands/LightCommands.h"

class LightCommandsOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
        UndoManager::getSingleton()->clear();
    }

    void TearDown() override
    {
        UndoManager::kill();
        LightsController::kill();
        LightManager::kill();
        Manager::kill();
    }
};

TEST_F(LightCommandsOgreTest, CreateDeleteUndoRedo)
{
    LightHandle created = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                                    QStringLiteral("UndoPoint"));
    ASSERT_TRUE(created.isValid());

    UndoManager::getSingleton()->push(
        new CreateLightCommand(LightSnapshot::fromHandle(created)));

    EXPECT_NE(LightManager::getSingleton()->findLight(created.name), nullptr);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(LightManager::getSingleton()->findLight(created.name), nullptr);

    UndoManager::getSingleton()->redo();
    EXPECT_NE(LightManager::getSingleton()->findLight(created.name), nullptr);
}

TEST_F(LightCommandsOgreTest, DuplicateAndDeleteLightsUndo)
{
    LightHandle source = LightManager::getSingleton()->createLight(Ogre::Light::LT_SPOTLIGHT,
                                                                   QStringLiteral("SpotA"));
    ASSERT_TRUE(source.isValid());

    LightHandle clone = LightManager::getSingleton()->duplicateLight(source.name);
    ASSERT_TRUE(clone.isValid());

    const QList<LightSnapshot> cloneSnapshots = {LightSnapshot::fromHandle(clone)};
    UndoManager::getSingleton()->push(new DuplicateLightsCommand(cloneSnapshots));

    SelectionSet::getSingleton()->selectOne(clone.sceneNode);
    LightsController::instance()->deleteSelectedLights();

    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 1);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 2);

    UndoManager::getSingleton()->redo();
    EXPECT_EQ(LightManager::getSingleton()->lights().size(), 1);
}

TEST_F(LightCommandsOgreTest, RenameUndoRedo)
{
    LightHandle created = LightManager::getSingleton()->createLight(Ogre::Light::LT_DIRECTIONAL,
                                                                    QStringLiteral("OldName"));
    ASSERT_TRUE(created.isValid());

    ASSERT_TRUE(LightManager::getSingleton()->renameLight(QStringLiteral("OldName"),
                                                          QStringLiteral("NewName")));
    UndoManager::getSingleton()->push(
        new RenameLightCommand(QStringLiteral("OldName"), QStringLiteral("NewName")));

    UndoManager::getSingleton()->undo();
    EXPECT_NE(LightManager::getSingleton()->findLight(QStringLiteral("OldName")), nullptr);

    UndoManager::getSingleton()->redo();
    EXPECT_NE(LightManager::getSingleton()->findLight(QStringLiteral("NewName")), nullptr);
}

TEST_F(LightCommandsOgreTest, ApplyLightRigUndoRestoresRigGroupParent)
{
    const LightRigApplyResult first =
        LightRigLibrary::apply(QStringLiteral("single_key"), false);
    ASSERT_TRUE(first.ok) << first.error.toStdString();
    ASSERT_EQ(first.removedRigGroups.size(), 0);

    const QString keyLightName = first.addedLights.first().name;
    const LightHandle* keyBefore = LightManager::getSingleton()->findLight(keyLightName);
    ASSERT_NE(keyBefore, nullptr);
    ASSERT_TRUE(keyBefore->sceneNode && keyBefore->sceneNode->getParent());
    EXPECT_TRUE(LightRigLibrary::sceneNodeIsRigGroup(
        static_cast<Ogre::SceneNode*>(keyBefore->sceneNode->getParent())));

    const LightRigApplyResult second =
        LightRigLibrary::apply(QStringLiteral("three_point_studio"), false);
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    ASSERT_EQ(second.removedRigGroups.size(), 1);
    ASSERT_EQ(second.removedRigGroups.first().lights.size(), 1);

    UndoManager::getSingleton()->push(new ApplyLightRigCommand(second));
    UndoManager::getSingleton()->undo();

    const LightHandle* keyAfter = LightManager::getSingleton()->findLight(keyLightName);
    ASSERT_NE(keyAfter, nullptr);
    ASSERT_TRUE(keyAfter->sceneNode && keyAfter->sceneNode->getParent());
    EXPECT_TRUE(LightRigLibrary::sceneNodeIsRigGroup(
        static_cast<Ogre::SceneNode*>(keyAfter->sceneNode->getParent())));
}
