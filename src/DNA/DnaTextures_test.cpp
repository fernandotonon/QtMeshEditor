// Companion-texture matching (names only): no GL, no assets.

#include <gtest/gtest.h>

#include "DnaTextures.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace Dna;

namespace {
void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
}
}  // namespace

TEST(DnaTextures, MeshTokenDropsTheLodAndMeshSuffix)
{
    EXPECT_EQ(meshToken("head_lod0_mesh"), "head");
    EXPECT_EQ(meshToken("eyeLeft_lod3_mesh"), "eyeLeft");
    EXPECT_EQ(meshToken("body"), "body");
}

TEST(DnaTextures, FilesAreMatchedToTheMeshTheyNameAndToTheirMapType)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const char* f : {"head_basecolor.png", "head_normal.png", "head_roughness.png", "body_color.png", "body_normal.png",
                          "body_metallic.png", "body_ao.png", "notes.txt", "colorful_rock.png"})
        touch(dir.filePath(f));
    const TextureScan s = findCompanionTextures(dir.filePath("character.dna"), {"head_lod0_mesh", "body_lod0_mesh"});
    ASSERT_EQ(s.perMesh.size(), 2u);
    EXPECT_TRUE(s.perMesh[0].albedo.endsWith("head_basecolor.png")) << s.perMesh[0].albedo.toStdString();
    EXPECT_TRUE(s.perMesh[0].normal.endsWith("head_normal.png"));
    EXPECT_TRUE(s.perMesh[0].roughness.endsWith("head_roughness.png"));
    EXPECT_TRUE(s.perMesh[1].albedo.endsWith("body_color.png"));
    EXPECT_TRUE(s.perMesh[1].normal.endsWith("body_normal.png"));
    EXPECT_TRUE(s.perMesh[1].roughness.isEmpty());
    // "colorful_rock" is not a color map (whole words only), and the unbindable ORM inputs are reported, not bound
    EXPECT_FALSE(s.perMesh[0].albedo.contains("colorful"));
    ASSERT_FALSE(s.notes.isEmpty());
    EXPECT_NE(s.notes[0].indexOf("body_metallic.png"), -1);
}

TEST(DnaTextures, ASingleMeshAcceptsGenericNamesButTwoMeshesDoNot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.filePath("albedo.png"));
    const TextureScan one = findCompanionTextures(dir.filePath("body.dna"), {"body_lod0_mesh"});
    ASSERT_EQ(one.perMesh.size(), 1u);
    EXPECT_TRUE(one.perMesh[0].albedo.endsWith("albedo.png"));
    const TextureScan two = findCompanionTextures(dir.filePath("body.dna"), {"head_lod0_mesh", "teeth_lod0_mesh"});
    EXPECT_TRUE(two.perMesh[0].albedo.isEmpty());
    EXPECT_TRUE(two.perMesh[1].albedo.isEmpty());
}

TEST(DnaTextures, ATexturesSubfolderIsSearchedToo)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    touch(dir.filePath("textures/body_diffuse.jpg"));
    const TextureScan s = findCompanionTextures(dir.filePath("body.dna"), {"body_lod0_mesh"});
    EXPECT_TRUE(s.perMesh[0].albedo.endsWith("body_diffuse.jpg"));
}
