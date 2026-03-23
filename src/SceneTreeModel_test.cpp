#include <gtest/gtest.h>
#include "SceneTreeModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TestHelpers.h"

class SceneTreeModelTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    SceneTreeModel* model = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }

        model = new SceneTreeModel();
    }

    void TearDown() override {
        delete model;
        model = nullptr;
        if (app) app->processEvents();
    }
};

TEST_F(SceneTreeModelTests, InitialState) {
    // Root should have at least 0 rows (empty scene)
    EXPECT_GE(model->rowCount(), 0);
    EXPECT_EQ(model->columnCount(), 1);
}

TEST_F(SceneTreeModelTests, RoleNames) {
    auto roles = model->roleNames();
    EXPECT_TRUE(roles.contains(SceneTreeModel::NameRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeLabelRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::SelectedRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::MaterialNameRole));
}

TEST_F(SceneTreeModelTests, Rebuild) {
    int initialRows = model->rowCount();
    model->rebuild();
    // After rebuild, row count should be consistent
    EXPECT_EQ(model->rowCount(), initialRows);
}

TEST_F(SceneTreeModelTests, InvalidIndex) {
    QModelIndex invalid;
    EXPECT_EQ(model->data(invalid), QVariant());
    EXPECT_EQ(model->parent(invalid), QModelIndex());
}
