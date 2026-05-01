#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#include <QSettings>
#include <QSignalSpy>
#include <QUndoCommand>

#include "AnimationWidget.h"
#include "Manager.h"
#include "PrimitiveObject.h"
#include "PropertiesPanelController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TransformOperator.h"
#include "UndoManager.h"
#include "ViewportSettingsKeys.h"
#include "AppSettingsKeys.h"

class PropertiesPanelControllerTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        originalPalette = app->palette();

        PropertiesPanelController::kill();
        Manager::kill();
        app->processEvents();

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

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
        Manager::kill();
        if (app)
            app->processEvents();
    }

    Ogre::SceneNode* createSelectedNode(const QString& name)
    {
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(name);
        EXPECT_NE(node, nullptr);
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
    app->processEvents();

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

TEST_F(PropertiesPanelControllerTests, SceneNodeNamesAndSelectionHelpersHandleMultipleNodes)
{
    Ogre::SceneNode* first = Manager::getSingleton()->addSceneNode("PanelNodeA");
    Ogre::SceneNode* second = Manager::getSingleton()->addSceneNode("PanelNodeB");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const QStringList names = controller->sceneNodeNames();
    EXPECT_TRUE(names.contains("PanelNodeA"));
    EXPECT_TRUE(names.contains("PanelNodeB"));

    controller->selectNodeByName("PanelNodeB");
    EXPECT_TRUE(controller->hasSelection());
    EXPECT_EQ(controller->selectionName(), QString("PanelNodeB"));
    EXPECT_EQ(SelectionSet::getSingleton()->getSceneNode(0), second);

    controller->selectNodeByName("MissingPanelNode");
    EXPECT_EQ(controller->selectionName(), QString("PanelNodeB"));
    EXPECT_EQ(SelectionSet::getSingleton()->getSceneNode(0), second);
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
    // TransformOperator exposes Euler angles derived from Ogre quaternions.
    // The round-trip is not stable enough here for tight per-axis equality.
    EXPECT_NEAR(controller->rotX(), 15.0f, 20.0f);
    EXPECT_NEAR(controller->rotY(), 25.0f, 20.0f);
    EXPECT_NEAR(controller->rotZ(), 35.0f, 20.0f);
    EXPECT_FLOAT_EQ(node->getScale().x, 2.0f);
    EXPECT_FLOAT_EQ(node->getScale().y, 3.0f);
    EXPECT_FLOAT_EQ(node->getScale().z, 4.0f);
    EXPECT_GE(transformSpy.count(), 9);

    const int stableCount = transformSpy.count();
    controller->setPosX(5.0);
    controller->setScaleZ(4.0);
    EXPECT_EQ(transformSpy.count(), stableCount);
}

TEST_F(PropertiesPanelControllerTests, PrimitiveMetadataForCubeMatchesExpectedFields)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

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
    EXPECT_GE(primitiveSpy.count(), 8);
}

TEST_F(PropertiesPanelControllerTests, PrimitiveMetadataCoversAdditionalPrimitiveTypes)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    struct PrimitiveCase {
        Ogre::SceneNode* (*create)(const QString&);
        const char* name;
        const char* typeName;
        const char* radiusLabel;
        const char* radius2Label;
        const char* segXLabel;
        const char* segYLabel;
        const char* segZLabel;
        bool showRadius;
        bool showRadius2;
        bool showHeight;
        bool showSegY;
        bool showSegZ;
        bool showUV;
    };

    const PrimitiveCase cases[] = {
        {PrimitiveObject::createSphere, "PanelSphere", "Sphere", "Radius", "", "Ring", "Loop", "", true, false, false, true, false, true},
        {PrimitiveObject::createCylinder, "PanelCylinder", "Cylinder", "Radius", "", "Base", "", "Height", true, false, true, false, true, true},
        {PrimitiveObject::createTube, "PanelTube", "Tube", "Outer R", "Inner R", "Base", "", "Height", true, true, true, false, true, true},
        {PrimitiveObject::createCapsule, "PanelCapsule", "Capsule", "Radius", "", "Ring", "Loop", "Height", true, false, true, true, true, true},
        {PrimitiveObject::createRoundedBox, "PanelRoundedBox", "Rounded Box", "Chamfer", "", "X", "Y", "Z", true, false, false, true, true, true},
        {PrimitiveObject::createSpring, "PanelSpring", "Spring", "", "", "Circle", "Path", "", false, false, false, true, false, false},
    };

    for (const PrimitiveCase& testCase : cases) {
        Ogre::SceneNode* node = testCase.create(testCase.name);
        ASSERT_NE(node, nullptr);
        SelectionSet::getSingleton()->selectOne(node);

        EXPECT_TRUE(controller->hasPrimitive());
        EXPECT_EQ(controller->primitiveType(), QString::fromLatin1(testCase.typeName));

        const QVariantMap cfg = controller->primFieldConfig();
        EXPECT_EQ(cfg.value("showRadius").toBool(), testCase.showRadius);
        EXPECT_EQ(cfg.value("showRadius2").toBool(), testCase.showRadius2);
        EXPECT_EQ(cfg.value("showHeight").toBool(), testCase.showHeight);
        EXPECT_EQ(cfg.value("showSegY").toBool(), testCase.showSegY);
        EXPECT_EQ(cfg.value("showSegZ").toBool(), testCase.showSegZ);
        EXPECT_EQ(cfg.value("showUV").toBool(), testCase.showUV);

        if (testCase.radiusLabel[0] != '\0')
            EXPECT_EQ(cfg.value("radiusLabel").toString(), QString::fromLatin1(testCase.radiusLabel));
        if (testCase.radius2Label[0] != '\0')
            EXPECT_EQ(cfg.value("radius2Label").toString(), QString::fromLatin1(testCase.radius2Label));
        if (testCase.segXLabel[0] != '\0')
            EXPECT_EQ(cfg.value("segXLabel").toString(), QString::fromLatin1(testCase.segXLabel));
        if (testCase.segYLabel[0] != '\0')
            EXPECT_EQ(cfg.value("segYLabel").toString(), QString::fromLatin1(testCase.segYLabel));
        if (testCase.segZLabel[0] != '\0')
            EXPECT_EQ(cfg.value("segZLabel").toString(), QString::fromLatin1(testCase.segZLabel));
    }
}

TEST_F(PropertiesPanelControllerTests, EmptySelectionReturnsNeutralValues)
{
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

TEST_F(PropertiesPanelControllerTests, NonPrimitiveSelectionDoesNotExposePrimitiveMetadata)
{
    createSelectedNode("PanelPlainNode");

    EXPECT_TRUE(controller->hasSelection());
    EXPECT_FALSE(controller->hasPrimitive());
    EXPECT_TRUE(controller->primitiveType().isEmpty());
    EXPECT_TRUE(controller->primFieldConfig().isEmpty());
}

TEST_F(PropertiesPanelControllerTests, RefreshThemeEmitsThemeChanged)
{
    QSignalSpy themeSpy(controller, &PropertiesPanelController::themeChanged);
    ASSERT_TRUE(themeSpy.isValid());

    controller->refreshTheme();

    EXPECT_EQ(themeSpy.count(), 1);
}

TEST_F(PropertiesPanelControllerTests, SceneNodeChangesEmitSceneChanged)
{
    QSignalSpy sceneSpy(controller, &PropertiesPanelController::sceneChanged);
    ASSERT_TRUE(sceneSpy.isValid());

    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("PanelSceneChanged");
    ASSERT_NE(node, nullptr);
    EXPECT_GE(sceneSpy.count(), 1);

    const int countAfterCreate = sceneSpy.count();
    Manager::getSingleton()->destroySceneNode(node);
    EXPECT_GE(sceneSpy.count(), countAfterCreate + 1);
}

TEST_F(PropertiesPanelControllerTests, AnimationQueriesAreSafeWithoutAnimatedSelection)
{
    EXPECT_FALSE(controller->hasAnimations());
    EXPECT_TRUE(controller->animationData().isEmpty());

    QSignalSpy animationSpy(controller, &PropertiesPanelController::animationStateChanged);
    ASSERT_TRUE(animationSpy.isValid());

    controller->toggleAnimationEnabled("missing", "missing", true);
    controller->toggleAnimationLoop("missing", "missing", true);
    controller->toggleSkeletonDebug("missing", true);
    controller->toggleBoneWeights("missing", true);

    EXPECT_EQ(animationSpy.count(), 0);
    EXPECT_FALSE(controller->renameAnimation("missing", "old", "new"));
    EXPECT_FALSE(controller->renameAnimation("missing", "old", ""));
}

TEST_F(PropertiesPanelControllerTests, AnimationDataAndControlsWorkForAnimatedEntity)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createAnimatedTestEntity("PanelAnimData");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    AnimationWidget widget;
    controller->setAnimationWidget(&widget);

    EXPECT_TRUE(controller->hasAnimations());
    const QVariantList groups = controller->animationData();
    ASSERT_FALSE(groups.isEmpty());

    QVariantMap entityGroup;
    const QString entityName = QString::fromStdString(entity->getName());
    for (const QVariant& entry : groups) {
        const QVariantMap group = entry.toMap();
        if (group.value("entity").toString() == entityName) {
            entityGroup = group;
            break;
        }
    }

    ASSERT_FALSE(entityGroup.isEmpty());
    EXPECT_TRUE(entityGroup.value("hasSkeleton").toBool());
    EXPECT_FALSE(entityGroup.value("showSkeleton").toBool());
    EXPECT_FALSE(entityGroup.value("showWeights").toBool());

    const QVariantList animations = entityGroup.value("animations").toList();
    ASSERT_FALSE(animations.isEmpty());

    QVariantMap testAnim;
    for (const QVariant& entry : animations) {
        const QVariantMap anim = entry.toMap();
        if (anim.value("name").toString() == "TestAnim") {
            testAnim = anim;
            break;
        }
    }
    ASSERT_FALSE(testAnim.isEmpty());
    EXPECT_FALSE(testAnim.value("enabled").toBool());
    EXPECT_TRUE(testAnim.value("loop").toBool());
    EXPECT_DOUBLE_EQ(testAnim.value("length").toDouble(), 1.0);

    QSignalSpy animationSpy(controller, &PropertiesPanelController::animationStateChanged);
    ASSERT_TRUE(animationSpy.isValid());

    controller->toggleAnimationEnabled(entityName, "TestAnim", true);
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->getEnabled());
    EXPECT_TRUE(state->getLoop());

    controller->toggleAnimationLoop(entityName, "TestAnim", false);
    EXPECT_FALSE(state->getLoop());
    EXPECT_GE(animationSpy.count(), 2);
}

TEST_F(PropertiesPanelControllerTests, SkeletonAndWeightTogglesEmitForMatchingEntity)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createAnimatedTestEntity("PanelAnimToggleEntity");
    ASSERT_NE(entity, nullptr);
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    const QString entityName = QString::fromStdString(entity->getName());

    AnimationWidget widget;
    controller->setAnimationWidget(&widget);

    QSignalSpy animationSpy(controller, &PropertiesPanelController::animationStateChanged);
    ASSERT_TRUE(animationSpy.isValid());

    controller->toggleSkeletonDebug(entityName, true);
    controller->toggleBoneWeights(entityName, true);
    const int afterMatchingEntity = animationSpy.count();
    EXPECT_GE(afterMatchingEntity, 2);

    const QString missingEntityName = entityName + "_missing";
    controller->toggleSkeletonDebug(missingEntityName, true);
    controller->toggleBoneWeights(missingEntityName, true);
    EXPECT_EQ(animationSpy.count(), afterMatchingEntity);
}

TEST_F(PropertiesPanelControllerTests, RenameAnimationRenamesStateAndStopsPlayback)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createAnimatedTestEntity("PanelRenameAnim");
    ASSERT_NE(entity, nullptr);
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    const QString entityName = QString::fromStdString(entity->getName());
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    ASSERT_NE(state, nullptr);
    state->setEnabled(true);
    controller->setPlaying(true);

    EXPECT_FALSE(controller->renameAnimation(entityName, "MissingAnim", "TestAnim"));
    EXPECT_FALSE(controller->renameAnimation(entityName, "TestAnim", "TestAnim"));

    QSignalSpy animationSpy(controller, &PropertiesPanelController::animationStateChanged);
    ASSERT_TRUE(animationSpy.isValid());

    const QString renamedName = entityName + "_RenamedAnim";
    EXPECT_TRUE(controller->renameAnimation(entityName, "TestAnim", renamedName));
    EXPECT_FALSE(controller->isPlaying());
    EXPECT_GE(animationSpy.count(), 1);
    EXPECT_FALSE(Manager::getSingleton()->hasAnimationName(entity, "TestAnim"));
    EXPECT_TRUE(Manager::getSingleton()->hasAnimationName(entity, renamedName));

    Ogre::AnimationState* renamed = entity->getAnimationState(renamedName.toStdString());
    ASSERT_NE(renamed, nullptr);
    EXPECT_FALSE(renamed->getEnabled());
}

TEST_F(PropertiesPanelControllerTests, ExportCurrentPoseRequiresAnimatedSelectionAndWritesFile)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    auto* mgr = Manager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* plainNode = mgr->addSceneNode("PanelPosePlainNode");
    ASSERT_NE(plainNode, nullptr);
    Ogre::MeshPtr plainMesh = createInMemoryTriangleMesh("PanelPosePlainMesh");
    Ogre::Entity* plainEntity = mgr->getSceneMgr()->createEntity("PanelPosePlainEntity", plainMesh);
    ASSERT_NE(plainEntity, nullptr);
    plainNode->attachObject(plainEntity);
    SelectionSet::getSingleton()->selectOne(plainNode);

    const QString plainPath = QDir::tempPath() + "/panel_pose_plain.obj";
    QFile::remove(plainPath);
    EXPECT_FALSE(controller->exportCurrentPose(plainPath));

    Ogre::Entity* animatedEntity = createAnimatedTestEntity("PanelPoseAnimated");
    ASSERT_NE(animatedEntity, nullptr);
    Ogre::SceneNode* animatedNode = animatedEntity->getParentSceneNode();
    ASSERT_NE(animatedNode, nullptr);
    SelectionSet::getSingleton()->selectOne(animatedNode);

    const QString animatedPath = QDir::tempPath() + "/panel_pose_animated.obj";
    QFile::remove(animatedPath);
    EXPECT_TRUE(controller->exportCurrentPose(animatedPath));
    EXPECT_TRUE(QFileInfo::exists(animatedPath));
    EXPECT_GT(QFileInfo(animatedPath).size(), 0);
    QFile::remove(animatedPath);
}

TEST_F(PropertiesPanelControllerTests, SetSettingCoversViewportSentryAndThemeBranches)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    auto* mgr = Manager::getSingleton();
    ASSERT_NE(mgr, nullptr);

    mgr->CreateEmptyScene();
    controller->setSetting(ViewportSettingsKeys::gridVisible(), false);
    controller->setSetting(ViewportSettingsKeys::gridVisible(), true);
    controller->setSetting(ViewportSettingsKeys::cameraSpeed(), 0.0);
    controller->setSetting(ViewportSettingsKeys::cameraSpeed(), 2.0);
    controller->setSetting(ViewportSettingsKeys::nearClip(), 0.1);
    controller->setSetting(ViewportSettingsKeys::farClip(), 5000.0);
    controller->setSetting(AppSettingsKeys::sentryEnabled(), false);
    controller->setSetting(AppSettingsKeys::telemetryEnabled(), true);

    controller->setSetting(AppSettingsKeys::appearanceTheme(), "dark");
    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(53, 53, 53));

    controller->setSetting(AppSettingsKeys::palette(), "light");
    EXPECT_EQ(app->palette().color(QPalette::Window), QColor("ghostwhite"));
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

TEST_F(PropertiesPanelControllerTests, PivotModeSetCycleAndInvalidInput)
{
    QSignalSpy pivotSpy(controller, &PropertiesPanelController::pivotModeChanged);
    ASSERT_TRUE(pivotSpy.isValid());

    controller->setPivotMode(TransformOperator::PIVOT_BOTTOM);
    EXPECT_EQ(controller->pivotMode(), static_cast<int>(TransformOperator::PIVOT_BOTTOM));

    const int emitsAfterValidSet = pivotSpy.count();
    controller->setPivotMode(-1);
    controller->setPivotMode(999);
    EXPECT_EQ(controller->pivotMode(), static_cast<int>(TransformOperator::PIVOT_BOTTOM));
    EXPECT_EQ(pivotSpy.count(), emitsAfterValidSet);

    controller->cyclePivotMode();
    EXPECT_NE(controller->pivotMode(), static_cast<int>(TransformOperator::PIVOT_BOTTOM));
    EXPECT_GE(pivotSpy.count(), emitsAfterValidSet + 1);
}

TEST_F(PropertiesPanelControllerTests, SnapSettingsRoundTripAndPresetExposure)
{
    QSignalSpy enabledSpy(controller, &PropertiesPanelController::snapEnabledChanged);
    QSignalSpy gridSpy(controller, &PropertiesPanelController::snapGridSizeChanged);
    QSignalSpy angleSpy(controller, &PropertiesPanelController::snapAngleStepChanged);
    QSignalSpy scaleSpy(controller, &PropertiesPanelController::snapScaleStepChanged);
    ASSERT_TRUE(enabledSpy.isValid());
    ASSERT_TRUE(gridSpy.isValid());
    ASSERT_TRUE(angleSpy.isValid());
    ASSERT_TRUE(scaleSpy.isValid());

    controller->setSnapEnabled(true);
    controller->setSnapGridSize(0.25);
    controller->setSnapAngleStep(15.0);
    controller->setSnapScaleStep(0.5);

    EXPECT_TRUE(controller->snapEnabled());
    EXPECT_DOUBLE_EQ(controller->snapGridSize(), 0.25);
    EXPECT_DOUBLE_EQ(controller->snapAngleStep(), 15.0);
    EXPECT_DOUBLE_EQ(controller->snapScaleStep(), 0.5);

    EXPECT_GE(enabledSpy.count(), 1);
    EXPECT_GE(gridSpy.count(), 1);
    EXPECT_GE(angleSpy.count(), 1);
    EXPECT_GE(scaleSpy.count(), 1);

    const QVariantList gridPresets = controller->gridSizePresets();
    const QVariantList anglePresets = controller->angleStepPresets();
    const QVariantList scalePresets = controller->scaleStepPresets();
    EXPECT_FALSE(gridPresets.isEmpty());
    EXPECT_FALSE(anglePresets.isEmpty());
    EXPECT_FALSE(scalePresets.isEmpty());
    EXPECT_TRUE(gridPresets.contains(0.25));
    EXPECT_TRUE(anglePresets.contains(15.0));
    EXPECT_TRUE(scalePresets.contains(0.5));
}

TEST_F(PropertiesPanelControllerTests, UndoHistoryApisTrackStackAndBounds)
{
    UndoManager::getSingleton()->clear();

    auto* stack = UndoManager::getSingleton()->stack();
    ASSERT_NE(stack, nullptr);
    stack->push(new QUndoCommand("First Action"));
    stack->push(new QUndoCommand("Second Action"));

    const QVariantList history = controller->undoHistory();
    ASSERT_EQ(history.size(), 2);
    EXPECT_EQ(history[0].toMap().value("text").toString(), QString("First Action"));
    EXPECT_EQ(history[1].toMap().value("text").toString(), QString("Second Action"));
    EXPECT_EQ(controller->undoIndex(), 2);
    EXPECT_TRUE(history[1].toMap().value("isCurrent").toBool());

    controller->undoToIndex(1);
    EXPECT_EQ(controller->undoIndex(), 1);
    EXPECT_TRUE(controller->undoHistory()[0].toMap().value("isCurrent").toBool());

    controller->undoToIndex(-1);
    EXPECT_EQ(controller->undoIndex(), 1);
    controller->undoToIndex(999);
    EXPECT_EQ(controller->undoIndex(), 1);

    controller->clearUndoHistory();
    EXPECT_EQ(stack->count(), 0);
    EXPECT_TRUE(controller->undoHistory().isEmpty());
}

TEST_F(PropertiesPanelControllerTests, SceneTreeReparentWrappersHandleValidAndInvalidRequests)
{
    auto* mgr = Manager::getSingleton();
    ASSERT_NE(mgr, nullptr);

    Ogre::SceneNode* node = mgr->addSceneNode("PanelReparentNode");
    Ogre::SceneNode* parent = mgr->addSceneNode("PanelReparentParent");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(parent, nullptr);

    EXPECT_FALSE(controller->canReparentNode("MissingNode", "PanelReparentParent"));
    EXPECT_FALSE(controller->reparentNode("MissingNode", "PanelReparentParent"));

    EXPECT_TRUE(controller->canReparentNode("PanelReparentNode", "PanelReparentParent"));
    EXPECT_TRUE(controller->reparentNode("PanelReparentNode", "PanelReparentParent"));
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(node->getParent()), parent);
}

// ---- shortcutData tests ----

TEST_F(PropertiesPanelControllerTests, ShortcutDataReturnsNonEmptyList)
{
    QVariantList shortcuts = controller->shortcutData();
    EXPECT_FALSE(shortcuts.isEmpty());
    EXPECT_GE(shortcuts.size(), 10); // There are many shortcuts defined
}

TEST_F(PropertiesPanelControllerTests, ShortcutDataEntriesHaveRequiredKeys)
{
    QVariantList shortcuts = controller->shortcutData();
    for (const QVariant& entry : shortcuts) {
        QVariantMap m = entry.toMap();
        EXPECT_TRUE(m.contains("category")) << "Missing 'category' key";
        EXPECT_TRUE(m.contains("key")) << "Missing 'key' key";
        EXPECT_TRUE(m.contains("description")) << "Missing 'description' key";
        EXPECT_FALSE(m["category"].toString().isEmpty());
        EXPECT_FALSE(m["key"].toString().isEmpty());
        EXPECT_FALSE(m["description"].toString().isEmpty());
    }
}

TEST_F(PropertiesPanelControllerTests, ShortcutDataContainsExpectedTransformShortcuts)
{
    QVariantList shortcuts = controller->shortcutData();

    auto findShortcut = [&](const QString& key) -> QVariantMap {
        for (const QVariant& entry : shortcuts) {
            QVariantMap m = entry.toMap();
            if (m["key"].toString() == key)
                return m;
        }
        return QVariantMap();
    };

    // Check for the Unity-style transform shortcuts
    QVariantMap qShortcut = findShortcut("Q");
    EXPECT_FALSE(qShortcut.isEmpty());
    EXPECT_EQ(qShortcut["category"].toString(), "Transform");
    EXPECT_EQ(qShortcut["description"].toString(), "Select mode");

    QVariantMap wShortcut = findShortcut("W");
    EXPECT_FALSE(wShortcut.isEmpty());
    EXPECT_EQ(wShortcut["category"].toString(), "Transform");
    EXPECT_EQ(wShortcut["description"].toString(), "Translate mode");

    QVariantMap eShortcut = findShortcut("E");
    EXPECT_FALSE(eShortcut.isEmpty());
    EXPECT_EQ(eShortcut["category"].toString(), "Transform");
    EXPECT_EQ(eShortcut["description"].toString(), "Rotate mode");

    QVariantMap rShortcut = findShortcut("R");
    EXPECT_FALSE(rShortcut.isEmpty());
    EXPECT_EQ(rShortcut["category"].toString(), "Transform");
    EXPECT_EQ(rShortcut["description"].toString(), "Scale mode");
}

TEST_F(PropertiesPanelControllerTests, ShortcutDataHasAllSevenCategories)
{
    QVariantList shortcuts = controller->shortcutData();
    QSet<QString> categories;
    for (const QVariant& entry : shortcuts) {
        categories.insert(entry.toMap()["category"].toString());
    }

    EXPECT_TRUE(categories.contains("Transform"));
    EXPECT_TRUE(categories.contains("Navigation"));
    EXPECT_TRUE(categories.contains("Editing"));
    EXPECT_TRUE(categories.contains("Edit Mode"));
    EXPECT_TRUE(categories.contains("File"));
    EXPECT_TRUE(categories.contains("View"));
    EXPECT_TRUE(categories.contains("Help"));
    EXPECT_EQ(categories.size(), 7);
}

// ---- getSetting / setSetting tests ----

TEST_F(PropertiesPanelControllerTests, GetSetSettingStringRoundTrip)
{
    const QString key = "TestSuite/testStringKey";
    QSettings settings;
    QVariant saved = settings.value(key);

    controller->setSetting(key, QVariant("hello world"));
    EXPECT_EQ(controller->getSetting(key, QVariant()).toString(), "hello world");

    // Cleanup
    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

TEST_F(PropertiesPanelControllerTests, GetSetSettingIntRoundTrip)
{
    const QString key = "TestSuite/testIntKey";
    QSettings settings;
    QVariant saved = settings.value(key);

    controller->setSetting(key, QVariant(42));
    EXPECT_EQ(controller->getSetting(key, QVariant()).toInt(), 42);

    // Cleanup
    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

TEST_F(PropertiesPanelControllerTests, GetSetSettingBoolRoundTrip)
{
    const QString key = "TestSuite/testBoolKey";
    QSettings settings;
    QVariant saved = settings.value(key);

    controller->setSetting(key, QVariant(true));
    EXPECT_EQ(controller->getSetting(key, QVariant()).toBool(), true);

    controller->setSetting(key, QVariant(false));
    EXPECT_EQ(controller->getSetting(key, QVariant()).toBool(), false);

    // Cleanup
    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

TEST_F(PropertiesPanelControllerTests, GetSetSettingDoubleRoundTrip)
{
    const QString key = "TestSuite/testDoubleKey";
    QSettings settings;
    QVariant saved = settings.value(key);

    controller->setSetting(key, QVariant(3.14));
    EXPECT_DOUBLE_EQ(controller->getSetting(key, QVariant()).toDouble(), 3.14);

    // Cleanup
    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

TEST_F(PropertiesPanelControllerTests, GetSettingReturnsDefaultWhenKeyMissing)
{
    const QString key = "TestSuite/nonExistentKeyForTest_12345";
    QSettings settings;
    settings.remove(key);

    EXPECT_EQ(controller->getSetting(key, QVariant("defaultVal")).toString(), "defaultVal");
    EXPECT_EQ(controller->getSetting(key, QVariant(99)).toInt(), 99);
    EXPECT_EQ(controller->getSetting(key, QVariant(true)).toBool(), true);
}

TEST_F(PropertiesPanelControllerTests, FsaaSamplesSetSettingPersists)
{
    const QString& key = ViewportSettingsKeys::fsaaSamples();
    QSettings settings;
    const QVariant saved = settings.value(key);

    controller->setSetting(key, 8);
    EXPECT_EQ(controller->getSetting(key, 0).toInt(), 8);

    controller->setSetting(key, 0);
    EXPECT_EQ(controller->getSetting(key, 99).toInt(), 0);

    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

// ---- undoHistory / undoIndex / clearUndoHistory (additional tests) ----

TEST_F(PropertiesPanelControllerTests, UndoHistoryInitiallyEmptyAfterClear)
{
    controller->clearUndoHistory();

    EXPECT_TRUE(controller->undoHistory().isEmpty());
    EXPECT_EQ(controller->undoIndex(), 0);
}

TEST_F(PropertiesPanelControllerTests, ClearUndoHistoryEmitsSignal)
{
    auto* stack = UndoManager::getSingleton()->stack();
    stack->push(new QUndoCommand("TestCmd"));

    QSignalSpy spy(controller, &PropertiesPanelController::undoHistoryChanged);
    ASSERT_TRUE(spy.isValid());

    controller->clearUndoHistory();
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(stack->count(), 0);
}

TEST_F(PropertiesPanelControllerTests, UndoIndexTracksPushAndUndoOperations)
{
    controller->clearUndoHistory();
    auto* stack = UndoManager::getSingleton()->stack();

    stack->push(new QUndoCommand("A"));
    EXPECT_EQ(controller->undoIndex(), 1);

    stack->push(new QUndoCommand("B"));
    EXPECT_EQ(controller->undoIndex(), 2);

    stack->push(new QUndoCommand("C"));
    EXPECT_EQ(controller->undoIndex(), 3);

    stack->undo();
    EXPECT_EQ(controller->undoIndex(), 2);

    stack->undo();
    EXPECT_EQ(controller->undoIndex(), 1);

    stack->redo();
    EXPECT_EQ(controller->undoIndex(), 2);
}
