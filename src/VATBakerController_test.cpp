#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "VATBakerController.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

namespace {

void clearSelection()
{
    if (auto* sel = SelectionSet::getSingleton()) sel->clear();
}

}  // namespace

// ===========================================================================
// Standalone — no Ogre setup needed for these.
// ===========================================================================

TEST(VATBakerControllerStandalone, InstanceIsSingleton) {
    auto* a = VATBakerController::instance();
    auto* b = VATBakerController::instance();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, nullptr);
}

TEST(VATBakerControllerStandalone, BakeRefusedWhenNothingSelected) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    const bool kicked = ctrl->bake(
        QStringLiteral("Idle"), 30.0,
        QStringLiteral("/tmp/vat-ctrl-test"));
    EXPECT_FALSE(kicked);
    ASSERT_GE(spy.count(), 1);
    // First emission carries the failure.
    const auto args = spy.first();
    EXPECT_FALSE(args.at(0).toBool());
    EXPECT_FALSE(args.at(2).toString().isEmpty());
}

TEST(VATBakerControllerStandalone, BakeRefusesEmptyAnimName) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(QString(), 30.0, QStringLiteral("/tmp")));
    ASSERT_GE(spy.count(), 1);
    EXPECT_FALSE(spy.first().at(0).toBool());
    EXPECT_TRUE(spy.first().at(2).toString().contains("animationName"));
}

TEST(VATBakerControllerStandalone, BakeRefusesEmptyOutputDir) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(QStringLiteral("Idle"), 30.0, QString()));
    ASSERT_GE(spy.count(), 1);
    EXPECT_FALSE(spy.first().at(0).toBool());
    EXPECT_TRUE(spy.first().at(2).toString().contains("outputDir"));
}

TEST(VATBakerControllerStandalone, AvailableAnimationsEmptyWithoutSelection) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    ctrl->refreshAnimations();
    EXPECT_TRUE(ctrl->availableAnimations().isEmpty());
}

// ===========================================================================
// Scene fixture — bake against an in-memory animated entity.
// ===========================================================================

class VATBakerControllerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
        clearSelection();
    }
    void TearDown() override
    {
        clearSelection();
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }
};

TEST_F(VATBakerControllerSceneTest, RefreshAnimationsFindsSelectionAnim) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_Refresh");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);

    auto* ctrl = VATBakerController::instance();
    ctrl->refreshAnimations();
    EXPECT_FALSE(ctrl->availableAnimations().isEmpty());
    EXPECT_TRUE(ctrl->availableAnimations().contains(QStringLiteral("TestAnim")));
}

TEST_F(VATBakerControllerSceneTest, BakeKicksOffAndEmitsFinished) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_Run");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto* ctrl = VATBakerController::instance();
    QSignalSpy finishedSpy(ctrl, &VATBakerController::bakeFinished);
    QSignalSpy isBakingSpy(ctrl, &VATBakerController::isBakingChanged);

    const bool kicked = ctrl->bake(
        QStringLiteral("TestAnim"), 10.0,
        tmp.path(), QStringLiteral("CT"));
    EXPECT_TRUE(kicked);
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(0).toBool())
        << "error was: " << finishedSpy.first().at(2).toString().toStdString();
    EXPECT_FALSE(finishedSpy.first().at(1).toString().isEmpty());
    EXPECT_FALSE(ctrl->isBaking())
        << "isBaking must be false again after bake finishes";
    // isBakingChanged fires at least twice — entering and leaving.
    EXPECT_GE(isBakingSpy.count(), 2);
}

TEST_F(VATBakerControllerSceneTest, BakeReportsErrorForMissingAnim) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_MissAnim");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);

    auto* ctrl = VATBakerController::instance();
    QSignalSpy finishedSpy(ctrl, &VATBakerController::bakeFinished);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const bool kicked = ctrl->bake(
        QStringLiteral("NoSuchAnim"), 10.0, tmp.path());
    EXPECT_TRUE(kicked);  // arg validation passed, bake itself failed
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.first().at(0).toBool());
    EXPECT_TRUE(finishedSpy.first().at(2).toString().contains("not found"));
}

TEST_F(VATBakerControllerSceneTest, AvailableAnimationsRefreshesOnSelectionChange) {
    auto* ctrl = VATBakerController::instance();
    QSignalSpy spy(ctrl, &VATBakerController::availableAnimationsChanged);
    ctrl->refreshAnimations();  // no selection
    const int beforeAttach = spy.count();

    auto* entity = createAnimatedTestEntity("VAT_Ctrl_SelChange");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);
    // refreshAnimations is wired to selectionChanged, but the test
    // helper appends directly — call refresh manually here.
    ctrl->refreshAnimations();
    EXPECT_GT(spy.count(), beforeAttach);
    EXPECT_FALSE(ctrl->availableAnimations().isEmpty());
}

// ===========================================================================
// #522 — mode family surface on the controller.
// ===========================================================================

TEST(VATBakerControllerStandalone, ModesEmptyWithoutSelection) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    ctrl->refreshAnimations();
    EXPECT_TRUE(ctrl->availableModes().isEmpty());
    EXPECT_TRUE(ctrl->animationsForMode(QStringLiteral("skeletal")).isEmpty());
    EXPECT_TRUE(ctrl->animationsForMode(QStringLiteral("bogus")).isEmpty());
    EXPECT_EQ(ctrl->encodingIds().size(), 3);
    EXPECT_EQ(ctrl->targetIds().size(), 4);
    EXPECT_EQ(ctrl->modeLabel(QStringLiteral("rigid")), QStringLiteral("Rigid body (per-chunk)"));
    EXPECT_EQ(ctrl->modeLabel(QStringLiteral("bogus")), QStringLiteral("bogus"));
}

TEST(VATBakerControllerStandalone, BakeRefusesUnknownModeEncodingTarget) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    // These refusals come AFTER the selection check, so an entity is
    // needed for them to be reached; without a scene the "no entity"
    // refusal fires first — pinned in the scene fixture below. Here we
    // only assert the call is refused and emits exactly one finish.
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(QStringLiteral("Idle"), 30.0, QStringLiteral("/tmp/x"),
                            QString(), QStringList(), QStringLiteral("bogus")));
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(VATBakerControllerSceneTest, ModesReflectSelectionAndBakeRefusesUnknownIds) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_Modes");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);
    auto* ctrl = VATBakerController::instance();
    ctrl->refreshAnimations();
    // Skeletal test triangle: skeletal yes; rigid no (shared vertex
    // data); no vertex clips → no mesh-anim / morph.
    EXPECT_TRUE(ctrl->availableModes().contains(QStringLiteral("skeletal")));
    EXPECT_FALSE(ctrl->availableModes().contains(QStringLiteral("rigid")));
    EXPECT_FALSE(ctrl->availableModes().contains(QStringLiteral("morph")));
    EXPECT_EQ(ctrl->animationsForMode(QStringLiteral("skeletal")),
              QStringList{QStringLiteral("TestAnim")});
    EXPECT_TRUE(ctrl->animationsForMode(QStringLiteral("mesh-anim")).isEmpty());

    QTemporaryDir tmp;
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(QStringLiteral("TestAnim"), 30.0, tmp.path(), QString(),
                            QStringList(), QStringLiteral("bogus")));
    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.last().at(2).toString().contains(QStringLiteral("mode")));
    EXPECT_FALSE(ctrl->bake(QStringLiteral("TestAnim"), 30.0, tmp.path(), QString(),
                            QStringList(), QStringLiteral("skeletal"), QStringLiteral("jpeg")));
    ASSERT_EQ(spy.count(), 2);
    EXPECT_TRUE(spy.last().at(2).toString().contains(QStringLiteral("encoding")));
    EXPECT_FALSE(ctrl->bake(QStringLiteral("TestAnim"), 30.0, tmp.path(), QString(),
                            QStringList(), QStringLiteral("skeletal"), QStringLiteral("rgba16"),
                            QStringLiteral("blender")));
    ASSERT_EQ(spy.count(), 3);
    EXPECT_TRUE(spy.last().at(2).toString().contains(QStringLiteral("target")));

    // A valid explicit skeletal + rgba8 + godot bake succeeds and ships
    // the Godot template because of the target.
    EXPECT_TRUE(ctrl->bake(QStringLiteral("TestAnim"), 30.0, tmp.path(), QString(),
                           QStringList(), QStringLiteral("skeletal"), QStringLiteral("rgba8"),
                           QStringLiteral("godot")));
    ASSERT_EQ(spy.count(), 4);
    EXPECT_TRUE(spy.last().at(0).toBool()) << spy.last().at(2).toString().toStdString();
    EXPECT_TRUE(QFile::exists(QDir(tmp.path()).filePath(QStringLiteral("openvat.gdshader"))));
}

// #1166: the bone texture mode has its own label, encodings and clip list.
TEST(VATBakerControllerStandalone, BoneTextureModeListsFloatEncodingsOnly) {
    auto* ctrl = VATBakerController::instance();
    EXPECT_EQ(ctrl->encodingsForMode(QStringLiteral("bone-texture")),
              (QStringList{ QStringLiteral("rgba32f"), QStringLiteral("rgba16f") }));
    EXPECT_EQ(ctrl->encodingLabelsForMode(QStringLiteral("bone-texture")).size(), 2);
    EXPECT_EQ(ctrl->encodingsForMode(QStringLiteral("skeletal")), VATBaker::encodingIds());
    EXPECT_TRUE(ctrl->modeLabel(QStringLiteral("bone-texture")).contains(QStringLiteral("Bone texture")));
}

TEST_F(VATBakerControllerSceneTest, BoneTextureModeOffersAllClipsAndBakes) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_Bones");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);
    auto* ctrl = VATBakerController::instance();
    ctrl->refreshAnimations();
    EXPECT_TRUE(ctrl->availableModes().contains(QStringLiteral("bone-texture")));
    const QStringList clips = ctrl->animationsForMode(QStringLiteral("bone-texture"));
    ASSERT_GE(clips.size(), 2);
    EXPECT_EQ(clips.first(), VATBakerController::allClipsLabel());
    EXPECT_TRUE(clips.contains(QStringLiteral("TestAnim")));

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QSignalSpy finishedSpy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_TRUE(ctrl->bake(VATBakerController::allClipsLabel(), 10.0, tmp.path(),
                           QStringLiteral("Bones"), {}, QStringLiteral("bone-texture"),
                           QStringLiteral("rgba32f"), QStringLiteral("agnostic")));
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(0).toBool())
        << finishedSpy.first().at(2).toString().toStdString();
    EXPECT_TRUE(finishedSpy.first().at(1).toString().endsWith(QStringLiteral("Bones_bones.exr")));

    // A PNG encoding is refused for this mode.
    finishedSpy.clear();
    EXPECT_FALSE(ctrl->bake(QStringLiteral("TestAnim"), 10.0, tmp.path(), QString(), {},
                            QStringLiteral("bone-texture"), QStringLiteral("rgba16")));
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.first().at(0).toBool());
}

TEST_F(VATBakerControllerSceneTest, BoneTextureRefusesUnknownTarget) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_BonesTarget");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    auto* ctrl = VATBakerController::instance();
    QSignalSpy finishedSpy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(QStringLiteral("TestAnim"), 10.0, tmp.path(), QString(), {},
                            QStringLiteral("bone-texture"), QStringLiteral("rgba32f"),
                            QStringLiteral("gdoot")));
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.first().at(0).toBool());
    EXPECT_TRUE(QDir(tmp.path()).entryList(QDir::Files).isEmpty());
}
