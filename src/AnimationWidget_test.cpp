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
