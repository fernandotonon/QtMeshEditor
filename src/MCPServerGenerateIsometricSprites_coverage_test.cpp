// Coverage tests for MCPServer::toolGenerateIsometricSprites (#724).
//
// Exercises validation branches and the end-to-end file-in / PNG-out path via
// callTool("generate_isometric_sprites", …) — same renderer as `qtmesh isometric`.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include "MCPServer.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <memory>

namespace {

QString isoResultText(const QJsonObject &result)
{
    const QJsonArray content = result["content"].toArray();
    if (content.isEmpty())
        return QString();
    return content[0].toObject()["text"].toString();
}

bool isoIsError(const QJsonObject &result)
{
    return result["isError"].toBool(false);
}

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

bool imageHasNonBackgroundPixels(const QImage &img)
{
    if (img.isNull() || img.width() < 2 || img.height() < 2)
        return false;
    const QRgb ref = img.pixel(0, 0);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            if (qAbs(qRed(px) - qRed(ref)) > 8 || qAbs(qGreen(px) - qGreen(ref)) > 8
                || qAbs(qBlue(px) - qBlue(ref)) > 8 || qAbs(qAlpha(px) - qAlpha(ref)) > 8)
                return true;
        }
    }
    return false;
}

class MCPServerGenerateIsometricSpritesCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication *>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "Mesh resources unavailable in test environment";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
        ASSERT_TRUE(tmp.isValid());
    }

    void TearDown() override
    {
        server.reset();
        Manager::kill();
        if (app)
            app->processEvents();
    }

    QString meshInput(const QString &objName)
    {
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot))
            return robot;
        const QString obj = writeCubeObj(tmp.path(), objName);
        EXPECT_FALSE(obj.isEmpty());
        return obj;
    }

    std::unique_ptr<MCPServer> server;
    QApplication *app = nullptr;
    QTemporaryDir tmp;
};

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, MissingArgsReturnsError)
{
    const QJsonObject result = server->callTool(QStringLiteral("generate_isometric_sprites"), {});
    EXPECT_TRUE(isoIsError(result));
    EXPECT_TRUE(isoResultText(result).contains(QStringLiteral("missing required")));
}

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, MissingFileReturnsError)
{
    QJsonObject args;
    args["output"] = tmp.filePath(QStringLiteral("out.png"));
    const QJsonObject result = server->callTool(QStringLiteral("generate_isometric_sprites"), args);
    EXPECT_TRUE(isoIsError(result));
    EXPECT_TRUE(isoResultText(result).contains(QStringLiteral("missing required")));
}

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, NonexistentFileReturnsError)
{
    const QString missing = tmp.filePath(QStringLiteral("definitely_missing_mesh.fbx"));
    ASSERT_FALSE(QFile::exists(missing));

    QJsonObject args;
    args["file"] = missing;
    args["output"] = tmp.filePath(QStringLiteral("out.png"));
    const QJsonObject result = server->callTool(QStringLiteral("generate_isometric_sprites"), args);
    EXPECT_TRUE(isoIsError(result));
    EXPECT_TRUE(isoResultText(result).contains(QStringLiteral("file not found"), Qt::CaseInsensitive));
}

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, InvalidResolutionReturnsError)
{
    const QString mesh = meshInput(QStringLiteral("iso_bad_res.obj"));
    QJsonObject args;
    args["file"] = mesh;
    args["output"] = tmp.filePath(QStringLiteral("out.png"));
    args["resolution"] = 8;
    const QJsonObject result = server->callTool(QStringLiteral("generate_isometric_sprites"), args);
    EXPECT_TRUE(isoIsError(result));
    EXPECT_TRUE(isoResultText(result).contains(QStringLiteral("resolution")));
}

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, StaticGridWritesPngWithMeshPixels)
{
    const QString mesh = meshInput(QStringLiteral("iso_mcp.obj"));
    const QString out = tmp.filePath(QStringLiteral("iso_mcp.png"));

    QJsonObject args;
    args["file"] = mesh;
    args["output"] = out;
    args["directions"] = 2;
    args["resolution"] = 32;
    args["elevation"] = 30.0;

    const QJsonObject result = server->callTool(QStringLiteral("generate_isometric_sprites"), args);
    EXPECT_FALSE(isoIsError(result)) << isoResultText(result).toStdString();
    EXPECT_TRUE(QFile::exists(out));

    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 32);
    EXPECT_EQ(img.height(), 64);
    EXPECT_TRUE(imageHasNonBackgroundPixels(img))
        << "sprite sheet should contain mesh pixels, not a flat background";

    EXPECT_EQ(result["directions"].toInt(), 2);
    EXPECT_EQ(result["frames"].toInt(), 1);
    EXPECT_EQ(result["resolution"].toInt(), 32);
    EXPECT_FALSE(result["directionOrder"].toString().isEmpty());
}

TEST_F(MCPServerGenerateIsometricSpritesCoverageTest, AppearsInToolList)
{
    const QJsonArray tools = server->buildToolsList();
    bool found = false;
    for (const auto &entry : tools) {
        if (entry.toObject()["name"].toString() == QStringLiteral("generate_isometric_sprites")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "generate_isometric_sprites must be exposed in tools/list";
}

} // namespace
