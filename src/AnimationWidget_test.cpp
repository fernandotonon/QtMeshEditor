#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QSignalSpy>
#include <QTableWidget>
#include <QPushButton>
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "AnimationWidget.h"
#include "TestHelpers.h"

// ---------------------------------------------------------------------------
// Base test fixture: Manager + Ogre initialized, no mesh loaded
// ---------------------------------------------------------------------------
class AnimationWidgetTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
        // Start with a clean selection
        SelectionSet::getSingleton()->clear();
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

// ---------------------------------------------------------------------------
// Extended fixture: loads a mesh that has a skeleton and animations
// ---------------------------------------------------------------------------
class AnimationWidgetWithMeshTest : public AnimationWidgetTest {
protected:
    Ogre::Entity* entity = nullptr;

    void SetUp() override {
        AnimationWidgetTest::SetUp();
        if (::testing::Test::IsSkipped()) return;

        // Loading .mesh files can crash in headless/offscreen mode (no GPU context)
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
        }

        // Import a mesh with skeleton/animations (robot.mesh ships in media/)
        QStringList uris{"./media/models/robot.mesh"};
        try {
            MeshImporterExporter::importer(uris);
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Skipping: failed to import robot.mesh (" << e.what() << ")";
        } catch (...) {
            GTEST_SKIP() << "Skipping: failed to import robot.mesh (unknown error)";
        }

        if (Manager::getSingleton()->getEntities().isEmpty()) {
            GTEST_SKIP() << "Skipping: no entity available after import";
        }

        entity = Manager::getSingleton()->getEntities().last();
        ASSERT_NE(entity, nullptr);

        // Select the entity so AnimationWidget can see it
        SelectionSet::getSingleton()->selectOne(entity);
        if (app) app->processEvents();
    }
};

// ============================== Basic Tests ================================

TEST_F(AnimationWidgetTest, ConstructAndDestroy)
{
    // Simply constructing and destroying the widget should not crash
    AnimationWidget widget;
    SUCCEED();
}

TEST_F(AnimationWidgetTest, UIElementsExist)
{
    AnimationWidget widget;

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    EXPECT_EQ(animTable->columnCount(), 4);

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    EXPECT_EQ(skeletonTable->columnCount(), 3);

    QPushButton* playPauseButton = widget.findChild<QPushButton*>("PlayPauseButton");
    ASSERT_NE(playPauseButton, nullptr);
    EXPECT_TRUE(playPauseButton->isCheckable());
}

TEST_F(AnimationWidgetTest, EmptySelectionShowsNoRows)
{
    // With no entities selected, both tables should be empty
    AnimationWidget widget;
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    EXPECT_EQ(animTable->rowCount(), 0);

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    EXPECT_EQ(skeletonTable->rowCount(), 0);
}

TEST_F(AnimationWidgetTest, IsSkeletonShownReturnsFalseWhenNoEntity)
{
    AnimationWidget widget;
    // Passing nullptr should not crash and should return false
    EXPECT_FALSE(widget.isSkeletonShown(nullptr));
}

TEST_F(AnimationWidgetTest, ChangeAnimationStateSignal)
{
    AnimationWidget widget;
    QSignalSpy spy(&widget, &AnimationWidget::changeAnimationState);
    ASSERT_TRUE(spy.isValid());

    QPushButton* playPauseButton = widget.findChild<QPushButton*>("PlayPauseButton");
    ASSERT_NE(playPauseButton, nullptr);

    // Toggle the play/pause button to "playing" state
    playPauseButton->setChecked(true);
    if (app) app->processEvents();

    // The signal should have been emitted at least once with true
    ASSERT_GE(spy.count(), 1);
    QList<QVariant> args = spy.last();
    EXPECT_EQ(args.at(0).toBool(), true);

    // Toggle back to paused
    playPauseButton->setChecked(false);
    if (app) app->processEvents();

    // Should have emitted with false
    args = spy.last();
    EXPECT_EQ(args.at(0).toBool(), false);
}

// ===================== Tests with a loaded skeleton mesh ===================

TEST_F(AnimationWidgetWithMeshTest, AnimationTablePopulatedAfterSelection)
{
    AnimationWidget widget;
    // Re-select entity so the widget (which connects in its constructor) sees the signal
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);

    // robot.mesh has animations so the table should have at least one row
    const Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
    if (!set || set->getAnimationStates().empty()) {
        GTEST_SKIP() << "Skipping: entity has no animation states";
    }

    EXPECT_GT(animTable->rowCount(), 0);
}

TEST_F(AnimationWidgetWithMeshTest, AnimationTableHasCorrectColumns)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);

    if (animTable->rowCount() == 0) {
        GTEST_SKIP() << "Skipping: no animation rows";
    }

    // Column 0: Entity name, Column 1: Animation name,
    // Column 2: Enabled checkbox, Column 3: Loop checkbox
    auto* entityItem = animTable->item(0, 0);
    ASSERT_NE(entityItem, nullptr);
    EXPECT_FALSE(entityItem->text().isEmpty());

    auto* animItem = animTable->item(0, 1);
    ASSERT_NE(animItem, nullptr);
    EXPECT_FALSE(animItem->text().isEmpty());

    auto* enabledItem = animTable->item(0, 2);
    ASSERT_NE(enabledItem, nullptr);
    // Should be a checkbox-style item
    EXPECT_TRUE(enabledItem->checkState() == Qt::Checked ||
                enabledItem->checkState() == Qt::Unchecked);

    auto* loopItem = animTable->item(0, 3);
    ASSERT_NE(loopItem, nullptr);
    EXPECT_TRUE(loopItem->checkState() == Qt::Checked ||
                loopItem->checkState() == Qt::Unchecked);
}

TEST_F(AnimationWidgetWithMeshTest, SkeletonTablePopulatedAfterSelection)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);

    // We selected one entity so the skeleton table should have exactly one row
    EXPECT_EQ(skeletonTable->rowCount(), 1);
}

TEST_F(AnimationWidgetWithMeshTest, SkeletonTableEntityName)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);

    if (skeletonTable->rowCount() == 0) {
        GTEST_SKIP() << "Skipping: skeleton table empty";
    }

    auto* entityItem = skeletonTable->item(0, 0);
    ASSERT_NE(entityItem, nullptr);

    // Entity name in the table should match the Ogre entity name
    EXPECT_EQ(entityItem->text().toStdString(), entity->getName());
}

TEST_F(AnimationWidgetWithMeshTest, SkeletonTableCheckboxInitiallyUnchecked)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);

    if (skeletonTable->rowCount() == 0) {
        GTEST_SKIP() << "Skipping: skeleton table empty";
    }

    // Display Skeleton checkbox should default to unchecked
    auto* showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    EXPECT_EQ(showSkeletonItem->checkState(), Qt::Unchecked);
}

TEST_F(AnimationWidgetWithMeshTest, IsSkeletonShownReturnsFalseByDefault)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    // By default no skeleton debug display is active
    EXPECT_FALSE(widget.isSkeletonShown(entity));
}

TEST_F(AnimationWidgetWithMeshTest, TablesUpdateOnSelectionChange)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_NE(skeletonTable, nullptr);

    int animRowsBefore = animTable->rowCount();
    int skelRowsBefore = skeletonTable->rowCount();

    // Clear selection -- should empty the tables
    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();

    EXPECT_EQ(animTable->rowCount(), 0);
    EXPECT_EQ(skeletonTable->rowCount(), 0);

    // Re-select the entity -- tables should repopulate
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    EXPECT_EQ(animTable->rowCount(), animRowsBefore);
    EXPECT_EQ(skeletonTable->rowCount(), skelRowsBefore);
}

TEST_F(AnimationWidgetWithMeshTest, AnimationCountMatchesEntityAnimations)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);

    const Ogre::AnimationStateSet* set = entity->getAllAnimationStates();
    if (!set) {
        EXPECT_EQ(animTable->rowCount(), 0);
        return;
    }

    int expectedCount = static_cast<int>(set->getAnimationStates().size());
    EXPECT_EQ(animTable->rowCount(), expectedCount);
}

TEST_F(AnimationWidgetWithMeshTest, PlayPauseToggleEmitsSignal)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QSignalSpy spy(&widget, &AnimationWidget::changeAnimationState);
    ASSERT_TRUE(spy.isValid());

    QPushButton* playPauseButton = widget.findChild<QPushButton*>("PlayPauseButton");
    ASSERT_NE(playPauseButton, nullptr);

    // Simulate play
    playPauseButton->setChecked(true);
    if (app) app->processEvents();

    ASSERT_GE(spy.count(), 1);
    EXPECT_TRUE(spy.last().at(0).toBool());

    // Simulate pause
    playPauseButton->setChecked(false);
    if (app) app->processEvents();

    EXPECT_FALSE(spy.last().at(0).toBool());
}

TEST_F(AnimationWidgetWithMeshTest, AnimationItemsAreNonEditable)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);

    if (animTable->rowCount() == 0) {
        GTEST_SKIP() << "Skipping: no animation rows";
    }

    // All four columns should have the non-editable flag
    for (int col = 0; col < 4; ++col) {
        auto* item = animTable->item(0, col);
        ASSERT_NE(item, nullptr) << "Column " << col << " is null";
        EXPECT_FALSE(item->flags() & Qt::ItemIsEditable)
            << "Column " << col << " should not be editable";
    }
}

TEST_F(AnimationWidgetWithMeshTest, SkeletonItemsAreNonEditable)
{
    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);

    if (skeletonTable->rowCount() == 0) {
        GTEST_SKIP() << "Skipping: no skeleton rows";
    }

    for (int col = 0; col < 2; ++col) {
        auto* item = skeletonTable->item(0, col);
        ASSERT_NE(item, nullptr) << "Column " << col << " is null";
        EXPECT_FALSE(item->flags() & Qt::ItemIsEditable)
            << "Column " << col << " should not be editable";
    }
}

TEST_F(AnimationWidgetWithMeshTest, DestroyWidgetWithSkeletonCleanup)
{
    // Create the widget and immediately destroy it -- should not crash,
    // exercising the destructor path that calls disableAllSkeletonDebug().
    {
        AnimationWidget widget;
        SelectionSet::getSingleton()->selectOne(entity);
        if (app) app->processEvents();
    }
    // If we reach here without a crash, the cleanup logic is working
    SUCCEED();
}

TEST_F(AnimationWidgetWithMeshTest, MultipleWidgetInstancesShareSelection)
{
    // Two AnimationWidget instances observing the same SelectionSet should
    // both populate their tables identically.
    AnimationWidget widget1;
    AnimationWidget widget2;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable1 = widget1.findChild<QTableWidget*>("animTable");
    QTableWidget* animTable2 = widget2.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable1, nullptr);
    ASSERT_NE(animTable2, nullptr);

    EXPECT_EQ(animTable1->rowCount(), animTable2->rowCount());

    QTableWidget* skelTable1 = widget1.findChild<QTableWidget*>("skeletonTable");
    QTableWidget* skelTable2 = widget2.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skelTable1, nullptr);
    ASSERT_NE(skelTable2, nullptr);

    EXPECT_EQ(skelTable1->rowCount(), skelTable2->rowCount());
}

// ==================== Additional Tests ======================================

// ---------------------------------------------------------------------------
// Tests using AnimationWidgetTest (no mesh loaded)
// ---------------------------------------------------------------------------

TEST_F(AnimationWidgetTest, UpdateAnimationTableWithNoEntitiesSelected)
{
    // With nothing selected, updateAnimationTable should result in 0 rows.
    // This is exercised indirectly via the constructor + processEvents.
    AnimationWidget widget;
    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    EXPECT_EQ(animTable->rowCount(), 0);
}

TEST_F(AnimationWidgetTest, UpdateSkeletonTableWithNoEntitiesSelected)
{
    // Similarly, skeleton table should be empty with no selection.
    AnimationWidget widget;
    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    EXPECT_EQ(skeletonTable->rowCount(), 0);
}

TEST_F(AnimationWidgetTest, ToggleSkeletonDebugNullEntityReturnsFalse)
{
    AnimationWidget widget;
    // Passing nullptr should return false and not crash
    EXPECT_FALSE(widget.toggleSkeletonDebug(nullptr, true));
    EXPECT_FALSE(widget.toggleSkeletonDebug(nullptr, false));
}

TEST_F(AnimationWidgetTest, ToggleBoneWeightsNullEntityReturnsFalse)
{
    AnimationWidget widget;
    EXPECT_FALSE(widget.toggleBoneWeights(nullptr, true));
    EXPECT_FALSE(widget.toggleBoneWeights(nullptr, false));
}

TEST_F(AnimationWidgetTest, IsSkeletonDebugActiveReturnsFalseForNull)
{
    AnimationWidget widget;
    EXPECT_FALSE(widget.isSkeletonDebugActive(nullptr));
}

TEST_F(AnimationWidgetTest, IsBoneWeightsShownReturnsFalseForNull)
{
    AnimationWidget widget;
    EXPECT_FALSE(widget.isBoneWeightsShown(nullptr));
}

TEST_F(AnimationWidgetTest, GetSkeletonDebugReturnsNullForNull)
{
    AnimationWidget widget;
    EXPECT_EQ(widget.getSkeletonDebug(nullptr), nullptr);
}

TEST_F(AnimationWidgetTest, GetBoneWeightOverlayReturnsNullForNull)
{
    AnimationWidget widget;
    EXPECT_EQ(widget.getBoneWeightOverlay(nullptr), nullptr);
}

TEST_F(AnimationWidgetTest, DestroyWidgetWithNoSelectionDoesNotCrash)
{
    // Verify that destroying a widget when no entities are selected
    // (thus disableAllSkeletonDebug has nothing to clean up) does not crash.
    {
        AnimationWidget widget;
        if (app) app->processEvents();
    }
    SUCCEED();
}

TEST_F(AnimationWidgetTest, PollAnimationStateWithEmptyTableDoesNotCrash)
{
    // pollAnimationState() is called on a 200ms timer. With an empty table,
    // it should just silently do nothing.
    AnimationWidget widget;
    if (app) app->processEvents();

    // Manually invoke pollAnimationState via the timer mechanism.
    // Process events multiple times to trigger the poll timer.
    QThread::msleep(250);
    if (app) app->processEvents();
    SUCCEED();
}

TEST_F(AnimationWidgetTest, ChangeAnimationNameSignalExists)
{
    // Verify the changeAnimationName signal can be spied on
    AnimationWidget widget;
    QSignalSpy spy(&widget, &AnimationWidget::changeAnimationName);
    ASSERT_TRUE(spy.isValid());
    // Signal should not have been emitted yet
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(AnimationWidgetTest, MultipleWidgetDestructionOrder)
{
    // Create multiple widgets and destroy them in reverse order.
    // This tests that the SelectionSet signal disconnections are safe.
    auto* widget1 = new AnimationWidget;
    auto* widget2 = new AnimationWidget;
    auto* widget3 = new AnimationWidget;
    if (app) app->processEvents();

    delete widget3;
    if (app) app->processEvents();
    delete widget2;
    if (app) app->processEvents();
    delete widget1;
    if (app) app->processEvents();

    SUCCEED();
}

// ---------------------------------------------------------------------------
// Tests using in-memory entities (require canLoadMeshFiles but not robot.mesh)
// ---------------------------------------------------------------------------

TEST_F(AnimationWidgetTest, TriangleMeshEntityShowsNoAnimations)
{
    // A simple triangle mesh with no skeleton should show 0 animation rows
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("animwidget_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("animwidget_tri_node");
    auto* entity = sceneMgr->createEntity("animwidget_tri_ent", mesh);
    node->attachObject(entity);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    EXPECT_EQ(animTable->rowCount(), 0);

    // Skeleton table should have 1 row (the entity) but no skeleton debug available
    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    EXPECT_EQ(skeletonTable->rowCount(), 1);

    // toggleSkeletonDebug should return false for an entity without a skeleton
    EXPECT_FALSE(widget.toggleSkeletonDebug(entity, true));
    EXPECT_FALSE(widget.toggleBoneWeights(entity, true));
}

TEST_F(AnimationWidgetTest, SkeletonMeshEntityWithoutAnimations)
{
    // A mesh with a skeleton but no animations should show in skeleton table
    // but have 0 animation rows.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("animwidget_skel_noanim");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("animwidget_skel_noanim_node");
    auto* entity = sceneMgr->createEntity("animwidget_skel_noanim_ent", mesh);
    node->attachObject(entity);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    // No animations defined on the skeleton, so 0 rows
    EXPECT_EQ(animTable->rowCount(), 0);

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    EXPECT_EQ(skeletonTable->rowCount(), 1);

    // Entity has a skeleton, so skeleton debug should work
    EXPECT_FALSE(widget.isSkeletonShown(entity));
    EXPECT_FALSE(widget.isSkeletonDebugActive(entity));
}

TEST_F(AnimationWidgetTest, AnimatedEntityShowsAnimationRow)
{
    // An entity created via createAnimatedTestEntity has "TestAnim" animation.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_animated");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    // Should have exactly 1 animation ("TestAnim")
    EXPECT_EQ(animTable->rowCount(), 1);

    // Find the row with "TestAnim"
    bool foundTestAnim = false;
    for (int r = 0; r < animTable->rowCount(); ++r) {
        auto* item = animTable->item(r, 1);
        if (item && item->text() == "TestAnim") {
            foundTestAnim = true;
            break;
        }
    }
    EXPECT_TRUE(foundTestAnim);
}

TEST_F(AnimationWidgetTest, ToggleSkeletonDebugOnAndOff)
{
    // Test the full state transition of skeleton debug: off -> on -> off
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_skeldebug");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    // Initially off
    EXPECT_FALSE(widget.isSkeletonShown(entity));
    EXPECT_FALSE(widget.isSkeletonDebugActive(entity));
    EXPECT_EQ(widget.getSkeletonDebug(entity), nullptr);

    // Turn on
    bool result = widget.toggleSkeletonDebug(entity, true);
    EXPECT_TRUE(result);
    EXPECT_TRUE(widget.isSkeletonShown(entity));
    EXPECT_TRUE(widget.isSkeletonDebugActive(entity));
    EXPECT_NE(widget.getSkeletonDebug(entity), nullptr);

    // Turn off
    result = widget.toggleSkeletonDebug(entity, false);
    EXPECT_TRUE(result);
    EXPECT_FALSE(widget.isSkeletonShown(entity));
    EXPECT_FALSE(widget.isSkeletonDebugActive(entity));
    // After turning off, the SkeletonDebug object is removed
    EXPECT_EQ(widget.getSkeletonDebug(entity), nullptr);
}

TEST_F(AnimationWidgetTest, ToggleBoneWeightsOnAndOff)
{
    // Test the full state transition of bone weights: off -> on -> off
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_boneweights");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    // Initially off
    EXPECT_FALSE(widget.isBoneWeightsShown(entity));
    EXPECT_EQ(widget.getBoneWeightOverlay(entity), nullptr);

    // Turn on
    bool result = widget.toggleBoneWeights(entity, true);
    EXPECT_TRUE(result);
    EXPECT_TRUE(widget.isBoneWeightsShown(entity));
    EXPECT_NE(widget.getBoneWeightOverlay(entity), nullptr);

    // Turning on again should be idempotent (returns true, no double-create)
    result = widget.toggleBoneWeights(entity, true);
    EXPECT_TRUE(result);
    EXPECT_TRUE(widget.isBoneWeightsShown(entity));

    // Turn off
    result = widget.toggleBoneWeights(entity, false);
    EXPECT_TRUE(result);
    EXPECT_FALSE(widget.isBoneWeightsShown(entity));
    EXPECT_EQ(widget.getBoneWeightOverlay(entity), nullptr);

    // Turning off again should be safe
    result = widget.toggleBoneWeights(entity, false);
    EXPECT_TRUE(result);
}

TEST_F(AnimationWidgetTest, DisableAllSkeletonDebugViaDestructor)
{
    // Enable skeleton debug and bone weights, then destroy the widget.
    // The destructor calls disableAllSkeletonDebug() which should clean up.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_destructor");
    ASSERT_NE(entity, nullptr);

    {
        AnimationWidget widget;
        SelectionSet::getSingleton()->selectOne(entity);
        if (app) app->processEvents();

        widget.toggleSkeletonDebug(entity, true);
        widget.toggleBoneWeights(entity, true);
        EXPECT_TRUE(widget.isSkeletonDebugActive(entity));
        EXPECT_TRUE(widget.isBoneWeightsShown(entity));
        // Widget goes out of scope here, destructor should clean up
    }
    if (app) app->processEvents();
    SUCCEED();
}

TEST_F(AnimationWidgetTest, PollAnimationStateUpdatesCheckbox)
{
    // Verify that pollAnimationState picks up externally-changed animation state.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_poll");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    // Find the row for "TestAnim"
    int testAnimRow = -1;
    for (int r = 0; r < animTable->rowCount(); ++r) {
        auto* item = animTable->item(r, 1);
        if (item && item->text() == "TestAnim") {
            testAnimRow = r;
            break;
        }
    }
    ASSERT_GE(testAnimRow, 0) << "Could not find TestAnim row";

    // Initially the animation should be disabled
    auto* enabledItem = animTable->item(testAnimRow, 2);
    ASSERT_NE(enabledItem, nullptr);
    EXPECT_EQ(enabledItem->checkState(), Qt::Unchecked);

    // Externally enable the animation via Ogre API
    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    animState->setEnabled(true);

    // Wait for the poll timer to fire (200ms interval)
    QThread::msleep(250);
    if (app) app->processEvents();

    // The checkbox should now reflect the enabled state
    EXPECT_EQ(enabledItem->checkState(), Qt::Checked);

    // Disable externally and poll again
    animState->setEnabled(false);
    QThread::msleep(250);
    if (app) app->processEvents();

    EXPECT_EQ(enabledItem->checkState(), Qt::Unchecked);
}

TEST_F(AnimationWidgetTest, MultipleWidgetsSyncOnSelectionChange)
{
    // Two AnimationWidget instances should both update when selection changes.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_sync");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget1;
    AnimationWidget widget2;

    // Select entity
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable1 = widget1.findChild<QTableWidget*>("animTable");
    QTableWidget* animTable2 = widget2.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable1, nullptr);
    ASSERT_NE(animTable2, nullptr);

    EXPECT_GT(animTable1->rowCount(), 0);
    EXPECT_EQ(animTable1->rowCount(), animTable2->rowCount());

    QTableWidget* skelTable1 = widget1.findChild<QTableWidget*>("skeletonTable");
    QTableWidget* skelTable2 = widget2.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skelTable1, nullptr);
    ASSERT_NE(skelTable2, nullptr);

    EXPECT_EQ(skelTable1->rowCount(), 1);
    EXPECT_EQ(skelTable1->rowCount(), skelTable2->rowCount());

    // Clear selection -- both should empty
    SelectionSet::getSingleton()->clear();
    if (app) app->processEvents();

    EXPECT_EQ(animTable1->rowCount(), 0);
    EXPECT_EQ(animTable2->rowCount(), 0);
    EXPECT_EQ(skelTable1->rowCount(), 0);
    EXPECT_EQ(skelTable2->rowCount(), 0);
}

TEST_F(AnimationWidgetTest, SkeletonTableWeightsColumnDisabledForNoSkeleton)
{
    // For an entity without a skeleton, the "Show Weights" checkbox should be disabled.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("animwidget_noskel_weights");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("animwidget_noskel_weights_node");
    auto* entity = sceneMgr->createEntity("animwidget_noskel_weights_ent", mesh);
    node->attachObject(entity);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    ASSERT_EQ(skeletonTable->rowCount(), 1);

    // Column 2 is the "Show Weights" checkbox -- should NOT have ItemIsEnabled
    auto* weightsItem = skeletonTable->item(0, 2);
    ASSERT_NE(weightsItem, nullptr);
    EXPECT_FALSE(weightsItem->flags() & Qt::ItemIsEnabled);
}

TEST_F(AnimationWidgetTest, SkeletonDebugToggleUpdatesSkeletonTable)
{
    // When toggling skeleton debug, the skeleton table checkbox should update.
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_skeltable_update");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    ASSERT_EQ(skeletonTable->rowCount(), 1);

    // Initially unchecked
    auto* showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    EXPECT_EQ(showSkeletonItem->checkState(), Qt::Unchecked);

    // Enable skeleton debug programmatically
    widget.toggleSkeletonDebug(entity, true);
    if (app) app->processEvents();

    // The table is rebuilt by toggleSkeletonDebug -> updateSkeletonTable
    // so we need to re-fetch the item
    showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    EXPECT_EQ(showSkeletonItem->checkState(), Qt::Checked);

    // Disable skeleton debug
    widget.toggleSkeletonDebug(entity, false);
    if (app) app->processEvents();

    showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    EXPECT_EQ(showSkeletonItem->checkState(), Qt::Unchecked);
}

TEST_F(AnimationWidgetTest, PlayPauseButtonInitiallyUnchecked)
{
    AnimationWidget widget;
    QPushButton* playPauseButton = widget.findChild<QPushButton*>("PlayPauseButton");
    ASSERT_NE(playPauseButton, nullptr);
    // The button should start in the unchecked (paused) state
    EXPECT_FALSE(playPauseButton->isChecked());
}

// ===========================================================================
// NEW: on_animTable_clicked with enable/disable columns
// ===========================================================================

TEST_F(AnimationWidgetTest, AnimTableClicked_EnableColumn)
{
    // Test clicking on the enable column (col 2) to toggle animation enabled state
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_enable_click");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    // Find "TestAnim" row
    int testAnimRow = -1;
    for (int r = 0; r < animTable->rowCount(); ++r) {
        auto* item = animTable->item(r, 1);
        if (item && item->text() == "TestAnim") {
            testAnimRow = r;
            break;
        }
    }
    ASSERT_GE(testAnimRow, 0);

    // Enable the animation by setting the checkbox and emitting clicked
    auto* enabledItem = animTable->item(testAnimRow, 2);
    ASSERT_NE(enabledItem, nullptr);
    enabledItem->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(enabledItem));
    if (app) app->processEvents();

    // Verify the animation state is now enabled
    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    EXPECT_TRUE(animState->getEnabled());

    // Disable the animation
    enabledItem->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(enabledItem));
    if (app) app->processEvents();

    EXPECT_FALSE(animState->getEnabled());
}

TEST_F(AnimationWidgetTest, AnimTableClicked_LoopColumn)
{
    // Test clicking on the loop column (col 3) to toggle animation loop state
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_loop_click");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    int testAnimRow = -1;
    for (int r = 0; r < animTable->rowCount(); ++r) {
        auto* item = animTable->item(r, 1);
        if (item && item->text() == "TestAnim") {
            testAnimRow = r;
            break;
        }
    }
    ASSERT_GE(testAnimRow, 0);

    // Enable loop
    auto* loopItem = animTable->item(testAnimRow, 3);
    ASSERT_NE(loopItem, nullptr);
    loopItem->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(loopItem));
    if (app) app->processEvents();

    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    EXPECT_TRUE(animState->getLoop());

    // Disable loop
    loopItem->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(loopItem));
    if (app) app->processEvents();

    EXPECT_FALSE(animState->getLoop());
}

TEST_F(AnimationWidgetTest, AnimTableClicked_Column0And1_NoEffect)
{
    // Clicking on column 0 or 1 should NOT change animation state
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_col01_click");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    bool wasEnabled = animState->getEnabled();

    // Click on column 0 (entity name)
    auto* entityItem = animTable->item(0, 0);
    ASSERT_NE(entityItem, nullptr);
    emit animTable->clicked(animTable->indexFromItem(entityItem));
    if (app) app->processEvents();

    EXPECT_EQ(animState->getEnabled(), wasEnabled);

    // Click on column 1 (animation name)
    auto* animNameItem = animTable->item(0, 1);
    ASSERT_NE(animNameItem, nullptr);
    emit animTable->clicked(animTable->indexFromItem(animNameItem));
    if (app) app->processEvents();

    EXPECT_EQ(animState->getEnabled(), wasEnabled);
}

// ===========================================================================
// NEW: on_skeletonTable_clicked column 1 (skeleton debug)
// ===========================================================================

TEST_F(AnimationWidgetTest, SkeletonTableClicked_Column1_ToggleSkeletonDebug)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_skel_col1");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    ASSERT_EQ(skeletonTable->rowCount(), 1);

    // Initially skeleton debug should be off
    EXPECT_FALSE(widget.isSkeletonShown(entity));

    // Check the skeleton debug checkbox (column 1)
    auto* showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    showSkeletonItem->setCheckState(Qt::Checked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(showSkeletonItem));
    if (app) app->processEvents();

    EXPECT_TRUE(widget.isSkeletonShown(entity));

    // Uncheck it
    // After toggle, the table is rebuilt, so re-fetch the item
    showSkeletonItem = skeletonTable->item(0, 1);
    ASSERT_NE(showSkeletonItem, nullptr);
    showSkeletonItem->setCheckState(Qt::Unchecked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(showSkeletonItem));
    if (app) app->processEvents();

    EXPECT_FALSE(widget.isSkeletonShown(entity));
}

// ===========================================================================
// NEW: on_skeletonTable_clicked column 2 (bone weights)
// ===========================================================================

TEST_F(AnimationWidgetTest, SkeletonTableClicked_Column2_ToggleBoneWeights)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_skel_col2");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    ASSERT_EQ(skeletonTable->rowCount(), 1);

    // Initially bone weights should be off
    EXPECT_FALSE(widget.isBoneWeightsShown(entity));

    // Check the bone weights checkbox (column 2)
    auto* weightsItem = skeletonTable->item(0, 2);
    ASSERT_NE(weightsItem, nullptr);

    // The item should be enabled for entities with a skeleton
    if (!(weightsItem->flags() & Qt::ItemIsEnabled)) {
        GTEST_SKIP() << "Skipping: bone weights item is disabled for this entity";
    }

    weightsItem->setCheckState(Qt::Checked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(weightsItem));
    if (app) app->processEvents();

    EXPECT_TRUE(widget.isBoneWeightsShown(entity));

    // Uncheck it
    weightsItem = skeletonTable->item(0, 2);
    ASSERT_NE(weightsItem, nullptr);
    weightsItem->setCheckState(Qt::Unchecked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(weightsItem));
    if (app) app->processEvents();

    EXPECT_FALSE(widget.isBoneWeightsShown(entity));
}

// ===========================================================================
// NEW: on_skeletonTable_clicked column 0 (entity name) -- no effect
// ===========================================================================

TEST_F(AnimationWidgetTest, SkeletonTableClicked_Column0_NoEffect)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_skel_col0");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* skeletonTable = widget.findChild<QTableWidget*>("skeletonTable");
    ASSERT_NE(skeletonTable, nullptr);
    ASSERT_EQ(skeletonTable->rowCount(), 1);

    // Click on column 0 (entity name) -- should do nothing
    auto* entityItem = skeletonTable->item(0, 0);
    ASSERT_NE(entityItem, nullptr);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(entityItem));
    if (app) app->processEvents();

    // No state change -- skeleton debug should still be off
    EXPECT_FALSE(widget.isSkeletonShown(entity));
    EXPECT_FALSE(widget.isBoneWeightsShown(entity));
}

// ===========================================================================
// NEW: on_animTable_cellDoubleClicked column 0 (entity name) -- no effect
// ===========================================================================

TEST_F(AnimationWidgetTest, AnimTableCellDoubleClicked_Column0_NoEffect)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_dblclick_col0");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    // Double-click on column 0 -- should do nothing (the handler returns early if column != 1)
    emit animTable->cellDoubleClicked(0, 0);
    if (app) app->processEvents();

    // If we get here without crash or a modal dialog, the test passes
    SUCCEED();
}

// ===========================================================================
// NEW: Enable animation, then toggle enable off via table click
// ===========================================================================

TEST_F(AnimationWidgetTest, AnimTableClicked_EnableThenDisable_RoundTrip)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("animwidget_enable_roundtrip");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget;
    SelectionSet::getSingleton()->selectOne(entity);
    if (app) app->processEvents();

    QTableWidget* animTable = widget.findChild<QTableWidget*>("animTable");
    ASSERT_NE(animTable, nullptr);
    ASSERT_GT(animTable->rowCount(), 0);

    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);

    // Start disabled
    EXPECT_FALSE(animState->getEnabled());
    EXPECT_FALSE(animState->getLoop());

    // Enable via table
    auto* enableItem = animTable->item(0, 2);
    ASSERT_NE(enableItem, nullptr);
    enableItem->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(enableItem));

    EXPECT_TRUE(animState->getEnabled());

    // Set loop
    auto* loopItem = animTable->item(0, 3);
    ASSERT_NE(loopItem, nullptr);
    loopItem->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(loopItem));

    EXPECT_TRUE(animState->getLoop());

    // Disable both
    enableItem = animTable->item(0, 2);
    enableItem->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(enableItem));
    EXPECT_FALSE(animState->getEnabled());

    loopItem = animTable->item(0, 3);
    loopItem->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(loopItem));
    EXPECT_FALSE(animState->getLoop());
}
