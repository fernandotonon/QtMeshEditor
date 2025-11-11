#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TransformOperator.h"
#include "Manager.h"
#include "mainwindow.h"

// Test fixture for TransformOperator tests that require Manager
class TransformOperatorTestFixture : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    MainWindow* mainWindow = nullptr;

    void SetUp() override {
        // Ensure Manager is completely destroyed from previous test
        Manager::kill();
        TransformOperator::kill();
        QThread::msleep(50); // Small delay to ensure cleanup is complete
        
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        
        // Create MainWindow to initialize Manager
        try {
            mainWindow = new MainWindow();
            ASSERT_NE(mainWindow, nullptr);
            Manager::getSingleton(mainWindow);
        } catch (const Ogre::RenderingAPIException& e) {
            GTEST_SKIP() << "Skipping TransformOperator tests: unable to create OGRE render window ("
                         << e.getFullDescription() << ")";
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Skipping TransformOperator tests: " << e.what();
        }
    }

    void TearDown() override {
        // Clean up in reverse order
        TransformOperator::kill();
        if (mainWindow) {
            delete mainWindow;
            mainWindow = nullptr;
        }
        Manager::kill();
        
        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

// Test if getSingleton returns a valid pointer
TEST_F(TransformOperatorTestFixture, GetSingleton) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NE(instance, nullptr);
}

// Test if getSingleton always returns the same instance
TEST_F(TransformOperatorTestFixture, SingletonInstance) {
    TransformOperator* instance1 = TransformOperator::getSingleton();
    TransformOperator* instance2 = TransformOperator::getSingleton();
    EXPECT_EQ(instance1, instance2);
}

// Test if setTransformState sets the state correctly
TEST_F(TransformOperatorTestFixture, SetSelectionBoxColour) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectionBoxColour(Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
}

// Swap test doesn't need Manager, so it can be standalone
TEST(TransformOperatorTest, Swap) {
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}

TEST_F(TransformOperatorTestFixture, RayFromScreenPoint) {
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::Ray ray = instance->rayFromScreenPoint(QPoint(0, 0));
    EXPECT_EQ(ray.getOrigin(), Ogre::Vector3::ZERO);
    EXPECT_EQ(ray.getDirection(), Ogre::Vector3::UNIT_Z);
}
