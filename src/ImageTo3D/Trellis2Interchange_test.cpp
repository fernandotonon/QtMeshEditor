// Unit tests for the QTM3D interchange container (TRELLIS.2 backend).
// Pure data — no Ogre/GL/ONNX/Python (MeshGenBaker_test.cpp convention).
#include "Trellis2Interchange.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

Trellis2Interchange::Data makeSample()
{
    Trellis2Interchange::Data d;
    d.positions = {0.0f, 0.0f, 0.0f,
                   0.5f, 0.0f, 0.0f,
                   0.0f, 0.5f, 0.0f,
                   0.0f, 0.0f, 0.5f};
    d.indices = {0, 1, 2, 0, 2, 3};
    d.vertexCount = 4;
    d.triangleCount = 2;
    d.voxelCoords = {1, 2, 3, 4, 5, 6};
    d.voxelAttrs = {255, 0, 0, 10, 200, 255,
                    0, 255, 0, 250, 20, 128};
    d.voxelCount = 2;
    d.resolution = 64;
    d.voxelSize = 1.0f / 64.0f;
    d.origin[0] = d.origin[1] = d.origin[2] = -0.5f;
    d.vertexColors = {255, 0, 0, 255, 0, 255, 0, 255,
                      0, 0, 255, 255, 128, 128, 128, 255};
    d.meta.insert(QStringLiteral("seed"), 7);
    return d;
}

} // namespace

TEST(Trellis2InterchangeTest, RoundTripPreservesEverything)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("roundtrip.qtm3d");

    const Trellis2Interchange::Data d = makeSample();
    QString err;
    ASSERT_TRUE(Trellis2Interchange::write(path, d, &err)) << err.toStdString();

    const auto r = Trellis2Interchange::read(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.data.vertexCount, 4);
    EXPECT_EQ(r.data.triangleCount, 2);
    EXPECT_EQ(r.data.voxelCount, 2);
    EXPECT_EQ(r.data.positions, d.positions);
    EXPECT_EQ(r.data.indices, d.indices);
    EXPECT_EQ(r.data.voxelCoords, d.voxelCoords);
    EXPECT_EQ(r.data.voxelAttrs, d.voxelAttrs);
    EXPECT_EQ(r.data.vertexColors, d.vertexColors);
    EXPECT_EQ(r.data.resolution, 64);
    EXPECT_FLOAT_EQ(r.data.voxelSize, 1.0f / 64.0f);
    EXPECT_FLOAT_EQ(r.data.origin[0], -0.5f);
    EXPECT_EQ(r.data.meta.value(QStringLiteral("seed")).toInt(), 7);
}

TEST(Trellis2InterchangeTest, OptionalArraysCanBeAbsent)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("minimal.qtm3d");
    Trellis2Interchange::Data d = makeSample();
    d.voxelCoords.clear();
    d.voxelAttrs.clear();
    d.voxelCount = 0;
    d.vertexColors.clear();
    ASSERT_TRUE(Trellis2Interchange::write(path, d));
    const auto r = Trellis2Interchange::read(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.data.voxelCount, 0);
    EXPECT_TRUE(r.data.vertexColors.empty());
}

TEST(Trellis2InterchangeTest, RejectsBadMagicAndTruncation)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString bad = QDir(tmp.path()).filePath("bad.qtm3d");
    {
        QFile f(bad);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("NOTQTM3Dxxxxxxxxxxxx", 20);
    }
    EXPECT_FALSE(Trellis2Interchange::read(bad).ok);

    // Truncate a valid file mid-blob → bounds check must fire.
    const QString path = QDir(tmp.path()).filePath("trunc.qtm3d");
    ASSERT_TRUE(Trellis2Interchange::write(path, makeSample()));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    ASSERT_TRUE(f.resize(f.size() - 24));
    f.close();
    const auto r = Trellis2Interchange::read(path);
    EXPECT_FALSE(r.ok);
}

TEST(Trellis2InterchangeTest, RejectsOutOfRangeIndices)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("oob.qtm3d");
    Trellis2Interchange::Data d = makeSample();
    d.indices[1] = 99;   // >= vertexCount
    ASSERT_TRUE(Trellis2Interchange::write(path, d));
    const auto r = Trellis2Interchange::read(path);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("out of range")));
}

TEST(Trellis2InterchangeTest, MissingFileFailsCleanly)
{
    const auto r = Trellis2Interchange::read(
        QStringLiteral("/nonexistent/nowhere.qtm3d"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}


TEST(Trellis2InterchangeTest, ReadsTrellisCppDump)
{
    // Binary layout of trellis-cli --dump-post: i32 V,F,Mv,res; f32 verts;
    // i32 faces; i32 coords; f32 pbr6.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("post.trellisraw");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        const int32_t hdr[4] = {3, 1, 2, 64};
        const float verts[9] = {0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 0};
        const int32_t faces[3] = {0, 1, 2};
        const int32_t coords[6] = {1, 2, 3, 60, 61, 62};
        const float pbr6[12] = {1.0f, 0.8f, 0.2f, 0.9f, 0.25f, 1.0f,
                                0.0f, 0.5f, 1.0f, 0.0f, 0.75f, 0.5f};
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        f.write(reinterpret_cast<const char*>(verts), sizeof(verts));
        f.write(reinterpret_cast<const char*>(faces), sizeof(faces));
        f.write(reinterpret_cast<const char*>(coords), sizeof(coords));
        f.write(reinterpret_cast<const char*>(pbr6), sizeof(pbr6));
    }
    const auto r = Trellis2Interchange::readTrellisCppDump(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.data.vertexCount, 3);
    EXPECT_EQ(r.data.triangleCount, 1);
    EXPECT_EQ(r.data.voxelCount, 2);
    EXPECT_EQ(r.data.resolution, 64);
    EXPECT_FLOAT_EQ(r.data.voxelSize, 1.0f / 64.0f);
    EXPECT_FLOAT_EQ(r.data.origin[0], -0.5f);
    EXPECT_EQ(r.data.voxelCoords[3], 60u);
    EXPECT_EQ(r.data.voxelAttrs[0], 255);            // 1.0 -> 255
    EXPECT_EQ(r.data.voxelAttrs[4], 64);             // 0.25 -> 64
    EXPECT_EQ(r.data.voxelAttrs[11], 128);           // 0.5 -> 128

    // Face index out of range must be rejected.
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        f.seek(16 + 9 * 4);                          // first face index
        const int32_t bad = 7;
        f.write(reinterpret_cast<const char*>(&bad), 4);
    }
    EXPECT_FALSE(Trellis2Interchange::readTrellisCppDump(path).ok);
    // Truncation must be rejected.
    EXPECT_FALSE(Trellis2Interchange::readTrellisCppDump(
                     QStringLiteral("/nonexistent/x.trellisraw")).ok);
}
