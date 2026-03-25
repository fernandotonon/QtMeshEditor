#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QSignalSpy>
#include <QThread>

#include "Manager.h"
#include "PrimitiveObject.h"
#include "PropertiesPanelController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TransformOperator.h"

class PropertiesPanelControllerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        originalPalette = app->palette();

        PropertiesPanelController::kill();
        TransformOperator::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }

        createStandardOgreMaterials();
        controller = PropertiesPanelController::instance();
        ASSERT_NE(controller, nullptr);
    }

    void TearDown() override
    {
        if (app) {
            app->setPalette(originalPalette);
            app->processEvents();
        }

        PropertiesPanelController::kill();
        TransformOperator::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);
    }

    Ogre::SceneNode* createSelectedNode(const QString& name)
    {
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(name);
        SelectionSet::getSingleton()->selectOne(node);
        return node;
    }

    QApplication* app = nullptr;
    QPalette originalPalette;
    PropertiesPanelController* controller = nullptr;
};

TEST_F(PropertiesPanelControllerTests, SingletonAndQmlInstanceShareObject)
{
    EXPECT_EQ(controller, PropertiesPanelController::instance());
    EXPECT_EQ(controller, PropertiesPanelController::qmlInstance(nullptr, nullptr));
    EXPECT_NE(controller->sceneTreeModel(), nullptr);
}

TEST_F(PropertiesPanelControllerTests, ThemeColorsTrackApplicationPalette)
{
    QPalette palette = app->palette();
    const QColor window(240, 241, 242);
    const QColor base(33, 44, 55);
    const QColor text(12, 23, 34);
    const QColor disabled(80, 81, 82);
    const QColor placeholder(90, 91, 92);
    const QColor highlight(120, 130, 140);
    const QColor highlightedText(220, 221, 222);
    const QColor button(150, 151, 152);
    const QColor buttonText(160, 161, 162);
    const QColor mid(170, 171, 172);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, highlightedText);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, buttonText);
    palette.setColor(QPalette::Mid, mid);

    app->setPalette(palette);

    EXPECT_EQ(controller->panelColor(), window);
    EXPECT_EQ(controller->headerColor(), window.darker(110));
    EXPECT_EQ(controller->textColor(), text);
    EXPECT_EQ(controller->borderColor(), mid);
    EXPECT_EQ(controller->inputColor(), base);
    EXPECT_EQ(controller->highlightColor(), highlight);
}

TEST_F(PropertiesPanelControllerTests, PaletteChangeEmitsThemeChanged)
{
    QSignalSpy themeSpy(controller, &PropertiesPanelController::themeChanged);
    ASSERT_TRUE(themeSpy.isValid());

    QPalette palette = app->palette();
    palette.setColor(QPalette::Window, QColor(10, 10, 10));
    app->setPalette(palette);
    app->processEvents();

    EXPECT_GE(themeSpy.count(), 1);
}

TEST_F(PropertiesPanelControllerTests, SelectionStateFollowsSelectedSceneNode)
{
    Ogre::SceneNode* node = createSelectedNode("PanelSelectionNode");

    EXPECT_TRUE(controller->hasSelection());
    EXPECT_FALSE(controller->hasEntitySelection());
    EXPECT_EQ(controller->selectionName(), QString("PanelSelectionNode"));
    EXPECT_TRUE(controller->sceneNodeNames().contains("PanelSelectionNode"));

    controller->selectNodeByName("PanelSelectionNode");
    EXPECT_EQ(SelectionSet::getSingleton()->getSceneNode(0), node);
}

TEST_F(PropertiesPanelControllerTests, TransformSettersUpdateSelectedNodeAndEmitSignal)
{
    Ogre::SceneNode* node = createSelectedNode("PanelTransformNode");
    QSignalSpy transformSpy(controller, &PropertiesPanelController::transformChanged);
    ASSERT_TRUE(transformSpy.isValid());

    controller->setPosX(5.0);
    controller->setPosY(6.0);
    controller->setPosZ(7.0);
    controller->setRotX(15.0);
    controller->setRotY(25.0);
    controller->setRotZ(35.0);
    controller->setScaleX(2.0);
    controller->setScaleY(3.0);
    controller->setScaleZ(4.0);

    EXPECT_FLOAT_EQ(node->getPosition().x, 5.0f);
    EXPECT_FLOAT_EQ(node->getPosition().y, 6.0f);
    EXPECT_FLOAT_EQ(node->getPosition().z, 7.0f);
    EXPECT_FLOAT_EQ(controller->rotX(), 15.0f);
    EXPECT_FLOAT_EQ(controller->rotY(), 25.0f);
    EXPECT_FLOAT_EQ(controller->rotZ(), 35.0f);
    EXPECT_FLOAT_EQ(node->getScale().x, 2.0f);
    EXPECT_FLOAT_EQ(node->getScale().y, 3.0f);
    EXPECT_FLOAT_EQ(node->getScale().z, 4.0f);
    EXPECT_GE(transformSpy.count(), 9);

    const int stableCount = transformSpy.count();
    controller->setPosX(5.0);
    controller->setRotY(25.0);
    controller->setScaleZ(4.0);
    EXPECT_EQ(transformSpy.count(), stableCount);
}

TEST_F(PropertiesPanelControllerTests, PrimitiveMetadataForCubeMatchesExpectedFields)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SceneNode* cubeNode = PrimitiveObject::createCube("PanelCube");
    ASSERT_NE(cubeNode, nullptr);
    SelectionSet::getSingleton()->selectOne(cubeNode);

    EXPECT_TRUE(controller->hasPrimitive());
    EXPECT_EQ(controller->primitiveType(), QString("Cube"));
    EXPECT_FLOAT_EQ(controller->primSizeX(), 2.0f);
    EXPECT_FLOAT_EQ(controller->primSizeY(), 2.0f);
    EXPECT_FLOAT_EQ(controller->primSizeZ(), 2.0f);
    EXPECT_EQ(controller->primUTile(), 1.0);
    EXPECT_EQ(controller->primVTile(), 1.0);

    const QVariantMap cfg = controller->primFieldConfig();
    EXPECT_TRUE(cfg.value("showSizeX").toBool());
    EXPECT_TRUE(cfg.value("showSizeY").toBool());
    EXPECT_TRUE(cfg.value("showSizeZ").toBool());
    EXPECT_FALSE(cfg.value("showRadius").toBool());
    EXPECT_FALSE(cfg.value("showRadius2").toBool());
    EXPECT_FALSE(cfg.value("showHeight").toBool());
    EXPECT_TRUE(cfg.value("showSegX").toBool());
    EXPECT_TRUE(cfg.value("showSegY").toBool());
    EXPECT_TRUE(cfg.value("showSegZ").toBool());
    EXPECT_TRUE(cfg.value("showUV").toBool());
    EXPECT_EQ(cfg.value("segXLabel").toString(), QString("X"));
    EXPECT_EQ(cfg.value("segYLabel").toString(), QString("Y"));
    EXPECT_EQ(cfg.value("segZLabel").toString(), QString("Z"));
}

TEST_F(PropertiesPanelControllerTests, PrimitiveMetadataForTorusUsesRadiusLabels)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SceneNode* torusNode = PrimitiveObject::createTorus("PanelTorus");
    ASSERT_NE(torusNode, nullptr);
    SelectionSet::getSingleton()->selectOne(torusNode);

    EXPECT_EQ(controller->primitiveType(), QString("Torus"));

    const QVariantMap cfg = controller->primFieldConfig();
    EXPECT_FALSE(cfg.value("showSizeX").toBool());
    EXPECT_TRUE(cfg.value("showRadius").toBool());
    EXPECT_TRUE(cfg.value("showRadius2").toBool());
    EXPECT_FALSE(cfg.value("showHeight").toBool());
    EXPECT_TRUE(cfg.value("showSegX").toBool());
    EXPECT_TRUE(cfg.value("showSegY").toBool());
    EXPECT_FALSE(cfg.value("showSegZ").toBool());
    EXPECT_EQ(cfg.value("radiusLabel").toString(), QString("Radius"));
    EXPECT_EQ(cfg.value("radius2Label").toString(), QString("Section R"));
    EXPECT_EQ(cfg.value("segXLabel").toString(), QString("Circle"));
    EXPECT_EQ(cfg.value("segYLabel").toString(), QString("Section"));
}

TEST_F(PropertiesPanelControllerTests, PrimitiveSettersMutateSelectedPrimitiveAndEmitSignal)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SceneNode* cubeNode = PrimitiveObject::createCube("PanelPrimitiveMutation");
    ASSERT_NE(cubeNode, nullptr);
    SelectionSet::getSingleton()->selectOne(cubeNode);

    QSignalSpy primitiveSpy(controller, &PropertiesPanelController::primitiveChanged);
    ASSERT_TRUE(primitiveSpy.isValid());

    controller->setPrimSizeX(4.0);
    controller->setPrimSizeY(5.0);
    controller->setPrimSizeZ(6.0);
    controller->setPrimSegX(7);
    controller->setPrimSegY(8);
    controller->setPrimSegZ(9);
    controller->setPrimUTile(2.5);
    controller->setPrimVTile(3.5);

    EXPECT_FLOAT_EQ(controller->primSizeX(), 4.0f);
    EXPECT_FLOAT_EQ(controller->primSizeY(), 5.0f);
    EXPECT_FLOAT_EQ(controller->primSizeZ(), 6.0f);
    EXPECT_EQ(controller->primSegX(), 7);
    EXPECT_EQ(controller->primSegY(), 8);
    EXPECT_EQ(controller->primSegZ(), 9);
    EXPECT_DOUBLE_EQ(controller->primUTile(), 2.5);
    EXPECT_DOUBLE_EQ(controller->primVTile(), 3.5);
    EXPECT_EQ(primitiveSpy.count(), 8);
}

TEST_F(PropertiesPanelControllerTests, EmptySelectionReturnsNeutralValues)
{
    SelectionSet::getSingleton()->clear();

    EXPECT_FALSE(controller->hasSelection());
    EXPECT_FALSE(controller->hasEntitySelection());
    EXPECT_FALSE(controller->hasPrimitive());
    EXPECT_TRUE(controller->selectionName().isEmpty());
    EXPECT_TRUE(controller->primitiveType().isEmpty());
    EXPECT_DOUBLE_EQ(controller->primSizeX(), 0.0);
    EXPECT_DOUBLE_EQ(controller->primRadius(), 0.0);
    EXPECT_DOUBLE_EQ(controller->primUTile(), 1.0);
    EXPECT_TRUE(controller->primFieldConfig().isEmpty());
}

TEST_F(PropertiesPanelControllerTests, PlayingStateOnlyEmitsWhenValueChanges)
{
    QSignalSpy playingSpy(controller, &PropertiesPanelController::playingChanged);
    ASSERT_TRUE(playingSpy.isValid());

    EXPECT_FALSE(controller->isPlaying());

    controller->setPlaying(true);
    controller->setPlaying(true);
    controller->setPlaying(false);

    EXPECT_FALSE(controller->isPlaying());
    EXPECT_EQ(playingSpy.count(), 2);
}
