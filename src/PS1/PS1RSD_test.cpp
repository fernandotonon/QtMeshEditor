#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "PS1/PS1RSD.h"

TEST(PS1RSD, ParseAsciiRsd_BasicKeys)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString rsdPath = QDir(dir.path()).filePath(QStringLiteral("model.rsd"));

    const QByteArray rsdBytes =
        "@RSD940102\n"
        "# comment\n"
        "PLY=MODEL.TMD\n"
        "MAT=MODEL.MAT\n"
        "GRP=MODEL.GRP\n"
        "NTEX=3\n"
        "TEX[0]=A.TIM\n"
        "TEX[1]=B.TIM\n"
        "TEX[2]=C.TIM\n";

    QFile f(rsdPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    ASSERT_EQ(f.write(rsdBytes), rsdBytes.size());
    f.close();

    PS1RSD::RsdDescriptor d;
    QString err;
    ASSERT_TRUE(PS1RSD::parseRsdFile(rsdPath, d, &err)) << err.toStdString();
    EXPECT_EQ(d.headerId, "@RSD940102");
    EXPECT_EQ(d.plyPath, "MODEL.TMD");
    EXPECT_EQ(d.matPath, "MODEL.MAT");
    EXPECT_EQ(d.grpPath, "MODEL.GRP");
    EXPECT_EQ(d.ntex, 3);
    ASSERT_EQ(d.textures.size(), 3);
    EXPECT_EQ(d.textures[0], "A.TIM");
    EXPECT_EQ(d.textures[1], "B.TIM");
    EXPECT_EQ(d.textures[2], "C.TIM");
}

TEST(PS1RSD, WriteAndParse_RoundTrip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString rsdPath = QDir(dir.path()).filePath(QStringLiteral("out.rsd"));

    PS1RSD::RsdDescriptor in;
    in.headerId = "@RSD940102";
    in.plyPath = "MODEL.TMD";
    in.ntex = 1;
    in.textures = { "T0.TIM" };

    QString err;
    ASSERT_TRUE(PS1RSD::writeRsdFile(rsdPath, in, &err)) << err.toStdString();

    PS1RSD::RsdDescriptor out;
    ASSERT_TRUE(PS1RSD::parseRsdFile(rsdPath, out, &err)) << err.toStdString();
    EXPECT_EQ(out.headerId, "@RSD940102");
    EXPECT_EQ(out.plyPath, "MODEL.TMD");
    EXPECT_EQ(out.ntex, 1);
    ASSERT_EQ(out.textures.size(), 1);
    EXPECT_EQ(out.textures[0], "T0.TIM");
}

TEST(PS1RSD, ParseBlenderExporterTextureLayout)
{
    // Reproduces the descriptor produced by the PlayStation-RSD-Blender exporter when
    // a single texture is bound (e.g. Wood.jpg). The parser must accept .jpg / .png
    // entries even though the historical Psy-Q toolchain emitted .tim.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString rsdPath = QDir(dir.path()).filePath(QStringLiteral("blender.rsd"));
    const QByteArray rsdBytes =
        "#RSD data describing the relationships to the PLY, MAT, and texture files\n"
        "@RSD940102 \n"
        "PLY=Example Project.ply\n"
        "MAT=Example Project.mat\n"
        "NTEX=1\n"
        "TEX[0]=Wood.jpg\n";

    QFile f(rsdPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    ASSERT_EQ(f.write(rsdBytes), rsdBytes.size());
    f.close();

    PS1RSD::RsdDescriptor d;
    QString err;
    ASSERT_TRUE(PS1RSD::parseRsdFile(rsdPath, d, &err)) << err.toStdString();
    EXPECT_EQ(d.ntex, 1);
    ASSERT_EQ(d.textures.size(), 1);
    EXPECT_EQ(d.textures[0], "Wood.jpg");
    EXPECT_EQ(d.plyPath, "Example Project.ply");
    EXPECT_EQ(d.matPath, "Example Project.mat");
}

