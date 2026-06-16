// Coverage tests for CLIPipeline::cmdAnim exercising the Ogre-native .mesh
// skeleton import path (robot.mesh + robot.skeleton).
//
// Every existing cmdAnim --list / --analyze success test in CLIPipeline_test.cpp
// uses "Twist Dance.fbx", which loads through the Assimp import branch and an
// anim-only skeleton. None of them cover:
//   * the .mesh import branch where MeshImporterExporter::importer reads
//     robot.skeleton and creates a real Ogre::Entity,
//   * the skel-from-entity->getMesh()->getSkeleton() path (lines ~1798-1802),
//   * --list / --analyze (text + JSON) over a native .mesh skeleton
//     (lines ~1832-1841 and ~1856-1884),
//   * --rename on a .mesh that takes the non-anim-only export branch
//     (lines ~2310-2320: entity->refreshAvailableAnimationState() +
//      MeshImporterExporter::exporter on the parent SceneNode).
//
// Distinct filename + distinct suite name (CLIPipeline_cmdAnimMeshPathCoverageTest)
// from CLIPipeline_test.cpp's CLIPipelineCmdAnim* suites so there is no ODR clash
// or duplicate registration.
//
// These tests require Ogre + a GL context (CI provides this via Xvfb). SetUp does
// ASSERT_TRUE(tryInitOgre()) + createStandardOgreMaterials(), per repo convention.
// No GTEST_SKIP — the suite must pass with all tests run.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include <Ogre.h>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

namespace {

// RAII argc/argv builder. Anonymous-namespace local so it does not collide with
// the TestArgv / VatArgv copies in sibling translation units.
class AnimArgv {
public:
    AnimArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args)
            m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

// Destroy every scene node + attached movable object so a prior import's robot
// entity does not leak into the next cmdAnim invocation (cmdAnim reads
// Manager::getEntities().first()).
void clearScene()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

// Import a .mesh once and return the name of its first animation, then clear the
// scene so the subsequent cmdAnim call starts from a clean state. Returns empty
// on failure (no entity / no skeleton / no animations).
QString discoverFirstAnimationName(const QString& meshAbsPath)
{
    MeshImporterExporter::importer({meshAbsPath});
    QString result;
    auto& entities = Manager::getSingleton()->getEntities();
    if (!entities.isEmpty()) {
        Ogre::Entity* ent = entities.first();
        if (ent->hasSkeleton()) {
            Ogre::SkeletonPtr skel = ent->getMesh()->getSkeleton();
            if (skel && skel->getNumAnimations() > 0)
                result = QString::fromStdString(skel->getAnimation(0)->getName());
        }
    }
    clearScene();
    return result;
}

// Copy robot.mesh + sibling robot.skeleton into `dir` so Ogre can resolve the
// skeleton link, mirroring the CLIPipelineCmdLodTest fixture setup. Returns the
// absolute path of the copied .mesh, or empty on failure.
QString stageRobotMesh(const QString& dir)
{
    const QString srcMesh = testRobotMeshPath();
    if (srcMesh.isEmpty() || !QFile::exists(srcMesh))
        return {};

    const QString dstMesh = QDir(dir).filePath("robot.mesh");
    QFile::remove(dstMesh);
    if (!QFile::copy(srcMesh, dstMesh))
        return {};

    const QString srcSkel = QFileInfo(srcMesh).absolutePath() + "/robot.skeleton";
    if (QFile::exists(srcSkel)) {
        const QString dstSkel = QDir(dir).filePath("robot.skeleton");
        QFile::remove(dstSkel);
        QFile::copy(srcSkel, dstSkel);
    }
    return QFileInfo(dstMesh).absoluteFilePath();
}

class CLIPipeline_cmdAnimMeshPathCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        CLIPipeline::initOgreHeadless();
        clearScene();
    }

    void TearDown() override
    {
        clearScene();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// --list (text) on robot.mesh: exercises the .mesh import branch +
// entity->getMesh()->getSkeleton() path + text listing (lines ~1842-1851).
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, ListTextOnNativeMeshSucceeds)
{
    const QString mesh = testRobotMeshPath();
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture not found";

    // Confirm the fixture really has at least one animation through the
    // .mesh skeleton path (this is what the cmdAnim list branch walks).
    const QString firstAnim = discoverFirstAnimationName(mesh);
    EXPECT_FALSE(firstAnim.isEmpty())
        << "robot.mesh should expose at least one skeletal animation";

    const QByteArray meshBa = mesh.toUtf8();
    AnimArgv args({"qtmesh", "anim", meshBa.constData(), "--list"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --list --json on robot.mesh: JSON listing branch (lines ~1832-1841).
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, ListJsonOnNativeMeshSucceeds)
{
    const QString mesh = testRobotMeshPath();
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture not found";

    const QByteArray meshBa = mesh.toUtf8();
    AnimArgv args({"qtmesh", "anim", meshBa.constData(), "--list", "--json"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --analyze (text) on robot.mesh: skeleton-structure analyze block
// (lines ~1871-1883) over a native .mesh skeleton.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, AnalyzeTextOnNativeMeshSucceeds)
{
    const QString mesh = testRobotMeshPath();
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture not found";

    const QByteArray meshBa = mesh.toUtf8();
    AnimArgv args({"qtmesh", "anim", meshBa.constData(), "--analyze"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --analyze --json on robot.mesh: JSON structure branch (lines ~1857-1870).
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, AnalyzeJsonOnNativeMeshSucceeds)
{
    const QString mesh = testRobotMeshPath();
    ASSERT_FALSE(mesh.isEmpty()) << "robot.mesh fixture not found";

    const QByteArray meshBa = mesh.toUtf8();
    AnimArgv args({"qtmesh", "anim", meshBa.constData(), "--analyze", "--json"});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --rename valid on a staged copy of robot.mesh, exported to a temp .mesh.
// Exercises renameAnimation + the non-anim-only export branch (lines
// ~2310-2320). Then reimport the output and assert the new name is present and
// the old name absent.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, RenameOnNativeMeshRoundTrips)
{
    const QString srcMesh = testRobotMeshPath();
    ASSERT_FALSE(srcMesh.isEmpty()) << "robot.mesh fixture not found";

    // Discover the first animation name from the original fixture (this also
    // leaves the scene clean for the cmdAnim call below).
    const QString oldName = discoverFirstAnimationName(srcMesh);
    ASSERT_FALSE(oldName.isEmpty())
        << "robot.mesh must have an animation to rename";

    const QString newName = oldName + "_renamed_cov";

    // Stage robot.mesh (+ sibling skeleton) into a temp dir so the in-place
    // overwrite does not mutate the committed fixture.
    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString stagedMesh = stageRobotMesh(sourceDir.path());
    ASSERT_FALSE(stagedMesh.isEmpty()) << "failed to stage robot.mesh";

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outMesh = QDir(outDir.path()).filePath("robot_renamed.mesh");
    QFile::remove(outMesh);

    clearScene();

    const QByteArray stagedBa = stagedMesh.toUtf8();
    const QByteArray oldBa    = oldName.toUtf8();
    const QByteArray newBa    = newName.toUtf8();
    const QByteArray outBa    = outMesh.toUtf8();

    AnimArgv args({"qtmesh", "anim", stagedBa.constData(),
                   "--rename", oldBa.constData(), newBa.constData(),
                   "-o", outBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdAnim(args.argc(), args.argv()));

    // The export branch should have written the output .mesh.
    EXPECT_TRUE(QFile::exists(outMesh))
        << "rename should have exported: " << outMesh.toStdString();

    // Reimport the renamed output and assert the rename took effect.
    clearScene();
    // Unload any cached mesh of the same logical name so reimport reads fresh.
    MeshImporterExporter::importer({QFileInfo(outMesh).absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    ASSERT_FALSE(entities.isEmpty()) << "reimport of renamed mesh produced no entity";
    Ogre::Entity* ent = entities.first();
    ASSERT_TRUE(ent->hasSkeleton());
    Ogre::SkeletonPtr skel = ent->getMesh()->getSkeleton();
    ASSERT_TRUE(static_cast<bool>(skel));

    EXPECT_TRUE(skel->hasAnimation(newName.toStdString()))
        << "renamed animation '" << newName.toStdString() << "' should be present";
    EXPECT_FALSE(skel->hasAnimation(oldName.toStdString()))
        << "old animation name '" << oldName.toStdString() << "' should be gone";

    QFile::remove(outMesh);
    QFile::remove(QDir(outDir.path()).filePath("robot_renamed.skeleton"));
}

// ---------------------------------------------------------------------------
// --rename with a non-existent source animation on robot.mesh: hits the
// "Animation not found" error branch (lines ~2287-2293) before any export.
// ---------------------------------------------------------------------------
TEST_F(CLIPipeline_cmdAnimMeshPathCoverageTest, RenameUnknownAnimationReturns1)
{
    const QString srcMesh = testRobotMeshPath();
    ASSERT_FALSE(srcMesh.isEmpty()) << "robot.mesh fixture not found";

    QTemporaryDir sourceDir;
    ASSERT_TRUE(sourceDir.isValid());
    const QString stagedMesh = stageRobotMesh(sourceDir.path());
    ASSERT_FALSE(stagedMesh.isEmpty());

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString outMesh = QDir(outDir.path()).filePath("robot_unknown_rename.mesh");
    QFile::remove(outMesh);

    clearScene();

    const QByteArray stagedBa = stagedMesh.toUtf8();
    const QByteArray outBa    = outMesh.toUtf8();

    AnimArgv args({"qtmesh", "anim", stagedBa.constData(),
                   "--rename", "__no_such_animation__", "whatever",
                   "-o", outBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdAnim(args.argc(), args.argv()));

    // Error branch returns before exporting, so no output file should exist.
    EXPECT_FALSE(QFile::exists(outMesh));

    QFile::remove(outMesh);
}
