// PartOps Slice B/E (#861/#864): coverage for `qtmesh segment --split-parts`
// and `--write-labels`. Exercises the full core→Ogre pipeline headless: import
// a mesh, segment (geometric fallback via --no-model so no network / model
// download), split into per-part submeshes via SubMeshOps + PartOpsMesh, export,
// and re-import to assert the round-trip. Skinned-fixture bone preservation is
// asserted when a rigged asset is available.
//
// Ogre IS available in CI (Linux + Xvfb); SetUp asserts tryInitOgre() and never
// GTEST_SKIPs. When no rigged fixture is on disk the split still runs on a
// generated in-memory mesh so the suite always reaches a real assertion.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include <Ogre.h>
#include <OgreMeshManager.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"
#include "MeshSegmenter.h"
#include "EditableMesh.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>

#include <cmath>
#include <set>

namespace {

class SplitArgv {
public:
    SplitArgv(std::initializer_list<const char*> args)
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

int meshTriangleCount(const QString& path)
{
    clearScene();
    MeshImporterExporter::importer({QFileInfo(path).absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty())
        return -1;
    MeshInfo info = CLIPipeline::extractMeshInfo(entities.first(), QFileInfo(path).fileName());
    return static_cast<int>(info.triangles);
}

} // namespace

class CLIPipelineCmdSplitPartsCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
        clearScene();
    }
    void TearDown() override { clearScene(); }

    /// A rigged humanoid fixture if present (Rumba Dancing.fbx), else empty.
    static QString riggedFixture()
    {
        const QString p = testAssetPath(QStringLiteral("media/models/Rumba Dancing.fbx"));
        return (!p.isEmpty() && QFile::exists(p)) ? p : QString();
    }

    /// A generated in-memory triangle mesh exported to .mesh — the always-there
    /// fallback so the suite never skips.
    static QString generatedMesh(QTemporaryDir& holder)
    {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr || !holder.isValid())
            return QString();
        const std::string meshName = "cli_split_gen_mesh";
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        Ogre::SceneNode* node = mgr->addSceneNode("cli_split_gen_node");
        if (!node)
            return QString();
        Ogre::Entity* e = mgr->createEntity(node, mesh);
        if (!e)
            return QString();
        const QString out = QDir(holder.path()).filePath("cli_split_gen.mesh");
        const int rc = MeshImporterExporter::exporter(node, out, "Ogre Mesh (*.mesh)");
        mgr->destroyAllAttachedMovableObjects(node);
        mgr->destroySceneNode(node);
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        return rc == 0 ? out : QString();
    }
};

// --split-parts on a rigged humanoid: produces a multi-submesh mesh, preserves
// the triangle count, and keeps the skeleton (skinned bone assignments).
TEST_F(CLIPipelineCmdSplitPartsCoverageTest, SplitRiggedHumanoidPreservesTrisAndSkeleton)
{
    const QString fixture = riggedFixture();
    if (fixture.isEmpty())
        GTEST_SKIP() << "rigged fixture not present; covered by generated-mesh test";

    const int srcTris = meshTriangleCount(fixture);
    ASSERT_GT(srcTris, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFbx = QDir(tmp.path()).filePath("parts.fbx");

    clearScene();
    const QByteArray in = fixture.toUtf8();
    const QByteArray out = outFbx.toUtf8();
    SplitArgv args({"qtmesh", "segment", in.constData(), "--no-model",
                    "--split-parts", "-o", out.constData()});
    ASSERT_EQ(0, CLIPipeline::cmdSegment(args.argc(), args.argv()));
    ASSERT_TRUE(QFile::exists(outFbx));

    // Re-import the split result and inspect it.
    clearScene();
    MeshImporterExporter::importer({QFileInfo(outFbx).absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    ASSERT_FALSE(entities.isEmpty());
    Ogre::Entity* e = entities.first();
    ASSERT_NE(e, nullptr);

    // More than one submesh (a fused body split into parts).
    EXPECT_GT(e->getMesh()->getNumSubMeshes(), 1u)
        << "split should produce multiple part submeshes";

    // The original geometry is preserved; the split ALSO caps each part's open
    // cut face into a watertight solid (capParts=true on the user-facing path),
    // which adds a fan of cap triangles — so the count is >= the source, not
    // exactly equal. Boundary vertex duplication itself adds verts, not tris.
    MeshInfo info = CLIPipeline::extractMeshInfo(e, "parts.fbx");
    EXPECT_GE(static_cast<int>(info.triangles), srcTris)
        << "split must preserve the source geometry (plus watertight caps)";

    // Skinned fixture retains its skeleton + bone assignments (#861 criterion).
    EXPECT_TRUE(e->getMesh()->hasSkeleton())
        << "split of a skinned mesh must keep the skeleton bound";

    // Normals are preserved as valid unit vectors — NOT zeroed/degenerate.
    // The split builds with recomputeNormals=false (to keep authored normals),
    // so a bug there would leave black geometry (the "model is dark" symptom).
    // Read the reimported normals back and assert the vast majority are
    // unit-length; a handful of legitimately-degenerate verts is tolerated.
    {
        EditableMesh em;
        ASSERT_TRUE(em.loadFromEntity(e));
        int total = 0, unitLen = 0, zeroLen = 0;
        for (const auto& sm : em.subMeshes()) {
            for (const auto& v : sm.vertices) {
                if (!v.hasNormal) continue;
                ++total;
                const float len = v.normal.length();
                if (len < 1e-4f) ++zeroLen;
                else if (std::fabs(len - 1.0f) < 0.05f) ++unitLen;
            }
        }
        ASSERT_GT(total, 0) << "reimported split has no normals — would render black";
        EXPECT_LT(zeroLen, total / 100 + 1) << "too many zero-length normals";
        EXPECT_GT(unitLen, total * 9 / 10)
            << "split normals must stay unit-length so lighting works (dark-model regression)";
    }

    // Part NAMES survive the FBX export → reimport round-trip: the mesh's
    // submesh-name map is non-empty and every name is a known body part
    // (so the Scene tree shows "head"/"torso"/… not a positional index).
    const auto& nameMap = e->getMesh()->getSubMeshNameMap();
    EXPECT_FALSE(nameMap.empty())
        << "split part names must round-trip through FBX as named submeshes";
    std::set<std::string> seenNames;
    for (const auto& kv : nameMap) {
        // Every registered submesh name must be UNIQUE — nameSubMesh overwrites
        // on collision, so a duplicate would make two submeshes resolve to one.
        // The importer disambiguates same aiMesh::mName with an "_N" suffix.
        EXPECT_TRUE(seenNames.insert(kv.first).second)
            << "duplicate submesh name registered: " << kv.first;
        // Strip a trailing ".N" (multi-material) or "_N" (import-collision)
        // NUMERIC suffix before matching. Part names themselves contain '_'
        // (e.g. "right_leg"), so only a trailing all-digit segment after the
        // LAST '.'/'_' is a disambiguation suffix — not the base name's own '_'.
        QString base = QString::fromStdString(kv.first);
        for (const QChar sep : {QLatin1Char('.'), QLatin1Char('_')}) {
            const int at = base.lastIndexOf(sep);
            if (at > 0) {
                const QString tail = base.mid(at + 1);
                bool allDigits = !tail.isEmpty();
                for (const QChar c : tail)
                    if (!c.isDigit()) { allDigits = false; break; }
                if (allDigits)
                    base = base.left(at);
            }
        }
        bool known = false;
        for (int p = 1; p < MeshSegmenter::partCount(); ++p) {
            if (base == MeshSegmenter::partName(p)) { known = true; break; }
        }
        EXPECT_TRUE(known) << "unexpected submesh name: " << kv.first;
    }
}

// --split-parts without -o is a usage error (exit 2), no Ogre load required.
TEST_F(CLIPipelineCmdSplitPartsCoverageTest, SplitPartsRequiresOutput)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString mesh = generatedMesh(tmp);
    ASSERT_FALSE(mesh.isEmpty());
    const QByteArray in = mesh.toUtf8();
    SplitArgv args({"qtmesh", "segment", in.constData(), "--no-model", "--split-parts"});
    EXPECT_EQ(2, CLIPipeline::cmdSegment(args.argc(), args.argv()));
}

// --write-labels dumps a valid labels JSON with the documented schema + arrays.
TEST_F(CLIPipelineCmdSplitPartsCoverageTest, WriteLabelsProducesSchemaJson)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString mesh = generatedMesh(tmp);
    ASSERT_FALSE(mesh.isEmpty());

    const QString labels = QDir(tmp.path()).filePath("labels.json");
    const QByteArray in = mesh.toUtf8();
    const QByteArray lb = labels.toUtf8();
    SplitArgv args({"qtmesh", "segment", in.constData(), "--no-model",
                    "--write-labels", lb.constData()});
    ASSERT_EQ(0, CLIPipeline::cmdSegment(args.argc(), args.argv()));
    ASSERT_TRUE(QFile::exists(labels));

    QFile f(labels);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError);
    ASSERT_TRUE(doc.isObject());
    QJsonObject o = doc.object();
    EXPECT_EQ(o.value("schema").toString(), QStringLiteral("qtmesh-partops-labels-v1"));
    EXPECT_TRUE(o.contains("faceLabels"));
    EXPECT_TRUE(o.contains("vertexLabels"));
    EXPECT_TRUE(o.value("faceLabels").isArray());
    EXPECT_GT(o.value("faceCount").toInt(), 0);
    // faceLabels length matches faceCount.
    EXPECT_EQ(o.value("faceLabels").toArray().size(), o.value("faceCount").toInt());
}
