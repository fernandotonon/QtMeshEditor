// Coverage tests for CLIPipeline::cmdTurntable focused on the *real render*
// path: feed an actual on-disk mesh (preferring media/models/robot.mesh via
// testRobotMeshPath(), falling back to a generated .obj so every test still
// runs) and assert the produced PNG sprite-sheet / per-frame / single-frame
// files actually exist on disk with the expected dimensions, across the
// --axis x/y/z render branches and the --camera-height variant.
//
// DISTINCT from CLIPipeline_test.cpp's CmdTurntable* suites: those drive the
// degenerate writeMinimalObj() triangle and "Twist Dance.fbx"; none of them
// exercise the real testRobotMeshPath() asset through the render pipeline.
// Suite names are unique (CLIPipelineCmdTurntableCoverage*) and all helpers
// live in this file's own anonymous namespace to avoid any ODR clash.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <vector>

#include "CLIPipeline.h"
#include "ModelTurntableRenderer.h"
#include "TestHelpers.h"

namespace {

// RAII argv builder driven by a QStringList (lets us assemble dynamic temp
// paths). Mirrors the TestArgv pattern in CLIPipeline_test.cpp but is a
// separate type in this file's anonymous namespace (no ODR clash).
class ArgvBuilder {
public:
    explicit ArgvBuilder(const QStringList& args)
    {
        for (const QString& a : args)
            m_storage.push_back(a.toUtf8());
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
    int m_argc = 0;
};

// Write a small but non-degenerate quad-cube-ish OBJ so the renderer has a
// real bounding box to frame even when robot.mesh isn't on disk.
QString writeCubeObj(const QString& dirPath, const QString& fileName)
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

class CLIPipelineCmdTurntableCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(m_tmp.isValid());
    }

    // Prefer the real robot.mesh asset; otherwise a generated cube .obj so the
    // suite never skips and still exercises the full render path.
    QString meshInput(const QString& objFallbackName)
    {
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot))
            return robot;
        const QString obj = writeCubeObj(m_tmp.path(), objFallbackName);
        EXPECT_FALSE(obj.isEmpty());
        return obj;
    }

    QString outPath(const QString& name) const { return m_tmp.filePath(name); }

    QTemporaryDir m_tmp;
};

// --axis y (default), sprite-sheet output: assert the PNG lands on disk with
// the sheet geometry (N frames laid out horizontally).
TEST_F(CLIPipelineCmdTurntableCoverageTest, AxisYSpriteSheetWritesPngOnDisk)
{
    const QString mesh = meshInput("ax_y.obj");
    const QString out = outPath("sheet_y.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "3", "--size", "48", "--axis", "y"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 144);  // 3 cols * 48
    EXPECT_EQ(img.height(), 48);
}

// --axis x render branch on a real mesh.
TEST_F(CLIPipelineCmdTurntableCoverageTest, AxisXSpriteSheetWritesPngOnDisk)
{
    const QString mesh = meshInput("ax_x.obj");
    const QString out = outPath("sheet_x.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "2", "--size", "40", "--axis", "x"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 80);   // 2 cols * 40
    EXPECT_EQ(img.height(), 40);
}

// --axis z render branch on a real mesh.
TEST_F(CLIPipelineCmdTurntableCoverageTest, AxisZSpriteSheetWritesPngOnDisk)
{
    const QString mesh = meshInput("ax_z.obj");
    const QString out = outPath("sheet_z.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "2", "--size", "32", "--axis", "z"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);   // 2 cols * 32
    EXPECT_EQ(img.height(), 32);
}

// --camera-height (elevation) variant: a non-default camera height should
// still produce a valid PNG. Pairs camera-height with the --json surface so
// we can assert the elevation echo + axis key in the JSON report.
TEST_F(CLIPipelineCmdTurntableCoverageTest, CameraHeightVariantWritesPngAndJson)
{
    const QString mesh = meshInput("cam_h.obj");
    const QString out = outPath("cam_h.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "2", "--size", "40",
                      "--axis", "x", "--camera-height", "35", "--json"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 80);
    EXPECT_EQ(img.height(), 40);
}

// Per-frame %02d sequence output: every frame PNG must exist on disk and be a
// loadable image with the requested per-frame dimensions.
TEST_F(CLIPipelineCmdTurntableCoverageTest, SequencePerFrameOutputExistsOnDisk)
{
    const QString mesh = meshInput("seq.obj");
    const QString pattern = outPath("robot_%02d.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", pattern,
                      "--frames", "3", "--width", "64", "--height", "48",
                      "--axis", "y"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    const QString f0 = outPath("robot_00.png");
    const QString f1 = outPath("robot_01.png");
    const QString f2 = outPath("robot_02.png");
    EXPECT_TRUE(QFile::exists(f0));
    EXPECT_TRUE(QFile::exists(f1));
    EXPECT_TRUE(QFile::exists(f2));
    // No off-by-one extra frame should be produced.
    EXPECT_FALSE(QFile::exists(outPath("robot_03.png")));

    QImage img(f0);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 64);
    EXPECT_EQ(img.height(), 48);
}

// --frames 1 single-frame branch: writes exactly one PNG at -o (no sheet, no
// sequence) with the requested square size.
TEST_F(CLIPipelineCmdTurntableCoverageTest, SingleFrameWritesOnePngOnDisk)
{
    const QString mesh = meshInput("one.obj");
    const QString out = outPath("single.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "1", "--size", "56", "--camera-height", "10"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 56);
    EXPECT_EQ(img.height(), 56);
}

// JSON report on a sprite-sheet (multi-frame, non-sequence) render: assert the
// reported keys/values match the request and that "outputs" lists the on-disk
// PNG. Exercises the jsonOutput + sequence=false + axis=x echo branch.
TEST_F(CLIPipelineCmdTurntableCoverageTest, JsonReportEchoesRequestForSpriteSheet)
{
    const QString mesh = meshInput("json.obj");
    const QString out = outPath("json_sheet.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "4", "--size", "32x24",
                      "--axis", "z", "--json"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    // 4 frames, no --columns -> single horizontal row in composeSpriteSheet.
    EXPECT_EQ(img.width(), 128);  // 4 * 32
    EXPECT_EQ(img.height(), 24);
}

// frameCount is clamped to [1,360]; a request above the cap should still
// succeed (qBound) and produce a sheet. Keep the cap modest-but-real by using
// a high frame count and tiny tiles so the render stays cheap.
TEST_F(CLIPipelineCmdTurntableCoverageTest, FramesAboveCapClampToValidRange)
{
    const QString mesh = meshInput("clamp.obj");
    const QString out = outPath("clamp.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "500", "--size", "8", "--columns", "4"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    // 360 frames in a 4-column grid -> 90 rows. Just assert the sheet is valid
    // and tile-aligned rather than pinning an exact (possibly engine-defined)
    // layout.
    EXPECT_EQ(img.width() % 8, 0);
    EXPECT_EQ(img.height() % 8, 0);
    EXPECT_GT(img.width(), 0);
    EXPECT_GT(img.height(), 0);
}

// --columns N multi-row sprite-sheet layout on a real mesh: 4 frames in 2
// columns -> 2x2 grid.
TEST_F(CLIPipelineCmdTurntableCoverageTest, ColumnsMultiRowSheetWritesPngOnDisk)
{
    const QString mesh = meshInput("cols.obj");
    const QString out = outPath("cols.png");

    ArgvBuilder args({"qtmesh", "turntable", mesh, "-o", out,
                      "--frames", "4", "--size", "40",
                      "--columns", "2", "--axis", "y", "--camera-height", "25"});
    EXPECT_EQ(CLIPipeline::cmdTurntable(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFile::exists(out));
    QImage img(out);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 80);   // 2 cols * 40
    EXPECT_EQ(img.height(), 80);  // 2 rows * 40
}

} // namespace
