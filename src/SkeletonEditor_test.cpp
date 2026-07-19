#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
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

TEST_F(SkeletonEditorTest, ReparentKeepWorldPreservesDerivedPosition) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_ReparentWorld");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    Ogre::Bone* tip = skel->createBone("Tip", 2);
    skel->getBone("Child")->addChild(tip);
    tip->setPosition(Ogre::Vector3(0, 1, 0));
    tip->setInitialState();
    entity->_initialise(true);

    const Ogre::Vector3 worldBefore = tip->_getDerivedPosition();

    SkeletonEditor::ReparentOptions opts;
    opts.keepWorld = true;
    // Reparent Tip under Root (skip Child)
    const auto result = SkeletonEditor::reparentBone(
        entity, QStringLiteral("Tip"), QStringLiteral("Root"), opts);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    skel = entity->getMesh()->getSkeleton();
    Ogre::Bone* tipAfter = skel->getBone("Tip");
    ASSERT_NE(tipAfter->getParent(), nullptr);
    EXPECT_EQ(tipAfter->getParent()->getName(), "Root");
    EXPECT_NEAR(tipAfter->_getDerivedPosition().y, worldBefore.y, 1e-3f);
}

TEST_F(SkeletonEditorTest, ReparentKeepLocalPreservesLocalTRS) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_ReparentLocal");
    ASSERT_NE(entity, nullptr);
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    Ogre::Bone* tip = skel->createBone("Tip", 2);
    skel->getBone("Child")->addChild(tip);
    tip->setPosition(Ogre::Vector3(0.25f, 0.75f, 0));
    tip->setInitialState();
    entity->_initialise(true);

    const Ogre::Vector3 localBefore = tip->getPosition();

    SkeletonEditor::ReparentOptions opts;
    opts.keepWorld = false;
    const auto result = SkeletonEditor::reparentBone(
        entity, QStringLiteral("Tip"), QStringLiteral("Root"), opts);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    skel = entity->getMesh()->getSkeleton();
    Ogre::Bone* tipAfter = skel->getBone("Tip");
    EXPECT_NEAR(tipAfter->getPosition().x, localBefore.x, 1e-5f);
    EXPECT_NEAR(tipAfter->getPosition().y, localBefore.y, 1e-5f);
    EXPECT_NEAR(tipAfter->getPosition().z, localBefore.z, 1e-5f);
}

TEST_F(SkeletonEditorTest, DetachMovesChainToRoot) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Detach");
    ASSERT_NE(entity, nullptr);

    const auto result = SkeletonEditor::detachBone(entity, QStringLiteral("Child"));
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    Ogre::Bone* child = skel->getBone("Child");
    EXPECT_EQ(child->getParent(), nullptr);

    // Weights still resolve to Child handle.
    bool childWeighted = false;
    for (const auto& kv : entity->getMesh()->getBoneAssignments()) {
        if (kv.second.boneIndex == child->getHandle() && kv.second.weight > 0.f) {
            childWeighted = true;
            break;
        }
    }
    EXPECT_TRUE(childWeighted);
}

TEST_F(SkeletonEditorTest, ReparentRejectsCycle) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Cycle");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ReparentOptions opts;
    const auto result = SkeletonEditor::reparentBone(
        entity, QStringLiteral("Root"), QStringLiteral("Child"), opts);
    EXPECT_FALSE(result.ok);
}

TEST_F(SkeletonEditorTest, SplitBoneInsertsChildAndRemapsWeights) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Split");
    ASSERT_NE(entity, nullptr);
    Ogre::Mesh* mesh = entity->getMesh().get();
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

    Ogre::Bone* tip = skel->createBone("Tip", 2);
    skel->getBone("Child")->addChild(tip);
    tip->setPosition(Ogre::Vector3(0, 1, 0));
    tip->setInitialState();
    entity->_initialise(true);

    float weightSumBefore = 0.f;
    for (const auto& kv : mesh->getBoneAssignments())
        weightSumBefore += kv.second.weight;

    const float t = 0.25f;
    const auto result = SkeletonEditor::splitBone(entity, QStringLiteral("Child"), t);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    skel = entity->getMesh()->getSkeleton();
    ASSERT_TRUE(skel->hasBone(result.boneName.toStdString()));
    Ogre::Bone* split = skel->getBone(result.boneName.toStdString());
    ASSERT_NE(split->getParent(), nullptr);
    EXPECT_EQ(split->getParent()->getName(), "Child");
    // Split joint sits at fraction t along Child → Tip.
    EXPECT_NEAR(split->getInitialPosition().y, t, 1e-4f);
    ASSERT_TRUE(skel->hasBone("Tip"));
    Ogre::Bone* tipAfter = skel->getBone("Tip");
    ASSERT_NE(tipAfter->getParent(), nullptr);
    EXPECT_EQ(tipAfter->getParent()->getName(), result.boneName.toStdString());
    EXPECT_NEAR(tipAfter->getInitialPosition().y, 1.f - t, 1e-4f);

    float weightSumAfter = 0.f;
    for (const auto& kv : mesh->getBoneAssignments())
        weightSumAfter += kv.second.weight;
    EXPECT_NEAR(weightSumAfter, weightSumBefore, 1e-3f);
}

TEST_F(SkeletonEditorTest, ConnectDisconnectTogglesHeadGap) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_Connect");
    ASSERT_NE(entity, nullptr);

    EXPECT_TRUE(SkeletonEditor::isBoneConnected(entity, QStringLiteral("Child")));

    auto disc = SkeletonEditor::setBoneConnected(entity, QStringLiteral("Child"), false);
    ASSERT_TRUE(disc.ok) << disc.error.toStdString();
    EXPECT_FALSE(SkeletonEditor::isBoneConnected(entity, QStringLiteral("Child")));

    auto conn = SkeletonEditor::setBoneConnected(entity, QStringLiteral("Child"), true);
    ASSERT_TRUE(conn.ok) << conn.error.toStdString();
    EXPECT_TRUE(SkeletonEditor::isBoneConnected(entity, QStringLiteral("Child")));
}

TEST_F(SkeletonEditorTest, AttachBoneToEntityCopiesRigOnly) {
    Ogre::Entity* src = createAnimatedTestEntity("SkelEd_AttachSrc");
    Ogre::Entity* dst = createAnimatedTestEntity("SkelEd_AttachDst");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);

    // Force a name collision on destination.
    ASSERT_TRUE(dst->getMesh()->getSkeleton()->hasBone("Child"));

    const auto result = SkeletonEditor::attachBonesToEntity(
        src, QStringList{QStringLiteral("Child")}, dst, {});
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.boneName.startsWith(QStringLiteral("Child")));
    EXPECT_NE(result.boneName, QStringLiteral("Child"));

    Ogre::SkeletonPtr dstSkel = dst->getMesh()->getSkeleton();
    EXPECT_TRUE(dstSkel->hasBone(result.boneName.toStdString()));
    EXPECT_EQ(dstSkel->getNumBones(), 3u);
    // Source unchanged.
    EXPECT_TRUE(src->getMesh()->getSkeleton()->hasBone("Child"));
    EXPECT_EQ(src->getMesh()->getSkeleton()->getNumBones(), 2u);
}

TEST_F(SkeletonEditorTest, ReparentAndSplitUndoViaCommand) {
    Ogre::Entity* entity = createAnimatedTestEntity("SkelEd_HierUndo");
    ASSERT_NE(entity, nullptr);

    SkeletonEditor::ReparentOptions opts;
    opts.keepWorld = true;
    UndoManager::getSingleton()->push(
        new ReparentBoneCommand(entity->getName(), QStringLiteral("Child"), {}, opts));
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getBone("Child")->getParent(), nullptr);

    UndoManager::getSingleton()->undo();
    EXPECT_NE(entity->getMesh()->getSkeleton()->getBone("Child")->getParent(), nullptr);

    UndoManager::getSingleton()->push(
        new SplitBoneCommand(entity->getName(), QStringLiteral("Child"), 0.5f));
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getNumBones(), 3u);

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getNumBones(), 2u);
}
