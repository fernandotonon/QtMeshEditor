#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThread>

#include <OgreHardwareBufferManager.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include "Manager.h"
#include "PS1/PS1PLY.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

constexpr unsigned long kSingletonSettleMs = 30;

static void ensureBaseMaterialForPlyImport()
{
    if (Ogre::MaterialManager::getSingleton().getByName(
            "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME)) {
        return;
    }
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().create(
        "BaseMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    m->getTechnique(0)->getPass(0)->setDiffuse(1.0f, 1.0f, 1.0f, 1.0f);
    m->getTechnique(0)->getPass(0)->setAmbient(1.0f, 1.0f, 1.0f);
}

static Ogre::MeshPtr createInterleavedPosNormalMesh(const std::string& name,
                                                   const float (*vertexRows)[6],
                                                   int nVerts,
                                                   const uint16_t* indices,
                                                   int indexCount)
{
    if (auto old = Ogre::MeshManager::getSingleton().getByName(name))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    Ogre::SubMesh* sm = mesh->createSubMesh();
    sm->setMaterialName("BaseWhite");
    sm->useSharedVertices = false;

    Ogre::VertexData* vd = new Ogre::VertexData();
    sm->vertexData = vd;
    vd->vertexCount = static_cast<unsigned>(nVerts);
    Ogre::VertexDeclaration* decl = vd->vertexDeclaration;
    Ogre::VertexBufferBinding* bind = vd->vertexBufferBinding;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    const size_t vsize = decl->getVertexSize(0);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        vsize, static_cast<size_t>(nVerts), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint8_t* dst = static_cast<uint8_t*>(vbuf->lock(Ogre::HardwareBuffer::HBL_DISCARD));
    for (int i = 0; i < nVerts; ++i) {
        uint8_t* row = dst + static_cast<size_t>(i) * vsize;
        float* pf = nullptr;
        decl->findElementBySemantic(Ogre::VES_POSITION)->baseVertexPointerToElement(row, &pf);
        pf[0] = vertexRows[i][0];
        pf[1] = vertexRows[i][1];
        pf[2] = vertexRows[i][2];
        decl->findElementBySemantic(Ogre::VES_NORMAL)->baseVertexPointerToElement(row, &pf);
        pf[0] = vertexRows[i][3];
        pf[1] = vertexRows[i][4];
        pf[2] = vertexRows[i][5];
    }
    vbuf->unlock();
    bind->setBinding(0, vbuf);

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, static_cast<size_t>(indexCount),
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, static_cast<size_t>(indexCount) * sizeof(uint16_t), indices);
    sm->indexData->indexBuffer = ibuf;
    sm->indexData->indexCount = static_cast<unsigned>(indexCount);
    sm->indexData->indexStart = 0;

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, 1, 1, 0));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();
    return mesh;
}

/** Two triangles (0,1,2) and (1,2,3) — PS1 quad split; flat +Z normal. */
static Ogre::MeshPtr createTwoTriQuadMesh(const std::string& name)
{
    static const float corners[][6] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 1.f, 0.f, 0.f, 0.f, 1.f},
        {0.f, 1.f, 0.f, 0.f, 0.f, 1.f},
    };
    static const uint16_t idx[] = {0, 1, 2, 1, 2, 3};
    return createInterleavedPosNormalMesh(name, corners, 4, idx, 6);
}

/** Two coplanar tris sharing a geometric edge with different normals on that edge (6 verts). */
static Ogre::MeshPtr createSplitNormalTwoTriMesh(const std::string& name)
{
    static const float rows[][6] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 1.f, 0.f, 0.f, 0.f, 1.f},
        {1.f, 0.f, 0.f, 1.f, 0.f, 0.f},
        {1.f, 1.f, 0.f, 1.f, 0.f, 0.f},
        {0.f, 1.f, 0.f, 0.f, 0.f, 1.f},
    };
    static const uint16_t idx[] = {0, 1, 2, 3, 4, 5};
    return createInterleavedPosNormalMesh(name, rows, 6, idx, 6);
}

static bool readPsyqPlyCountsAndFirstFace(const QString& path, int& nV, int& nN, int& nF, QString& firstFaceLine)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream ts(&file);
    QStringList lines;
    while (!ts.atEnd())
        lines.append(ts.readLine().trimmed());

    int idx = 0;
    while (idx < lines.size() && !lines[idx].contains(QStringLiteral("@PLY"), Qt::CaseInsensitive))
        ++idx;
    if (idx >= lines.size())
        return false;
    ++idx;
    while (idx < lines.size()
           && (lines[idx].isEmpty() || lines[idx].startsWith(QLatin1Char('#'))))
        ++idx;
    if (idx >= lines.size())
        return false;

    const QStringList countParts = lines[idx].split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (countParts.size() < 3)
        return false;
    nV = countParts[0].toInt();
    nN = countParts[1].toInt();
    nF = countParts[2].toInt();
    ++idx;
    idx += nV + nN;
    while (idx < lines.size() && lines[idx].isEmpty())
        ++idx;
    if (idx >= lines.size())
        return false;
    firstFaceLine = lines[idx];
    return nV > 0 && nN > 0 && nF > 0;
}

} // namespace

TEST(PS1PLY, IsPsyqPlyFile_TrueWhenHeaderPresent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("m.ply"));
    const QByteArray data =
        "# comment\n"
        "@PLY940102\n"
        "3 4 1\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_TRUE(PS1PLY::isPsyqPlyFile(path));
}

// Some exporters (e.g. RSD toolchains) prefix with "#PLY Mesh Data" before @PLY940102.
TEST(PS1PLY, IsPsyqPlyFile_TrueWithPlyMeshDataCommentPrefix)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("planetish.ply"));
    const QByteArray data =
        "#PLY Mesh Data\n"
        "@PLY940102\n"
        "3 3 1\n"
        "0 0 0\n"
        "1 0 0\n"
        "0 1 0\n"
        "0 0 1\n"
        "0 0 1\n"
        "0 0 1\n"
        "0 0 2 1 0 0 2 1 0\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_TRUE(PS1PLY::isPsyqPlyFile(path));
}

TEST(PS1PLY, IsPsyqPlyFile_FalseForStanfordPly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("stanford.ply"));
    const QByteArray data = "ply\nformat ascii 1.0\n";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
    f.close();
    EXPECT_FALSE(PS1PLY::isPsyqPlyFile(path));
}

class PS1PLYOgreTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSingletonSettleMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed";
        createStandardOgreMaterials();
        ensureBaseMaterialForPlyImport();
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        SelectionSet::kill();
        Manager::kill();
        if (app)
            app->processEvents();
        QThread::msleep(kSingletonSettleMs);
    }
};

TEST_F(PS1PLYOgreTest, ExportHeuristicMergeProducesOneQuadAndSharedNormalPool)
{
    ASSERT_TRUE(canLoadMeshFiles());

    const std::string meshName = "PS1PlyQuadHeuristicMesh";
    Ogre::MeshPtr mesh = createTwoTriQuadMesh(meshName);
    ASSERT_TRUE(mesh);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode(QStringLiteral("PS1PlyQuadHeuristicNode"));
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outPly(QDir::tempPath() + QStringLiteral("/qtmesh_ps1ply_heur_XXXXXX.ply"));
    outPly.setAutoRemove(true);
    ASSERT_TRUE(outPly.open());
    outPly.close();
    const QString path = outPly.fileName();

    QString err;
    ASSERT_TRUE(PS1PLY::exportPsyqPlyFromEntity(ent, path, nullptr, &err)) << err.toUtf8().constData();

    mgr->destroySceneNode(QStringLiteral("PS1PlyQuadHeuristicNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);

    int nV = 0, nN = 0, nF = 0;
    QString face0;
    ASSERT_TRUE(readPsyqPlyCountsAndFirstFace(path, nV, nN, nF, face0));
    EXPECT_EQ(nF, 1);
    EXPECT_EQ(nV, 4);
    EXPECT_EQ(nN, 1);
    EXPECT_TRUE(face0.startsWith(QLatin1String("1 ")));
}

TEST_F(PS1PLYOgreTest, ExportHeuristicSkipsQuadMergeWhenSharedEdgeNormalsDisagree)
{
    ASSERT_TRUE(canLoadMeshFiles());

    const std::string meshName = "PS1PlySplitNormalHeuristicMesh";
    Ogre::MeshPtr mesh = createSplitNormalTwoTriMesh(meshName);
    ASSERT_TRUE(mesh);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode(QStringLiteral("PS1PlySplitNormalHeuristicNode"));
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outPly(QDir::tempPath() + QStringLiteral("/qtmesh_ps1ply_splitnorm_XXXXXX.ply"));
    outPly.setAutoRemove(true);
    ASSERT_TRUE(outPly.open());
    outPly.close();
    const QString path = outPly.fileName();

    QString err;
    ASSERT_TRUE(PS1PLY::exportPsyqPlyFromEntity(ent, path, nullptr, &err)) << err.toUtf8().constData();

    mgr->destroySceneNode(QStringLiteral("PS1PlySplitNormalHeuristicNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);

    int nV = 0, nN = 0, nF = 0;
    QString face0;
    ASSERT_TRUE(readPsyqPlyCountsAndFirstFace(path, nV, nN, nF, face0));
    EXPECT_EQ(nF, 2);
    EXPECT_TRUE(face0.startsWith(QLatin1String("0 ")));
}

TEST_F(PS1PLYOgreTest, ImportQuadThenExportKeepsSingleQuadFaceLine)
{
    ASSERT_TRUE(canLoadMeshFiles());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString plyIn = QDir(dir.path()).filePath(QStringLiteral("quad_in.ply"));
    {
        QFile wf(plyIn);
        ASSERT_TRUE(wf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&wf);
        ts << "@PLY940102\n";
        ts << "4 4 1\n";
        ts << "0 0 0\n1 0 0\n1 1 0\n0 1 0\n";
        ts << "0 0 1\n0 0 1\n0 0 1\n0 0 1\n";
        ts << "1 0 1 2 3 0 1 2 3\n";
    }

    const std::string meshName = "PS1PlyQuadImportMesh";
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    Ogre::MeshPtr mesh = PS1PLY::importPsyqPly(plyIn, meshName);
    ASSERT_TRUE(mesh);

    auto* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode(QStringLiteral("PS1PlyQuadImportNode"));
    ASSERT_NE(node, nullptr);
    Ogre::Entity* ent = mgr->createEntity(node, mesh);
    ASSERT_NE(ent, nullptr);

    QTemporaryFile outPly(QDir::tempPath() + QStringLiteral("/qtmesh_ps1ply_ngon_XXXXXX.ply"));
    outPly.setAutoRemove(true);
    ASSERT_TRUE(outPly.open());
    outPly.close();

    QString err;
    ASSERT_TRUE(PS1PLY::exportPsyqPlyFromEntity(ent, outPly.fileName(), nullptr, &err)) << err.toUtf8().constData();

    mgr->destroySceneNode(QStringLiteral("PS1PlyQuadImportNode"));
    Ogre::MeshManager::getSingleton().remove(meshName);

    int nV = 0, nN = 0, nF = 0;
    QString face0;
    ASSERT_TRUE(readPsyqPlyCountsAndFirstFace(outPly.fileName(), nV, nN, nF, face0));
    EXPECT_EQ(nF, 1);
    EXPECT_LE(nN, 4);
    EXPECT_TRUE(face0.startsWith(QLatin1String("1 ")));
}
