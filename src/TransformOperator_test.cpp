#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include <Ogre.h>

#define private public
#define protected public
#include "TransformOperator.h"
#undef protected
#undef private

#include "commands/TransformCommands.h"
#include "GlobalDefinitions.h"
#include "Manager.h"
#include "RotationGizmo.h"
#include "SelectionBoxObject.h"
#include "SelectionSet.h"
#include "ScaleGizmo.h"
#include "TestHelpers.h"
#include "TranslationGizmo.h"
#include "UndoManager.h"

namespace {
void expectQuaternionNear(const Ogre::Quaternion& actual, const Ogre::Quaternion& expected)
{
    EXPECT_FLOAT_EQ(actual.w, expected.w);
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
}

void expectVectorNear(const Ogre::Vector3& actual, const Ogre::Vector3& expected, float tolerance = 0.001f)
{
    EXPECT_NEAR(actual.x, expected.x, tolerance);
    EXPECT_NEAR(actual.y, expected.y, tolerance);
    EXPECT_NEAR(actual.z, expected.z, tolerance);
}

bool isRotationGizmoVisible(const RotationGizmo* gizmo)
{
    return gizmo->getXCircle().isVisible()
        && gizmo->getYCircle().isVisible()
        && gizmo->getZCircle().isVisible();
}

bool isTranslationGizmoVisible(const TranslationGizmo* gizmo)
{
    return gizmo->getXAxis().isVisible()
        && gizmo->getYAxis().isVisible()
        && gizmo->getZAxis().isVisible();
}

bool isScaleGizmoVisible(const ScaleGizmo* gizmo)
{
    return gizmo->getXAxis().isVisible()
        && gizmo->getYAxis().isVisible()
        && gizmo->getZAxis().isVisible();
}
}

Q_DECLARE_METATYPE(Ogre::Vector3)

class TransformOperatorTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TransformOperator::kill();
        UndoManager::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

        createStandardOgreMaterials();
        op = TransformOperator::getSingleton();
        ASSERT_NE(op, nullptr);
    }

    void TearDown() override
    {
        TransformOperator::kill();
        UndoManager::kill();
        Manager::kill();
        if (app)
            app->processEvents();
    }

    Ogre::SceneNode* createSelectedNode(const QString& name)
    {
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(name);
        EXPECT_NE(node, nullptr);
        if (node)
            SelectionSet::getSingleton()->selectOne(node);
        return node;
    }

    Ogre::Entity* createSelectedEntity(const QString& nodeName,
                                       const QString& entityName,
                                       const std::string& meshName)
    {
        if (!canLoadMeshFiles())
            return nullptr;

        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        EXPECT_NE(mesh, nullptr);

        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);
        if (!mesh || !node)
            return nullptr;

        Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(entityName.toStdString(), mesh);
        EXPECT_NE(entity, nullptr);
        if (!entity)
            return nullptr;

        node->attachObject(entity);
        SelectionSet::getSingleton()->selectOne(entity);
        return entity;
    }

    QApplication* app = nullptr;
    TransformOperator* op = nullptr;
};

TEST(TransformOperatorTest, Swap)
{
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}

// Bone-gizmo routing decision: with no bone selected, all states route
// to entity transform (return false). With a bone selected, rotate/scale
// always route to the bone; translate only routes when the bone supports
// translation (root or unrigged) — otherwise it falls through so the user
// can still translate the entity that owns the rig.
//
// `selectedBone` is treated as an opaque non-null marker — the helper
// doesn't dereference it. Using a non-null int and reinterpret_cast lets
// the test stay header-only without a real Ogre::Bone instance.
TEST(TransformOperatorTest, ShouldRouteToBoneGizmoNoBoneNeverRoutes)
{
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_TRANSLATE, nullptr, /*canTranslate=*/true));
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_ROTATE, nullptr, true));
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_SCALE, nullptr, true));
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_SELECT, nullptr, true));
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_NONE, nullptr, true));
}

TEST(TransformOperatorTest, ShouldRouteToBoneGizmoRotateAlwaysRoutes)
{
    int sentinel = 0;
    auto* bone = reinterpret_cast<const Ogre::Bone*>(&sentinel);
    // Rotate goes through the bone gizmo regardless of translate-ability —
    // posing is the primary workflow and rotate is always valid on a bone.
    EXPECT_TRUE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_ROTATE, bone, /*canTranslate=*/false));
    EXPECT_TRUE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_ROTATE, bone, /*canTranslate=*/true));
}

TEST(TransformOperatorTest, ShouldRouteToBoneGizmoScaleAlwaysRoutes)
{
    int sentinel = 0;
    auto* bone = reinterpret_cast<const Ogre::Bone*>(&sentinel);
    // Scale also bypasses the translate-ability check — it's uncommon
    // but valid (stretchy rigs).
    EXPECT_TRUE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_SCALE, bone, /*canTranslate=*/false));
    EXPECT_TRUE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_SCALE, bone, /*canTranslate=*/true));
}

TEST(TransformOperatorTest, ShouldRouteToBoneGizmoTranslateRespectsBoneCanTranslate)
{
    int sentinel = 0;
    auto* bone = reinterpret_cast<const Ogre::Bone*>(&sentinel);
    // Translatable bone (root / unrigged): take the bone path.
    EXPECT_TRUE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_TRANSLATE, bone, /*canTranslate=*/true));
    // Non-translatable bone (rigged non-root): fall through to entity
    // translate. This is the bug-fix case: previously the bone branch
    // was taken and silently returned, so the user could not move the
    // imported skinned model at all once a bone got auto-selected.
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_TRANSLATE, bone, /*canTranslate=*/false));
}

TEST(TransformOperatorTest, ShouldRouteToBoneGizmoNonTransformStatesDoNotRoute)
{
    int sentinel = 0;
    auto* bone = reinterpret_cast<const Ogre::Bone*>(&sentinel);
    // SELECT and NONE never go through the bone gizmo branch.
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_SELECT, bone, true));
    EXPECT_FALSE(TransformOperator::shouldRouteToBoneGizmo(
        TransformOperator::TS_NONE, bone, true));
}

TEST_F(TransformOperatorTests, TransformSpaceChangesOnlyWhenValueDiffers)
{
    QSignalSpy spy(op, &TransformOperator::transformSpaceChanged);
    ASSERT_TRUE(spy.isValid());

    EXPECT_EQ(op->getTransformSpace(), TransformOperator::SPACE_WORLD);

    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->toggleTransformSpace();

    EXPECT_EQ(op->getTransformSpace(), TransformOperator::SPACE_WORLD);
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(TransformOperatorTests, TransformStateChangeWithoutSelectionUpdatesStateAndTracking)
{
    op->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    EXPECT_FALSE(op->mTrackingEnable);
    EXPECT_EQ(op->mTransformState, TransformOperator::TS_TRANSLATE);
}

TEST_F(TransformOperatorTests, SelectionBoxColourRoundTrips)
{
    const Ogre::ColourValue colour(0.1f, 0.2f, 0.3f, 0.4f);
    op->setSelectionBoxColour(colour);

    const Ogre::ColourValue current = op->getSelectionBoxColour();
    EXPECT_FLOAT_EQ(current.r, colour.r);
    EXPECT_FLOAT_EQ(current.g, colour.g);
    EXPECT_FLOAT_EQ(current.b, colour.b);
    EXPECT_FLOAT_EQ(current.a, colour.a);
}

TEST_F(TransformOperatorTests, PivotModeCycleWrapsAndEmitsSignal)
{
    op->setPivotMode(TransformOperator::PIVOT_CENTER);

    QSignalSpy spy(op, &TransformOperator::pivotModeChanged);
    ASSERT_TRUE(spy.isValid());
    spy.clear();

    op->cyclePivotMode();
    EXPECT_EQ(op->pivotMode(), TransformOperator::PIVOT_BOTTOM);
    op->cyclePivotMode();
    EXPECT_EQ(op->pivotMode(), TransformOperator::PIVOT_ORIGIN);
    op->cyclePivotMode();
    EXPECT_EQ(op->pivotMode(), TransformOperator::PIVOT_CENTER);

    EXPECT_EQ(spy.count(), 3);
}

TEST_F(TransformOperatorTests, PivotPointWithEmptySelectionIsZero)
{
    SelectionSet::getSingleton()->clear();
    EXPECT_EQ(op->getPivotPoint(), Ogre::Vector3::ZERO);
}

TEST_F(TransformOperatorTests, PivotPointForNodeSelectionSupportsCenterBottomAndOrigin)
{
    Ogre::SceneNode* nodeA = createSelectedNode("PivotNodeA");
    Ogre::SceneNode* nodeB = Manager::getSingleton()->addSceneNode("PivotNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);

    nodeA->setPosition(Ogre::Vector3(0.0f, 4.0f, 10.0f));
    nodeB->setPosition(Ogre::Vector3(6.0f, 2.0f, 2.0f));

    op->setPivotMode(TransformOperator::PIVOT_CENTER);
    expectVectorNear(op->getPivotPoint(), Ogre::Vector3(3.0f, 3.0f, 6.0f));

    op->setPivotMode(TransformOperator::PIVOT_BOTTOM);
    expectVectorNear(op->getPivotPoint(), Ogre::Vector3(3.0f, 2.0f, 6.0f));

    op->setPivotMode(TransformOperator::PIVOT_ORIGIN);
    expectVectorNear(op->getPivotPoint(), Ogre::Vector3(3.0f, 3.0f, 6.0f));
}

TEST_F(TransformOperatorTests, PivotPointForEntitySelectionSupportsCenterBottomAndOrigin)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entityA = createSelectedEntity("PivotEntityNodeA", "PivotEntityA", "PivotEntityMeshA");
    Ogre::Entity* entityB = createSelectedEntity("PivotEntityNodeB", "PivotEntityB", "PivotEntityMeshB");
    ASSERT_NE(entityA, nullptr);
    ASSERT_NE(entityB, nullptr);

    Ogre::SceneNode* nodeA = entityA->getParentSceneNode();
    Ogre::SceneNode* nodeB = entityB->getParentSceneNode();
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    nodeA->setPosition(Ogre::Vector3(0.0f, 4.0f, 10.0f));
    nodeB->setPosition(Ogre::Vector3(6.0f, 2.0f, 2.0f));

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(entityA);
    SelectionSet::getSingleton()->append(entityB);

    op->setPivotMode(TransformOperator::PIVOT_CENTER);
    const Ogre::Vector3 center = op->getPivotPoint();
    expectVectorNear(center, Ogre::Vector3(3.0f, 3.0f, 6.0f), 0.2f);

    op->setPivotMode(TransformOperator::PIVOT_BOTTOM);
    const Ogre::Vector3 bottom = op->getPivotPoint();
    EXPECT_NEAR(bottom.x, 3.0f, 0.2f);
    EXPECT_NEAR(bottom.z, 6.0f, 0.2f);
    EXPECT_NEAR(bottom.y, 1.0f, 0.2f); // min Y of bbox: nodeB.y + meshMinY(-1)

    op->setPivotMode(TransformOperator::PIVOT_ORIGIN);
    expectVectorNear(op->getPivotPoint(), Ogre::Vector3(3.0f, 3.0f, 6.0f), 0.2f);
}

TEST_F(TransformOperatorTests, PivotPointForSubEntitySelectionSupportsCenterBottomAndOrigin)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createSelectedEntity("PivotSubNode", "PivotSubEntity", "PivotSubMesh");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    node->setPosition(Ogre::Vector3(2.0f, 5.0f, -1.0f));

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->selectOne(entity->getSubEntity(0));
    ASSERT_TRUE(SelectionSet::getSingleton()->hasSubEntities());

    op->setPivotMode(TransformOperator::PIVOT_CENTER);
    const Ogre::Vector3 center = op->getPivotPoint();
    EXPECT_NEAR(center.x, 2.0f, 0.2f);
    EXPECT_NEAR(center.y, 5.0f, 0.2f);
    EXPECT_NEAR(center.z, -1.0f, 0.2f);

    op->setPivotMode(TransformOperator::PIVOT_BOTTOM);
    const Ogre::Vector3 bottom = op->getPivotPoint();
    EXPECT_NEAR(bottom.x, center.x, 0.2f);
    EXPECT_NEAR(bottom.z, center.z, 0.2f);
    EXPECT_NEAR(bottom.y, 4.0f, 0.2f); // node.y + meshMinY(-1)

    op->setPivotMode(TransformOperator::PIVOT_ORIGIN);
    expectVectorNear(op->getPivotPoint(), node->getPosition(), 0.2f);
}

TEST_F(TransformOperatorTests, UpdateGizmoWithoutSelectionHidesEveryGizmo)
{
    op->m_pRotationGizmo->setVisible(true);
    op->m_pTranslationGizmo->setVisible(true);
    op->m_pScaleGizmo->setVisible(true);

    op->updateGizmo();

    EXPECT_FALSE(isRotationGizmoVisible(op->m_pRotationGizmo));
    EXPECT_FALSE(isTranslationGizmoVisible(op->m_pTranslationGizmo));
    EXPECT_FALSE(isScaleGizmoVisible(op->m_pScaleGizmo));
}

TEST_F(TransformOperatorTests, RayFromScreenPointWithoutActiveWidgetReturnsDefaultRay)
{
    const Ogre::Ray ray = op->rayFromScreenPoint(QPoint(25, 40));
    const Ogre::Ray defaultRay;
    EXPECT_EQ(ray.getOrigin(), defaultRay.getOrigin());
    EXPECT_EQ(ray.getDirection(), defaultRay.getDirection());
}

TEST_F(TransformOperatorTests, TranslateStateShowsTranslationGizmoForNodeSelection)
{
    ASSERT_NE(createSelectedNode("TranslateGizmoNode"), nullptr);

    op->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    EXPECT_TRUE(isTranslationGizmoVisible(op->m_pTranslationGizmo));
    EXPECT_FALSE(isRotationGizmoVisible(op->m_pRotationGizmo));
    EXPECT_FALSE(isScaleGizmoVisible(op->m_pScaleGizmo));
    EXPECT_TRUE(op->mTrackingEnable);
}

TEST_F(TransformOperatorTests, RotateStateShowsRotationGizmoForNodeSelection)
{
    ASSERT_NE(createSelectedNode("RotateGizmoNode"), nullptr);

    op->onTransformStateChange(TransformOperator::TS_ROTATE);

    EXPECT_TRUE(isRotationGizmoVisible(op->m_pRotationGizmo));
    EXPECT_FALSE(isTranslationGizmoVisible(op->m_pTranslationGizmo));
    EXPECT_FALSE(isScaleGizmoVisible(op->m_pScaleGizmo));
    EXPECT_TRUE(op->mTrackingEnable);
}

TEST_F(TransformOperatorTests, ScaleStateShowsScaleGizmoForNodeSelection)
{
    ASSERT_NE(createSelectedNode("ScaleGizmoNode"), nullptr);

    op->onTransformStateChange(TransformOperator::TS_SCALE);

    EXPECT_TRUE(isScaleGizmoVisible(op->m_pScaleGizmo));
    EXPECT_FALSE(isRotationGizmoVisible(op->m_pRotationGizmo));
    EXPECT_FALSE(isTranslationGizmoVisible(op->m_pTranslationGizmo));
    EXPECT_TRUE(op->mTrackingEnable);
}

TEST_F(TransformOperatorTests, SelectStateHidesGizmosEvenWithSelection)
{
    ASSERT_NE(createSelectedNode("SelectStateNode"), nullptr);

    op->onTransformStateChange(TransformOperator::TS_SELECT);

    EXPECT_FALSE(isRotationGizmoVisible(op->m_pRotationGizmo));
    EXPECT_FALSE(isTranslationGizmoVisible(op->m_pTranslationGizmo));
    EXPECT_FALSE(isScaleGizmoVisible(op->m_pScaleGizmo));
}

TEST_F(TransformOperatorTests, UpdateGizmoPositionForNodeSelectionEmitsCurrentValues)
{
    Ogre::SceneNode* node = createSelectedNode("SignalNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(Ogre::Vector3(2.0f, 3.0f, 4.0f));
    node->setScale(Ogre::Vector3(1.5f, 2.5f, 3.5f));

    QSignalSpy positionSpy(op, &TransformOperator::selectedPositionChanged);
    QSignalSpy scaleSpy(op, &TransformOperator::selectedScaleChanged);
    ASSERT_TRUE(positionSpy.isValid());
    ASSERT_TRUE(scaleSpy.isValid());

    op->updateGizmoPosition();

    ASSERT_FALSE(positionSpy.isEmpty());
    ASSERT_FALSE(scaleSpy.isEmpty());
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(positionSpy.takeLast().at(0)), Ogre::Vector3(2.0f, 3.0f, 4.0f));
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(scaleSpy.takeLast().at(0)), Ogre::Vector3(1.5f, 2.5f, 3.5f));
}

TEST_F(TransformOperatorTests, UpdateGizmoUsesSingleNodeOrientationInLocalSpace)
{
    Ogre::SceneNode* node = createSelectedNode("LocalOrientationNode");
    ASSERT_NE(node, nullptr);
    const Ogre::Quaternion expected(Ogre::Degree(35), Ogre::Vector3::UNIT_Y);
    node->setOrientation(expected);

    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    expectQuaternionNear(op->m_pTransformNode->getOrientation(), expected);
}

TEST_F(TransformOperatorTests, UpdateGizmoUsesRootOrientationForMultipleLocalNodes)
{
    Ogre::SceneNode* nodeA = createSelectedNode("LocalMultiNodeA");
    Ogre::SceneNode* nodeB = Manager::getSingleton()->addSceneNode("LocalMultiNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);
    nodeA->setOrientation(Ogre::Quaternion(Ogre::Degree(15), Ogre::Vector3::UNIT_X));
    nodeB->setOrientation(Ogre::Quaternion(Ogre::Degree(25), Ogre::Vector3::UNIT_Z));

    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->onTransformStateChange(TransformOperator::TS_ROTATE);

    expectQuaternionNear(op->m_pTransformNode->getOrientation(),
                         Manager::getSingleton()->getSceneMgr()->getRootSceneNode()->getOrientation());
}

TEST_F(TransformOperatorTests, UpdateGizmoPositionForEntitySelectionUsesTrackedScaleAndOrientation)
{
    Ogre::Entity* entity = createSelectedEntity("TrackedEntityNode", "TrackedEntity", "TrackedEntityMesh");
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->setEntityScaleFactor(entity, Ogre::Vector3(1.4f, 1.5f, 1.6f));
    SelectionSet::getSingleton()->setEntityRotation(entity, Ogre::Vector3(10.0f, 20.0f, 30.0f));

    QSignalSpy orientationSpy(op, &TransformOperator::selectedOrientationChanged);
    QSignalSpy scaleSpy(op, &TransformOperator::selectedScaleChanged);
    ASSERT_TRUE(orientationSpy.isValid());
    ASSERT_TRUE(scaleSpy.isValid());

    op->updateGizmoPosition();

    ASSERT_FALSE(orientationSpy.isEmpty());
    ASSERT_FALSE(scaleSpy.isEmpty());
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(orientationSpy.takeLast().at(0)), Ogre::Vector3(10.0f, 20.0f, 30.0f));
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(scaleSpy.takeLast().at(0)), Ogre::Vector3(1.4f, 1.5f, 1.6f));
}

TEST_F(TransformOperatorTests, SelectedNodeTransformsUpdateNodeState)
{
    Ogre::SceneNode* node = createSelectedNode("TransformNode");
    ASSERT_NE(node, nullptr);

    op->setSelectedPosition(Ogre::Vector3(5.0f, 6.0f, 7.0f));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(5.0f, 6.0f, 7.0f));

    op->setSelectedScale(Ogre::Vector3(2.0f, 3.0f, 4.0f));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 3.0f, 4.0f));

    op->setSelectedOrientation(Ogre::Vector3(15.0f, 25.0f, 35.0f));
    EXPECT_NE(node->getOrientation(), Ogre::Quaternion::IDENTITY);

    op->translateSelected(Ogre::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(6.0f, 8.0f, 10.0f));
}

TEST_F(TransformOperatorTests, SetSelectedScaleForNodeUsesSelectionAverage)
{
    Ogre::SceneNode* nodeA = createSelectedNode("ScaleAverageNodeA");
    Ogre::SceneNode* nodeB = Manager::getSingleton()->addSceneNode("ScaleAverageNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(nodeA);
    SelectionSet::getSingleton()->append(nodeB);
    nodeA->setScale(Ogre::Vector3(1.0f, 1.0f, 1.0f));
    nodeB->setScale(Ogre::Vector3(3.0f, 3.0f, 3.0f));

    op->setSelectedScale(Ogre::Vector3(4.0f, 4.0f, 4.0f));

    EXPECT_EQ(nodeA->getScale(), Ogre::Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_EQ(nodeB->getScale(), Ogre::Vector3(6.0f, 6.0f, 6.0f));
}

TEST_F(TransformOperatorTests, SetSelectedOrientationForNodeAppliesRequestedEulerAngles)
{
    Ogre::SceneNode* node = createSelectedNode("OrientationSetterNode");
    ASSERT_NE(node, nullptr);

    op->setSelectedOrientation(Ogre::Vector3(15.0f, 30.0f, 45.0f));

    const Ogre::Vector3 current = SelectionSet::getSingleton()->getSelectionOrientation();
    expectVectorNear(current, Ogre::Vector3(15.0f, 30.0f, 45.0f), 1.0f);
}

TEST_F(TransformOperatorTests, EmptySelectionTransformMutatorsAreNoOps)
{
    EXPECT_NO_THROW(op->setSelectedPosition(Ogre::Vector3(1.0f, 2.0f, 3.0f)));
    EXPECT_NO_THROW(op->translateSelected(Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_NO_THROW(op->setSelectedScale(Ogre::Vector3(2.0f, 2.0f, 2.0f)));
    EXPECT_NO_THROW(op->scaleSelected(Ogre::Vector3(1.1f, 1.1f, 1.1f)));
    EXPECT_NO_THROW(op->setSelectedOrientation(Ogre::Vector3(10.0f, 20.0f, 30.0f)));
    EXPECT_NO_THROW(op->rotateSelected(Ogre::Quaternion::IDENTITY));
    EXPECT_NO_THROW(op->rotateSelected(Ogre::Vector3(0.0f, 0.0f, 0.0f)));
}

TEST_F(TransformOperatorTests, OnSelectionChangedRestoresNodeInitialState)
{
    Ogre::SceneNode* node = createSelectedNode("InitialStateNode");
    ASSERT_NE(node, nullptr);

    // onSelectionChanged() restores a selected SceneNode to its explicitly saved initial state.
    node->setScale(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    const Ogre::Quaternion expectedOrientation(Ogre::Degree(30), Ogre::Vector3::UNIT_Y);
    node->setOrientation(expectedOrientation);
    node->setInitialState();

    node->setScale(Ogre::Vector3(5.0f, 6.0f, 7.0f));
    node->setOrientation(Ogre::Quaternion(Ogre::Degree(80), Ogre::Vector3::UNIT_X));

    op->onSelectionChanged();

    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 2.0f, 2.0f));
    expectQuaternionNear(node->getOrientation(), expectedOrientation);
}

TEST_F(TransformOperatorTests, OnSelectionChangedNormalizesSelectedEntityParentNode)
{
    Ogre::Entity* entity = createSelectedEntity("EntityStateNode", "EntityStateEntity", "EntityStateMesh");
    ASSERT_NE(entity, nullptr);

    Ogre::SceneNode* parentNode = entity->getParentSceneNode();
    // onSelectionChanged() treats an Entity selection differently: it normalizes the parent SceneNode
    // back to identity instead of restoring a previously saved SceneNode initial state.
    parentNode->setScale(Ogre::Vector3(3.0f, 4.0f, 5.0f));
    parentNode->setOrientation(Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Z));

    op->onSelectionChanged();

    EXPECT_EQ(parentNode->getScale(), Ogre::Vector3::UNIT_SCALE);
    expectQuaternionNear(parentNode->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTests, OnSelectionChangedNormalizesSelectedSubEntityParentNode)
{
    Ogre::Entity* entity = createSelectedEntity("SubEntityStateNode", "SubEntityStateEntity", "SubEntityStateMesh");
    ASSERT_NE(entity, nullptr);

    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    ASSERT_NE(subEntity, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(subEntity);

    Ogre::SceneNode* parentNode = entity->getParentSceneNode();
    parentNode->setScale(Ogre::Vector3(6.0f, 7.0f, 8.0f));
    parentNode->setOrientation(Ogre::Quaternion(Ogre::Degree(15), Ogre::Vector3::UNIT_X));

    op->onSelectionChanged();

    EXPECT_EQ(parentNode->getScale(), Ogre::Vector3::UNIT_SCALE);
    expectQuaternionNear(parentNode->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTests, EntityRotationVectorSetterTracksAbsoluteRotation)
{
    Ogre::Entity* entity = createSelectedEntity("EntityRotateNode", "EntityRotateEntity", "EntityRotateMesh");
    ASSERT_NE(entity, nullptr);

    op->rotateSelected(Ogre::Vector3(10.0f, 20.0f, 30.0f));
    EXPECT_EQ(SelectionSet::getSingleton()->getEntityRotation(entity), Ogre::Vector3(10.0f, 20.0f, 30.0f));

    op->rotateSelected(Ogre::Vector3(15.0f, 25.0f, 35.0f));
    EXPECT_EQ(SelectionSet::getSingleton()->getEntityRotation(entity), Ogre::Vector3(15.0f, 25.0f, 35.0f));
}

TEST_F(TransformOperatorTests, EntityScaleSetterTracksScaleFactor)
{
    Ogre::Entity* entity = createSelectedEntity("EntityScaleNode", "EntityScaleEntity", "EntityScaleMesh");
    ASSERT_NE(entity, nullptr);

    op->setSelectedScale(Ogre::Vector3(1.2f, 1.3f, 1.4f));

    EXPECT_EQ(SelectionSet::getSingleton()->getEntityScaleFactor(entity), Ogre::Vector3(1.2f, 1.3f, 1.4f));
}

TEST_F(TransformOperatorTests, EntityScaleSetterReplacesPreviousTrackedScale)
{
    Ogre::Entity* entity = createSelectedEntity("EntityScaleNode2", "EntityScaleEntity2", "EntityScaleMesh2");
    ASSERT_NE(entity, nullptr);

    op->setSelectedScale(Ogre::Vector3(1.2f, 1.3f, 1.4f));
    op->setSelectedScale(Ogre::Vector3(2.0f, 2.5f, 3.0f));

    EXPECT_EQ(SelectionSet::getSingleton()->getEntityScaleFactor(entity), Ogre::Vector3(2.0f, 2.5f, 3.0f));
}

TEST_F(TransformOperatorTests, EntityQuaternionRotationAccumulatesTrackedEulerDelta)
{
    Ogre::Entity* entity = createSelectedEntity("EntityQuatNode", "EntityQuatEntity", "EntityQuatMesh");
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->setEntityRotation(entity, Ogre::Vector3(0.0f, 10.0f, 0.0f));
    op->rotateSelected(Ogre::Quaternion(Ogre::Degree(20), Ogre::Vector3::UNIT_Y));

    const Ogre::Vector3 tracked = SelectionSet::getSingleton()->getEntityRotation(entity);
    EXPECT_NEAR(tracked.y, 30.0f, 0.5f);
}

TEST_F(TransformOperatorTests, RemoveSelectedWithEmptySelectionKeepsUndoHistory)
{
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("HistoryNode");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes;
    nodes.append(node);
    UndoManager::getSingleton()->push(new TranslateCommand(nodes, Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(UndoManager::getSingleton()->canUndo());

    SelectionSet::getSingleton()->clear();
    op->removeSelected();

    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());
}

TEST_F(TransformOperatorTests, RemoveSelectedDestroysNodesAndClearsUndoHistory)
{
    Ogre::SceneNode* node = createSelectedNode("DeleteNode");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes;
    nodes.append(node);
    UndoManager::getSingleton()->push(new TranslateCommand(nodes, Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(UndoManager::getSingleton()->canUndo());

    op->removeSelected();

    EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("DeleteNode"));
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
}

// ---- Snap Utility Tests ----

TEST(TransformOperatorSnap, SnapValueRoundsToNearestStep)
{
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(0.0, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(0.4, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(0.6, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(-0.6, 1.0), -1.0);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(2.3, 0.5), 2.5);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(2.2, 0.5), 2.0);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(0.13, 0.25), 0.25);
    EXPECT_DOUBLE_EQ(TransformOperator::snapValue(0.12, 0.25), 0.0);
}

TEST(TransformOperatorSnap, SnapTranslationSnapsEachAxis)
{
    Ogre::Vector3 input(1.7f, 0.3f, -2.6f);
    Ogre::Vector3 snapped = TransformOperator::snapTranslation(input, 1.0);
    EXPECT_NEAR(snapped.x, 2.0f, 0.001f);
    EXPECT_NEAR(snapped.y, 0.0f, 0.001f);
    EXPECT_NEAR(snapped.z, -3.0f, 0.001f);
}

TEST(TransformOperatorSnap, SnapTranslationWithSmallGrid)
{
    Ogre::Vector3 input(0.13f, 0.07f, 0.24f);
    Ogre::Vector3 snapped = TransformOperator::snapTranslation(input, 0.1);
    EXPECT_NEAR(snapped.x, 0.1f, 0.001f);
    EXPECT_NEAR(snapped.y, 0.1f, 0.001f);
    EXPECT_NEAR(snapped.z, 0.2f, 0.001f);
}

TEST(TransformOperatorSnap, SnapAngleRoundsToNearestAngle)
{
    EXPECT_NEAR(TransformOperator::snapAngle(10.0f, 15.0), 15.0f, 0.001f);
    EXPECT_NEAR(TransformOperator::snapAngle(6.0f, 15.0), 0.0f, 0.001f);
    EXPECT_NEAR(TransformOperator::snapAngle(44.0f, 45.0), 45.0f, 0.001f);
    EXPECT_NEAR(TransformOperator::snapAngle(22.0f, 45.0), 0.0f, 0.001f);
    EXPECT_NEAR(TransformOperator::snapAngle(-80.0f, 90.0), -90.0f, 0.001f);
    EXPECT_NEAR(TransformOperator::snapAngle(3.0f, 5.0), 5.0f, 0.001f);
}

TEST(TransformOperatorSnap, SnapScaleSnapsEachAxis)
{
    Ogre::Vector3 input(0.3f, 0.6f, -0.13f);
    Ogre::Vector3 snapped = TransformOperator::snapScale(input, 0.25);
    EXPECT_NEAR(snapped.x, 0.25f, 0.001f);
    EXPECT_NEAR(snapped.y, 0.5f, 0.001f);
    EXPECT_NEAR(snapped.z, -0.25f, 0.001f);
}

TEST(TransformOperatorSnap, PresetsReturnExpectedValues)
{
    auto grid = TransformOperator::gridSizePresets();
    EXPECT_EQ(grid.size(), 6);
    EXPECT_DOUBLE_EQ(grid[0], 0.1);
    EXPECT_DOUBLE_EQ(grid[5], 5.0);

    auto angle = TransformOperator::angleStepPresets();
    EXPECT_EQ(angle.size(), 4);
    EXPECT_DOUBLE_EQ(angle[0], 5.0);
    EXPECT_DOUBLE_EQ(angle[3], 90.0);

    auto scale = TransformOperator::scaleStepPresets();
    EXPECT_EQ(scale.size(), 3);
    EXPECT_DOUBLE_EQ(scale[0], 0.1);
    EXPECT_DOUBLE_EQ(scale[2], 0.5);
}

TEST_F(TransformOperatorTests, SnapSettingsRoundTripAndEmitSignal)
{
    // Reset to known state first (QSettings may have persisted different values)
    op->setSnapEnabled(false);
    op->setSnapGridSize(1.0);
    op->setSnapAngleStep(15.0);
    op->setSnapScaleStep(0.25);

    QSignalSpy spy(op, &TransformOperator::snapSettingsChanged);
    ASSERT_TRUE(spy.isValid());

    op->setSnapEnabled(true);
    EXPECT_TRUE(op->isSnapEnabled());
    EXPECT_EQ(spy.count(), 1);

    op->setSnapGridSize(2.0);
    EXPECT_DOUBLE_EQ(op->snapGridSize(), 2.0);
    EXPECT_EQ(spy.count(), 2);

    op->setSnapAngleStep(45.0);
    EXPECT_DOUBLE_EQ(op->snapAngleStep(), 45.0);
    EXPECT_EQ(spy.count(), 3);

    op->setSnapScaleStep(0.5);
    EXPECT_DOUBLE_EQ(op->snapScaleStep(), 0.5);
    EXPECT_EQ(spy.count(), 4);

    // Setting same value should not emit again
    op->setSnapEnabled(true);
    EXPECT_EQ(spy.count(), 4);

    op->setSnapGridSize(2.0);
    EXPECT_EQ(spy.count(), 4);
}

TEST_F(TransformOperatorTests, SnapSettingsRejectInvalidValues)
{
    op->setSnapGridSize(1.0);
    op->setSnapGridSize(-1.0);
    EXPECT_DOUBLE_EQ(op->snapGridSize(), 1.0);

    op->setSnapAngleStep(15.0);
    op->setSnapAngleStep(0.0);
    EXPECT_DOUBLE_EQ(op->snapAngleStep(), 15.0);

    op->setSnapScaleStep(0.25);
    op->setSnapScaleStep(-0.5);
    EXPECT_DOUBLE_EQ(op->snapScaleStep(), 0.25);
}
