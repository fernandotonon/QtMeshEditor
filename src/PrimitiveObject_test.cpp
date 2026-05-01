#include <gtest/gtest.h>
#include "PrimitiveObject.h"
#include "Manager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TestHelpers.h"

// ---------- Existing standalone tests (no Manager needed) ----------

TEST(PrimitivesTest, CreateDefaultPrimitive)
{
    PrimitiveObject primitive("");
    ASSERT_EQ(primitive.getName(), "");
    ASSERT_EQ(primitive.getType(), PrimitiveObject::PrimitiveType::AP_NONE);
    ASSERT_EQ(primitive.getSceneNode(), nullptr);
    ASSERT_EQ(primitive.getNumIterations(), 1);
    ASSERT_EQ(primitive.getNumSegBase(), 1);
    ASSERT_EQ(primitive.getNumSegCircle(), 1);
    ASSERT_EQ(primitive.getNumSegX(), 1);
    ASSERT_EQ(primitive.getNumSegY(), 1);
    ASSERT_EQ(primitive.getNumSegZ(), 1);
    ASSERT_EQ(primitive.getInnerRadius(), 0.5f);
    ASSERT_EQ(primitive.getSectionRadius(), 0.5f);
    ASSERT_EQ(primitive.getChamferRadius(), 1.0f);
    ASSERT_EQ(primitive.getOuterRadius(), 1.0f);
    ASSERT_EQ(primitive.getRadius(), 1.0f);
    ASSERT_EQ(primitive.getHeight(), 1.0f);
    ASSERT_EQ(primitive.getSizeZ(), 1.0f);
    ASSERT_EQ(primitive.getSizeY(), 1.0f);
    ASSERT_EQ(primitive.getSizeX(), 1.0f);
}

TEST(PrimitivesTest, isPrimitiveWithNullptr)
{
    ASSERT_FALSE(PrimitiveObject::isPrimitive(nullptr));
}

// ---------- New standalone tests ----------

TEST(PrimitivesTest, SettersGetters)
{
    PrimitiveObject primitive("SetterTest");

    // setSizeX / getSizeX
    primitive.setSizeX(5.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeX(), 5.0f);

    // setSizeY / getSizeY
    primitive.setSizeY(6.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeY(), 6.0f);

    // setSizeZ / getSizeZ
    primitive.setSizeZ(7.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeZ(), 7.0f);

    // setRadius / getRadius
    primitive.setRadius(3.0f);
    EXPECT_FLOAT_EQ(primitive.getRadius(), 3.0f);

    // setHeight / getHeight
    primitive.setHeight(4.0f);
    EXPECT_FLOAT_EQ(primitive.getHeight(), 4.0f);

    // setNumSegX / getNumSegX
    primitive.setNumSegX(8);
    EXPECT_EQ(primitive.getNumSegX(), 8);

    // setNumSegY / getNumSegY
    primitive.setNumSegY(9);
    EXPECT_EQ(primitive.getNumSegY(), 9);

    // setNumSegZ / getNumSegZ
    primitive.setNumSegZ(10);
    EXPECT_EQ(primitive.getNumSegZ(), 10);

    // setNumSegBase / getNumSegBase (aliases mNumSegX)
    primitive.setNumSegBase(12);
    EXPECT_EQ(primitive.getNumSegBase(), 12);
    EXPECT_EQ(primitive.getNumSegX(), 12);

    // setNumSegCircle / getNumSegCircle (aliases mNumSegX)
    primitive.setNumSegCircle(14);
    EXPECT_EQ(primitive.getNumSegCircle(), 14);
    EXPECT_EQ(primitive.getNumSegX(), 14);

    // setNumIterations / getNumIterations (aliases mNumSegX)
    primitive.setNumIterations(3);
    EXPECT_EQ(primitive.getNumIterations(), 3);
    EXPECT_EQ(primitive.getNumSegX(), 3);

    // setOuterRadius is an alias for setRadius
    primitive.setOuterRadius(2.5f);
    EXPECT_FLOAT_EQ(primitive.getOuterRadius(), 2.5f);
    EXPECT_FLOAT_EQ(primitive.getRadius(), 2.5f);

    // setInnerRadius / getInnerRadius (requires < mRadius)
    primitive.setRadius(5.0f);
    primitive.setInnerRadius(2.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 2.0f);

    // setSectionRadius is an alias for setInnerRadius
    primitive.setSectionRadius(1.5f);
    EXPECT_FLOAT_EQ(primitive.getSectionRadius(), 1.5f);

    // setUTile / getUTile
    primitive.setUTile(2.0f);
    EXPECT_FLOAT_EQ(primitive.getUTile(), 2.0f);

    // setVTile / getVTile
    primitive.setVTile(3.0f);
    EXPECT_FLOAT_EQ(primitive.getVTile(), 3.0f);

    // setUVSwitch / hasUVSwitched
    primitive.setUVSwitch(true);
    EXPECT_TRUE(primitive.hasUVSwitched());
    primitive.setUVSwitch(false);
    EXPECT_FALSE(primitive.hasUVSwitched());
}

TEST(PrimitivesTest, PrimitiveTypeConstructor)
{
    // AP_CUBE
    {
        PrimitiveObject cube("cube", PrimitiveObject::AP_CUBE);
        EXPECT_EQ(cube.getType(), PrimitiveObject::AP_CUBE);
        EXPECT_FLOAT_EQ(cube.getSizeX(), 2.0f);
        EXPECT_FLOAT_EQ(cube.getSizeY(), 2.0f);
        EXPECT_FLOAT_EQ(cube.getSizeZ(), 2.0f);
        EXPECT_EQ(cube.getNumSegX(), 1);
        EXPECT_EQ(cube.getNumSegY(), 1);
        EXPECT_EQ(cube.getNumSegZ(), 1);
    }
    // AP_SPHERE
    {
        PrimitiveObject sphere("sphere", PrimitiveObject::AP_SPHERE);
        EXPECT_EQ(sphere.getType(), PrimitiveObject::AP_SPHERE);
        EXPECT_FLOAT_EQ(sphere.getRadius(), 1.0f);
        EXPECT_EQ(sphere.getNumSegX(), 16);
        EXPECT_EQ(sphere.getNumSegY(), 16);
    }
    // AP_PLANE
    {
        PrimitiveObject plane("plane", PrimitiveObject::AP_PLANE);
        EXPECT_EQ(plane.getType(), PrimitiveObject::AP_PLANE);
        EXPECT_FLOAT_EQ(plane.getSizeX(), 2.0f);
        EXPECT_FLOAT_EQ(plane.getSizeY(), 2.0f);
        EXPECT_EQ(plane.getNumSegX(), 3);
        EXPECT_EQ(plane.getNumSegY(), 3);
    }
    // AP_CYLINDER
    {
        PrimitiveObject cyl("cyl", PrimitiveObject::AP_CYLINDER);
        EXPECT_EQ(cyl.getType(), PrimitiveObject::AP_CYLINDER);
        EXPECT_FLOAT_EQ(cyl.getRadius(), 1.0f);
        EXPECT_FLOAT_EQ(cyl.getHeight(), 3.0f);
        EXPECT_EQ(cyl.getNumSegX(), 16);
        EXPECT_EQ(cyl.getNumSegZ(), 1);
    }
    // AP_CONE
    {
        PrimitiveObject cone("cone", PrimitiveObject::AP_CONE);
        EXPECT_EQ(cone.getType(), PrimitiveObject::AP_CONE);
        EXPECT_FLOAT_EQ(cone.getRadius(), 2.0f);
        EXPECT_FLOAT_EQ(cone.getHeight(), 3.0f);
        EXPECT_EQ(cone.getNumSegX(), 16);
        EXPECT_EQ(cone.getNumSegZ(), 1);
    }
    // AP_TORUS
    {
        PrimitiveObject torus("torus", PrimitiveObject::AP_TORUS);
        EXPECT_EQ(torus.getType(), PrimitiveObject::AP_TORUS);
        EXPECT_FLOAT_EQ(torus.getRadius(), 3.0f);
        EXPECT_FLOAT_EQ(torus.getSectionRadius(), 1.0f);
        EXPECT_EQ(torus.getNumSegX(), 16);
        EXPECT_EQ(torus.getNumSegY(), 16);
    }
    // AP_TUBE
    {
        PrimitiveObject tube("tube", PrimitiveObject::AP_TUBE);
        EXPECT_EQ(tube.getType(), PrimitiveObject::AP_TUBE);
        EXPECT_FLOAT_EQ(tube.getRadius(), 3.0f);
        EXPECT_FLOAT_EQ(tube.getInnerRadius(), 2.0f);
        EXPECT_FLOAT_EQ(tube.getHeight(), 3.0f);
        EXPECT_EQ(tube.getNumSegX(), 16);
        EXPECT_EQ(tube.getNumSegZ(), 1);
    }
    // AP_CAPSULE
    {
        PrimitiveObject capsule("capsule", PrimitiveObject::AP_CAPSULE);
        EXPECT_EQ(capsule.getType(), PrimitiveObject::AP_CAPSULE);
        EXPECT_FLOAT_EQ(capsule.getRadius(), 1.0f);
        EXPECT_FLOAT_EQ(capsule.getHeight(), 2.0f);
        EXPECT_EQ(capsule.getNumSegX(), 8);
        EXPECT_EQ(capsule.getNumSegY(), 16);
        EXPECT_EQ(capsule.getNumSegZ(), 1);
    }
    // AP_ICOSPHERE
    {
        PrimitiveObject ico("ico", PrimitiveObject::AP_ICOSPHERE);
        EXPECT_EQ(ico.getType(), PrimitiveObject::AP_ICOSPHERE);
        EXPECT_FLOAT_EQ(ico.getRadius(), 2.0f);
        EXPECT_EQ(ico.getNumSegX(), 2);
    }
    // AP_ROUNDEDBOX
    {
        PrimitiveObject rbox("rbox", PrimitiveObject::AP_ROUNDEDBOX);
        EXPECT_EQ(rbox.getType(), PrimitiveObject::AP_ROUNDEDBOX);
        EXPECT_FLOAT_EQ(rbox.getSizeX(), 2.0f);
        EXPECT_FLOAT_EQ(rbox.getSizeY(), 2.0f);
        EXPECT_FLOAT_EQ(rbox.getSizeZ(), 2.0f);
        EXPECT_FLOAT_EQ(rbox.getChamferRadius(), 1.0f);
        EXPECT_EQ(rbox.getNumSegX(), 1);
        EXPECT_EQ(rbox.getNumSegY(), 1);
        EXPECT_EQ(rbox.getNumSegZ(), 1);
    }
    // AP_SPRING
    {
        PrimitiveObject spring("spring", PrimitiveObject::AP_SPRING);
        EXPECT_EQ(spring.getType(), PrimitiveObject::AP_SPRING);
        EXPECT_EQ(spring.getNumSegX(), 10);
        EXPECT_EQ(spring.getNumSegY(), 10);
    }
    // AP_NONE (default)
    {
        PrimitiveObject none("none", PrimitiveObject::AP_NONE);
        EXPECT_EQ(none.getType(), PrimitiveObject::AP_NONE);
        EXPECT_FLOAT_EQ(none.getSizeX(), 1.0f);
        EXPECT_FLOAT_EQ(none.getSizeY(), 1.0f);
        EXPECT_FLOAT_EQ(none.getSizeZ(), 1.0f);
        EXPECT_FLOAT_EQ(none.getRadius(), 1.0f);
        EXPECT_FLOAT_EQ(none.getHeight(), 1.0f);
        EXPECT_EQ(none.getNumSegX(), 1);
        EXPECT_EQ(none.getNumSegY(), 1);
        EXPECT_EQ(none.getNumSegZ(), 1);
    }
}

// ---------- Fixture tests (need Manager / Ogre) ----------

class PrimitiveObjectOgreTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    }
    void TearDown() override {
        if (app) app->processEvents();
    }
    QApplication* app = nullptr;
};

TEST_F(PrimitiveObjectOgreTest, CreateCubeWithSceneNode)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TestCube");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(PrimitiveObject::isPrimitive(node));

    // Verify the scene node is tracked by Manager
    EXPECT_TRUE(Manager::getSingleton()->hasSceneNode("TestCube"));

    Manager::getSingleton()->destroySceneNode("TestCube");
}

TEST_F(PrimitiveObjectOgreTest, CreateSphereWithSceneNode)
{
    Ogre::SceneNode* node = PrimitiveObject::createSphere("TestSphere");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(PrimitiveObject::isPrimitive(node));

    // Verify the scene node is tracked by Manager
    EXPECT_TRUE(Manager::getSingleton()->hasSceneNode("TestSphere"));

    Manager::getSingleton()->destroySceneNode("TestSphere");
}

TEST_F(PrimitiveObjectOgreTest, CreateAllPrimitiveTypes)
{
    // Create each primitive type via the static factory methods
    struct PrimitiveTestCase {
        const char* name;
        Ogre::SceneNode* (*creator)(const QString&);
    };

    PrimitiveTestCase cases[] = {
        {"AllCube",       PrimitiveObject::createCube},
        {"AllSphere",     PrimitiveObject::createSphere},
        {"AllPlane",      PrimitiveObject::createPlane},
        {"AllCylinder",   PrimitiveObject::createCylinder},
        {"AllCone",       PrimitiveObject::createCone},
        {"AllTorus",      PrimitiveObject::createTorus},
        {"AllTube",       PrimitiveObject::createTube},
        {"AllCapsule",    PrimitiveObject::createCapsule},
        {"AllIcoSphere",  PrimitiveObject::createIcoSphere},
        {"AllRoundedBox", PrimitiveObject::createRoundedBox},
        {"AllSpring",     PrimitiveObject::createSpring},
    };

    for (const auto& tc : cases) {
        Ogre::SceneNode* node = tc.creator(tc.name);
        ASSERT_NE(node, nullptr) << "Failed to create primitive: " << tc.name;
        EXPECT_TRUE(PrimitiveObject::isPrimitive(node)) << "isPrimitive failed for: " << tc.name;
        EXPECT_TRUE(Manager::getSingleton()->hasSceneNode(tc.name)) << "Manager missing node: " << tc.name;
    }

    // Clean up all created primitives
    for (const auto& tc : cases) {
        Manager::getSingleton()->destroySceneNode(tc.name);
    }
}

TEST_F(PrimitiveObjectOgreTest, GetPrimitiveFromSceneNode)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RetrieveCube");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(PrimitiveObject::isPrimitive(node));

    PrimitiveObject* retrieved = PrimitiveObject::getPrimitiveFromSceneNode(node);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->getType(), PrimitiveObject::AP_CUBE);
    EXPECT_EQ(retrieved->getSceneNode(), node);
    EXPECT_FLOAT_EQ(retrieved->getSizeX(), 2.0f);
    EXPECT_FLOAT_EQ(retrieved->getSizeY(), 2.0f);
    EXPECT_FLOAT_EQ(retrieved->getSizeZ(), 2.0f);

    Manager::getSingleton()->destroySceneNode("RetrieveCube");
}

// ==========================================================================
// NEW: isPrimitive with non-PrimitiveObject UserAny
// ==========================================================================

TEST_F(PrimitiveObjectOgreTest, IsPrimitiveNonPrimitiveUserAny)
{
    // Create a plain scene node and set a UserAny that is NOT a PrimitiveObject*
    auto node = Manager::getSingleton()->addSceneNode("NonPrimAny");
    ASSERT_NE(node, nullptr);

    node->getUserObjectBindings().setUserAny(Ogre::Any(std::string("NotAPrimitive")));

    // isPrimitive should return false because any_cast<PrimitiveObject*> will throw
    EXPECT_FALSE(PrimitiveObject::isPrimitive(node));

    Manager::getSingleton()->destroySceneNode(node);
}

// ==========================================================================
// NEW: isPrimitive with node that has no UserAny at all
// ==========================================================================

TEST_F(PrimitiveObjectOgreTest, IsPrimitiveNoUserAny)
{
    auto node = Manager::getSingleton()->addSceneNode("NoUserAnyNode");
    ASSERT_NE(node, nullptr);

    // Node has no UserAny set, so isPrimitive should return false
    EXPECT_FALSE(PrimitiveObject::isPrimitive(node));

    Manager::getSingleton()->destroySceneNode(node);
}

// ==========================================================================
// NEW: setInnerRadius validation — must be < radius
// ==========================================================================

TEST(PrimitivesTest, SetInnerRadiusValidation)
{
    PrimitiveObject primitive("InnerRadTest");

    // Set radius first
    primitive.setRadius(5.0f);
    EXPECT_FLOAT_EQ(primitive.getRadius(), 5.0f);

    // setInnerRadius with value >= radius should not change
    primitive.setInnerRadius(5.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 0.5f); // default

    primitive.setInnerRadius(6.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 0.5f); // unchanged

    // setInnerRadius with value == 0 should not change (must be > 0)
    primitive.setInnerRadius(0.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 0.5f); // unchanged

    // setInnerRadius with negative value should not change
    primitive.setInnerRadius(-1.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 0.5f); // unchanged

    // setInnerRadius with valid value should change
    primitive.setInnerRadius(3.0f);
    EXPECT_FLOAT_EQ(primitive.getInnerRadius(), 3.0f);
}

// ==========================================================================
// NEW: setChamferRadius is alias for setRadius, getChamferRadius returns mRadius
// ==========================================================================

TEST(PrimitivesTest, ChamferRadiusGetterSetter)
{
    PrimitiveObject primitive("ChamferTest", PrimitiveObject::AP_ROUNDEDBOX);
    EXPECT_FLOAT_EQ(primitive.getChamferRadius(), 1.0f); // mRadius for ROUNDEDBOX

    // setOuterRadius is alias for setRadius which sets mRadius
    primitive.setOuterRadius(3.0f);
    EXPECT_FLOAT_EQ(primitive.getChamferRadius(), 3.0f);
    EXPECT_FLOAT_EQ(primitive.getRadius(), 3.0f);
}

// ==========================================================================
// NEW: Setter validation — negative values rejected
// ==========================================================================

TEST(PrimitivesTest, SetterValidationRejectsInvalid)
{
    PrimitiveObject primitive("ValidationTest");

    // setSizeX with 0 should not change
    primitive.setSizeX(5.0f);
    primitive.setSizeX(0.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeX(), 5.0f);

    primitive.setSizeX(-1.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeX(), 5.0f);

    // setSizeY with 0 should not change
    primitive.setSizeY(5.0f);
    primitive.setSizeY(0.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeY(), 5.0f);

    // setSizeZ with 0 should not change
    primitive.setSizeZ(5.0f);
    primitive.setSizeZ(0.0f);
    EXPECT_FLOAT_EQ(primitive.getSizeZ(), 5.0f);

    // setRadius with 0 should not change
    primitive.setRadius(5.0f);
    primitive.setRadius(0.0f);
    EXPECT_FLOAT_EQ(primitive.getRadius(), 5.0f);

    // setHeight with 0 should not change
    primitive.setHeight(5.0f);
    primitive.setHeight(0.0f);
    EXPECT_FLOAT_EQ(primitive.getHeight(), 5.0f);

    // setNumSegX with 0 should not change
    primitive.setNumSegX(8);
    primitive.setNumSegX(0);
    EXPECT_EQ(primitive.getNumSegX(), 8);

    primitive.setNumSegX(-1);
    EXPECT_EQ(primitive.getNumSegX(), 8);

    // setNumSegY with 0 should not change
    primitive.setNumSegY(8);
    primitive.setNumSegY(0);
    EXPECT_EQ(primitive.getNumSegY(), 8);

    // setNumSegZ with 0 should not change
    primitive.setNumSegZ(8);
    primitive.setNumSegZ(0);
    EXPECT_EQ(primitive.getNumSegZ(), 8);
}

// ==========================================================================
// NEW: setNumSegSection and setNumSegHeight aliases
// ==========================================================================

TEST(PrimitivesTest, SetNumSegSectionAndHeight)
{
    PrimitiveObject primitive("SegAliasTest");

    primitive.setNumSegSection(12);
    EXPECT_EQ(primitive.getNumSegY(), 12);

    primitive.setNumSegHeight(8);
    EXPECT_EQ(primitive.getNumSegZ(), 8);

    // Invalid values should not change
    primitive.setNumSegSection(0);
    EXPECT_EQ(primitive.getNumSegY(), 12);

    primitive.setNumSegHeight(-1);
    EXPECT_EQ(primitive.getNumSegZ(), 8);
}

// ==========================================================================
// NEW: PrimitiveObject scene node link after creation
// ==========================================================================

TEST_F(PrimitiveObjectOgreTest, PrimitiveSceneNodeLink)
{
    Ogre::SceneNode* node = PrimitiveObject::createSphere("LinkedSphere");
    ASSERT_NE(node, nullptr);

    PrimitiveObject* primitive = PrimitiveObject::getPrimitiveFromSceneNode(node);
    ASSERT_NE(primitive, nullptr);
    EXPECT_EQ(primitive->getSceneNode(), node);
    EXPECT_EQ(primitive->getName(), "LinkedSphere");
    EXPECT_EQ(primitive->getType(), PrimitiveObject::AP_SPHERE);

    Manager::getSingleton()->destroySceneNode("LinkedSphere");
}
