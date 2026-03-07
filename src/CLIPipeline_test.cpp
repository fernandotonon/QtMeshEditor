#include <gtest/gtest.h>
#include "CLIPipeline.h"
#include "TestHelpers.h"

// --- Formatting tests (no Ogre needed) ---

class CLIPipelineFormatTest : public ::testing::Test {};

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_BasicFields)
{
    MeshInfo info;
    info.file = "test.mesh";
    info.vertices = 100;
    info.triangles = 50;
    info.submeshes = 2;
    info.materials << "mat1" << "mat2";
    info.bbMin = Ogre::Vector3(-1, -2, -3);
    info.bbMax = Ogre::Vector3(1, 2, 3);

    QString text = CLIPipeline::formatMeshInfoText(info);

    EXPECT_TRUE(text.contains("File: test.mesh"));
    EXPECT_TRUE(text.contains("Vertices: 100"));
    EXPECT_TRUE(text.contains("Triangles: 50"));
    EXPECT_TRUE(text.contains("Submeshes: 2"));
    EXPECT_TRUE(text.contains("mat1, mat2"));
    EXPECT_TRUE(text.contains("Bounding Box:"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_WithSkeleton)
{
    MeshInfo info;
    info.file = "animated.fbx";
    info.vertices = 200;
    info.triangles = 100;
    info.submeshes = 1;
    info.skeletonName = "test.skeleton";
    info.boneCount = 10;
    info.animations.append({"walk", 1.2f});
    info.animations.append({"run", 0.8f});

    QString text = CLIPipeline::formatMeshInfoText(info);

    EXPECT_TRUE(text.contains("Skeleton: test.skeleton (10 bones)"));
    EXPECT_TRUE(text.contains("Animations:"));
    EXPECT_TRUE(text.contains("walk"));
    EXPECT_TRUE(text.contains("run"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_NoMaterials)
{
    MeshInfo info;
    info.file = "empty.mesh";
    QString text = CLIPipeline::formatMeshInfoText(info);
    EXPECT_TRUE(text.contains("(none)"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_Structure)
{
    MeshInfo info;
    info.file = "test.mesh";
    info.vertices = 300;
    info.triangles = 150;
    info.submeshes = 3;
    info.materials << "matA";
    info.bbMin = Ogre::Vector3(0, 0, 0);
    info.bbMax = Ogre::Vector3(1, 1, 1);

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    ASSERT_TRUE(doc.isObject());

    QJsonObject obj = doc.object();
    EXPECT_EQ(obj["file"].toString(), "test.mesh");
    EXPECT_EQ(obj["vertices"].toInt(), 300);
    EXPECT_EQ(obj["triangles"].toInt(), 150);
    EXPECT_EQ(obj["submeshes"].toInt(), 3);
    EXPECT_TRUE(obj["materials"].isArray());
    EXPECT_EQ(obj["materials"].toArray().size(), 1);
    EXPECT_TRUE(obj["boundingBox"].isObject());
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_WithAnimations)
{
    MeshInfo info;
    info.file = "anim.fbx";
    info.skeletonName = "skel.skeleton";
    info.boneCount = 5;
    info.animations.append({"idle", 3.5f});

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_TRUE(obj.contains("skeleton"));
    EXPECT_EQ(obj["skeleton"].toObject()["bones"].toInt(), 5);
    EXPECT_TRUE(obj.contains("animations"));
    EXPECT_EQ(obj["animations"].toArray().size(), 1);
    EXPECT_EQ(obj["animations"].toArray()[0].toObject()["name"].toString(), "idle");
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_NoSkeleton)
{
    MeshInfo info;
    info.file = "noskel.obj";

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_FALSE(obj.contains("skeleton"));
    EXPECT_FALSE(obj.contains("animations"));
}

// --- MeshInfo extraction tests (need Ogre) ---

class CLIPipelineOgreTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
    }
};

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_TriangleMesh)
{
    auto mesh = createInMemoryTriangleMesh("cli_test_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("cli_test_tri");
    auto* entity = sceneMgr->createEntity("cli_test_tri", mesh);
    node->attachObject(entity);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, "triangle.mesh");

    EXPECT_EQ(info.file, "triangle.mesh");
    EXPECT_EQ(info.vertices, 3u);
    EXPECT_EQ(info.triangles, 1u);
    EXPECT_EQ(info.submeshes, 1u);
    EXPECT_TRUE(info.skeletonName.isEmpty());
    EXPECT_EQ(info.boneCount, 0);
    EXPECT_TRUE(info.animations.isEmpty());
}

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_AnimatedEntity)
{
    auto* entity = createAnimatedTestEntity("cli_test_anim");
    ASSERT_NE(entity, nullptr);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, "animated.fbx");

    EXPECT_EQ(info.file, "animated.fbx");
    EXPECT_GT(info.vertices, 0u);
    EXPECT_TRUE(entity->hasSkeleton());
    EXPECT_FALSE(info.skeletonName.isEmpty());
    EXPECT_EQ(info.boneCount, 2);
    EXPECT_EQ(info.animations.size(), 1);
    EXPECT_EQ(info.animations[0].name, "TestAnim");
    EXPECT_FLOAT_EQ(info.animations[0].duration, 1.0f);
}

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_NullEntity)
{
    MeshInfo info = CLIPipeline::extractMeshInfo(nullptr, "null.mesh");
    EXPECT_EQ(info.vertices, 0u);
    EXPECT_EQ(info.triangles, 0u);
}

// --- FixOptions tests ---

TEST(FixOptionsTest, AnySet_DefaultIsFalse)
{
    FixOptions opts;
    EXPECT_FALSE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_RemoveDegenerates)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_MergeMaterials)
{
    FixOptions opts;
    opts.mergeMaterials = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_AllFlags)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    opts.mergeMaterials = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, ToAssimpFlags_Default)
{
    FixOptions opts;
    EXPECT_EQ(opts.toAssimpFlags(), 0u);
}

TEST(FixOptionsTest, ToAssimpFlags_RemoveDegenerates)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    EXPECT_EQ(opts.toAssimpFlags(), static_cast<unsigned int>(aiProcess_FindDegenerates));
}

TEST(FixOptionsTest, ToAssimpFlags_MergeMaterials)
{
    FixOptions opts;
    opts.mergeMaterials = true;
    EXPECT_EQ(opts.toAssimpFlags(), static_cast<unsigned int>(aiProcess_RemoveRedundantMaterials));
}

TEST(FixOptionsTest, ToAssimpFlags_All)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    opts.mergeMaterials = true;
    unsigned int expected = aiProcess_FindDegenerates | aiProcess_RemoveRedundantMaterials;
    EXPECT_EQ(opts.toAssimpFlags(), expected);
}
