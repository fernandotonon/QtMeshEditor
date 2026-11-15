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
    // Base color is stored RAW: TRELLIS.2 already predicts sRGB (Microsoft's
    // GLB export writes these values straight into baseColorTexture). The
    // old extra linear->sRGB encode turned 0.8 into 231 and 0.5 into 188.
    EXPECT_EQ(r.data.voxelAttrs[1], 204);            // 0.8 -> 204, not 231
    EXPECT_EQ(r.data.voxelAttrs[7], 128);            // 0.5 -> 128, not 188
    EXPECT_EQ(r.data.meta.value(QStringLiteral("colorSpace")).toString(),
              QStringLiteral("srgb-v2"));
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

namespace {

// Writes `d` and rewrites the color-space tag in the file in place (same
// byte length, JSON whitespace pads the shorter value), so tests can produce
// files exactly as older builds stamped them.
QString writeWithTag(const QTemporaryDir& tmp, const Trellis2Interchange::Data& d,
                     const QByteArray& replacement)
{
    const QString path = QDir(tmp.path()).filePath("tagged.qtm3d");
    QString err;
    if (!Trellis2Interchange::write(path, d, &err))
        return QString();
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return QString();
    QByteArray bytes = f.readAll();
    const QByteArray current("\"colorSpace\":\"srgb-v2\"");
    const qsizetype at = bytes.indexOf(current);
    if (at < 0 || replacement.size() != current.size())
        return QString();
    bytes.replace(at, current.size(), replacement);
    f.seek(0);
    f.write(bytes);
    return path;
}

} // namespace

TEST(Trellis2InterchangeTest, OldSrgbTagIsDecodedOnceOnLoad)
{
    // Files written between #978 and the fix carry raw output run through an
    // unneeded linear->sRGB encode and the tag "srgb". Reading undoes it once.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    Trellis2Interchange::Data d = makeSample();
    d.voxelAttrs = {231, 188, 0, 10, 200, 255,     // 0.8 / 0.5 encoded by the old code
                    255, 0, 128, 250, 20, 128};
    const QString path = writeWithTag(tmp, d, QByteArray("\"colorSpace\":\"srgb\"   "));
    ASSERT_FALSE(path.isEmpty());
    const auto r = Trellis2Interchange::read(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_NEAR(r.data.voxelAttrs[0], 204, 1);  // back to 0.8
    EXPECT_NEAR(r.data.voxelAttrs[1], 128, 1);  // back to 0.5
    EXPECT_EQ(r.data.voxelAttrs[2], 0);
    EXPECT_EQ(r.data.voxelAttrs[3], 10);        // metallic: untouched
    EXPECT_EQ(r.data.voxelAttrs[4], 200);       // roughness: untouched
    EXPECT_EQ(r.data.voxelAttrs[6], 255);
    EXPECT_NEAR(r.data.voxelAttrs[8], 55, 1);   // 128 (sRGB) -> 0.216
    EXPECT_EQ(r.data.voxelAttrs[9], 250);
    EXPECT_EQ(r.data.meta.value(QStringLiteral("colorSpace")).toString(),
              QStringLiteral("srgb-v2"));
}

TEST(Trellis2InterchangeTest, UntaggedFilesAreRawAndKeptAsIs)
{
    // Pre-#978 files and the Python sidecar write raw model output with no
    // tag: those were right all along and must not be converted.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Trellis2Interchange::Data d = makeSample();
    const QString path = writeWithTag(tmp, d, QByteArray("\"colorSpacX\":\"srgb-v2\""));
    ASSERT_FALSE(path.isEmpty());
    const auto r = Trellis2Interchange::read(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.data.voxelAttrs, d.voxelAttrs);
}

TEST(Trellis2InterchangeTest, RoundTripNeverReconverts)
{
    // A stale "srgb" in the in-memory meta must not survive write(): the next
    // read() would decode data that is already correct.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    Trellis2Interchange::Data d = makeSample();
    d.meta.insert(QStringLiteral("colorSpace"), QStringLiteral("srgb"));
    const QString path = QDir(tmp.path()).filePath("rt.qtm3d");
    QString err;
    ASSERT_TRUE(Trellis2Interchange::write(path, d, &err)) << err.toStdString();
    const auto r1 = Trellis2Interchange::read(path);
    ASSERT_TRUE(r1.ok);
    EXPECT_EQ(r1.data.voxelAttrs, d.voxelAttrs);
    ASSERT_TRUE(Trellis2Interchange::write(path, r1.data, &err));
    const auto r2 = Trellis2Interchange::read(path);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r2.data.voxelAttrs, d.voxelAttrs);
}
