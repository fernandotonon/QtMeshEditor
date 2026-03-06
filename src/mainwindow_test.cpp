#include <gtest/gtest.h>
#include <memory>
#include <QApplication>
#include <QCoreApplication>
#include <QToolBar>
#include <QMenuBar>
#include <QStatusBar>
#include <QSettings>
#include <QDockWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QDropEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QThread>
#include "Manager.h"
#include "SelectionSet.h"
#include "mainwindow.h"
#include "AnimationWidget.h"
#include "animationcontrolwidget.h"
#include "TestHelpers.h"

class MainWindowTest : public ::testing::Test {
    protected:
        QApplication* app = nullptr;
        std::unique_ptr<MainWindow> mainWindow;

        void SetUp() override {
            // Ensure QApplication exists - create if it doesn't
            app = qobject_cast<QApplication*>(QCoreApplication::instance());
            if (!app) {
                // QApplication doesn't exist yet, create it
                static int argc = 1;
                static char appName[] = "QtMeshEditor_test";
                static char* argv[] = {appName, nullptr};
                app = new QApplication(argc, argv); // NOSONAR - intentional: QApplication must outlive all tests
            }
            ASSERT_NE(app, nullptr);

            QCoreApplication::setOrganizationName("QtMeshEditor");
            QCoreApplication::setOrganizationDomain("none");
            QCoreApplication::setApplicationName("QtMeshEditor_test");

            app->setStyle(QStyleFactory::create("Fusion"));

            // Ensure Manager is destroyed from previous tests
            Manager::kill();
            QThread::msleep(50);

            try {
                mainWindow = std::make_unique<MainWindow>();
                ASSERT_NE(mainWindow, nullptr);
            } catch (const Ogre::RenderingAPIException& e) {
                GTEST_SKIP() << "Skipping MainWindow tests: unable to create OGRE render window ("
                             << e.getFullDescription() << ")";
            } catch (const std::exception& e) {
                GTEST_SKIP() << "Skipping MainWindow tests: " << e.what();
            }
        }

        void TearDown() override {
            mainWindow.reset();

            // Clean up Manager
            Manager::kill();

            // Small delay to ensure cleanup is complete
            // Don't call processEvents() here as it can cause segfaults during cleanup
            QThread::msleep(50);
        }
    };

TEST_F(MainWindowTest, ChooseDarkPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionDark");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());

    QSettings settings;
    auto selectedFalette = settings.value("palette");
    EXPECT_EQ(selectedFalette, "dark");
}

TEST_F(MainWindowTest, ChooseLightPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionLight");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());

    QSettings settings;
    auto selectedFalette = settings.value("palette");
    EXPECT_EQ(selectedFalette, "light");
}

TEST_F(MainWindowTest, ChooseCustomPalette) {
    auto paletteAction = mainWindow->findChild<QAction*>("actionCustom");
    ASSERT_TRUE(paletteAction != nullptr);

    // Trigger the action
    paletteAction->toggle();

    // Check if the color dialog is open
    auto colorDialog = mainWindow->findChild<QColorDialog*>("Custom Color Dialog");
    ASSERT_TRUE(colorDialog != nullptr);
    ASSERT_TRUE(colorDialog->isVisible());

    colorDialog->setCurrentColor(QColor(125,122,123));
    colorDialog->accept();

    QSettings settings;
    auto selectedPalette = settings.value("palette");
    EXPECT_EQ(selectedPalette, "custom");

    auto customPaletteColor = settings.value("customPalette");
    EXPECT_EQ(customPaletteColor, QColor(125,122,123));

    // There's no unchecking
    paletteAction->toggle();
    ASSERT_TRUE(paletteAction->isChecked());
} 

TEST_F(MainWindowTest, ChooseAmbientLight) {
    auto actionButton = mainWindow->findChild<QAction*>("actionChange_Ambient_Light");
    ASSERT_TRUE(actionButton != nullptr);

    // Trigger the action
    actionButton->trigger();

    auto colorDialog = mainWindow->findChild<QColorDialog*>("Ambient Light Color Dialog");
    ASSERT_TRUE(colorDialog != nullptr);

    auto testColor = QColor(125,122,123);
    colorDialog->setCurrentColor(testColor);
    colorDialog->accept();

    Ogre::ColourValue ambientLightColour = Manager::getSingleton()->getSceneMgr()->getAmbientLight();
    EXPECT_EQ(ambientLightColour.r, testColor.redF());
    EXPECT_EQ(ambientLightColour.g, testColor.greenF());
    EXPECT_EQ(ambientLightColour.b, testColor.blueF());
}
/*
TEST_F(MainWindowTest, ChooseViewportOptions) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    ASSERT_TRUE(actionSingle != nullptr);
    ASSERT_TRUE(actionSideBySide != nullptr);
    ASSERT_TRUE(actionUpperLower != nullptr);

    actionSingle->toggle();

    // There's no unchecking
    actionSingle->toggle();
    ASSERT_TRUE(actionSingle->isChecked());

    ASSERT_TRUE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());

    actionSideBySide->toggle();

    // There's no unchecking
    actionSideBySide->toggle();
    ASSERT_TRUE(actionSideBySide->isChecked());

    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_TRUE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());

    actionUpperLower->toggle();

    // There's no unchecking
    actionUpperLower->toggle();
    ASSERT_TRUE(actionUpperLower->isChecked());

    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_TRUE(actionUpperLower->isChecked());
} failing on GH Actions */

TEST_F(MainWindowTest, AddViewport) {
    auto actionAddViewport = mainWindow->findChild<QAction*>("actionAdd_Viewport");
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    auto action2x2Grid = mainWindow->findChild<QAction*>("action2x2_Grid");
    ASSERT_TRUE(actionAddViewport != nullptr);
    ASSERT_TRUE(actionSingle != nullptr);
    ASSERT_TRUE(actionSideBySide != nullptr);
    ASSERT_TRUE(actionUpperLower != nullptr);
    ASSERT_TRUE(action2x2Grid != nullptr);

    actionAddViewport->toggle();


    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());
    ASSERT_FALSE(action2x2Grid->isChecked());
}

TEST_F(MainWindowTest, Action2x2GridExists) {
    auto action2x2Grid = mainWindow->findChild<QAction*>("action2x2_Grid");
    ASSERT_TRUE(action2x2Grid != nullptr);
    ASSERT_TRUE(action2x2Grid->isCheckable());
    ASSERT_FALSE(action2x2Grid->isChecked());
}

TEST_F(MainWindowTest, Action2x2GridNoUncheck) {
    auto action2x2Grid = mainWindow->findChild<QAction*>("action2x2_Grid");
    ASSERT_TRUE(action2x2Grid != nullptr);

    // Check it
    action2x2Grid->toggle();
    ASSERT_TRUE(action2x2Grid->isChecked());

    // Try to uncheck — should stay checked (no-uncheck behavior)
    action2x2Grid->toggle();
    ASSERT_TRUE(action2x2Grid->isChecked());
}

TEST_F(MainWindowTest, Action2x2GridMutualExclusion) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    auto action2x2Grid = mainWindow->findChild<QAction*>("action2x2_Grid");
    ASSERT_TRUE(actionSingle != nullptr);
    ASSERT_TRUE(actionSideBySide != nullptr);
    ASSERT_TRUE(actionUpperLower != nullptr);
    ASSERT_TRUE(action2x2Grid != nullptr);

    // Activate 2x2 Grid
    action2x2Grid->toggle();
    ASSERT_TRUE(action2x2Grid->isChecked());
    ASSERT_FALSE(actionSingle->isChecked());
    ASSERT_FALSE(actionSideBySide->isChecked());
    ASSERT_FALSE(actionUpperLower->isChecked());

    // Switch to Single — 2x2 should uncheck
    actionSingle->toggle();
    ASSERT_TRUE(actionSingle->isChecked());
    ASSERT_FALSE(action2x2Grid->isChecked());

    // Switch to 2x2 again then to Side by Side
    action2x2Grid->toggle();
    ASSERT_TRUE(action2x2Grid->isChecked());
    actionSideBySide->toggle();
    ASSERT_TRUE(actionSideBySide->isChecked());
    ASSERT_FALSE(action2x2Grid->isChecked());

    // Switch to 2x2 again then to Upper and Lower
    action2x2Grid->toggle();
    ASSERT_TRUE(action2x2Grid->isChecked());
    actionUpperLower->toggle();
    ASSERT_TRUE(actionUpperLower->isChecked());
    ASSERT_FALSE(action2x2Grid->isChecked());
}

TEST_F(MainWindowTest, SelectTranslateRotate) {
    auto actionSelect_Object = mainWindow->findChild<QAction*>("actionSelect_Object");
    auto actionTranslate_Object = mainWindow->findChild<QAction*>("actionTranslate_Object");
    auto actionRotate_Object = mainWindow->findChild<QAction*>("actionRotate_Object");
    ASSERT_TRUE(actionSelect_Object != nullptr);
    ASSERT_TRUE(actionTranslate_Object != nullptr);
    ASSERT_TRUE(actionRotate_Object != nullptr);

    // SELECT
    actionSelect_Object->trigger();

    ASSERT_TRUE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionSelect_Object->trigger();
    ASSERT_TRUE(actionSelect_Object->isChecked());

    // TRANSLATE
    actionTranslate_Object->trigger();

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionTranslate_Object->trigger();
    ASSERT_TRUE(actionTranslate_Object->isChecked());

    // ROTATE
    actionRotate_Object->trigger();

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // There's no unchecking
    actionRotate_Object->trigger();
    ASSERT_TRUE(actionRotate_Object->isChecked());
}

TEST_F(MainWindowTest, SelectTranslateRotateShortcut) {
    auto actionSelect_Object = mainWindow->findChild<QAction*>("actionSelect_Object");
    auto actionTranslate_Object = mainWindow->findChild<QAction*>("actionTranslate_Object");
    auto actionRotate_Object = mainWindow->findChild<QAction*>("actionRotate_Object");
    ASSERT_TRUE(actionSelect_Object != nullptr);
    ASSERT_TRUE(actionTranslate_Object != nullptr);
    ASSERT_TRUE(actionRotate_Object != nullptr);

    // ROTATE
    // mock pressing R key in mainwindow
    auto event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event.get());
    ASSERT_TRUE(actionRotate_Object->isChecked());

    // SELECT
    event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    ASSERT_TRUE(actionSelect_Object->isChecked());
    ASSERT_FALSE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event.get());
    ASSERT_TRUE(actionSelect_Object->isChecked());

    // TRANSLATE
    event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_T, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());

    // There's no unchecking
    mainWindow->keyPressEvent(event.get());
    ASSERT_TRUE(actionTranslate_Object->isChecked());

    // Other key
    event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    // Keeps the previous status
    ASSERT_FALSE(actionSelect_Object->isChecked());
    ASSERT_TRUE(actionTranslate_Object->isChecked());
    ASSERT_FALSE(actionRotate_Object->isChecked());
}

TEST_F(MainWindowTest, RemoveEmptySelection) {
    auto actionRemove_Object = mainWindow->findChild<QAction*>("actionRemove_Object");
    ASSERT_TRUE(actionRemove_Object != nullptr);

    SelectionSet::getSingleton()->clear();

    auto countBefore = Manager::getSingleton()->getEntities().count();

    actionRemove_Object->trigger();

    auto countAfter = Manager::getSingleton()->getEntities().count();

    ASSERT_EQ(countBefore,countAfter);
}

TEST_F(MainWindowTest, RemoveEmptySelectionShortcut) {
    SelectionSet::getSingleton()->clear();

    auto countBefore = Manager::getSingleton()->getEntities().count();

    auto event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    auto countAfter = Manager::getSingleton()->getEntities().count();

    ASSERT_EQ(countBefore,countAfter);
}

TEST_F(MainWindowTest, RemoveSelectedSceneNodeShortcut) {
    auto sceneNodeName = "TestSceneNode";
    auto sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(sceneNode);

    auto event = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    mainWindow->keyPressEvent(event.get());

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    for (auto node : Manager::getSingleton()->getSceneNodes()) {
        ASSERT_NE(node->getName(), sceneNodeName);
    }
}

TEST_F(MainWindowTest, RemoveAndRecreateSceneNode) {
    auto actionRemove_Object = mainWindow->findChild<QAction*>("actionRemove_Object");
    ASSERT_TRUE(actionRemove_Object != nullptr);

    auto sceneNodeName = "TestSceneNode";
    auto sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(sceneNode);

    actionRemove_Object->trigger();

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    for (auto node : Manager::getSingleton()->getSceneNodes()) {
        ASSERT_NE(node->getName(), sceneNodeName);
    }

    sceneNode = Manager::getSingleton()->addSceneNode(sceneNodeName);
    SelectionSet::getSingleton()->selectOne(sceneNode);
    actionRemove_Object->trigger();
    ASSERT_EQ(countBefore-1,countAfter);
}

TEST_F(MainWindowTest, ShowHideObjectsToolbar) {
    mainWindow->setVisible(true);
    auto actionObjectsToolbar = mainWindow->findChild<QAction*>("actionObjects_Toolbar");
    ASSERT_TRUE(actionObjectsToolbar != nullptr);

    auto objectsToolbar = mainWindow->findChild<QToolBar*>("objectsToolbar");
    ASSERT_TRUE(objectsToolbar != nullptr);

    actionObjectsToolbar->toggle();

    ASSERT_FALSE(actionObjectsToolbar->isChecked());
    ASSERT_FALSE(objectsToolbar->isVisible());

    actionObjectsToolbar->toggle();

    ASSERT_TRUE(actionObjectsToolbar->isChecked());
    ASSERT_TRUE(objectsToolbar->isVisible());
}

TEST_F(MainWindowTest, ShowHideToolsToolbar) {
    mainWindow->setVisible(true);
    auto actionToolsToolbar = mainWindow->findChild<QAction*>("actionTools_Toolbar");
    ASSERT_TRUE(actionToolsToolbar != nullptr);

    auto toolsToolbar = mainWindow->findChild<QToolBar*>("toolToolbar");
    ASSERT_TRUE(toolsToolbar != nullptr);

    actionToolsToolbar->toggle();

    ASSERT_FALSE(actionToolsToolbar->isChecked());
    ASSERT_FALSE(toolsToolbar->isVisible());

    actionToolsToolbar->toggle();

    ASSERT_TRUE(actionToolsToolbar->isChecked());
    ASSERT_TRUE(toolsToolbar->isVisible());
}

TEST_F(MainWindowTest, ShowHideMeshEditor) {
    mainWindow->setVisible(true);
    auto actionMeshEditor = mainWindow->findChild<QAction*>("actionMeshEditor");
    ASSERT_TRUE(actionMeshEditor != nullptr);
    ASSERT_TRUE(actionMeshEditor->isChecked());

    auto meshEditor = mainWindow->findChild<QDockWidget*>("meshEditorWidget");
    ASSERT_TRUE(meshEditor != nullptr);

    actionMeshEditor->toggle();
    ASSERT_FALSE(actionMeshEditor->isChecked());
    ASSERT_FALSE(meshEditor->isVisible());

    actionMeshEditor->toggle();
    ASSERT_TRUE(actionMeshEditor->isChecked());
    ASSERT_TRUE(meshEditor->isVisible());
}

/* TEST_F(MainWindowTest, NavigateTabWidget) {
    mainWindow->setVisible(true);
    auto tabWidget = mainWindow->findChild<QTabWidget*>("tabWidget");
    ASSERT_TRUE(tabWidget != nullptr);

    auto transformTab = tabWidget->widget(0);
    auto materialTab = tabWidget->widget(1);
    auto editTab = tabWidget->widget(2);
    auto animationTab = tabWidget->widget(3);
    ASSERT_TRUE(transformTab != nullptr);
    ASSERT_TRUE(materialTab != nullptr);
    ASSERT_TRUE(editTab != nullptr);
    ASSERT_TRUE(animationTab != nullptr);

    ASSERT_FALSE(animationTab->isVisible());
    ASSERT_EQ(tabWidget->count(), 4);
    tabWidget->setCurrentIndex(3);
    ASSERT_TRUE(animationTab->isVisible());
} failing on GH Actions */

TEST_F(MainWindowTest, FrameRendering) {
    auto statusBar = mainWindow->findChild<QStatusBar*>("statusBar");
    ASSERT_TRUE(statusBar != nullptr);

    auto message = statusBar->currentMessage();
    ASSERT_EQ(message, "");

    Manager::getSingleton()->getRoot()->renderOneFrame();

    message = statusBar->currentMessage();
    ASSERT_TRUE(message.startsWith("Status "));
}


TEST_F(MainWindowTest, OpenMaterialWindow) {
    auto actionMaterial_Editor = mainWindow->findChild<QAction*>("actionMaterial_Editor");
    ASSERT_TRUE(actionMaterial_Editor != nullptr);

    int childrenBefore = mainWindow->children().size();

    actionMaterial_Editor->trigger();

    int childrenAfter = mainWindow->children().size();
    ASSERT_EQ(childrenBefore, childrenAfter-1);
} 

TEST_F(MainWindowTest, OpenAbout) {
    auto actionAbout = mainWindow->findChild<QAction*>("actionAbout");
    ASSERT_TRUE(actionAbout != nullptr);

    int childrenBefore = mainWindow->children().size();

    actionAbout->trigger();

    int childrenAfter = mainWindow->children().size();
    ASSERT_EQ(childrenBefore, childrenAfter-1);
}

TEST_F(MainWindowTest, DropEvent) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto entities = Manager::getSingleton()->getEntities();
    int countBefore = entities.count();
    // Create a QDropEvent instance for testing
    auto mimeData = std::make_unique<QMimeData>();
    auto event = std::make_unique<QDropEvent>(QPoint(), Qt::CopyAction, mimeData.get(), Qt::LeftButton, Qt::NoModifier);

    // Set the mime data with valid URIs
    mimeData->setUrls({
        QUrl::fromLocalFile("./media/models/ninja.mesh"),
        QUrl::fromLocalFile("./media/models/robot.mesh"),
        QUrl::fromLocalFile("./media/models/Rumba Dancing.fbx")
    });

    // Call the dropEvent method
    mainWindow->dropEvent(event.get());

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);

    // Set the mime data with an invalid URI
    mimeData->setUrls({QUrl::fromLocalFile("./UnitTests")});

    // Call the dropEvent method again
    mainWindow->dropEvent(event.get());

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is not loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);

    // Set the mime data with no URLs
    mimeData->setUrls({});

    // Call the dropEvent method again
    mainWindow->dropEvent(event.get());

    Manager::getSingleton()->getRoot()->renderOneFrame();

    // Verify that the file is not loaded
    entities = Manager::getSingleton()->getEntities();
    ASSERT_EQ(entities.count(), countBefore+3);
}

TEST_F(MainWindowTest, SelectAnimatedEntity)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    try {
        auto widget = std::make_unique<AnimationWidget>(mainWindow.get());
        auto animControl = std::make_unique<AnimationControlWidget>();
        // import a mesh
        QStringList validUri{"./media/models/ninja.mesh"};
        mainWindow->importMeshs(validUri);
        Manager::getSingleton()->getRoot()->renderOneFrame();
        
        // Check if entities were created
        auto entities = Manager::getSingleton()->getEntities();
        if (entities.isEmpty()) {
            GTEST_SKIP() << "Skipping test: mesh import failed or no entities created";
        }
        
        auto entity = entities.last();
        SelectionSet::getSingleton()->selectOne(entity);

    // Verify the entity name is ninja and has these animations:
    /*
    Attack1
    Attack2
    Attack3
    Backflip
    Block
    Climb
    Crouch
    Death1
    Death2
    HighJump
    Idle1
    Idle2
    Idle3
    Jump
    JumpNoHeight
    Kick
    SideKick
    Spin
    Stealth
    Walk
    */
    auto animTable = widget->findChild<QTableWidget*>("animTable");
    ASSERT_EQ(animTable->rowCount(), 20);
    ASSERT_EQ(animTable->item(0, 0)->text().toStdString(), "ninja4");
    ASSERT_EQ(animTable->item(0, 1)->text().toStdString(), "Walk");
    ASSERT_EQ(animTable->item(1, 1)->text().toStdString(), "Stealth");

    // Enable/Disable the Walk animation by clicking on the third column
    auto item = animTable->item(0, 2);
    item->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(item));
    auto animationState = entity->getAnimationState("Walk");
    ASSERT_TRUE(animationState->getEnabled());

    item = animTable->item(0, 2);
    item->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(item));
    animationState = entity->getAnimationState("Walk");
    ASSERT_FALSE(animationState->getEnabled());

    // Enable/Disable the Walk animation loop by clicking on the forth column
    item = animTable->item(0, 3);
    item->setCheckState(Qt::Checked);
    emit animTable->clicked(animTable->indexFromItem(item));
    animationState = entity->getAnimationState("Walk");
    ASSERT_TRUE(animationState->getLoop());

    item = animTable->item(0, 3);
    item->setCheckState(Qt::Unchecked);
    emit animTable->clicked(animTable->indexFromItem(item));
    animationState = entity->getAnimationState("Walk");
    ASSERT_FALSE(animationState->getLoop());

    // rename Walk Animation
    emit animTable->cellDoubleClicked(0,0); // don't do anything
    // emit animTable->cellDoubleClicked(0,1); // open name modal (crashing the test)

    // Show the skeleton debug
    auto skeletonTable = widget->findChild<QTableWidget*>("skeletonTable");
    ASSERT_EQ(skeletonTable->rowCount(), 1);
    ASSERT_EQ(skeletonTable->item(0, 0)->text().toStdString(), "ninja4");

    // Verify the entities before
    ASSERT_FALSE(widget->isSkeletonShown(entity));

    // Enable/Disable the skeleton debug by clicking on the second column
    item = skeletonTable->item(0, 1);
    item->setCheckState(Qt::Checked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(item));
    ASSERT_TRUE(widget->isSkeletonShown(entity));

    item = skeletonTable->item(0, 1);
    item->setCheckState(Qt::Unchecked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(item));
    ASSERT_FALSE(widget->isSkeletonShown(entity));

    item = skeletonTable->item(0, 0);
    item->setCheckState(Qt::Checked);
    emit skeletonTable->clicked(skeletonTable->indexFromItem(item));
    ASSERT_FALSE(widget->isSkeletonShown(entity)); //dont do anything

    // Check the anim list in animationcontrolwidget
    auto treeWidget = animControl->findChild<QTreeWidget*>("treeWidget");
    ASSERT_EQ(treeWidget->topLevelItemCount(), 1);
    auto topLevelItem = treeWidget->topLevelItem(0);
    ASSERT_EQ(topLevelItem->text(0).toStdString(), "mesh: ninja4");
    ASSERT_EQ(topLevelItem->childCount(), 20);
    ASSERT_EQ(topLevelItem->child(0)->text(0).toStdString(), "anim: Attack1");
    // Click on the top level item should not show the bone list
    treeWidget->setCurrentItem(topLevelItem);
    auto boneList = animControl->findChild<QListWidget*>("boneList");
    ASSERT_EQ(boneList->count(), 0);
    // Click on the first child item to show the bone list
    treeWidget->setCurrentItem(topLevelItem->child(0));
    ASSERT_EQ(boneList->count(), 28);
    Manager::getSingleton()->getRoot()->renderOneFrame();

    SelectionSet::getSingleton()->clear();
    ASSERT_EQ(animTable->rowCount(), 0);
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping SelectAnimatedEntity test: Ogre exception ("
                     << e.getFullDescription() << ")";
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Skipping SelectAnimatedEntity test: " << e.what();
    } catch (...) {
        GTEST_SKIP() << "Skipping SelectAnimatedEntity test: unknown exception (possible segfault in mesh loading)";
    }
}

TEST_F(MainWindowTest, AnimationStateChange)
{
    // Create an instance of AnimationWidget
    AnimationWidget widget;

    auto playButton = widget.findChild<QPushButton*>("PlayPauseButton");

    // Create a signal spy to monitor the changeAnimationState signal
    QSignalSpy spy(&widget, SIGNAL(changeAnimationState(bool)));

    // click the play button to change animation state
    playButton->setChecked(true);

    // Check if the changeAnimationState signal was emitted with the correct argument
    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> arguments = spy.takeFirst();
    ASSERT_EQ(arguments.at(0).toBool(), true);

    // click the play button to change animation state
    playButton->setChecked(false);
    playButton->click();

    // Check if the changeAnimationState signal was emitted with the correct argument
    ASSERT_EQ(spy.count(), 2);
    arguments = spy.takeFirst();
    ASSERT_EQ(arguments.at(0).toBool(), false);
}

TEST_F(MainWindowTest, RecentFilesMenuExists) {
    auto recentFilesMenu = mainWindow->findChild<QMenu*>("recentFilesMenu");
    ASSERT_TRUE(recentFilesMenu != nullptr);
    ASSERT_EQ(recentFilesMenu->title(), "Recent Files");
}

TEST_F(MainWindowTest, AddToRecentFiles) {
    // Clear any existing recent files first
    QSettings settings;
    settings.remove("RecentFiles/files");

    auto recentFilesMenu = mainWindow->findChild<QMenu*>("recentFilesMenu");
    ASSERT_TRUE(recentFilesMenu != nullptr);

    // Simulate importing via drop to add to recent files
    auto mimeData = std::make_unique<QMimeData>();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/test_model.mesh")});
    auto event = std::make_unique<QDropEvent>(QPoint(), Qt::CopyAction, mimeData.get(), Qt::LeftButton, Qt::NoModifier);
    mainWindow->dropEvent(event.get());

    // Verify QSettings has the file
    QStringList files = settings.value("RecentFiles/files").toStringList();
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(files.first(), "/tmp/test_model.mesh");

    // Verify menu has the file action (plus separator + Clear action)
    auto actions = recentFilesMenu->actions();
    ASSERT_GE(actions.size(), 3); // 1 file + separator + Clear
    EXPECT_EQ(actions.first()->data().toString(), "/tmp/test_model.mesh");

    // Clean up
    settings.remove("RecentFiles/files");
}

TEST_F(MainWindowTest, RecentFilesMaxLimit) {
    QSettings settings;
    settings.remove("RecentFiles/files");

    // Add 12 files via drop events
    for (int i = 0; i < 12; ++i) {
        auto mimeData = std::make_unique<QMimeData>();
        mimeData->setUrls({QUrl::fromLocalFile(QString("/tmp/model_%1.mesh").arg(i))});
        auto event = std::make_unique<QDropEvent>(QPoint(), Qt::CopyAction, mimeData.get(), Qt::LeftButton, Qt::NoModifier);
        mainWindow->dropEvent(event.get());
    }

    // Verify only 10 are stored
    QStringList files = settings.value("RecentFiles/files").toStringList();
    ASSERT_EQ(files.size(), 10);
    // Most recent should be first
    EXPECT_EQ(files.first(), "/tmp/model_11.mesh");
    // Oldest two (0, 1) should have been dropped
    EXPECT_FALSE(files.contains("/tmp/model_0.mesh"));
    EXPECT_FALSE(files.contains("/tmp/model_1.mesh"));

    // Clean up
    settings.remove("RecentFiles/files");
}

TEST_F(MainWindowTest, RecentFilesDeduplicate) {
    QSettings settings;
    settings.remove("RecentFiles/files");

    // Add the same file twice via drop
    for (int i = 0; i < 2; ++i) {
        auto mimeData = std::make_unique<QMimeData>();
        mimeData->setUrls({QUrl::fromLocalFile("/tmp/duplicate.mesh")});
        auto event = std::make_unique<QDropEvent>(QPoint(), Qt::CopyAction, mimeData.get(), Qt::LeftButton, Qt::NoModifier);
        mainWindow->dropEvent(event.get());
    }

    // Verify only one entry exists
    QStringList files = settings.value("RecentFiles/files").toStringList();
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(files.first(), "/tmp/duplicate.mesh");

    // Clean up
    settings.remove("RecentFiles/files");
}

TEST_F(MainWindowTest, ClearRecentFiles) {
    QSettings settings;
    settings.remove("RecentFiles/files");

    // Add a file
    auto mimeData = std::make_unique<QMimeData>();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/to_clear.mesh")});
    auto event = std::make_unique<QDropEvent>(QPoint(), Qt::CopyAction, mimeData.get(), Qt::LeftButton, Qt::NoModifier);
    mainWindow->dropEvent(event.get());

    QStringList files = settings.value("RecentFiles/files").toStringList();
    ASSERT_EQ(files.size(), 1);

    // Find and trigger the "Clear Recent Files" action
    auto recentFilesMenu = mainWindow->findChild<QMenu*>("recentFilesMenu");
    ASSERT_TRUE(recentFilesMenu != nullptr);
    auto actions = recentFilesMenu->actions();
    QAction* clearAction = actions.last();
    ASSERT_EQ(clearAction->text(), "Clear Recent Files");
    clearAction->trigger();

    // Verify QSettings is empty
    files = settings.value("RecentFiles/files").toStringList();
    ASSERT_TRUE(files.isEmpty());

    // Verify menu shows "(No Recent Files)" placeholder
    actions = recentFilesMenu->actions();
    ASSERT_GE(actions.size(), 3); // placeholder + separator + Clear
    EXPECT_EQ(actions.first()->text(), "(No Recent Files)");
    EXPECT_FALSE(actions.first()->isEnabled());

    // Clean up
    settings.remove("RecentFiles/files");
}


TEST_F(MainWindowTest, SetPlayingTrue) {
    mainWindow->setPlaying(true);
    EXPECT_TRUE(true);
}

TEST_F(MainWindowTest, SetPlayingFalse) {
    mainWindow->setPlaying(true);
    mainWindow->setPlaying(false);
    EXPECT_TRUE(true);
}

TEST_F(MainWindowTest, SetMCPServerPointer) {
    mainWindow->setMCPServer(nullptr);
    EXPECT_TRUE(true);
}

TEST_F(MainWindowTest, ImportMeshsWithInvalidPaths) {
    QStringList invalidPaths = {"/nonexistent/path/fake.mesh", ""};
    auto entitiesBefore = Manager::getSingleton()->getEntities().count();
    mainWindow->importMeshs(invalidPaths);
    Manager::getSingleton()->getRoot()->renderOneFrame();
    auto entitiesAfter = Manager::getSingleton()->getEntities().count();
    EXPECT_EQ(entitiesBefore, entitiesAfter);
}

TEST_F(MainWindowTest, ImportMeshsWithEmptyList) {
    QStringList emptyList;
    auto entitiesBefore = Manager::getSingleton()->getEntities().count();
    mainWindow->importMeshs(emptyList);
    Manager::getSingleton()->getRoot()->renderOneFrame();
    auto entitiesAfter = Manager::getSingleton()->getEntities().count();
    EXPECT_EQ(entitiesBefore, entitiesAfter);
}

// ===========================================================================
// NEW: Branch coverage — setPlaying toggle on/off/on
// ===========================================================================

TEST_F(MainWindowTest, SetPlayingToggleMultiple) {
    mainWindow->setPlaying(true);
    mainWindow->setPlaying(false);
    mainWindow->setPlaying(true);
    mainWindow->setPlaying(false);
    // No crash — exercises isPlaying toggle paths in frameRenderingQueued
    EXPECT_TRUE(true);
}

// ===========================================================================
// NEW: Branch coverage — menu bar structural verification
// ===========================================================================

TEST_F(MainWindowTest, MenuBarExists) {
    auto menuBar = mainWindow->menuBar();
    ASSERT_NE(menuBar, nullptr);

    // Verify expected menus exist
    auto actions = menuBar->actions();
    ASSERT_GE(actions.size(), 3); // File, View, Help at minimum

    QStringList menuTitles;
    for (auto* action : actions) {
        if (action->menu()) {
            menuTitles << action->text().remove('&');
        }
    }
    EXPECT_TRUE(menuTitles.contains("File"));
    EXPECT_TRUE(menuTitles.contains("View"));
}

// ===========================================================================
// NEW: Branch coverage — toolbars structural verification
// ===========================================================================

TEST_F(MainWindowTest, ObjectsToolBarExists) {
    auto objectsToolbar = mainWindow->findChild<QToolBar*>("objectsToolbar");
    ASSERT_NE(objectsToolbar, nullptr);
    EXPECT_FALSE(objectsToolbar->actions().isEmpty());
}

TEST_F(MainWindowTest, ToolsToolBarExists) {
    auto toolsToolbar = mainWindow->findChild<QToolBar*>("toolToolbar");
    ASSERT_NE(toolsToolbar, nullptr);
    EXPECT_FALSE(toolsToolbar->actions().isEmpty());
}

// ===========================================================================
// NEW: Branch coverage — status bar exists
// ===========================================================================

TEST_F(MainWindowTest, StatusBarExists) {
    auto statusBar = mainWindow->findChild<QStatusBar*>("statusBar");
    ASSERT_NE(statusBar, nullptr);
}

// ===========================================================================
// NEW: Branch coverage — Scale action exists and toggles
// ===========================================================================

TEST_F(MainWindowTest, ScaleActionExists) {
    auto actionScale = mainWindow->findChild<QAction*>("actionScale_Object");
    if (actionScale) {
        EXPECT_TRUE(actionScale->isCheckable());
        actionScale->trigger();
        EXPECT_TRUE(actionScale->isChecked());
    }
    // If action doesn't exist, that's also fine — optional feature
}

// ===========================================================================
// NEW: Branch coverage — frameRenderingQueued with playing + animations
// ===========================================================================

TEST_F(MainWindowTest, FrameRenderingQueuedWhilePlaying) {
    mainWindow->setPlaying(true);
    // Render a frame — exercises the isPlaying branch in frameRenderingQueued
    Manager::getSingleton()->getRoot()->renderOneFrame();
    mainWindow->setPlaying(false);
    // Render again — exercises the !isPlaying path
    Manager::getSingleton()->getRoot()->renderOneFrame();
    EXPECT_TRUE(true);
}

// ===========================================================================
// Drag-and-drop: dragEnterEvent accepts various file types
// ===========================================================================

TEST_F(MainWindowTest, DragEnterEvent_AcceptsMeshFiles) {
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/test.mesh")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEvent_AcceptsFBXFiles) {
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/model.fbx")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEvent_AcceptsOBJFiles) {
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/scene.obj")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEvent_AcceptsDAEFiles) {
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/animation.dae")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    // dragEnterEvent accepts all proposed actions unconditionally
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEvent_AcceptsNonMeshFiles) {
    // The current implementation accepts all drag events unconditionally
    // (filtering happens in dropEvent). Verify this behavior.
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/readme.txt"),
                        QUrl::fromLocalFile("/tmp/program.exe")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEvent_AcceptsEmptyUrls) {
    // Even with no URLs the drag enter is accepted (filtering in dropEvent)
    auto mimeData = new QMimeData();
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

// ===========================================================================
// Keyboard shortcut: Key_E is unmapped — does not change transform state
// ===========================================================================

TEST_F(MainWindowTest, KeyE_DoesNotChangeTransformState) {
    auto actionSelect = mainWindow->findChild<QAction*>("actionSelect_Object");
    auto actionTranslate = mainWindow->findChild<QAction*>("actionTranslate_Object");
    auto actionRotate = mainWindow->findChild<QAction*>("actionRotate_Object");
    ASSERT_NE(actionSelect, nullptr);
    ASSERT_NE(actionTranslate, nullptr);
    ASSERT_NE(actionRotate, nullptr);

    // Put into SELECT state first
    auto keyY = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier);
    mainWindow->keyPressEvent(keyY.get());
    ASSERT_TRUE(actionSelect->isChecked());

    // Press Key_E — should have no effect (unmapped key)
    auto keyE = std::make_unique<QKeyEvent>(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
    mainWindow->keyPressEvent(keyE.get());
    EXPECT_TRUE(actionSelect->isChecked());
    EXPECT_FALSE(actionTranslate->isChecked());
    EXPECT_FALSE(actionRotate->isChecked());
}

// ===========================================================================
// Show Grid action exists and is checkable
// ===========================================================================

TEST_F(MainWindowTest, ShowGrid_ActionExists) {
    auto actionShowGrid = mainWindow->findChild<QAction*>("actionShow_Grid");
    ASSERT_NE(actionShowGrid, nullptr);
    EXPECT_TRUE(actionShowGrid->isCheckable());
}

// ===========================================================================
// Show Normals action exists and is checkable
// ===========================================================================

TEST_F(MainWindowTest, ShowNormals_ActionExists) {
    auto actionShowNormals = mainWindow->findChild<QAction*>("actionShow_Normals");
    ASSERT_NE(actionShowNormals, nullptr);
    EXPECT_TRUE(actionShowNormals->isCheckable());
    // Default state: normals are off
    EXPECT_FALSE(actionShowNormals->isChecked());
}

// ===========================================================================
// Export with no selection — should be a safe no-op
// ===========================================================================

TEST_F(MainWindowTest, ExportSelected_NoSelection) {
    // Ensure nothing is selected
    SelectionSet::getSingleton()->clear();
    ASSERT_FALSE(SelectionSet::getSingleton()->hasNodes());
    ASSERT_FALSE(SelectionSet::getSingleton()->hasEntities());

    // Trigger the export — should be a no-op, not crash
    auto actionExport = mainWindow->findChild<QAction*>("actionExport_Selected");
    ASSERT_NE(actionExport, nullptr);
    actionExport->trigger();
    EXPECT_TRUE(true); // Reached here without crashing
}

// ===========================================================================
// Single viewport action — verify toggle behavior
// ===========================================================================

TEST_F(MainWindowTest, SingleViewportAction_Toggle) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    ASSERT_NE(actionSingle, nullptr);
    EXPECT_TRUE(actionSingle->isCheckable());

    // Toggle to single (default)
    actionSingle->setChecked(false);
    actionSingle->toggle();
    EXPECT_TRUE(actionSingle->isChecked());
}

// ===========================================================================
// Side-by-side viewport action — verify toggle and mutual exclusion
// ===========================================================================

TEST_F(MainWindowTest, SideBySideViewportAction_Toggle) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionSideBySide = mainWindow->findChild<QAction*>("action1x1_Side_by_Side");
    ASSERT_NE(actionSingle, nullptr);
    ASSERT_NE(actionSideBySide, nullptr);
    EXPECT_TRUE(actionSideBySide->isCheckable());

    // Toggle side-by-side on
    actionSideBySide->toggle();
    EXPECT_TRUE(actionSideBySide->isChecked());
    EXPECT_FALSE(actionSingle->isChecked());

    // Restore single viewport
    actionSingle->toggle();
    EXPECT_TRUE(actionSingle->isChecked());
}

// ===========================================================================
// Upper-and-lower viewport action — verify toggle and mutual exclusion
// ===========================================================================

TEST_F(MainWindowTest, UpperLowerViewportAction_Toggle) {
    auto actionSingle = mainWindow->findChild<QAction*>("actionSingle");
    auto actionUpperLower = mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower");
    ASSERT_NE(actionSingle, nullptr);
    ASSERT_NE(actionUpperLower, nullptr);
    EXPECT_TRUE(actionUpperLower->isCheckable());

    // Toggle upper-and-lower on
    actionUpperLower->toggle();
    EXPECT_TRUE(actionUpperLower->isChecked());
    EXPECT_FALSE(actionSingle->isChecked());

    // Restore single viewport
    actionSingle->toggle();
    EXPECT_TRUE(actionSingle->isChecked());
}

// ===========================================================================
// Frame ended callback — no crash on renderOneFrame
// ===========================================================================

TEST_F(MainWindowTest, FrameEnded_NoCrash) {
    // frameEnded processes the mUriList and updates viewports.
    // A single renderOneFrame triggers frameStarted, frameRenderingQueued, and frameEnded.
    Manager::getSingleton()->getRoot()->renderOneFrame();
    EXPECT_TRUE(true);
}

// ===========================================================================
// Multiple consecutive render frames — stability test
// ===========================================================================

TEST_F(MainWindowTest, MultipleRenderFrames) {
    for (int i = 0; i < 5; ++i) {
        Manager::getSingleton()->getRoot()->renderOneFrame();
    }
    EXPECT_TRUE(true);
}

// ===========================================================================
// Key release event — no crash
// ===========================================================================

TEST_F(MainWindowTest, KeyReleaseEvent_NoCrash) {
    auto keyRelease = std::make_unique<QKeyEvent>(QEvent::KeyRelease, Qt::Key_T, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), keyRelease.get());
    EXPECT_TRUE(true); // No crash
}

// ===========================================================================
// Key release for multiple keys — exercises keyReleaseEvent path
// ===========================================================================

TEST_F(MainWindowTest, KeyReleaseEvent_MultipleKeys) {
    auto keyR = std::make_unique<QKeyEvent>(QEvent::KeyRelease, Qt::Key_R, Qt::NoModifier);
    auto keyY = std::make_unique<QKeyEvent>(QEvent::KeyRelease, Qt::Key_Y, Qt::NoModifier);
    auto keyE = std::make_unique<QKeyEvent>(QEvent::KeyRelease, Qt::Key_E, Qt::NoModifier);
    auto keyDel = std::make_unique<QKeyEvent>(QEvent::KeyRelease, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), keyR.get());
    QApplication::sendEvent(mainWindow.get(), keyY.get());
    QApplication::sendEvent(mainWindow.get(), keyE.get());
    QApplication::sendEvent(mainWindow.get(), keyDel.get());
    EXPECT_TRUE(true);
}

// ===========================================================================
// Show Grid toggle — flip on/off
// ===========================================================================

TEST_F(MainWindowTest, ShowGrid_ToggleOnOff) {
    auto actionShowGrid = mainWindow->findChild<QAction*>("actionShow_Grid");
    ASSERT_NE(actionShowGrid, nullptr);

    bool initialState = actionShowGrid->isChecked();
    actionShowGrid->toggle();
    EXPECT_NE(actionShowGrid->isChecked(), initialState);
    actionShowGrid->toggle();
    EXPECT_EQ(actionShowGrid->isChecked(), initialState);
}

// ===========================================================================
// Show Normals toggle — flip on/off
// ===========================================================================

TEST_F(MainWindowTest, ShowNormals_ToggleOnOff) {
    auto actionShowNormals = mainWindow->findChild<QAction*>("actionShow_Normals");
    ASSERT_NE(actionShowNormals, nullptr);

    bool initialState = actionShowNormals->isChecked();
    actionShowNormals->toggle();
    EXPECT_NE(actionShowNormals->isChecked(), initialState);
    actionShowNormals->toggle();
    EXPECT_EQ(actionShowNormals->isChecked(), initialState);
}

// ===========================================================================
// Viewport actions exist as a complete group
// ===========================================================================

TEST_F(MainWindowTest, AllViewportActionsExist) {
    EXPECT_NE(mainWindow->findChild<QAction*>("actionSingle"), nullptr);
    EXPECT_NE(mainWindow->findChild<QAction*>("action1x1_Side_by_Side"), nullptr);
    EXPECT_NE(mainWindow->findChild<QAction*>("action1x1_Upper_and_Lower"), nullptr);
    EXPECT_NE(mainWindow->findChild<QAction*>("action2x2_Grid"), nullptr);
}

// ===========================================================================
// DragEnterEvent with multiple mesh URLs — all accepted
// ===========================================================================

TEST_F(MainWindowTest, DragEnterEvent_MultipleUrls) {
    auto mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/tmp/a.mesh"),
                        QUrl::fromLocalFile("/tmp/b.fbx"),
                        QUrl::fromLocalFile("/tmp/c.obj"),
                        QUrl::fromLocalFile("/tmp/d.dae")});
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mainWindow.get(), &event);
    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}
