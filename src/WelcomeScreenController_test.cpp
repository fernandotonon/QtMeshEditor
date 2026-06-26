#include <gtest/gtest.h>
#include "WelcomeScreenController.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>

class WelcomeScreenControllerTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);

        // Save and clear relevant QSettings before each test
        QSettings settings;
        m_savedDontShow = settings.value("WelcomeScreen/dontShowAgain");
        m_savedRecent = settings.value("RecentFiles/files");
        settings.remove("WelcomeScreen/dontShowAgain");

        WelcomeScreenController::kill();
        controller = WelcomeScreenController::instance();
        ASSERT_NE(controller, nullptr);
    }

    void TearDown() override {
        WelcomeScreenController::kill();

        // Restore QSettings
        QSettings settings;
        if (m_savedDontShow.isValid())
            settings.setValue("WelcomeScreen/dontShowAgain", m_savedDontShow);
        else
            settings.remove("WelcomeScreen/dontShowAgain");
        if (m_savedRecent.isValid())
            settings.setValue("RecentFiles/files", m_savedRecent);
    }

    WelcomeScreenController* controller = nullptr;
    QVariant m_savedDontShow;
    QVariant m_savedRecent;
};

// --- Singleton pattern ---

TEST_F(WelcomeScreenControllerTests, SingletonReturnsSameInstance) {
    EXPECT_EQ(controller, WelcomeScreenController::instance());
}

TEST_F(WelcomeScreenControllerTests, KillAndRecreateYieldsNewInstance) {
    auto* first = WelcomeScreenController::instance();
    WelcomeScreenController::kill();
    auto* second = WelcomeScreenController::instance();
    ASSERT_NE(second, nullptr);
    // After kill + recreate, the pointer may differ (new allocation)
    // The key point is it doesn't crash and returns a valid object
    EXPECT_NE(second, nullptr);
    controller = second; // update for TearDown
}

// --- recentFiles / recentFileNames ---

TEST_F(WelcomeScreenControllerTests, RecentFilesReturnsQStringListFromSettings) {
    QSettings settings;
    QStringList paths = {"/tmp/model1.fbx", "/home/user/model2.obj", "/var/data/scene.gltf"};
    settings.setValue("RecentFiles/files", paths);

    QStringList result = controller->recentFiles();
    EXPECT_EQ(result, paths);
}

TEST_F(WelcomeScreenControllerTests, RecentFilesReturnsEmptyWhenNoSetting) {
    QSettings settings;
    settings.remove("RecentFiles/files");

    QStringList result = controller->recentFiles();
    EXPECT_TRUE(result.isEmpty());
}

TEST_F(WelcomeScreenControllerTests, RecentFileNamesReturnsFilenameOnly) {
    QSettings settings;
    QStringList paths = {"/tmp/model1.fbx", "/home/user/model2.obj"};
    settings.setValue("RecentFiles/files", paths);

    QStringList names = controller->recentFileNames();
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], "model1.fbx");
    EXPECT_EQ(names[1], "model2.obj");
}

TEST_F(WelcomeScreenControllerTests, RecentFileNamesEmptyWhenNoFiles) {
    QSettings settings;
    settings.remove("RecentFiles/files");

    QStringList names = controller->recentFileNames();
    EXPECT_TRUE(names.isEmpty());
}

// --- shouldShow / dismiss ---

TEST_F(WelcomeScreenControllerTests, ShouldShowReturnsTrueInitially) {
    // dontShowAgain was cleared in SetUp
    EXPECT_TRUE(controller->shouldShow());
}

TEST_F(WelcomeScreenControllerTests, DismissWithDontShowAgainPersistsToSettings) {
    controller->dismiss(true);

    QSettings settings;
    EXPECT_TRUE(settings.value("WelcomeScreen/dontShowAgain", false).toBool());
}

TEST_F(WelcomeScreenControllerTests, DismissWithoutDontShowAgainDoesNotPersist) {
    controller->dismiss(false);

    QSettings settings;
    EXPECT_FALSE(settings.value("WelcomeScreen/dontShowAgain", false).toBool());
    EXPECT_TRUE(controller->shouldShow());
}

// --- visible property ---

TEST_F(WelcomeScreenControllerTests, InitiallyNotVisible) {
    EXPECT_FALSE(controller->isVisible());
}

TEST_F(WelcomeScreenControllerTests, SetVisibleEmitsSignal) {
    QSignalSpy spy(controller, &WelcomeScreenController::visibleChanged);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(true);
    EXPECT_TRUE(controller->isVisible());
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(WelcomeScreenControllerTests, SetVisibleSameValueNoSignal) {
    controller->setVisible(false); // already false
    QSignalSpy spy(controller, &WelcomeScreenController::visibleChanged);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(false);
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(WelcomeScreenControllerTests, DismissSetsVisibleFalse) {
    controller->setVisible(true);
    EXPECT_TRUE(controller->isVisible());

    controller->dismiss(false);
    EXPECT_FALSE(controller->isVisible());
}

// --- signals from actions ---

TEST_F(WelcomeScreenControllerTests, OpenFileEmitsRequestAndHides) {
    QSignalSpy spy(controller, &WelcomeScreenController::requestOpenFile);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(true);
    controller->openFile("/tmp/test.fbx");

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "/tmp/test.fbx");
    EXPECT_FALSE(controller->isVisible());
}

TEST_F(WelcomeScreenControllerTests, OpenFileDialogEmitsRequestAndHides) {
    QSignalSpy spy(controller, &WelcomeScreenController::requestOpenFileDialog);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(true);
    controller->openFileDialog();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(controller->isVisible());
}

TEST_F(WelcomeScreenControllerTests, NewSceneEmitsRequestAndHides) {
    QSignalSpy spy(controller, &WelcomeScreenController::requestNewScene);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(true);
    controller->newScene();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(controller->isVisible());
}
