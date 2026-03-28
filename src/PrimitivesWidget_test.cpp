#include <gtest/gtest.h>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QInputDialog>
#include <QDialog>
#include <QCoreApplication>
#include <QLabel>
#include <QGroupBox>
#include <QPushButton>
#include <QTimer>
#include <array>
#include "PrimitivesWidget.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <OgreException.h>
#include "TestHelpers.h"

class PrimitivesWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
        if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    }

    void TearDown() override
    {
        if(app)
        {
            app->processEvents();
        }
    }

private:
    QApplication* app = nullptr;
};

namespace {
struct PrimitiveDialogCase {
    QString defaultName;
    QString customName;
    void (PrimitivesWidget::*slot)();
};

const std::array<PrimitiveDialogCase, 11> kPrimitiveDialogCases{{
    {QStringLiteral("Cube"), QStringLiteral("DialogCube"), &PrimitivesWidget::createCube},
    {QStringLiteral("Sphere"), QStringLiteral("DialogSphere"), &PrimitivesWidget::createSphere},
    {QStringLiteral("Plane"), QStringLiteral("DialogPlane"), &PrimitivesWidget::createPlane},
    {QStringLiteral("Cylinder"), QStringLiteral("DialogCylinder"), &PrimitivesWidget::createCylinder},
    {QStringLiteral("Cone"), QStringLiteral("DialogCone"), &PrimitivesWidget::createCone},
    {QStringLiteral("Torus"), QStringLiteral("DialogTorus"), &PrimitivesWidget::createTorus},
    {QStringLiteral("Tube"), QStringLiteral("DialogTube"), &PrimitivesWidget::createTube},
    {QStringLiteral("Capsule"), QStringLiteral("DialogCapsule"), &PrimitivesWidget::createCapsule},
    {QStringLiteral("IcoSphere"), QStringLiteral("DialogIcoSphere"), &PrimitivesWidget::createIcoSphere},
    {QStringLiteral("RoundedBox"), QStringLiteral("DialogRoundedBox"), &PrimitivesWidget::createRoundedBox},
    {QStringLiteral("Spring"), QStringLiteral("DialogSpring"), &PrimitivesWidget::createSpring},
}};

Ogre::SceneNode* createPlainNode(const QString& nodeName, const std::string& meshName)
{
    auto* node = Manager::getSingleton()->addSceneNode(nodeName);
    if (!node)
        return nullptr;

    auto mesh = createInMemoryTriangleMesh(meshName);
    if (!mesh)
        return nullptr;

    if (!Manager::getSingleton()->createEntity(node, mesh))
        return nullptr;

    SelectionSet::getSingleton()->clear();
    return node;
}

void driveModalInputDialogResponse(bool& handled, const QString& text, QDialog::DialogCode result)
{
    QTimer::singleShot(0, [&handled, text, result]() {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (!dialog) {
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget* widget : widgets) {
                dialog = qobject_cast<QInputDialog*>(widget);
                if (dialog && dialog->isVisible())
                    break;
            }
        }

        if (!dialog)
            return;

        handled = true;
        dialog->setTextValue(text);
        dialog->done(result);
    });
}
} // namespace

TEST_F(PrimitivesWidgetTest, CreateCube)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCube("Cube");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cube");
}

TEST_F(PrimitivesWidgetTest, RemoveAndRecreateCube) {
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCube("Cube");

    auto countBefore = Manager::getSingleton()->getSceneNodes().count();

    Manager::getSingleton()->destroySceneNode("Cube");

    auto countAfter = Manager::getSingleton()->getSceneNodes().count();

    ASSERT_EQ(countBefore-1,countAfter);

    PrimitiveObject::createCube("Cube");
    Manager::getSingleton()->destroySceneNode("Cube");
    ASSERT_EQ(countBefore-1,countAfter);
}

TEST_F(PrimitivesWidgetTest, CreateSphere)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createSphere("Sphere");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Sphere");
}

TEST_F(PrimitivesWidgetTest, CreatePlane)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createPlane("Plane");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Plane");
}

TEST_F(PrimitivesWidgetTest, CreateCylinder)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCylinder("Cylinder");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cylinder");
}

TEST_F(PrimitivesWidgetTest, CreateCone)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCone("Cone");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Cone");
}

TEST_F(PrimitivesWidgetTest, CreateTorus)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createTorus("Torus");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Torus");
}

TEST_F(PrimitivesWidgetTest, CreateTube)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createTube("Tube");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Tube");
}

TEST_F(PrimitivesWidgetTest, CreateCapsule)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createCapsule("Capsule");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Capsule");
}

TEST_F(PrimitivesWidgetTest, CreateIcoSphere)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createIcoSphere("IcoSphere");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "IcoSphere");
}

TEST_F(PrimitivesWidgetTest, CreateRoundedBox)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createRoundedBox("Rounded Box");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Rounded Box");
}

TEST_F(PrimitivesWidgetTest, CreateSpring)
{
    PrimitivesWidget widget;
    QLineEdit* primitiveTypeLineEdit = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_TRUE(primitiveTypeLineEdit != nullptr);

    PrimitiveObject::createSpring("Spring");

    QString primitiveType = primitiveTypeLineEdit->text();
    ASSERT_EQ(primitiveType, "Spring");
}

TEST_F(PrimitivesWidgetTest, UpdateUiFromParams)
{
    PrimitivesWidget widget;
    QDoubleSpinBox* sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    ASSERT_TRUE(sizeXLineEdit != nullptr);

    QDoubleSpinBox* sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    ASSERT_TRUE(sizeYLineEdit != nullptr);

    QDoubleSpinBox* sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    ASSERT_TRUE(sizeZLineEdit != nullptr);

    widget.updateUiFromParams();

    ASSERT_EQ(sizeXLineEdit->value(), sizeXLineEdit->minimum());
    ASSERT_EQ(sizeYLineEdit->value(), sizeXLineEdit->minimum());
    ASSERT_EQ(sizeZLineEdit->value(), sizeXLineEdit->minimum());
}

TEST_F(PrimitivesWidgetTest, UpdateParamsFromUi)
{
    PrimitivesWidget widget;
    auto sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    ASSERT_TRUE(sizeXLineEdit != nullptr);

    auto sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    ASSERT_TRUE(sizeYLineEdit != nullptr);

    auto sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    ASSERT_TRUE(sizeZLineEdit != nullptr);

    auto edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    ASSERT_TRUE(edit_radius != nullptr);

    auto edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    ASSERT_TRUE(edit_radius2 != nullptr);

    auto edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    ASSERT_TRUE(edit_height != nullptr);

    auto edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    ASSERT_TRUE(edit_numSegX != nullptr);

    auto edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    ASSERT_TRUE(edit_numSegY != nullptr);

    auto edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    ASSERT_TRUE(edit_numSegZ != nullptr);

    auto edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    ASSERT_TRUE(edit_UTile != nullptr);

    auto edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    ASSERT_TRUE(edit_VTile != nullptr);

    auto pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");
    ASSERT_TRUE(pb_switchUV != nullptr);

    sizeXLineEdit->setValue(1.0);
    sizeYLineEdit->setValue(2.0);
    sizeZLineEdit->setValue(3.0);
    edit_radius->setValue(1.0);
    edit_radius2->setValue(2.0);
    edit_height->setValue(3.0);
    edit_numSegX->setValue(4);
    edit_numSegY->setValue(5);
    edit_numSegZ->setValue(6);
    edit_UTile->setValue(7.0);
    edit_VTile->setValue(8.0);
    pb_switchUV->click();

    ASSERT_EQ(sizeXLineEdit->value(), 1.0);
    ASSERT_EQ(sizeYLineEdit->value(), 2.0);
    ASSERT_EQ(sizeZLineEdit->value(), 3.0);
    ASSERT_EQ(edit_radius->value(), 1.0);
    ASSERT_EQ(edit_radius2->value(), 2.0);
    ASSERT_EQ(edit_height->value(), 3.0);
    ASSERT_EQ(edit_numSegX->value(), 4);
    ASSERT_EQ(edit_numSegY->value(), 5);
    ASSERT_EQ(edit_numSegZ->value(), 6);
    ASSERT_EQ(edit_UTile->value(), 7.0);
    ASSERT_EQ(edit_VTile->value(), 8.0);
    ASSERT_TRUE(pb_switchUV->isChecked());
}

// --- Primitive entity verification tests ---

TEST_F(PrimitivesWidgetTest, CreateCubeVerifyEntity)
{
    PrimitiveObject::createCube("VerifyCube");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyCube"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyCube"));
    auto* entity = sceneMgr->getEntity("VerifyCube");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyCube");
}

TEST_F(PrimitivesWidgetTest, CreateSphereVerifyEntity)
{
    PrimitiveObject::createSphere("VerifySphere");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifySphere"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifySphere"));
    auto* entity = sceneMgr->getEntity("VerifySphere");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifySphere");
}

TEST_F(PrimitivesWidgetTest, CreateTorusVerifyEntity)
{
    PrimitiveObject::createTorus("VerifyTorus");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyTorus"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyTorus"));
    auto* entity = sceneMgr->getEntity("VerifyTorus");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyTorus");
}

TEST_F(PrimitivesWidgetTest, CreateConeVerifyEntity)
{
    PrimitiveObject::createCone("VerifyCone");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyCone"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyCone"));
    auto* entity = sceneMgr->getEntity("VerifyCone");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyCone");
}

TEST_F(PrimitivesWidgetTest, CreateTubeVerifyEntity)
{
    PrimitiveObject::createTube("VerifyTube");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyTube"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyTube"));
    auto* entity = sceneMgr->getEntity("VerifyTube");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyTube");
}

TEST_F(PrimitivesWidgetTest, CreateCapsuleVerifyEntity)
{
    PrimitiveObject::createCapsule("VerifyCapsule");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyCapsule"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyCapsule"));
    auto* entity = sceneMgr->getEntity("VerifyCapsule");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyCapsule");
}

TEST_F(PrimitivesWidgetTest, CreateIcoSphereVerifyEntity)
{
    PrimitiveObject::createIcoSphere("VerifyIcoSphere");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyIcoSphere"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyIcoSphere"));
    auto* entity = sceneMgr->getEntity("VerifyIcoSphere");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyIcoSphere");
}

TEST_F(PrimitivesWidgetTest, CreateRoundedBoxVerifyEntity)
{
    PrimitiveObject::createRoundedBox("VerifyRBox");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifyRBox"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifyRBox"));
    auto* entity = sceneMgr->getEntity("VerifyRBox");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifyRBox");
}

TEST_F(PrimitivesWidgetTest, CreateSpringVerifyEntity)
{
    PrimitiveObject::createSpring("VerifySpring");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("VerifySpring"));
    auto* sceneMgr = mgr->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity("VerifySpring"));
    auto* entity = sceneMgr->getEntity("VerifySpring");
    auto* mesh = entity->getMesh().get();
    auto* submesh = mesh->getSubMesh(0);
    Ogre::VertexData* vdata = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;
    ASSERT_NE(vdata, nullptr);
    EXPECT_GT(vdata->vertexCount, 0u);
    mgr->destroySceneNode("VerifySpring");
}

TEST_F(PrimitivesWidgetTest, CreateAndDestroyMultiplePrimitives)
{
    PrimitiveObject::createCube("MultiPrim1");
    PrimitiveObject::createSphere("MultiPrim2");
    PrimitiveObject::createCylinder("MultiPrim3");

    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("MultiPrim1"));
    EXPECT_TRUE(mgr->hasSceneNode("MultiPrim2"));
    EXPECT_TRUE(mgr->hasSceneNode("MultiPrim3"));

    mgr->destroySceneNode("MultiPrim1");
    mgr->destroySceneNode("MultiPrim2");
    mgr->destroySceneNode("MultiPrim3");

    EXPECT_FALSE(mgr->hasSceneNode("MultiPrim1"));
    EXPECT_FALSE(mgr->hasSceneNode("MultiPrim2"));
    EXPECT_FALSE(mgr->hasSceneNode("MultiPrim3"));
}

TEST_F(PrimitivesWidgetTest, UpdateParamsFromUiForACube)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("Cube");
    auto cube = widget.getSelectedPrimitiveList()[0];

    auto sizeXLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto sizeYLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto sizeZLineEdit = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    auto edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    auto pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");

    sizeXLineEdit->setValue(1.0);
    sizeYLineEdit->setValue(2.0);
    sizeZLineEdit->setValue(3.0);
    edit_radius->setValue(1.0);
    edit_radius2->setValue(2.0);
    edit_height->setValue(3.0);
    edit_numSegX->setValue(4);
    edit_numSegY->setValue(5);
    edit_numSegZ->setValue(6);
    edit_UTile->setValue(7.0);
    edit_VTile->setValue(8.0);
    pb_switchUV->click();

    ASSERT_EQ(cube->getType(), PrimitiveObject::PrimitiveType::AP_CUBE);
    ASSERT_EQ(cube->getSizeX(), 1.0);
    ASSERT_EQ(cube->getSizeY(), 2.0);
    ASSERT_EQ(cube->getSizeZ(), 3.0);
    ASSERT_EQ(cube->getRadius(), 1.0);
    ASSERT_EQ(cube->getSectionRadius(), 0);
    ASSERT_EQ(cube->getHeight(), 3.0);
    ASSERT_EQ(cube->getNumSegX(), 4);
    ASSERT_EQ(cube->getNumSegY(), 5);
    ASSERT_EQ(cube->getNumSegZ(), 6);
    ASSERT_EQ(cube->getUTile(), 7.0);
    ASSERT_EQ(cube->getVTile(), 8.0);
    ASSERT_TRUE(cube->hasUVSwitched());

    Manager::getSingleton()->destroySceneNode("Cube");
}

// --- UI state tests for setUi* methods ---

TEST_F(PrimitivesWidgetTest, SetUiEmptyHidesAllFields)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* gb_Geometry = widget.findChild<QGroupBox*>("gb_Geometry");
    auto* gb_Mesh = widget.findChild<QGroupBox*>("gb_Mesh");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_sizeY = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto* edit_sizeZ = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    auto* edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    auto* pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");

    // setUiEmpty is called during construction, so verify initial state.
    // Use isHidden() instead of !isVisible() because isVisible() checks the
    // entire ancestor chain, which is unreliable in headless test environments.
    EXPECT_EQ(edit_type->text(), "");
    EXPECT_TRUE(gb_Geometry->isHidden());
    EXPECT_TRUE(gb_Mesh->isHidden());
    EXPECT_TRUE(edit_sizeX->isHidden());
    EXPECT_TRUE(edit_sizeY->isHidden());
    EXPECT_TRUE(edit_sizeZ->isHidden());
    EXPECT_TRUE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_TRUE(edit_numSegX->isHidden());
    EXPECT_TRUE(edit_numSegY->isHidden());
    EXPECT_TRUE(edit_numSegZ->isHidden());
    EXPECT_TRUE(edit_UTile->isHidden());
    EXPECT_TRUE(edit_VTile->isHidden());
    EXPECT_TRUE(pb_switchUV->isHidden());
}

TEST_F(PrimitivesWidgetTest, CubeUiShowsSizeFieldsAndSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("UiCube");
    // Select the node so onSelectionChanged fires
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiCube"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* gb_Geometry = widget.findChild<QGroupBox*>("gb_Geometry");
    auto* gb_Mesh = widget.findChild<QGroupBox*>("gb_Mesh");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_sizeY = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto* edit_sizeZ = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    auto* edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    auto* pb_switchUV = widget.findChild<QPushButton*>("pb_switchUV");

    EXPECT_EQ(edit_type->text(), "Cube");
    // Use !isHidden() instead of isVisible() because isVisible() requires
    // the entire ancestor widget chain to be shown, which fails under
    // headless (Xvfb) testing where the top-level widget is never shown.
    EXPECT_FALSE(gb_Geometry->isHidden());
    EXPECT_FALSE(gb_Mesh->isHidden());
    EXPECT_FALSE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_sizeY->isHidden());
    EXPECT_FALSE(edit_sizeZ->isHidden());
    EXPECT_TRUE(edit_radius->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_FALSE(edit_numSegZ->isHidden());
    EXPECT_FALSE(edit_UTile->isHidden());
    EXPECT_FALSE(edit_VTile->isHidden());
    // Cube hides switchUV
    EXPECT_TRUE(pb_switchUV->isHidden());

    Manager::getSingleton()->destroySceneNode("UiCube");
}

TEST_F(PrimitivesWidgetTest, SphereUiShowsRadiusAndRingLoopSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSphere("UiSphere");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiSphere"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegY = widget.findChild<QLabel*>("label_numSegY");

    EXPECT_EQ(edit_type->text(), "Sphere");
    EXPECT_TRUE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_TRUE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Seg Ring");
    EXPECT_EQ(label_numSegY->text(), "Seg Loop");

    Manager::getSingleton()->destroySceneNode("UiSphere");
}

TEST_F(PrimitivesWidgetTest, CylinderUiShowsRadiusHeightAndBaseHeightSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCylinder("UiCylinder");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiCylinder"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegZ = widget.findChild<QLabel*>("label_numSegZ");

    EXPECT_EQ(edit_type->text(), "Cylinder");
    EXPECT_TRUE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_FALSE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_TRUE(edit_numSegY->isHidden());
    EXPECT_FALSE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Seg Base");
    EXPECT_EQ(label_numSegZ->text(), "Seg Height");

    Manager::getSingleton()->destroySceneNode("UiCylinder");
}

TEST_F(PrimitivesWidgetTest, TorusUiShowsTwoRadiiAndCircleSectionSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createTorus("UiTorus");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiTorus"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* label_radius = widget.findChild<QLabel*>("label_radius");
    auto* label_radius2 = widget.findChild<QLabel*>("label_radius2");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegY = widget.findChild<QLabel*>("label_numSegY");

    EXPECT_EQ(edit_type->text(), "Torus");
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_FALSE(edit_radius2->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_EQ(label_radius->text(), "Radius");
    EXPECT_EQ(label_radius2->text(), "Section Radius");
    EXPECT_EQ(label_numSegX->text(), "Seg Circle");
    EXPECT_EQ(label_numSegY->text(), "Seg Section");

    Manager::getSingleton()->destroySceneNode("UiTorus");
}

TEST_F(PrimitivesWidgetTest, TubeUiShowsOuterInnerRadiusAndHeight)
{
    PrimitivesWidget widget;
    PrimitiveObject::createTube("UiTube");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiTube"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* label_radius = widget.findChild<QLabel*>("label_radius");
    auto* label_radius2 = widget.findChild<QLabel*>("label_radius2");

    EXPECT_EQ(edit_type->text(), "Tube");
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_FALSE(edit_radius2->isHidden());
    EXPECT_FALSE(edit_height->isHidden());
    EXPECT_EQ(label_radius->text(), "Outer Radius");
    EXPECT_EQ(label_radius2->text(), "Inner Radius");

    Manager::getSingleton()->destroySceneNode("UiTube");
}

TEST_F(PrimitivesWidgetTest, IcoSphereUiShowsRadiusAndIterations)
{
    PrimitivesWidget widget;
    PrimitiveObject::createIcoSphere("UiIco");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiIco"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");

    EXPECT_EQ(edit_type->text(), "IcoSphere");
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_TRUE(edit_numSegY->isHidden());
    EXPECT_TRUE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Iterations");

    Manager::getSingleton()->destroySceneNode("UiIco");
}

// --- Selection change handler tests ---

TEST_F(PrimitivesWidgetTest, OnSelectionChangedWithNoPrimitiveSetsUiEmpty)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");

    // Create a non-primitive scene node (no PrimitiveObject binding)
    auto* node = Manager::getSingleton()->addSceneNode("PlainNode");
    auto mesh = createInMemoryTriangleMesh("PlainMesh");
    Manager::getSingleton()->createEntity(node, mesh);

    SelectionSet::getSingleton()->selectOne(node);

    // onSelectionChanged should detect no primitive and set UI empty
    EXPECT_EQ(edit_type->text(), "");

    Manager::getSingleton()->destroySceneNode("PlainNode");
}

TEST_F(PrimitivesWidgetTest, OnSelectionChangedEmptySelectionSetsUiEmpty)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");

    // First create and select a cube
    PrimitiveObject::createCube("SelCube");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SelCube"));
    EXPECT_EQ(edit_type->text(), "Cube");

    // Clear selection
    SelectionSet::getSingleton()->clear();
    EXPECT_EQ(edit_type->text(), "");

    Manager::getSingleton()->destroySceneNode("SelCube");
}

TEST_F(PrimitivesWidgetTest, SelectCubeThenSphereSwitchesUi)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");

    PrimitiveObject::createCube("SwitchCube");
    PrimitiveObject::createSphere("SwitchSphere");

    // Select cube
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SwitchCube"));
    EXPECT_EQ(edit_type->text(), "Cube");
    EXPECT_FALSE(edit_sizeX->isHidden());
    EXPECT_TRUE(edit_radius->isHidden());

    // Switch to sphere
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SwitchSphere"));
    EXPECT_EQ(edit_type->text(), "Sphere");
    EXPECT_TRUE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_radius->isHidden());

    Manager::getSingleton()->destroySceneNode("SwitchCube");
    Manager::getSingleton()->destroySceneNode("SwitchSphere");
}

// --- Default parameter verification tests ---

TEST_F(PrimitivesWidgetTest, CubeDefaultParameters)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("DefCube");
    auto primitives = widget.getSelectedPrimitiveList();
    ASSERT_EQ(primitives.count(), 1);
    auto* cube = primitives[0];

    EXPECT_EQ(cube->getType(), PrimitiveObject::AP_CUBE);
    EXPECT_FLOAT_EQ(cube->getSizeX(), 2.0f);
    EXPECT_FLOAT_EQ(cube->getSizeY(), 2.0f);
    EXPECT_FLOAT_EQ(cube->getSizeZ(), 2.0f);
    EXPECT_EQ(cube->getNumSegX(), 1);
    EXPECT_EQ(cube->getNumSegY(), 1);
    EXPECT_EQ(cube->getNumSegZ(), 1);
    EXPECT_FLOAT_EQ(cube->getUTile(), 1.0f);
    EXPECT_FLOAT_EQ(cube->getVTile(), 1.0f);
    EXPECT_FALSE(cube->hasUVSwitched());

    Manager::getSingleton()->destroySceneNode("DefCube");
}

TEST_F(PrimitivesWidgetTest, SphereDefaultParameters)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSphere("DefSphere");
    auto primitives = widget.getSelectedPrimitiveList();
    ASSERT_EQ(primitives.count(), 1);
    auto* sphere = primitives[0];

    EXPECT_EQ(sphere->getType(), PrimitiveObject::AP_SPHERE);
    EXPECT_FLOAT_EQ(sphere->getRadius(), 1.0f);
    EXPECT_EQ(sphere->getNumSegX(), 16);
    EXPECT_EQ(sphere->getNumSegY(), 16);
    EXPECT_FLOAT_EQ(sphere->getUTile(), 1.0f);
    EXPECT_FLOAT_EQ(sphere->getVTile(), 1.0f);

    Manager::getSingleton()->destroySceneNode("DefSphere");
}

TEST_F(PrimitivesWidgetTest, TorusDefaultParameters)
{
    PrimitivesWidget widget;
    PrimitiveObject::createTorus("DefTorus");
    auto primitives = widget.getSelectedPrimitiveList();
    ASSERT_EQ(primitives.count(), 1);
    auto* torus = primitives[0];

    EXPECT_EQ(torus->getType(), PrimitiveObject::AP_TORUS);
    EXPECT_FLOAT_EQ(torus->getRadius(), 3.0f);
    EXPECT_FLOAT_EQ(torus->getSectionRadius(), 1.0f);
    EXPECT_EQ(torus->getNumSegX(), 16);
    EXPECT_EQ(torus->getNumSegY(), 16);

    Manager::getSingleton()->destroySceneNode("DefTorus");
}

// --- Edge case: setters guard against zero/negative values ---

TEST_F(PrimitivesWidgetTest, SetZeroSizeDoesNotChangeCubeParams)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("ZeroCube");
    auto* cube = widget.getSelectedPrimitiveList()[0];

    Ogre::Real origSizeX = cube->getSizeX();
    Ogre::Real origSizeY = cube->getSizeY();
    Ogre::Real origSizeZ = cube->getSizeZ();

    // Attempt to set zero (should be rejected by the guard)
    cube->setSizeX(0.0);
    cube->setSizeY(0.0);
    cube->setSizeZ(0.0);

    EXPECT_FLOAT_EQ(cube->getSizeX(), origSizeX);
    EXPECT_FLOAT_EQ(cube->getSizeY(), origSizeY);
    EXPECT_FLOAT_EQ(cube->getSizeZ(), origSizeZ);

    Manager::getSingleton()->destroySceneNode("ZeroCube");
}

TEST_F(PrimitivesWidgetTest, SetNegativeRadiusDoesNotChangeSphereParams)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSphere("NegSphere");
    auto* sphere = widget.getSelectedPrimitiveList()[0];

    Ogre::Real origRadius = sphere->getRadius();

    sphere->setRadius(-5.0);
    EXPECT_FLOAT_EQ(sphere->getRadius(), origRadius);

    sphere->setRadius(-0.001f);
    EXPECT_FLOAT_EQ(sphere->getRadius(), origRadius);

    Manager::getSingleton()->destroySceneNode("NegSphere");
}

TEST_F(PrimitivesWidgetTest, SetNegativeHeightDoesNotChangeCylinderParams)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCylinder("NegCyl");
    auto* cyl = widget.getSelectedPrimitiveList()[0];

    Ogre::Real origHeight = cyl->getHeight();
    cyl->setHeight(-1.0);
    EXPECT_FLOAT_EQ(cyl->getHeight(), origHeight);

    cyl->setHeight(0.0);
    EXPECT_FLOAT_EQ(cyl->getHeight(), origHeight);

    Manager::getSingleton()->destroySceneNode("NegCyl");
}

TEST_F(PrimitivesWidgetTest, SetZeroNumSegDoesNotChangeParams)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("ZeroSegCube");
    auto* cube = widget.getSelectedPrimitiveList()[0];

    int origSegX = cube->getNumSegX();
    int origSegY = cube->getNumSegY();
    int origSegZ = cube->getNumSegZ();

    cube->setNumSegX(0);
    cube->setNumSegY(0);
    cube->setNumSegZ(0);

    EXPECT_EQ(cube->getNumSegX(), origSegX);
    EXPECT_EQ(cube->getNumSegY(), origSegY);
    EXPECT_EQ(cube->getNumSegZ(), origSegZ);

    cube->setNumSegX(-5);
    EXPECT_EQ(cube->getNumSegX(), origSegX);

    Manager::getSingleton()->destroySceneNode("ZeroSegCube");
}

TEST_F(PrimitivesWidgetTest, SetLargeParameterValuesSucceeds)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("BigCube");
    auto* cube = widget.getSelectedPrimitiveList()[0];

    cube->setSizeX(10000.0);
    cube->setSizeY(10000.0);
    cube->setSizeZ(10000.0);

    EXPECT_FLOAT_EQ(cube->getSizeX(), 10000.0f);
    EXPECT_FLOAT_EQ(cube->getSizeY(), 10000.0f);
    EXPECT_FLOAT_EQ(cube->getSizeZ(), 10000.0f);

    Manager::getSingleton()->destroySceneNode("BigCube");
}

// --- isPrimitive and getPrimitiveFromSceneNode tests ---

TEST_F(PrimitivesWidgetTest, IsPrimitiveReturnsTrueForPrimitive)
{
    auto* node = PrimitiveObject::createCube("IsPrimCube");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(PrimitiveObject::isPrimitive(node));

    Manager::getSingleton()->destroySceneNode("IsPrimCube");
}

TEST_F(PrimitivesWidgetTest, IsPrimitiveReturnsFalseForNonPrimitive)
{
    auto* node = Manager::getSingleton()->addSceneNode("NonPrimNode");
    auto mesh = createInMemoryTriangleMesh("NonPrimMesh");
    Manager::getSingleton()->createEntity(node, mesh);

    EXPECT_FALSE(PrimitiveObject::isPrimitive(node));

    Manager::getSingleton()->destroySceneNode("NonPrimNode");
}

TEST_F(PrimitivesWidgetTest, IsPrimitiveReturnsFalseForNull)
{
    EXPECT_FALSE(PrimitiveObject::isPrimitive(nullptr));
}

TEST_F(PrimitivesWidgetTest, GetPrimitiveFromSceneNodeReturnsCorrectObject)
{
    auto* node = PrimitiveObject::createSphere("GetPrimSphere");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(PrimitiveObject::isPrimitive(node));

    auto* prim = PrimitiveObject::getPrimitiveFromSceneNode(node);
    ASSERT_NE(prim, nullptr);
    EXPECT_EQ(prim->getType(), PrimitiveObject::AP_SPHERE);
    EXPECT_EQ(prim->getName(), "GetPrimSphere");

    Manager::getSingleton()->destroySceneNode("GetPrimSphere");
}

// --- Destroy and cleanup tests ---

TEST_F(PrimitivesWidgetTest, DestroyPrimitiveRemovesSceneNode)
{
    PrimitiveObject::createCube("DestroyCube");
    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("DestroyCube"));

    mgr->destroySceneNode("DestroyCube");
    EXPECT_FALSE(mgr->hasSceneNode("DestroyCube"));
}

TEST_F(PrimitivesWidgetTest, DestroyAllPrimitivesSequentially)
{
    PrimitiveObject::createCube("SeqCube");
    PrimitiveObject::createSphere("SeqSphere");
    PrimitiveObject::createCone("SeqCone");
    PrimitiveObject::createTorus("SeqTorus");

    auto* mgr = Manager::getSingleton();
    EXPECT_TRUE(mgr->hasSceneNode("SeqCube"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqSphere"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqCone"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqTorus"));

    mgr->destroySceneNode("SeqCube");
    EXPECT_FALSE(mgr->hasSceneNode("SeqCube"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqSphere"));

    mgr->destroySceneNode("SeqSphere");
    EXPECT_FALSE(mgr->hasSceneNode("SeqSphere"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqCone"));

    mgr->destroySceneNode("SeqCone");
    EXPECT_FALSE(mgr->hasSceneNode("SeqCone"));
    EXPECT_TRUE(mgr->hasSceneNode("SeqTorus"));

    mgr->destroySceneNode("SeqTorus");
    EXPECT_FALSE(mgr->hasSceneNode("SeqTorus"));
}

// --- updateUiFromParams with single selection shows correct values ---

TEST_F(PrimitivesWidgetTest, UpdateUiFromParamsShowsSphereValues)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSphere("ParamSphere");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("ParamSphere"));

    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");

    // Sphere defaults: radius=1.0, numSegX=16, numSegY=16
    EXPECT_DOUBLE_EQ(edit_radius->value(), 1.0);
    EXPECT_EQ(edit_numSegX->value(), 16);
    EXPECT_EQ(edit_numSegY->value(), 16);

    Manager::getSingleton()->destroySceneNode("ParamSphere");
}

// --- Selecting two primitives of different types yields AP_NONE ---

TEST_F(PrimitivesWidgetTest, SelectDifferentTypesPrimitivesShowsEmpty)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");

    PrimitiveObject::createCube("MixCube");
    PrimitiveObject::createSphere("MixSphere");

    // Select cube first, then append sphere (different types)
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("MixCube"));
    SelectionSet::getSingleton()->append(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("MixSphere"));

    // Different types should result in empty UI
    EXPECT_EQ(edit_type->text(), "");

    Manager::getSingleton()->destroySceneNode("MixCube");
    Manager::getSingleton()->destroySceneNode("MixSphere");
}

// --- Selecting two primitives of same type populates the widget ---

TEST_F(PrimitivesWidgetTest, SelectSameTypePrimitivesShowsType)
{
    PrimitivesWidget widget;
    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");

    PrimitiveObject::createCube("SameCube1");
    PrimitiveObject::createCube("SameCube2");

    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SameCube1"));
    SelectionSet::getSingleton()->append(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SameCube2"));

    // Same type should show the type
    EXPECT_EQ(edit_type->text(), "Cube");

    Manager::getSingleton()->destroySceneNode("SameCube1");
    Manager::getSingleton()->destroySceneNode("SameCube2");
}

// --- UV tile and switch parameter propagation ---

TEST_F(PrimitivesWidgetTest, SetUTileAndVTileOnSphere)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSphere("TileSphere");
    auto* sphere = widget.getSelectedPrimitiveList()[0];

    sphere->setUTile(3.5);
    sphere->setVTile(4.5);

    EXPECT_FLOAT_EQ(sphere->getUTile(), 3.5f);
    EXPECT_FLOAT_EQ(sphere->getVTile(), 4.5f);

    // Zero and negative should not change
    sphere->setUTile(0.0);
    sphere->setVTile(-1.0);
    EXPECT_FLOAT_EQ(sphere->getUTile(), 3.5f);
    EXPECT_FLOAT_EQ(sphere->getVTile(), 4.5f);

    Manager::getSingleton()->destroySceneNode("TileSphere");
}

TEST_F(PrimitivesWidgetTest, UVSwitchToggle)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("SwitchCube2");
    auto* cube = widget.getSelectedPrimitiveList()[0];

    EXPECT_FALSE(cube->hasUVSwitched());

    cube->setUVSwitch(true);
    EXPECT_TRUE(cube->hasUVSwitched());

    cube->setUVSwitch(false);
    EXPECT_FALSE(cube->hasUVSwitched());

    Manager::getSingleton()->destroySceneNode("SwitchCube2");
}

// --- InnerRadius guard: must be positive and less than outer radius ---

TEST_F(PrimitivesWidgetTest, SetInnerRadiusMustBeLessThanOuterRadius)
{
    PrimitivesWidget widget;
    PrimitiveObject::createTube("InnerTube");
    auto* tube = widget.getSelectedPrimitiveList()[0];

    // Tube defaults: radius=3.0 (outer), radius2=2.0 (inner)
    EXPECT_FLOAT_EQ(tube->getRadius(), 3.0f);
    EXPECT_FLOAT_EQ(tube->getInnerRadius(), 2.0f);

    // Set inner radius larger than outer -- should be rejected
    tube->setInnerRadius(5.0);
    EXPECT_FLOAT_EQ(tube->getInnerRadius(), 2.0f);

    // Set inner radius equal to outer -- should be rejected (guard is < not <=)
    tube->setInnerRadius(3.0);
    EXPECT_FLOAT_EQ(tube->getInnerRadius(), 2.0f);

    // Set a valid inner radius
    tube->setInnerRadius(1.5);
    EXPECT_FLOAT_EQ(tube->getInnerRadius(), 1.5f);

    Manager::getSingleton()->destroySceneNode("InnerTube");
}

// --- RoundedBox and Capsule UI layout verification ---

TEST_F(PrimitivesWidgetTest, RoundedBoxUiShowsSizeRadiusAndAllSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createRoundedBox("UiRBox");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiRBox"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_sizeY = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto* edit_sizeZ = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_radius = widget.findChild<QLabel*>("label_radius");

    EXPECT_EQ(edit_type->text(), "Rounded Box");
    EXPECT_FALSE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_sizeY->isHidden());
    EXPECT_FALSE(edit_sizeZ->isHidden());
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_FALSE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_radius->text(), "Chamfer");

    Manager::getSingleton()->destroySceneNode("UiRBox");
}

TEST_F(PrimitivesWidgetTest, CapsuleUiShowsRadiusHeightAndThreeSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCapsule("UiCapsule");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiCapsule"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegY = widget.findChild<QLabel*>("label_numSegY");
    auto* label_numSegZ = widget.findChild<QLabel*>("label_numSegZ");

    EXPECT_EQ(edit_type->text(), "Capsule");
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_TRUE(edit_radius2->isHidden());
    EXPECT_FALSE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_FALSE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Seg Ring");
    EXPECT_EQ(label_numSegY->text(), "Seg Loop");
    EXPECT_EQ(label_numSegZ->text(), "Seg Height");

    Manager::getSingleton()->destroySceneNode("UiCapsule");
}

// --- Multi-primitive: create, select different ones, verify params update ---

TEST_F(PrimitivesWidgetTest, SwitchSelectionBetweenPrimitivesUpdatesParams)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("ParCube");
    PrimitiveObject::createCylinder("ParCyl");

    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");

    // Select cube and verify size params are displayed
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("ParCube"));
    EXPECT_DOUBLE_EQ(edit_sizeX->value(), 2.0);

    // Switch to cylinder and verify radius/height params
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("ParCyl"));
    EXPECT_DOUBLE_EQ(edit_radius->value(), 1.0);
    EXPECT_DOUBLE_EQ(edit_height->value(), 3.0);

    Manager::getSingleton()->destroySceneNode("ParCube");
    Manager::getSingleton()->destroySceneNode("ParCyl");
}

// --- Plane UI test ---

TEST_F(PrimitivesWidgetTest, PlaneUiShowsTwoSizeFieldsAndTwoSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createPlane("UiPlane");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiPlane"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    auto* edit_sizeY = widget.findChild<QDoubleSpinBox*>("edit_sizeY");
    auto* edit_sizeZ = widget.findChild<QDoubleSpinBox*>("edit_sizeZ");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");

    EXPECT_EQ(edit_type->text(), "Plane");
    EXPECT_FALSE(edit_sizeX->isHidden());
    EXPECT_FALSE(edit_sizeY->isHidden());
    EXPECT_TRUE(edit_sizeZ->isHidden());
    EXPECT_TRUE(edit_radius->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_TRUE(edit_numSegZ->isHidden());

    Manager::getSingleton()->destroySceneNode("UiPlane");
}

TEST_F(PrimitivesWidgetTest, CreatePrimitiveSlotsUseDefaultNamesWhenAcceptedWithoutText)
{
    PrimitivesWidget widget;
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    for (const auto& testCase : kPrimitiveDialogCases) {
        bool handled = false;
        driveModalInputDialogResponse(handled, QString(), QDialog::Accepted);

        (widget.*(testCase.slot))();

        EXPECT_TRUE(handled) << testCase.defaultName.toStdString();
        ASSERT_TRUE(sceneMgr->hasSceneNode(testCase.defaultName.toStdString()))
            << testCase.defaultName.toStdString();
        Manager::getSingleton()->destroySceneNode(testCase.defaultName);
    }
}

TEST_F(PrimitivesWidgetTest, CreatePrimitiveSlotsUseCustomNamesWhenAccepted)
{
    PrimitivesWidget widget;
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    for (const auto& testCase : kPrimitiveDialogCases) {
        bool handled = false;
        driveModalInputDialogResponse(handled, testCase.customName, QDialog::Accepted);

        (widget.*(testCase.slot))();

        EXPECT_TRUE(handled) << testCase.customName.toStdString();
        ASSERT_TRUE(sceneMgr->hasSceneNode(testCase.customName.toStdString()))
            << testCase.customName.toStdString();
        Manager::getSingleton()->destroySceneNode(testCase.customName);
    }
}

TEST_F(PrimitivesWidgetTest, CreatePrimitiveSlotsRejectDialogDoesNotCreateNodes)
{
    PrimitivesWidget widget;
    const int initialNodeCount = Manager::getSingleton()->getSceneNodes().count();

    for (const auto& testCase : kPrimitiveDialogCases) {
        bool handled = false;
        driveModalInputDialogResponse(handled, QString(), QDialog::Rejected);

        (widget.*(testCase.slot))();

        EXPECT_TRUE(handled) << testCase.defaultName.toStdString();
        EXPECT_EQ(Manager::getSingleton()->getSceneNodes().count(), initialNodeCount);
        EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode(testCase.defaultName.toStdString()));
        EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode(testCase.customName.toStdString()));
    }
}

TEST_F(PrimitivesWidgetTest, ConeUiShowsRadiusHeightAndBaseHeightSegments)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCone("UiCone");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiCone"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegZ = widget.findChild<QLabel*>("label_numSegZ");

    EXPECT_EQ(edit_type->text(), "Cone");
    EXPECT_FALSE(edit_radius->isHidden());
    EXPECT_FALSE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_TRUE(edit_numSegY->isHidden());
    EXPECT_FALSE(edit_numSegZ->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Seg Base");
    EXPECT_EQ(label_numSegZ->text(), "Seg Height");

    Manager::getSingleton()->destroySceneNode("UiCone");
}

TEST_F(PrimitivesWidgetTest, SpringUiShowsOnlyMeshSegmentControls)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSpring("UiSpring");
    SelectionSet::getSingleton()->selectOne(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("UiSpring"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    auto* gb_Geometry = widget.findChild<QGroupBox*>("gb_Geometry");
    auto* gb_Mesh = widget.findChild<QGroupBox*>("gb_Mesh");
    auto* edit_radius = widget.findChild<QDoubleSpinBox*>("edit_radius");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    auto* edit_numSegZ = widget.findChild<QSpinBox*>("edit_numSegZ");
    auto* edit_UTile = widget.findChild<QDoubleSpinBox*>("edit_UTile");
    auto* edit_VTile = widget.findChild<QDoubleSpinBox*>("edit_VTile");
    auto* label_numSegX = widget.findChild<QLabel*>("label_numSegX");
    auto* label_numSegY = widget.findChild<QLabel*>("label_numSegY");

    EXPECT_EQ(edit_type->text(), "Spring");
    EXPECT_TRUE(gb_Geometry->isHidden());
    EXPECT_FALSE(gb_Mesh->isHidden());
    EXPECT_TRUE(edit_radius->isHidden());
    EXPECT_TRUE(edit_height->isHidden());
    EXPECT_FALSE(edit_numSegX->isHidden());
    EXPECT_FALSE(edit_numSegY->isHidden());
    EXPECT_TRUE(edit_numSegZ->isHidden());
    EXPECT_TRUE(edit_UTile->isHidden());
    EXPECT_TRUE(edit_VTile->isHidden());
    EXPECT_EQ(label_numSegX->text(), "Circle Segments");
    EXPECT_EQ(label_numSegY->text(), "Path Segments");

    Manager::getSingleton()->destroySceneNode("UiSpring");
}

TEST_F(PrimitivesWidgetTest, MixedPrimitiveAndPlainNodesStillTrackMatchingPrimitiveSelection)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("MixedCubeA");
    PrimitiveObject::createCube("MixedCubeB");
    auto* plainNode = createPlainNode("MixedPlainNode", "mixed_plain_node_mesh");
    ASSERT_NE(plainNode, nullptr);

    auto* selection = SelectionSet::getSingleton();
    selection->selectOne(plainNode);
    selection->append(Manager::getSingleton()->getSceneMgr()->getSceneNode("MixedCubeA"));
    selection->append(Manager::getSingleton()->getSceneMgr()->getSceneNode("MixedCubeB"));

    auto* edit_type = widget.findChild<QLineEdit*>("edit_type");
    ASSERT_NE(edit_type, nullptr);
    EXPECT_EQ(edit_type->text(), "Cube");

    const auto& selectedPrimitives = widget.getSelectedPrimitiveList();
    ASSERT_EQ(selectedPrimitives.count(), 2);
    EXPECT_EQ(selectedPrimitives[0]->getType(), PrimitiveObject::AP_CUBE);
    EXPECT_EQ(selectedPrimitives[1]->getType(), PrimitiveObject::AP_CUBE);

    Manager::getSingleton()->destroySceneNode("MixedCubeA");
    Manager::getSingleton()->destroySceneNode("MixedCubeB");
    Manager::getSingleton()->destroySceneNode("MixedPlainNode");
}

TEST_F(PrimitivesWidgetTest, MultipleSelectedCubesShowDashValuesAndShareSizeEdits)
{
    PrimitivesWidget widget;
    PrimitiveObject::createCube("DashCubeA");
    PrimitiveObject::createCube("DashCubeB");

    auto* cubeA = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("DashCubeA"));
    auto* cubeB = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("DashCubeB"));
    ASSERT_NE(cubeA, nullptr);
    ASSERT_NE(cubeB, nullptr);

    cubeA->setSizeX(2.5f);
    cubeB->setSizeX(5.5f);

    auto* selection = SelectionSet::getSingleton();
    selection->selectOne(Manager::getSingleton()->getSceneMgr()->getSceneNode("DashCubeA"));
    selection->append(Manager::getSingleton()->getSceneMgr()->getSceneNode("DashCubeB"));

    auto* edit_sizeX = widget.findChild<QDoubleSpinBox*>("edit_sizeX");
    ASSERT_NE(edit_sizeX, nullptr);

    EXPECT_EQ(edit_sizeX->specialValueText(), "-");
    EXPECT_DOUBLE_EQ(edit_sizeX->value(), edit_sizeX->minimum());

    edit_sizeX->setValue(7.25);
    EXPECT_FLOAT_EQ(cubeA->getSizeX(), 7.25f);
    EXPECT_FLOAT_EQ(cubeB->getSizeX(), 7.25f);

    Manager::getSingleton()->destroySceneNode("DashCubeA");
    Manager::getSingleton()->destroySceneNode("DashCubeB");
}

TEST_F(PrimitivesWidgetTest, MultipleSelectedTubesShareRadiusHeightAndUvEdits)
{
    PrimitivesWidget widget;
    PrimitiveObject::createTube("TubeEditA");
    PrimitiveObject::createTube("TubeEditB");

    auto* tubeA = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("TubeEditA"));
    auto* tubeB = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("TubeEditB"));
    ASSERT_NE(tubeA, nullptr);
    ASSERT_NE(tubeB, nullptr);

    auto* selection = SelectionSet::getSingleton();
    selection->selectOne(Manager::getSingleton()->getSceneMgr()->getSceneNode("TubeEditA"));
    selection->append(Manager::getSingleton()->getSceneMgr()->getSceneNode("TubeEditB"));

    auto* edit_radius2 = widget.findChild<QDoubleSpinBox*>("edit_radius2");
    auto* edit_height = widget.findChild<QDoubleSpinBox*>("edit_height");
    auto* toggle_uv = widget.findChild<QPushButton*>("pb_switchUV");
    ASSERT_NE(edit_radius2, nullptr);
    ASSERT_NE(edit_height, nullptr);
    ASSERT_NE(toggle_uv, nullptr);

    edit_radius2->setValue(0.25);
    edit_height->setValue(9.5);
    if (!toggle_uv->isChecked())
        toggle_uv->click();
    QCoreApplication::processEvents();

    EXPECT_FLOAT_EQ(tubeA->getInnerRadius(), 0.25f);
    EXPECT_FLOAT_EQ(tubeB->getInnerRadius(), 0.25f);

    Manager::getSingleton()->destroySceneNode("TubeEditA");
    Manager::getSingleton()->destroySceneNode("TubeEditB");
}

TEST_F(PrimitivesWidgetTest, MultipleSelectedSpringsShareSegmentEdits)
{
    PrimitivesWidget widget;
    PrimitiveObject::createSpring("SpringEditA");
    PrimitiveObject::createSpring("SpringEditB");

    auto* springA = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SpringEditA"));
    auto* springB = PrimitiveObject::getPrimitiveFromSceneNode(
        Manager::getSingleton()->getSceneMgr()->getSceneNode("SpringEditB"));
    ASSERT_NE(springA, nullptr);
    ASSERT_NE(springB, nullptr);

    auto* selection = SelectionSet::getSingleton();
    selection->selectOne(Manager::getSingleton()->getSceneMgr()->getSceneNode("SpringEditA"));
    selection->append(Manager::getSingleton()->getSceneMgr()->getSceneNode("SpringEditB"));

    auto* edit_numSegX = widget.findChild<QSpinBox*>("edit_numSegX");
    auto* edit_numSegY = widget.findChild<QSpinBox*>("edit_numSegY");
    ASSERT_NE(edit_numSegX, nullptr);
    ASSERT_NE(edit_numSegY, nullptr);

    edit_numSegX->setValue(12);
    edit_numSegY->setValue(42);
    QCoreApplication::processEvents();

    EXPECT_EQ(springA->getNumSegX(), 12);
    EXPECT_EQ(springB->getNumSegX(), 12);

    Manager::getSingleton()->destroySceneNode("SpringEditA");
    Manager::getSingleton()->destroySceneNode("SpringEditB");
}
