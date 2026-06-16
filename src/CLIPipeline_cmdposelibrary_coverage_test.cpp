// Coverage tests for CLIPipeline::cmdPose --library apply mode.
//
// The "--library apply" branch (CLIPipeline.cpp lines ~2731-2832) is entirely
// untested at the CLI level. PoseLibrary_test.cpp drives PoseLibrary directly
// (savePose / loadPoseLibrary / applyPose) but NEVER through cmdPose. This
// suite exercises the cmdPose argv path end-to-end:
//
//   * every required-flag validation branch -> exit 2
//     (missing mesh, empty --lib, empty --apply, empty -o)
//   * mesh-file-not-found            -> exit 1
//   * lib-file-not-found             -> exit 1
//   * full happy path: import an animated skinned mesh, write a real
//     .poselib sidecar via PoseLibrary::savePoseLibrary, then run cmdPose to
//     load + apply the named pose and export the posed mesh -> exit 0, output
//     file exists on disk
//   * pose-name not present in the loaded library -> exit 1 (lists poses)
//
// DISTINCT filename + DISTINCT suite name (CLIPipelineCmdPoseLibraryCoverage)
// and a private anonymous namespace so there is no ODR clash / duplicate
// registration with any prior cmdPose coverage. Auto-registered by the
// src/*_test.cpp CMake glob — no CMake edit needed.
//
// NEVER GTEST_SKIP: the validation branches need no Ogre at all (they return
// before initOgreHeadless), and the Ogre-backed cases assert ASSERT_TRUE on
// tryInitOgre() so a broken CI env fails the suite rather than skipping it.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <initializer_list>
#include <vector>

#include <OgreEntity.h>
#include <OgreMovableObject.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "PoseLibrary.h"
#include "TestHelpers.h"

namespace {

// RAII argv builder driven by a QStringList so we can splice in dynamic temp
// paths. Mirrors the TestArgv pattern from the sibling coverage tests but is a
// distinct type in this file's anonymous namespace (no cross-TU ODR clash).
class PoseLibArgv {
public:
    explicit PoseLibArgv(const QStringList& args)
    {
        for (const QString& a : args)
            m_storage.push_back(a.toUtf8());
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    std::vector<QByteArray> m_storage;
    std::vector<char*> m_argv;
    int m_argc = 0;
};

// media/models resolved relative to the test binary (bin -> build -> root).
QString modelsDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

QString twistDanceFbx() { return modelsDir() + "/Twist Dance.fbx"; }

// First skinned Ogre::Entity currently attached in the scene, or nullptr.
Ogre::Entity* firstSkinnedEntity()
{
    if (!Manager::getSingletonPtr())
        return nullptr;
    auto& movables = Manager::getSingleton()->getEntities();
    for (auto* obj : movables) {
        if (!obj || obj->getMovableType() != "Entity")
            continue;
        auto* e = static_cast<Ogre::Entity*>(obj);
        if (e->hasSkeleton())
            return e;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture: real Ogre init (CI provides Xvfb/GL). No GTEST_SKIP — a broken env
// fails via ASSERT_TRUE. clearScene() between tests so each case starts clean.
// ---------------------------------------------------------------------------
class CLIPipelineCmdPoseLibraryCoverage : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
        ASSERT_TRUE(m_tmp.isValid());
        clearScene();
        if (PoseLibrary::instance())
            PoseLibrary::instance()->clearAll();
    }

    void TearDown() override
    {
        if (PoseLibrary::instance())
            PoseLibrary::instance()->clearAll();
        clearScene();
    }

    static void clearScene()
    {
        if (!Manager::getSingletonPtr())
            return;
        const auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }

    QString tmpPath(const QString& name) const { return m_tmp.filePath(name); }

    QTemporaryDir m_tmp;
};

// ===========================================================================
// Required-flag validation branches — these all return BEFORE any Ogre import,
// so they exercise the pure argument-parsing path. Each returns exit code 2.
// ===========================================================================

// No positional mesh (filePath empty) -> usage error -> 2.
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyMissingMeshReturns2)
{
    PoseLibArgv args({"qtmesh", "pose", "--library", "apply",
                      "--lib", "lib.poselib", "--apply", "p1",
                      "-o", "out.obj"});
    EXPECT_EQ(2, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// Mesh given but --lib empty -> 2.
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyEmptyLibReturns2)
{
    PoseLibArgv args({"qtmesh", "pose", "model.fbx", "--library", "apply",
                      "--lib", "", "--apply", "p1", "-o", "out.obj"});
    EXPECT_EQ(2, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// Mesh + --lib given but --apply empty -> 2.
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyEmptyApplyNameReturns2)
{
    PoseLibArgv args({"qtmesh", "pose", "model.fbx", "--library", "apply",
                      "--lib", "lib.poselib", "--apply", "", "-o", "out.obj"});
    EXPECT_EQ(2, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// Mesh + --lib + --apply given but -o empty -> 2.
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyEmptyOutputReturns2)
{
    PoseLibArgv args({"qtmesh", "pose", "model.fbx", "--library", "apply",
                      "--lib", "lib.poselib", "--apply", "p1", "-o", ""});
    EXPECT_EQ(2, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// ===========================================================================
// Filesystem-existence branches -> exit code 1.
// ===========================================================================

// Mesh path that does not exist on disk -> 1 (lib path is irrelevant; mesh is
// checked first).
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyMeshFileNotFoundReturns1)
{
    const QString lib = tmpPath("present.poselib");
    {   // a real, present library file so only the mesh trips the guard
        QFile f(lib);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{}");
        f.close();
    }
    const QString missingMesh = tmpPath("does_not_exist.fbx");
    ASSERT_FALSE(QFile::exists(missingMesh));

    PoseLibArgv args({"qtmesh", "pose", missingMesh, "--library", "apply",
                      "--lib", lib, "--apply", "p1", "-o", tmpPath("out.obj")});
    EXPECT_EQ(1, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// Mesh exists, but the --lib library file does not -> 1.
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyLibFileNotFoundReturns1)
{
    // A present mesh file (content need not be a real mesh — the existence
    // check fires before the import because lib is checked too; here we make
    // the mesh exist and the lib NOT exist so the lib guard is the one hit).
    const QString mesh = tmpPath("present_mesh.obj");
    {
        QFile f(mesh);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("o Tri\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        f.close();
    }
    const QString missingLib = tmpPath("nope.poselib");
    ASSERT_FALSE(QFile::exists(missingLib));

    PoseLibArgv args({"qtmesh", "pose", mesh, "--library", "apply",
                      "--lib", missingLib, "--apply", "p1",
                      "-o", tmpPath("out.obj")});
    EXPECT_EQ(1, CLIPipeline::cmdPose(args.argc(), args.argv()));
}

// ===========================================================================
// Happy path: build a real .poselib from a real skinned asset, then drive the
// whole load -> apply -> export pipeline through cmdPose -> exit 0.
// ===========================================================================
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyFullHappyPathExportsPosedMesh)
{
    const QString fbx = twistDanceFbx();
    if (!QFile::exists(fbx))
        GTEST_FAIL() << "Twist Dance.fbx missing from media/models — required asset";
    ASSERT_TRUE(canLoadMeshFiles());

    // 1. Import the source asset to get a live skinned entity, capture a pose.
    MeshImporterExporter::importer({fbx});
    Ogre::Entity* entity = firstSkinnedEntity();
    ASSERT_NE(entity, nullptr) << "Twist Dance.fbx should yield a skinned entity";

    auto* lib = PoseLibrary::instance();
    ASSERT_NE(lib, nullptr);
    ASSERT_TRUE(lib->savePose(entity, "p1"))
        << "savePose should capture the bind/current pose";

    // 2. Persist the .poselib sidecar (matching bone names baked in).
    const QString libPath = tmpPath("twist.poselib");
    ASSERT_TRUE(lib->savePoseLibrary(entity, libPath));
    ASSERT_TRUE(QFile::exists(libPath));

    // 3. Drop everything cmdPose itself re-imports the asset fresh.
    clearScene();
    lib->clearAll();

    // 4. Run cmdPose --library apply: it re-imports, loads the sidecar onto the
    //    freshly imported entity (bone names match — same asset), applies "p1",
    //    and exports the posed mesh.
    const QString out = tmpPath("posed.obj");
    PoseLibArgv args({"qtmesh", "pose", fbx, "--library", "apply",
                      "--lib", libPath, "--apply", "p1", "-o", out});
    EXPECT_EQ(0, CLIPipeline::cmdPose(args.argc(), args.argv()));
    EXPECT_TRUE(QFile::exists(out)) << "posed mesh export should land on disk";
}

// ===========================================================================
// Library loads fine but the requested pose name isn't in it -> 1, and the
// "Available poses:" listing branch is exercised.
// ===========================================================================
TEST_F(CLIPipelineCmdPoseLibraryCoverage, ApplyPoseNameNotInLibraryReturns1)
{
    const QString fbx = twistDanceFbx();
    if (!QFile::exists(fbx))
        GTEST_FAIL() << "Twist Dance.fbx missing from media/models — required asset";
    ASSERT_TRUE(canLoadMeshFiles());

    // Build a real .poselib that contains only "p1".
    MeshImporterExporter::importer({fbx});
    Ogre::Entity* entity = firstSkinnedEntity();
    ASSERT_NE(entity, nullptr);

    auto* lib = PoseLibrary::instance();
    ASSERT_NE(lib, nullptr);
    ASSERT_TRUE(lib->savePose(entity, "p1"));
    const QString libPath = tmpPath("twist_one.poselib");
    ASSERT_TRUE(lib->savePoseLibrary(entity, libPath));
    ASSERT_TRUE(QFile::exists(libPath));

    clearScene();
    lib->clearAll();

    // Ask for a pose name that is NOT in the library -> 1 (lists "p1").
    const QString out = tmpPath("nope.obj");
    PoseLibArgv args({"qtmesh", "pose", fbx, "--library", "apply",
                      "--lib", libPath, "--apply", "missing_pose", "-o", out});
    EXPECT_EQ(1, CLIPipeline::cmdPose(args.argc(), args.argv()));
    EXPECT_FALSE(QFile::exists(out)) << "no export should happen on missing pose";
}
