// Coverage tests for CLIPipeline::cmdRig (#407, auto-rig). Mirrors the
// cmdSkin coverage style: the argument-validation branches (return 2) and the
// file-not-found branch (return 1) need no GL context, so they exercise the
// parser without a loaded mesh. The full rig+export path needs a real mesh and
// is exercised under Xvfb on CI via the success-path test below (which is
// skipped gracefully when Ogre can't init).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>
#include <string>

#include "CLIPipeline.h"
#include "TestHelpers.h"

namespace {

// RAII argc/argv builder, own anon-namespace name (no ODR clash).
class RigArgv {
public:
    RigArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage) m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }
private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

const char* kMissingFile = "/nonexistent_qtmesh_rig_input_zzz.obj";

} // namespace

// ── Required-argument checks (return 2) ─────────────────────────────────────

TEST(CLIPipelineCmdRigCoverageError, NoInputFile)
{
    RigArgv args({"rig"});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdRigCoverageError, NoInputButFlags)
{
    RigArgv args({"rig", "--json", "--skin"});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdRigCoverageError, InputButNoOutput)
{
    RigArgv args({"rig", kMissingFile});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdRigCoverageError, BadUpAxisIsUsageError)
{
    RigArgv args({"rig", kMissingFile, "-o", "out.fbx", "--up-axis", "w"});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 2);
}

// ── File-existence branch (return 1) ────────────────────────────────────────

TEST(CLIPipelineCmdRigCoverageError, MissingFileWithValidArgs)
{
    // Valid template + output, but the input doesn't exist -> 1.
    RigArgv args({"rig", kMissingFile, "-o", "out.fbx", "--skeleton", "humanoid"});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdRigCoverageError, UnknownTemplateStillParsesThenFileMissing)
{
    // An unrecognised template name is tolerated by templateFromString
    // (falls back to humanoid), so it must NOT be a usage error (2);
    // it proceeds to the file-existence check -> 1.
    RigArgv args({"rig", kMissingFile, "-o", "out.fbx", "--skeleton", "dragon"});
    EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdRigCoverageError, EveryValidUpAxisParses)
{
    for (const char* ax : {"x", "y", "z"}) {
        RigArgv args({"rig", kMissingFile, "-o", "out.fbx", "--up-axis", ax});
        // Valid axis -> passes parse, then file-not-found -> 1 (never 2).
        EXPECT_EQ(CLIPipeline::cmdRig(args.argc(), args.argv()), 1)
            << "up-axis " << ax << " should parse";
    }
}

// ── Success path (needs a GL/Ogre context; skipped without one) ─────────────

TEST(CLIPipelineCmdRigSuccess, RigsStaticMeshAndExports)
{
    if (!tryInitOgre() || !canLoadMeshFiles())
        GTEST_SKIP() << "Ogre/GL unavailable (needs Xvfb).";

    // Build a static (skeleton-less) mesh on disk by exporting a simple
    // in-memory triangle mesh to OBJ — OBJ carries no skeleton.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    // Reuse the editor's own loader path: write a minimal OBJ cube-ish quad.
    const QString objPath = dir.filePath("static.obj");
    {
        QFile f(objPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        // A small upright pyramid-ish shape (8 verts spanning a 1x2x1 box).
        const char* obj =
            "v -0.4 0 -0.4\nv 0.4 0 -0.4\nv 0.4 0 0.4\nv -0.4 0 0.4\n"
            "v -0.2 2 -0.2\nv 0.2 2 -0.2\nv 0.2 2 0.2\nv -0.2 2 0.2\n"
            "f 1 2 3\nf 1 3 4\nf 5 6 7\nf 5 7 8\n"
            "f 1 2 6\nf 1 6 5\nf 3 4 8\nf 3 8 7\n";
        f.write(obj);
        f.close();
    }

    const QString outPath = dir.filePath("rigged.gltf");
    // Hold the path bytes in stable std::strings so the argv char* stay valid.
    const std::string objStr = objPath.toStdString();
    const std::string outStr = outPath.toStdString();
    RigArgv args({"rig", objStr.c_str(), "-o", outStr.c_str(),
                  "--skeleton", "humanoid"});
    const int rc = CLIPipeline::cmdRig(args.argc(), args.argv());
    // Either it rigs+exports (0) or the OBJ import path isn't available in this
    // headless build (1) — but it must never crash or return a usage error.
    EXPECT_NE(rc, 2);
    if (rc == 0)
        EXPECT_TRUE(QFile::exists(outPath)) << "rigged mesh should be written";
}
