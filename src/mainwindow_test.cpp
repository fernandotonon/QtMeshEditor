#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QDateTime>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>

// NOTE: These access-specifier redefinitions are a pragmatic test-only workaround
// to cover MainWindow internals. Prefer dedicated test APIs or friend tests when feasible.
#define private public
#define protected public
#include "mainwindow.h"
#undef protected
#undef private

#include "Manager.h"
#include "MCPServer.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "TransformOperator.h"
#include "EditModeController.h"
#include "EditorModeController.h"
#include "MeshInfoOverlay.h"
#include "TestHelpers.h"
#include "EditorViewport.h"
#include "ViewportSettingsKeys.h"
#include "ViewportTitleBar.h"
#include "OgreWidget.h"
#include "ui_mainwindow.h"

class MainWindowTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* window = nullptr;
    QTemporaryDir tempDir;
    QString previousOrganizationName;
    QString previousApplicationName;

    void SetUp() override {
        TransformOperator::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);

        previousOrganizationName = QCoreApplication::organizationName();
        previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName("QtMeshEditorTests");
        QCoreApplication::setApplicationName("MainWindowTest");
        QSettings().clear();

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        try {
            window = new MainWindow();
        } catch (const std::exception& e) {
            FAIL() << "MainWindow construction failed: " << e.what();
        } catch (...) {
            FAIL() << "MainWindow construction failed with unknown exception";
        }

        ASSERT_NE(window, nullptr);
    }

    void TearDown() override {
        delete window;
        window = nullptr;
        // Tests below switch the editor mode (Animation/Material/etc). Reset
        // the singleton so subsequent test cases see a fresh ObjectMode
        // controller — otherwise stale state leaks into m_editModeLabel and
        // any test that implicitly assumes the default mode.
        EditorModeController::kill();
        QSettings().clear();
        QCoreApplication::setOrganizationName(previousOrganizationName);
        QCoreApplication::setApplicationName(previousApplicationName);
        if (app) app->processEvents();
    }

    QAction* recentFileAction(int index) const
    {
        auto actions = window->m_recentFilesMenu->actions();
        if (index < 0 || index >= actions.size()) {
            return nullptr;
        }
        return actions.at(index);
    }

    Ogre::Entity* createAnimatedEntity(const char* name)
    {
        if (!canLoadMeshFiles()) {
            return nullptr;
        }

        return createAnimatedTestEntity(name);
    }

    QAction* findActionByObjectName(const QString& objectName) const
    {
        if (!window) {
            return nullptr;
        }
        return window->findChild<QAction*>(objectName);
    }

    void closeAnyOpenMessageBoxesSoon() const
    {
        QTimer::singleShot(0, [] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* messageBox = qobject_cast<QMessageBox*>(widget)) {
                    messageBox->accept();
                }
            }
        });
    }
};

TEST_F(MainWindowTest, ViewMenuContextPanelAndConsoleDefaultChecked)
{
    QAction* ctx = window->findChild<QAction*>(QStringLiteral("actionView_Context_Panel"));
    QAction* con = window->findChild<QAction*>(QStringLiteral("actionView_Console"));
    ASSERT_NE(ctx, nullptr);
    ASSERT_NE(con, nullptr);
    EXPECT_TRUE(ctx->isChecked());
    EXPECT_TRUE(con->isChecked());
    EXPECT_TRUE(QSettings().value(QStringLiteral("View/showContextPanel"), false).toBool());
    EXPECT_TRUE(QSettings().value(QStringLiteral("View/showConsole"), false).toBool());
}

TEST_F(MainWindowTest, ViewMenuConsoleToggleUpdatesDockVisibilityAndSettings)
{
    QAction* con = window->findChild<QAction*>(QStringLiteral("actionView_Console"));
    ASSERT_NE(con, nullptr);

    con->setChecked(false);
    app->processEvents();
    EXPECT_TRUE(window->m_consoleDock->isHidden());
    EXPECT_FALSE(QSettings().value(QStringLiteral("View/showConsole"), true).toBool());

    con->setChecked(true);
    app->processEvents();
    EXPECT_FALSE(window->m_consoleDock->isHidden());
    EXPECT_TRUE(QSettings().value(QStringLiteral("View/showConsole"), false).toBool());
}

TEST_F(MainWindowTest, ConsoleWidgetReceivesQtLogLine)
{
    const QString marker = QStringLiteral("QtMeshEditor_TestLogMarker_%1")
                               .arg(QDateTime::currentMSecsSinceEpoch());
    qWarning().noquote() << marker;
    app->processEvents();

    ASSERT_NE(window->m_consoleEdit, nullptr);
    EXPECT_TRUE(window->m_consoleEdit->toPlainText().contains(marker))
        << window->m_consoleEdit->toPlainText().toStdString();
}

// ---- setTransformState ----

TEST_F(MainWindowTest, SetTransformStateSelect) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

TEST_F(MainWindowTest, SetTransformStateTranslate) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

TEST_F(MainWindowTest, SetTransformStateRotate) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

TEST_F(MainWindowTest, SetTransformStateScale) {
    QKeyEvent event(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

// ---- Key shortcuts ----

TEST_F(MainWindowTest, KeyXTogglesTransformSpace) {
    auto space_before = TransformOperator::getSingleton()->getTransformSpace();
    QKeyEvent firstToggle(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    window->keyPressEvent(&firstToggle);
    auto space_after = TransformOperator::getSingleton()->getTransformSpace();
    EXPECT_NE(space_before, space_after);

    // Toggle back
    QKeyEvent secondToggle(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    window->keyPressEvent(&secondToggle);
    EXPECT_EQ(TransformOperator::getSingleton()->getTransformSpace(), space_before);
}

TEST_F(MainWindowTest, TransformSpaceActionShowsCurrentSpaceName)
{
    auto* action = window->findChild<QAction*>(QStringLiteral("actionToggle_Transform_Space"));
    ASSERT_NE(action, nullptr);

    TransformOperator::getSingleton()->setTransformSpace(TransformOperator::SPACE_WORLD);
    EXPECT_EQ(action->text(), QStringLiteral("World"));

    TransformOperator::getSingleton()->setTransformSpace(TransformOperator::SPACE_LOCAL);
    EXPECT_EQ(action->text(), QStringLiteral("Local"));
}

TEST_F(MainWindowTest, KeyDeleteWithEmptySelection) {
    SelectionSet::getSingleton()->clear();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

TEST_F(MainWindowTest, KeyFFrameSelectionEmptyDoesNotCrash) {
    SelectionSet::getSingleton()->clear();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&event));
}

TEST_F(MainWindowTest, KeyReleaseEventDoesNotCrash) {
    QKeyEvent event(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
    EXPECT_NO_THROW(window->keyReleaseEvent(&event));
}

// keyReleaseEvent is protected — tested implicitly via keyPressEvent

// ---- setPlaying ----

TEST_F(MainWindowTest, SetPlayingTrue) {
    window->setPlaying(true);
    EXPECT_TRUE(window->isPlaying);
}

TEST_F(MainWindowTest, SetPlayingFalse) {
    window->setPlaying(true);
    window->setPlaying(false);
    EXPECT_FALSE(window->isPlaying);
}

TEST_F(MainWindowTest, RebuildAllOgreViewportsDoesNotCrash)
{
    ASSERT_FALSE(window->mDockWidgetList.isEmpty());
    QSettings settings;
    settings.setValue(ViewportSettingsKeys::fsaaSamples(), 4);
    EXPECT_NO_THROW(window->rebuildAllOgreViewports());
    app->processEvents();
    auto* ow = window->mDockWidgetList.first()->getOgreWidget();
    ASSERT_NE(ow, nullptr);
    EXPECT_NO_THROW(ow->fsaaSamples());
}

TEST_F(MainWindowTest, ModeBarLoadsAndModeChangeUpdatesStatusIndicator)
{
    ASSERT_NE(window->m_modeBarShell, nullptr);
    ASSERT_NE(window->m_modeBar, nullptr);
    ASSERT_NE(window->m_modeBar->rootObject(), nullptr);
    ASSERT_EQ(window->m_modeBar->status(), QQuickWidget::Ready);
    EXPECT_GE(window->m_modeBar->minimumWidth(), 560);
    EXPECT_EQ(window->toolBarArea(window->m_modeBarShell), Qt::TopToolBarArea);
    EXPECT_FALSE(window->m_modeBarShell->isHidden());
    ASSERT_NE(window->m_editModeLabel, nullptr);

    auto* modeController = EditorModeController::instance();

    modeController->requestMode(EditorModeController::AnimationMode);
    app->processEvents();
    EXPECT_EQ(modeController->currentMode(), EditorModeController::AnimationMode);
    EXPECT_EQ(modeController->statusText(), QStringLiteral("Animation mode"));
    EXPECT_EQ(window->m_editModeLabel->text(), QStringLiteral("Animation mode"));

    modeController->requestMode(EditorModeController::MaterialMode);
    app->processEvents();
    EXPECT_EQ(modeController->currentMode(), EditorModeController::MaterialMode);
    EXPECT_EQ(modeController->statusText(), QStringLiteral("Material mode"));
    EXPECT_EQ(window->m_editModeLabel->text(), QStringLiteral("Material mode"));
}

TEST_F(MainWindowTest, ContextualToolRailSwitchesActionsByMode)
{
    QAction* primitiveAction = findActionByObjectName("modeObjectPrimitiveAction");
    QAction* editExtrudeAction = findActionByObjectName("modeEditExtrudeAction");
    QAction* dopeAction = findActionByObjectName("modeAnimationDopeSheetAction");
    QAction* curveAction = findActionByObjectName("modeAnimationCurveEditorAction");
    QAction* validationAction = findActionByObjectName("modeValidationRunAction");
    ASSERT_NE(primitiveAction, nullptr);
    ASSERT_NE(editExtrudeAction, nullptr);
    ASSERT_NE(dopeAction, nullptr);
    ASSERT_NE(curveAction, nullptr);
    ASSERT_NE(validationAction, nullptr);

    auto* modeController = EditorModeController::instance();

    modeController->requestMode(EditorModeController::ObjectMode);
    app->processEvents();
    EXPECT_TRUE(primitiveAction->isVisible());
    EXPECT_FALSE(editExtrudeAction->isVisible());
    EXPECT_FALSE(dopeAction->isVisible());
    EXPECT_FALSE(validationAction->isVisible());

    modeController->requestMode(EditorModeController::AnimationMode);
    app->processEvents();
    EXPECT_FALSE(primitiveAction->isVisible());
    EXPECT_TRUE(dopeAction->isVisible());
    EXPECT_TRUE(curveAction->isVisible());
    EXPECT_FALSE(validationAction->isVisible());

    modeController->requestMode(EditorModeController::MaterialMode);
    app->processEvents();
    EXPECT_FALSE(dopeAction->isVisible());
    EXPECT_FALSE(validationAction->isVisible());
    EXPECT_TRUE(window->ui->actionSelect_Object->isVisible());
    EXPECT_TRUE(window->ui->actionTranslate_Object->isVisible());
    EXPECT_TRUE(window->ui->actionRotate_Object->isVisible());
    EXPECT_TRUE(window->ui->actionScale_Object->isVisible());
    EXPECT_TRUE(window->ui->actionToggle_Transform_Space->isVisible());

    modeController->requestMode(EditorModeController::ValidationMode);
    app->processEvents();
    EXPECT_TRUE(validationAction->isVisible());
    EXPECT_FALSE(validationAction->isEnabled());
}

TEST_F(MainWindowTest, ContextualToolRailKeepsSharedMenuActionsReachable)
{
    auto* modeController = EditorModeController::instance();
    modeController->requestMode(EditorModeController::ValidationMode);
    app->processEvents();

    EXPECT_TRUE(window->ui->actionMaterial_Editor->isVisible());
    EXPECT_TRUE(window->ui->actionMerge_Animations->isVisible());

    // Material Editor and Merge Animations are no longer on the top Tools bar;
    // menu actions remain for discoverability.
    EXPECT_EQ(window->ui->toolToolbar->widgetForAction(window->ui->actionMaterial_Editor),
              nullptr);
    EXPECT_EQ(window->ui->toolToolbar->widgetForAction(window->ui->actionMerge_Animations),
              nullptr);
}

TEST_F(MainWindowTest, ContextualToolRailButtonsTriggerExistingBottomTools)
{
    QAction* dopeAction = findActionByObjectName("modeAnimationDopeSheetAction");
    ASSERT_NE(dopeAction, nullptr);
    auto* dopeButton = qobject_cast<QToolButton*>(window->ui->objectsToolbar->widgetForAction(dopeAction));
    ASSERT_NE(dopeButton, nullptr);
    ASSERT_NE(window->m_dopeSheetDock, nullptr);

    window->m_dopeSheetDock->hide();
    EditorModeController::instance()->requestMode(EditorModeController::AnimationMode);
    app->processEvents();

    dopeButton->click();
    app->processEvents();
    EXPECT_FALSE(window->m_dopeSheetDock->isHidden());
    EXPECT_EQ(window->dockWidgetArea(window->m_dopeSheetDock), Qt::BottomDockWidgetArea);
}

TEST_F(MainWindowTest, BottomContextPanelLoadsAndTracksCurrentMode)
{
    ASSERT_NE(window->m_bottomContextDock, nullptr);
    window->showBottomToolDock(window->m_bottomContextDock);
    app->processEvents();
    EXPECT_FALSE(window->m_bottomContextDock->isHidden());
    auto* quickWidget = qobject_cast<QQuickWidget*>(window->m_bottomContextDock->widget());
    ASSERT_NE(quickWidget, nullptr);
    ASSERT_EQ(quickWidget->status(), QQuickWidget::Ready);
    QObject* root = quickWidget->rootObject();
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(root->property("currentSummaryObjectName").toString(),
              QStringLiteral("objectSummaryRoot"));

    EditorModeController::instance()->requestMode(EditorModeController::AnimationMode);
    app->processEvents();
    EXPECT_EQ(root->property("currentSummaryObjectName").toString(),
              QStringLiteral("animationSummaryRoot"));

    EditorModeController::instance()->requestMode(EditorModeController::MaterialMode);
    app->processEvents();
    EXPECT_EQ(root->property("currentSummaryObjectName").toString(),
              QStringLiteral("materialSummaryRoot"));

    EditorModeController::instance()->requestMode(EditorModeController::ValidationMode);
    app->processEvents();
    EXPECT_EQ(root->property("currentSummaryObjectName").toString(),
              QStringLiteral("validationSummaryRoot"));
}

TEST_F(MainWindowTest, BottomToolRevealTabsContextWithOtherBottomTools)
{
    ASSERT_NE(window->m_bottomContextDock, nullptr);
    ASSERT_NE(window->m_assetBrowserDock, nullptr);
    ASSERT_NE(window->m_dopeSheetDock, nullptr);
    ASSERT_NE(window->m_curveEditorDock, nullptr);
    ASSERT_NE(window->m_consoleDock, nullptr);

    window->show();
    app->processEvents();

    window->revealBottomTool(QStringLiteral("dopeSheet"));
    window->revealBottomTool(QStringLiteral("curveEditor"));
    window->revealBottomTool(QStringLiteral("assetBrowser"));
    window->revealBottomTool(QStringLiteral("console"));
    window->revealBottomTool(QStringLiteral("context"));
    app->processEvents();

    EXPECT_FALSE(window->m_bottomContextDock->isHidden());
    EXPECT_FALSE(window->m_assetBrowserDock->isHidden());
    EXPECT_FALSE(window->m_dopeSheetDock->isHidden());
    EXPECT_FALSE(window->m_curveEditorDock->isHidden());
    EXPECT_FALSE(window->m_consoleDock->isHidden());

    EXPECT_EQ(window->dockWidgetArea(window->m_bottomContextDock), Qt::BottomDockWidgetArea);
    EXPECT_EQ(window->dockWidgetArea(window->m_assetBrowserDock), Qt::BottomDockWidgetArea);
    EXPECT_EQ(window->dockWidgetArea(window->m_dopeSheetDock), Qt::BottomDockWidgetArea);
    EXPECT_EQ(window->dockWidgetArea(window->m_curveEditorDock), Qt::BottomDockWidgetArea);
    EXPECT_EQ(window->dockWidgetArea(window->m_consoleDock), Qt::BottomDockWidgetArea);

    const auto areTabified = [this](QDockWidget* first, QDockWidget* second) {
        return window->tabifiedDockWidgets(first).contains(second)
            || window->tabifiedDockWidgets(second).contains(first);
    };

    EXPECT_TRUE(areTabified(window->m_bottomContextDock, window->m_assetBrowserDock));
    EXPECT_TRUE(areTabified(window->m_bottomContextDock, window->m_dopeSheetDock));
    EXPECT_TRUE(areTabified(window->m_bottomContextDock, window->m_curveEditorDock));
    EXPECT_TRUE(areTabified(window->m_bottomContextDock, window->m_consoleDock));
}

TEST_F(MainWindowTest, BottomToolRevealReturnsDetachedContextDockToBottomArea)
{
    ASSERT_NE(window->m_bottomContextDock, nullptr);
    window->show();
    app->processEvents();

    window->m_bottomContextDock->setFloating(true);
    app->processEvents();
    ASSERT_TRUE(window->m_bottomContextDock->isFloating());

    window->m_bottomContextDock->hide();
    app->processEvents();

    window->revealBottomTool(QStringLiteral("context"));
    app->processEvents();

    EXPECT_FALSE(window->m_bottomContextDock->isFloating());
    EXPECT_FALSE(window->m_bottomContextDock->isHidden());
    EXPECT_EQ(window->dockWidgetArea(window->m_bottomContextDock), Qt::BottomDockWidgetArea);
}

TEST_F(MainWindowTest, ViewportDisplayActionsLiveInViewMenuNotToolbar)
{
    const QList<QAction*> viewMenuActions = window->ui->menuView->actions();
    EXPECT_TRUE(viewMenuActions.contains(window->ui->actionShow_Grid));
    EXPECT_TRUE(viewMenuActions.contains(window->ui->actionShow_Normals));
    EXPECT_TRUE(viewMenuActions.contains(window->ui->actionShow_Mesh_Info));
    EXPECT_TRUE(viewMenuActions.contains(window->ui->actionShow_View_Cube));

    const QList<QAction*> topOptionsActions = window->ui->menuOp_es->actions();
    EXPECT_FALSE(topOptionsActions.contains(window->ui->actionShow_Grid));
    EXPECT_FALSE(topOptionsActions.contains(window->ui->actionShow_Normals));
    EXPECT_FALSE(topOptionsActions.contains(window->ui->actionShow_Mesh_Info));
    EXPECT_FALSE(topOptionsActions.contains(window->ui->actionShow_View_Cube));
}

TEST_F(MainWindowTest, ViewportTitleBarHostsViewportActions)
{
    // The viewport display menu is now embedded in each EditorViewport's
    // custom title bar instead of a free-floating QQuickWidget. Verify the
    // title bar exists, exposes G/N/I/C toolbuttons, and that triggering one
    // propagates to the underlying QAction (and therefore the matching
    // display state in MainWindow).
    ASSERT_FALSE(window->mDockWidgetList.isEmpty());
    EditorViewport* viewport = window->mDockWidgetList.first();
    ASSERT_NE(viewport, nullptr);

    ViewportTitleBar* titleBar = viewport->titleBarWidgetCustom();
    ASSERT_NE(titleBar, nullptr);
    EXPECT_EQ(viewport->titleBarWidget(), titleBar);

    ASSERT_NE(titleBar->gridButton(), nullptr);
    ASSERT_NE(titleBar->normalsButton(), nullptr);
    ASSERT_NE(titleBar->meshInfoButton(), nullptr);
    ASSERT_NE(titleBar->viewCubeButton(), nullptr);
    ASSERT_NE(titleBar->floatButton(), nullptr);
    ASSERT_NE(titleBar->closeButton(), nullptr);

    EXPECT_EQ(titleBar->viewCubeButton()->text(), QStringLiteral("C"));

    // Buttons should mirror the QAction's checked state immediately.
    EXPECT_EQ(titleBar->gridButton()->isChecked(),
              window->ui->actionShow_Grid->isChecked());
    EXPECT_EQ(titleBar->viewCubeButton()->isChecked(),
              window->ui->actionShow_View_Cube->isChecked());

    // The viewport action buttons and recreated dock chrome buttons should
    // share a visible bordered style so they read as one title-bar group.
    EXPECT_TRUE(titleBar->gridButton()->styleSheet().contains(QStringLiteral("border: 1px")));
    EXPECT_TRUE(titleBar->viewCubeButton()->styleSheet().contains(QStringLiteral("border: 1px")));
    EXPECT_TRUE(titleBar->floatButton()->styleSheet().contains(QStringLiteral("border: 1px")));
    EXPECT_TRUE(titleBar->closeButton()->styleSheet().contains(QStringLiteral("border: 1px")));

    window->ui->actionShow_Mesh_Info->setChecked(false);
    titleBar->meshInfoButton()->click();
    app->processEvents();

    EXPECT_TRUE(window->ui->actionShow_Mesh_Info->isChecked());
    ASSERT_NE(window->m_meshInfoOverlay, nullptr);
    EXPECT_TRUE(window->m_meshInfoOverlay->isVisible());
    EXPECT_TRUE(titleBar->meshInfoButton()->isChecked());
}

// ---- Cycle all transform states via keyboard ----

TEST_F(MainWindowTest, CycleAllStatesViaKeyboard) {
    QKeyEvent selectEvent(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
    QKeyEvent translateEvent(QEvent::KeyPress, Qt::Key_W, Qt::NoModifier);
    QKeyEvent rotateEvent(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
    QKeyEvent scaleEvent(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    QKeyEvent selectAgainEvent(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier);
    window->keyPressEvent(&selectEvent);
    window->keyPressEvent(&translateEvent);
    window->keyPressEvent(&rotateEvent);
    window->keyPressEvent(&scaleEvent);
    window->keyPressEvent(&selectAgainEvent);
    // Should not crash
}

// ---- Unmapped key ----

TEST_F(MainWindowTest, UnmappedKeyDoesNotCrash) {
    QKeyEvent firstEvent(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier);
    QKeyEvent secondEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    window->keyPressEvent(&firstEvent);
    window->keyPressEvent(&secondEvent);
}

// ---- importMeshs with empty list ----

TEST_F(MainWindowTest, ImportMeshsEmptyList) {
    EXPECT_NO_THROW(window->importMeshs(QStringList()));
}

// ---- Drop event ----

TEST_F(MainWindowTest, DropEventNoValidFiles) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/nonexistent/file.txt")});
    QDropEvent event(QPointF(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    EXPECT_NO_THROW(window->dropEvent(&event));
    delete mimeData;
}

TEST_F(MainWindowTest, DropEventWithFbxFile) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({QUrl::fromLocalFile("/some/model.fbx")});
    QDropEvent event(QPointF(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);
    EXPECT_NO_THROW(window->dropEvent(&event));
    delete mimeData;
}

TEST_F(MainWindowTest, DragEnterEventAcceptsProposedAction) {
    auto* mimeData = new QMimeData();
    QDragEnterEvent event(QPoint(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);

    window->dragEnterEvent(&event);

    EXPECT_TRUE(event.isAccepted());
    delete mimeData;
}

TEST_F(MainWindowTest, DropEventKeepsOnlySupportedFiles) {
    auto* mimeData = new QMimeData();
    mimeData->setUrls({
        QUrl::fromLocalFile("/tmp/first.mesh"),
        QUrl::fromLocalFile("/tmp/second.obj"),
        QUrl::fromLocalFile("/tmp/ignore.unsupported")
    });
    QDropEvent event(QPointF(0, 0), Qt::CopyAction, mimeData, Qt::LeftButton, Qt::NoModifier);

    window->dropEvent(&event);

    EXPECT_EQ(window->mUriList.size(), 2);
    EXPECT_TRUE(window->mUriList.contains("/tmp/first.mesh"));
    EXPECT_TRUE(window->mUriList.contains("/tmp/second.obj"));
    EXPECT_FALSE(window->mUriList.contains("/tmp/ignore.unsupported"));
    delete mimeData;
}

TEST_F(MainWindowTest, RecentFilesMenuShowsPlaceholderWhenEmpty) {
    window->updateRecentFilesMenu();

    const QList<QAction*> actions = window->m_recentFilesMenu->actions();
    ASSERT_GE(actions.size(), 2);
    EXPECT_EQ(actions.first()->text(), "(No Recent Files)");
    EXPECT_FALSE(actions.first()->isEnabled());
    EXPECT_EQ(actions.last()->text(), "Clear Recent Files");
}

TEST_F(MainWindowTest, AddToRecentFilesDeduplicatesAndLimitsToTenEntries) {
    QStringList expected;
    for (int i = 0; i < 12; ++i) {
        const QString path = QString("/tmp/file_%1.mesh").arg(i);
        window->addToRecentFiles(path);
        expected.prepend(path);
        while (expected.size() > 10) {
            expected.removeLast();
        }
    }

    window->addToRecentFiles("/tmp/file_5.mesh");
    expected.removeAll("/tmp/file_5.mesh");
    expected.prepend("/tmp/file_5.mesh");

    const QStringList files = QSettings().value("RecentFiles/files").toStringList();
    ASSERT_EQ(files.size(), 10);
    EXPECT_EQ(files, expected);
    EXPECT_EQ(window->m_recentFilesMenu->actions().first()->data().toString(), "/tmp/file_5.mesh");
}

TEST_F(MainWindowTest, RecentFilesMenuUsesFileNameAsLabelAndFullPathAsTooltip) {
    const QString path = "/tmp/assets/character.mesh";
    window->addToRecentFiles(path);

    QAction* action = recentFileAction(0);
    ASSERT_NE(action, nullptr);
    EXPECT_EQ(action->text(), "character.mesh");
    EXPECT_EQ(action->toolTip(), path);
    EXPECT_EQ(action->data().toString(), path);
}

TEST_F(MainWindowTest, ClearRecentFilesActionRemovesStoredEntries) {
    window->addToRecentFiles("/tmp/a.mesh");
    window->addToRecentFiles("/tmp/b.mesh");

    QAction* clearAction = window->m_recentFilesMenu->actions().last();
    ASSERT_NE(clearAction, nullptr);
    ASSERT_EQ(clearAction->text(), "Clear Recent Files");

    clearAction->trigger();

    EXPECT_TRUE(QSettings().value("RecentFiles/files").toStringList().isEmpty());
    EXPECT_EQ(window->m_recentFilesMenu->actions().first()->text(), "(No Recent Files)");
}

TEST_F(MainWindowTest, OpenRecentFileQueuesExistingMeshPath) {
    ASSERT_TRUE(tempDir.isValid());
    const QString meshPath = tempDir.filePath("existing.mesh");
    QFile file(meshPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mesh");
    file.close();

    window->addToRecentFiles(meshPath);
    QAction* action = recentFileAction(0);
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(action->data().toString(), meshPath);

    action->trigger();

    EXPECT_TRUE(window->mUriList.contains(meshPath));
    EXPECT_EQ(QSettings().value("RecentFiles/files").toStringList().first(), meshPath);
}

TEST_F(MainWindowTest, OpenRecentFileWithoutActionSenderDoesNothing) {
    window->mUriList.clear();

    window->openRecentFile();

    EXPECT_TRUE(window->mUriList.isEmpty());
}

TEST_F(MainWindowTest, OpenRecentFileRemovesMissingPathFromSettings) {
    const QString missingPath = tempDir.filePath("missing.mesh");
    window->addToRecentFiles(missingPath);
    QAction* action = recentFileAction(0);
    ASSERT_NE(action, nullptr);
    ASSERT_EQ(action->data().toString(), missingPath);

    // Auto-close the warning QMessageBox shown by openRecentFile().
    QTimer::singleShot(0, []() {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(w)) {
                box->accept();
            }
        }
    });

    action->trigger();

    const QStringList files = QSettings().value("RecentFiles/files").toStringList();
    EXPECT_FALSE(files.contains(missingPath));
    EXPECT_FALSE(window->mUriList.contains(missingPath));
}

TEST_F(MainWindowTest, ToolbarTogglesUpdateWidgetVisibility) {
    window->on_actionObjects_Toolbar_toggled(false);
    EXPECT_TRUE(window->ui->objectsToolbar->isHidden());
    window->on_actionObjects_Toolbar_toggled(true);
    EXPECT_FALSE(window->ui->objectsToolbar->isHidden());

    window->on_actionTools_Toolbar_toggled(false);
    EXPECT_TRUE(window->ui->toolToolbar->isHidden());
    window->on_actionTools_Toolbar_toggled(true);
    EXPECT_FALSE(window->ui->toolToolbar->isHidden());

    window->on_actionMeshEditor_toggled(false);
    EXPECT_TRUE(window->ui->meshEditorWidget->isHidden());
    window->on_actionMeshEditor_toggled(true);
    EXPECT_FALSE(window->ui->meshEditorWidget->isHidden());
}

TEST_F(MainWindowTest, LightPaletteToggleUpdatesSettingsAndActions) {
    window->ui->actionDark->setChecked(true);
    window->ui->actionLight->setChecked(true);
    app->processEvents();

    EXPECT_EQ(QSettings().value("palette").toString(), "light");
    EXPECT_TRUE(window->ui->actionLight->isChecked());
    EXPECT_FALSE(window->ui->actionDark->isChecked());
    EXPECT_FALSE(window->ui->actionCustom->isChecked());
}

TEST_F(MainWindowTest, DarkPaletteToggleUpdatesSettingsAndActions) {
    window->ui->actionLight->setChecked(true);
    window->ui->actionDark->setChecked(true);
    app->processEvents();

    EXPECT_EQ(QSettings().value("palette").toString(), "dark");
    EXPECT_TRUE(window->ui->actionDark->isChecked());
    EXPECT_FALSE(window->ui->actionLight->isChecked());
    EXPECT_FALSE(window->ui->actionCustom->isChecked());
}

TEST_F(MainWindowTest, PaletteActionsCannotAllBecomeUnchecked) {
    window->ui->actionLight->setChecked(true);
    window->ui->actionDark->setChecked(false);
    window->ui->actionCustom->setChecked(false);
    window->on_actionLight_toggled(false);
    EXPECT_TRUE(window->ui->actionLight->isChecked());

    window->ui->actionDark->setChecked(true);
    window->ui->actionLight->setChecked(false);
    window->ui->actionCustom->setChecked(false);
    window->on_actionDark_toggled(false);
    EXPECT_TRUE(window->ui->actionDark->isChecked());
}

TEST_F(MainWindowTest, DarkPaletteReturnsExpectedWindowColor) {
    const QPalette& palette = window->darkPalette();
    EXPECT_EQ(palette.color(QPalette::Window), QColor(37, 37, 37));
    EXPECT_EQ(palette.color(QPalette::Highlight), QColor(38, 79, 120));
}

TEST_F(MainWindowTest, SetTransformStateUpdatesActionsAndOperator) {
    window->setTransformState(TransformOperator::TS_ROTATE);
    EXPECT_TRUE(window->ui->actionRotate_Object->isChecked());
    EXPECT_FALSE(window->ui->actionSelect_Object->isChecked());

    window->setTransformState(TransformOperator::TS_SCALE);
    EXPECT_TRUE(window->ui->actionScale_Object->isChecked());
    EXPECT_FALSE(window->ui->actionRotate_Object->isChecked());
}

TEST_F(MainWindowTest, SetMCPServerReplacesExistingServer) {
    auto* original = new MCPServer();
    auto* replacement = new MCPServer();
    window->setMCPServer(original);

    window->setMCPServer(replacement);

    EXPECT_EQ(window->m_mcpServer, replacement);
}

TEST_F(MainWindowTest, SetMCPServerNullClearsExistingServer) {
    auto* original = new MCPServer();
    window->setMCPServer(original);

    window->setMCPServer(nullptr);

    EXPECT_EQ(window->m_mcpServer, nullptr);
}

TEST_F(MainWindowTest, StartAndStopMCPServerPersistSettings) {
    constexpr int requestedPort = 0;
    ASSERT_TRUE(window->startMCPServer(requestedPort));
    EXPECT_NE(window->m_mcpServer, nullptr);
    EXPECT_TRUE(window->m_mcpServer->isHttpRunning());
    EXPECT_TRUE(QSettings().value("MCP/enabled").toBool());

    EXPECT_GT(window->m_mcpServer->httpPort(), 0);
    EXPECT_EQ(QSettings().value("MCP/port").toInt(), requestedPort);

    window->stopMCPServer();

    EXPECT_FALSE(window->m_mcpServer->isHttpRunning());
    EXPECT_FALSE(QSettings().value("MCP/enabled").toBool());
}

TEST_F(MainWindowTest, StartMCPServerIsIdempotentWhenAlreadyRunning) {
    ASSERT_TRUE(window->startMCPServer(0));
    auto* server = window->m_mcpServer;
    ASSERT_NE(server, nullptr);
    const int actualPort = server->httpPort();

    EXPECT_TRUE(window->startMCPServer(12345));
    EXPECT_EQ(window->m_mcpServer, server);
    EXPECT_EQ(window->m_mcpServer->httpPort(), actualPort);
}

TEST_F(MainWindowTest, StopMCPServerWithoutInstanceStillClearsSettings) {
    delete window->m_mcpServer;
    window->m_mcpServer = nullptr;

    QSettings().setValue("MCP/enabled", true);
    window->stopMCPServer();

    EXPECT_FALSE(QSettings().value("MCP/enabled").toBool());
}

TEST_F(MainWindowTest, ConstructorAutostartsMCPServerWhenEnabledInSettings)
{
    delete window;
    window = nullptr;

    QSettings settings;
    settings.setValue("MCP/enabled", true);
    settings.setValue("MCP/port", 0);

    try {
        window = new MainWindow();
    } catch (const std::exception& e) {
        FAIL() << "MainWindow reconstruction failed: " << e.what();
    } catch (...) {
        FAIL() << "MainWindow reconstruction failed with unknown exception";
    }

    ASSERT_NE(window, nullptr);
    ASSERT_NE(window->m_mcpServer, nullptr);
    EXPECT_TRUE(window->m_mcpServer->isHttpRunning());
    EXPECT_GT(window->m_mcpServer->httpPort(), 0);
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonDisablesActionWhenFewerThanTwoNodeOrEntityPicks) {
    window->ui->actionMerge_Animations->setEnabled(true);
    SelectionSet::getSingleton()->clear();

    window->updateMergeAnimationsButton();

    EXPECT_FALSE(window->ui->actionMerge_Animations->isEnabled());
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonEnablesActionForTwoSceneNodes) {
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);
    Ogre::SceneNode* nodeA = manager->addSceneNode("MergeEnableNodeA");
    Ogre::SceneNode* nodeB = manager->addSceneNode("MergeEnableNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);

    window->updateMergeAnimationsButton();

    EXPECT_TRUE(window->ui->actionMerge_Animations->isEnabled());
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonDisablesActionForSingleSkeletonEntity) {
    Ogre::Entity* entity = createAnimatedEntity("SingleSkeletonEntity");
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(entity);

    window->updateMergeAnimationsButton();

    EXPECT_FALSE(window->ui->actionMerge_Animations->isEnabled());
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonEnablesActionForCompatibleSkeletonEntities) {
    Ogre::Entity* entityA = createAnimatedEntity("MergeSkeletonEntityA");
    Ogre::Entity* entityB = createAnimatedEntity("MergeSkeletonEntityB");
    ASSERT_NE(entityA, nullptr);
    ASSERT_NE(entityB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(entityA);
    SelectionSet::getSingleton()->append(entityB);

    window->updateMergeAnimationsButton();

    EXPECT_TRUE(window->ui->actionMerge_Animations->isEnabled());
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonResolvesEntitiesFromSelectedNodes) {
    Ogre::Entity* entityA = createAnimatedEntity("MergeNodeEntityA");
    Ogre::Entity* entityB = createAnimatedEntity("MergeNodeEntityB");
    ASSERT_NE(entityA, nullptr);
    ASSERT_NE(entityB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(entityA->getParentSceneNode());
    SelectionSet::getSingleton()->append(entityB->getParentSceneNode());

    window->updateMergeAnimationsButton();

    EXPECT_TRUE(window->ui->actionMerge_Animations->isEnabled());
}

TEST_F(MainWindowTest, KeyPCyclesPivotMode) {
    auto* transformOperator = TransformOperator::getSingleton();
    ASSERT_NE(transformOperator, nullptr);
    const auto before = transformOperator->pivotMode();

    QKeyEvent event(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    window->keyPressEvent(&event);

    EXPECT_NE(transformOperator->pivotMode(), before);
}

TEST_F(MainWindowTest, GroupActionsEnablementFollowsSelectionState) {
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    Ogre::SceneNode* nodeA = manager->addSceneNode("GroupEnableNodeA");
    Ogre::SceneNode* nodeB = manager->addSceneNode("GroupEnableNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    app->processEvents();
    EXPECT_FALSE(window->ui->actionGroup->isEnabled());
    EXPECT_FALSE(window->ui->actionUngroup->isEnabled());

    SelectionSet::getSingleton()->append(nodeA);
    app->processEvents();
    EXPECT_FALSE(window->ui->actionGroup->isEnabled());
    EXPECT_FALSE(window->ui->actionUngroup->isEnabled());

    SelectionSet::getSingleton()->append(nodeB);
    app->processEvents();
    EXPECT_TRUE(window->ui->actionGroup->isEnabled());
    EXPECT_FALSE(window->ui->actionUngroup->isEnabled());
}

TEST_F(MainWindowTest, GroupThenUngroupSelectedNodesUpdatesSelectionAndActions) {
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    Ogre::SceneNode* nodeA = manager->addSceneNode("GroupFlowNodeA");
    Ogre::SceneNode* nodeB = manager->addSceneNode("GroupFlowNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);
    app->processEvents();
    ASSERT_EQ(SelectionSet::getSingleton()->getNodesCount(), 2);

    window->groupSelected();
    app->processEvents();

    ASSERT_EQ(SelectionSet::getSingleton()->getNodesCount(), 1);
    Ogre::SceneNode* groupNode = SelectionSet::getSingleton()->getSceneNode(0);
    ASSERT_NE(groupNode, nullptr);
    EXPECT_TRUE(manager->isGroupNode(groupNode));
    EXPECT_TRUE(window->ui->actionUngroup->isEnabled());

    window->ungroupSelected();
    app->processEvents();

    EXPECT_EQ(SelectionSet::getSingleton()->getNodesCount(), 2);
    EXPECT_TRUE(window->ui->actionGroup->isEnabled());
    EXPECT_FALSE(window->ui->actionUngroup->isEnabled());
}

TEST_F(MainWindowTest, UngroupSelectedIgnoresNonGroupNode) {
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    Ogre::SceneNode* node = manager->addSceneNode("UngroupNoopNode");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(manager->isGroupNode(node));

    SelectionSet::getSingleton()->selectOne(node);
    app->processEvents();
    EXPECT_FALSE(window->ui->actionUngroup->isEnabled());

    window->ungroupSelected();

    EXPECT_TRUE(manager->hasSceneNode("UngroupNoopNode"));
    EXPECT_EQ(SelectionSet::getSingleton()->getNodesCount(), 1);
}
TEST_F(MainWindowTest, EditModeKeyboardShortcutsCoverModeAndTopologyPaths)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "mesh loading requires GL (Xvfb in CI)";

    const std::string meshName = "mainwindow_shortcuts_mesh";
    auto mesh = createInMemoryTriangleMesh(meshName);
    auto* node = Manager::getSingleton()->addSceneNode("mainwindow_shortcuts_node");
    ASSERT_NE(node, nullptr);
    auto* entity = Manager::getSingleton()->createEntity(node, mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(node);

    auto* ctrl = EditModeController::instance();
    ASSERT_TRUE(ctrl->enterEditMode());
    ASSERT_TRUE(ctrl->isEditModeActive());

    QKeyEvent key1(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier);
    window->keyPressEvent(&key1);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::VertexMode);

    QKeyEvent key2(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier);
    window->keyPressEvent(&key2);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::EdgeMode);

    QKeyEvent key3(QEvent::KeyPress, Qt::Key_3, Qt::NoModifier);
    window->keyPressEvent(&key3);
    EXPECT_EQ(ctrl->selectionMode(), EditModeController::FaceMode);

    ctrl->selectFace(0, false);
    QKeyEvent ctrlE(QEvent::KeyPress, Qt::Key_E, Qt::ControlModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&ctrlE));

    ctrl->setSelectionMode(EditModeController::EdgeMode);
    ctrl->selectEdge(0, 1, false);
    QKeyEvent ctrlB(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier);
    EXPECT_NO_THROW(window->keyPressEvent(&ctrlB));

    ctrl->setSelectionMode(EditModeController::VertexMode);
    ctrl->deselectAll();
    QKeyEvent ctrlA(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    window->keyPressEvent(&ctrlA);
    EXPECT_GT(ctrl->selectedVertexCount(), 0);

    QKeyEvent altA(QEvent::KeyPress, Qt::Key_A, Qt::AltModifier);
    window->keyPressEvent(&altA);
    EXPECT_EQ(ctrl->selectedVertexCount(), 0);

    QKeyEvent tabKey(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    window->keyPressEvent(&tabKey);
    EXPECT_FALSE(ctrl->isEditModeActive());

    if (ctrl->isEditModeActive()) {
        ctrl->exitEditMode(false);
    }
    SelectionSet::getSingleton()->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(MainWindowTest, TriggeringDynamicHelpAndPreferencesActionsCreatesDialogs)
{
    QAction* shortcutsAction = findActionByObjectName("actionKeyboardShortcuts");
    ASSERT_NE(shortcutsAction, nullptr);
    shortcutsAction->trigger();
    app->processEvents();

    bool foundShortcutsDialog = false;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* quick = qobject_cast<QQuickWidget*>(w);
            quick && quick->source().toString().contains("ShortcutReference.qml")) {
            foundShortcutsDialog = true;
            quick->close();
        }
    }
    EXPECT_TRUE(foundShortcutsDialog);

    window->ui->actionPreferences->trigger();
    app->processEvents();

    bool foundPreferencesDialog = false;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* quick = qobject_cast<QQuickWidget*>(w);
            quick && quick->source().toString().contains("PreferencesDialog.qml")) {
            foundPreferencesDialog = true;
            quick->close();
        }
    }
    EXPECT_TRUE(foundPreferencesDialog);
}

TEST_F(MainWindowTest, AiChatToolbarButtonAndMenuActionRevealDock)
{
    ASSERT_NE(window->m_chatDock, nullptr);

    window->show();
    app->processEvents();

    window->m_chatDock->hide();
    app->processEvents();
    EXPECT_TRUE(window->m_chatDock->isHidden());

    QAction* aiChatAction = findActionByObjectName("actionAIChatDock");
    ASSERT_NE(aiChatAction, nullptr);
    aiChatAction->trigger();
    app->processEvents();
    EXPECT_FALSE(window->m_chatDock->isHidden());

    window->m_chatDock->hide();
    app->processEvents();
    EXPECT_TRUE(window->m_chatDock->isHidden());

    QToolButton* aiButton = window->findChild<QToolButton*>("aiChatToolbarButton");
    ASSERT_NE(aiButton, nullptr);
    aiButton->click();
    app->processEvents();
    EXPECT_FALSE(window->m_chatDock->isHidden());
}

TEST_F(MainWindowTest, AssetBrowserMenuActionTracksDockVisibility)
{
    ASSERT_NE(window->m_assetBrowserDock, nullptr);

    window->show();
    app->processEvents();

    window->m_assetBrowserDock->hide();
    app->processEvents();
    EXPECT_TRUE(window->m_assetBrowserDock->isHidden());

    window->ui->actionAsset_Browser->setChecked(true);
    app->processEvents();
    EXPECT_TRUE(window->ui->actionAsset_Browser->isChecked());
    EXPECT_FALSE(window->m_assetBrowserDock->isHidden());

    window->ui->actionAsset_Browser->setChecked(false);
    app->processEvents();
    EXPECT_FALSE(window->ui->actionAsset_Browser->isChecked());
    EXPECT_TRUE(window->m_assetBrowserDock->isHidden());
}

TEST_F(MainWindowTest, BottomToolDockRedocksAndUsesDefaultDockedHeight)
{
    ASSERT_NE(window->m_assetBrowserDock, nullptr);
    ASSERT_NE(window->m_assetBrowserDock->widget(), nullptr);

    window->show();
    app->processEvents();

    window->showBottomToolDock(window->m_assetBrowserDock);
    app->processEvents();

    EXPECT_FALSE(window->m_assetBrowserDock->isFloating());
    EXPECT_EQ(window->dockWidgetArea(window->m_assetBrowserDock), Qt::BottomDockWidgetArea);
    EXPECT_FALSE(window->m_assetBrowserDock->isHidden());
    EXPECT_EQ(window->m_assetBrowserDock->widget()->maximumHeight(),
              MainWindow::kDefaultDockedHeight);

    window->m_assetBrowserDock->setFloating(true);
    app->processEvents();
    EXPECT_TRUE(window->m_assetBrowserDock->isFloating());

    window->m_assetBrowserDock->hide();
    app->processEvents();

    window->showBottomToolDock(window->m_assetBrowserDock);
    app->processEvents();

    EXPECT_FALSE(window->m_assetBrowserDock->isFloating());
    EXPECT_EQ(window->dockWidgetArea(window->m_assetBrowserDock), Qt::BottomDockWidgetArea);
    EXPECT_FALSE(window->m_assetBrowserDock->isHidden());
    EXPECT_LT(window->m_assetBrowserDock->maximumHeight(), QWIDGETSIZE_MAX);
    EXPECT_EQ(window->m_assetBrowserDock->widget()->maximumHeight(),
              MainWindow::kDefaultDockedHeight);
}

TEST_F(MainWindowTest, LoadFileQueuesNonSceneFileAndTracksRecentFiles)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString meshPath = tempDir.filePath("load_file.mesh");
    QFile file(meshPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mesh");
    file.close();

    window->loadFile(meshPath);

    EXPECT_TRUE(window->mUriList.contains(meshPath));
    EXPECT_TRUE(QSettings().value("RecentFiles/files").toStringList().contains(meshPath));
}

TEST_F(MainWindowTest, LoadFileScenePathUsesSceneImporterBranchWithoutQueueing)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("scene_only.scene.gltf");
    QFile file(scenePath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{\"asset\":{\"version\":\"2.0\"}}");
    file.close();

    window->mUriList.clear();
    window->loadFile(scenePath);

    EXPECT_FALSE(window->mUriList.contains(scenePath));
    EXPECT_TRUE(QSettings().value("RecentFiles/files").toStringList().contains(scenePath));
}

TEST_F(MainWindowTest, FrameEndedStatusMessageCoversSelectionKindsAndTransformSpace)
{
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    auto mesh = createInMemoryTriangleMesh("mainwindow_frameended_mesh");
    auto* node = manager->addSceneNode("mainwindow_frameended_node");
    ASSERT_NE(node, nullptr);
    auto* entity = manager->createEntity(node, mesh);
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    Ogre::FrameEvent evt{};
    evt.timeSinceLastFrame = 0.016f;
    evt.timeSinceLastEvent = 0.016f;

    TransformOperator::getSingleton()->setTransformSpace(TransformOperator::SPACE_WORLD);
    SelectionSet::getSingleton()->selectOne(node);
    EXPECT_TRUE(window->frameEnded(evt));
    EXPECT_TRUE(window->ui->statusBar->currentMessage().startsWith("World | Nodes: 1"));

    SelectionSet::getSingleton()->selectOne(entity);
    EXPECT_TRUE(window->frameEnded(evt));
    EXPECT_TRUE(window->ui->statusBar->currentMessage().startsWith("World | Entities: 1"));

    SelectionSet::getSingleton()->selectOne(entity->getSubEntity(0));
    EXPECT_TRUE(window->frameEnded(evt));
    EXPECT_TRUE(window->ui->statusBar->currentMessage().startsWith("World | Submeshes: 1"));

    TransformOperator::getSingleton()->setTransformSpace(TransformOperator::SPACE_LOCAL);
    SelectionSet::getSingleton()->clear();
    EXPECT_TRUE(window->frameEnded(evt));
    EXPECT_TRUE(window->ui->statusBar->currentMessage().startsWith("Local | No selection"));
}

TEST_F(MainWindowTest, FrameRenderingQueuedAdvancesEnabledAnimationWhenPlaying)
{
    Ogre::Entity* entity = createAnimatedEntity("mainwindow_frame_anim_entity");
    ASSERT_NE(entity, nullptr);

    Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    ASSERT_NE(states, nullptr);
    ASSERT_FALSE(states->getAnimationStates().empty());
    Ogre::AnimationState* state = states->getAnimationStates().begin()->second;
    ASSERT_NE(state, nullptr);

    state->setEnabled(true);
    state->setTimePosition(0.0f);

    Ogre::FrameEvent evt{};
    evt.timeSinceLastFrame = 0.2f;
    evt.timeSinceLastEvent = 0.2f;

    window->setPlaying(true);
    EXPECT_TRUE(window->frameRenderingQueued(evt));
    const float advanced = state->getTimePosition();
    EXPECT_GT(advanced, 0.0f);

    window->setPlaying(false);
    EXPECT_TRUE(window->frameRenderingQueued(evt));
    EXPECT_FLOAT_EQ(state->getTimePosition(), advanced);
}

TEST_F(MainWindowTest, DuplicateSelectedReplacesSelectionWithClonedNodes)
{
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    auto meshA = createInMemoryTriangleMesh("mainwindow_dup_mesh_a");
    auto meshB = createInMemoryTriangleMesh("mainwindow_dup_mesh_b");
    auto* nodeA = manager->addSceneNode("mainwindow_dup_node_a");
    auto* nodeB = manager->addSceneNode("mainwindow_dup_node_b");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);
    ASSERT_NE(manager->createEntity(nodeA, meshA), nullptr);
    ASSERT_NE(manager->createEntity(nodeB, meshB), nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);
    ASSERT_EQ(SelectionSet::getSingleton()->getNodesCount(), 2);

    window->duplicateSelected();

    const QList<Ogre::SceneNode*> selected = SelectionSet::getSingleton()->getNodesSelectionList();
    ASSERT_EQ(selected.size(), 2);
    EXPECT_NE(selected[0], nodeA);
    EXPECT_NE(selected[0], nodeB);
    EXPECT_NE(selected[1], nodeA);
    EXPECT_NE(selected[1], nodeB);
    EXPECT_GE(manager->getSceneNodes().size(), 4);
}

TEST_F(MainWindowTest, ConstructorAppliesCustomPaletteFromSettings)
{
    delete window;
    window = nullptr;

    QSettings settings;
    settings.setValue("palette", "custom");
    settings.setValue("customPalette", QColor(12, 34, 56));

    try {
        window = new MainWindow();
    } catch (const std::exception& e) {
        FAIL() << "MainWindow reconstruction failed: " << e.what();
    } catch (...) {
        FAIL() << "MainWindow reconstruction failed with unknown exception";
    }
    ASSERT_NE(window, nullptr);

    EXPECT_TRUE(window->ui->actionCustom->isChecked());
    EXPECT_FALSE(window->ui->actionLight->isChecked());
    EXPECT_FALSE(window->ui->actionDark->isChecked());
    EXPECT_EQ(QSettings().value("palette").toString(), "custom");
}

TEST_F(MainWindowTest, CrashReportMenuToggleToEnabledShowsConfirmationPath)
{
    QAction* crashAction = findActionByObjectName("actionCrashReports");
    ASSERT_NE(crashAction, nullptr);
    ASSERT_TRUE(crashAction->isCheckable());

    const bool oldEnabled = SentryReporter::isEnabled();
    SentryReporter::setEnabled(false);
    crashAction->setChecked(false);

    closeAnyOpenMessageBoxesSoon();
    crashAction->trigger();
    app->processEvents();

    EXPECT_TRUE(crashAction->isChecked());
    SentryReporter::setEnabled(oldEnabled);
}
