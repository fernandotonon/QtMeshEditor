#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <OgreEntity.h>
#include <OgreSkeletonInstance.h>
#include <OgreBone.h>

#include "Manager.h"
#include "commands/BoneTransformCommand.h"
#include "TestHelpers.h"

class BoneTransformCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

TEST_F(BoneTransformCommandTest, RedoAppliesAfterState)
{
    auto* entity = createAnimatedTestEntity("BTC_redo");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    ASSERT_TRUE(skel->hasBone("Child"));
    auto* bone = skel->getBone("Child");
    bone->setManuallyControlled(true);
    const Ogre::Vector3 beforePos = bone->getPosition();
    const Ogre::Quaternion beforeOrient = bone->getOrientation();
    const Ogre::Vector3 beforeScale = bone->getScale();

    const Ogre::Vector3 afterPos(5, 6, 7);
    const Ogre::Quaternion afterOrient(Ogre::Radian(Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_Y);
    const Ogre::Vector3 afterScale(2, 2, 2);

    BoneTransformCommand cmd(entity->getName(), "Child",
                              beforePos, beforeOrient, beforeScale,
                              afterPos,  afterOrient,  afterScale);
    cmd.redo();
    EXPECT_EQ(bone->getPosition(), afterPos);
    EXPECT_EQ(bone->getScale(), afterScale);
}

TEST_F(BoneTransformCommandTest, UndoRestoresBeforeState)
{
    auto* entity = createAnimatedTestEntity("BTC_undo");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* bone = skel->getBone("Child");
    bone->setManuallyControlled(true);
    const Ogre::Vector3 beforePos = bone->getPosition();
    const Ogre::Quaternion beforeOrient = bone->getOrientation();
    const Ogre::Vector3 beforeScale = bone->getScale();

    BoneTransformCommand cmd(entity->getName(), "Child",
                              beforePos, beforeOrient, beforeScale,
                              Ogre::Vector3(9, 9, 9),
                              Ogre::Quaternion::IDENTITY,
                              Ogre::Vector3(3, 3, 3));
    cmd.redo();
    EXPECT_EQ(bone->getPosition(), Ogre::Vector3(9, 9, 9));
    cmd.undo();
    EXPECT_EQ(bone->getPosition(), beforePos);
    EXPECT_EQ(bone->getScale(), beforeScale);
}

TEST_F(BoneTransformCommandTest, MissingEntityIsNoCrash)
{
    // No entity registered with this name → resolve returns nullptr →
    // command is a silent no-op. We just need to not crash.
    BoneTransformCommand cmd(std::string("NoSuchEntity"), std::string("NoBone"),
                              Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                              Ogre::Vector3::UNIT_X, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    cmd.redo();
    cmd.undo();
}

TEST_F(BoneTransformCommandTest, MissingBoneIsNoCrash)
{
    auto* entity = createAnimatedTestEntity("BTC_no_bone");
    ASSERT_NE(entity, nullptr);
    BoneTransformCommand cmd(entity->getName(), std::string("ThisBoneDoesNotExist"),
                              Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                              Ogre::Vector3::UNIT_X, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    cmd.redo();
    cmd.undo();
}

TEST_F(BoneTransformCommandTest, BindModeSetsInitialState)
{
    auto* entity = createAnimatedTestEntity("BTC_bind");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* bone = skel->getBone("Child");
    bone->setManuallyControlled(true);
    const Ogre::Vector3 origInitialPos = bone->getInitialPosition();
    const Ogre::Vector3 newBindPos(2.5f, 0, 0);

    BoneTransformCommand cmd(entity->getName(), "Child",
                              bone->getPosition(), bone->getOrientation(), bone->getScale(),
                              newBindPos, bone->getOrientation(), bone->getScale(),
                              /*bindMode=*/true);
    cmd.redo();
    // setInitialState() should have captured the new local as the new initial pose.
    EXPECT_EQ(bone->getInitialPosition(), newBindPos);

    cmd.undo();
    // After undo, the bind pose returns to the original initial pos.
    EXPECT_EQ(bone->getInitialPosition(), origInitialPos);
}

TEST_F(BoneTransformCommandTest, NonBindModeDoesNotChangeInitialState)
{
    auto* entity = createAnimatedTestEntity("BTC_nonbind");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* bone = skel->getBone("Child");
    bone->setManuallyControlled(true);
    const Ogre::Vector3 origInitialPos = bone->getInitialPosition();

    BoneTransformCommand cmd(entity->getName(), "Child",
                              bone->getPosition(), bone->getOrientation(), bone->getScale(),
                              Ogre::Vector3(3, 3, 3), bone->getOrientation(), bone->getScale(),
                              /*bindMode=*/false);
    cmd.redo();
    // Initial pose remains untouched in non-bind mode.
    EXPECT_EQ(bone->getInitialPosition(), origInitialPos);
}

TEST_F(BoneTransformCommandTest, CommandTextDifferentForBindMode)
{
    BoneTransformCommand normal(std::string("E"), std::string("B"),
                                 Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                                 Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                                 /*bindMode=*/false);
    BoneTransformCommand bind  (std::string("E"), std::string("B"),
                                 Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                                 Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE,
                                 /*bindMode=*/true);
    EXPECT_NE(normal.text(), bind.text());
}
