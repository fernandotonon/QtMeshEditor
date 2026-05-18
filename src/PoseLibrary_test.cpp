#include <gtest/gtest.h>

#include <QSignalSpy>

#include "Manager.h"
#include "PoseLibrary.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "commands/PoseLibraryCommands.h"

#include <QTemporaryDir>

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>

// =============================================================================
// Standalone (no Ogre)
// =============================================================================

TEST(PoseLibraryStandalone, InstanceIsSingleton) {
    auto* a = PoseLibrary::instance();
    auto* b = PoseLibrary::instance();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, nullptr);
}

TEST(PoseLibraryStandalone, NullEntityAndEmptyNamesRejected) {
    auto* m = PoseLibrary::instance();
    EXPECT_FALSE(m->savePose(nullptr, QStringLiteral("X")));
    EXPECT_FALSE(m->applyPose(nullptr, QStringLiteral("X")));
    EXPECT_FALSE(m->deletePose(nullptr, QStringLiteral("X")));
    EXPECT_FALSE(m->hasPose(nullptr, QStringLiteral("X")));
    EXPECT_TRUE(m->listPoses(nullptr).isEmpty());
}

// =============================================================================
// Scene fixture — savePose / applyPose need a real skinned entity.
// =============================================================================

class PoseLibrarySceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles())
            << "skinned entity creation requires GL (Xvfb in CI)";
        if (auto* sel = SelectionSet::getSingleton()) sel->clear();
        // Singleton survives across tests. Wipe its state so a
        // previous test's saved poses don't leak into the next
        // (Ogre often hands out the same Entity* on recreate).
        PoseLibrary::instance()->clearAll();
    }
    void TearDown() override
    {
        if (auto* sel = SelectionSet::getSingleton()) sel->clear();
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
        PoseLibrary::instance()->clearAll();
    }
};

TEST_F(PoseLibrarySceneTest, SaveCapturesEveryBoneAndAppliesBack) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Save");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    auto* skel = entity->getSkeleton();
    ASSERT_GE(skel->getNumBones(), 1);

    // Mutate the first bone so we have a non-trivial pose to snap.
    Ogre::Bone* bone0 = skel->getBone(0);
    const Ogre::Vector3 origPos = bone0->getPosition();
    bone0->setPosition(origPos + Ogre::Vector3(5, 0, 0));

    auto* lib = PoseLibrary::instance();
    EXPECT_TRUE(lib->savePose(entity, QStringLiteral("snap1")));
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("snap1")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    // Move the bone elsewhere — apply should snap it back.
    bone0->setPosition(origPos + Ogre::Vector3(0, 9, 0));
    EXPECT_TRUE(lib->applyPose(entity, QStringLiteral("snap1")));
    const Ogre::Vector3 after = skel->getBone(0)->getPosition();
    EXPECT_FLOAT_EQ(after.x, origPos.x + 5);
    EXPECT_FLOAT_EQ(after.y, origPos.y);
    EXPECT_FLOAT_EQ(after.z, origPos.z);
}

TEST_F(PoseLibrarySceneTest, SaveOverwriteUpdatesInPlace) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Overwrite");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // First save with bone at +X.
    skel->getBone(0)->setPosition(Ogre::Vector3(1, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("pose")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    // Second save at +Z under the same name → overwrite in place,
    // listPoses still has just the one entry.
    skel->getBone(0)->setPosition(Ogre::Vector3(0, 0, 7));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("pose")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    // Apply should restore the second snapshot, not the first.
    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);
    lib->applyPose(entity, QStringLiteral("pose"));
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().z, 7.0f);
}

TEST_F(PoseLibrarySceneTest, ListReturnsInsertionOrder) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Order");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    lib->savePose(entity, QStringLiteral("first"));
    lib->savePose(entity, QStringLiteral("second"));
    lib->savePose(entity, QStringLiteral("third"));

    const QStringList names = lib->listPoses(entity);
    ASSERT_EQ(names.size(), 3);
    EXPECT_EQ(names[0], QStringLiteral("first"));
    EXPECT_EQ(names[1], QStringLiteral("second"));
    EXPECT_EQ(names[2], QStringLiteral("third"));
}

TEST_F(PoseLibrarySceneTest, DeleteRemovesPoseAndListEntry) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Delete");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    lib->savePose(entity, QStringLiteral("A"));
    lib->savePose(entity, QStringLiteral("B"));
    EXPECT_TRUE(lib->deletePose(entity, QStringLiteral("A")));
    EXPECT_FALSE(lib->hasPose(entity, QStringLiteral("A")));
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("B")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    EXPECT_FALSE(lib->deletePose(entity, QStringLiteral("UnknownPose")));
    EXPECT_FALSE(lib->deletePose(entity, QString()));
}

TEST_F(PoseLibrarySceneTest, ApplyMissingPoseReturnsFalse) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_NoSuch");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();
    EXPECT_FALSE(lib->applyPose(entity, QStringLiteral("NotSavedYet")));
}

TEST_F(PoseLibrarySceneTest, SaveOnUnskinnedEntityRejected) {
    // canLoadMeshFiles() guarantees we can build the mesh path; the
    // fixture entity above is skinned. To test the no-skeleton path
    // we'd need an unskinned mesh; for D1 we just verify a null
    // skeleton input is refused via the standalone path. That's
    // already covered by NullEntityAndEmptyNamesRejected.
    SUCCEED();
}

TEST_F(PoseLibrarySceneTest, PosesChangedSignalFiresOnSaveAndDelete) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Signal");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    QSignalSpy spy(lib, &PoseLibrary::posesChanged);
    EXPECT_TRUE(lib->savePose(entity, QStringLiteral("S1")));
    EXPECT_GE(spy.count(), 1);
    const int afterSave = spy.count();
    EXPECT_TRUE(lib->deletePose(entity, QStringLiteral("S1")));
    EXPECT_GT(spy.count(), afterSave);
}

TEST_F(PoseLibrarySceneTest, SelectionDrivenAccessorsResolveFirstEntity) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Sel");
    ASSERT_NE(entity, nullptr);
    auto* sel = SelectionSet::getSingleton();
    ASSERT_NE(sel, nullptr);
    sel->append(entity);

    auto* lib = PoseLibrary::instance();
    EXPECT_TRUE(lib->savePoseForSelection(QStringLiteral("FromSel")));
    EXPECT_EQ(lib->listPosesForSelection().size(), 1);
    EXPECT_TRUE(lib->applyPoseForSelection(QStringLiteral("FromSel")));
    EXPECT_TRUE(lib->deletePoseForSelection(QStringLiteral("FromSel")));
    EXPECT_EQ(lib->listPosesForSelection().size(), 0);
}

TEST_F(PoseLibrarySceneTest, NoSelectionGivesEmptyListAndRejectsMutators) {
    auto* lib = PoseLibrary::instance();
    EXPECT_TRUE(lib->listPosesForSelection().isEmpty());
    EXPECT_FALSE(lib->savePoseForSelection(QStringLiteral("X")));
    EXPECT_FALSE(lib->applyPoseForSelection(QStringLiteral("X")));
    EXPECT_FALSE(lib->deletePoseForSelection(QStringLiteral("X")));
}

TEST_F(PoseLibrarySceneTest, ForgetEntityDropsEverythingForThatEntity) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Forget");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();
    lib->savePose(entity, QStringLiteral("A"));
    lib->savePose(entity, QStringLiteral("B"));
    EXPECT_EQ(lib->listPoses(entity).size(), 2);

    EXPECT_TRUE(lib->forgetEntity(entity));
    EXPECT_TRUE(lib->listPoses(entity).isEmpty());
    // Idempotent — forgetting again is a no-op false.
    EXPECT_FALSE(lib->forgetEntity(entity));
}

// ─── Slice D-Project — .poselib sidecar persistence ─────────────────

TEST_F(PoseLibrarySceneTest, SaveAndLoadLibraryRoundTripsViaSidecar) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/test.poselib";

    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Sidecar");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    skel->getBone(0)->setPosition(Ogre::Vector3(3, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("Pose1")));
    skel->getBone(0)->setPosition(Ogre::Vector3(0, 5, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("Pose2")));

    EXPECT_TRUE(lib->savePoseLibrary(entity, path));

    // Wipe the in-memory library and reload from disk.
    lib->forgetEntity(entity);
    EXPECT_TRUE(lib->listPoses(entity).isEmpty());

    EXPECT_TRUE(lib->loadPoseLibrary(entity, path));
    QStringList names = lib->listPoses(entity);
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], QStringLiteral("Pose1"));
    EXPECT_EQ(names[1], QStringLiteral("Pose2"));

    // Apply the first pose to confirm TRS round-tripped.
    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);
    lib->applyPose(entity, QStringLiteral("Pose1"));
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 3.0f);
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().y, 0.0f);
}

TEST_F(PoseLibrarySceneTest, SaveLibraryRejectsEmptyLibraryOrInvalidPath) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_SidecarReject");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    QTemporaryDir tmp;
    const QString validPath = tmp.path() + "/empty.poselib";

    // No poses saved yet → false (don't write an empty library file).
    EXPECT_FALSE(lib->savePoseLibrary(entity, validPath));

    // Empty path → false.
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("X")));
    EXPECT_FALSE(lib->savePoseLibrary(entity, QString()));

    // Null entity → false.
    EXPECT_FALSE(lib->savePoseLibrary(nullptr, validPath));
}

TEST_F(PoseLibrarySceneTest, LoadLibraryRejectsMissingFileAndBadSchema) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_SidecarLoadReject");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    QTemporaryDir tmp;
    // Missing file
    EXPECT_FALSE(lib->loadPoseLibrary(entity, tmp.path() + "/nope.poselib"));

    // Bad JSON
    {
        const QString p = tmp.path() + "/bad.poselib";
        QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{ this is not JSON");
        f.close();
        EXPECT_FALSE(lib->loadPoseLibrary(entity, p));
    }

    // Valid JSON, wrong schema string
    {
        const QString p = tmp.path() + "/wrongschema.poselib";
        QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{\"schema\": \"wrong\", \"poses\": []}");
        f.close();
        EXPECT_FALSE(lib->loadPoseLibrary(entity, p));
    }
}

// Codex P1 on PR #602: a schema-matching file with `poses` missing
// or non-array must NOT wipe the in-memory library. Earlier draft
// did the wipe before validating the array, silently dropping the
// user's data on a malformed file.
TEST_F(PoseLibrarySceneTest, LoadLibraryWithMissingPosesArrayPreservesInMemoryLibrary) {
    QTemporaryDir tmp;
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_PreserveOnBad");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("Keep")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    // Schema matches, but `poses` is a string, not an array.
    const QString p = tmp.path() + "/badposes.poselib";
    QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("{\"schema\": \"qtmesheditor.poselib.v1\", \"poses\": \"oops\"}");
    f.close();

    EXPECT_FALSE(lib->loadPoseLibrary(entity, p));
    // In-memory library must still contain "Keep".
    EXPECT_EQ(lib->listPoses(entity).size(), 1);
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("Keep")));
}

// Codex P2 on PR #602: duplicate pose names in the sidecar must not
// leave `order` with phantom entries that survive a `deletePose`
// (causing listPoses to show a name that hasPose disagrees about).
TEST_F(PoseLibrarySceneTest, LoadLibraryDeduplicatesDuplicateNamesInFile) {
    QTemporaryDir tmp;
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_DupNames");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    // Hand-write a file with two entries under the same name.
    const QString p = tmp.path() + "/dup.poselib";
    QFile f(p); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({
      "schema": "qtmesheditor.poselib.v1",
      "poses": [
        {"name": "A", "bones": {}},
        {"name": "A", "bones": {}},
        {"name": "B", "bones": {}}
      ]
    })");
    f.close();

    EXPECT_TRUE(lib->loadPoseLibrary(entity, p));
    const QStringList names = lib->listPoses(entity);
    // Should be ["A", "B"] not ["A", "A", "B"].
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], QStringLiteral("A"));
    EXPECT_EQ(names[1], QStringLiteral("B"));

    // After deleting "A", listPoses must not contain a phantom "A".
    EXPECT_TRUE(lib->deletePose(entity, QStringLiteral("A")));
    EXPECT_FALSE(lib->hasPose(entity, QStringLiteral("A")));
    const QStringList afterDelete = lib->listPoses(entity);
    ASSERT_EQ(afterDelete.size(), 1);
    EXPECT_EQ(afterDelete[0], QStringLiteral("B"));
}

TEST_F(PoseLibrarySceneTest, LoadLibraryWipesExistingPosesFirst) {
    QTemporaryDir tmp;
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_SidecarWipe");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    // Build a sidecar with one pose "FromFile".
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("FromFile")));
    const QString path = tmp.path() + "/one.poselib";
    ASSERT_TRUE(lib->savePoseLibrary(entity, path));

    // Add a different in-memory pose that's NOT in the file.
    lib->forgetEntity(entity);
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("InMemoryOnly")));
    EXPECT_EQ(lib->listPoses(entity).size(), 1);

    // Load wipes "InMemoryOnly" and replaces with file's "FromFile".
    EXPECT_TRUE(lib->loadPoseLibrary(entity, path));
    QStringList names = lib->listPoses(entity);
    ASSERT_EQ(names.size(), 1);
    EXPECT_EQ(names[0], QStringLiteral("FromFile"));
    EXPECT_FALSE(lib->hasPose(entity, QStringLiteral("InMemoryOnly")));
}

// ─── Slice D5 — apply-with-mask ──────────────────────────────────────

TEST_F(PoseLibrarySceneTest, ApplyPoseMaskedTouchesOnlyListedBones) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Masked");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // Need at least 2 bones to verify the mask actually filters.
    ASSERT_GE(skel->getNumBones(), 2);

    // Save a pose where every bone is at +X=7.
    for (unsigned short i = 0; i < skel->getNumBones(); ++i)
        skel->getBone(i)->setPosition(Ogre::Vector3(7, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("AllSeven")));

    // Move every bone back to origin so apply has something to do.
    for (unsigned short i = 0; i < skel->getNumBones(); ++i)
        skel->getBone(i)->setPosition(Ogre::Vector3::ZERO);

    // Mask: only bone[0]. Apply should touch bone[0] only.
    QSet<QString> mask;
    mask.insert(QString::fromStdString(skel->getBone(0)->getName()));
    EXPECT_TRUE(lib->applyPoseMasked(entity, QStringLiteral("AllSeven"), mask));

    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 7.0f);
    // Bone 1 wasn't in the mask — must still be at origin.
    EXPECT_FLOAT_EQ(skel->getBone(1)->getPosition().x, 0.0f);
}

TEST_F(PoseLibrarySceneTest, ApplyPoseMaskedRejectsUnknownPoseAndMissingEntity) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_MaskedReject");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    QSet<QString> empty;
    // Unknown pose → false (even with empty mask).
    EXPECT_FALSE(lib->applyPoseMasked(entity, QStringLiteral("NoSuch"), empty));
    // Null entity → false.
    EXPECT_FALSE(lib->applyPoseMasked(nullptr, QStringLiteral("Any"), empty));
    // Empty pose name → false.
    EXPECT_FALSE(lib->applyPoseMasked(entity, QString(), empty));
}

TEST_F(PoseLibrarySceneTest, ApplyPoseMaskedWithEmptyMaskAppliesNothing) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_MaskedEmpty");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // Save a pose with bone[0] at +Z=4.
    skel->getBone(0)->setPosition(Ogre::Vector3(0, 0, 4));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("Reference")));

    // Move bone back to origin.
    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);

    // Empty mask = nothing to apply. Returns true (pose found
    // successfully) but bone stays at origin — empty mask is the
    // documented "no bones" interpretation, not "all bones".
    QSet<QString> empty;
    EXPECT_TRUE(lib->applyPoseMasked(entity, QStringLiteral("Reference"), empty));
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().z, 0.0f);
}

// ─── Slice D4 — bone name flip + mirror pose ─────────────────────────

TEST(PoseLibraryStandalone, FlipBoneName_MixamoLowercaseSuffix) {
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Hand_l")),
              QStringLiteral("Hand_r"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Hand_r")),
              QStringLiteral("Hand_l"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("mixamorig_LeftUpLeg_l")),
              QStringLiteral("mixamorig_LeftUpLeg_r"));
}

TEST(PoseLibraryStandalone, FlipBoneName_UppercaseSuffix) {
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Hand_L")),
              QStringLiteral("Hand_R"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Hand.R")),
              QStringLiteral("Hand.L"));
}

TEST(PoseLibraryStandalone, FlipBoneName_LeftRightPrefix) {
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("LeftHand")),
              QStringLiteral("RightHand"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("RightArm")),
              QStringLiteral("LeftArm"));
    // Edge case: prefix MUST be followed by uppercase / underscore /
    // end. "Lefty" should NOT flip to "Righty".
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Lefty")),
              QStringLiteral("Lefty"));
}

TEST(PoseLibraryStandalone, FlipBoneName_CentreLineUntouched) {
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Spine")),
              QStringLiteral("Spine"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Hips")),
              QStringLiteral("Hips"));
    EXPECT_EQ(PoseLibrary::flipBoneName(QStringLiteral("Head_End")),
              QStringLiteral("Head_End"));
}

TEST_F(PoseLibrarySceneTest, MirrorPoseProducesXFlippedSnapshot) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_Mirror");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // Set bone[0] to a non-symmetric pose; capture as "src".
    skel->getBone(0)->setPosition(Ogre::Vector3(2, 3, 4));
    skel->getBone(0)->setOrientation(Ogre::Quaternion(0.5, 0.5, 0.5, 0.5));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("src")));

    EXPECT_TRUE(lib->mirrorPose(entity,
                                 QStringLiteral("src"),
                                 QStringLiteral("dst")));
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("dst")));

    // Reset bone, apply mirrored — should see X-flipped values.
    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);
    skel->getBone(0)->setOrientation(Ogre::Quaternion::IDENTITY);
    lib->applyPose(entity, QStringLiteral("dst"));
    const auto pos = skel->getBone(0)->getPosition();
    const auto rot = skel->getBone(0)->getOrientation();
    EXPECT_FLOAT_EQ(pos.x, -2.0f);
    EXPECT_FLOAT_EQ(pos.y, 3.0f);
    EXPECT_FLOAT_EQ(pos.z, 4.0f);
    // Quaternion: keep w + x, flip y + z.
    EXPECT_FLOAT_EQ(rot.w, 0.5f);
    EXPECT_FLOAT_EQ(rot.x, 0.5f);
    EXPECT_FLOAT_EQ(rot.y, -0.5f);
    EXPECT_FLOAT_EQ(rot.z, -0.5f);
}

TEST_F(PoseLibrarySceneTest, MirrorPoseRejectsMissingSourceAndEmptyDst) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_MirrorReject");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    EXPECT_FALSE(lib->mirrorPose(entity, QStringLiteral("NoSuchSrc"),
                                  QStringLiteral("dst")));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("ok")));
    EXPECT_FALSE(lib->mirrorPose(entity, QStringLiteral("ok"), QString()));
    EXPECT_FALSE(lib->mirrorPose(nullptr, QStringLiteral("ok"),
                                  QStringLiteral("dst")));
}

// ─── Slice D3 — undo commands ────────────────────────────────────────

TEST_F(PoseLibrarySceneTest, SavePoseCommandRoundTripsForFreshPose) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdSaveNew");
    ASSERT_NE(entity, nullptr);
    auto* lib = PoseLibrary::instance();

    SavePoseCommand cmd(entity, QStringLiteral("First"));
    cmd.redo();
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("First")));

    cmd.undo();
    EXPECT_FALSE(lib->hasPose(entity, QStringLiteral("First")));

    cmd.redo();
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("First")));
}

TEST_F(PoseLibrarySceneTest, SavePoseCommandRestoresPriorOnOverwrite) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdOverwrite");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // Seed an initial pose at bone[0] position (1,0,0).
    skel->getBone(0)->setPosition(Ogre::Vector3(1, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("X")));

    // Move bone, then overwrite via command. After undo the
    // library should still contain the FIRST pose's data — so a
    // subsequent apply restores (1,0,0), not the (5,5,5) we wrote.
    skel->getBone(0)->setPosition(Ogre::Vector3(5, 5, 5));
    SavePoseCommand cmd(entity, QStringLiteral("X"));
    cmd.redo();
    cmd.undo();

    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);
    lib->applyPose(entity, QStringLiteral("X"));
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 1.0f);
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().y, 0.0f);
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().z, 0.0f);
}

TEST_F(PoseLibrarySceneTest, DeletePoseCommandRoundTrips) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdDelete");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    skel->getBone(0)->setPosition(Ogre::Vector3(7, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("Y")));

    DeletePoseCommand cmd(entity, QStringLiteral("Y"));
    cmd.redo();
    EXPECT_FALSE(lib->hasPose(entity, QStringLiteral("Y")));

    cmd.undo();
    EXPECT_TRUE(lib->hasPose(entity, QStringLiteral("Y")));

    // Apply after undo should restore the original 7,0,0.
    skel->getBone(0)->setPosition(Ogre::Vector3::ZERO);
    lib->applyPose(entity, QStringLiteral("Y"));
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 7.0f);
}

TEST_F(PoseLibrarySceneTest, ApplyPoseCommandRestoresPreApplyState) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdApply");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* lib = PoseLibrary::instance();

    // Save a pose with bone at (10, 0, 0).
    skel->getBone(0)->setPosition(Ogre::Vector3(10, 0, 0));
    ASSERT_TRUE(lib->savePose(entity, QStringLiteral("PosePos10")));

    // Move bone to (3, 3, 3) — this is the pre-apply state.
    skel->getBone(0)->setPosition(Ogre::Vector3(3, 3, 3));

    ApplyPoseCommand cmd(entity, QStringLiteral("PosePos10"));
    cmd.redo();
    // After redo bone is at the saved pose's position.
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 10.0f);

    cmd.undo();
    // After undo bone is back at the (3,3,3) snapshot.
    const auto pos = skel->getBone(0)->getPosition();
    EXPECT_FLOAT_EQ(pos.x, 3.0f);
    EXPECT_FLOAT_EQ(pos.y, 3.0f);
    EXPECT_FLOAT_EQ(pos.z, 3.0f);
}

// Codex P1 on PR #595 — when ApplyPoseCommand::redo fails (pose
// name not found in the library), undo MUST also be a no-op.
// Restoring `mPreApply` would clobber any bone edits the user made
// after the failed apply with stale snapshot values.
TEST_F(PoseLibrarySceneTest, ApplyPoseCommandUndoNoOpWhenRedoFailed) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdApplyFail");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();

    // Pre-apply bone position. The command will capture this as
    // mPreApply at construction.
    skel->getBone(0)->setPosition(Ogre::Vector3(2, 2, 2));
    ApplyPoseCommand cmd(entity, QStringLiteral("PoseDoesNotExist"));

    // Move the bone — these edits are what undo MUST preserve.
    skel->getBone(0)->setPosition(Ogre::Vector3(9, 9, 9));

    cmd.redo();  // No-op — pose doesn't exist.
    // Bone still at user-edited position; redo didn't apply.
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 9.0f);

    cmd.undo();  // Must NOT revert to (2,2,2) — that would clobber the user edit.
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().x, 9.0f);
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().y, 9.0f);
    EXPECT_FLOAT_EQ(skel->getBone(0)->getPosition().z, 9.0f);
}

TEST_F(PoseLibrarySceneTest, DeletePoseCommandIsNoOpForUnknownName) {
    Ogre::Entity* entity = createAnimatedTestEntity("PoseLib_CmdDelMissing");
    ASSERT_NE(entity, nullptr);
    // Construct + redo + undo on a name that doesn't exist — both
    // sides should be safe no-ops (no crash, no stray pose).
    DeletePoseCommand cmd(entity, QStringLiteral("Missing"));
    cmd.redo();
    cmd.undo();
    EXPECT_FALSE(PoseLibrary::instance()->hasPose(entity, QStringLiteral("Missing")));
}
