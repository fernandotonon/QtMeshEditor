#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

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
#include "TransformOperator.h"
#include "TestHelpers.h"
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

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        try {
            window = new MainWindow();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Skipping: MainWindow construction failed in this environment: " << e.what();
        } catch (...) {
            GTEST_SKIP() << "Skipping: MainWindow construction failed in this environment";
        }

        ASSERT_NE(window, nullptr);
    }

    void TearDown() override {
        delete window;
        window = nullptr;
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
};

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

TEST_F(MainWindowTest, ToolbarTogglesUpdateWidgetVisibility) {
    window->on_actionObjects_Toolbar_toggled(false);
    EXPECT_TRUE(window->ui->objectsToolbar->isHidden());
    window->on_actionObjects_Toolbar_toggled(true);
    EXPECT_FALSE(window->ui->objectsToolbar->isHidden());

    window->on_actionTools_Toolbar_toggled(false);
    EXPECT_TRUE(window->ui->toolToolbar->isHidden());
    window->on_actionTools_Toolbar_toggled(true);
    EXPECT_FALSE(window->ui->toolToolbar->isHidden());

    window->on_actionView_Toolbar_toggled(false);
    EXPECT_TRUE(window->ui->viewToolbar->isHidden());
    window->on_actionView_Toolbar_toggled(true);
    EXPECT_FALSE(window->ui->viewToolbar->isHidden());

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
        GTEST_SKIP() << "Skipping: MainWindow reconstruction failed in this environment: " << e.what();
    } catch (...) {
        GTEST_SKIP() << "Skipping: MainWindow reconstruction failed in this environment";
    }

    ASSERT_NE(window, nullptr);
    ASSERT_NE(window->m_mcpServer, nullptr);
    EXPECT_TRUE(window->m_mcpServer->isHttpRunning());
    EXPECT_GT(window->m_mcpServer->httpPort(), 0);
}

TEST_F(MainWindowTest, UpdateMergeAnimationsButtonDisablesActionWhenSelectionHasNoSkeletons) {
    window->ui->actionMerge_Animations->setEnabled(true);
    SelectionSet::getSingleton()->clear();

    window->updateMergeAnimationsButton();

    EXPECT_FALSE(window->ui->actionMerge_Animations->isEnabled());
}
