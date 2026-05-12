#include <gtest/gtest.h>

#include "MemoryEstimator.h"

#include <QJsonArray>
#include <QJsonObject>

// All tests here exercise the pure-data primitives — no Ogre required, so
// they run on every CI build (including Linux Xvfb where Ogre is gated).

TEST(MemoryEstimatorTest, MeshBytesEmpty)
{
    EXPECT_EQ(0u, MemoryEstimator::meshBytes(0, 32, 0, 2));
}

TEST(MemoryEstimatorTest, MeshBytesVertices16BitIndices)
{
    // 1024 verts × 32 bytes/vert + 3072 indices × 2 bytes = 32768 + 6144 = 38912
    EXPECT_EQ(38912u, MemoryEstimator::meshBytes(1024, 32, 3072, 2));
}

TEST(MemoryEstimatorTest, MeshBytes32BitIndices)
{
    // 1024 verts × 32 + 3072 × 4 = 32768 + 12288 = 45056
    EXPECT_EQ(45056u, MemoryEstimator::meshBytes(1024, 32, 3072, 4));
}

TEST(MemoryEstimatorTest, TextureBytesNoMips)
{
    // 1024 × 1024 × 4 = 4 MB base
    EXPECT_EQ(4ull * 1024 * 1024,
              MemoryEstimator::textureBytes(1024, 1024, 4, false));
}

TEST(MemoryEstimatorTest, TextureBytesWithMips)
{
    // Base 4 MB × 4/3 ≈ 5.33 MB
    quint64 base = 4ull * 1024 * 1024;
    EXPECT_EQ((base * 4) / 3,
              MemoryEstimator::textureBytes(1024, 1024, 4, true));
}

TEST(MemoryEstimatorTest, TextureBytesNonSquare)
{
    // 2048 × 512 × 1 byte (alpha-only) = 1 MB
    EXPECT_EQ(1ull * 1024 * 1024,
              MemoryEstimator::textureBytes(2048, 512, 1, false));
}

TEST(MemoryEstimatorTest, FormatBytesSmall)
{
    EXPECT_EQ(QString("512 B"), MemoryEstimator::formatBytes(512));
}

TEST(MemoryEstimatorTest, FormatBytesKB)
{
    EXPECT_EQ(QString("2.0 KB"), MemoryEstimator::formatBytes(2048));
}

TEST(MemoryEstimatorTest, FormatBytesMB)
{
    EXPECT_EQ(QString("4.00 MB"),
              MemoryEstimator::formatBytes(4ull * 1024 * 1024));
}

TEST(MemoryEstimatorTest, FormatBytesGB)
{
    EXPECT_EQ(QString("1.50 GB"),
              MemoryEstimator::formatBytes(3ull * 512 * 1024 * 1024));
}

TEST(MemoryEstimatorTest, ParseBudgetBytes)
{
    EXPECT_EQ(2048u, MemoryEstimator::parseBudget("2048"));
    EXPECT_EQ(2048u, MemoryEstimator::parseBudget("2048B"));
    EXPECT_EQ(2048u, MemoryEstimator::parseBudget("2048 B"));
}

TEST(MemoryEstimatorTest, ParseBudgetKB)
{
    EXPECT_EQ(1024u * 5, MemoryEstimator::parseBudget("5KB"));
    EXPECT_EQ(1024u * 5, MemoryEstimator::parseBudget("5 kb"));
}

TEST(MemoryEstimatorTest, ParseBudgetMB)
{
    EXPECT_EQ(50ull * 1024 * 1024, MemoryEstimator::parseBudget("50MB"));
    EXPECT_EQ(50ull * 1024 * 1024, MemoryEstimator::parseBudget("50 mb"));
}

TEST(MemoryEstimatorTest, ParseBudgetFractionalGB)
{
    EXPECT_EQ(static_cast<quint64>(1.5 * 1024 * 1024 * 1024),
              MemoryEstimator::parseBudget("1.5 GB"));
}

TEST(MemoryEstimatorTest, ParseBudgetGarbage)
{
    EXPECT_EQ(0u, MemoryEstimator::parseBudget(""));
    EXPECT_EQ(0u, MemoryEstimator::parseBudget("not a budget"));
    EXPECT_EQ(0u, MemoryEstimator::parseBudget("--50MB"));
}

TEST(MemoryEstimatorTest, JsonRoundTripEmpty)
{
    SceneMemoryReport empty;
    QJsonObject obj = MemoryEstimator::toJson(empty);
    EXPECT_TRUE(obj.contains("meshes"));
    EXPECT_TRUE(obj.contains("textures"));
    EXPECT_TRUE(obj.contains("totals"));
    EXPECT_EQ(0, obj["meshes"].toArray().size());
    EXPECT_EQ(0, obj["textures"].toArray().size());
    EXPECT_FALSE(obj.contains("budget"));  // omitted when budgetBytes == 0
}

TEST(MemoryEstimatorTest, JsonWithMeshAndTexture)
{
    SceneMemoryReport report;
    MeshMemoryEstimate m;
    m.name = "Cube.mesh";
    m.vertexCount = 24;
    m.vertexBytes = 24 * 32;
    m.indexCount = 36;
    m.indexBytes = 36 * 2;
    report.meshes.append(m);
    report.meshTotalBytes = m.totalBytes();

    TextureMemoryEstimate t;
    t.name = "diffuse.png";
    t.width = 512;
    t.height = 512;
    t.bytesPerPixel = 4;
    t.bytes = 512 * 512 * 4;
    report.textures.append(t);
    report.textureTotalBytes = t.bytes;

    QJsonObject obj = MemoryEstimator::toJson(report);
    EXPECT_EQ(1, obj["meshes"].toArray().size());
    EXPECT_EQ(1, obj["textures"].toArray().size());
    EXPECT_EQ(QString("Cube.mesh"), obj["meshes"].toArray()[0].toObject()["name"].toString());
    EXPECT_EQ(static_cast<qint64>(m.totalBytes()),
              obj["totals"].toObject()["meshBytes"].toVariant().toLongLong());
}

TEST(MemoryEstimatorTest, JsonIncludesBudgetWhenSet)
{
    SceneMemoryReport report;
    report.budgetBytes = 50 * 1024 * 1024;
    report.meshTotalBytes = 60 * 1024 * 1024;
    QJsonObject obj = MemoryEstimator::toJson(report);
    ASSERT_TRUE(obj.contains("budget"));
    QJsonObject budget = obj["budget"].toObject();
    EXPECT_EQ(static_cast<qint64>(report.budgetBytes),
              budget["bytes"].toVariant().toLongLong());
    EXPECT_TRUE(budget["overBudget"].toBool());
}

TEST(MemoryEstimatorTest, TextHasHeader)
{
    SceneMemoryReport empty;
    QString out = MemoryEstimator::toText(empty);
    EXPECT_TRUE(out.contains("Memory Report"));
    EXPECT_TRUE(out.contains("Meshes (0)"));
    EXPECT_TRUE(out.contains("Textures (0)"));
}

TEST(MemoryEstimatorTest, TextFlagsOverBudget)
{
    SceneMemoryReport report;
    report.budgetBytes = 10 * 1024 * 1024;
    report.meshTotalBytes = 15 * 1024 * 1024;
    QString out = MemoryEstimator::toText(report);
    EXPECT_TRUE(out.contains("OVER BUDGET"));
}

TEST(MemoryEstimatorTest, TextOmitsBudgetWhenZero)
{
    SceneMemoryReport report;
    report.meshTotalBytes = 1024;
    QString out = MemoryEstimator::toText(report);
    EXPECT_FALSE(out.contains("Budget"));
    EXPECT_FALSE(out.contains("OVER BUDGET"));
}
