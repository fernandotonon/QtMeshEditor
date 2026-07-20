#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QThread>

#include "SkeletonEditor.h"
#include "SkeletonDebug.h"
#include "UndoManager.h"
#include "commands/SkeletonBoneCommands.h"
#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreHardwareBuffer.h>
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

namespace {

Ogre::Entity* createThreeBoneAnimatedEntity(const std::string& name)
{
    Ogre::Entity* entity = createAnimatedTestEntity(name);
    if (!entity) return nullptr;
    Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
    Ogre::Bone* child = skel->getBone("Child");
    Ogre::Bone* tip = skel->createBone("Tip", 2);
    tip->setPosition(Ogre::Vector3(0, 1, 0));
    child->addChild(tip);
    tip->setInitialState();
    skel->setBindingPose();
    entity->_initialise(true);
    return entity;
}

Ogre::Vector3 sampleBoneDerived(Ogre::Entity* entity, const char* boneName, float time)
{
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    Ogre::SkeletonPtr meshSkel = entity->getMesh()->getSkeleton();
    if (!skel || !meshSkel || !meshSkel->hasAnimation("TestAnim"))
        return Ogre::Vector3::ZERO;

    // Apply the mesh skeleton's animation directly onto the instance.
    // Entity::_updateAnimation can leave bones at bind after a prior t=0
    // sample on this fixture; Animation::apply is deterministic.
    skel->reset(true);
    Ogre::Animation* anim = meshSkel->getAnimation("TestAnim");
    anim->apply(skel, time, 1.0f, Ogre::Real(1.0f));
    for (Ogre::Bone* root : skel->getRootBones())
        root->_update(true, false);
    return skel->getBone(boneName)->_getDerivedPosition();
}

} // namespace

TEST_F(SkeletonEditorTest, CaptureRestPoseRebasesAnimation) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestCapture");
    ASSERT_NE(entity, nullptr);
    ASSERT_EQ(entity->getMesh()->getSkeleton()->getNumBones(), 3u);

    // Pose samples for world-motion preservation checks.
    const Ogre::Vector3 beforeT0 = sampleBoneDerived(entity, "Child", 0.0f);
    const Ogre::Vector3 beforeT05 = sampleBoneDerived(entity, "Child", 0.5f);
    const Ogre::Vector3 beforeT1 = sampleBoneDerived(entity, "Child", 1.0f);
    // Mid-frame must actually move — otherwise the fixture/anim is broken.
    ASSERT_GT(beforeT05.distance(beforeT0), 0.1f);

    sampleBoneDerived(entity, "Child", 0.5f);
    const auto result = SkeletonEditor::captureRestPose(entity);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    Ogre::Bone* meshChild = entity->getMesh()->getSkeleton()->getBone("Child");
    EXPECT_NEAR(meshChild->getInitialPosition().x, 0.5f, 1e-4f);

    Ogre::Animation* anim = entity->getMesh()->getSkeleton()->getAnimation("TestAnim");
    Ogre::NodeAnimationTrack* track = nullptr;
    for (const auto& [handle, t] : anim->_getNodeTrackList()) {
        if (t->getAssociatedNode() && t->getAssociatedNode()->getName() == "Child") {
            track = t;
            break;
        }
    }
    ASSERT_NE(track, nullptr);
    const auto* kfMid = track->getNodeKeyFrame(1);
    EXPECT_NEAR(kfMid->getTranslate().length(), 0.0f, 1e-3f);

    EXPECT_NEAR(sampleBoneDerived(entity, "Child", 0.0f).distance(beforeT0), 0.0f, 1e-3f);
    EXPECT_NEAR(sampleBoneDerived(entity, "Child", 0.5f).distance(beforeT05), 0.0f, 1e-3f);
    EXPECT_NEAR(sampleBoneDerived(entity, "Child", 1.0f).distance(beforeT1), 0.0f, 1e-3f);
}

TEST_F(SkeletonEditorTest, ResetRestPoseRestoresImportedBind) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestReset");
    ASSERT_NE(entity, nullptr);

    SkeletonEditor::ensureImportedRestCache(entity);
    const Ogre::Vector3 imported =
        entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition();

    sampleBoneDerived(entity, "Child", 0.5f);
    ASSERT_TRUE(SkeletonEditor::captureRestPose(entity).ok);
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.5f, 1e-4f);

    const auto reset = SkeletonEditor::resetRestPose(entity);
    ASSERT_TRUE(reset.ok) << reset.error.toStdString();
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition(),
              imported);
}

TEST_F(SkeletonEditorTest, SnapSelectedBoneLeavesOthersUnchanged) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestSnap");
    ASSERT_NE(entity, nullptr);

    const Ogre::Vector3 tipInitial =
        entity->getMesh()->getSkeleton()->getBone("Tip")->getInitialPosition();

    sampleBoneDerived(entity, "Child", 0.5f);
    const auto result = SkeletonEditor::captureRestPose(
        entity, QStringList{QStringLiteral("Child")});
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.5f, 1e-4f);
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getBone("Tip")->getInitialPosition(),
              tipInitial);
}

TEST_F(SkeletonEditorTest, SetRestPoseCommandUndoRedo) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestUndo");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);

    sampleBoneDerived(entity, "Child", 0.5f);
    auto* cmd = new SetRestPoseCommand(entity->getName(), SetRestPoseCommand::Op::CaptureAll);
    UndoManager::getSingleton()->push(cmd);
    ASSERT_TRUE(cmd->applied());
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.5f, 1e-4f);

    UndoManager::getSingleton()->undo();
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.0f, 1e-4f);

    UndoManager::getSingleton()->redo();
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.5f, 1e-4f);
}

// Gizmo CommitBind must use the captured after-TRS, not re-read the instance
// bone — an animation tick can reset the bone to the old bind between drag
// end and undo-push, which previously made the mesh snap back to rest while
// debug joints looked posed.
TEST_F(SkeletonEditorTest, ExplicitPoseCommitIgnoresStaleInstanceBone) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestExplicit");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);

    const Ogre::Vector3 capturedPos(0.4f, 1.0f, 0.0f);
    const Ogre::Quaternion capturedOri = Ogre::Quaternion::IDENTITY;
    const Ogre::Vector3 capturedScale = Ogre::Vector3::UNIT_SCALE;

    // Simulate post-drag reset: instance bone already back at old bind.
    Ogre::SkeletonInstance* inst = entity->getSkeleton();
    ASSERT_NE(inst, nullptr);
    Ogre::Bone* child = inst->getBone("Child");
    ASSERT_NE(child, nullptr);
    child->setPosition(Ogre::Vector3::ZERO);
    child->setOrientation(Ogre::Quaternion::IDENTITY);
    child->setScale(Ogre::Vector3::UNIT_SCALE);
    child->setManuallyControlled(false);

    SetRestPoseCommand::ExplicitPose pose;
    pose.boneName = QStringLiteral("Child");
    pose.position = capturedPos;
    pose.orientation = capturedOri;
    pose.scale = capturedScale;

    auto* cmd = new SetRestPoseCommand(entity->getName(), std::vector{pose});
    UndoManager::getSingleton()->push(cmd);
    ASSERT_TRUE(cmd->applied());
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.4f, 1e-4f);

    UndoManager::getSingleton()->undo();
    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.0f, 1e-4f);
}

// Rest commit must bake skinned verts into the mesh bind buffers.
// Changing bone initials alone leaves LBS as identity at the new bind,
// so the mesh would always show the authored T-pose geometry again.
TEST_F(SkeletonEditorTest, CommitBoneRestPoseBakesMeshVertices) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestBakeMesh");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);

    Ogre::Mesh* mesh = entity->getMesh().get();
    ASSERT_NE(mesh->sharedVertexData, nullptr);
    const auto* posElem =
        mesh->sharedVertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    ASSERT_NE(posElem, nullptr);
    auto vbuf = mesh->sharedVertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    float* p0 = nullptr;
    posElem->baseVertexPointerToElement(base, &p0);
    const Ogre::Vector3 before(p0[0], p0[1], p0[2]);
    vbuf->unlock();

    // Rotate Child 90° so weighted verts must move when baked.
    const Ogre::Quaternion rot(Ogre::Radian(Ogre::Degree(90)), Ogre::Vector3::UNIT_Z);
    const auto result = SkeletonEditor::commitBoneRestPose(
        entity, QStringLiteral("Child"),
        entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition(),
        rot, Ogre::Vector3::UNIT_SCALE);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    posElem->baseVertexPointerToElement(base, &p0);
    const Ogre::Vector3 after(p0[0], p0[1], p0[2]);
    vbuf->unlock();

    EXPECT_GT(before.distance(after), 1e-3f)
        << "Bind-pose mesh verts should move after rest-pose bake";

    // At the new bind, skinning is identity — mesh must still show the baked pose.
    entity->getSkeleton()->reset(true);
    entity->_updateAnimation();
    base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    posElem->baseVertexPointerToElement(base, &p0);
    const Ogre::Vector3 atBind(p0[0], p0[1], p0[2]);
    vbuf->unlock();
    EXPECT_NEAR(atBind.distance(after), 0.0f, 1e-4f);
}

// Bake must not bump mesh state count — that forces Entity::_initialise(true)
// and destroys SkeletonDebug TagPoints (overlay vanishes, checkbox stays on).
TEST_F(SkeletonEditorTest, CommitBoneRestPoseDoesNotDirtyMeshState) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestNoDirty");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);
    Ogre::Mesh* mesh = entity->getMesh().get();
    const size_t stateBefore = mesh->getStateCount();

    const auto result = SkeletonEditor::commitBoneRestPose(
        entity, QStringLiteral("Child"),
        Ogre::Vector3(0.2f, 1.0f, 0.0f),
        Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(mesh->getStateCount(), stateBefore);
}

// Gizmo rest commit must NOT rebake clips to preserve absolute poses —
// that made Mixamo-enabled meshes snap back to the pre-edit look on release.
TEST_F(SkeletonEditorTest, CommitBoneRestPoseKeepsNewBindWithEnabledAnim) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestCommitNoRebake");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);

    ASSERT_TRUE(entity->hasAnimationState("TestAnim"));
    auto* state = entity->getAnimationState("TestAnim");
    state->setEnabled(true);
    state->setTimePosition(0.0f);

    const Ogre::Vector3 newPos(0.35f, 1.0f, 0.0f);
    const auto result = SkeletonEditor::commitBoneRestPose(
        entity, QStringLiteral("Child"), newPos,
        Ogre::Quaternion::IDENTITY, Ogre::Vector3::UNIT_SCALE);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    EXPECT_NEAR(entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition().x,
                0.35f, 1e-4f);

    // Clip must still be enabled, and evaluating at t=0 (identity keys in
    // this fixture) must land on the new bind — not the old.
    ASSERT_TRUE(entity->hasAnimationState("TestAnim"));
    EXPECT_TRUE(entity->getAnimationState("TestAnim")->getEnabled());
    entity->getAnimationState("TestAnim")->setTimePosition(0.0f);
    entity->_updateAnimation();
    entity->getSkeleton()->_updateTransforms();
    EXPECT_NEAR(entity->getSkeleton()->getBone("Child")->getPosition().x, 0.35f, 1e-3f);
}

// Rest ghost must keep a private mesh clone so a rest bake does not move it.
TEST_F(SkeletonEditorTest, RestGhostMeshStaysAtEnableTimePoseAfterBake) {
    Ogre::Entity* entity = createThreeBoneAnimatedEntity("SkelEd_RestGhostFreeze");
    ASSERT_NE(entity, nullptr);
    SkeletonEditor::ensureImportedRestCache(entity);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    SkeletonDebug ghostHost(entity, sceneMgr, 0.1f, 0.01f);
    ghostHost.showBones(true);
    ghostHost.showRestGhost(true);
    ASSERT_TRUE(ghostHost.restGhostShown());
    ASSERT_TRUE(ghostHost.bonesShown());

    const Ogre::String ghostMeshName = entity->getName() + "_restGhostMeshRes";
    Ogre::MeshPtr ghostMesh = Ogre::static_pointer_cast<Ogre::Mesh>(
        Ogre::MeshManager::getSingleton().getByName(ghostMeshName));
    ASSERT_NE(ghostMesh, nullptr);
    ASSERT_NE(ghostMesh->sharedVertexData, nullptr);

    auto readFirstPos = [](Ogre::Mesh* mesh) -> Ogre::Vector3 {
        const auto* posElem =
            mesh->sharedVertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        auto vbuf = mesh->sharedVertexData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        float* p0 = nullptr;
        posElem->baseVertexPointerToElement(base, &p0);
        Ogre::Vector3 v(p0[0], p0[1], p0[2]);
        vbuf->unlock();
        return v;
    };

    const Ogre::Vector3 ghostBefore = readFirstPos(ghostMesh.get());
    const Ogre::Vector3 liveBefore = readFirstPos(entity->getMesh().get());
    EXPECT_NEAR(ghostBefore.distance(liveBefore), 0.0f, 1e-4f);

    const Ogre::Quaternion rot(Ogre::Radian(Ogre::Degree(90)), Ogre::Vector3::UNIT_Z);
    const auto result = SkeletonEditor::commitBoneRestPose(
        entity, QStringLiteral("Child"),
        entity->getMesh()->getSkeleton()->getBone("Child")->getInitialPosition(),
        rot, Ogre::Vector3::UNIT_SCALE);
    ASSERT_TRUE(result.ok) << result.error.toStdString();

    const Ogre::Vector3 ghostAfter = readFirstPos(ghostMesh.get());
    const Ogre::Vector3 liveAfter = readFirstPos(entity->getMesh().get());
    EXPECT_NEAR(ghostBefore.distance(ghostAfter), 0.0f, 1e-4f)
        << "Ghost mesh clone must stay at the enable-time rest";
    EXPECT_GT(liveBefore.distance(liveAfter), 1e-3f)
        << "Live mesh should bake to the new rest";

    // Skeleton overlay flag must survive the bake (no mesh-state reinit).
    EXPECT_TRUE(ghostHost.bonesShown());
    EXPECT_TRUE(ghostHost.restGhostShown());
}

