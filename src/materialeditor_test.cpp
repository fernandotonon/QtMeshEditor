#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
    protected:
    std::unique_ptr<QApplication> app;
    void SetUp() override {
        int argc{0};
        char* argv[] = { nullptr };
        app = std::make_unique<QApplication>(argc, argv);
    }
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();
    editor->setMaterialText("Test Material");

    ASSERT_EQ(editor->getMaterialText(), "Test Material");
}

TEST_F(MaterialEditorTest, SetMaterialEmptyTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();
    editor->setMaterial("");

    ASSERT_EQ(editor->getMaterialText(), "material material_name\n{\n}");
    ASSERT_EQ(editor->getMaterialName(), "material_name");
    ASSERT_FALSE(editor->isScrollAreaEnabled());
}

TEST_F(MaterialEditorTest, SetMaterial) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    editor->setMaterial("TestMaterial");

    ASSERT_EQ(editor->getMaterialText(), "\nmaterial TestMaterial\n{\n\ttechnique\n\t{\n\t\tpass \n\t\t{\n\t\t}\n\n\t}\n\n}\n");
    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_TRUE(editor->isScrollAreaEnabled());
}

TEST_F(MaterialEditorTest, SetTechFieldsTestWithEmptyList) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();

    QMap<int, Ogre::Pass*> techMap;
    QList<QString> passList;

    editor->setTechFields(techMap, passList);

    // Verify that the passComboBox is in the default state
    ASSERT_EQ(editor->getUI()->passComboBox->count(), 1);
    ASSERT_EQ(editor->getUI()->passComboBox->itemText(0), "");
    ASSERT_TRUE(editor->getUI()->passComboBox->isEnabled());
    ASSERT_TRUE(editor->getUI()->passNewButton->isEnabled());
    ASSERT_EQ(editor->getUI()->passComboBox->currentIndex(), -1);

    ASSERT_FALSE(editor->getUI()->checkBoxLightning->isChecked());
    ASSERT_EQ(editor->getUI()->srcSceneBlendBox->currentIndex(), 0);
    ASSERT_EQ(editor->getUI()->dstSceneBlendBox->currentIndex(), 0);
    ASSERT_FALSE(editor->getUI()->checkBoxDepthWrite->isChecked());
    ASSERT_FALSE(editor->getUI()->checkBoxDepthCheck->isChecked());
    ASSERT_FALSE(editor->getUI()->checkBoxUseVertexColorToAmbient->isChecked());
    ASSERT_FALSE(editor->getUI()->checkBoxUseVertexColorToDifuse->isChecked());
    ASSERT_FALSE(editor->getUI()->checkBoxUseVertexColorToSpecular->isChecked());
    ASSERT_FALSE(editor->getUI()->checkBoxUseVertexColorToEmissive->isChecked());
    ASSERT_EQ(editor->getUI()->ComboTextureUnit->count(), 0);
    ASSERT_EQ(editor->getUI()->alphaDifuse->value(), 0);
    ASSERT_EQ(editor->getUI()->alphaSpecular->value(), 0);
    ASSERT_EQ(editor->getUI()->shineSpecular->value(), 0);
    ASSERT_EQ(editor->getUI()->textureName->text(), "*Select a texture*");
}

TEST_F(MaterialEditorTest, SetTechFieldsTest) {
    std::unique_ptr<MaterialEditor> editor;
    editor = std::make_unique<MaterialEditor>();

    QMap<int, Ogre::Pass*> techMap;
    QList<QString> passList;
    passList << "Pass1" << "Pass2" << "Pass3";

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("MyMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::Technique* technique = material->createTechnique();
    // Create techMap based on passList
    for (int i = 0; i < passList.size(); ++i) {
        Ogre::Pass* pass = technique->createPass();
        pass->setName(passList[i].toStdString());
        techMap[i]=pass;
    }

    editor->setMaterial("MyMaterial");

    editor->setTechFields(techMap, passList);

    // Verify that the passComboBox is populated correctly
    ASSERT_EQ(editor->getUI()->passComboBox->count(), 4); // Including the empty item
    ASSERT_EQ(editor->getUI()->passComboBox->itemText(0), "");
    ASSERT_EQ(editor->getUI()->passComboBox->itemText(1), "Pass1");
    ASSERT_EQ(editor->getUI()->passComboBox->itemText(2), "Pass2");
    ASSERT_EQ(editor->getUI()->passComboBox->itemText(3), "Pass3");

    // Verify that the passComboBox is enabled
    ASSERT_TRUE(editor->getUI()->passComboBox->isEnabled());

    // Verify that the passNewButton is enabled
    ASSERT_TRUE(editor->getUI()->passNewButton->isEnabled());

    // Verify that the passComboBox is set to the first item
    ASSERT_EQ(editor->getUI()->passComboBox->currentIndex(), 1);
}
