// Coverage tests for CLIPipeline::cmdIsometric — end-to-end render path with
// real on-disk mesh assets (robot.mesh fallback to generated cube .obj).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <vector>

#include "CLIPipeline.h"
#include "MeshImporterExporter.h"
#include "Manager.h"
#include "ModelIsometricRenderer.h"
#include "TestHelpers.h"

namespace {

class ArgvBuilder {
public:
    explicit ArgvBuilder(const QStringList &args)
    {
        for (const QString &a : args)
            m_storage.push_back(a.toUtf8());
        for (auto &ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() { return m_argc; }
    char **argv() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char *> m_argv;
    int m_argc = 0;
};

QString writeCubeObj(const QString &dirPath, const QString &fileName)
{
    const QString path = QDir(dirPath).filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write(
        "o Cube\n"
        "v -1 -1 -1\n"
        "v  1 -1 -1\n"
        "v  1  1 -1\n"
        "v -1  1 -1\n"
        "v -1 -1  1\n"
        "v  1 -1  1\n"
        "v  1  1  1\n"
        "v -1  1  1\n"
        "f 1 2 3\n"
        "f 1 3 4\n"
        "f 5 6 7\n"
        "f 5 7 8\n"
        "f 1 2 6\n"
        "f 1 6 5\n");
    f.close();
    return path;
}

QString modelsDir()
{
#ifdef QTMESH_UT_SOURCE_ROOT
    return QDir(QString::fromUtf8(QTMESH_UT_SOURCE_ROOT)).filePath(QStringLiteral("media/models"));
#else
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.cdUp() && dir.cdUp())
        return dir.absoluteFilePath(QStringLiteral("media/models"));
    return QStringLiteral("./media/models");
#endif
}

QByteArray firstAnimNameForFile(const QString &filePath)
{
    if (!Manager::getSingletonPtr())
        return QByteArray();

    auto *mgr = Manager::getSingleton();
    auto nodes = mgr->getSceneNodes();
    for (auto *node : nodes) {
        mgr->destroyAllAttachedMovableObjects(node);
        mgr->destroySceneNode(node);
    }

    MeshImporterExporter::importer({filePath});
    auto &entities = mgr->getEntities();
    QByteArray name;
    if (!entities.isEmpty() && entities.first()->hasSkeleton()) {
        Ogre::SkeletonPtr skel = entities.first()->getMesh()->getSkeleton();
        if (skel && skel->getNumAnimations() > 0)
            name = QString::fromStdString(
                       skel->getAnimation(static_cast<unsigned short>(0))->getName())
                       .toUtf8();
    }

    nodes = mgr->getSceneNodes();
    for (auto *node : nodes) {
        mgr->destroyAllAttachedMovableObjects(node);
        mgr->destroySceneNode(node);
    }
    return name;
}

class CLIPipelineCmdIsometricCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(m_tmp.isValid());
    }

    QString meshInput(const QString &objFallbackName)
    {
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot))
            return robot;
        const QString obj = writeCubeObj(m_tmp.path(), objFallbackName);
        EXPECT_FALSE(obj.isEmpty());
        return obj;
    }

    QString outPath(const QString &name) const { return m_tmp.filePath(name); }

    QTemporaryDir m_tmp;
};

TEST_F(CLIPipelineCmdIsometricCoverageTest, StaticGridWritesPngOnDisk)
{
    const QString mesh = meshInput("iso_static.obj");
    const QString out = outPath("iso_static.png");

    ArgvBuilder args({"qtmesh", "isometric", mesh, "-o", out, "--directions", "4", "--resolution", "40"});
    EXPECT_EQ(CLIPipeline::cmdIsometric(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 40);
    EXPECT_EQ(img.height(), 160);
}

TEST_F(CLIPipelineCmdIsometricCoverageTest, JsonFlagStillWritesGridPng)
{
    const QString mesh = meshInput("iso_json.obj");
    const QString out = outPath("iso_json.png");

    ArgvBuilder args({"qtmesh", "isometric", mesh, "-o", out, "--directions", "2", "--size", "32", "--json"});
    EXPECT_EQ(CLIPipeline::cmdIsometric(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 32);
    EXPECT_EQ(img.height(), 64);
}

TEST_F(CLIPipelineCmdIsometricCoverageTest, ElevationAndStartAzimuthVariants)
{
    const QString mesh = meshInput("iso_elev.obj");
    const QString out = outPath("iso_elev.png");

    ArgvBuilder args({"qtmesh", "isometric", mesh, "-o", out, "--directions", "2", "--size", "36",
                      "--elevation", "25", "--start-azimuth", "15"});
    EXPECT_EQ(CLIPipeline::cmdIsometric(args.argc(), args.argv()), 0);
    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    EXPECT_EQ(img.width(), 36);
    EXPECT_EQ(img.height(), 72);
}

TEST_F(CLIPipelineCmdIsometricCoverageTest, CameraPaddingJsonReport)
{
    const QString mesh = meshInput("iso_pad.obj");
    const QString out = outPath("iso_pad.png");

    ArgvBuilder args({"qtmesh", "isometric", mesh, "-o", out, "--directions", "2", "--size", "28",
                      "--padding", "1.5", "--json"});
    EXPECT_EQ(CLIPipeline::cmdIsometric(args.argc(), args.argv()), 0);
    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 28);
    EXPECT_EQ(img.height(), 56);
}

TEST_F(CLIPipelineCmdIsometricCoverageTest, AnimatedGridWhenAssetAvailable)
{
    const QString fbx = modelsDir() + "/Twist Dance.fbx";
    if (!QFile::exists(fbx))
        GTEST_SKIP() << "Twist Dance.fbx not available";

    const QByteArray animName = firstAnimNameForFile(fbx);
    if (animName.isEmpty())
        GTEST_SKIP() << "No animation found in Twist Dance.fbx";

    const QString out = outPath("iso_anim.png");
    ArgvBuilder args({"qtmesh", "isometric", fbx, "-o", out, "--animation", animName.constData(), "--frames", "4",
                      "--directions", "4", "--size", "32"});
    EXPECT_EQ(CLIPipeline::cmdIsometric(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 128);
    EXPECT_EQ(img.height(), 128);
}

} // namespace
