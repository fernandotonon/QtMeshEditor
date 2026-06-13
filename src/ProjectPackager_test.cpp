#include "DependencyResolver.h"
#include "ProjectPackager.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

TEST(DependencyResolver, ObjMtlTextureDetection)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString objPath = QDir(dir.path()).filePath(QStringLiteral("model.obj"));
    const QString mtlPath = QDir(dir.path()).filePath(QStringLiteral("model.mtl"));
    const QString texPath = QDir(dir.path()).filePath(QStringLiteral("albedo.png"));

    QFile tex(texPath);
    ASSERT_TRUE(tex.open(QIODevice::WriteOnly));
    tex.write("png");
    tex.close();

    QFile mtl(mtlPath);
    ASSERT_TRUE(mtl.open(QIODevice::WriteOnly | QIODevice::Text));
    mtl.write("newmtl mat0\nmap_Kd albedo.png\n");
    mtl.close();

    QFile obj(objPath);
    ASSERT_TRUE(obj.open(QIODevice::WriteOnly | QIODevice::Text));
    obj.write("mtllib model.mtl\n");
    obj.close();

    const auto deps = DependencyResolver::detectFromFiles(objPath);
    ASSERT_GE(deps.size(), 3);
    bool hasMain = false;
    bool hasTexture = false;
    for (const DependencyEntry& entry : deps) {
        if (entry.role == QLatin1String("main"))
            hasMain = true;
        if (entry.absolutePath.endsWith(QStringLiteral("albedo.png")))
            hasTexture = true;
    }
    EXPECT_TRUE(hasMain);
    EXPECT_TRUE(hasTexture);
}

TEST(DependencyResolver, RsdSidecarDetection)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString rsdPath = QDir(dir.path()).filePath(QStringLiteral("stage.rsd"));
    const QString timPath = QDir(dir.path()).filePath(QStringLiteral("wall.tim"));
    QFile tim(timPath);
    ASSERT_TRUE(tim.open(QIODevice::WriteOnly));
    tim.write("tim");
    tim.close();

    QFile rsd(rsdPath);
    ASSERT_TRUE(rsd.open(QIODevice::WriteOnly | QIODevice::Text));
    rsd.write("@RSD940102\nPLY=STAGE.PLY\nMAT=STAGE.MAT\nNTEX=1\nTEX[0]=wall.tim\n");
    rsd.close();

    const auto deps = DependencyResolver::detectFromFiles(rsdPath);
    bool hasTim = false;
    for (const DependencyEntry& entry : deps) {
        if (entry.absolutePath.endsWith(QStringLiteral("wall.tim")))
            hasTim = true;
    }
    EXPECT_TRUE(hasTim);
}

TEST(ProjectPackager, SanitisedManifestJson)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString secretRoot = QDir(dir.path()).filePath(QStringLiteral("Users/SECRET/models"));
    QDir().mkpath(secretRoot);
    const QString mainPath = QDir(secretRoot).filePath(QStringLiteral("hero.obj"));
    const QString mtlPath = QDir(secretRoot).filePath(QStringLiteral("hero.mtl"));
    const QString texPath = QDir(secretRoot).filePath(QStringLiteral("skin.png"));

    for (const QString& path : {mainPath, mtlPath, texPath}) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
    }
    QFile mtl(mtlPath);
    ASSERT_TRUE(mtl.open(QIODevice::WriteOnly | QIODevice::Text));
    mtl.write("map_Kd skin.png\n");
    mtl.close();

    const PackageMetadata metadata =
        ProjectPackager::buildManifest(mainPath, {}, QStringLiteral("Hero Pack"));
    const QJsonObject json = ProjectPackager::toJson(metadata);
    const QString serialised = QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact));

    EXPECT_FALSE(serialised.contains(QStringLiteral("SECRET")));
    EXPECT_TRUE(ProjectPackager::jsonPassesPathSanitisationLint(json));
    EXPECT_FALSE(metadata.mainFile.contains(QStringLiteral("/Users/")));
    EXPECT_GE(metadata.files.size(), 2);
}

TEST(ProjectPackager, ComputesSha256)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("cube.mesh"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mesh-bytes");
    file.close();

    const PackageMetadata metadata = ProjectPackager::buildManifest(path, {}, QStringLiteral("Cube"));
    ASSERT_EQ(metadata.files.size(), 1);
    EXPECT_FALSE(metadata.files.first().sha256.isEmpty());
    EXPECT_EQ(metadata.files.first().sha256.size(), 64);
}
