#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "materialeditor.h"
#include "ui_materialeditor.h"
#include <QApplication>

class MaterialEditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        int argc{0};
        char* argv[] = { nullptr };
        app = std::make_unique<QApplication>(argc, argv);
    }
private:
    std::unique_ptr<QApplication> app;
};

TEST_F(MaterialEditorTest, SetMaterialTextTest) {
    auto editor = std::make_unique<MaterialEditor>();
    editor->setMaterialText("Test Material");

    ASSERT_EQ(editor->getMaterialText(), "Test Material");
}

TEST_F(MaterialEditorTest, SetMaterialEmptyTest) {
    auto editor = std::make_unique<MaterialEditor>();
    editor->setMaterial("");

    ASSERT_EQ(editor->getMaterialText(), "material material_name\n{\n}");
    ASSERT_EQ(editor->getMaterialName(), "material_name");
    ASSERT_FALSE(editor->isScrollAreaEnabled());
}

TEST_F(MaterialEditorTest, SetMaterial) {
    auto editor = std::make_unique<MaterialEditor>();

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    editor->setMaterial("TestMaterial");

    ASSERT_EQ(editor->getMaterialText(), "\nmaterial TestMaterial\n{\n\ttechnique\n\t{\n\t\tpass \n\t\t{\n\t\t}\n\n\t}\n\n}\n");
    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_TRUE(editor->isScrollAreaEnabled());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, SetTechFieldsTestWithEmptyList) {
    auto editor = std::make_unique<MaterialEditor>();

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
    auto editor = std::make_unique<MaterialEditor>();

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

TEST_F(MaterialEditorTest, ApplyMaterial) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set lighting to false
    editor->setMaterial("TestMaterial");
    editor->setMaterialText("\nmaterial TestMaterial\n{\n\ttechnique\n\t{\n\t\tpass \n\t\t{\n\t\tlighting off\n\t\t}\n\n\t}\n\n}\n");

    // Apply
    editor->getUI()->applyButton->setEnabled(true);
    editor->getUI()->applyButton->click();

    // Assert it applied the text to the material
    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_FALSE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());
    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onAmbientColorSelected) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Set ambient color
    auto testColor = QColor(233, 127, 90);
    editor->on_Ambient_Color_Selected(testColor);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getAmbient().r, testColor.redF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getAmbient().g, testColor.greenF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getAmbient().b, testColor.blueF());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onDifuseColorSelected) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Set difuse color
    auto testColor = QColor(233, 127, 90);
    editor->on_Difuse_Color_Selected(testColor);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().r, testColor.redF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().g, testColor.greenF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().b, testColor.blueF());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onSpecularColorSelected) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Set specular color
    auto testColor = QColor(233, 127, 90);
    editor->on_Specular_Color_Selected(testColor);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().r, testColor.redF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().g, testColor.greenF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().b, testColor.blueF());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onEmissiveColorSelected) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Set emissive color
    auto testColor = QColor(233, 127, 90);
    editor->on_Emissive_Color_Selected(testColor);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSelfIllumination().r, testColor.redF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSelfIllumination().g, testColor.greenF());
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSelfIllumination().b, testColor.blueF());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxLightningToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle lightning
    editor->on_checkBoxLightning_toggled(false);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_FALSE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    // Toggle lightning back
    editor->on_checkBoxLightning_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getLightingEnabled());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxDepthWriteToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthWriteEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle depth write
    editor->on_checkBoxDepthWrite_toggled(false);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_FALSE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthWriteEnabled());

    // Toggle depth write back
    editor->on_checkBoxDepthWrite_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthWriteEnabled());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxDepthCheckToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthCheckEnabled());

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle depth check
    editor->on_checkBoxDepthCheck_toggled(false);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_FALSE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthCheckEnabled());

    // Toggle depth check back
    editor->on_checkBoxDepthCheck_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_TRUE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDepthCheckEnabled());

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxUseVertexColorToAmbientToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_AMBIENT);

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle vertex color to ambient
    editor->on_checkBoxUseVertexColorToAmbient_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_AMBIENT);

    // Toggle vertex color to ambient back
    editor->on_checkBoxUseVertexColorToAmbient_toggled(false);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_AMBIENT);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxUseVertexColorToDifuseToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_DIFFUSE);

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle vertex color to difuse
    editor->on_checkBoxUseVertexColorToDifuse_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_DIFFUSE);

    // Toggle vertex color to difuse back
    editor->on_checkBoxUseVertexColorToDifuse_toggled(false);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_DIFFUSE);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxUseVertexColorToSpecularToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_SPECULAR);

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle vertex color to specular
    editor->on_checkBoxUseVertexColorToSpecular_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_SPECULAR);

    // Toggle vertex color to specular back
    editor->on_checkBoxUseVertexColorToSpecular_toggled(false);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_SPECULAR);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onCheckBoxUseVertexColorToEmissiveToggled) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_EMISSIVE);

    // Set material
    editor->setMaterial("TestMaterial");

    // Toggle vertex color to emissive
    editor->on_checkBoxUseVertexColorToEmissive_toggled(true);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_EMISSIVE);

    // Toggle vertex color to emissive back
    editor->on_checkBoxUseVertexColorToEmissive_toggled(false);
    ASSERT_NE(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getVertexColourTracking(), Ogre::TVC_EMISSIVE);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onComboPolygonModeCurrentIndexChanged) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getPolygonMode(), Ogre::PM_SOLID);

    // Set material
    editor->setMaterial("TestMaterial");

    // Set polygon mode PM_WIREFRAME
    editor->on_comboPolygonMode_currentIndexChanged(1);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getPolygonMode(), Ogre::PM_WIREFRAME);

    // Set polygon mode PM_POINTS
    editor->on_comboPolygonMode_currentIndexChanged(0);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getPolygonMode(), Ogre::PM_POINTS);

    // Set polygon mode PM_SOLID
    editor->on_comboPolygonMode_currentIndexChanged(2);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getPolygonMode(), Ogre::PM_SOLID);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onAlphaDifuseValueChanged) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().a, 1.0f);

    // Set material
    editor->setMaterial("TestMaterial");

    // Set alpha difuse
    editor->on_alphaDifuse_valueChanged(0.5);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().a, 0.5);

    // Set alpha difuse to 0
    editor->on_alphaDifuse_valueChanged(0.0);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getDiffuse().a, 0.0);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onAlphaSpecularValueChanged) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().a, 1.0f);

    // Set material
    editor->setMaterial("TestMaterial");

    // Set alpha specular
    editor->on_alphaSpecular_valueChanged(0.5);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().a, 0.5);

    // Set alpha specular to 0
    editor->on_alphaSpecular_valueChanged(0.0);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getSpecular().a, 0.0);

    Ogre::MaterialManager::getSingleton().remove(material);
}

TEST_F(MaterialEditorTest, onShineSpecularValueChanged) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getShininess(), 0.0);

    // Set material
    editor->setMaterial("TestMaterial");

    // Set shine specular
    editor->on_shineSpecular_valueChanged(0.5);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getShininess(), 0.5);

    // Set shine specular back
    editor->on_shineSpecular_valueChanged(0.0);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getShininess(), 0.0);

    Ogre::MaterialManager::getSingleton().remove(material);
}

/*
TEST_F(MaterialEditorTest, onScrollAnimSpeedValueChanged) {
    auto editor = std::make_unique<MaterialEditor>();

    //Create test material
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureUScroll(), 0.0f);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureVScroll(), 0.0f);

    // Set material
    editor->setMaterial("TestMaterial");

    // Set animation u speed
    editor->getUI()->scrollAnimUSpeed->setValue(1.0);
    editor->getUI()->scrollAnimVSpeed->setValue(1.0);

    ASSERT_EQ(editor->getMaterialName(), "TestMaterial");
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureUScroll(), 1.0f);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureVScroll(), 1.0f);
    // Set animation u speed back
    editor->getUI()->scrollAnimUSpeed->setValue(0.0);
    editor->getUI()->scrollAnimVSpeed->setValue(0.0);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureUScroll(), 0.0f);
    ASSERT_EQ(Ogre::MaterialManager::getSingleton().getByName("TestMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)->getTechniques()[0]->getPasses()[0]->getTextureUnitState(0)->getTextureVScroll(), 0.0f);
    Ogre::MaterialManager::getSingleton().remove(material);
} Enable after adding textures to the test*/
