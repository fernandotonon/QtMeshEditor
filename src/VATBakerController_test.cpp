#include <gtest/gtest.h>

#include <QSignalSpy>
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
        QStringLiteral("rgba8"), QStringLiteral("agnostic"),
        false, QStringLiteral("/tmp/vat-ctrl-test"));
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
    EXPECT_FALSE(ctrl->bake(
        QString(), 30.0, QStringLiteral("rgba8"),
        QStringLiteral("agnostic"), false, QStringLiteral("/tmp")));
    ASSERT_GE(spy.count(), 1);
    EXPECT_FALSE(spy.first().at(0).toBool());
    EXPECT_TRUE(spy.first().at(2).toString().contains("animationName"));
}

TEST(VATBakerControllerStandalone, BakeRefusesEmptyOutputDir) {
    clearSelection();
    auto* ctrl = VATBakerController::instance();
    QSignalSpy spy(ctrl, &VATBakerController::bakeFinished);
    EXPECT_FALSE(ctrl->bake(
        QStringLiteral("Idle"), 30.0, QStringLiteral("rgba8"),
        QStringLiteral("agnostic"), false, QString()));
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
        QStringLiteral("rgba8"), QStringLiteral("agnostic"),
        false, tmp.path(), QStringLiteral("CT"));
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
        QStringLiteral("NoSuchAnim"), 10.0,
        QStringLiteral("rgba8"), QStringLiteral("agnostic"),
        false, tmp.path());
    EXPECT_TRUE(kicked);  // arg validation passed, bake itself failed
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_FALSE(finishedSpy.first().at(0).toBool());
    EXPECT_TRUE(finishedSpy.first().at(2).toString().contains("not found"));
}

TEST_F(VATBakerControllerSceneTest, EncodingAndTargetStringsRoutedThrough) {
    auto* entity = createAnimatedTestEntity("VAT_Ctrl_Routing");
    ASSERT_NE(entity, nullptr);
    SelectionSet::getSingleton()->append(entity);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto* ctrl = VATBakerController::instance();
    QSignalSpy finishedSpy(ctrl, &VATBakerController::bakeFinished);

    // rgba16 + godot — exercises both string→enum mappings.
    EXPECT_TRUE(ctrl->bake(
        QStringLiteral("TestAnim"), 10.0,
        QStringLiteral("rgba16"), QStringLiteral("godot"),
        false, tmp.path(), QStringLiteral("R")));
    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_TRUE(finishedSpy.first().at(0).toBool());
    const QString posTex = finishedSpy.first().at(1).toString();
    EXPECT_TRUE(posTex.endsWith("_pos.png"));
    // godot target writes a .gdshader alongside the png.
    const QString gd = posTex.left(posTex.size() - 8) + ".gdshader";
    EXPECT_TRUE(QFile::exists(gd))
        << "godot shader should be at " << gd.toStdString();
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
