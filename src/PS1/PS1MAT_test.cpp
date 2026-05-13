#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "PS1/PS1MAT.h"

namespace {

QString writeMatFile(const QString& dir, const QByteArray& body)
{
    const QString path = QDir(dir).filePath(QStringLiteral("test.mat"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    f.write(body);
    return path;
}

} // namespace

TEST(PS1MAT, ParseFlatColorEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QByteArray body =
        "@MAT940801\n"
        "3\n"
        "0 0 F C 207 207 207\n"
        "1 0 F C 58 58 58\n"
        "2 0 F C 152 152 152\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, entries, &err)) << err.toStdString();
    ASSERT_EQ(entries.size(), 3);

    EXPECT_EQ(entries[0].typeChar, 'C');
    EXPECT_FALSE(entries[0].textured);
    EXPECT_EQ(entries[0].textureIndex, -1);
    EXPECT_TRUE(entries[0].uvs.isEmpty());
    ASSERT_EQ(entries[0].vertColors.size(), 1);
    EXPECT_EQ(entries[0].vertColors[0], QColor(207, 207, 207));
    EXPECT_EQ(entries[0].rgb,           QColor(207, 207, 207));
    EXPECT_FALSE(entries[0].unlit);
    EXPECT_FALSE(entries[1].unlit);
    EXPECT_FALSE(entries[2].unlit);
}

TEST(PS1MAT, ParseSmoothColorQuadEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // G-type quad: 4 RGB triples after the type char.
    const QByteArray body =
        "@MAT940801\n"
        "1\n"
        "0 0 F G 152 152 152 197 197 197 84 84 84 120 120 120\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, entries, &err)) << err.toStdString();
    ASSERT_EQ(entries.size(), 1);

    const auto& e = entries[0];
    EXPECT_EQ(e.typeChar, 'G');
    EXPECT_FALSE(e.textured);
    ASSERT_EQ(e.vertColors.size(), 4);
    EXPECT_EQ(e.vertColors[0], QColor(152, 152, 152));
    EXPECT_EQ(e.vertColors[1], QColor(197, 197, 197));
    EXPECT_EQ(e.vertColors[2], QColor(84, 84, 84));
    EXPECT_EQ(e.vertColors[3], QColor(120, 120, 120));
    // Back-compat rgb is the first vert colour.
    EXPECT_EQ(e.rgb, QColor(152, 152, 152));
    EXPECT_FALSE(e.unlit);
}

TEST(PS1MAT, ParseMaterialFlag_UnlitUsesLsb)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QByteArray body =
        "@MAT940801\n"
        "2\n"
        "0 0 F C 255 0 0\n"
        "1 1 F C 0 255 0\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, entries, &err)) << err.toStdString();
    ASSERT_EQ(entries.size(), 2);
    EXPECT_FALSE(entries[0].unlit);
    EXPECT_TRUE(entries[1].unlit);
}

TEST(PS1MAT, ParseTexturedQuadEntry_HType)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // H-type quad: texIdx=0, 8 UVs, 4 RGB triples = 21 trailing ints.
    const QByteArray body =
        "@MAT940801\n"
        "1\n"
        "0 0 F H 0 0 127 0 0 127 127 127 0 72 72 72 72 72 72 79 79 79 80 80 80\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, entries, &err)) << err.toStdString();
    ASSERT_EQ(entries.size(), 1);

    const auto& e = entries[0];
    EXPECT_EQ(e.typeChar, 'H');
    EXPECT_TRUE(e.textured);
    EXPECT_EQ(e.textureIndex, 0);
    ASSERT_EQ(e.uvs.size(), 4);
    EXPECT_EQ(e.uvs[0].u, 0);   EXPECT_EQ(e.uvs[0].v, 127);
    EXPECT_EQ(e.uvs[1].u, 0);   EXPECT_EQ(e.uvs[1].v, 0);
    EXPECT_EQ(e.uvs[2].u, 127); EXPECT_EQ(e.uvs[2].v, 127);
    EXPECT_EQ(e.uvs[3].u, 127); EXPECT_EQ(e.uvs[3].v, 0);
    ASSERT_EQ(e.vertColors.size(), 4);
    EXPECT_EQ(e.vertColors[0], QColor(72, 72, 72));
    EXPECT_EQ(e.vertColors[3], QColor(80, 80, 80));
}

TEST(PS1MAT, ParseTexturedTriEntry_TType)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // T-type tri: texIdx=2 + 8 UVs (last is padding for tri).
    const QByteArray body =
        "@MAT940801\n"
        "1\n"
        "0 0 F T 2 10 20 30 40 50 60 0 0\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, entries, &err)) << err.toStdString();
    ASSERT_EQ(entries.size(), 1);

    const auto& e = entries[0];
    EXPECT_EQ(e.typeChar, 'T');
    EXPECT_TRUE(e.textured);
    EXPECT_EQ(e.textureIndex, 2);
    ASSERT_EQ(e.uvs.size(), 4);
    EXPECT_EQ(e.uvs[0].u, 10); EXPECT_EQ(e.uvs[0].v, 20);
    EXPECT_EQ(e.uvs[1].u, 30); EXPECT_EQ(e.uvs[1].v, 40);
    EXPECT_EQ(e.uvs[2].u, 50); EXPECT_EQ(e.uvs[2].v, 60);
    EXPECT_TRUE(e.vertColors.isEmpty());
}

TEST(PS1MAT, WriteAndRoundTrip_MixedEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("rt.mat"));

    QVector<PS1MAT::MatEntry> in;
    {
        PS1MAT::MatEntry c;
        c.typeChar = 'C';
        c.rgb = QColor(100, 110, 120);
        c.vertColors.push_back(c.rgb);
        in.push_back(c);
    }
    {
        PS1MAT::MatEntry t;
        t.typeChar = 'T';
        t.textured = true;
        t.textureIndex = 1;
        t.uvs = { {0, 0}, {127, 0}, {127, 127}, {0, 127} };
        t.unlit = true;
        in.push_back(t);
    }
    {
        PS1MAT::MatEntry h;
        h.typeChar = 'H';
        h.textured = true;
        h.textureIndex = 0;
        h.uvs = { {10, 20}, {30, 40}, {50, 60}, {70, 80} };
        h.vertColors = {
            QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255), QColor(128, 128, 128)
        };
        h.rgb = h.vertColors.first();
        in.push_back(h);
    }

    QString err;
    ASSERT_TRUE(PS1MAT::writeMatFile(path, in, &err)) << err.toStdString();

    QVector<PS1MAT::MatEntry> out;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, out, &err)) << err.toStdString();
    ASSERT_EQ(out.size(), in.size());

    EXPECT_EQ(out[0].typeChar, 'C');
    EXPECT_EQ(out[0].rgb, QColor(100, 110, 120));

    EXPECT_EQ(out[1].typeChar, 'T');
    EXPECT_TRUE(out[1].textured);
    EXPECT_EQ(out[1].textureIndex, 1);
    ASSERT_EQ(out[1].uvs.size(), 4);
    EXPECT_EQ(out[1].uvs[2].u, 127);

    EXPECT_EQ(out[2].typeChar, 'H');
    EXPECT_TRUE(out[2].textured);
    EXPECT_EQ(out[2].textureIndex, 0);
    ASSERT_EQ(out[2].uvs.size(), 4);
    ASSERT_EQ(out[2].vertColors.size(), 4);
    EXPECT_EQ(out[2].vertColors[0], QColor(255, 0, 0));
    EXPECT_EQ(out[2].vertColors[2], QColor(0, 0, 255));
    EXPECT_FALSE(out[0].unlit);
    EXPECT_TRUE(out[1].unlit);
    EXPECT_FALSE(out[2].unlit);
}

TEST(PS1MAT, WriteAndRoundTrip_PreservesSmoothShadingToken)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("smooth.mat"));

    QVector<PS1MAT::MatEntry> in;
    PS1MAT::MatEntry g;
    g.typeChar = 'G';
    g.shadingChar = 'G';
    g.vertColors = {
        QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255), QColor(255, 255, 255)
    };
    g.rgb = g.vertColors.first();
    in.push_back(g);

    QString err;
    ASSERT_TRUE(PS1MAT::writeMatFile(path, in, &err)) << err.toStdString();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromLatin1(f.readAll());
    EXPECT_TRUE(text.contains(QStringLiteral("0 0 G G "))) << text.toStdString();

    QVector<PS1MAT::MatEntry> out;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, out, &err)) << err.toStdString();
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].shadingChar, 'G');
}

TEST(PS1MAT, WritesGouraudTriWithFourCornerPadding)
{
    // Repro for Example Project.rsd round-trip: the Blender RSD exporter pads tri G
    // entries with a trailing `0 0 0` so every G row has 12 colour ints. Match that
    // convention so PS1 emulators and third-party tools see a stable layout.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QVector<PS1MAT::MatEntry> in;
    {
        PS1MAT::MatEntry g;
        g.typeChar = 'G';
        g.shadingChar = 'F';
        g.vertColors = {
            QColor(245, 245, 245), QColor(107, 107, 107), QColor(84, 84, 84)
        };
        g.rgb = g.vertColors.first();
        in.push_back(g);
    }

    const QString path = QDir(dir.path()).filePath(QStringLiteral("triG.mat"));
    QString err;
    ASSERT_TRUE(PS1MAT::writeMatFile(path, in, &err)) << err.toStdString();

    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromLatin1(f.readAll());
    EXPECT_TRUE(text.contains(QStringLiteral("0 0 F G 245 245 245 107 107 107 84 84 84 0 0 0")))
        << "tri G entry must be padded to 4 corners with a trailing 0 0 0 -- got:\n"
        << text.toStdString();

    // Round-trip should still parse the padding back into a 4-colour vector; the
    // importer is responsible for truncating to the PLY face shape.
    QVector<PS1MAT::MatEntry> out;
    ASSERT_TRUE(PS1MAT::parseMatFile(path, out, &err)) << err.toStdString();
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].typeChar, 'G');
    ASSERT_EQ(out[0].vertColors.size(), 4);
    EXPECT_EQ(out[0].vertColors[3], QColor(0, 0, 0));
}

TEST(PS1MAT, RejectsMissingHeader)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QByteArray body =
        "3\n"
        "0 1 F C 207 207 207\n";

    const QString path = writeMatFile(dir.path(), body);
    QVector<PS1MAT::MatEntry> entries;
    QString err;
    EXPECT_FALSE(PS1MAT::parseMatFile(path, entries, &err));
    EXPECT_FALSE(err.isEmpty());
}
