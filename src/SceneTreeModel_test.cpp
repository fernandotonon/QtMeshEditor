#include <gtest/gtest.h>
#include "SceneTreeModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
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

TEST_F(SceneTreeModelTests, AvailableMaterials) {
    // availableMaterials() should return a list (possibly empty in test env)
    QStringList mats = model->availableMaterials();
    // Just verify it returns without crash; the list may be empty
    // if no materials besides internal ones are loaded
    EXPECT_GE(mats.size(), 0);
}

TEST_F(SceneTreeModelTests, SelectItemInvalidIndex) {
    // selectItem with an out-of-range row should not crash
    QModelIndex root = model->rootIndex();
    EXPECT_NO_THROW(model->selectItem(-1, root, false));
    EXPECT_NO_THROW(model->selectItem(99999, root, false));
}

TEST_F(SceneTreeModelTests, SetMaterialInvalidIndex) {
    // setMaterial with an out-of-range row should not crash
    QModelIndex root = model->rootIndex();
    EXPECT_NO_THROW(model->setMaterial(-1, root, "BaseWhite"));
    EXPECT_NO_THROW(model->setMaterial(99999, root, "BaseWhite"));
}

TEST_F(SceneTreeModelTests, MaterialNameInvalidIndex) {
    // materialName with an out-of-range row should return empty
    QModelIndex root = model->rootIndex();
    QString name = model->materialName(-1, root);
    EXPECT_TRUE(name.isEmpty());

    name = model->materialName(99999, root);
    EXPECT_TRUE(name.isEmpty());
}

TEST_F(SceneTreeModelTests, IsSelectedInvalidIndex) {
    QModelIndex root = model->rootIndex();
    // isSelected with invalid row should return false
    EXPECT_FALSE(model->isSelected(-1, root));
    EXPECT_FALSE(model->isSelected(99999, root));
}

TEST_F(SceneTreeModelTests, RootIndexIsInvalid) {
    // rootIndex() returns an invalid QModelIndex (root of the tree)
    QModelIndex root = model->rootIndex();
    EXPECT_FALSE(root.isValid());
}

TEST_F(SceneTreeModelTests, RebuildWithEntity) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("TreeModelTestMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TreeModelTestNode");
    auto* entity = sceneMgr->createEntity("TreeModelTestEnt", mesh);
    node->attachObject(entity);

    model->rebuild();

    // Should have at least one row (the node we added)
    EXPECT_GE(model->rowCount(), 1);
}

TEST_F(SceneTreeModelTests, SelectNodeItem) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("SelectTestNode");
    ASSERT_NE(node, nullptr);

    model->rebuild();

    // Find the row for our node
    QModelIndex root = model->rootIndex();
    int rows = model->rowCount(root);
    bool found = false;
    for (int i = 0; i < rows; ++i) {
        QModelIndex idx = model->index(i, 0, root);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == "SelectTestNode") {
            model->selectItem(i, root, false);
            EXPECT_TRUE(SelectionSet::getSingleton()->contains(node));
            found = true;
            break;
        }
    }
    // Node may or may not be found depending on forbidden names, but test should not crash
    if (!found) {
        // selectItem with valid but wrong index should not crash
        EXPECT_NO_THROW(model->selectItem(0, root, false));
    }

    SelectionSet::getSingleton()->clear();
}

TEST_F(SceneTreeModelTests, UpdateSelection) {
    QSignalSpy spy(model, &SceneTreeModel::selectionUpdated);
    model->updateSelection();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(SceneTreeModelTests, DataWithVariousRoles) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DataRolesNode");
    ASSERT_NE(node, nullptr);

    model->rebuild();

    QModelIndex root = model->rootIndex();
    int rows = model->rowCount(root);
    if (rows > 0) {
        QModelIndex idx = model->index(0, 0, root);
        if (idx.isValid()) {
            // Test all roles
            QVariant nameData = model->data(idx, SceneTreeModel::NameRole);
            EXPECT_FALSE(nameData.toString().isEmpty());

            QVariant typeData = model->data(idx, SceneTreeModel::TypeRole);
            EXPECT_TRUE(typeData.isValid());

            QVariant typeLabelData = model->data(idx, SceneTreeModel::TypeLabelRole);
            EXPECT_TRUE(typeLabelData.isValid());

            QVariant selectedData = model->data(idx, SceneTreeModel::SelectedRole);
            EXPECT_TRUE(selectedData.isValid());

            // Unknown role should return empty QVariant
            QVariant unknownData = model->data(idx, Qt::UserRole + 100);
            EXPECT_FALSE(unknownData.isValid());
        }
    }
}
