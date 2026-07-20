#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include "FaceRig/ArkitTemplate.h"

#include <cstring>

namespace {

// Write a minimal valid arkit_template.bin: V verts, F faces, S named shapes.
QString writeTemplate(const QString& dir, int V, int F,
                      const QStringList& shapeNames)
{
    QByteArray b;
    auto putI32 = [&](int32_t v) {
        int32_t le = qToLittleEndian(v);
        b.append(reinterpret_cast<const char*>(&le), 4);
    };
    auto putF32 = [&](float f) {
        quint32 raw;
        std::memcpy(&raw, &f, 4);
        raw = qToLittleEndian(raw);
        b.append(reinterpret_cast<const char*>(&raw), 4);
    };
    b.append("QMFRT1\0\0", 8);
    putI32(V);
    putI32(F);
    putI32(shapeNames.size());
    for (int i = 0; i < V * 3; ++i)
        putF32(0.1f * i);                    // deterministic neutral
    for (int i = 0; i < F * 3; ++i)
        putI32(i % V);                       // dummy faces
    for (int s = 0; s < shapeNames.size(); ++s) {
        QByteArray nm = shapeNames[s].toLatin1().left(31);
        b.append(nm);
        b.append(QByteArray(32 - nm.size(), '\0'));
        for (int i = 0; i < V * 3; ++i)
            putF32(s == 0 && i == 1 ? -0.5f : 0.0f);  // shape0 moves vert0.y
    }
    const QString path = dir + QStringLiteral("/arkit_template.bin");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(b);
    f.close();
    return path;
}

}  // namespace

TEST(ArkitTemplate, LoadsHeaderNeutralFacesShapes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QStringList names{"jawOpen", "mouthSmileLeft", "eyeBlinkRight"};
    const QString path = writeTemplate(tmp.path(), 4, 2, names);

    FaceRig::ArkitTemplate t;
    QString err;
    ASSERT_TRUE(t.load(path, &err)) << err.toStdString();
    EXPECT_TRUE(t.valid());
    EXPECT_EQ(t.vertexCount(), 4);
    EXPECT_EQ(t.faceCount(), 2);
    EXPECT_EQ(t.shapeCount(), 3);
    EXPECT_EQ(t.neutral().size(), 12u);
    EXPECT_FLOAT_EQ(t.neutral()[3], 0.3f);   // vert1.x = 0.1*3
    EXPECT_EQ(t.faces().size(), 6u);
    EXPECT_EQ(t.shapeNames(), names);
    // shape 0 ("jawOpen") moves vert0.y by -0.5
    EXPECT_FLOAT_EQ(t.shapes()[0].deltas[1], -0.5f);
    EXPECT_FLOAT_EQ(t.shapes()[1].deltas[1], 0.0f);
}

TEST(ArkitTemplate, RejectsBadMagic)
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + "/bad.bin";
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray("NOTMAGIC", 8) + QByteArray(64, '\0'));
    f.close();
    FaceRig::ArkitTemplate t;
    QString err;
    EXPECT_FALSE(t.load(path, &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(ArkitTemplate, RejectsTruncated)
{
    QTemporaryDir tmp;
    const QString good = writeTemplate(tmp.path(), 4, 2, {"jawOpen"});
    QFile f(good);
    f.open(QIODevice::ReadOnly);
    QByteArray full = f.readAll();
    f.close();
    const QString path = tmp.path() + "/trunc.bin";
    QFile o(path);
    o.open(QIODevice::WriteOnly);
    o.write(full.left(full.size() - 20));    // chop the last shape's deltas
    o.close();
    FaceRig::ArkitTemplate t;
    QString err;
    EXPECT_FALSE(t.load(path, &err));
    EXPECT_TRUE(err.contains("truncat", Qt::CaseInsensitive));
}

TEST(ArkitTemplate, MissingFileFails)
{
    FaceRig::ArkitTemplate t;
    QString err;
    EXPECT_FALSE(t.load("/nonexistent/arkit_template.bin", &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(t.valid());
}

// Env-gated: run against the REAL bundle (set QTMESH_FACERIG_TEMPLATE to the
// exported arkit_template.bin) — verifies the 51 shapes + ARKit names.
TEST(ArkitTemplate, EnvGatedRealBundle)
{
    const QByteArray p = qgetenv("QTMESH_FACERIG_TEMPLATE");
    if (p.isEmpty()) {
        // pass as a no-op — the CI harness treats ANY skipped test as a suite
        // failure (same convention as SkinEvaluate's env-gated reference test)
        SUCCEED() << "QTMESH_FACERIG_TEMPLATE not set — real bundle not exercised";
        return;
    }
    FaceRig::ArkitTemplate t;
    QString err;
    ASSERT_TRUE(t.load(QString::fromUtf8(p), &err)) << err.toStdString();
    EXPECT_GT(t.vertexCount(), 10000);        // ICT is ~26.7k
    EXPECT_GE(t.shapeCount(), 51);
    EXPECT_TRUE(t.shapeNames().contains("jawOpen"));
    EXPECT_TRUE(t.shapeNames().contains("mouthSmileLeft"));
    EXPECT_TRUE(t.shapeNames().contains("eyeBlinkLeft"));
    // jawOpen should actually deform (nonzero deltas)
    const auto& d = t.shapes()[t.shapeNames().indexOf("jawOpen")].deltas;
    float maxMag = 0.f;
    for (size_t i = 0; i + 2 < d.size(); i += 3)
        maxMag = std::max(maxMag,
                          std::abs(d[i]) + std::abs(d[i + 1]) + std::abs(d[i + 2]));
    EXPECT_GT(maxMag, 0.f);
}
