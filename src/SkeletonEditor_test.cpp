#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "SkeletonEditor.h"
#include "UndoManager.h"
#include "commands/SkeletonBoneCommands.h"
#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeleton.h>

class SkeletonEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        SkeletonEditor::kill();
        UndoManager::kill();
        AnimationControlController::kill();
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());
        UndoManager::getSingleton();
    }

    void TearDown() override {
        SkeletonEditor::kill();
        UndoManager::kill();
        AnimationControlController::kill();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }

    QApplication* app = nullptr;
};

TEST_F(SkeletonEditorTest, CreateBoneUnderParent) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Create");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    ASSERT_NE(skel, nullptr);

    SkeletonEditor::CreateOptions opts;
    opts.parentBoneName = QStringLiteral("Root");
    const auto result = SkeletonEditor::createBone(entity, opts);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(skel->hasBone(result.boneName.toStdString()));

    Ogre::Bone* created = skel->getBone(result.boneName.toStdString());
    ASSERT_NE(created->getParent(), nullptr);
    EXPECT_EQ(created->getParent()->getName(), "Root");
}

TEST_F(SkeletonEditorTest, UniqueBoneNameUsesBlenderSuffix) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Unique");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    EXPECT_EQ(SkeletonEditor::uniqueBoneName(skel.get(), QStringLiteral("Bone")),
              QStringLiteral("Bone"));
    EXPECT_EQ(SkeletonEditor::uniqueBoneName(skel.get(), QStringLiteral("Root")),
              QStringLiteral("Root.001"));
}

TEST_F(SkeletonEditorTest, RenameBoneUpdatesAnimationTrackBoneName) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Rename");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    ASSERT_TRUE(skel->hasAnimation("TestAnim"));

    const auto result = SkeletonEditor::renameBone(entity, QStringLiteral("Child"),
                                                   QStringLiteral("RenamedChild"));
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    skel = entity->getMesh()->getSkeleton();
    EXPECT_TRUE(skel->hasBone("RenamedChild"));
    EXPECT_FALSE(skel->hasBone("Child"));

    Ogre::Animation* anim = skel->getAnimation("TestAnim");
    bool foundTrack = false;
    for (const auto& [handle, track] : anim->_getNodeTrackList()) {
        if (track->getAssociatedNode()
            && track->getAssociatedNode()->getName() == "RenamedChild") {
            foundTrack = true;
            break;
        }
    }
    EXPECT_TRUE(foundTrack);
}

TEST_F(SkeletonEditorTest, DuplicateBoneCreatesSiblingWithSuffix) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Dup");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    const auto result = SkeletonEditor::duplicateBone(entity, QStringLiteral("Child"));
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.boneName.startsWith(QStringLiteral("Child.")));
    EXPECT_TRUE(skel->hasBone(result.boneName.toStdString()));

    Ogre::Bone* dup = skel->getBone(result.boneName.toStdString());
    Ogre::Bone* src = skel->getBone("Child");
    ASSERT_NE(dup->getParent(), nullptr);
    EXPECT_EQ(dup->getParent()->getName(), src->getParent()->getName());
}

TEST_F(SkeletonEditorTest, RemoveBoneTransfersWeightsToParent) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Remove");
    ASSERT_NE(entity, nullptr);
    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::SkeletonPtr skel = mesh->getSkeleton();

    SkeletonEditor::RemoveOptions opts;
    opts.removeChildren = false;
    opts.transferWeightsToParent = true;
    const auto result = SkeletonEditor::removeBone(entity, QStringLiteral("Child"), opts);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    skel = mesh->getSkeleton();
    EXPECT_EQ(skel->getNumBones(), 1u);
    EXPECT_FALSE(skel->hasBone("Child"));
    EXPECT_TRUE(skel->hasBone("Root"));

    bool rootWeighted = false;
    for (const auto& kv : mesh->getBoneAssignments()) {
        if (kv.second.boneIndex == 0 && kv.second.weight > 0.f) {
            rootWeighted = true;
            break;
        }
    }
    EXPECT_TRUE(rootWeighted);
}

TEST_F(SkeletonEditorTest, PromoteChildrenPreservesWorldTransform) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Promote");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    Ogre::Bone* child = skel->getBone("Child");
    Ogre::Bone* tip = skel->createBone("Tip", 2);
    child->addChild(tip);
    tip->setPosition(Ogre::Vector3(0, 1, 0));
    tip->setInitialState();
    entity->_initialise(true);

    const Ogre::Vector3 tipWorldBefore = tip->_getDerivedPosition();

    SkeletonEditor::RemoveOptions opts;
    opts.removeChildren = false;
    opts.transferWeightsToParent = false;
    const auto result = SkeletonEditor::removeBone(entity, QStringLiteral("Child"), opts);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    skel = entity->getMesh()->getSkeleton();
    ASSERT_TRUE(skel->hasBone("Tip"));
    EXPECT_FALSE(skel->hasBone("Child"));
    Ogre::Bone* tipAfter = skel->getBone("Tip");
    EXPECT_NEAR(tipAfter->_getDerivedPosition().y, tipWorldBefore.y, 1e-4f);
}

TEST_F(SkeletonEditorTest, CreateAndUndoViaCommand) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_UndoCreate");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    const unsigned short beforeCount = skel->getNumBones();

    SkeletonEditor::CreateOptions opts;
    opts.parentBoneName = QStringLiteral("Root");
    UndoManager::getSingleton()->push(new CreateBoneCommand(entity->getName(), opts));
    skel = entity->getMesh()->getSkeleton();
    EXPECT_EQ(skel->getNumBones(), beforeCount + 1);

    UndoManager::getSingleton()->undo();
    skel = entity->getMesh()->getSkeleton();
    EXPECT_EQ(skel->getNumBones(), beforeCount);
}

TEST_F(SkeletonEditorTest, RemoveAndUndoViaCommand) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_UndoRemove");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    ASSERT_TRUE(skel->hasBone("Child"));

    SkeletonEditor::RemoveOptions opts;
    UndoManager::getSingleton()->push(
        new RemoveBoneCommand(entity->getName(), QStringLiteral("Child"), opts));
    skel = entity->getMesh()->getSkeleton();
    EXPECT_FALSE(skel->hasBone("Child"));

    UndoManager::getSingleton()->undo();
    skel = entity->getMesh()->getSkeleton();
    EXPECT_TRUE(skel->hasBone("Child"));
    EXPECT_TRUE(skel->hasAnimation("TestAnim"));
}

TEST_F(SkeletonEditorTest, SnapshotRoundTrip) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Snap");
    ASSERT_NE(entity, nullptr);
    const auto snap = SkeletonEditor::captureSnapshot(entity);
    EXPECT_FALSE(snap.bones.empty());
    EXPECT_FALSE(snap.animations.empty());

    SkeletonEditor::RemoveOptions opts;
    ASSERT_TRUE(SkeletonEditor::removeBone(entity, QStringLiteral("Child"), opts).ok);

    QString err;
    EXPECT_TRUE(SkeletonEditor::restoreSnapshot(entity, snap, &err)) << err.toStdString();
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    EXPECT_TRUE(skel->hasBone("Child"));
    EXPECT_TRUE(skel->hasAnimation("TestAnim"));
}

TEST_F(SkeletonEditorTest, DuplicateBoneRebindsAnimationControllerSkeleton) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_DupRebind");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->selectOne(entity->getParentSceneNode());

    auto* anim = AnimationControlController::instance();
    anim->updateAnimationTree();
    anim->selectAnimation(QString::fromStdString(entity->getName()), QStringLiteral("TestAnim"));
    anim->selectBone(QStringLiteral("Child"));

    const auto result = SkeletonEditor::duplicateBone(entity, QStringLiteral("Child"));
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    SkeletonEditor::refreshAfterEdit(entity->getName(), result.boneName);

    EXPECT_EQ(anim->selectedBone(), result.boneName);
    EXPECT_NE(anim->selectedBonePtr(), nullptr);
    EXPECT_TRUE(anim->boneNames().contains(result.boneName));
    EXPECT_EQ(anim->selectedEntity()->getSkeleton(), entity->getSkeleton());
}
