#include <gtest/gtest.h>

#include "SceneLightsIO.h"

#include "LightManager.h"
#include "LightRigLibrary.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "ShadowController.h"
#include "TestHelpers.h"

#include <QTemporaryDir>
#include <QFile>

#include <assimp/scene.h>

class SceneLightsIOTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        ShadowController::kill();
        LightManager::kill();
        Manager::kill();
    }
};

class SceneLightsIOOgreTest : public SceneLightsIOTest {
protected:
    QTemporaryDir tempDir;

    void SetUp() override
    {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
        ASSERT_TRUE(tempDir.isValid());
    }
};

TEST(SceneLightsIOTest, ChunkedMetadataRoundTripPreservesLargeDocument)
{
    SceneLightsIO::SceneLightsDocument doc;
    doc.ambient = Ogre::ColourValue(0.2f, 0.25f, 0.3f);
    for (int i = 0; i < 24; ++i)
    {
        LightSnapshot light;
        light.name = QStringLiteral("Fill_%1").arg(i);
        light.type = Ogre::Light::LT_POINT;
        light.enabled = true;
        light.diffuse = Ogre::ColourValue(0.8f, 0.7f, 0.6f);
        light.specular = Ogre::ColourValue(0.4f, 0.4f, 0.4f);
        light.powerScale = 1.5f + static_cast<float>(i) * 0.1f;
        light.position = Ogre::Vector3(static_cast<float>(i), 1.f, 2.f);
        light.castShadows = (i % 3) == 0;
        doc.standaloneLights.append(light);
    }

    const QByteArray json = SceneLightsIO::documentToJson(doc);
    ASSERT_GT(json.size(), 1023);

    aiScene scene;
    scene.mRootNode = new aiNode("root");
    SceneLightsIO::appendLightsToAiScene(&scene, doc);

    SceneLightsIO::SceneLightsDocument restored;
    ASSERT_TRUE(SceneLightsIO::readDocumentFromAiScene(&scene, restored));
    EXPECT_EQ(restored.standaloneLights.size(), doc.standaloneLights.size());
    EXPECT_EQ(restored.standaloneLights.first().name, doc.standaloneLights.first().name);
    EXPECT_EQ(restored.standaloneLights.last().powerScale, doc.standaloneLights.last().powerScale);
}

TEST(SceneLightsIOTest, JsonRoundTripPreservesLightSnapshot)
{
    LightSnapshot original;
    original.name = QStringLiteral("Rim");
    original.type = Ogre::Light::LT_SPOTLIGHT;
    original.enabled = true;
    original.diffuse = Ogre::ColourValue(0.9f, 0.8f, 0.7f);
    original.specular = Ogre::ColourValue(0.5f, 0.5f, 0.5f);
    original.powerScale = 2.5f;
    original.position = Ogre::Vector3(1.0f, 2.0f, 3.0f);
    original.orientation = Ogre::Quaternion(Ogre::Degree(15.0f), Ogre::Vector3::UNIT_Y);
    original.scale = Ogre::Vector3(1.0f, 1.0f, 1.0f);
    original.usesDirection = true;
    original.direction = Ogre::Vector3(0.0f, -1.0f, -0.5f);
    original.attenuationRange = 42.0f;
    original.attenuationConstant = 1.0f;
    original.attenuationLinear = 0.05f;
    original.attenuationQuadratic = 0.01f;
    original.spotlightInnerAngleDeg = 20.0f;
    original.spotlightOuterAngleDeg = 35.0f;
    original.spotlightFalloff = 1.25f;
    original.castShadows = true;
    original.shadowDepthBias = 0.0001f;
    original.shadowSlopeBias = 1.5f;

    SceneLightsIO::SceneLightsDocument doc;
    doc.ambient = Ogre::ColourValue(0.11f, 0.12f, 0.13f);
    doc.standaloneLights.append(original);

    SceneLightsIO::SceneLightsDocument restored;
    ASSERT_TRUE(SceneLightsIO::documentFromJson(SceneLightsIO::documentToJson(doc), restored));
    ASSERT_EQ(restored.standaloneLights.size(), 1);
    EXPECT_EQ(restored.standaloneLights.first(), original);
    EXPECT_EQ(restored.ambient, doc.ambient);
}

TEST_F(SceneLightsIOOgreTest, SceneGltfRoundTripPreservesLights)
{
    auto* lights = LightManager::getSingleton();
    Manager::getSingleton()->CreateEmptyScene();

    const QList<LightSnapshot> before = lights->captureAllSnapshots();
    ASSERT_GE(before.size(), 3);

    Manager::getSingleton()->addSceneNode(QStringLiteral("Prop"));
    const QString scenePath = tempDir.filePath(QStringLiteral("lit.scene.gltf"));
    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);

    lights->deleteAllUserLights();
    EXPECT_TRUE(lights->lights().isEmpty());

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(scenePath));
    const QList<LightSnapshot> after = lights->captureAllSnapshots();
    ASSERT_EQ(after.size(), before.size());
    for (const LightSnapshot& snapshot : before)
    {
        bool found = false;
        for (const LightSnapshot& imported : after)
        {
            if (imported.name == snapshot.name)
            {
                EXPECT_EQ(imported, snapshot) << snapshot.name.toStdString();
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << snapshot.name.toStdString();
    }
}

TEST_F(SceneLightsIOOgreTest, UserAddedLightGlbRoundTripUsesSidecar)
{
    auto* lights = LightManager::getSingleton();
    Manager::getSingleton()->CreateEmptyScene();

    const int rigLightCount = lights->lights().size();
    ASSERT_GE(rigLightCount, 3);

    const LightHandle added =
        lights->createLight(Ogre::Light::LT_POINT, QStringLiteral("UserPointLight"));
    ASSERT_TRUE(added.isValid());

    Manager::getSingleton()->addSceneNode(QStringLiteral("Prop"));
    const QString scenePath = tempDir.filePath(QStringLiteral("user_light.scene.glb"));
    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);
    ASSERT_TRUE(QFile::exists(tempDir.filePath(QStringLiteral("user_light.scene.lights.json"))));

    lights->deleteAllUserLights();
    EXPECT_TRUE(lights->lights().isEmpty());

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(scenePath));
    EXPECT_EQ(lights->lights().size(), rigLightCount + 1);
    EXPECT_NE(lights->findLight(QStringLiteral("UserPointLight")), nullptr);
}

TEST_F(SceneLightsIOOgreTest, EmptyLightsBlockRestoresDefaultRig)
{
    auto* lights = LightManager::getSingleton();
    lights->deleteAllUserLights();
    Manager::getSingleton()->addSceneNode(QStringLiteral("Solo"));

    const QString scenePath = tempDir.filePath(QStringLiteral("mesh_only.scene.gltf"));
    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);

    lights->deleteAllUserLights();
    ASSERT_TRUE(MeshImporterExporter::sceneImporter(scenePath));
    EXPECT_GE(lights->lights().size(), 3);
}

TEST_F(SceneLightsIOOgreTest, RigGroupRoundTripPreservesGrouping)
{
    auto* lights = LightManager::getSingleton();
    Manager::getSingleton()->CreateEmptyScene();

    const LightRigApplyResult applied =
        LightRigLibrary::apply(QStringLiteral("three_point_studio"), true);
    ASSERT_TRUE(applied.ok) << applied.error.toStdString();
    ASSERT_FALSE(applied.addedLights.isEmpty());

    const SceneLightsIO::SceneLightsDocument captured = SceneLightsIO::captureFromScene();
    ASSERT_FALSE(captured.rigGroups.isEmpty());

    const QString scenePath = tempDir.filePath(QStringLiteral("rig.scene.gltf"));
    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);

    lights->deleteAllUserLights();
    ASSERT_TRUE(MeshImporterExporter::sceneImporter(scenePath));

    const SceneLightsIO::SceneLightsDocument restored = SceneLightsIO::captureFromScene();
    ASSERT_EQ(restored.rigGroups.size(), captured.rigGroups.size());
    EXPECT_FALSE(restored.rigGroups.first().lights.isEmpty());
    EXPECT_FALSE(restored.rigGroups.first().rigId.isEmpty());
}

TEST_F(SceneLightsIOOgreTest, FbxSidecarRoundTripPreservesLights)
{
    auto* lights = LightManager::getSingleton();
    Manager::getSingleton()->CreateEmptyScene();

    const QString robot = testRobotMeshPath();
    if (robot.isEmpty() || !QFile::exists(robot))
        GTEST_SKIP() << "robot.mesh fixture unavailable";

    MeshImporterExporter::importer({robot});
    const QList<LightSnapshot> before = lights->captureAllSnapshots();
    ASSERT_GE(before.size(), 3);

    Ogre::Entity* entity = nullptr;
    for (auto* obj : Manager::getSingleton()->getEntities())
    {
        if (obj && obj->getMovableType() == QStringLiteral("Entity"))
        {
            entity = static_cast<Ogre::Entity*>(obj);
            break;
        }
    }
    ASSERT_NE(entity, nullptr);

    const QString meshPath = tempDir.filePath(QStringLiteral("lit.fbx"));
    ASSERT_EQ(MeshImporterExporter::exporter(entity->getParentSceneNode(), meshPath,
                                             QStringLiteral("FBX Binary (*.fbx)")),
              0);
    ASSERT_TRUE(QFile::exists(tempDir.filePath(QStringLiteral("lit.lights.json"))));

    lights->deleteAllUserLights();
    ASSERT_TRUE(lights->lights().isEmpty());

    MeshImporterExporter::importer({meshPath});
    const QList<LightSnapshot> after = lights->captureAllSnapshots();
    ASSERT_EQ(after.size(), before.size());
    for (const LightSnapshot& snapshot : before)
    {
        bool found = false;
        for (const LightSnapshot& imported : after)
        {
            if (imported.name == snapshot.name)
            {
                EXPECT_EQ(imported, snapshot) << snapshot.name.toStdString();
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << snapshot.name.toStdString();
    }
}
