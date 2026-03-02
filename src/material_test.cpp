#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QListWidget>
#include <QPushButton>
#include <QThread>
#include "material.h"
#include "Manager.h"
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include "TestHelpers.h"

class MaterialTest : public ::testing::Test {
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

        // Ensure materials are initialized
        ensureMaterialManagerInitialised();
    }

    void TearDown() override {
        Manager::kill();
        QThread::msleep(50);
    }
};

TEST_F(MaterialTest, ConstructionDoesNotCrash) {
    Material materialWidget;
    EXPECT_TRUE(true);
}

TEST_F(MaterialTest, UIElementsExist) {
    Material materialWidget;

    // Check if the list widget exists
    auto listWidget = materialWidget.findChild<QListWidget*>("listMaterial");
    ASSERT_NE(listWidget, nullptr);

    // Check if buttons exist
    auto buttonEdit = materialWidget.findChild<QPushButton*>("buttonEdit");
    EXPECT_NE(buttonEdit, nullptr);

    auto buttonExport = materialWidget.findChild<QPushButton*>("buttonExport");
    EXPECT_NE(buttonExport, nullptr);

    auto buttonNew = materialWidget.findChild<QPushButton*>("buttonNew");
    EXPECT_NE(buttonNew, nullptr);
}

TEST_F(MaterialTest, SetMaterialListPopulatesList) {
    Material materialWidget;

    // Create some test material names
    QStringList testMaterials;
    testMaterials << "Material1" << "Material2" << "Material3";

    // Set the material list
    materialWidget.SetMaterialList(testMaterials);

    // Check if list widget was populated
    auto listWidget = materialWidget.findChild<QListWidget*>("listMaterial");
    ASSERT_NE(listWidget, nullptr);
    EXPECT_EQ(listWidget->count(), testMaterials.size());

    // Check if the materials are in the list
    for (int i = 0; i < testMaterials.size(); ++i) {
        EXPECT_EQ(listWidget->item(i)->text(), testMaterials[i]);
    }
}

TEST_F(MaterialTest, SetMaterialListWithEmptyList) {
    Material materialWidget;

    // Set an empty list
    QStringList emptyList;
    materialWidget.SetMaterialList(emptyList);

    // List widget should be empty
    auto listWidget = materialWidget.findChild<QListWidget*>("listMaterial");
    ASSERT_NE(listWidget, nullptr);
    EXPECT_EQ(listWidget->count(), 0);
}

TEST_F(MaterialTest, SetMaterialListWithOgreMaterials) {
    Material materialWidget;

    // Create some actual Ogre materials
    createStandardOgreMaterials();

    auto mat1 = Ogre::MaterialManager::getSingleton().create(
        "TestMaterial1", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto mat2 = Ogre::MaterialManager::getSingleton().create(
        "TestMaterial2", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    QStringList materials;
    materials << "TestMaterial1" << "TestMaterial2";

    materialWidget.SetMaterialList(materials);

    auto listWidget = materialWidget.findChild<QListWidget*>("listMaterial");
    ASSERT_NE(listWidget, nullptr);
    EXPECT_EQ(listWidget->count(), 2);

    // Clean up
    Ogre::MaterialManager::getSingleton().remove(mat1);
    Ogre::MaterialManager::getSingleton().remove(mat2);
}
